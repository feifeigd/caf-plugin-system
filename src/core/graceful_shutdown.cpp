#include "graceful_shutdown.hpp"
#include "plugin_manager.hpp"
#include <iostream>

GracefulShutdown::GracefulShutdown(ShutdownConfig cfg) : config_(cfg) {}

auto GracefulShutdown::make_behavior(caf::event_based_actor* self,
                                     caf::actor plugin_mgr,
                                     caf::actor registry,
                                     caf::actor checkpoint_mgr,
                                     std::function<std::vector<std::string>()> get_stop_order) {
    struct State {
        SystemState state = SystemState::initializing;
        std::vector<std::string> remaining_plugins;
    };
    self->state = State{};

    return caf::behavior{
        [=](ready_atom) {
            self->state.state = SystemState::ready;
            std::cout << "[System] State: READY" << std::endl;
        },

        [=](shutdown_atom) {
            if (self->state.state != SystemState::ready) {
                std::cout << "[Shutdown] Already shutting down or not ready" << std::endl;
                return;
            }
            self->state.state = SystemState::shutting_down;
            std::cout << "[System] Graceful shutdown initiated..." << std::endl;

            self->send(registry, shutdown_atom::value);
            self->state.remaining_plugins = get_stop_order();

            auto& remaining = self->state.remaining_plugins;
            if (!remaining.empty()) {
                const auto& name = remaining.back();
                self->request(plugin_mgr, caf::infinite, resolve_plugin_atom::value, name)
                    .then([=](const caf::actor& actor) {
                        if (actor) {
                            self->send(actor, drain_atom::value, self);
                            self->request(actor, config_.plugin_stop_timeout, save_state_atom::value)
                                .then(
                                    [=](const std::vector<uint8_t>& data) {
                                        if (!data.empty()) {
                                            self->request(checkpoint_mgr, std::chrono::seconds(10),
                                                         save_state_atom::value, name, data)
                                                .then([=](bool) {
                                                    self->send(actor, shutdown_atom::value);
                                                    self->send(self, plugin_saved_atom::value, name, true);
                                                });
                                        } else {
                                            self->send(actor, shutdown_atom::value);
                                            self->send(self, plugin_saved_atom::value, name, true);
                                        }
                                    },
                                    [=](const caf::error&) {
                                        self->send(actor, shutdown_atom::value);
                                        self->send(self, plugin_saved_atom::value, name, false);
                                    }
                                );
                        }
                    });
            }

            self->delayed_send(self, config_.drain_timeout, force_exit_atom::value);
        },

        [=](plugin_saved_atom, const std::string& plugin_name, bool) {
            auto& remaining = self->state.remaining_plugins;
            remaining.erase(std::remove(remaining.begin(), remaining.end(), plugin_name), remaining.end());

            if (!remaining.empty()) {
                const auto& name = remaining.back();
                self->request(plugin_mgr, caf::infinite, resolve_plugin_atom::value, name)
                    .then([=](const caf::actor& actor) {
                        if (actor) {
                            self->send(actor, drain_atom::value, self);
                            self->request(actor, config_.plugin_stop_timeout, save_state_atom::value)
                                .then(
                                    [=](const std::vector<uint8_t>& data) {
                                        if (!data.empty()) {
                                            self->request(checkpoint_mgr, std::chrono::seconds(10),
                                                         save_state_atom::value, name, data)
                                                .then([=](bool) {
                                                    self->send(actor, shutdown_atom::value);
                                                    self->send(self, plugin_saved_atom::value, name, true);
                                                });
                                        } else {
                                            self->send(actor, shutdown_atom::value);
                                            self->send(self, plugin_saved_atom::value, name, true);
                                        }
                                    },
                                    [=](const caf::error&) {
                                        self->send(actor, shutdown_atom::value);
                                        self->send(self, plugin_saved_atom::value, name, false);
                                    }
                                );
                        }
                    });
            } else {
                std::cout << "[Shutdown] All plugins saved. Stopping registry..." << std::endl;
                self->send_exit(registry, caf::exit_reason::user_shutdown);
                self->state.state = SystemState::stopped;
                std::cout << "[System] State: STOPPED" << std::endl;
                self->quit();
            }
        },

        [=](force_exit_atom) {
            if (self->state.state == SystemState::stopped) return;
            std::cout << "[Shutdown] TIMEOUT! Force exiting..." << std::endl;
            self->send_exit(plugin_mgr, caf::exit_reason::user_shutdown);
            self->send_exit(registry, caf::exit_reason::user_shutdown);
            self->send_exit(checkpoint_mgr, caf::exit_reason::user_shutdown);
            self->state.state = SystemState::stopped;
            self->quit();
        },

        [=](health_check_atom) -> SystemState {
            return self->state.state;
        }
    };
}
