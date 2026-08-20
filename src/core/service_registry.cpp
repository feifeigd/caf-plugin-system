#include "service_registry.hpp"
#include <algorithm>
#include <iostream>

caf::actor spawn_service_proxy(caf::actor_system& sys, caf::actor initial_target) {
    struct ProxyState {
        caf::actor current;
        caf::actor old;
        int version = 1;
        // 服务代理 ACL（docs/plugin-guide.md §6）：
        // restricted=false → 开放策略，转发一切（默认，兼容旧行为）；
        // restricted=true  → 只转发白名单 sender 的消息，其余在入口拦截。
        bool restricted = false;
        std::vector<caf::actor_addr> allowed;
    };

    return sys.spawn([initial_target](caf::stateful_actor<ProxyState>* self) {
        self->state().current = initial_target;

        // CAF 1.1: default_handler 使用 caf::message&（message_view 已移除）
        self->set_default_handler([self](caf::scheduled_actor*, caf::message& msg)
                                  -> caf::skippable_result {
            auto& st = self->state();
            if (!st.current) {
                return caf::make_error(caf::sec::invalid_request);
            }
            if (st.restricted) {
                // ACL 拦截点：匿名 sender（anon_send）addr 为空，一律不受信
                caf::actor_addr from;
                if (auto& snd = self->current_sender()) from = snd->address();
                if (std::find(st.allowed.begin(), st.allowed.end(), from)
                    == st.allowed.end()) {
                    std::cout << "[Proxy] ACL blocked a call to the service"
                              << std::endl;
                    // 返回 error：带 promise 的 request 会收到错误响应，
                    // 不会挂到超时；纯 send 的则被 CAF 丢弃（良性日志）
                    return caf::make_error(caf::sec::invalid_request,
                                           "ACL: sender not trusted");
                }
            }
            // 将请求转发给最新的实例
            self->delegate(st.current, msg);
            return caf::delegated<caf::message>();
        });

        return caf::behavior{
            [=](set_acl_atom, std::vector<caf::actor_addr> allowed) {
                auto& s = self->state();
                s.allowed = std::move(allowed);
                s.restricted = true;
                std::cout << "[Proxy] ACL enabled, " << s.allowed.size()
                          << " trusted sender(s)" << std::endl;
            },
            [=](switch_target_atom, caf::actor new_target, int new_ver) {
                auto& s = self->state();
                s.old = s.current;
                s.current = new_target;
                s.version = new_ver;

                std::cout << "[Proxy] v" << s.version << " switched, draining old..." << std::endl;
                // 注意必须发 actor 句柄而不是地址：插件的 drain handler 匹配
                // (drain_atom, caf::actor)，发 actor_addr 会因类型不匹配走
                // 意外消息路径，旧 actor 会被杀而不是正常排空
                self->send(s.old, drain_atom{}, caf::actor_cast<caf::actor>(self));
                self->delayed_send(self, std::chrono::seconds(30), force_cleanup_atom{}, s.old);
            },
            [=](drain_atom, const caf::actor_addr&) {
                self->state().old = caf::actor{};
                std::cout << "[Proxy] Old version fully drained" << std::endl;
            },
            [=](force_cleanup_atom, caf::actor old_actor) {
                if (self->state().old == old_actor) {
                    self->send_exit(old_actor, caf::exit_reason::user_shutdown);
                    self->state().old = caf::actor{};
                }
            }
        };
    });
}

caf::behavior ServiceRegistry::make_behavior() {
    caf::event_based_actor* self = this;
    return caf::behavior{
        // 注意：register/unregister 都是被 send（非 request）调用的。
        // CAF 1.1 里 handler 的返回值会作为普通消息回弹给 sender，
        // 对方没有对应 handler 时会被 print_and_drop 转成 error 并 quit，
        // 因此这些 handler 必须返回 void，失败只在本地记日志。
        [=, this](register_atom, const std::string& name, caf::actor impl,
            const std::string& plugin) {
            if (services_.count(name)) {
                std::cerr << "[Registry] Service already registered: " << name
                          << ". Use hot_reload to switch implementation." << std::endl;
                return;
            }
            auto proxy = spawn_service_proxy(self->system(), impl);
            VersionedEntry entry{name, proxy, impl, 1, plugin};
            services_[name] = std::move(entry);
            std::cout << "[Registry] Registered: " << name << " (v1)" << std::endl;
        },

        // 被 PluginManager 热更新流程以 send 调用，必须返回 void
        // （CAF 1.1 里 send 调用的 handler 返回值会回弹成普通消息）
        [=, this](hot_reload_atom, const std::string& name, caf::actor new_impl) {
            auto it = services_.find(name);
            if (it == services_.end()) {
                std::cerr << "[Registry] Hot-reload failed, service not found: "
                          << name << std::endl;
                return;
            }

            auto& entry = it->second;
            entry.version++;
            self->send(entry.proxy, switch_target_atom{}, new_impl, entry.version);
            entry.impl = new_impl;
            std::cout << "[Registry] Hot-reloaded: " << name << " (v" << entry.version << ")" << std::endl;
        },

        [=, this](unregister_atom, const std::string& name) {
            auto it = services_.find(name);
            if (it == services_.end()) return;

            // 安全退出 proxy，通知所有调用方
            self->send_exit(it->second.proxy, caf::exit_reason::user_shutdown);
            services_.erase(it);
            std::cout << "[Registry] Unregistered: " << name << std::endl;
        },

        [=, this](resolve_atom, const std::string& name) -> caf::actor {
            auto it = services_.find(name);
            return (it != services_.end()) ? it->second.proxy : caf::actor{};
        },

        [=, this](list_services_atom) -> std::vector<std::string> {
            std::vector<std::string> names;
            for (const auto& [name, _] : services_) {
                names.push_back(name);
            }
            return names;
        },

        // 设置服务的代理 ACL（docs/plugin-guide.md §6）。被 PluginManager
        // 在加载时以 send 调用，必须返回 void（返回值会回弹成普通消息）。
        [=, this](set_service_acl_atom, const std::string& name,
                  const std::vector<caf::actor_addr>& allowed) {
            auto it = services_.find(name);
            if (it == services_.end()) return;
            self->send(it->second.proxy, set_acl_atom{}, allowed);
            std::cout << "[Registry] ACL set for: " << name << " ("
                      << allowed.size() << " trusted sender(s))" << std::endl;
        },

        // 关机时 GracefulShutdown 会 send(shutdown_atom)。必须显式接住：
        // 意外消息在 CAF 1.1 里会经 print_and_drop 变成 error 进而 quit。
        // 顺便停掉所有服务 proxy，不让它们悬挂到进程退出。
        [=, this](shutdown_atom) {
            for (auto& [name, entry] : services_) {
                self->send_exit(entry.proxy, caf::exit_reason::user_shutdown);
            }
            services_.clear();
        }
    };
}
