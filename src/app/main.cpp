#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <caf/caf_main.hpp>
#include <caf/io/middleman.hpp>
#include <caf/logger.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "framework_bootstrap.hpp"
#include "services/logging_service.hpp"
#include "app_tests.hpp"
#include "cluster/bootstrap.hpp"
#include "cluster/ops_actor.hpp"
#include "common/message_tags.hpp"
#include "plugin/plugin_loader.hpp"  // unload_all_meta_libs
#include "plugin/plugin_manager.hpp" // unload_all_plugin_libs

#ifdef _WIN32
#include <windows.h> // ExitProcess（DLL 池卸载后跳过静态析构阶段）
#endif

#ifdef _DEBUG
// Debug CRT 泄露检测：进程退出时（actor_system 析构之后）自动 dump 堆泄露
#include <crtdbg.h>
#endif

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
// 验证后门（--test-auto-shutdown / --test-cross-call*）实现见 app_tests.cpp
// ------------------------------------------------------------------

// ------------------------------------------------------------------
// 每个进程的入口：节点引导（可选）→ 插件引导（可选）→ 等待关机。
// 两个模块正交：node-kind 决定集群角色，entry-plugins 决定插件加载。
// ------------------------------------------------------------------

int caf_main(caf::actor_system& sys, const app_config& cfg) {
    #ifdef _DEBUG
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);  // 在控制台打印内存泄漏
    #endif

    // 构建标识：双击测试时一眼确认 exe 新旧（旧实例窗口不会随代码更新）
    std::cout << "[App] caf_plugin_app build " << __DATE__ << " " << __TIME__
              << std::endl;
    // ---- 系统级组件引导（任何进程都执行）----
    // registry/checkpoint_mgr/plugin_mgr/shutdown_mgr 是系统组件，必然存在；
    // 节点模式同样依赖它们（exported_actors 收集、shutdown_mgr 作 monitor）。
    BootstrapResult fw;
    if (!bootstrap_system_components(sys, cfg, fw)) return 0;

    // ---- 插件加载（可选；entry-plugins 非空时）----
    // 先于节点引导：混合模式下 shutdown_mgr 作为节点 monitor 上报给 master
    // （优雅关机/进程退出时 master 立即感知，无需等 lease 过期）。
    if (!cfg.entry_plugins.empty()) {
        if (!bootstrap_plugins(sys, cfg, fw)) return 0;
        if (cfg.test_auto_shutdown) {
            run_smoke_tests(sys, fw);
            // 冒烟测试跑完自动关机（选项名即语义）：走 shutdown_mgr 统一
            // 关机链（插件保存 → 集群 → 组件 → logging_service 最后退出）。
            // 注意：重构把关机统一到 shutdown_mgr 后，此触发必须显式发
            // shutdown_atom——曾漏接导致冒烟测试跑完进程挂死在等输入。
            std::cout << "[OpsTest] auto shutdown after smoke tests (delayed 2s)"
                      << std::endl;
            caf::scoped_actor self{sys};
            self->delayed_send(fw.shutdown_mgr, std::chrono::seconds(2),
                               shutdown_atom{});
        }
    }

    // 注：日志无注入环节——logging_service 是系统组件，bootstrap_system_components
    // 最先 spawn 时即成为唯一日志入口（log_info/log_error，见 logging_service.hpp）。

    // ---- 集群节点引导（可选；--caf-plugin-system.node-kind 非空时）----
    // 单节点（纯插件）与集群节点（含插件）都经此路径，正交组合。
    cluster::BootstrapResult nb;
    if (cfg.node_cfg.is_node()) {
        // shutdown_mgr 作 monitor（系统组件，所有进程都有）；master 监控它
        // 感知本进程退出（优雅关机时立即 down，无需等 lease 过期）。
        caf::actor monitor = fw.shutdown_mgr;
        // 本节点导出的服务名：从 ServiceRegistry 台账自动收集（registry 是
        // 系统级组件必然存在，无需判空），节点注册时随 manifest 上报 master
        //（node_resolve 路由依据）。
        auto node_cfg = cfg.node_cfg;
        caf::scoped_actor self{sys};
        self->request(fw.registry, caf::infinite, exported_actors_atom_v)
            .receive([&](std::vector<std::string>& names) {
                node_cfg.exported_actors = std::move(names);
                for (auto& n : names)
                    std::cout << "[Node] exporting service: " << n
                              << std::endl;
            },
            [&](caf::error& err) {
                std::cout << "[Node] exported_actors query failed: "
                          << caf::to_string(err) << std::endl;
            });
        if (!cluster::bootstrap_node(sys, node_cfg, monitor, nb))
            return 0;
        // 集群节点 actor 注册给 shutdown_mgr：关机统一终止
        //（插件保存后 → 集群 → 组件，main 不再手动 send_exit）
        if (nb.master)
            self->send(fw.shutdown_mgr, register_cluster_atom_v, nb.master);
        if (nb.client)
            self->send(fw.shutdown_mgr, register_cluster_atom_v, nb.client);
    }

    // ---- 本地运维控制台（系统组件：任何进程都有）----
    // ops 管理本机（help/list/reload + quit 统一关机）；master 进程额外
    // 注入注册表句柄 → reload-node/reload-nodes/nodes（管理下级节点）
    // 命令启用；worker/region 只管理自己。
    {
        auto ops = cluster::spawn_ops_actor(
            sys,
            cfg.node_cfg.node_name.empty() ? "standalone"
                                           : cfg.node_cfg.node_name,
            fw.plugin_mgr, nb.master);
        sys.registry().put("ops", ops);
        // ops 注册给 shutdown_mgr 统一终止：Ctrl+C 等直接 shutdown_atom
        // 的路径不经 ops，若 ops 不退出，actor_system 析构会等它永久挂起
        //（进程不退出——quit 能退而 Ctrl+C 不能退的根因）。
        caf::scoped_actor self{sys};
        self->send(fw.shutdown_mgr, register_cluster_atom_v, ops);
        cluster::start_console_thread(ops);
    }

    // ---- 集群验证后门：跨节点调用（resolve → connect → lookup → call）----
    if (!cfg.test_cross_call.empty() && nb.master) {
        run_cross_call_test(sys, nb.master, cfg.node_cfg.node_name,
                            cfg.test_cross_call);
    }
    // ---- 集群验证后门：跨节点调用 + 有界重试（重启窗口期不丢）----
    if (!cfg.test_cross_call_ex.empty() && nb.master) {
        run_cross_call_ex_test(sys, nb.master, cfg.node_cfg.node_name,
                               cfg.test_cross_call_ex);
    }

    // ---- 运维验证后门：远程热更（--test-remote-reload=<node>,<plugin>,<path>）----
    // 复用控制台命令路径：把 reload-node 命令字符串发给本机 OpsActor，
    // 走与交互控制台完全相同的解析/寻址/热更链路。
    if (!cfg.test_remote_reload.empty() && nb.master) {
        auto ops = caf::actor_cast<caf::actor>(sys.registry().get("ops"));
        if (ops) {
            // 完整命令前缀（reload / reload-node / reload-nodes / reload-all）
            // 原样发送；否则按单节点 reload-node <node>,<plugin>,<path> 拼装。
            std::string cmd;
            const bool full_cmd =
                cfg.test_remote_reload.rfind("reload ", 0) == 0
                || cfg.test_remote_reload.rfind("reload-node ", 0) == 0
                || cfg.test_remote_reload.rfind("reload-nodes ", 0) == 0
                || cfg.test_remote_reload.rfind("reload-all ", 0) == 0;
            if (full_cmd)
                cmd = cfg.test_remote_reload;
            else
                cmd = "reload-node " + cfg.test_remote_reload;
            std::replace(cmd.begin(), cmd.end(), ',', ' ');
            std::cout << "[OpsTest] " << cmd << " (delayed 12s for worker registration)"
                      << std::endl;
            // delayed_send：给 worker 完成注册留时间（node_resolve 才能命中）
            caf::scoped_actor self{sys};
            self->delayed_send(ops, std::chrono::seconds(12),
                               console_cmd_atom_v, cmd);
        } else {
            std::cout << "[OpsTest] ops actor not found in registry" << std::endl;
        }
    }

    // ---- 运维验证后门：自动 quit（--test-quit，任何模式）----
    // 复用控制台 quit 命令路径，验证优雅关机链自然退出（进程必须自己
    // 退出且 EXIT 0，不能靠外部强杀——优雅关机回归测试）。
    if (cfg.test_quit) {
        auto ops = caf::actor_cast<caf::actor>(sys.registry().get("ops"));
        if (ops) {
            std::cout << "[OpsTest] triggering quit (delayed 2s)"
                      << std::endl;
            caf::scoped_actor self{sys};
            self->delayed_send(ops, std::chrono::seconds(2),
                               console_cmd_atom_v, std::string("quit"));
        } else {
            std::cout << "[OpsTest] ops actor not found in registry"
                      << std::endl;
        }
    }

    // ---- 运维验证后门：模拟 Ctrl+C（--test-ctrl-c，任何模式）----
    // 直接发 shutdown_atom 给 shutdown_mgr（不经 ops），等价 Ctrl+C 的
    // console_handler 路径；验证 ops 注册给 shutdown_mgr 后也能自然退出。
    if (cfg.test_ctrl_c) {
        std::cout << "[OpsTest] simulating Ctrl+C (direct shutdown_atom, delayed 2s)"
                  << std::endl;
        caf::scoped_actor self{sys};
        self->delayed_send(fw.shutdown_mgr, std::chrono::seconds(2),
                           shutdown_atom{});
    }

    // ---- 等待关机：统一由 shutdown_mgr 负责 ----
    // shutdown_mgr 关机链：停插件（drain→save→shutdown）→ 杀集群
    // （register_cluster_atom 注册的 master/client）→ 杀组件
    //（plugin_mgr/registry/checkpoint）。所有触发（Ctrl+C / stdin EOF /
    // ops quit / 插件请求 / --test-quit）都发 shutdown_atom 给它，
    // main 只等它退出——进程必须自己自然退出，不许外部强杀。
    wait_for_shutdown(sys, fw);

    // current_logger() = caf::actor{};

    // ---- 关机泄露诊断 ----
    // actor_system 析构会 join 所有存活 actor——若有 actor 不退出，进程会
    // 挂死在析构（EXIT 非 0 或超时）。running() 是关机链走完后仍存活的 actor 数
    // 给 send_exit 异步排空留时间：等 200ms 后组件逐个退完，数字趋近 0
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto remaining = sys.registry().running();
    if (remaining > 0) {
        std::cout << "[LeakCheck] actors remaining after shutdown: " << remaining
                  << std::endl;
    }

    std::cout.flush();
    fflush(stdout);

    return 0;
}

CAF_MAIN()
