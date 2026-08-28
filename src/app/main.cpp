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
#include "app_backdoors.hpp"
#include "cluster/bootstrap.hpp"
#include "cluster/ops_actor.hpp"
#include "bridge_actor.hpp"
#include "common/message_tags.hpp"

#ifdef _WIN32
#include <windows.h> // ExitProcess（DLL 池卸载后跳过静态析构阶段）
#endif

#ifdef _DEBUG
// Debug CRT 泄露检测：进程退出时（actor_system 析构之后）自动 dump 堆泄露
#include <crtdbg.h>
#endif

using namespace caf_plugin_system;

// ------------------------------------------------------------------
// 验证后门（--test-auto-shutdown / --test-cross-call* / --test-*）
// 统一入口见 app_backdoors.cpp（run_test_backdoors）。
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
    // 断言也走 stderr + 调试器断点：默认的模态对话框会在无头/自动化
    // 运行时卡死进程（is_block_type_valid 这类堆断言现场会停在对话框）。
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
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
    // 实现见 app_backdoors.cpp：auto_shutdown / cross_call /
    // cross_call_ex / bridge_call / remote_reload / quit / ctrl_c。
    // 各后门按 cfg 字段分发，无需本进程配置时静默跳过。
    run_test_backdoors(sys, cfg, nb, fw);

    // ---- 等待关机：统一由 shutdown_mgr 负责 ----
    // shutdown_mgr 关机链：停插件（drain→save→shutdown）→ 杀集群
    // （register_cluster_atom 注册的 master/client）→ 杀组件
    //（plugin_mgr/registry/checkpoint）。所有触发（Ctrl+C / stdin EOF /
    // ops quit / 插件请求 / --test-quit）都发 shutdown_atom 给它，
    // main 只等它退出——进程必须自己自然退出，不许外部强杀。
    wait_for_shutdown(sys, fw);

    // TODO(leakfix): 清空核心 current_logger() 静态持有——logging_service actor
    // 引用被函数内 static 持有，DLL 常驻不析构 → CRT leak dump 报 126 块簇
    current_logger() = caf::actor{};
    // TODO(leakfix): 清空 shutdown_manager_ref()——级联持有
    // shutdown_mgr→plugin_mgr→PluginEntry→插件主 actor 整棵对象树
    clear_shutdown_manager_ref();
    // TODO(leakfix-probe): spdlog registry 是 static 单例，sinks 常驻到进程退出
    spdlog::shutdown();

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

// ------------------------------------------------------------------
// 手动 main（替代 CAF_MAIN）：显式控制 actor_system 生命周期。
// 业务退出后先 delete actor_system（join 全部 actor），再循环 FreeLibrary
// 卸载 caf_plugin_core → caf_core：引用计数归零后 DLL 真卸载，
// static 析构提前到 leak dump 之前 → 进程级 static 持有的对象随之释放
//（CRT 泄漏检测只报"进程退出时仍存活"的堆对象，DLL 静态析构晚于 dump）。
// ------------------------------------------------------------------
// 卸载插件 DLL：SEH 保护（插件 static 析构（Python 等）可能崩溃——接住后
// 继续卸载其余模块，泄漏 dump 照常进行）
static void unload_plugin_dlls() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32 me; me.dwSize = sizeof(me);
    if (Module32First(snap, &me)) {
        do {
            if (strstr(me.szExePath, "\\plugins\\")) {
                for (int i = 0; i < 4; ++i) {
                    HMODULE h = GetModuleHandleA(me.szModule);
                    if (!h) break;
                    __try {
                        FreeLibrary(h);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        printf("[Unload] plugin %s DETACH crash code=0x%X\n",
                               me.szModule, GetExceptionCode());
                    }
                    Sleep(200);
                }
            }
        } while (Module32Next(snap, &me));
    }
    CloseHandle(snap);
}

static int main_dispatch(caf::actor_system& sys, const app_config& cfg) {
    return caf_main(sys, cfg);
}

int main(int argc, char** argv) {
    caf::core::init_global_meta_objects();
    app_config cfg;
    if (auto err = cfg.parse(argc, argv)) {
        auto err_str = caf::to_string(err);
        fprintf(stderr, "error while parsing CLI and file options: %s\n",
                err_str.c_str());
        return EXIT_FAILURE;
    }
    if (cfg.helptext_printed())
        return EXIT_SUCCESS;

    auto* sys = new caf::actor_system(cfg);
    int rc = main_dispatch(*sys, cfg);
    delete sys;  // join 所有 actor——插件 actor 全部析构完毕

    #ifdef _DEBUG
    // ---- 卸载链：leak dump 前强制 DLL static 析构 ----
    // 进程级 static（current_logger/shutdown_manager_ref/CAF 元对象表/
    // Python 解释器等）持有的堆对象在 DLL 常驻时不析构 → CRT dump 误报。
    // 手动 main 先析构 actor_system（插件 actor 全部退出），再按依赖序
    // 卸载：插件 DLL（含其专属引擎 DLL，如 python3.dll）→ caf_plugin_core
    // → caf_core。循环 FreeLibrary 把引用计数减到 0 → static 析构提前执行；
    // Sleep 等延迟卸载完成（模块映射移除可能滞后，但 static 已析构）。
    // ① 插件 DLL：路径含 \plugins\ 的模块（先卸，释放对 core/caf_core 的依赖）
    // 暂禁用：实测改变 lua 场景 shutdown 竞态（0xC0000005，二进制布局敏感 UB），
    // 插件卸载改由插件自身 shutdown 流程负责；py 场景另行处理
    // unload_plugin_dlls();
    // ② caf_plugin_core（delayimp + 插件依赖引用）
    for (int i = 0; i < 4; ++i) {
        HMODULE hc = GetModuleHandleA("caf_plugin_core.dll");
        if (!hc) break;
        FreeLibrary(hc);
        Sleep(300);
    }
    // ③ caf_core（delayimp + core/caf_io 依赖引用）
    for (int i = 0; i < 4; ++i) {
        HMODULE hca = GetModuleHandleA("caf_core.dll");
        if (!hca) break;
        FreeLibrary(hca);
        Sleep(300);
    }
    #endif

    return rc;
}
