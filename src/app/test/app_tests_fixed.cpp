// ------------------------------------------------------------------
// 应用级验证后门实现（见 app_tests.hpp）
// ------------------------------------------------------------------

#include "app_tests.hpp"

#include "cluster/remote_caller.hpp"
#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "services/time_service.hpp"

#include <caf/all.hpp>

#include <chrono>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace caf_plugin_system {

namespace {

/// business 插件信封方法名：hello（跨节点验证载荷，见 business_plugin.cpp）。
inline constexpr const char* k_env_hello = "hello";

} // namespace

// ------------------------------------------------------------------
// 冒烟测试（--test-auto-shutdown）：验证 ACL 拦截、缓冲冲刷、
// 热更新、新代码生效。
// ------------------------------------------------------------------

void run_smoke_tests(caf::actor_system& sys, const BootstrapResult& fw) {
    caf::scoped_actor self{sys};

    // ACL 自测：以未受信身份（main 的 scoped_actor 不在白名单）调用
    // business_service，应被服务代理拦截。
    caf::actor biz_proxy;
    self->request(fw.registry, caf::infinite, resolve_atom{}, "business_service")
        .receive([&biz_proxy](const caf::actor& a) { biz_proxy = a; },
                 [](caf::error&) {});
    if (biz_proxy) {
        self->request(biz_proxy, std::chrono::seconds(2), std::string("shutdown"))
            .receive(
                [](const std::string&) {
                    std::cout << "[ACL test] FAILED: untrusted call went through!"
                              << std::endl;
                },
                [](const caf::error& e) {
                    std::cout << "[ACL test] blocked as expected: "
                              << caf::to_string(e) << std::endl;
                });
    }

    // 缓冲路径的确定性验证：先手动 quiesce 代理，再挂起一个 request——
    // 它进入代理缓冲，等 reload 内部 resume 冲刷时才被处理（main 不在
    // ACL 白名单，冲刷复查时被拦）。响应在 reload 完成后才到达，即为
    // "曾进入缓冲"的时序证据。
    using buffered_req_t = decltype(self->request(
        biz_proxy, std::chrono::seconds(5), std::string("")));
    std::optional<buffered_req_t> pending;
    if (biz_proxy) {
        self->request(biz_proxy, std::chrono::seconds(2), quiesce_atom{})
            .receive([](bool) {}, [](caf::error&) {});
        pending.emplace(self->request(biz_proxy, std::chrono::seconds(5),
                                      std::string("buffered-hello")));
    }

    // 热更新自测：从【旁路新路径】加载 BusinessPlugin v2（同一份源码加
    // BIZ_HOT_V2 编出，绕过 Windows 文件锁与 LoadLibrary 路径缓存）。
    self->request(fw.plugin_mgr, caf::infinite, reload_atom{},
                  std::string("BusinessPlugin"),
#ifdef _WIN32
                  std::string("./updates/business_plugin_v2.dll"))
#else
                  std::string("./updates/libbusiness_plugin_v2.so"))
#endif
        .receive([](bool ok) {
                     std::cout << "[HotUpdate] reload result: " << ok << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[HotUpdate] reload error: "
                               << caf::to_string(e) << std::endl;
                 });

    // 收缓冲验证的响应：只有代理 resume 冲刷后才会到达
    if (pending) {
        pending->receive(
            [](const std::string& r) {
                std::cout << "[HotUpdate] buffered response: " << r << std::endl;
            },
            [](const caf::error& e) {
                std::cout << "[HotUpdate] buffered call resolved post-reload: "
                          << caf::to_string(e) << std::endl;
            });
    }

    // 验证新代码：直连插件 actor（不经服务代理，不受 ACL 约束）
    caf::actor biz;
    self->request(fw.plugin_mgr, caf::infinite, resolve_plugin_atom{}, "BusinessPlugin")
        .receive([&biz](const caf::actor& a) { biz = a; }, [](caf::error&) {});
    if (biz) {
        self->request(biz, std::chrono::seconds(2), std::string("hello"))
            .receive([](const std::string& r) {
                         std::cout << "[HotUpdate] response: " << r << std::endl;
                     },
                     [](const caf::error& e) {
                         std::cout << "[HotUpdate] call error: "
                                   << caf::to_string(e) << std::endl;
                     });
        // v2 热更新新增的私有子协议号：走公共信封，无需新 type_id
        plugin_envelope env;
        env.function = "v2_ping";
        self->send(biz, env);
    }
}

// ------------------------------------------------------------------
// 脚本插件验证（--test-lua-script）：resolve echo_service，发 envelope
// 与 string，校验 on_call / on_string 回执——验证 lua_host 桥接层。
// ------------------------------------------------------------------

void run_lua_script_test(caf::actor_system& sys, const BootstrapResult& fw) {
    caf::scoped_actor self{sys};

    caf::actor echo_proxy;
    // 脚本服务由宿主 on_init 异步注册，晚于 bootstrap_plugins 返回——
    // 重试 resolve 直到 echo_service 就绪（镜像 run_cross_call_test 的重试）
    for (int i = 0; i < 20 && !echo_proxy; ++i) {
        self->request(fw.registry, caf::infinite, resolve_atom{}, "echo_service")
            .receive([&](const caf::actor& a) { echo_proxy = a; },
                     [](caf::error&) {});
        if (!echo_proxy)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!echo_proxy) {
        std::cout << "[LuaTest] echo_service not registered "
                     "(LuaHostPlugin not loaded?)" << std::endl;
        return;
    }

    // 信封调用：function="echo" → 脚本 on_call(fn, payload)
    plugin_envelope env;
    env.function = "echo";
    const char* text = "ping";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                      reinterpret_cast<const std::byte*>(text)
                          + std::strlen(text));
    self->request(echo_proxy, std::chrono::seconds(5), env)
        .receive([](const std::string& r) {
                     std::cout << "[LuaTest] envelope call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[LuaTest] envelope call error: "
                               << caf::to_string(e) << std::endl;
                 });

    // 字符串命令调用：脚本 on_string(cmd)
    self->request(echo_proxy, std::chrono::seconds(5), std::string("hello"))
        .receive([](const std::string& r) {
                     std::cout << "[LuaTest] string call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[LuaTest] string call error: "
                               << caf::to_string(e) << std::endl;
                 });

    // 热更验证：发管理信封（function="reload"）给 lua_host_service 触发 echo.lua
    // 重载，再调 echo_service——若状态交接正确，counter 应从 2 续到 3
    //（"echo:3:ping"）；若状态丢失则重置回 1（"echo:1:ping"）。echo_proxy 经
    // resume 切到新实例，同一句柄继续可用（quiesce 期调用会进缓冲后冲刷）。
    caf::actor host_proxy;
    self->request(fw.registry, caf::infinite, resolve_atom{}, "lua_host_service")
        .receive([&](const caf::actor& a) { host_proxy = a; },
                 [](caf::error&) {});
    if (host_proxy) {
        plugin_envelope reload_env;
        reload_env.function = "reload";
        const char* svc = "echo_service";
        reload_env.payload.assign(reinterpret_cast<const std::byte*>(svc),
                                 reinterpret_cast<const std::byte*>(svc)
                                     + std::strlen(svc));
        std::cout << "[LuaTest] triggering reload of echo_service" << std::endl;
        self->send(host_proxy, reload_env);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        self->request(echo_proxy, std::chrono::seconds(5), env)
            .receive([](const std::string& r) {
                         std::cout << "[LuaTest] post-reload envelope call -> "
                                   << r << std::endl;
                     },
                     [](const caf::error& e) {
                         std::cout << "[LuaTest] post-reload call error: "
                                   << caf::to_string(e) << std::endl;
                     });
    }
}

// ------------------------------------------------------------------
// 脚本插件验证（--test-py-script）：镜像 run_lua_script_test，验证
// py_host 桥接层（on_call/on_string）+ 热更状态交接。
// ------------------------------------------------------------------

void run_py_script_test(caf::actor_system& sys, const BootstrapResult& fw) {
    caf::scoped_actor self{sys};

    caf::actor echo_proxy;
    for (int i = 0; i < 20 && !echo_proxy; ++i) {
        self->request(fw.registry, caf::infinite, resolve_atom{}, "echo_service")
            .receive([&](const caf::actor& a) { echo_proxy = a; },
                     [](caf::error&) {});
        if (!echo_proxy)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!echo_proxy) {
        std::cout << "[PyTest] echo_service not registered "
                     "(PythonHostPlugin not loaded?)" << std::endl;
        return;
    }

    plugin_envelope env;
    env.function = "echo";
    const char* text = "ping";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                      reinterpret_cast<const std::byte*>(text)
                          + std::strlen(text));
    self->request(echo_proxy, std::chrono::seconds(5), env)
        .receive([](const std::string& r) {
                     std::cout << "[PyTest] envelope call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[PyTest] envelope call error: "
                               << caf::to_string(e) << std::endl;
                 });

    self->request(echo_proxy, std::chrono::seconds(5), std::string("hello"))
        .receive([](const std::string& r) {
                     std::cout << "[PyTest] string call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[PyTest] string call error: "
                               << caf::to_string(e) << std::endl;
                 });

    // 热更：发管理信封（function="reload"）给 py_host_service 触发 echo.py 重载，
    // 再调 echo_service——状态交接正确则 counter 从 2 续到 3（"echo:3:ping"）。
    caf::actor host_proxy;
    self->request(fw.registry, caf::infinite, resolve_atom{}, "py_host_service")
        .receive([&](const caf::actor& a) { host_proxy = a; },
                 [](caf::error&) {});
    if (host_proxy) {
        plugin_envelope reload_env;
        reload_env.function = "reload";
        const char* svc = "echo_service";
        reload_env.payload.assign(reinterpret_cast<const std::byte*>(svc),
                                 reinterpret_cast<const std::byte*>(svc)
                                     + std::strlen(svc));
        std::cout << "[PyTest] triggering reload of echo_service" << std::endl;
        self->send(host_proxy, reload_env);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        self->request(echo_proxy, std::chrono::seconds(5), env)
            .receive([](const std::string& r) {
                         std::cout << "[PyTest] post-reload envelope call -> "
                                   << r << std::endl;
                     },
                     [](const caf::error& e) {
                         std::cout << "[PyTest] post-reload call error: "
                                   << caf::to_string(e) << std::endl;
                     });
    }
}

// ------------------------------------------------------------------
// 脚本插件验证（--test-ts-script）：镜像 run_py_script_test，验证
// ts_host 桥接层（on_call/on_string）+ 热更状态交接。
// ------------------------------------------------------------------

void run_ts_script_test(caf::actor_system& sys, const BootstrapResult& fw) {
    caf::scoped_actor self{sys};

    caf::actor echo_proxy;
    for (int i = 0; i < 20 && !echo_proxy; ++i) {
        self->request(fw.registry, caf::infinite, resolve_atom{}, "echo_service")
            .receive([&](const caf::actor& a) { echo_proxy = a; },
                     [](caf::error&) {});
        if (!echo_proxy)
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (!echo_proxy) {
        std::cout << "[TsTest] echo_service not registered "
                     "(TsHostPlugin not loaded?)" << std::endl;
        return;
    }

    plugin_envelope env;
    env.function = "echo";
    const char* text = "ping";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                      reinterpret_cast<const std::byte*>(text)
                          + std::strlen(text));
    self->request(echo_proxy, std::chrono::seconds(5), env)
        .receive([](const std::string& r) {
                     std::cout << "[TsTest] envelope call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[TsTest] envelope call error: "
                               << caf::to_string(e) << std::endl;
                 });

    self->request(echo_proxy, std::chrono::seconds(5), std::string("hello"))
        .receive([](const std::string& r) {
                     std::cout << "[TsTest] string call -> " << r << std::endl;
                 },
                 [](const caf::error& e) {
                     std::cout << "[TsTest] string call error: "
                               << caf::to_string(e) << std::endl;
                 });

    caf::actor host_proxy;
    self->request(fw.registry, caf::infinite, resolve_atom{}, "ts_host_service")
        .receive([&](const caf::actor& a) { host_proxy = a; },
                 [](caf::error&) {});
    if (host_proxy) {
        plugin_envelope reload_env;
        reload_env.function = "reload";
        const char* svc = "echo_service";
        reload_env.payload.assign(reinterpret_cast<const std::byte*>(svc),
                                 reinterpret_cast<const std::byte*>(svc)
                                     + std::strlen(svc));
        std::cout << "[TsTest] triggering reload of echo_service" << std::endl;
        self->send(host_proxy, reload_env);
        std::this_thread::sleep_for(std::chrono::milliseconds(800));

        self->request(echo_proxy, std::chrono::seconds(5), env)
            .receive([](const std::string& r) {
                         std::cout << "[TsTest] post-reload envelope call -> "
                                   << r << std::endl;
                     },
                     [](const caf::error& e) {
                         std::cout << "[TsTest] post-reload call error: "
                                   << caf::to_string(e) << std::endl;
                     });
    }
}

// ------------------------------------------------------------------
// 跨节点调用验证（--test-cross-call=<服务名>，master 进程执行）。
// RemoteCaller actor 缓存句柄 + 失败自动重试（模式 B）；循环调用
// 观察杀/重启目标节点时 "失败 → 自动恢复"（缓存失效 → 重新 resolve）。
// ------------------------------------------------------------------

void run_cross_call_test(caf::actor_system& sys, caf::actor master,
                         const std::string& local_node_name,
                         const std::string& actor_name) {
    auto caller = cluster::spawn_remote_caller(sys, master, local_node_name);
    plugin_envelope env;
    env.function = k_env_hello;
    const char* text = "cross";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                       reinterpret_cast<const std::byte*>(text)
                           + std::strlen(text));
    caf::scoped_actor self{sys};
    auto do_call = [&] {
        caf::expected<std::string> r = caf::make_error(
            caf::sec::runtime_error, "no response");
        self->request(caller, std::chrono::seconds(15), cross_call_atom_v,
                      actor_name, env)
            .receive([&](std::string& s) { r = std::move(s); },
                     [&](caf::error& err) { r = std::move(err); });
        std::cout << "[CrossCall] "
                  << (r ? ("OK: " + *r)
                        : ("fail: " + caf::to_string(r.error())))
                  << std::endl;
        return static_cast<bool>(r);
    };
    bool ok = false;
    for (int i = 0; i < 5 && !ok; ++i) {
        ok = do_call();
        if (!ok)
            std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    if (!ok) {
        std::cout << "[CrossCall] timeout: service '" << actor_name
                  << "' never reachable" << std::endl;
        return;
    }
    for (int i = 0; i < 11; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        do_call();
    }
    // 清理 RemoteCaller：不 send_exit 它，actor_system 析构会 join 这个
    // 常驻 actor → 进程挂起（关机链 STOPPED 后不退出，实测踩过）。
    self->send_exit(caller, caf::exit_reason::user_shutdown);
}

// ------------------------------------------------------------------
// 跨节点调用验证（--test-cross-call-ex=<服务名>，有界重试路径）。
// RemoteCaller 的 cross_call_ex：attempts 次尝试 + 1s 间隔；
// 配合"先启 master 后启 worker"可验证重启窗口期的调用不丢失——
// 目标节点未就绪时重试等待，节点上线后自动成功。
// ------------------------------------------------------------------

void run_cross_call_ex_test(caf::actor_system& sys, caf::actor master,
                            const std::string& local_node_name,
                            const std::string& actor_name) {
    auto caller = cluster::spawn_remote_caller(sys, master, local_node_name);
    plugin_envelope env;
    env.function = k_env_hello;
    const char* text = "cross";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                       reinterpret_cast<const std::byte*>(text)
                           + std::strlen(text));
    constexpr int k_attempts = 15;
    caf::scoped_actor self{sys};
    std::cout << "[CrossCallEx] request with attempts=" << k_attempts
              << " (interval 1s)" << std::endl;
    self->request(caller, std::chrono::seconds(60), cross_call_ex_atom_v,
                  actor_name, k_attempts, env)
        .receive([&](std::string& s) {
                     std::cout << "[CrossCallEx] OK: " << s << std::endl;
                 },
                 [&](caf::error& err) {
                     std::cout << "[CrossCallEx] fail: "
                               << caf::to_string(err) << std::endl;
                 });
    // 清理 RemoteCaller（同 run_cross_call_test：不杀则析构挂起）
    self->send_exit(caller, caf::exit_reason::user_shutdown);
}

// ------------------------------------------------------------------
// bridge 验证（--test-bridge-call=<节点名>，master 进程执行）。
// 跨节点调用指定节点的 external_echo——handler 在外部进程（Python/Go），
// bridge 转发 REQ/RESULT。验证 集群→bridge→外部进程 完整链路。
// 循环重试 60×1s：bridge 节点可能比 master 晚启动/晚注册。
// ------------------------------------------------------------------

void run_bridge_call_test(caf::actor_system& sys, caf::actor master,
                          const std::string& local_node_name,
                          const std::string& node_name,
                          caf::actor shutdown_mgr) {
    auto caller = cluster::spawn_remote_caller(sys, master, local_node_name);
    // 注册给 shutdown_mgr：重试循环（最长 60×4s）若撞上 EOF 关机，
    // 关机链统一 send_exit → request 立即失败 → 循环快速退出。
    // 否则 caller 残活 → actor_system 析构 join 挂起（实测）。
    if (shutdown_mgr)
        caf::anon_send(shutdown_mgr, register_cluster_atom_v, caller);
    plugin_envelope env;
    env.function = k_env_hello;
    const char* text = "bridge-ping";
    env.payload.assign(reinterpret_cast<const std::byte*>(text),
                       reinterpret_cast<const std::byte*>(text)
                           + std::strlen(text));
    caf::scoped_actor self{sys};
    std::cout << "[BridgeTest] cross-node call external_echo@'" << node_name
              << "' (retry 60x1s)" << std::endl;
    bool ok = false;
    for (int i = 0; i < 60 && !ok; ++i) {
        self->request(caller, std::chrono::seconds(3), cross_call_atom_v,
                      std::string("external_echo"), node_name, env)
            .receive([&](std::string& s) {
                         std::cout << "[BridgeTest] external_echo@'"
                                   << node_name << "' -> " << s << std::endl;
                         ok = true;
                     },
                     [&](caf::error& err) {
                         std::cout << "[BridgeTest] attempt " << (i + 1)
                                   << ": " << caf::to_string(err) << std::endl;
                     });
        if (!ok)
            std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (!ok) {
        std::cout << "[BridgeTest] timeout: external_echo@'" << node_name
                  << "' never reachable" << std::endl;
    }
    // 清理 RemoteCaller（同 run_cross_call_test：不杀则析构挂起）
    self->send_exit(caller, caf::exit_reason::user_shutdown);
}

// ------------------------------------------------------------------
// 统一时间源验证（--test-time-offset）：校验全局偏移注入生效。
// business_now() - 真实 now 必须 == 配置的 time-offset；同时打印
// 业务时间串与真实时间串（日志时间戳 = 业务时间，同源）。
// ------------------------------------------------------------------

void run_time_offset_test() {
    const auto real = std::chrono::system_clock::now();
    const auto biz  = business_now();
    const auto diff = std::chrono::duration_cast<std::chrono::seconds>(biz - real);
    const auto expect = time_offset();
    const bool pass = diff == expect;

    const auto rt = std::chrono::system_clock::to_time_t(real);
    std::tm rtm{};
#ifdef _WIN32
    localtime_s(&rtm, &rt);
#else
    localtime_r(&rt, &rtm);
#endif
    char rbuf[32];
    std::strftime(rbuf, sizeof rbuf, "%Y-%m-%d %H:%M:%S", &rtm);

    std::cout << "[TimeTest] business_now() - now = " << diff.count()
              << "s | configured offset = " << expect.count()
              << "s -> " << (pass ? "PASS" : "FAIL") << std::endl;
    std::cout << "[TimeTest] business now = " << format_business_now()
              << " | real now = " << rbuf << std::endl;
}

} // namespace caf_plugin_system
