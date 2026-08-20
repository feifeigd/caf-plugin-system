#include "service_registry.hpp"
#include <iostream>

caf::actor spawn_service_proxy(caf::actor_system& sys, caf::actor initial_target) {
    struct ProxyState {
        caf::actor current;
        caf::actor old;
        int version = 1;
    };

    return sys.spawn([initial_target](caf::stateful_actor<ProxyState>* self) {
        self->state.current = initial_target;

        self->set_default_handler([self](caf::scheduled_actor*, caf::message_view& xv)
                                  -> caf::skippable_result {
            if (!self->state.current) {
                return caf::make_error(caf::sec::invalid_request);
            }
            self->delegate(self->state.current, xv.content());
            return caf::delegated<caf::message>();
        });

        return caf::behavior{
            [=](switch_target_atom, caf::actor new_target, int new_ver) {
                auto& s = self->state;
                s.old = s.current;
                s.current = new_target;
                s.version = new_ver;

                std::cout << "[Proxy] v" << s.version << " switched, draining old..." << std::endl;
                self->send(s.old, drain_done_atom::value, self);
                self->delayed_send(self, std::chrono::seconds(30), force_cleanup_atom::value, s.old);
            },
            [=](drain_done_atom, const caf::actor_addr&) {
                self->state.old = caf::actor{};
                std::cout << "[Proxy] Old version fully drained" << std::endl;
            },
            [=](force_cleanup_atom, caf::actor old_actor) {
                if (self->state.old == old_actor) {
                    self->send_exit(old_actor, caf::exit_reason::user_shutdown);
                    self->state.old = caf::actor{};
                }
            }
        };
    });
}

auto ServiceRegistry::make_behavior(caf::event_based_actor* self) {
    return caf::behavior{
        [=](register_atom, const std::string& name, caf::actor impl,
            const std::string& plugin) -> caf::result<void> {
            if (self->state.count(name)) {
                return caf::make_error(caf::sec::invalid_argument,
                    "Service already registered: " + name +
                    ". Use hot_reload to switch implementation.");
            }
            auto proxy = spawn_service_proxy(self->system(), impl);
            VersionedEntry entry{name, proxy, impl, 1, plugin};
            self->state[name] = std::move(entry);
            std::cout << "[Registry] Registered: " << name << " (v1)" << std::endl;
            return {};
        },

        [=](hot_reload_atom, const std::string& name, caf::actor new_impl) -> caf::result<bool> {
            auto it = self->state.find(name);
            if (it == self->state.end()) {
                return caf::make_error(caf::sec::invalid_argument,
                    "Service not found: " + name + ". Register first.");
            }

            auto& entry = it->second;
            entry.version++;
            self->send(entry.proxy, switch_target_atom::value, new_impl, entry.version);
            entry.impl = new_impl;
            std::cout << "[Registry] Hot-reloaded: " << name << " (v" << entry.version << ")" << std::endl;
            return true;
        },

        [=](unregister_atom, const std::string& name) -> bool {
            auto it = self->state.find(name);
            if (it == self->state.end()) return false;

            // 安全退出 proxy，通知所有调用方
            self->send_exit(it->second.proxy, caf::exit_reason::user_shutdown);
            self->state.erase(it);
            std::cout << "[Registry] Unregistered: " << name << std::endl;
            return true;
        },

        [=](resolve_atom, const std::string& name) -> caf::actor {
            auto it = self->state.find(name);
            return (it != self->state.end()) ? it->second.proxy : caf::actor{};
        },

        [=](list_services_atom) -> std::vector<std::string> {
            std::vector<std::string> names;
            for (const auto& [name, _] : self->state) {
                names.push_back(name);
            }
            return names;
        }
    };
}
