// ------------------------------------------------------------------
// TypeScript 脚本宿主插件（ts_host）
//
// 目标：让 .ts 脚本也能当插件用。与 lua_host/py_host 同构，区别只在运行时：
// QuickJS 内嵌（vendor 源码）。构建时用 tsc 把 .ts 预编译成 .js，宿主扫描
// scripts/*.js，每个脚本 = 一个 JSContext + 一个 worker 线程 + 一等 CAF 服务。
//
// 脚本约定（.ts 侧，与 .lua/.py 对齐）：
//   const plugin = { name, version, provides, deps }
//   function on_init(manager) / on_call(fn, payload) / on_string(cmd)
//   function on_save() / on_restore(state_str) / on_shutdown()
//
// 桥接 API（脚本全局可用）：log / call / call_string / config / self_name。
// Date.now()/new Date()/Date() 由宿主自动 hook 到统一业务时间。
//
// 关键决策：
//   - 零新 type_id / 免 register_meta_objects：全程 plugin_envelope + std::string。
//   - QuickJS 单线程每 context（类似 lua_State），worker 独占，无 GIL。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "services/time_service.hpp"
#include "common/plugin_config.hpp"
#include "common/plugin_envelope.hpp"
#include "templates/job_queue.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>

#include "quickjs/quickjs.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 声明式配置（PLUGIN_CONFIG，见 common/plugin_config.hpp）。scripts_dir 留空 = asset_dir/scripts。
#define TS_FIELDS(X, XC)                                                       \
    X(std::string, scripts_dir, "")
PLUGIN_CONFIG(TS_FIELDS)
#undef TS_FIELDS

// 宿主管理方法名（走 plugin_envelope，发给 ts_host_service）
//   "reload"：payload = 脚本服务名 → 重载
constexpr std::string_view k_reload_function = "reload";

struct ScriptManifest {
    std::string name;
    std::string version;
    std::string provides;
    std::vector<std::string> deps;
    external_protocol_table protocols;  // 外部协议号→function 契约表
};

enum class ScriptOp { Init, Call, String, Save, Restore, Shutdown };

struct ScriptJob {
    ScriptOp op = ScriptOp::Call;
    plugin_envelope env;
    std::string str;
    std::vector<std::byte> bytes;
    std::function<void(const std::string&)> done_str;
    std::function<void(std::vector<std::byte>)> done_bytes;
    std::function<void()> done_void;

    void fail(const std::string& err) {
        if (done_str)
            done_str("js error: " + err);
        else if (done_bytes)
            done_bytes({});
        else if (done_void)
            done_void();
    }
};

using JobQueue = caf_plugin_system::JobQueue<ScriptJob>;

// 脚本实例共享状态：子 actor 与 worker 线程共同持有
struct ScriptState {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    ScriptManifest manifest;
    caf::actor logger;
    caf::actor registry;
    caf::actor_system* sys = nullptr;
    std::string self_name;
    std::shared_ptr<JobQueue> queue;
    std::shared_ptr<std::thread> worker;
    std::atomic<bool> stopped{false};
};

/// TLS 当前脚本（桥接函数读它定位上下文，等价 lua_host 里 lambda 捕获 st）
thread_local ScriptState* g_current_script = nullptr;

// ------------------------------------------------------------------
// 桥接辅助（跑在 worker 线程，脚本里同步调用）
// ------------------------------------------------------------------

std::pair<bool, std::string> call_service(caf::actor_system& sys,
                                          caf::actor registry,
                                          const std::string& service,
                                          const std::string& function,
                                          const std::string& payload) {
    caf::scoped_actor self{sys};
    caf::actor proxy;
    self->request(registry, std::chrono::seconds(5), resolve_atom_v, service)
        .receive([&](caf::actor a) { proxy = std::move(a); },
                 [](caf::error&) {});

    if (!proxy)
        return {false, "service not found: " + service};

    auto env = plugin_wire::encode_text(function, payload);
    if (!env)
        return {false, "unsupported payload format"};

    bool ok = false;
    std::string reply;
    self->request(proxy, std::chrono::seconds(5), *env).receive(
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

// ------------------------------------------------------------------
// 桥接函数（暴露给脚本的 JS C 函数）
// ------------------------------------------------------------------

static JSValue js_log(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_UNDEFINED;
    const char* level = JS_ToCString(ctx, argv[0]);
    const char* msg = JS_ToCString(ctx, argv[1]);
    if (g_current_script && g_current_script->logger && level && msg)
        caf::anon_send(g_current_script->logger, log_atom{},
                       g_current_script->self_name, std::string(level),
                       std::string("[ts] ") + msg);
    if (level) JS_FreeCString(ctx, level);
    if (msg) JS_FreeCString(ctx, msg);
    return JS_UNDEFINED;
}

static JSValue js_call(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 3)
        return JS_UNDEFINED;
    const char* service = JS_ToCString(ctx, argv[0]);
    const char* function = JS_ToCString(ctx, argv[1]);
    const char* payload = JS_ToCString(ctx, argv[2]);

    bool ok = false;
    std::string reply;
    if (g_current_script && g_current_script->sys && service && function
        && payload)
        std::tie(ok, reply) = call_service(*g_current_script->sys,
                                           g_current_script->registry, service,
                                           function, payload);
    else
        reply = "no script context";

    if (service) JS_FreeCString(ctx, service);
    if (function) JS_FreeCString(ctx, function);
    if (payload) JS_FreeCString(ctx, payload);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, ok));
    JS_SetPropertyStr(ctx, obj, "reply", JS_NewString(ctx, reply.c_str()));
    return obj;
}

static JSValue js_call_string(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    if (argc < 2)
        return JS_UNDEFINED;
    const char* service = JS_ToCString(ctx, argv[0]);
    const char* cmd = JS_ToCString(ctx, argv[1]);

    bool ok = false;
    std::string reply;
    if (g_current_script && g_current_script->sys && service && cmd)
        std::tie(ok, reply) = call_string_service(*g_current_script->sys,
                                                  g_current_script->registry,
                                                  service, cmd);
    else
        reply = "no script context";

    if (service) JS_FreeCString(ctx, service);
    if (cmd) JS_FreeCString(ctx, cmd);

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "ok", JS_NewBool(ctx, ok));
    JS_SetPropertyStr(ctx, obj, "reply", JS_NewString(ctx, reply.c_str()));
    return obj;
}

static JSValue js_config(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
    std::string v;
    if (argc >= 1 && g_current_script && g_current_script->sys) {
        const char* key = JS_ToCString(ctx, argv[0]);
        if (key) {
            v = caf::get_or(g_current_script->sys->config(),
                            "caf-plugin-system.ts_host." + std::string(key),
                            std::string{});
            JS_FreeCString(ctx, key);
        }
    }
    return JS_NewString(ctx, v.c_str());
}

static JSValue js_self_name(JSContext* ctx, JSValueConst, int, JSValueConst*) {
    const char* n = g_current_script ? g_current_script->self_name.c_str() : "";
    return JS_NewString(ctx, n);
}

static JSValue js_business_time_ms(JSContext* ctx, JSValueConst, int,
                                   JSValueConst*) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  caf_plugin_system::business_now().time_since_epoch())
                  .count();
    return JS_NewFloat64(ctx, static_cast<double>(ms));
}

void inject_bridge(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "log",
                      JS_NewCFunction(ctx, js_log, "log", 2));
    JS_SetPropertyStr(ctx, global, "call",
                      JS_NewCFunction(ctx, js_call, "call", 3));
    JS_SetPropertyStr(ctx, global, "call_string",
                      JS_NewCFunction(ctx, js_call_string, "call_string", 2));
    JS_SetPropertyStr(ctx, global, "config",
                      JS_NewCFunction(ctx, js_config, "config", 1));
    JS_SetPropertyStr(ctx, global, "self_name",
                      JS_NewCFunction(ctx, js_self_name, "self_name", 0));
    JS_SetPropertyStr(ctx, global, "_caf_business_time_ms",
                      JS_NewCFunction(ctx, js_business_time_ms,
                                     "_caf_business_time_ms", 0));
    JS_FreeValue(ctx, global);

    constexpr const char* hook = R"js(
        (() => {
            const NativeDate = Date;
            const clock = globalThis._caf_business_time_ms;
            const BusinessDate = new Proxy(NativeDate, {
                apply() {
                    return new NativeDate(clock()).toString();
                },
                construct(target, args, newTarget) {
                    return Reflect.construct(
                        target, args.length === 0 ? [clock()] : args, newTarget);
                }
            });
            BusinessDate.now = clock;
            globalThis.Date = BusinessDate;
            delete globalThis._caf_business_time_ms;
        })();
    )js";
    JSValue result = JS_Eval(ctx, hook, std::strlen(hook), "<time-hook>",
                             JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JS_FreeValue(ctx, result);
        throw std::runtime_error("install JavaScript Date hooks failed");
    }
    JS_FreeValue(ctx, result);
}

/// 读脚本 `plugin` 对象（manifest）。须在 worker 线程调用。
void read_manifest(ScriptState& st) {
    JSContext* ctx = st.ctx;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue plugin = JS_GetPropertyStr(ctx, global, "plugin");
    if (JS_IsObject(plugin)) {
        auto get_str = [&](const char* key) -> std::string {
            JSValue v = JS_GetPropertyStr(ctx, plugin, key);
            std::string s;
            if (JS_IsString(v)) {
                const char* c = JS_ToCString(ctx, v);
                if (c) { s = c; JS_FreeCString(ctx, c); }
            }
            JS_FreeValue(ctx, v);
            return s;
        };
        st.manifest.name = get_str("name");
        st.manifest.version = get_str("version");
        st.manifest.provides = get_str("provides");

        JSValue deps = JS_GetPropertyStr(ctx, plugin, "deps");
        if (JS_IsArray(deps)) {
            int64_t len = 0;
            JSValue lv = JS_GetPropertyStr(ctx, deps, "length");
            JS_ToInt64(ctx, &len, lv);
            JS_FreeValue(ctx, lv);
            for (int64_t i = 0; i < len; ++i) {
                JSValue d = JS_GetPropertyUint32(ctx, deps, static_cast<uint32_t>(i));
                if (JS_IsString(d)) {
                    const char* c = JS_ToCString(ctx, d);
                    if (c) { st.manifest.deps.push_back(c); JS_FreeCString(ctx, c); }
                }
                JS_FreeValue(ctx, d);
            }
        }
        JS_FreeValue(ctx, deps);

        // 外部协议表：protocols = {1: "echo"} —— 外部协议号 → function
        JSValue protos = JS_GetPropertyStr(ctx, plugin, "protocols");
        if (JS_IsObject(protos)) {
            JSPropertyEnum* tab = nullptr;
            uint32_t n = 0;
            if (JS_GetOwnPropertyNames(ctx, &tab, &n, protos,
                                       JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK)
                == 0) {
                for (uint32_t i = 0; i < n; ++i) {
                    JSValue k = JS_AtomToString(ctx, tab[i].atom);
                    JSValue v = JS_GetProperty(ctx, protos, tab[i].atom);
                    if (JS_IsString(k) && JS_IsString(v)) {
                        const char* ks = JS_ToCString(ctx, k);
                        const char* vs = JS_ToCString(ctx, v);
                        if (ks && vs) {
                            char* endp = nullptr;
                            auto num = std::strtoul(ks, &endp, 10);
                            if (endp != ks && *endp == '\0' && num <= 0xFFFF)
                                st.manifest.protocols.emplace(
                                    static_cast<std::uint16_t>(num),
                                    std::string(vs));
                        }
                        if (ks) JS_FreeCString(ctx, ks);
                        if (vs) JS_FreeCString(ctx, vs);
                    }
                    JS_FreeValue(ctx, k);
                    JS_FreeValue(ctx, v);
                }
                js_free(ctx, tab);
            }
        }
        JS_FreeValue(ctx, protos);
    }
    JS_FreeValue(ctx, plugin);
    JS_FreeValue(ctx, global);
}

/// 执行单个 job 的 JS 逻辑（在 worker 线程调用）。
void run_job(ScriptState& st, ScriptJob& job) {
    JSContext* ctx = st.ctx;
    g_current_script = &st;

    // 调用全局函数，返回其字符串结果（无/非字符串 → 空串）
    auto call_str = [&](const char* name, int argc, JSValueConst* argv) -> std::string {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue f = JS_GetPropertyStr(ctx, global, name);
        std::string out;
        if (JS_IsFunction(ctx, f)) {
            JSValue r = JS_Call(ctx, f, global, argc, argv);
            if (JS_IsException(r)) {
                JSValue exc = JS_GetException(ctx);
                const char* m = JS_ToCString(ctx, exc);
                if (m) { LOG_ERROR("script '{}' {} error: {}", st.self_name, name, m); JS_FreeCString(ctx, m); }
                JS_FreeValue(ctx, exc);
            } else if (JS_IsString(r)) {
                const char* s = JS_ToCString(ctx, r);
                if (s) { out = s; JS_FreeCString(ctx, s); }
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, global);
        return out;
    };
    auto call_void = [&](const char* name, int argc, JSValueConst* argv) {
        JSValue global = JS_GetGlobalObject(ctx);
        JSValue f = JS_GetPropertyStr(ctx, global, name);
        if (JS_IsFunction(ctx, f)) {
            JSValue r = JS_Call(ctx, f, global, argc, argv);
            if (JS_IsException(r)) {
                JSValue exc = JS_GetException(ctx);
                const char* m = JS_ToCString(ctx, exc);
                if (m) { LOG_ERROR("script '{}' {} error: {}", st.self_name, name, m); JS_FreeCString(ctx, m); }
                JS_FreeValue(ctx, exc);
            }
            JS_FreeValue(ctx, r);
        }
        JS_FreeValue(ctx, f);
        JS_FreeValue(ctx, global);
    };

    switch (job.op) {
        case ScriptOp::Init: {
            JSValue a = JS_NewString(ctx, "<manager>");
            call_void("on_init", 1, &a);
            JS_FreeValue(ctx, a);
            if (job.done_void) job.done_void();
            break;
        }
        case ScriptOp::Call: {
            auto payload = plugin_wire::decode_text(job.env);
            if (!payload) {
                if (job.done_str)
                    job.done_str("unsupported payload format");
                break;
            }
            JSValue av[2] = { JS_NewString(ctx, job.env.function.c_str()),
                              JS_NewString(ctx, payload->c_str()) };
            std::string out = call_str("on_call", 2, av);
            JS_FreeValue(ctx, av[0]);
            JS_FreeValue(ctx, av[1]);
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::String: {
            JSValue a = JS_NewString(ctx, job.str.c_str());
            std::string out = call_str("on_string", 1, &a);
            JS_FreeValue(ctx, a);
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Save: {
            std::string out = call_str("on_save", 0, nullptr);
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Restore: {
            std::string data(reinterpret_cast<const char*>(job.bytes.data()),
                             job.bytes.size());
            JSValue a = JS_NewString(ctx, data.c_str());
            call_void("on_restore", 1, &a);
            JS_FreeValue(ctx, a);
            if (job.done_void) job.done_void();
            break;
        }
        case ScriptOp::Shutdown: {
            call_void("on_shutdown", 0, nullptr);
            if (job.done_void) job.done_void();
            break;
        }
    }

    g_current_script = nullptr;
}

/// worker 主循环：独占 JSContext，串行执行队列。
void js_worker_main(std::shared_ptr<ScriptState> st) {
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
    // 退出前回收 context + runtime
    if (st->ctx) {
        JS_FreeContext(st->ctx);
        st->ctx = nullptr;
    }
    if (st->rt) {
        JS_FreeRuntime(st->rt);
        st->rt = nullptr;
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

/// 从 .js 文件构建 ScriptState（QuickJS context + 起 worker）。失败返回 null。
std::shared_ptr<ScriptState> load_script_state(caf::actor_system& sys,
                                               caf::actor logger,
                                               caf::actor registry,
                                               const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LOG_ERROR("script open failed: {}", path.string());
        return nullptr;
    }
    std::string src((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());

    auto st = std::make_shared<ScriptState>();
    st->sys = &sys;
    st->logger = logger;
    st->registry = registry;

    st->rt = JS_NewRuntime();
    if (!st->rt)
        return nullptr;
    st->ctx = JS_NewContext(st->rt);
    if (!st->ctx) {
        JS_FreeRuntime(st->rt);
        st->rt = nullptr;
        return nullptr;
    }
    inject_bridge(st->ctx);

    JSValue r = JS_Eval(st->ctx, src.c_str(), src.size(), path.string().c_str(),
                        JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue exc = JS_GetException(st->ctx);
        const char* m = JS_ToCString(st->ctx, exc);
        LOG_ERROR("script eval failed {}: {}", path.string(), m ? m : "?");
        if (m) JS_FreeCString(st->ctx, m);
        JS_FreeValue(st->ctx, exc);
        JS_FreeValue(st->ctx, r);
        JS_FreeContext(st->ctx);
        JS_FreeRuntime(st->rt);
        return nullptr;
    }
    JS_FreeValue(st->ctx, r);

    read_manifest(*st);
    if (st->manifest.provides.empty())
        st->manifest.provides = path.stem().string();
    st->self_name = st->manifest.provides;

    st->queue = std::make_shared<JobQueue>();
    st->worker = std::make_shared<std::thread>(js_worker_main, st);
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
class TsHostPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"TsHostPlugin", "1.0.0",
                 {},
                {"ts_host_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        auto cfg = load_plugin_config(sys.config());
        std::string scripts_dir = cfg.scripts_dir.empty()
                                      ? this->asset_path("scripts")
                                      : cfg.scripts_dir;

        return sys.spawn([logger, scripts_dir](caf::event_based_actor* self)
                             -> caf::behavior {
            caf::actor registry = caf::actor_cast<caf::actor>(
                self->system().registry().get("service_registry"));

            auto instances = std::make_shared<
                std::map<std::string, std::pair<caf::actor, std::string>>>();

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
                           "TsHostPlugin", st->manifest.protocols);
                (*instances)[st->self_name] = {child, path};
                self->send(child, init_atom{}, caf::actor{self}, std::string{});
                LOG_INFO_SELF(self, "script loaded: {} (service={}, deps={})",
                              std::filesystem::path{path}.filename().string(),
                              st->self_name, st->manifest.deps.size());
            };

            auto reload_script = [=](caf::event_based_actor* self,
                                     const std::string& service_name) {
                auto it = instances->find(service_name);
                if (it == instances->end()) {
                    LOG_WARN_SELF(self, "reload: unknown script service {}", service_name);
                    return;
                }
                std::string path = it->second.second;
                caf::actor old_child = it->second.first;

                auto st = load_script_state(self->system(), logger, registry,
                                            std::filesystem::path{path});
                if (!st) {
                    LOG_ERROR_SELF(self, "reload: failed to load {}", path);
                    return;
                }
                auto new_child = spawn_script_actor(self->system(), st);

                caf::scoped_actor blocking{self->system()};
                caf::actor proxy;
                blocking->request(registry, std::chrono::seconds(5),
                                  resolve_atom_v, service_name)
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

                std::vector<std::byte> state;
                blocking->request(old_child, std::chrono::seconds(5), save_state_atom_v)
                    .receive([&](const std::vector<std::byte>& d) { state = d; },
                             [](caf::error&) {});

                if (!state.empty())
                    self->send(new_child, restore_state_atom_v, state);
                self->send(new_child, init_atom{}, caf::actor{self}, std::string{});

                self->send(registry, hot_reload_atom{}, service_name, new_child);
                if (proxy)
                    self->send(proxy, resume_atom{}, new_child);
                self->send_exit(old_child, caf::exit_reason::user_shutdown);
                it->second.first = new_child;
                LOG_INFO_SELF(self, "script reloaded: {}", service_name);
            };

            caf::message_handler business{
                [=](list_atom) -> std::vector<std::string> {
                    std::vector<std::string> names;
                    names.reserve(instances->size());
                    for (const auto& [service_name, pr] : *instances)
                        names.push_back(service_name);
                    return names;
                },
                [=](plugin_envelope env) {
                    if (env.function != k_reload_function)
                        return;
                    if (auto service_name = plugin_wire::decode_text(env))
                        reload_script(self, *service_name);
                    else
                        LOG_WARN_SELF(self, "ignoring reload with unsupported payload format");
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "TsHostPlugin initializing, scripts dir: {}",
                                  scripts_dir);
                    if (!std::filesystem::exists(scripts_dir)) {
                        LOG_WARN_SELF(self, "scripts dir not found: {}", scripts_dir);
                        return;
                    }
                    for (const auto& entry :
                         std::filesystem::directory_iterator(scripts_dir)) {
                        if (!entry.is_regular_file())
                            continue;
                        if (entry.path().extension().string() != ".js")
                            continue;
                        spawn_script(self, entry.path().string());
                    }
                },
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
                .on_restore = [=](const std::vector<std::byte>& blob) {
                    for (auto& [service_name, data] : parse_frames(blob)) {
                        auto it = instances->find(service_name);
                        if (it != instances->end())
                            self->send(it->second.first, restore_state_atom_v,
                                       data);
                    }
                },
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
                    std::cout << "[TsHostPlugin] shutdown hook: " << n
                              << " script(s) torn down" << std::endl;
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new TsHostPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
