#include "plugin_manager.hpp"
#include "checkpoint_manager.hpp"
#include <caf/logger.hpp>
#include <deque>
#include <vector>
#include <iostream>
#include <cstring>

namespace {
/// 插件 DLL 句柄池（进程级常驻）。见 LoadedPlugin::lib 注释：
/// actor 的 vtable/lambda 代码在插件 DLL 里，CAF 异步释放引用，
/// FreeLibrary 过早会崩（0xC0000005 exec 到已卸载代码段）。
/// 用 deque 保证元素地址稳定。进程退出时由 OS 回收。
std::deque<DynamicLibrary>& plugin_lib_pool() {
    static std::deque<DynamicLibrary> pool;
    return pool;
}
} // namespace

PluginManager::PluginManager(caf::actor_config& cfg, caf::actor registry, caf::actor checkpoint_mgr)
    : caf::event_based_actor(cfg), registry_(registry), checkpoint_mgr_(checkpoint_mgr) {}

caf::behavior PluginManager::make_behavior() {
    caf::event_based_actor* self = this;
    // 兜底：吞掉意外消息而不是退出。CAF 1.1 的默认 print_and_drop
    // 会产生 error 结果并让 actor quit；PluginManager 是长期驻留的
    // 宿主 actor，不能因协议毛刺被杀死。
    self->set_default_handler([](caf::scheduled_actor*, caf::message& msg)
                              -> caf::skippable_result {
        CAF_LOG_WARNING("PluginManager discarding unexpected message: " << caf::to_string(msg));
        return caf::make_message();
    });
    
    return caf::behavior{
        [=, this](load_atom, const std::string& name, const std::string& path) -> caf::result<bool> {
            CAF_LOG_INFO("Loading plugin: " << name);

            if (plugins_.count(name)) {
                CAF_LOG_ERROR("Plugin already loaded: " << name);
                return caf::make_error(caf::sec::invalid_argument, "Already loaded");
            }

            auto lib_opt = DynamicLibrary::open(path);
            if (!lib_opt) {
                CAF_LOG_ERROR("Failed to load library: " << path);
                return caf::make_error(caf::sec::invalid_argument, "Failed to load library");
            }

            auto create  = lib_opt->symbol<CreatePluginFunc>("create_plugin");
            auto destroy = lib_opt->symbol<DestroyPluginFunc>("destroy_plugin");
            if (!create || !destroy) {
                CAF_LOG_ERROR("Missing create/destroy symbols in: " << path);
                return false;
            }

            PluginEntry* plugin = create();
            auto manifest = plugin->manifest();
            CAF_LOG_INFO("Plugin manifest: " << manifest.name
                         << " deps=" << manifest.dependencies.size()
                         << " provides=" << manifest.provides.size());

            for (const auto& svc : manifest.provides) {
                dep_graph_.register_service(svc, name);
            }
            dep_graph_.add_plugin(name, manifest.dependencies);

            if (dep_graph_.has_cycle_from(name)) {
                CAF_LOG_ERROR("Circular dependency detected for: " << name);
                destroy(plugin);
                return caf::make_error(caf::sec::invalid_argument, "Circular dependency");
            }

            std::vector<caf::actor> deps;
            caf::scoped_actor blocking{self->system()};
            for (const auto& dep : manifest.dependencies) {
                CAF_LOG_INFO("Resolving dependency: " << dep << " for " << name);
                caf::actor dep_actor;
                blocking->request(registry_, caf::infinite, resolve_atom{}, dep)
                    .receive([&dep_actor](const caf::actor& a) { dep_actor = a; },
                             [](caf::error&) {});
                if (!dep_actor) {
                    CAF_LOG_ERROR("Missing dependency: " << dep << " for " << name);
                    destroy(plugin);
                    return caf::make_error(caf::sec::invalid_argument, "Missing dep: " + dep);
                }
                deps.push_back(dep_actor);
            }

            std::vector<std::byte> state_data;
            blocking->request(checkpoint_mgr_, std::chrono::seconds(2),
                              restore_state_atom{}, name)
                .receive([&state_data](const std::vector<std::byte>& d) { state_data = d; },
                         [](caf::error&) {});

            auto actor = plugin->spawn(self->system(), deps, "");

            if (!state_data.empty()) {
                CAF_LOG_INFO("Restoring state for: " << name);
                self->send(actor, restore_state_atom{}, state_data);
            }

            self->monitor(actor);
            self->send(actor, init_atom{}, self, "");

            for (const auto& svc : manifest.provides) {
                self->send(registry_, register_atom{}, svc, actor, name);
            }

            // 服务代理 ACL：插件在 manifest 声明 acl_allow 时，
            // 把它提供的服务全部切到受限策略（见 docs/plugin-guide.md §6）。
            // 与 register_svc 同一 sender→receiver 对，CAF 保证 FIFO，
            // 代理先建成、再收 ACL；两者之间有个开放窗口（仅启动期存在）。
            if (!manifest.acl_allow.empty()) {
                std::vector<caf::actor_addr> allowed;
                for (const auto& pname : manifest.acl_allow) {
                    auto pit = plugins_.find(pname);
                    if (pit != plugins_.end()) {
                        allowed.push_back(pit->second.actor->address());
                    } else {
                        // 拓扑序保证依赖已加载；找不到说明清单写错了
                        CAF_LOG_WARNING("ACL: unknown plugin in acl_allow of "
                                        << name << ": " << pname);
                    }
                }
                for (const auto& svc : manifest.provides) {
                    self->send(registry_, set_service_acl_atom{}, svc, allowed);
                }
            }

            plugin_lib_pool().push_back(std::move(*lib_opt));
            plugins_.emplace(name, LoadedPlugin{
                &plugin_lib_pool().back(), plugin, actor, manifest, destroy
            });
            CAF_LOG_INFO("Plugin loaded successfully: " << name);
            return true;
        },

        // 旁路热更新（docs/plugin-guide.md §8）：从【新路径】加载同名插件的
        // 新版本。流程（先排空后快照，无状态丢失窗口）：
        //   1. 准备：新 DLL 校验、依赖解析（失败时尚未动旧实现，直接返回）；
        //   2. quiesce 该插件所有服务的代理并等 ack——代理邮箱 FIFO 保证
        //      ack 之前到达的调用都已委托给旧 actor（失败则把已静默的代理
        //      resume 回旧实现，回滚）；
        //   3. 对旧 actor request save_state：邮箱到达序屏障，响应时旧 actor
        //      已处理完全部在途工作，且代理已静默不会有新工作——快照即终态；
        //   4. spawn 新 actor + restore + monitor + init；
        //   5. registry 台账（hot_reload_atom）→ resume 代理（切目标+冲刷
        //      缓冲，ACL 按原始 sender 复查）；
        //   6. 旧实例退役：邮箱已空，2s 后 shutdown，down_msg 销毁实例。
        // 约束：
        //   - 新代码必须在新文件/新路径：Windows 对已加载 DLL 有文件锁，
        //     且 LoadLibrary 对同路径返回缓存的旧模块，拿不到新代码；
        //   - 不能调用新 DLL 的 register_meta_objects：号段在启动时已注册，
        //     重复注册同一段 CAF 直接 abort。因此热更新不能引入未注册的
        //     新 type_id——新协议请走信封 sub_proto（无需新 ID）；
        //   - 旧 DLL 常驻句柄池不卸载（actor vtable 在 DLL 代码段，
        //     CAF 异步释放引用，FreeLibrary 过早会崩）。
        [=, this](reload_atom, const std::string& name, const std::string& path)
            -> caf::result<bool> {
            auto it = plugins_.find(name);
            if (it == plugins_.end()) {
                return caf::make_error(caf::sec::invalid_argument,
                                       "Plugin not loaded: " + name);
            }

            auto lib_opt = DynamicLibrary::open(path);
            if (!lib_opt) {
                return caf::make_error(caf::sec::invalid_argument,
                                       "Failed to load library: " + path);
            }
            auto create  = lib_opt->symbol<CreatePluginFunc>("create_plugin");
            auto destroy = lib_opt->symbol<DestroyPluginFunc>("destroy_plugin");
            if (!create || !destroy) {
                return caf::make_error(caf::sec::invalid_argument,
                                       "Missing create/destroy symbols in: " + path);
            }

            PluginEntry* new_plugin = create();
            auto manifest = new_plugin->manifest();
            if (manifest.name != name) {
                destroy(new_plugin);
                return caf::make_error(caf::sec::invalid_argument,
                                       "Reloaded manifest name mismatch: " + manifest.name);
            }

            // 依赖解析（与 load 相同）
            std::vector<caf::actor> deps;
            caf::scoped_actor blocking{self->system()};
            for (const auto& dep : manifest.dependencies) {
                caf::actor dep_actor;
                blocking->request(registry_, caf::infinite, resolve_atom{}, dep)
                    .receive([&dep_actor](const caf::actor& a) { dep_actor = a; },
                             [](caf::error&) {});
                if (!dep_actor) {
                    destroy(new_plugin);
                    return caf::make_error(caf::sec::invalid_argument,
                                           "Missing dep: " + dep);
                }
                deps.push_back(dep_actor);
            }

            // 静默该插件全部服务的代理；任一失败则把已静默的代理恢复回
            // 旧实现（回滚），旧服务不受半截热更新影响
            std::vector<caf::actor> proxies;
            for (const auto& svc : it->second.manifest.provides) {
                caf::actor proxy;
                blocking->request(registry_, caf::infinite, resolve_atom{}, svc)
                    .receive([&proxy](const caf::actor& a) { proxy = a; },
                             [](caf::error&) {});
                bool paused = false;
                if (proxy) {
                    blocking->request(proxy, std::chrono::seconds(5), quiesce_atom{})
                        .receive([&paused](bool b) { paused = b; },
                                 [](caf::error&) {});
                }
                if (!paused) {
                    for (auto& p : proxies)
                        self->send(p, resume_atom{}, it->second.actor);
                    destroy(new_plugin);
                    return caf::make_error(
                        caf::sec::invalid_argument,
                        "Reload: failed to quiesce proxy for service: " + svc);
                }
                proxies.push_back(proxy);
            }

            // 状态移交：此刻旧 actor 已静默——save_state 的响应本身就是
            // "邮箱已排空"的证据（邮箱到达序屏障），快照即终态，无丢失窗口
            std::vector<std::byte> state_data;
            blocking->request(it->second.actor, std::chrono::seconds(5),
                              save_state_atom{})
                .receive([&state_data](const std::vector<std::byte>& d) { state_data = d; },
                         [](caf::error&) {});

            auto new_actor = new_plugin->spawn(self->system(), deps, "");
            if (!state_data.empty()) {
                self->send(new_actor, restore_state_atom{}, state_data);
            }
            self->monitor(new_actor);
            self->send(new_actor, init_atom{}, self, "");

            // registry 台账（impl 引用与版本号）
            for (const auto& svc : manifest.provides) {
                self->send(registry_, hot_reload_atom{}, svc, new_actor);
            }
            // 恢复流量：代理切到新实现并冲刷静默期缓冲的调用。
            // ACL 白名单记的是【调用方】，与被切换的实现无关，热切换后原样保留；
            // 若新版本要改 acl_allow，需另行重新下发 set_service_acl_atom。
            for (auto& p : proxies) {
                self->send(p, resume_atom{}, new_actor);
            }

            // 旧实例退役：邮箱已空（quiesce 屏障），2s 后退出，down_msg
            // 到来时销毁旧实例（C++ 对象）；旧 DLL 留在句柄池。
            self->delayed_send(it->second.actor, std::chrono::seconds(2),
                               shutdown_atom{});
            retired_.push_back(std::move(it->second));

            plugin_lib_pool().push_back(std::move(*lib_opt));
            it->second = LoadedPlugin{
                &plugin_lib_pool().back(), new_plugin, new_actor, manifest, destroy
            };

            CAF_LOG_INFO("Plugin hot-reloaded: " << name << " -> " << path);
            return true;
        },

        [=, this](unload_atom, const std::string& name) -> bool {
            CAF_LOG_INFO("Unloading plugin: " << name);
            auto it = plugins_.find(name);
            if (it == plugins_.end()) {
                CAF_LOG_WARNING("Plugin not found for unload: " << name);
                return false;
            }

            for (const auto& svc : it->second.manifest.provides) {
                self->send(registry_, unregister_atom{}, svc);
            }

            self->send_exit(it->second.actor, caf::exit_reason::user_shutdown);

            if (it->second.destroy) {
                it->second.destroy(it->second.instance);
            }
            plugins_.erase(it);
            CAF_LOG_INFO("Plugin unloaded: " << name);
            return true;
        },

        [=, this](list_atom) -> std::vector<std::string> {
            std::vector<std::string> names;
            for (const auto& [n, _] : plugins_) names.push_back(n);
            return names;
        },

        [=, this](resolve_plugin_atom, const std::string& name) -> caf::actor {
            auto it = plugins_.find(name);
            return (it != plugins_.end()) ? it->second.actor : caf::actor{};
        },

        [=, this](shutdown_atom, caf::actor mgr) {
            shutdown_mgr_ = mgr;
            CAF_LOG_INFO("Shutdown manager registered");
        },

        [=, this](request_shutdown_atom) {
            if (shutdown_mgr_) {
                CAF_LOG_INFO("Forwarding shutdown request to GracefulShutdown");
                self->send(shutdown_mgr_, shutdown_atom{});
            } else {
                CAF_LOG_ERROR("Shutdown manager not set");
            }
        },

        [=, this](const caf::down_msg& dm) {
            // 热更新退役的旧 actor 退出：正常销毁旧实例，不算崩溃
            for (auto rit = retired_.begin(); rit != retired_.end(); ++rit) {
                if (rit->actor && rit->actor->address() == dm.source) {
                    CAF_LOG_INFO("Hot-reload: retired instance cleaned up");
                    if (rit->destroy) {
                        rit->destroy(rit->instance);
                    }
                    retired_.erase(rit);
                    return;
                }
            }
            for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
                if (it->second.actor->address() == dm.source) {
                    CAF_LOG_ERROR("Plugin crashed: " << it->first);

                    for (const auto& svc : it->second.manifest.provides) {
                        self->send(registry_, unregister_atom{}, svc);
                    }

                    if (it->second.destroy) {
                        it->second.destroy(it->second.instance);
                    }
                    plugins_.erase(it);
                    break;
                }
            }
        }
    };
}
