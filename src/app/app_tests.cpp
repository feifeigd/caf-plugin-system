// ------------------------------------------------------------------
// 应用级验证后门实现（见 app_tests.hpp）
// ------------------------------------------------------------------

#include "app_tests.hpp"

#include "cluster/remote_caller.hpp"
#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"

#include <caf/all.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

namespace caf_plugin_system {

namespace {

/// business 插件信封子协议：hello（跨节点验证载荷，见 business_plugin.cpp）。
constexpr std::uint16_t k_env_hello = 1;

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
                  std::string("./updates/business_plugin_v2.dll"))
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
        env.sub_proto = 2;
        self->send(biz, env);
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
    env.sub_proto = k_env_hello;
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
    env.sub_proto = k_env_hello;
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
                          const std::string& node_name) {
    auto caller = cluster::spawn_remote_caller(sys, master, local_node_name);
    plugin_envelope env;
    env.sub_proto = k_env_hello;
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

} // namespace caf_plugin_system
