// ------------------------------------------------------------------
// Lua 脚本宿主插件（lua_host）
//
// 目标：让 .lua 脚本也能当插件用——把 scripts/ 目录下的每个 .lua 包装成
// 一个一等 CAF 服务（一个 actor + 一个专属 lua_State + 一个 worker 线程），
// 复用框架的 lifecycle / registry / 优雅关机 / 热更（简单换实现）。
//
// 脚本约定（可选定义）：
//   plugin = { name=..., version=..., provides="服务名", deps={...} }
//   function on_init(manager) ... end          -- 初始化（可 bridge.call 依赖服务）
//   function on_call(sub_proto, payload) return "..." end  -- envelope 业务入口
//   function on_string(cmd) return "..." end               -- 字符串命令入口
//   function on_save() return "状态串" end
//   function on_restore(state_str) ... end
//   function on_shutdown() ... end               -- 快清理，禁止阻塞 call
//
// 桥接 API（脚本全局可用）：
//   log(level, msg)
//   call(service, sub_proto, payload) -> ok, reply   -- 同步阻塞调用服务
//   call_string(service, cmd) -> ok, reply
//   config(key) -> value
//   self_name() -> 服务名
//   now() -> 毫秒时间戳
//
// 关键决策：
//   - 零新 type_id / 免 register_meta_objects：全程 plugin_envelope(228) +
//     std::string，宿主像 DB 插件一样不需要 register_meta_objects 导出。
//   - 单 worker 独占 lua_State：所有 Lua 执行走 JobQueue，event actor 不阻塞；
//     bridge.call() 在 worker 线程用 scoped_actor 阻塞 request，Lua 里同步化。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/plugin_config.hpp"
#include "common/plugin_envelope.hpp"
#include "templates/job_queue.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>

#include <sol/sol.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 声明式配置（PLUGIN_CONFIG，见 common/plugin_config.hpp）：
// 读取路径 caf-plugin-system.lua_host.<字段名>。scripts_dir 留空 = 用
// asset_dir/scripts（框架注入的 DLL 所在目录）。
#define LUA_FIELDS(X, XC)                                                      \
    X(std::string, scripts_dir, "")
PLUGIN_CONFIG(LUA_FIELDS)
#undef LUA_FIELDS

// 保留的宿主管理子协议号（走 plugin_envelope，发给 lua_host_service）
constexpr uint16_t lua_reload_sub_proto = 1;   // payload = 脚本服务名 → 重载

// 脚本清单（从 Lua `plugin` 表读取）
struct ScriptManifest {
    std::string name;
    std::string version;
    std::string provides;
    std::vector<std::string> deps;
};

// ---- Job 与队列（复用 templates/job_queue.hpp 的阻塞 worker 模型）----

enum class ScriptOp { Init, Call, String, Save, Restore, Shutdown };

struct ScriptJob {
    ScriptOp op = ScriptOp::Call;
    plugin_envelope env;                        // Call
    std::string str;                            // String
    std::vector<std::byte> bytes;               // Restore
    std::function<void(const std::string&)> done_str;         // Call/String/Save 回执
    std::function<void(std::vector<std::byte>)> done_bytes;   // Save 回执
    std::function<void()> done_void;                          // Init/Restore/Shutdown 完成

    // JobQueue::fail_all / worker 异常路径统一交付
    void fail(const std::string& err) {
        if (done_str)
            done_str("lua error: " + err);
        else if (done_bytes)
            done_bytes({});
        else if (done_void)
            done_void();
    }
};

using JobQueue = caf_plugin_system::JobQueue<ScriptJob>;

// 脚本实例共享状态：子 actor 与 worker 线程共同持有
struct ScriptState {
    std::shared_ptr<sol::state> lua;
    ScriptManifest manifest;
    caf::actor logger;        // 日志服务 actor
    caf::actor registry;      // 服务注册表（bridge.call 解析用）
    caf::actor_system* sys = nullptr;
    std::string self_name;    // 服务名
    std::shared_ptr<JobQueue> queue;
    std::shared_ptr<std::thread> worker;
    std::atomic<bool> stopped{false};
};

// ------------------------------------------------------------------
// 桥接辅助（跑在 worker 线程，脚本里同步调用）
// ------------------------------------------------------------------

/// 同步调用服务：resolve → 发 plugin_envelope → 等 std::string 回复。
/// v1 契约：目标服务须回复 std::string（脚本服务 / 信封/字符串协议服务）。
std::pair<bool, std::string> call_service(caf::actor_system& sys,
                                          caf::actor registry,
                                          const std::string& service,
                                          uint16_t sub_proto,
                                          const std::string& payload) {
    caf::scoped_actor self{sys};
    caf::actor proxy;
    self->request(registry, std::chrono::seconds(5), resolve_atom_v, service)
        .receive([&](caf::actor a) { proxy = std::move(a); },
                 [](caf::error&) {});

    if (!proxy)
        return {false, "service not found: " + service};

    plugin_envelope env;
    env.sub_proto = sub_proto;
    const char* p = payload.data();
    env.payload.assign(reinterpret_cast<const std::byte*>(p),
                      reinterpret_cast<const std::byte*>(p) + payload.size());

    bool ok = false;
    std::string reply;
    self->request(proxy, std::chrono::seconds(5), env).receive(
        [&](std::string s) {
            ok = true;
            reply = std::move(s);
        },
        [&](caf::error& e) {
            ok = false;
            reply = caf::to_string(e);
        });
    return {ok, reply};
}

/// 同步调用服务的字符串 handler（发 std::string）。
std::pair<bool, std::string> call_string_service(caf::actor_system& sys,
                                                 caf::actor registry,
                                                 const std::string& service,
                                                 const std::string& cmd) {
    caf::scoped_actor self{sys};
    caf::actor proxy;
    self->request(registry, std::chrono::seconds(5), resolve_atom_v, service)
        .receive([&](caf::actor a) { proxy = std::move(a); },
                 [](caf::error&) {});

    if (!proxy)
        return {false, "service not found: " + service};

    bool ok = false;
    std::string reply;
    self->request(proxy, std::chrono::seconds(5), cmd).receive(
        [&](std::string s) {
            ok = true;
            reply = std::move(s);
        },
        [&](caf::error& e) {
            ok = false;
            reply = caf::to_string(e);
        });
    return {ok, reply};
}

/// 取 Lua 函数结果里的第一个字符串（nil/非字符串 → 空串）。
std::string first_string(const sol::protected_function_result& r) {
    if (!r.valid() || r.return_count() == 0)
        return {};
    sol::object o = r.get<sol::object>();
    if (o.is<std::string>())
        return o.as<std::string>();
    if (o.is<double>())
        return std::to_string(o.as<double>());
    if (o.is<bool>())
        return o.as<bool>() ? "true" : "false";
    return {};
}

/// 调用 Lua 函数（不存在则跳过），异常由 worker 的 try/catch 兜底。
sol::protected_function lua_fn(sol::state& L, const char* name) {
    sol::protected_function f = L[name];
    return f;
}

/// 把桥接 API 绑定到脚本全局命名空间。
void bind_bridge(sol::state& L, ScriptState* st) {
    L.set_function("log", [st](const std::string& level, const std::string& msg) {
        if (st->logger)
            caf::anon_send(st->logger, log_atom{}, st->self_name, level,
                           "[lua] " + msg);
    });
    L.set_function("call",
                   [st](const std::string& service, uint16_t sub_proto,
                        const std::string& payload) {
                       if (!st->sys)
                           return std::make_pair(false, std::string("no system"));
                       return call_service(*st->sys, st->registry, service,
                                           sub_proto, payload);
                   });
    L.set_function("call_string",
                   [st](const std::string& service, const std::string& cmd) {
                       if (!st->sys)
                           return std::make_pair(false, std::string("no system"));
                       return call_string_service(*st->sys, st->registry,
                                                  service, cmd);
                   });
    L.set_function("config", [st](const std::string& key) {
        if (!st->sys)
            return std::string{};
        return caf::get_or(st->sys->config(),
                           "caf-plugin-system.lua_host." + key, std::string{});
    });
    L.set_function("self_name", [st]() { return st->self_name; });
    L.set_function("now", []() {
        return static_cast<double>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count());
    });
}

/// 读脚本顶层的 `plugin` 表（manifest）。
void read_manifest(sol::state& L, ScriptManifest& m,
                   const std::filesystem::path& path) {
    sol::optional<sol::table> pt = L["plugin"];
    if (!pt)
        return;
    m.name = (*pt)["name"].get_or(std::string{});
    m.version = (*pt)["version"].get_or(std::string{});
    m.provides = (*pt)["provides"].get_or(std::string{});
    if (auto deps = (*pt)["deps"].get<sol::optional<sol::table>>()) {
        for (const auto& kv : *deps)
            if (kv.second.is<std::string>())
                m.deps.push_back(kv.second.as<std::string>());
    }
}

/// 执行单个 job 的 Lua 逻辑（在 worker 线程调用）。
void run_job(ScriptState& st, ScriptJob& job) {
    sol::state& L = *st.lua;
    switch (job.op) {
        case ScriptOp::Init: {
            auto f = lua_fn(L, "on_init");
            if (f.valid()) {
                auto r = f(std::string("<manager>"));
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
            }
            if (job.done_void) job.done_void();
            break;
        }
        case ScriptOp::Call: {
            std::string payload(
                reinterpret_cast<const char*>(job.env.payload.data()),
                job.env.payload.size());
            std::string out;
            auto f = lua_fn(L, "on_call");
            if (f.valid()) {
                auto r = f(static_cast<uint16_t>(job.env.sub_proto), payload);
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
                out = first_string(r);
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::String: {
            std::string out;
            auto f = lua_fn(L, "on_string");
            if (f.valid()) {
                auto r = f(job.str);
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
                out = first_string(r);
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Save: {
            std::string out;
            auto f = lua_fn(L, "on_save");
            if (f.valid()) {
                auto r = f();
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
                out = first_string(r);
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Restore: {
            std::string data(
                reinterpret_cast<const char*>(job.bytes.data()),
                job.bytes.size());
            auto f = lua_fn(L, "on_restore");
            if (f.valid()) {
                auto r = f(data);
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
            }
            if (job.done_void) job.done_void();
            break;
        }
        case ScriptOp::Shutdown: {
            auto f = lua_fn(L, "on_shutdown");
            if (f.valid()) {
                auto r = f();
                if (!r.valid()) { sol::error e = r; throw std::runtime_error(e.what()); }
            }
            if (job.done_void) job.done_void();
            break;
        }
    }
}

/// worker 主循环：独占 lua_State，串行执行队列。
void lua_worker_main(std::shared_ptr<ScriptState> st) {
    for (;;) {
        auto job = st->queue->pop();
        if (!job)
            break;
        try {
            run_job(*st, *job);
        } catch (const std::exception& e) {
            LOG_ERROR("script '{}' job error: {}", st->self_name, e.what());
            job->fail(e.what());
        }
    }
}

/// 幂等停 worker：stop 队列 + join 线程。
void stop_worker(const std::shared_ptr<ScriptState>& st) {
    bool expected = false;
    if (!st->stopped.compare_exchange_strong(expected, true))
        return;
    st->queue->stop();
    if (st->worker && st->worker->joinable())
        st->worker->join();
}

// ---- 状态聚合帧（宿主 on_save 聚合子脚本状态，on_restore 拆帧分发）----
// 帧格式：[4B name_len][name][4B data_len][data]，多帧首尾相接。同进程
// 内读写，字节序一致即可。

void append_frame(std::string& buf, const std::string& name,
                  const std::vector<std::byte>& data) {
    uint32_t nl = static_cast<uint32_t>(name.size());
    uint32_t dl = static_cast<uint32_t>(data.size());
    buf.append(reinterpret_cast<const char*>(&nl), sizeof(nl));
    buf.append(name);
    buf.append(reinterpret_cast<const char*>(&dl), sizeof(dl));
    buf.append(reinterpret_cast<const char*>(data.data()), data.size());
}

std::vector<std::pair<std::string, std::vector<std::byte>>>
parse_frames(const std::vector<std::byte>& blob) {
    std::vector<std::pair<std::string, std::vector<std::byte>>> out;
    const char* p = reinterpret_cast<const char*>(blob.data());
    size_t n = blob.size();
    size_t off = 0;
    while (off + sizeof(uint32_t) <= n) {
        uint32_t nl;
        std::memcpy(&nl, p + off, sizeof(nl));
        off += sizeof(nl);
        if (off + nl > n)
            break;
        std::string name(p + off, nl);
        off += nl;
        if (off + sizeof(uint32_t) > n)
            break;
        uint32_t dl;
        std::memcpy(&dl, p + off, sizeof(dl));
        off += sizeof(dl);
        if (off + dl > n)
            break;
        std::vector<std::byte> data(
            reinterpret_cast<const std::byte*>(p + off),
            reinterpret_cast<const std::byte*>(p + off) + dl);
        off += dl;
        out.emplace_back(std::move(name), std::move(data));
    }
    return out;
}

/// 从 .lua 文件构建一个完整 ScriptState（加载脚本 + 起 worker）。
/// 失败返回 nullptr。
std::shared_ptr<ScriptState> load_script_state(caf::actor_system& sys,
                                               caf::actor logger,
                                               caf::actor registry,
                                               const std::filesystem::path& path) {
    auto st = std::make_shared<ScriptState>();
    st->sys = &sys;
    st->logger = logger;
    st->registry = registry;
    st->lua = std::make_shared<sol::state>();
    st->lua->open_libraries(sol::lib::base, sol::lib::string, sol::lib::table,
                            sol::lib::math, sol::lib::utf8);
    bind_bridge(*st->lua, st.get());

    auto result = st->lua->safe_script_file(path.string());
    if (!result.valid()) {
        sol::error err = result;
        LOG_ERROR("script load failed {}: {}", path.string(), err.what());
        return nullptr;
    }

    read_manifest(*st->lua, st->manifest, path);
    if (st->manifest.provides.empty())
        st->manifest.provides = path.stem().string();
    st->self_name = st->manifest.provides;

    st->queue = std::make_shared<JobQueue>();
    st->worker = std::make_shared<std::thread>(lua_worker_main, st);
    return st;
}

/// 由 ScriptState 生成子 actor（每个脚本一个 actor）。
caf::actor spawn_script_actor(caf::actor_system& sys,
                              std::shared_ptr<ScriptState> st) {
    return sys.spawn([st](caf::event_based_actor* self) -> caf::behavior {
        caf::message_handler business{
            [=](plugin_envelope env) {
                if (!self->current_message_id().is_request())
                    return;
                auto rp = self->make_response_promise<std::string>();
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::Call;
                job->env = std::move(env);
                job->done_str = [rp](const std::string& s) mutable {
                    rp.deliver(s);
                };
                st->queue->push(std::move(job));
            },
            [=](const std::string& cmd) {
                if (!self->current_message_id().is_request())
                    return;
                auto rp = self->make_response_promise<std::string>();
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::String;
                job->str = cmd;
                job->done_str = [rp](const std::string& s) mutable {
                    rp.deliver(s);
                };
                st->queue->push(std::move(job));
            },
            // 被 send_exit（宿主卸载/重载退役）时：停 worker 再退，避免悬空线程
            [=](caf::exit_msg& em) {
                stop_worker(st);
                self->quit(em.reason);
            },
        };
        return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
            .on_init = [st](caf::actor, const std::string&) {
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::Init;
                st->queue->push(std::move(job));
            },
            // 同步移交：on_save 的 request 需要同步字节返回，用 future 阻塞
            // actor 线程等 worker 序列化完成（单 worker 独占 lua_State，无竞态）。
            .on_save = [st]() -> std::vector<std::byte> {
                std::promise<std::string> p;
                auto f = p.get_future();
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::Save;
                job->done_str = [&p](const std::string& s) mutable {
                    p.set_value(s);
                };
                st->queue->push(std::move(job));
                std::string s = f.get();
                std::vector<std::byte> out(s.size());
                std::memcpy(out.data(), s.data(), s.size());
                return out;
            },
            .on_restore = [st](const std::vector<std::byte>& data) {
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::Restore;
                job->bytes = data;
                st->queue->push(std::move(job));
            },
            // 先跑 Lua on_shutdown（经 worker），再停 worker。on_shutdown 约定
            // 快清理、禁止阻塞 call，避免与停机链互等。
            .on_shutdown = [st]() {
                std::promise<void> p;
                auto f = p.get_future();
                auto job = std::make_shared<ScriptJob>();
                job->op = ScriptOp::Shutdown;
                job->done_void = [&p]() { p.set_value(); };
                st->queue->push(std::move(job));
                f.wait();
                stop_worker(st);
            },
        }))};
    });
}

} // namespace

// ------------------------------------------------------------------
// 宿主插件本体
// ------------------------------------------------------------------
class LuaHostPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"LuaHostPlugin", "1.0.0",
                 {},
                {"lua_host_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        auto cfg = load_plugin_config(sys.config());
        // 脚本目录：显式配置优先，否则 asset_dir/scripts（框架注入 DLL 目录）
        std::string scripts_dir = cfg.scripts_dir.empty()
                                      ? this->asset_path("scripts")
                                      : cfg.scripts_dir;

        return sys.spawn([logger, scripts_dir](caf::event_based_actor* self)
                             -> caf::behavior {
            // 服务注册表：动态注册/换实现脚本服务用。与 shutdown_mgr 同款
            // 注入方式（见 framework_bootstrap.cpp 的 put("service_registry")）。
            caf::actor registry = caf::actor_cast<caf::actor>(
                self->system().registry().get("service_registry"));

            // 脚本实例台账：服务名 → (子 actor, 脚本路径)
            auto instances = std::make_shared<
                std::map<std::string, std::pair<caf::actor, std::string>>>();

            // 加载一个脚本并注册为服务（on_init 里逐个调用）
            auto spawn_script = [=](caf::event_based_actor* self,
                                    const std::string& path) {
                auto st = load_script_state(self->system(), logger, registry,
                                            std::filesystem::path{path});
                if (!st) {
                    LOG_ERROR_SELF(self, "failed to load script: {}", path);
                    return;
                }
                auto child = spawn_script_actor(self->system(), st);
                self->send(registry, register_atom{}, st->self_name, child,
                           "LuaHostPlugin");
                (*instances)[st->self_name] = {child, path};
                // 触发子脚本 on_init（框架只对 PluginManager 加载的插件发
                // init_atom，子脚本是宿主直接 spawn 的，须宿主补发）
                self->send(child, init_atom{}, caf::actor{self}, std::string{});
                LOG_INFO_SELF(self, "script loaded: {} (service={}, deps={})",
                              std::filesystem::path{path}.filename().string(),
                              st->self_name, st->manifest.deps.size());
            };

            // 重载脚本（完整热更，镜像框架 §8 先排空后快照）：
            //   1. 准备：加载新脚本 + spawn 新实例（失败时未动旧实现）；
            //   2. quiesce 该脚本服务代理并等 ack——ack 前到达的调用都已
            //      委托给旧实例，此后新调用进缓冲（断流）；
            //   3. request 旧实例 save_state：邮箱到达序屏障，响应时旧实例
            //      已处理完全部在途调用，快照即终态；
            //   4. 新实例 restore（如有状态）+ init；
            //   5. registry hot_reload 台账 + resume 代理（切目标 + 冲刷缓冲）；
            //   6. 旧实例立即 send_exit 退役（快照后无新工作，状态冻结）。
            auto reload_script = [=](caf::event_based_actor* self,
                                     const std::string& service_name) {
                auto it = instances->find(service_name);
                if (it == instances->end()) {
                    LOG_WARN_SELF(self, "reload: unknown script service {}", service_name);
                    return;
                }
                std::string path = it->second.second;
                caf::actor old_child = it->second.first;

                // 1. 准备新实例（失败直接返回，旧实现不受影响）
                auto st = load_script_state(self->system(), logger, registry,
                                            std::filesystem::path{path});
                if (!st) {
                    LOG_ERROR_SELF(self, "reload: failed to load {}", path);
                    return;
                }
                auto new_child = spawn_script_actor(self->system(), st);

                caf::scoped_actor blocking{self->system()};

                // 2. quiesce 代理：静默 + 缓冲，等 ack（失败则销毁新实例回滚）
                caf::actor proxy;
                blocking->request(registry, caf::infinite, resolve_atom_v, service_name)
                    .receive([&](caf::actor a) { proxy = std::move(a); },
                             [](caf::error&) {});
                bool paused = false;
                if (proxy) {
                    blocking->request(proxy, std::chrono::seconds(5), quiesce_atom_v)
                        .receive([&](bool b) { paused = b; }, [](caf::error&) {});
                }
                if (!paused) {
                    self->send_exit(new_child, caf::exit_reason::user_shutdown);
                    LOG_ERROR_SELF(self, "reload: failed to quiesce proxy for {}",
                                   service_name);
                    return;
                }

                // 3. 快照：旧实例已排空，save_state 响应即终态
                std::vector<std::byte> state;
                blocking->request(old_child, std::chrono::seconds(5), save_state_atom_v)
                    .receive([&](const std::vector<std::byte>& d) { state = d; },
                             [](caf::error&) {});

                // 4. 新实例：restore 状态 + init
                if (!state.empty())
                    self->send(new_child, restore_state_atom_v, state);
                self->send(new_child, init_atom{}, caf::actor{self}, std::string{});

                // 5. registry 换实现 + resume 代理（切目标 + 冲刷缓冲）
                self->send(registry, hot_reload_atom{}, service_name, new_child);
                if (proxy)
                    self->send(proxy, resume_atom{}, new_child);

                // 6. 旧实例退役：快照即终态，立即 send_exit（exit 后邮箱消息
                //    被 CAF 丢弃，状态冻结在快照时刻）
                self->send_exit(old_child, caf::exit_reason::user_shutdown);
                it->second.first = new_child;
                LOG_INFO_SELF(self, "script reloaded: {}", service_name);
            };

            caf::message_handler business{
                // 列已加载脚本服务名
                [=](list_atom) -> std::vector<std::string> {
                    std::vector<std::string> names;
                    names.reserve(instances->size());
                    for (const auto& [service_name, pr] : *instances)
                        names.push_back(service_name);
                    return names;
                },
                // 管理信封：sub_proto=1 → 重载脚本（payload = 服务名）
                [=](plugin_envelope env) {
                    if (env.sub_proto != lua_reload_sub_proto)
                        return;
                    std::string service_name(
                        reinterpret_cast<const char*>(env.payload.data()),
                        env.payload.size());
                    reload_script(self, service_name);
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "LuaHostPlugin initializing, scripts dir: {}",
                                  scripts_dir);
                    if (!std::filesystem::exists(scripts_dir)) {
                        LOG_WARN_SELF(self, "scripts dir not found: {}", scripts_dir);
                        return;
                    }
                    for (const auto& entry :
                         std::filesystem::directory_iterator(scripts_dir)) {
                        if (!entry.is_regular_file())
                            continue;
                        if (entry.path().extension().string() != ".lua")
                            continue;
                        spawn_script(self, entry.path().string());
                    }
                },
                // 聚合子脚本状态为单一字节串（checkpoint 按宿主名落盘，
                // 下次启动 PluginManager 会 restore_state_atom 回宿主）
                .on_save = [=]() -> std::vector<std::byte> {
                    caf::scoped_actor blocking{self->system()};
                    std::string buf;
                    for (const auto& [service_name, pr] : *instances) {
                        std::vector<std::byte> data;
                        blocking
                            ->request(pr.first, std::chrono::seconds(5),
                                      save_state_atom_v)
                            .receive(
                                [&](const std::vector<std::byte>& d) {
                                    data = d;
                                },
                                [](caf::error&) {});
                        append_frame(buf, service_name, data);
                    }
                    return std::vector<std::byte>(
                        reinterpret_cast<const std::byte*>(buf.data()),
                        reinterpret_cast<const std::byte*>(buf.data())
                            + buf.size());
                },
                // 恢复：拆帧后逐个发给子脚本（子脚本 on_restore 走 worker）
                .on_restore = [=](const std::vector<std::byte>& blob) {
                    for (auto& [service_name, data] : parse_frames(blob)) {
                        auto it = instances->find(service_name);
                        if (it != instances->end())
                            self->send(it->second.first, restore_state_atom_v,
                                       data);
                    }
                },
                // 关机：先摘服务，再发 shutdown_atom（子脚本 on_shutdown 跑
                // Lua 清理 + 停 worker，随后自 quit），最后 wait_for 等子脚本
                // 全部退出——保证 worker 线程在宿主退出前 join，不遗留悬空线程
                .on_shutdown = [=]() {
                    size_t n = instances->size();
                    caf::scoped_actor blocking{self->system()};
                    std::vector<caf::actor> children;
                    children.reserve(n);
                    for (auto& [service_name, pr] : *instances) {
                        self->send(registry, unregister_atom{}, service_name);
                        if (pr.first) {
                            self->send(pr.first, shutdown_atom{});
                            children.push_back(pr.first);
                        }
                    }
                    for (auto& c : children)
                        blocking->wait_for(c);
                    instances->clear();
                    std::cout << "[LuaHostPlugin] shutdown hook: " << n
                              << " script(s) torn down" << std::endl;
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new LuaHostPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
