
#include "plugin/plugin_interface.hpp"

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
#include <spdlog/spdlog.h>
#include <windows.h>
#include <tlhelp32.h>
#include "services/logging_service.hpp"
#include "app_config.hpp"
#include "plugin/dynamic_library.hpp"
#include "cluster/bootstrap.hpp"
#include "cluster/ops_actor.hpp"
#include "bridge_actor.hpp"
#include "common/message_tags.hpp"

using namespace caf_plugin_system;

// ------------------------------------------------------------------
// 测试后门注入（--test-*）
// 2026-08-31：测试代码从独立 DLL 移回 exe 编译。core 不再动态加载
// caf_plugin_test.dll（修复 test DLL 卸载后 delayed 线程执行已卸载
// 代码段 → 0xC0000005 的根因）。exe 侧 main() 在 static 初始化阶段
// 调 set_test_backdoor 注册函数指针，caf_main 在 bootstrap 完成后调用。
// 改测试只重编 exe，core.dll 与插件 DLL 全链不重建。
// ------------------------------------------------------------------
namespace {
using TestBackdoorFn =
  void (*)(caf::actor_system&, const app_config&,
           const caf_plugin_system::cluster::BootstrapResult&,
           const caf_plugin_system::BootstrapResult&);
TestBackdoorFn g_test_backdoor = nullptr;
} // namespace

extern "C" PLUGIN_API void set_test_backdoor(TestBackdoorFn fn) {
  g_test_backdoor = fn;
}

// ------------------------------------------------------------------
// 每个进程的入口：节点引导（可选）→ 插件引导（可选）→ 等待关机。
// 两个模块正交：node-kind 决定集群角色，entry-plugins 决定插件加载。
// 2026-08-31：入口由 CAF_MAIN 宏（exe main.cpp）生成 actor_system，
// 本函数改为纯业务逻辑；sys 析构由 exec_main 管理。
// ------------------------------------------------------------------

extern "C" PLUGIN_API int caf_main(caf::actor_system& sys,
                                   const app_config& cfg) {

    // 8-31 崩溃根因修复：CAF 1.1 impl 析构默认
    // await_actors_before_shutdown=false → 不等 actor 退出就
    // scheduler->stop()，worker 处理残留 mailbox（spawn-server /
    // config-server 等内部 actor 的消息）时 message_data 已被析构
    // 释放 → 双释放 0xC0000005（type_id_list::size/compare）。
    // 置 true：析构先 await_running_count_equal(0) 再停调度器。
    sys.await_actors_before_shutdown(true);

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
    }

    // 注：日志无注入环节——logging_service 是系统组件，bootstrap_system_components
    // 最先 spawn 时即成为唯一日志入口（log_info/log_error，见 logging_service.hpp）。

    // ---- 外部语言节点 sidecar（--caf-plugin-system.bridge-port>0）----
    // 必须在节点引导【之前】spawn：bootstrap_node 的 exported_actors 收集
    // 只查一次，bridge 的 external_echo 若注册晚了，manifest 上报不含它，
    // master 路由查不到（no_such_key）。注册给 shutdown_mgr 统一终止。
    if (cfg.bridge_port > 0) {
        auto bridge = spawn_bridge_actor(
            sys, fw.registry, cfg.bridge_port,
            cfg.node_cfg.node_name.empty() ? "standalone"
                                           : cfg.node_cfg.node_name);
        caf::scoped_actor self{sys};
        self->send(fw.shutdown_mgr, register_cluster_atom_v, bridge);
    }

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

    // ---- 集群/运维验证后门统一入口（--test-*）----
    // 实现编译在 exe（src/app/test/app_backdoors.cpp + app_tests_fixed.cpp），
    // main() 经 set_test_backdoor 注册函数指针，此处有 --test-* flag 时调用。
    // 2026-08-31 从独立 DLL 移回 exe：修 test DLL 卸载后 delayed 线程执行
    // 已卸载代码段 → 0xC0000005（崩溃根因）；且改测试只重编 exe，core.dll
    // 与插件 DLL 全链不重建（插件全链链接 caf_plugin_core，core 重编即全链）。
    if (cfg.has_test_flags() && g_test_backdoor) {
        g_test_backdoor(sys, cfg, nb, fw);
    }

    // ---- 等待关机：统一由 shutdown_mgr 负责 ----
    // shutdown_mgr 关机链：停插件（drain→save→shutdown）→ 杀集群
    // （register_cluster_atom 注册的 master/client）→ 杀组件
    //（plugin_mgr/registry/checkpoint）。所有触发（Ctrl+C / stdin EOF /
    // ops quit / 插件请求 / --test-quit）都发 shutdown_atom 给它，
    // main 只等它退出——进程必须自己自然退出，不许外部强杀。
    wait_for_shutdown(sys, fw, cfg);

    // TODO(leakfix): 清空核心 current_logger() 静态持有——logging_service actor
    // 引用被函数内 static 持有，DLL 常驻不析构 → CRT leak dump 报 126 块簇
    current_logger() = caf::actor{};
    // TODO(leakfix): 清空 shutdown_manager_ref()——级联持有
    // shutdown_mgr→plugin_mgr→PluginEntry→插件主 actor 整棵对象树
    clear_shutdown_manager_ref();
    // TODO(leakfix): spdlog registry 是 static 单例，sinks 常驻到进程退出
    spdlog::shutdown();

    // 2026-08-31：不再做关机泄露诊断（sleep+running()+assert 已删）——
    // await_actors_before_shutdown(true) 使 actor_system 析构强制等待全部
    // actor 退出（残留即挂死，不会静默），诊断冗余。

    std::cout.flush();
    fflush(stdout);

    // 2026-08-31 实验结论：不显式卸载插件 DLL（release_plugin_libraries）。
    // 静态导入架构下 exe 静态导入 core.dll（进程退出才卸载），"先卸插件
    // 才能卸 core"动机已失效；A/B 对照 13 次：泄漏数字完全一致（py 6 块
    // immortal 基线 / lua-ts 0 块），且不卸载版 EXIT 全 0（卸载版在
    // 17:12-24 窗口偶发 EXIT=1——卸载期插件 DLL static 析构竞态嫌疑）。
    // 插件 DLL 由 OS 在进程退出时统一清理。

    return 0;
}