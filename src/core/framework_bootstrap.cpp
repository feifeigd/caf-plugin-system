#include "framework_bootstrap.hpp"
#include "plugin_manager.hpp"
#include "service_registry.hpp"
#include "checkpoint_manager.hpp"
#include "graceful_shutdown.hpp"
#include "plugin_loader.hpp"
#include "common/message_meta.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>
#include <caf/logger.hpp>

#include <algorithm>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace caf_plugin_system {

namespace {

/// 当前关机总管（信号处理用）。进程级单例，bootstrap 时赋值。
caf::actor& shutdown_manager_ref() {
    static caf::actor mgr;
    return mgr;
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT
        || signal == CTRL_CLOSE_EVENT) {
        if (shutdown_manager_ref())
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) {
    if (shutdown_manager_ref())
        caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
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
             "auto trigger graceful shutdown after startup (smoke test)");
}

bool bootstrap_plugin_framework(caf::actor_system& sys,
                                const framework_config& cfg,
                                BootstrapResult& out) {
    CAF_LOG_INFO("framework startup begin");

    out.registry = sys.spawn<ServiceRegistry>();
    out.checkpoint_mgr
        = sys.spawn<CheckpointManager>(std::filesystem::path{"./checkpoints"});
    out.plugin_mgr = sys.spawn<PluginManager>(out.registry, out.checkpoint_mgr);

    // 关机顺序：显式配置优先，否则加载反序（依赖者先停）
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

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    // 让 PluginManager 知道谁是关机总管，以便插件请求关机时转发
    self->send(out.plugin_mgr, shutdown_atom{}, out.shutdown_mgr);

    // ---- 第 1 步：入口插件 ----
    if (cfg.entry_plugins.empty()) {
        CAF_LOG_ERROR("No entry plugins configured. Use --caf-plugin-system.entry-plugins=PluginA,PluginB"
                      " or --config-file=app.ini");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
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
    caf::scoped_actor self{sys};
    self->wait_for(fw.shutdown_mgr);
    CAF_LOG_INFO("framework shutdown complete");
}

} // namespace caf_plugin_system
