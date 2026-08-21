#include "graceful_shutdown.hpp"
#include "plugin/plugin_manager.hpp"
#include "checkpoint_manager.hpp"
#include <iostream>

GracefulShutdown::GracefulShutdown(caf::actor_config& cfg,
                                   ShutdownConfig shutdown_cfg,
                                   caf::actor plugin_mgr,
                                   caf::actor registry,
                                   caf::actor checkpoint_mgr,
                                   std::function<std::vector<std::string>()> get_stop_order)
    : caf::event_based_actor(cfg),
      config_(shutdown_cfg),
      plugin_mgr_(plugin_mgr),
      registry_(registry),
      checkpoint_mgr_(checkpoint_mgr),
      get_stop_order_(std::move(get_stop_order)) {}

caf::behavior GracefulShutdown::make_behavior() {
    caf::event_based_actor* self = this;
    auto plugin_mgr = plugin_mgr_;
    auto registry = registry_;
    auto checkpoint_mgr = checkpoint_mgr_;
    auto get_stop_order = get_stop_order_;

    // 逐个停插件的公共流程：resolve -> drain -> save_state -> checkpoint -> shutdown
    auto stop_next = [=, this](const std::string& name) {
        self->request(plugin_mgr, caf::infinite, resolve_plugin_atom{}, name)
            .then([=, this](const caf::actor& actor) {
                if (!actor) {
                    // resolve 失败也要推进停机链，避免卡死
                    self->send(self, plugin_saved_atom{}, name, false);
                    return;
                }
                self->send(actor, drain_atom{}, self);
                self->request(actor, config_.plugin_stop_timeout, save_state_atom{})
                    .then(
                        [=, this](const std::vector<std::byte>& data) {
                            if (!data.empty()) {
                                self->request(checkpoint_mgr, std::chrono::seconds(10),
                                             save_state_atom{}, name, data)
                                    .then([=, this](bool) {
                                        self->send(actor, shutdown_atom{});
                                        self->send(self, plugin_saved_atom{}, name, true);
                                    });
                            } else {
                                self->send(actor, shutdown_atom{});
                                self->send(self, plugin_saved_atom{}, name, true);
                            }
                        },
                        [=, this](const caf::error&) {
                            self->send(actor, shutdown_atom{});
                            self->send(self, plugin_saved_atom{}, name, false);
                        }
                    );
            });
    };

    return caf::behavior{
        [=, this](ready_atom) {
            state_.state = SystemState::ready;
            std::cout << "[System] State: READY" << std::endl;
        },

        [=, this](shutdown_atom) {
            if (state_.state != SystemState::ready) {
                // 未就绪（启动早期）也强制退出：Ctrl+C 任何阶段必须能响应，
                // 否则 actor_system 析构会等核心 actor 永久挂起。
                if (state_.state != SystemState::stopped) {
                    std::cout << "[Shutdown] Not ready, forcing exit..." << std::endl;
                    self->send_exit(plugin_mgr, caf::exit_reason::user_shutdown);
                    self->send_exit(registry, caf::exit_reason::user_shutdown);
                    self->send_exit(checkpoint_mgr, caf::exit_reason::user_shutdown);
                    state_.state = SystemState::stopped;
                    self->quit();
                }
                return;
            }
            state_.state = SystemState::shutting_down;
            std::cout << "[System] Graceful shutdown initiated..." << std::endl;

            self->send(registry, shutdown_atom{});
            state_.remaining_plugins = get_stop_order();

            if (!state_.remaining_plugins.empty()) {
                stop_next(state_.remaining_plugins.back());
            }

            self->delayed_send(self, config_.drain_timeout, force_exit_atom{});
        },

        [=, this](plugin_saved_atom, const std::string& plugin_name, bool) {
            auto& remaining = state_.remaining_plugins;
            remaining.erase(std::remove(remaining.begin(), remaining.end(), plugin_name), remaining.end());

            if (!remaining.empty()) {
                stop_next(remaining.back());
            } else {
                std::cout << "[Shutdown] All plugins saved. Stopping registry..." << std::endl;
                // 停掉全部核心 actor，否则 actor_system 析构会一直等它们退出
                self->send_exit(plugin_mgr, caf::exit_reason::user_shutdown);
                self->send_exit(registry, caf::exit_reason::user_shutdown);
                self->send_exit(checkpoint_mgr, caf::exit_reason::user_shutdown);
                state_.state = SystemState::stopped;
                std::cout << "[System] State: STOPPED" << std::endl;
                self->quit();
            }
        },

        // 插件 drain 完成后的回执 (drain_atom, actor_addr)。本实现对回执
        // 不做等待（drain 与 save_state 并行发出），但必须显式吞掉：
        // CAF 1.1 里意外消息会经 print_and_drop 产生 error 并让 actor quit，
        // 曾在关机中途杀死本 actor 导致停机链断裂。
        [=](drain_atom, const caf::actor_addr&) {
        },

        [=, this](force_exit_atom) {
            if (state_.state == SystemState::stopped) return;
            std::cout << "[Shutdown] TIMEOUT! Force exiting..." << std::endl;
            self->send_exit(plugin_mgr, caf::exit_reason::user_shutdown);
            self->send_exit(registry, caf::exit_reason::user_shutdown);
            self->send_exit(checkpoint_mgr, caf::exit_reason::user_shutdown);
            state_.state = SystemState::stopped;
            self->quit();
        },

        [=, this](health_check_atom) -> SystemState {
            return state_.state;
        }
    };
}
