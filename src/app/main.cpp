#include <caf/all.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <caf/logger.hpp>
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
#include "common/message_meta.hpp"
#include "common/plugin_envelope.hpp"

static caf::actor g_shutdown_mgr;

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        if (g_shutdown_mgr) caf::anon_send(g_shutdown_mgr, shutdown_atom{});
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) {
    if (g_shutdown_mgr) caf::anon_send(g_shutdown_mgr, shutdown_atom{});
}
#endif

// ------------------------------------------------------------------
// CAF 配置：同时启用 CAF 自带 logger（框架级）和 spdlog（业务级）
// ------------------------------------------------------------------

struct app_config : caf::actor_system_config {
    std::vector<std::string> entry_plugins;
    std::vector<std::string> shutdown_order;
    bool test_auto_shutdown = false;

    app_config() {
        // CAF 1.1 框架日志配置：控制台 + 文件双输出
        set("caf.logger.console.verbosity", "info");
        set("caf.logger.file.verbosity", "debug");
        set("caf.logger.file.path", "logs/caf-framework.log");
        set("caf.logger.console.colored", true);  // 带颜色的控制台输出

        opt_group{custom_options_, "caf-plugin-system"}
            .add(entry_plugins, "entry-plugins,e", "entry plugins to auto-load with deps")
            .add(shutdown_order, "shutdown-order,s", "plugin shutdown order (reverse topo if empty)")
            .add(test_auto_shutdown, "test-auto-shutdown", "auto trigger graceful shutdown after startup (smoke test)");
    }
};

// ------------------------------------------------------------------

void caf_main(caf::actor_system& sys, const app_config& cfg) {
    CAF_LOG_INFO("framework startup begin");

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

#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    // 让 PluginManager 知道谁是关机总管，以便插件请求关机时转发
    self->send(plugin_mgr, shutdown_atom{}, g_shutdown_mgr);

    // ---- 第 1 步：从 CAF 配置读取入口插件 ----
    if (cfg.entry_plugins.empty()) {
        CAF_LOG_ERROR("No entry plugins configured. Use --caf-plugin-system.entry-plugins=PluginA,PluginB"
                      " or --config-file=app.ini");
        self->send(g_shutdown_mgr, shutdown_atom{});
        return;
    }

    CAF_LOG_INFO("Entry plugins:" << [&]() {
        std::string s;
        for (const auto& name : cfg.entry_plugins) s += " " + name;
        return s;
    }());

    // ---- 第 2 步：扫描所有插件，获取 manifest ----
    auto all_plugins = scan_all_plugins("./plugins");
    if (all_plugins.empty()) {
        CAF_LOG_ERROR("No plugins found in ./plugins/");
        self->send(g_shutdown_mgr, shutdown_atom{});
        return;
    }

    CAF_LOG_INFO("Scanned " << all_plugins.size() << " plugin(s) in ./plugins/");

    // ---- 第 3 步：从入口出发，自动解析依赖链 ----
    auto required = resolve_dependencies(cfg.entry_plugins, all_plugins);
    if (required.empty()) {
        CAF_LOG_ERROR("Failed to resolve dependencies");
        self->send(g_shutdown_mgr, shutdown_atom{});
        return;
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
        self->send(g_shutdown_mgr, shutdown_atom{});
        return;
    }

    CAF_LOG_INFO("Load order:" << [&]() {
        std::string s;
        for (const auto& name : load_order) s += " " + name;
        return s;
    }());

    // ---- 第 5 步：按序加载 ----
    std::unordered_map<std::string, PluginInfo> info_map;
    for (auto& p : required) info_map[p.name] = std::move(p);

    for (const auto& name : load_order) {
        auto it = info_map.find(name);
        if (it == info_map.end()) continue;
        self->request(plugin_mgr, caf::infinite, load_atom{}, name, it->second.path.string())
            .receive([](bool ok) { CAF_LOG_INFO("Load result: " << ok); },
                     [](const caf::error& e) { CAF_LOG_ERROR("Load err: " << to_string(e)); });
    }

    bool healthy = true;
    for (const auto& name : load_order) {
        caf::actor plugin_actor;
        self->request(plugin_mgr, caf::infinite, resolve_plugin_atom{}, name)
            .receive([&plugin_actor](const caf::actor& a) { plugin_actor = a; },
                     [](caf::error&) {});
        if (!plugin_actor) { healthy = false; break; }
    }

    if (!healthy) {
        CAF_LOG_ERROR("One or more plugins failed to resolve");
        self->send(g_shutdown_mgr, shutdown_atom{});
        return;
    }

    self->send(g_shutdown_mgr, ready_atom{});
    CAF_LOG_INFO("Startup complete. Press Ctrl+C to shutdown.");

    // 冒烟测试后门：自动触发优雅关机，验证 drain/save/checkpoint 全链路
    if (cfg.test_auto_shutdown) {
        // ACL 自测：以未受信身份（main 的 scoped_actor 不在白名单）调用
        // business_service，应被服务代理拦截。若没被拦住，"shutdown" 命令
        // 会触发 request_shutdown 让系统立即开始关机——从日志一眼可辨。
        caf::actor biz_proxy;
        self->request(registry, caf::infinite, resolve_atom{}, "business_service")
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
        // 热更新自测：从【旁路新路径】加载 BusinessPlugin v2（同一份源码加
        // BIZ_HOT_V2 编出，绕过 Windows 文件锁与 LoadLibrary 路径缓存）。
        // 期望链路：reload → 状态内存移交 → 代理热切换 → 旧实例排空退役。
        self->request(plugin_mgr, caf::infinite, reload_atom{},
                      std::string("BusinessPlugin"),
                      std::string("./updates/business_plugin_v2.dll"))
            .receive([](bool ok) {
                         std::cout << "[HotUpdate] reload result: " << ok << std::endl;
                     },
                     [](const caf::error& e) {
                         std::cout << "[HotUpdate] reload error: "
                                   << caf::to_string(e) << std::endl;
                     });
        // 验证新代码：直连插件 actor（不经服务代理，不受 ACL 约束）
        caf::actor biz;
        self->request(plugin_mgr, caf::infinite, resolve_plugin_atom{}, "BusinessPlugin")
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
        self->delayed_send(g_shutdown_mgr, std::chrono::seconds(3), shutdown_atom{});
    }

    self->wait_for(g_shutdown_mgr);
    CAF_LOG_INFO("framework shutdown complete");
}

// 手写 main，避免 CAF_MAIN 宏的 id_block 限制
int main(int argc, char** argv) {
    // 手写 main 必须显式初始化 CAF 内置类型的运行时元对象（CAF_MAIN 会自动做这件事）
    caf::core::init_global_meta_objects();
    // CAF 1.1 消息析构通过元对象表调用 destroy；自定义类型也必须注册，
    // 否则消息释放时会通过空元对象指针发起调用（exec at 0x0 崩溃）
    app_meta::init();
    // 插件私有消息类型的元对象：CAF 禁止在 actor_system 构造后注册（UB），
    // 故提前扫描插件目录并调用各插件的可选导出 register_meta_objects()。
    // 注册了元对象的插件 DLL 自此常驻（元对象函数指针指向 DLL 代码段）。
    preregister_plugin_meta("./plugins");

    app_config cfg;
    if (auto err = cfg.parse(argc, argv)) {
        std::cerr << "Config error: " << caf::to_string(err) << std::endl;
        return 1;
    }
    caf::actor_system sys{cfg};
    caf_main(sys, cfg);
    return 0;
}
