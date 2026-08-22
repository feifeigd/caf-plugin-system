#include "framework_bootstrap.hpp"
#include "plugin/plugin_manager.hpp"
#include "service_registry.hpp"
#include "checkpoint_manager.hpp"
#include "graceful_shutdown.hpp"
#include "plugin/plugin_loader.hpp"
#include "common/message_meta.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>
#include <caf/logger.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#include <sys/stat.h>
#endif

namespace caf_plugin_system {

namespace {

/// 当前关机总管（信号处理用）。进程级单例，bootstrap 时赋值。
caf::actor& shutdown_manager_ref() {
    static caf::actor mgr;
    return mgr;
}

} // namespace

void install_stdin_watchdog(caf::actor shutdown_mgr) {
#ifdef _WIN32
    // 交互控制台（含 ConPTY 伪终端）不装 watchdog：用户可直接 Ctrl+C，
    // console_handler 走优雅关机。只有非交互 stdin 才需要 EOF 哨兵。
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        return;
    // 非交互：仅 FIFO（管道）装哨兵；文件重定向（REG）不装——EOF 是正常
    // 结束不是父进程消失。
    struct _stat64 st;
    if (_fstat64(_fileno(stdin), &st) != 0
        || (st.st_mode & _S_IFMT) != _S_IFIFO)
        return;
#else
    struct stat st;
    if (fstat(STDIN_FILENO, &st) != 0 || !S_ISFIFO(st.st_mode))
        return;
#endif
    // 框架日志可能静默丢失（CAF logger 输出目标不定），哨兵状态用 cout。
    std::cout << "[Watchdog] stdin EOF watchdog armed (pipe stdin)"
              << std::endl;
    std::thread([shutdown_mgr] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        char buf[64];
        size_t total = 0;
        while (fread(buf, 1, sizeof buf, stdin) > 0)
            total += sizeof buf;
        // 只有"读过数据后 EOF"才算父进程/终端消失；启动即 EOF 的空管道
        // （如 WSL→PowerShell 继承的空 stdin）不触发，避免误关机。
        if (total == 0)
            return;
        // 统一关机：EOF = 父进程消失 → shutdown_mgr 优雅关机链
        caf::anon_send(shutdown_mgr, shutdown_atom{});
    }).detach();
}

namespace {

/// 禁用控制台 QuickEdit/选择模式：Windows 控制台默认开启快速编辑，
/// 鼠标划过/选中文本后 Ctrl+C 会被劫持成"复制选中内容"而不是中断信号
/// （程序收不到 CTRL_C_EVENT，表现为 Ctrl+C 无法退出）。运维工具
/// 必须保证 Ctrl+C 语义 = 优雅关机。
void disable_quick_edit() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode)) {
        mode &= ~ENABLE_QUICK_EDIT_MODE;
        mode &= ~ENABLE_INSERT_MODE;  // 顺带关掉插入模式（防误触）
        SetConsoleMode(h, mode);
    }
#endif
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT
        || signal == CTRL_CLOSE_EVENT) {
        // 诊断日志（信号线程）：fprintf 单次调用原子，不会与主线程交错；
        // 若按了 Ctrl+C 但窗口无此输出 = 被 QuickEdit 选中模式劫持。
        fprintf(stderr, "[Signal] %s received, triggering graceful shutdown\n",
                signal == CTRL_C_EVENT ? "Ctrl+C" : "Ctrl+Break/Close");
        if (shutdown_manager_ref()) {
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        }
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) {
    if (shutdown_manager_ref()) {
        caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
    }
}
#endif

} // namespace

framework_config::framework_config() {
    // 元对象注册必须在 actor_system 构造前（CAF 禁止后置，UB）。
    // framework_config 由 exec_main 在 parse 前、actor_system 构造前创建，
    // 这里是最干净的注册窗口（CAF_MAIN 的模块参数机制只服务自带模块）。
    app_meta::init();
    // 插件私有消息类型的元对象：扫描插件目录并调用可选导出
    // register_meta_objects()。注册了元对象的插件 DLL 自此常驻。
    preregister_plugin_meta(plugins_dir);

    // CAF 1.1 框架日志配置：控制台 + 文件双输出
    set("caf.logger.console.verbosity", "info");
    set("caf.logger.file.verbosity", "debug");
    set("caf.logger.file.path", "logs/caf-framework.log");
    set("caf.logger.console.colored", true);  // 带颜色的控制台输出

    opt_group{custom_options_, "caf-plugin-system"}
        .add(entry_plugins, "entry-plugins,e", "entry plugins to auto-load with deps")
        .add(shutdown_order, "shutdown-order,s", "plugin shutdown order (reverse load order if empty)")
        .add(plugins_dir, "plugins-dir,p", "plugin scan directory (default ./plugins)")
        .add(test_auto_shutdown, "test-auto-shutdown",
             "auto trigger graceful shutdown after startup (smoke test)")
        .add(allow_cross_node, "allow-cross-node",
             "trust remote cluster nodes bypassing service ACL (default false)")
        .add(test_cross_call, "test-cross-call",
             "cross-node call this service after node registration (cluster test)")
        .add(test_cross_call_ex, "test-cross-call-ex",
             "cross-node call this service with bounded retry (15x1s) after node registration")
        .add(test_remote_reload, "test-remote-reload",
             "master: remote hot-reload <node>,<plugin>,<path> after node registration (ops test)")
        .add(test_quit, "test-quit",
             "trigger ops quit after startup (verify graceful shutdown, process must exit 0)")
        .add(test_ctrl_c, "test-ctrl-c",
             "send shutdown_atom directly after startup (simulate Ctrl+C path, verify exit 0)");
}

bool bootstrap_system_components(caf::actor_system& sys,
                                 const framework_config& cfg,
                                 BootstrapResult& out) {
    CAF_LOG_INFO("system components startup");

    out.registry = sys.spawn<ServiceRegistry>(cfg.allow_cross_node);
    out.checkpoint_mgr
        = sys.spawn<CheckpointManager>(std::filesystem::path{"./checkpoints"});
    out.plugin_mgr = sys.spawn<PluginManager>(out.registry, out.checkpoint_mgr);

    // 关机顺序：显式配置优先，否则加载反序（依赖者先停）。load_order 由
    // bootstrap_plugins 填充（组件引导阶段尚无插件）。
    auto get_stop_order = [&]() -> std::vector<std::string> {
        if (!cfg.shutdown_order.empty()) return cfg.shutdown_order;
        auto rev = out.load_order;
        std::reverse(rev.begin(), rev.end());
        return rev;
    };

    out.shutdown_mgr = sys.spawn<GracefulShutdown>(
        ShutdownConfig{}, out.plugin_mgr, out.registry, out.checkpoint_mgr,
        get_stop_order);
    shutdown_manager_ref() = out.shutdown_mgr;

    // 保证 Ctrl+C 语义 = 中断信号（QuickEdit 会劫持成复制）
    disable_quick_edit();
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    // 让 PluginManager 知道谁是关机总管，以便插件请求关机时转发
    self->send(out.plugin_mgr, shutdown_atom{}, out.shutdown_mgr);

    // 组件就绪（任何进程都 ready：纯节点无 bootstrap_plugins 不会发 ready，
    // 否则 quit 时 GracefulShutdown 走 "Not ready, forcing exit" 强杀路径）
    self->send(out.shutdown_mgr, ready_atom{});

    CAF_LOG_INFO("system components ready");
    return true;
}

bool bootstrap_plugins(caf::actor_system& sys, const framework_config& cfg,
                       BootstrapResult& out) {
    caf::scoped_actor self{sys};

    // ---- 第 1 步：入口插件 ----
    if (cfg.entry_plugins.empty()) {
        return true;  // 无插件：合法（纯节点/纯组件进程）
    }

    CAF_LOG_INFO("Entry plugins:" << [&]() {
        std::string s;
        for (const auto& name : cfg.entry_plugins) s += " " + name;
        return s;
    }());

    // ---- 第 2 步：扫描 ----
    auto all_plugins = scan_all_plugins(cfg.plugins_dir);
    if (all_plugins.empty()) {
        CAF_LOG_ERROR("No plugins found in " << cfg.plugins_dir);
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    CAF_LOG_INFO("Scanned " << all_plugins.size() << " plugin(s) in " << cfg.plugins_dir);

    // ---- 第 3 步：依赖解析 ----
    auto required = resolve_dependencies(cfg.entry_plugins, all_plugins);
    if (required.empty()) {
        CAF_LOG_ERROR("Failed to resolve dependencies");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    CAF_LOG_INFO("Resolved " << required.size() << " plugin(s) to load:" << [&]() {
        std::string s;
        for (const auto& p : required) s += " " + p.name;
        return s;
    }());

    // ---- 第 4 步：拓扑排序 ----
    auto load_order = compute_load_order(required);
    if (load_order.empty()) {
        CAF_LOG_ERROR("Circular dependency detected");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    CAF_LOG_INFO("Load order:" << [&]() {
        std::string s;
        for (const auto& name : load_order) s += " " + name;
        return s;
    }());
    out.load_order = load_order;

    // ---- 第 5 步：按序加载 ----
    std::unordered_map<std::string, PluginInfo> info_map;
    for (auto& p : required) info_map[p.name] = std::move(p);

    for (const auto& name : load_order) {
        auto it = info_map.find(name);
        if (it == info_map.end()) continue;
        self->request(out.plugin_mgr, caf::infinite, load_atom{}, name, it->second.path.string())
            .receive([](bool ok) { CAF_LOG_INFO("Load result: " << ok); },
                     [](const caf::error& e) { CAF_LOG_ERROR("Load err: " << to_string(e)); });
    }

    // ---- 健康检查：所有目标插件必须可 resolve ----
    bool healthy = true;
    for (const auto& name : load_order) {
        caf::actor plugin_actor;
        self->request(out.plugin_mgr, caf::infinite, resolve_plugin_atom{}, name)
            .receive([&plugin_actor](const caf::actor& a) { plugin_actor = a; },
                     [](caf::error&) {});
        if (!plugin_actor) { healthy = false; break; }
    }

    if (!healthy) {
        CAF_LOG_ERROR("One or more plugins failed to resolve");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    self->send(out.shutdown_mgr, ready_atom{});
    CAF_LOG_INFO("Startup complete. Press Ctrl+C to shutdown.");
    return true;
}

void wait_for_shutdown(caf::actor_system& sys, const BootstrapResult& fw) {
    // 信号处理已在 bootstrap_system_components 注册（console_handler /
    // signal_handler），不重复注册（SetConsoleCtrlHandler 重复注册会让
    // handler 被调用两次 → 双 shutdown_atom）。
    install_stdin_watchdog(fw.shutdown_mgr);
    caf::scoped_actor self{sys};
    self->wait_for(fw.shutdown_mgr);
    CAF_LOG_INFO("framework shutdown complete");
}

} // namespace caf_plugin_system
