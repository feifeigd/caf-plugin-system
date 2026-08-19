#include <caf/all.hpp>
#include <iostream>
#include <thread>

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

void caf_main(caf::actor_system& sys) {
    auto registry = sys.spawn<ServiceRegistry>();
    auto checkpoint_mgr = sys.spawn<CheckpointManager>(std::filesystem::path{"./checkpoints"});
    auto plugin_mgr = sys.spawn<PluginManager>(registry, checkpoint_mgr);

    auto get_stop_order = [&]() -> std::vector<std::string> {
        return {"BusinessPlugin", "LoggerPlugin"};
    };

    g_shutdown_mgr = sys.spawn<GracefulShutdown>(
        ShutdownConfig{},
        plugin_mgr, registry, checkpoint_mgr, get_stop_order);

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    std::vector<std::pair<std::string, std::string>> load_order = {
        {"LoggerPlugin", std::string("./plugins/logger/liblogger_plugin") + PLUGIN_EXT},
        {"BusinessPlugin", std::string("./plugins/business/libbusiness_plugin") + PLUGIN_EXT}
    };

    for (const auto& [name, path] : load_order) {
        self->request(plugin_mgr, caf::infinite, load_atom::value, name, path)
            .receive([](bool ok) { std::cout << "Load: " << ok << std::endl; },
                     [](const caf::error& e) { std::cerr << "Load err: " << to_string(e) << std::endl; });
    }

    bool healthy = true;
    for (const auto& [name, _] : load_order) {
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

CAF_MAIN()
