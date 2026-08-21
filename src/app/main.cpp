#include <caf/all.hpp>
#include <caf/caf_main.hpp>
#include <caf/logger.hpp>

#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "framework_bootstrap.hpp"
#include "cluster/bootstrap.hpp"
#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"

using namespace caf_plugin_system;

// ------------------------------------------------------------------
// 进程配置 = 插件框架（caf_plugin_core） + 集群节点（caf_plugin_cluster）
// 两个模块正交，可按需组合：
//   - 纯插件进程：默认加载 caf-application.conf（CAF 默认文件名，无需参数）
//   - 纯节点进程：--caf-plugin-system.node-kind=master|region|worker
//   - 节点 + 插件：两者都配（region 上跑服务插件）
// ------------------------------------------------------------------

struct app_config : framework_config {
    cluster::node_settings node_cfg;
    app_config() : framework_config() {
        // middleman 元对象注册 + 加载（必须在 actor_system 构造前）
        cluster::init_node_io(*this);
        cluster::add_node_options(*this, node_cfg);
    }
};

// ------------------------------------------------------------------
// 冒烟测试后门（--test-auto-shutdown）：验证 ACL 拦截、缓冲冲刷、
// 热更新、新代码生效。属于 app 特有的验证逻辑，不进框架。
// ------------------------------------------------------------------

static void run_smoke_tests(caf::actor_system& sys, const BootstrapResult& fw) {
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
// 每个进程的入口：节点引导（可选）→ 插件引导（可选）→ 等待关机。
// 两个模块正交：node-kind 决定集群角色，entry-plugins 决定插件加载。
// ------------------------------------------------------------------

void caf_main(caf::actor_system& sys, const app_config& cfg) {
    // ---- 插件框架引导（可选；--caf-plugin-system.entry-plugins 非空时）----
    // 先于节点引导：混合模式下 shutdown_mgr 作为节点 monitor 上报给 master
    // （优雅关机/进程退出时 master 立即感知，无需等 lease 过期）。
    BootstrapResult fw;
    if (!cfg.entry_plugins.empty()) {
        if (!bootstrap_plugin_framework(sys, cfg, fw)) return;
        if (cfg.test_auto_shutdown) run_smoke_tests(sys, fw);
    }

    // ---- 集群节点引导（可选；--caf-plugin-system.node-kind 非空时）----
    // 单节点（纯插件）与集群节点（含插件）都经此路径，正交组合。
    cluster::BootstrapResult nb;
    if (cfg.node_cfg.is_node()) {
        // 混合模式传 shutdown_mgr 作 monitor；纯节点模式传空 →
        // bootstrap_node 内部 spawn 进程哨兵 actor 兜底。
        caf::actor monitor = fw.shutdown_mgr;
        if (!cluster::bootstrap_node(sys, cfg.node_cfg, monitor, nb)) return;
    }

    // ---- 等待关机：插件模式等 shutdown_mgr；纯节点模式等节点 actor ----
    if (!cfg.entry_plugins.empty()) {
        wait_for_shutdown(sys, fw);
    } else if (cfg.node_cfg.is_node()) {
        cluster::wait_for_node_shutdown(sys, nb);
    }
}

CAF_MAIN()
