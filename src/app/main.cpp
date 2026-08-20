#include <caf/all.hpp>
#include <iostream>
#include <thread>
#include <filesystem>
#include <vector>
#include <unordered_map>

#ifdef _WIN32
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
#else
    #include <csignal>
#endif

#include "plugin_manager.hpp"
#include "service_registry.hpp"
#include "checkpoint_manager.hpp"
#include "graceful_shutdown.hpp"
#include "plugin_loader.hpp"

static caf::actor g_shutdown_mgr;

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        if (g_shutdown_mgr) caf::anon_send(g_shutdown_mgr, shutdown_atom::value);
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) {
    if (g_shutdown_mgr) caf::anon_send(g_shutdown_mgr, shutdown_atom::value);
}
#endif

// ------------------------------------------------------------------
// CAF 配置
// ------------------------------------------------------------------

struct app_config : caf::actor_system_config {
    std::vector<std::string> entry_plugins;
    std::vector<std::string> shutdown_order;

    app_config() {
        opt_group{custom_options_, "caf-plugin-system"}
            .add(entry_plugins, "entry-plugins,e", "entry plugins to auto-load with deps")
            .add(shutdown_order, "shutdown-order,s", "plugin shutdown order (reverse topo if empty)");
    }
};

// ------------------------------------------------------------------

void caf_main(caf::actor_system& sys, const app_config& cfg) {
    auto registry = sys.spawn<ServiceRegistry>();
    auto checkpoint_mgr = sys.spawn<CheckpointManager>(std::filesystem::path{"./checkpoints"});
    auto plugin_mgr = sys.spawn<PluginManager>(registry, checkpoint_mgr);

    auto get_stop_order = [&]() -> std::vector<std::string> {
        if (!cfg.shutdown_order.empty()) return cfg.shutdown_order;
        return {"BusinessPlugin", "LoggerPlugin"};
    };

    g_shutdown_mgr = sys.spawn<GracefulShutdown>(
        ShutdownConfig{},
        plugin_mgr, registry, checkpoint_mgr, get_stop_order);

    // 让 PluginManager 知道谁是关机总管，以便插件请求关机时转发
    self->send(plugin_mgr, shutdown_atom::value, g_shutdown_mgr);
        ShutdownConfig{},
        plugin_mgr, registry, checkpoint_mgr, get_stop_order);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    // ---- 第 1 步：从 CAF 配置读取入口插件 ----
    if (cfg.entry_plugins.empty()) {
        std::cerr << "[Init] No entry plugins configured. Use --caf-plugin-system.entry-plugins=PluginA,PluginB"
                  << " or --config-file=app.ini" << std::endl;
        self->send(g_shutdown_mgr, shutdown_atom::value);
        return;
    }

    std::cout << "[Init] Entry plugins:";
    for (const auto& name : cfg.entry_plugins) std::cout << " " << name;
    std::cout << std::endl;

    // ---- 第 2 步：扫描所有插件，获取 manifest ----
    auto all_plugins = scan_all_plugins("./plugins");
    if (all_plugins.empty()) {
        std::cerr << "[Init] No plugins found in ./plugins/" << std::endl;
        self->send(g_shutdown_mgr, shutdown_atom::value);
        return;
    }

    std::cout << "[Init] Scanned " << all_plugins.size() << " plugin(s) in ./plugins/" << std::endl;

    // ---- 第 3 步：从入口出发，自动解析依赖链 ----
    auto required = resolve_dependencies(cfg.entry_plugins, all_plugins);
    if (required.empty()) {
        std::cerr << "[Init] Failed to resolve dependencies" << std::endl;
        self->send(g_shutdown_mgr, shutdown_atom::value);
        return;
    }

    std::cout << "[Init] Resolved " << required.size() << " plugin(s) to load:";
    for (const auto& p : required) std::cout << " " << p.name;
    std::cout << std::endl;

    // ---- 第 4 步：拓扑排序 ----
    auto load_order = compute_load_order(required);
    if (load_order.empty()) {
        std::cerr << "[Init] Circular dependency detected" << std::endl;
        self->send(g_shutdown_mgr, shutdown_atom::value);
        return;
    }

    std::cout << "[Init] Load order:";
    for (const auto& name : load_order) std::cout << " " << name;
    std::cout << std::endl;

    // ---- 第 5 步：按序加载 ----
    std::unordered_map<std::string, PluginInfo> info_map;
    for (auto& p : required) info_map[p.name] = std::move(p);

    for (const auto& name : load_order) {
        auto it = info_map.find(name);
        if (it == info_map.end()) continue;
        self->request(plugin_mgr, caf::infinite, load_atom::value, name, it->second.path.string())
            .receive([](bool ok) { std::cout << "Load: " << ok << std::endl; },
                     [](const caf::error& e) { std::cerr << "Load err: " << to_string(e) << std::endl; });
    }

    bool healthy = true;
    for (const auto& name : load_order) {
        auto actor = self->request(plugin_mgr, caf::infinite, resolve_plugin_atom::value, name)
            .receive([](const caf::actor& a) { return a; }, [](auto) { return caf::actor{}; });
        if (!actor) { healthy = false; break; }
    }

    if (!healthy) {
        self->send(g_shutdown_mgr, shutdown_atom::value);
        return;
    }

    self->send(g_shutdown_mgr, ready_atom::value);
    std::cout << "[System] Startup complete. Press Ctrl+C to shutdown." << std::endl;

    self->wait_for(g_shutdown_mgr);
}

// 手写 main，避免 CAF_MAIN 宏的 id_block 限制
int main(int argc, char** argv) {
    app_config cfg;
    if (auto err = cfg.parse(argc, argv)) {
        std::cerr << "Config error" << std::endl;
        return 1;
    }
    caf::actor_system sys{cfg};
    caf_main(sys, cfg);
    return 0;
}
