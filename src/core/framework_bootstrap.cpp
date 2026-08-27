#include "framework_bootstrap.hpp"
#include "plugin/plugin_manager.hpp"
#include "service_registry.hpp"
#include "checkpoint_manager.hpp"
#include "graceful_shutdown.hpp"
#include "services/logging_service.hpp"
#include "services/time_service.hpp"
#include "plugin/plugin_loader.hpp"
#include "common/message_meta.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>
#include <caf/logger.hpp>
#include <caf/actor_registry.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
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

/// 控制台销毁标志：CTRL_CLOSE_EVENT（点窗口 X）时置 true。
/// 关机链的 std::cout 必须跳过（写销毁中的控制台句柄会阻塞，拖死关机链
/// 直到 5 秒窗口耗尽被系统强杀），日志一律走 shutdown-trace.log 落盘。
std::atomic<bool> g_console_closing{false};

/// 关机链完成信号（点 X 场景）：GracefulShutdown::finish_shutdown 完成后
/// notify_shutdown_complete()；CTRL_CLOSE handler 阻塞在 cv 上等待，
/// 链完成才 ExitProcess(0)。详见 hpp 声明处的实测依据。
std::mutex g_shutdown_done_mutex;
std::condition_variable g_shutdown_done_cv;
bool g_shutdown_done = false;

} // namespace

bool console_closing() {
    return g_console_closing.load();
}

void notify_shutdown_complete() {
    std::lock_guard<std::mutex> lock(g_shutdown_done_mutex);
    g_shutdown_done = true;
    g_shutdown_done_cv.notify_all();
}

bool wait_for_shutdown_complete(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(g_shutdown_done_mutex);
    return g_shutdown_done_cv.wait_for(lock, timeout,
                                       [] { return g_shutdown_done; });
}

std::mutex& trace_file_mutex() {
    static std::mutex m;
    return m;
}

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
    // 哨兵状态走统一日志（logging_service > CAF log > cout）。
    LOG_INFO("[Watchdog] stdin EOF watchdog armed (pipe stdin)");
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

/// DLL 分类目录支持：第三方依赖 DLL 可放 exe_dir/lib/ 子目录（分类存放），
/// 经 AddDllDirectory 注册进搜索路径。注意边界：
///   - exe 启动时【直接链接】的 DLL（CAF/fmt/spdlog 等）由进程加载器解析，
///     只搜 exe 目录/系统/PATH——必须留在 exe 同级，本机制管不到；
///   - 运行时 LoadLibrary 的 DLL（插件及其依赖，如 hiredisd.dll）在加载
///     时刻才解析依赖，AddDllDirectory 注册的目录全部生效。
/// SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS) 让普通
/// LoadLibrary 也走 USER_DIRS（AddDllDirectory 注册的目录），且保留
/// exe 目录优先（旧布局兼容）。
void setup_dll_search_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0)
        return;
    std::filesystem::path exe_dir = std::filesystem::path(buf).parent_path();
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    AddDllDirectory(exe_dir.c_str());              // 兼容旧布局（DLL 在 exe 同级）
    AddDllDirectory((exe_dir / L"lib").c_str());   // 分类目录
#endif
}

#ifdef _WIN32
// 信号落盘证据：点 X 时控制台正在销毁，fprintf(stderr) 可能写不进（句柄
// 关闭中），且用户看不到——写文件是唯一可靠证据。与 graceful_shutdown.cpp
// 的 shutdown-trace.log 同一文件（追加，进程工作目录 = exe 目录）。
// 带时间戳：区分"用户点 X 晚"vs"handler 之后处理慢"（曾误判为关机链卡）。
// 实现用 Windows API 显式共享（同 trace_shutdown，见其注释：MSVC ofstream
// 并发打开会阻塞——handler 线程卡住会拖死 shutdown_mgr 的 trace 写入）。
void trace_signal(const char* msg) {
    std::lock_guard<std::mutex> lock(caf_plugin_system::trace_file_mutex());
#ifdef _WIN32
    HANDLE h = CreateFileA("shutdown-trace.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    std::string line = "[" + std::string(buf) + "] [signal] " + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
#else
    std::ofstream f("shutdown-trace.log", std::ios::app);
    if (!f)
        return;
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    f << "[" << buf << "] [signal] " << msg << std::endl;
#endif
}

BOOL WINAPI console_handler(DWORD signal) {
    // 顺序铁律：先发 shutdown_atom 再打印——打印只是诊断（控制台销毁时
    // 可能写不进/阻塞），绝不能让输出阻塞关机信号。
    // 若按了 Ctrl+C 但窗口无此输出 = 被 QuickEdit 选中模式劫持。
    // CTRL_CLOSE_EVENT：用户点窗口 X / 控制台关闭——默认行为是
    // TerminateProcess（拉电闸），注册 handler 后系统给约 5 秒窗口期，
    // 期间走完整优雅关机链（保存数据），超时才被系统兜底强杀。
    // CTRL_LOGOFF/SHUTDOWN_EVENT：系统注销/关机，同样走优雅链。
    switch (signal) {
    case CTRL_C_EVENT:
        if (shutdown_manager_ref())
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        fprintf(stderr, "[Signal] Ctrl+C received, triggering graceful shutdown\n");
        return TRUE;
    case CTRL_BREAK_EVENT:
        if (shutdown_manager_ref())
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        fprintf(stderr, "[Signal] Ctrl+Break received, triggering graceful shutdown\n");
        return TRUE;
    case CTRL_CLOSE_EVENT:
        // 控制台销毁中：先设标志（后续 std::cout 全跳过，防止写销毁中的
        // 控制台句柄阻塞拖死关机链）+ 落盘 + 发信号，stderr 打印放最后。
        g_console_closing = true;
        // 终极兜底：fd 层面重定向 stdout/stderr → NUL。销毁中的控制台句柄
        // 写入会永久阻塞——spdlog 的 stdout sink 持有构造时的 FILE*（freopen
        // 改全局 stdout 对已捕获指针无效），但所有 FILE* 共享底层 fd：
        // _dup2 换掉 fd 1/2 后任何写入（spdlog fwrite / cout / printf /
        // fprintf）都落 NUL 立即返回，物理上杜绝阻塞。
        // 注意：插件 DLL 无法感知 exe 的 g_console_closing（静态库复制），
        // 只能靠这招全局兜底。
        {
            int nul = _open("NUL", _O_WRONLY);
            if (nul >= 0) {
                _dup2(nul, 1);
                _dup2(nul, 2);
                _close(nul);
            }
        }
        trace_signal("CTRL_CLOSE_EVENT: close button (X) pressed");
        if (shutdown_manager_ref()) {
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
            trace_signal("shutdown_atom sent to shutdown_mgr");
        } else {
            trace_signal("shutdown_manager_ref() EMPTY - cannot send shutdown");
        }
        // 铁律（2026-08-23 实测）：CTRL_CLOSE 的 handler 绝不能立即返回！
        // conhost 在 handler 返回后 ~0ms 就 TerminateProcess（0xC000013A），
        // 异步关机链根本没时间跑——此前所有"点 X 拉闸"的根因。只有 handler
        // 阻塞时 conhost 才给约 5 秒宽限（对照实验：阻塞 8s 存活 4.6s）。
        // 因此：阻塞等关机链完成（graceful_shutdown 的 finish_shutdown 会
        // notify），然后 ExitProcess(0) 主动退出——数据已保存，退出码干净。
        // 超时（链异常卡住）也 ExitProcess 兜底，总比被 conhost 强杀体面。
        if (wait_for_shutdown_complete(std::chrono::seconds(4))) {
            trace_signal("shutdown chain completed; ExitProcess(0)");
        } else {
            trace_signal("shutdown chain TIMEOUT 4s; ExitProcess(0) fallback");
        }
        fprintf(stderr, "[Signal] Console close: graceful shutdown done\n");
        ExitProcess(0);
    case CTRL_LOGOFF_EVENT:
        trace_signal("CTRL_LOGOFF_EVENT: user logoff");
        if (shutdown_manager_ref())
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        // 同 CTRL_CLOSE：阻塞等链完成再退出（系统注销窗口期同样有限）。
        if (wait_for_shutdown_complete(std::chrono::seconds(4))) {
            trace_signal("logoff: chain completed; ExitProcess(0)");
        } else {
            trace_signal("logoff: chain TIMEOUT 4s; ExitProcess(0) fallback");
        }
        fprintf(stderr, "[Signal] User logoff, exiting\n");
        ExitProcess(0);
    case CTRL_SHUTDOWN_EVENT:
        trace_signal("CTRL_SHUTDOWN_EVENT: system shutdown");
        if (shutdown_manager_ref())
            caf::anon_send(shutdown_manager_ref(), shutdown_atom{});
        if (wait_for_shutdown_complete(std::chrono::seconds(4))) {
            trace_signal("system shutdown: chain completed; ExitProcess(0)");
        } else {
            trace_signal("system shutdown: chain TIMEOUT 4s; ExitProcess(0) fallback");
        }
        fprintf(stderr, "[Signal] System shutdown, exiting\n");
        ExitProcess(0);
    default:
        return FALSE;
    }
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
    // DLL 分类目录（exe_dir/lib/）必须先于 preregister_plugin_meta 注册：
    // 预注册阶段的 LoadLibrary 同样要能解析 run/lib 下的第三方依赖，
    // 否则非 delayload 依赖的插件在预注册阶段会加载失败（静默跳过）。
    // 幂等——bootstrap_system_components 里还有一次兜底调用。
    setup_dll_search_path();
    // 插件私有消息类型的元对象：扫描插件目录并调用可选导出
    // register_meta_objects()。注册了元对象的插件 DLL 自此常驻。
    preregister_plugin_meta(plugins_dir);

    // CAF 1.1 框架日志配置：console 关闭（单一写者原则——console 只允许
    // logging_service 的 spdlog 写，否则与 CAF logger 双写者并发 → 乱序），
    // 文件 info 级（debug 全是 caf.core 调度噪音，曾实测 1.2MB 里 99%
    // 无用；info 保留启动/加载/关机诊断 + 错误）。
    set("caf.logger.console.verbosity", "quiet");
    set("caf.logger.file.verbosity", "info");
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
        .add(redis_uris, "redis-uris",
             "redis plugin connections: name1=uri1,name2=uri2 (uri=redis://host:port/db)")
        .add(mysql_uris, "mysql-uris",
             "mysql plugin connections: name1=uri1,name2=uri2 (uri=mysql://user:pass@host:port/dbname)")
        .add(pg_uris, "pg-uris",
             "postgres plugin connections: name1=uri1,name2=uri2 (uri=postgres://user:***@host:port/dbname)")
        .add(mongo_uris, "mongo-uris",
             "mongo plugin connections: name1=uri1,name2=uri2 (uri=mongodb://[user:pass@]host[:port][/dbname])")
        .add(db_pool_size, "db-pool-size",
             "sql plugin connection pool size per named connection (default 2)")
        .add(test_cross_call, "test-cross-call",
             "cross-node call this service after node registration (cluster test)")
        .add(test_cross_call_ex, "test-cross-call-ex",
             "cross-node call this service with bounded retry (15x1s) after node registration")
        .add(test_remote_reload, "test-remote-reload",
             "master: remote hot-reload <node>,<plugin>,<path> after node registration (ops test)")
        .add(test_quit, "test-quit",
             "trigger ops quit after startup (verify graceful shutdown, process must exit 0)")
        .add(test_ctrl_c, "test-ctrl-c",
             "send shutdown_atom directly after startup (simulate Ctrl+C path, verify exit 0)")
        .add(test_lua_script, "test-lua-script",
             "verify Lua script plugin: resolve echo_service and call envelope+string")
        .add(test_py_script, "test-py-script",
             "verify Python script plugin: resolve echo_service and call envelope+string")
        .add(test_ts_script, "test-ts-script",
             "verify TypeScript script plugin: resolve echo_service and call envelope+string")
        .add(time_offset, "time-offset",
             "global business time offset in seconds (test mode; business_now() = now + offset)")
        .add(test_time_offset, "test-time-offset",
             "verify time service: business_now() - now == configured offset after startup")
        .add(bridge_port, "bridge-port",
             "bridge sidecar TCP port for external-language nodes (0 = disabled)")
        .add(test_bridge_call, "test-bridge-call",
             "master: cross-node call external_echo on this node after startup (bridge test)");
}

bool bootstrap_system_components(caf::actor_system& sys,
                                 const framework_config& cfg,
                                 BootstrapResult& out) {
    // DLL 分类目录（exe_dir/lib/）注册：必须在任何 LoadLibrary 之前
    //（插件扫描在 bootstrap_plugins 里，此处早于它）
    setup_dll_search_path();
    // ---- 日志服务最先 spawn（进程第一个 actor）----
    // 系统组件（registry/shutdown_mgr 等）从创建起就要打日志；插件也依赖
    // "logging_service" 核心内置服务。spdlog 在此创建 sinks 并常驻到关机链
    // 最后（finish_shutdown 最后 send_exit 它，exit_msg 里 flush 再 quit）。
    out.logging_service = spawn_logging_service(sys);
    // 日志单例已在 spawn 内注册（logging_service() 访问器），启动期诊断全进 app.log
    LOG_INFO("[Bootstrap] logging service up");

    // ---- 统一时间源：全局业务时间偏移注入（必须早于任何业务取时）----
    // time_service 是头文件级单例（include/services/time_service.hpp），
    // 非 actor：快路径 business_now() inline 读原子变量零开销。偏移来自
    // 配置（--time-offset / caf-application.conf），多节点集群测试时各
    // 节点配置同一值即天然一致。非零必告警，防止忘改回 0 误上生产。
    set_time_offset(std::chrono::seconds{cfg.time_offset});
    if (cfg.time_offset != 0) {
        std::cout << "[WARN] TIME OFFSET = +" << cfg.time_offset
                  << "s (TEST MODE)" << std::endl;
        LOG_WARN("[Bootstrap] TIME OFFSET = +{}s (TEST MODE)", cfg.time_offset);
    }

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
        out.logging_service, get_stop_order);
    shutdown_manager_ref() = out.shutdown_mgr;
    sys.registry().put("shutdown_mgr", out.shutdown_mgr);
    // 暴露 ServiceRegistry 句柄：插件可查询服务清单（如 cluster_admin 的
    // system.services）。与 shutdown_mgr 同款模式（见 plugin_interface.hpp
    // 的注入说明：sys.registry().get("service_registry")）。
    sys.registry().put("service_registry", out.registry);

    // 保证 Ctrl+C 语义 = 中断信号（QuickEdit 会劫持成复制）
    disable_quick_edit();
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif

    caf::scoped_actor self{sys};

    // 核心内置服务注册：插件 manifest 依赖可直接引用 logging_service。
    // ServiceRegistry 会为它建 proxy + 镜像到 CAF sys.registry()，
    // 插件 deps[0]（proxy）与核心 current_logger()（原始 actor）殊途同归。
    self->send(out.registry, register_atom{}, "logging_service",
               out.logging_service, "@core");   // 虚拟的插件 @core ，它并不是一个真实的 DLL 插件

    // 组件就绪（任何进程都 ready：纯节点无 bootstrap_plugins 不会发 ready，
    // 否则 quit 时 GracefulShutdown 走 "Not ready, forcing exit" 强杀路径）
    self->send(out.shutdown_mgr, ready_atom{});

    LOG_INFO("[Bootstrap] system components ready");
    return true;
}

bool bootstrap_plugins(caf::actor_system& sys, const framework_config& cfg,
                       BootstrapResult& out) {
    caf::scoped_actor self{sys};

    // ---- 第 1 步：入口插件 ----
    if (cfg.entry_plugins.empty()) {
        return true;  // 无插件：合法（纯节点/纯组件进程）
    }

    LOG_INFO("Entry plugins:" + [&]() {
        std::string s;
        for (const auto& name : cfg.entry_plugins) s += " " + name;
        return s;
    }());

    // ---- 第 2 步：扫描 ----
    auto all_plugins = scan_all_plugins(cfg.plugins_dir);
    if (all_plugins.empty()) {
        LOG_ERROR("No plugins found in " + cfg.plugins_dir);
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    LOG_INFO("Scanned " + std::to_string(all_plugins.size())
                + " plugin(s) in " + cfg.plugins_dir);

    // ---- 第 3 步：依赖解析 ----
    auto required = resolve_dependencies(cfg.entry_plugins, all_plugins);
    if (required.empty()) {
        LOG_ERROR("Failed to resolve dependencies");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    LOG_INFO("Resolved " + std::to_string(required.size())
                + " plugin(s) to load:" + [&]() {
        std::string s;
        for (const auto& p : required) s += " " + p.name;
        return s;
    }());

    // ---- 第 4 步：拓扑排序 ----
    auto load_order = compute_load_order(required);
    if (load_order.empty()) {
        LOG_ERROR("Circular dependency detected");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    LOG_INFO("Load order:" + [&]() {
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
            .receive([](bool ok) { LOG_INFO("Load result: " + std::string(ok ? "OK" : "FAILED")); },
                     [](const caf::error& e) { LOG_ERROR("Load err: " + caf::to_string(e)); });
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
        LOG_ERROR("One or more plugins failed to resolve");
        self->send(out.shutdown_mgr, shutdown_atom{});
        return false;
    }

    self->send(out.shutdown_mgr, ready_atom{});
    LOG_INFO("Startup complete. Press Ctrl+C to shutdown.");
    return true;
}

void wait_for_shutdown(caf::actor_system& sys, const BootstrapResult& fw) {
    // 信号处理已在 bootstrap_system_components 注册（console_handler /
    // signal_handler），不重复注册（SetConsoleCtrlHandler 重复注册会让
    // handler 被调用两次 → 双 shutdown_atom）。
    install_stdin_watchdog(fw.shutdown_mgr);
    caf::scoped_actor self{sys};
    self->wait_for(fw.shutdown_mgr);
    LOG_INFO("framework shutdown complete");
}

} // namespace caf_plugin_system
