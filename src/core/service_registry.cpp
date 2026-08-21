#include "service_registry.hpp"
#include <algorithm>
#include <iostream>

caf::actor spawn_service_proxy(caf::actor_system& sys, caf::actor initial_target) {
    // 缓冲的消息：request 需带走响应承诺（响应由冲刷时 promise.delegate
    // 按原 sender/mid 直达调用方）；纯 send 无承诺，冲刷时普通转发
    struct Buffered {
        caf::message msg;
        caf::actor_addr from;           // 原始 sender（冲刷时复查 ACL 用）
        caf::response_promise promise;  // 无效值 = 纯 send
    };
    struct ProxyState {
        caf::actor current;
        int version = 1;
        // 服务代理 ACL（docs/plugin-guide.md §6）：
        // restricted=false → 开放策略，转发一切（默认，兼容旧行为）；
        // restricted=true  → 只转发白名单 sender 的消息，其余在入口拦截。
        bool restricted = false;
        std::vector<caf::actor_addr> allowed;
        // 热更新静默态（§8 先排空后快照）：paused=true 时新消息进缓冲而不
        // 转发，旧实现不再收到新工作——快照因此无丢失窗口；resume 时切到
        // 新目标并冲刷缓冲。
        bool paused = false;
        std::vector<Buffered> buffered;
    };

    return sys.spawn([initial_target](caf::stateful_actor<ProxyState>* self) {
        self->state().current = initial_target; // 实现

        // CAF 1.1: default_handler 使用 caf::message&（message_view 已移除）
        self->set_default_handler([self](caf::scheduled_actor*, caf::message& msg)
                                  -> caf::skippable_result {
            auto& st = self->state();
            if (!st.current) {
                return caf::make_error(caf::sec::invalid_request);
            }

            // 热更新的时候，暂停消息的处理
            if (st.paused) {
                // 静默中：只缓冲不处理，ACL 延后到冲刷时统一复查
                caf::actor_addr from; // msg 的发送者
                if (auto& snd = self->current_sender()) from = snd->address();
                if (self->current_message_id().is_request()) {
                    // request：取走响应承诺并返回 delegated（不等价于丢弃——
                    // 承诺里存着原 sender/mid，冲刷时 promise.delegate 交付）
                    st.buffered.push_back(
                        Buffered{std::move(msg), from, self->make_response_promise()});
                    return caf::delegated<caf::message>();
                }
                st.buffered.push_back(Buffered{std::move(msg), from, {}});
                return caf::make_message();
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

            // 热更新·静默：暂停转发，新消息进缓冲。PM 以 request 调用并等
            // ack——代理邮箱 FIFO 保证：ack 之前到达的调用都已委托给旧实现。
            // 之后 PM 对旧 actor 的 save_state 构成邮箱到达序屏障：响应时
            // 旧 actor 已处理完全部在途工作（先排空后快照，无丢失窗口）。
            [=](quiesce_atom) -> bool {
                self->state().paused = true;    // 暂停转发，新消息进缓冲
                std::cout << "[Proxy] quiesced, buffering new calls" << std::endl;
                return true;    // 设置暂停成功
            },

            // 热更新·恢复：切到新目标并冲刷缓冲（按缓冲时记录的原始 sender
            // 复查 ACL）。被 PM 以 send 调用，必须返回 void（CAF 1.1 里 send
            // 调用的 handler 返回值会回弹成普通消息）。
            // 注意：冲刷纯 send 时当前邮箱元素是 resume（sender=PM），
            // 目标看到的 sender 是 PM 而非原始调用方；request 不受影响
            // （promise 里存着原始路由）。业务 handler 不依赖 sender，可接受。
            [=](resume_atom, caf::actor new_target) {
                auto& s = self->state();
                s.current = new_target;
                s.version++;
                s.paused = false;
                std::cout << "[Proxy] v" << s.version << " resumed, flushing "
                          << s.buffered.size() << " buffered call(s)" << std::endl;
                for (auto& b : s.buffered) {
                    if (s.restricted
                        && std::find(s.allowed.begin(), s.allowed.end(), b.from)
                               == s.allowed.end()) {
                        std::cout << "[Proxy] ACL blocked a buffered call"
                                  << std::endl;
                        if (b.promise.pending()) {
                            b.promise.deliver(caf::make_error(
                                caf::sec::invalid_request,
                                "ACL: sender not trusted"));
                        }
                        continue;
                    }
                    
                    if (b.promise.pending()) {
                        // promise.delegate 对单个 caf::message 参数原样透传
                        // （response_promise.hpp 的 type_list<message> 分支），
                        // 响应按原 sender/mid 直达调用方
                        b.promise.delegate(s.current, std::move(b.msg));
                    } else {
                        self->delegate(s.current, std::move(b.msg));
                    }
                }
                s.buffered.clear();
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

        // 被 PluginManager 热更新流程以 send 调用。代理的静默/切换/冲刷由
        // PM 通过 quiesce/resume 直接编排（§8 先排空后快照），这里只做台账：
        // 更新实现引用与版本号。必须返回 void（send 调用的 handler 返回值
        // 会回弹成普通消息）。
        [=, this](hot_reload_atom, const std::string& name, caf::actor new_impl) {
            auto it = services_.find(name);
            if (it == services_.end()) {
                std::cerr << "[Registry] Hot-reload failed, service not found: "
                          << name << std::endl;
                return;
            }
            it->second.version++;
            it->second.impl = new_impl;
            std::cout << "[Registry] Hot-reloaded: " << name << " (v"
                      << it->second.version << ")" << std::endl;
        },

        [=, this](unregister_atom, const std::string& name) {
            auto it = services_.find(name);
            if (it == services_.end()) return;

            // 安全退出 proxy，通知所有调用方
            self->send_exit(it->second.proxy, caf::exit_reason::user_shutdown);
            services_.erase(it);
            std::cout << "[Registry] Unregistered: " << name << std::endl;
        },

        // 返回 dll 的代理 actor
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
