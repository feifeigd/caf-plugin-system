// ------------------------------------------------------------------
// Python 脚本宿主插件（py_host）
//
// 目标：让 .py 脚本也能当插件用——把 scripts/ 目录下的每个 .py 包装成
// 一个一等 CAF 服务（一个 actor + 一个模块命名空间 + 一个 worker 线程），
// 复用框架的 lifecycle / registry / 优雅关机 / 热更（完整 quiesce/快照）。
//
// 与 lua_host 的差异只在运行时：CPython 有 GIL。v1 用【共享解释器】：
// Py_Initialize 一次，每脚本 worker 线程用 PyGILState_Ensure/Release 持 GIL
// 跑 Python，bridge.call 阻塞期间 Py_BEGIN_ALLOW_THREADS 释放 GIL（I/O 型
// 脚本可互相重叠）。子解释器（PEP 684 每解释器 GIL，真并行）留后续。
//
// 脚本约定（.py 侧，与 .lua 对齐）：
//   plugin = {"name":..., "version":..., "provides":..., "deps":[...]}
//   def on_init(manager): ...
//   def on_call(sub_proto, payload): return "..."    # envelope 业务入口
//   def on_string(cmd): return "..."
//   def on_save(): return "状态串"
//   def on_restore(state_str): ...
//   def on_shutdown(): ...
//
// 桥接 API（脚本全局可用）：log / call / call_string / config / self_name / now。
//
// 关键决策：
//   - 零新 type_id / 免 register_meta_objects：全程 plugin_envelope(228) +
//     std::string。
//   - 每脚本一个模块 dict（共享解释器内隔离 globals）；桥接上下文用 TLS。
//   - 单 worker 独占脚本模块：event actor 不阻塞，bridge.call 在 worker 线程
//     阻塞 request（先释放 GIL），Lua 版同构。
//
// 运行时依赖部署（2026-08-31 定稿）：
//   python 依赖全部随插件走（不进 lib/ 分类目录）：run/plugins/py_host/ 下
//   放 python312_d.dll + python3_d.dll + Lib/（标准库，标准 <home>/Lib 布局）。
//   启动必须设 PYTHONHOME=<插件目录>——CPython 3.12 Windows getpath 的
//   prefix 取 exe 目录 / PYTHONHOME，stdlib 只认 <prefix>/Lib；DLL 目录
//   混放 .py 不生效（init_fs_encoding 崩）。python312_d.dll 的 DLL 解析走
//   framework_bootstrap 的 AddDllDirectory(plugins/*/)（USER_DIRS）。
//
//   业务脚本的第三方依赖（将来脚本 import requests/pydantic 等）：用 uv
//   装进 Lib/site-packages/（PYTHONHOME 布局自动识别，site 模块自动入 path）：
//     uv pip install --python <vcpkg tools\python3\python.exe> \
//       --target run/plugins/py_host/Lib/site-packages <pkg>
//   解释器本体必须 vcpkg debug 版（/MDd + python312_d.lib）；uv 只有 release
//   版 CPython，CRT 不匹配且 Debug CRT 泄漏检测失效，不可用于解释器。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/plugin_config.hpp"
#include "common/plugin_envelope.hpp"
#include "templates/job_queue.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>

// Python.h 必须最先包含（它要压 Windows 头警告）。vcpkg 的 python3 同时提供
// debug（python312_d.lib）与 release（python312.lib），debug 构建下 Python.h
// 的 _DEBUG auto-link 正确指向 debug 库，无需特殊处理。
#include <Python.h>

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
#include <mutex>
#include <condition_variable>
#include <utility>
#include <vector>

namespace {

// 声明式配置（PLUGIN_CONFIG，见 common/plugin_config.hpp）。
// scripts_dir 留空 = 用 asset_dir/scripts。
#define PY_FIELDS(X, XC)                                                       \
    X(std::string, scripts_dir, "")
PLUGIN_CONFIG(PY_FIELDS)
#undef PY_FIELDS

// 保留的宿主管理子协议号（走 plugin_envelope，发给 py_host_service）
constexpr uint16_t py_reload_sub_proto = 1;   // payload = 脚本服务名 → 重载

// 脚本清单（从 Python `plugin` dict 读取）
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

    void fail(const std::string& err) {
        if (done_str)
            done_str("python error: " + err);
        else if (done_bytes)
            done_bytes({});
        else if (done_void)
            done_void();
    }
};

using JobQueue = caf_plugin_system::JobQueue<ScriptJob>;

// 脚本实例共享状态：子 actor 与 worker 线程共同持有
struct ScriptState {
    PyObject* module_dict = nullptr;   // 脚本命名空间（globals dict，worker 退出时 DECREF）
    ScriptManifest manifest;
    caf::actor logger;
    caf::actor registry;
    caf::actor_system* sys = nullptr;
    std::string self_name;
    std::shared_ptr<JobQueue> queue;
    std::shared_ptr<std::thread> worker;
    std::atomic<bool> stopped{false};
};

// ------------------------------------------------------------------
// 桥接辅助（跑在 worker 线程，脚本里同步调用）
// ------------------------------------------------------------------

/// TLS 当前脚本（桥接函数读它定位上下文，等价 lua_host 里 lambda 捕获 st）
thread_local ScriptState* g_current_script = nullptr;

/// 同步调用服务：resolve → 发 plugin_envelope → 等 std::string 回复。
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

    auto env = plugin_wire::encode_text(sub_proto, payload);
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

// ------------------------------------------------------------------
// 桥接函数（暴露给脚本的 C 函数，METH_VARARGS/METH_NOARGS）
// ------------------------------------------------------------------

static PyObject* py_log(PyObject*, PyObject* args) {
    const char* level = nullptr;
    const char* msg = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &level, &msg))
        return nullptr;
    if (g_current_script && g_current_script->logger)
        caf::anon_send(g_current_script->logger, log_atom{},
                       g_current_script->self_name, std::string(level),
                       std::string("[py] ") + msg);
    Py_RETURN_NONE;
}

static PyObject* py_call(PyObject*, PyObject* args) {
    const char* service = nullptr;
    int sub_proto = 0;
    const char* payload = nullptr;
    if (!PyArg_ParseTuple(args, "sis", &service, &sub_proto, &payload))
        return nullptr;
    if (!g_current_script || !g_current_script->sys) {
        PyErr_SetString(PyExc_RuntimeError, "no script context");
        return nullptr;
    }
    bool ok = false;
    std::string reply;
    Py_BEGIN_ALLOW_THREADS
    auto r = call_service(*g_current_script->sys, g_current_script->registry,
                          service, static_cast<uint16_t>(sub_proto), payload);
    ok = r.first;
    reply = std::move(r.second);
    Py_END_ALLOW_THREADS
    return Py_BuildValue("(Os)", ok ? Py_True : Py_False, reply.c_str());
}

static PyObject* py_call_string(PyObject*, PyObject* args) {
    const char* service = nullptr;
    const char* cmd = nullptr;
    if (!PyArg_ParseTuple(args, "ss", &service, &cmd))
        return nullptr;
    if (!g_current_script || !g_current_script->sys) {
        PyErr_SetString(PyExc_RuntimeError, "no script context");
        return nullptr;
    }
    bool ok = false;
    std::string reply;
    Py_BEGIN_ALLOW_THREADS
    auto r = call_string_service(*g_current_script->sys,
                                 g_current_script->registry, service, cmd);
    ok = r.first;
    reply = std::move(r.second);
    Py_END_ALLOW_THREADS
    return Py_BuildValue("(Os)", ok ? Py_True : Py_False, reply.c_str());
}

static PyObject* py_config(PyObject*, PyObject* args) {
    const char* key = nullptr;
    if (!PyArg_ParseTuple(args, "s", &key))
        return nullptr;
    std::string v;
    if (g_current_script && g_current_script->sys)
        v = caf::get_or(g_current_script->sys->config(),
                        "caf-plugin-system.py_host." + std::string(key),
                        std::string{});
    return PyUnicode_FromString(v.c_str());
}

static PyObject* py_self_name(PyObject*, PyObject*) {
    if (!g_current_script)
        return PyUnicode_FromString("");
    return PyUnicode_FromString(g_current_script->self_name.c_str());
}

static PyObject* py_now(PyObject*, PyObject*) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::steady_clock::now().time_since_epoch())
                  .count();
    return PyLong_FromLongLong(static_cast<long long>(ms));
}

static PyMethodDef bridge_methods[] = {
    {"log", py_log, METH_VARARGS, "log(level, msg)"},
    {"call", py_call, METH_VARARGS,
     "call(service, sub_proto, payload) -> (ok, reply)"},
    {"call_string", py_call_string, METH_VARARGS,
     "call_string(service, cmd) -> (ok, reply)"},
    {"config", py_config, METH_VARARGS, "config(key) -> value"},
    {"self_name", py_self_name, METH_NOARGS, "self_name() -> name"},
    {"now", py_now, METH_NOARGS, "now() -> ms"},
    {nullptr, nullptr, 0, nullptr}
};

/// 把桥接函数注入脚本 globals（等价 lua_host 的 sol2 set_function）
void inject_bridge(PyObject* globals) {
    for (PyMethodDef* m = bridge_methods; m->ml_name; ++m) {
        PyObject* fn = PyCFunction_New(m, nullptr);
        PyDict_SetItemString(globals, m->ml_name, fn);
        Py_DECREF(fn);
    }
}

/// 读脚本 `plugin` dict（manifest）。须在持 GIL 下调用。
void read_manifest(ScriptState& st) {
    PyObject* plugin = PyDict_GetItemString(st.module_dict, "plugin");
    if (!plugin || !PyDict_Check(plugin))
        return;
    PyObject* name = PyDict_GetItemString(plugin, "name");
    if (name && PyUnicode_Check(name))
        st.manifest.name = PyUnicode_AsUTF8(name);
    PyObject* version = PyDict_GetItemString(plugin, "version");
    if (version && PyUnicode_Check(version))
        st.manifest.version = PyUnicode_AsUTF8(version);
    PyObject* provides = PyDict_GetItemString(plugin, "provides");
    if (provides && PyUnicode_Check(provides))
        st.manifest.provides = PyUnicode_AsUTF8(provides);
    PyObject* deps = PyDict_GetItemString(plugin, "deps");
    if (deps && PyList_Check(deps)) {
        for (Py_ssize_t i = 0; i < PyList_Size(deps); ++i) {
            PyObject* d = PyList_GetItem(deps, i);
            if (d && PyUnicode_Check(d))
                st.manifest.deps.push_back(PyUnicode_AsUTF8(d));
        }
    }
}

/// 执行单个 job 的 Python 逻辑（在 worker 线程调用，内部持 GIL）。
void run_job(ScriptState& st, ScriptJob& job) {
    PyGILState_STATE gil = PyGILState_Ensure();
    g_current_script = &st;

    switch (job.op) {
        case ScriptOp::Init: {
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_init");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(f, "s", "<manager>");
                if (!r)
                    PyErr_Print();
                Py_XDECREF(r);
            }
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
            std::string out;
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_call");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(
                    f, "is", static_cast<int>(job.env.sub_proto),
                    payload->c_str());
                if (r) {
                    if (PyUnicode_Check(r))
                        out = PyUnicode_AsUTF8(r);
                    Py_DECREF(r);
                } else {
                    PyErr_Print();
                }
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::String: {
            std::string out;
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_string");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(f, "s", job.str.c_str());
                if (r) {
                    if (PyUnicode_Check(r))
                        out = PyUnicode_AsUTF8(r);
                    Py_DECREF(r);
                } else {
                    PyErr_Print();
                }
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Save: {
            std::string out;
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_save");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(f, nullptr);
                if (r) {
                    if (PyUnicode_Check(r))
                        out = PyUnicode_AsUTF8(r);
                    Py_DECREF(r);
                } else {
                    PyErr_Print();
                }
            }
            if (job.done_str) job.done_str(out);
            break;
        }
        case ScriptOp::Restore: {
            std::string data(
                reinterpret_cast<const char*>(job.bytes.data()),
                job.bytes.size());
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_restore");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(f, "s", data.c_str());
                if (!r)
                    PyErr_Print();
                Py_XDECREF(r);
            }
            if (job.done_void) job.done_void();
            break;
        }
        case ScriptOp::Shutdown: {
            PyObject* f = PyDict_GetItemString(st.module_dict, "on_shutdown");
            if (f && PyCallable_Check(f)) {
                PyObject* r = PyObject_CallFunction(f, nullptr);
                if (!r)
                    PyErr_Print();
                Py_XDECREF(r);
            }
            if (job.done_void) job.done_void();
            break;
        }
    }

    g_current_script = nullptr;
    PyGILState_Release(gil);
}

/// worker 主循环：独占脚本模块，串行执行队列。
void py_worker_main(std::shared_ptr<ScriptState> st) {
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
    // 退出前回收脚本命名空间（持 GIL DECREF）
    PyGILState_STATE gil = PyGILState_Ensure();
    if (st->module_dict) {
        Py_DECREF(st->module_dict);
        st->module_dict = nullptr;
    }
    PyGILState_Release(gil);
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

/// 从 .py 文件构建 ScriptState（编译进独立命名空间 + 起 worker）。失败返回 null。
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

    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject* globals = PyDict_New();
    inject_bridge(globals);
    PyObject* code = Py_CompileString(src.c_str(), path.string().c_str(),
                                      Py_file_input);
    bool ok = true;
    if (!code) {
        PyErr_Print();
        ok = false;
    } else {
        PyObject* result = PyEval_EvalCode(code, globals, globals);
        Py_DECREF(code);
        if (!result) {
            PyErr_Print();
            ok = false;
        } else {
            Py_DECREF(result);
        }
    }
    if (!ok) {
        Py_DECREF(globals);
        PyGILState_Release(gil);
        return nullptr;
    }

    st->module_dict = globals;   // 转移所有权（worker 退出时 DECREF）
    read_manifest(*st);
    if (st->manifest.provides.empty())
        st->manifest.provides = path.stem().string();
    st->self_name = st->manifest.provides;
    PyGILState_Release(gil);

    st->queue = std::make_shared<JobQueue>();
    st->worker = std::make_shared<std::thread>(py_worker_main, st);
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

/// 进程级 Python 初始化状态（spawn 时 Py_Initialize 一次）。
std::atomic<bool>& python_ready() {
    static std::atomic<bool> ready{false};
    return ready;
}

/// PyEval_SaveThread 返回的主线程状态——finalize 时必须恢复（同线程）才能
/// Py_FinalizeEx；跨线程 finalize 必崩（0xC0000005，CPython 硬约束）。
PyThreadState*& py_main_thread_state() {
    static PyThreadState* ts = nullptr;
    return ts;
}

/// 专用解释器线程：Py_Initialize/Py_FinalizeEx 都在本线程执行。
/// CAF 线程池不保证 spawn 与 destroy_plugin 同线程（actor 在 worker 间
/// 迁移）——pystate.c:1770 断言证明跨线程 RestoreThread 必崩。专用线程
/// 保证 init/finalize 同线程，脚本 worker 照旧用 PyGILState_Ensure。
struct PyHostThread {
    std::thread th;
    std::mutex mtx;
    std::condition_variable cv;
    bool init_done = false;
    bool stop = false;
};
PyHostThread& py_host_thread() {
    static PyHostThread t;
    return t;
}

/// 触发解释器 finalize（专用线程执行）。幂等：python_ready 原子交换保证
/// 只跑一次。shutdown hook（脚本实例已 clear、worker 全退）与
/// destroy_plugin（热卸载兜底）都调用；关机路径 exit_msg 兜底可能先于
/// shutdown hook 到达 destroy_plugin——但 python_ready 交换后另一路跳过。
static void py_finalize_interpreter() {
    if (!python_ready().exchange(false))
        return;
    auto& h = py_host_thread();
    {
        std::lock_guard<std::mutex> lk(h.mtx);
        h.stop = true;
    }
    h.cv.notify_all();
    if (h.th.joinable())
        h.th.join();
    std::cout << "[PythonHostPlugin] Py_FinalizeEx done (dedicated thread)"
              << std::endl;
}

} // namespace

// ------------------------------------------------------------------
// 宿主插件本体
// ------------------------------------------------------------------
class PythonHostPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"PythonHostPlugin", "1.0.0",
                 {},
                {"py_host_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // Python 解释器只初始化一次，随后释放 GIL（worker 用 PyGILState）。
        // init/finalize 都在专用解释器线程：CAF 线程池不保证插件 actor 的
        // handler 同线程（worker 间迁移），跨线程 Py_FinalizeEx 必崩
        // （pystate.c:1770 断言 / 0xC0000005）。见 PyHostThread 注释。
        if (!python_ready().exchange(true)) {
            auto& h = py_host_thread();
            h.th = std::thread([&h]() {
                // PYTHONMALLOC=malloc：禁用 pymalloc arena（CPython 的
                // arena 在 Py_Finalize 后不归还 CRT 堆，Debug CRT 泄漏检测
                // 会把它报成固定 262168/131096 字节的"泄漏"）。走系统
                // malloc 后基线固定泄漏 6 块（248/120/38/90/82/82 B，
                // 8-31 起各轮实测一致）。分配栈 hook 实锤：6 块全部由
                // python312_d.dll 内部（PyMem→type-alloc 路径）分配，
                // py_host 仅触发者——最小实验（纯 init / 线程结构复刻 /
                // 桥接注入+完整脚本链）均 0 块，仅在 py_host 的 CAF 环境
                // （多线程 actor 系统 + 完整关机链）下出现；Py_FinalizeEx
                // 后解释器驻留，进程退出由 OS 统一回收，非真泄漏
                // （EXIT 恒 0、与 DLL 卸载无关）。
                _putenv("PYTHONMALLOC=malloc");
                Py_Initialize();
                py_main_thread_state() = PyEval_SaveThread();
                {
                    std::lock_guard<std::mutex> lk(h.mtx);
                    h.init_done = true;
                }
                h.cv.notify_all();
                // 空闲等待 finalize 指令（无 GIL）
                std::unique_lock<std::mutex> lk(h.mtx);
                h.cv.wait(lk, [&h]() { return h.stop; });
                // 同线程（本线程）恢复主线程状态 + finalize
                if (auto* ts = py_main_thread_state())
                    PyEval_RestoreThread(ts);
                Py_FinalizeEx();
            });
            // 等解释器初始化完成（脚本 worker 的 PyGILState_Ensure 依赖）
            std::unique_lock<std::mutex> lk(h.mtx);
            h.cv.wait(lk, [&h]() { return h.init_done; });
        }

        auto cfg = load_plugin_config(sys.config());
        std::string scripts_dir = cfg.scripts_dir.empty()
                                      ? this->asset_path("scripts")
                                      : cfg.scripts_dir;

        return sys.spawn([logger, scripts_dir](caf::event_based_actor* self)
                             -> caf::behavior {
            caf::actor registry = caf::actor_cast<caf::actor>(
                self->system().registry().get("service_registry"));

            // 脚本实例台账：服务名 → (子 actor, 脚本路径)
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
                           "PythonHostPlugin");
                (*instances)[st->self_name] = {child, path};
                self->send(child, init_atom{}, caf::actor{self}, std::string{});
                LOG_INFO_SELF(self, "script loaded: {} (service={}, deps={})",
                              std::filesystem::path{path}.filename().string(),
                              st->self_name, st->manifest.deps.size());
            };

            // 完整热更（镜像框架 §8 先排空后快照）
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
                    if (env.sub_proto != py_reload_sub_proto)
                        return;
                    if (auto service_name = plugin_wire::decode_text(env))
                        reload_script(self, *service_name);
                    else
                        LOG_WARN_SELF(self, "ignoring reload with unsupported payload format");
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "PythonHostPlugin initializing, scripts dir: {}",
                                  scripts_dir);
                    if (!std::filesystem::exists(scripts_dir)) {
                        LOG_WARN_SELF(self, "scripts dir not found: {}", scripts_dir);
                        return;
                    }
                    for (const auto& entry :
                         std::filesystem::directory_iterator(scripts_dir)) {
                        if (!entry.is_regular_file())
                            continue;
                        if (entry.path().extension().string() != ".py")
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
                    // 脚本实例已全部销毁（worker 线程 join 完毕）——现在
                    // finalize 解释器。必须在专用线程执行（与 Py_Initialize
                    // 同线程）；此处只发指令并 join，finalize 本身在专用
                    // 线程完成。destroy_plugin 的 exit_msg 兜底可能先到，
                    // python_ready() 交换保证只 finalize 一次。
                    py_finalize_interpreter();
                    std::cout << "[PythonHostPlugin] shutdown hook: " << n
                              << " script(s) torn down" << std::endl;
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new PythonHostPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    // finalize 只由 shutdown hook 触发（脚本实例 clear、worker 全退之后）。
    // 本函数在关机路径可能先于 shutdown hook 执行（plugin_mgr 的 exit_msg
    // 兜底与插件 actor 退出竞态）——此处触发 finalize 会让仍在运行的脚本
    // worker 在 finalize 后调用 Python API → abort（pystate.c 断言 332/2214）。
    // 热卸载路径（down_msg destroy）时 shutdown hook 已跑完（actor 退出
    // 先于 down_msg），python_ready 已 false，无需兜底。
    delete p;
}
