#pragma once
// ------------------------------------------------------------------
// 插件框架引导 + 统一关机（多进程复用）
//
// 每个基于 caf-plugin-system 的进程只需：
//
//   void caf_main(caf::actor_system& sys, const framework_config& cfg) {
//       BootstrapResult fw;
//       if (!bootstrap_system_components(sys, cfg, fw)) return;
//       if (!cfg.entry_plugins.empty()) {
//           if (!bootstrap_plugins(sys, cfg, fw)) return;   // 失败已触发关机
//       }
//       // ... 业务：fw.registry / fw.plugin_mgr / fw.shutdown_mgr ...
//       wait_for_shutdown(sys, fw);
//   }
//   CAF_MAIN()
//
// 引导分两步（任何进程都执行系统组件；插件加载可选）：
//   1. bootstrap_system_components：spawn registry/checkpoint_mgr/
//      plugin_mgr/shutdown_mgr + 信号处理注册 + 组件 ready。
//   2. bootstrap_plugins（entry-plugins 非空时）：扫描/依赖解析/拓扑排序/
//      加载/健康检查 → ready。默认关机顺序 = 加载反序（依赖者先停）。
//
// 关机统一由 shutdown_mgr（GracefulShutdown）负责：
//   停插件（drain→save→shutdown）→ 杀集群节点（master/client，经
//   register_cluster_atom 注册）→ 杀组件（plugin_mgr/registry/checkpoint）。
// 所有触发（Ctrl+C / stdin EOF / ops quit / 插件请求 / 故障路径）都发
// shutdown_atom 给它，main 只等 wait_for_shutdown 返回，进程自然退出。
// ------------------------------------------------------------------

#include <caf/actor_system_config.hpp>

#include "pomelo_bridge.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

class caf::actor_system;

namespace caf_plugin_system {

/// 框架级配置：CAF 选项 + 自定义插件选项 + 元对象注册窗口。
/// 必须在任何 actor_system 构造前创建（注册窗口在构造函数内）。
struct framework_config : caf::actor_system_config {
    /// 入口插件（--entry-plugins / -e，可重复或逗号分隔）
    std::vector<std::string> entry_plugins;
    /// 显式关机顺序（--shutdown-order / -s）；空 = 加载反序
    std::vector<std::string> shutdown_order;
    /// 插件扫描目录（--plugins-dir / -p，默认 ./plugins）
    std::string plugins_dir = "./plugins";
    /// 启动完成后自动触发优雅关机（冒烟测试后门）
    bool test_auto_shutdown = false;
    /// stdin EOF 时无条件触发关机（--exit-on-stdin-eof）：脚本/CI 场景
    /// `prog < input` 跑完输入即优雅退出。默认 false = 仅"读过数据后
    /// EOF"才触发（空管道不误关机，见 install_stdin_watchdog）。
    bool exit_on_stdin_eof = false;
    /// 跨节点调用信任开关：为 true 时远端节点 sender 可绕过服务代理 ACL
    /// 白名单（集群内互信；默认 false = ACL 只管本地，跨节点一律拦截）
    bool allow_cross_node = false;
    /// 集群验证后门：节点注册完成后跨节点调用该服务（resolve→lookup→call）
    std::string test_cross_call;
    /// 集群验证后门：跨节点调用 + 有界重试（--test-cross-call-ex=<服务名>，
    /// 15 次尝试 × 1s 间隔；配合启动延迟可验证重启窗口期的调用不丢失）
    std::string test_cross_call_ex;
    /// 运维验证后门：master 启动后远程热更（--test-remote-reload=<node>,<plugin>,<path>，
    /// 逗号分隔；复用控制台 reload-node 命令路径，发本机 OpsActor）
    std::string test_remote_reload;
    /// 运维验证后门：启动后自动触发 ops quit（验证优雅关机链自然退出，
    /// 进程必须自己 EXIT 0，不能靠外部强杀）
    bool test_quit = false;
    /// 运维验证后门：启动后直接发 shutdown_atom 给 shutdown_mgr
    /// （模拟 Ctrl+C 路径——不经 ops，验证 ops 注册后也能自然退出）
    bool test_ctrl_c = false;
    /// 脚本插件验证后门：启动后 resolve echo_service 并调 envelope + string
    ///（--test-lua-script；需 entry-plugins 含 LuaHostPlugin + scripts/echo.lua）
    bool test_lua_script = false;
    /// 脚本插件验证后门（--test-py-script；需 entry-plugins 含 PythonHostPlugin）
    bool test_py_script = false;
    /// 脚本插件验证后门（--test-ts-script；需 entry-plugins 含 TsHostPlugin）
    bool test_ts_script = false;
    /// bridge 模式：外部语言节点（Python/Go）sidecar 的本地 TCP 监听端口
    ///（0 = 禁用）。bridge 以正常集群节点身份注册，external_echo 服务
    /// 的 handler 在外部进程（行协议，见 src/core/bridge_actor.hpp）。
    std::uint16_t bridge_port = 0;
    /// Pomelo 协议端点端口（0 = 禁用）：兼容 Pomelo 游戏客户端
    ///（pomelo-protocol 0.1.6，完整握手链 + REQUEST/NOTIFY 翻译）。
    /// 与行协议 bridge 并存（见 src/core/pomelo_bridge.hpp）。
    std::uint16_t pomelo_port = 0;
    /// Pomelo route 映射表：外部 route 字符串 → "svc:function"（内部
    /// MFA）。外部只见 route 别名，内部函数名永不上线；未知 route 拒绝。
    /// conf 格式：pomelo-routes = { "connector.entryHandler.entry" =
    /// "echo_service:hello" }
    pomelo_route_table pomelo_routes;
    /// 集群验证后门：master 延迟后跨节点调用指定节点的 external_echo
    ///（--test-bridge-call=<节点名>，验证集群→外部进程链路）。
    std::string test_bridge_call;
    /// 数据库插件配置（同一配置文件、字段区分）：
    /// Redis 连接表（--redis-uris / caf-application.conf 的 redis-uris 字段），
    /// 格式 "name1=uri1,name2=uri2"（逗号分隔、等号配对），
    /// uri = redis://host:port/db。插件 spawn 时经 sys.config() 读取。
    std::string redis_uris = "default=redis://127.0.0.1:6379";
    /// MySQL 连接表（libmariadb），uri = mysql://user:pass@host:port/dbname
    ///（user/pass/dbname 可省略，默认 root/空/空库）
    std::string mysql_uris = "default=mysql://root@127.0.0.1:3306";
    /// PostgreSQL 连接表（libpq），uri = postgres://user:pass@host:port/dbname
    std::string pg_uris = "default=postgres://postgres@127.0.0.1:5432";
    /// MongoDB 连接表（mongo-cxx-driver），uri = mongodb://[user:pass@]host[:port][/dbname]
    std::string mongo_uris = "default=mongodb://127.0.0.1:27017";
    /// SQL 插件连接池大小（每个命名连接的 worker/连接数，全局统一）
    int db_pool_size = 2;
    /// 全局业务时间偏移（秒，--time-offset；默认 0 = 真实时间）。
    /// 统一时间源：启动时注入 time_service 全局原子，业务代码读
    /// business_now()。测试未来时间用，不改机器时钟。非零时启动打
    /// [WARN] TIME OFFSET ... (TEST MODE) 告警防误上生产。
    std::int64_t time_offset = 0;
    /// 时间服务验证后门（--test-time-offset）：启动后校验
    /// business_now() - 真实 now == 配置偏移，打印 [TimeTest] 结果。
    bool test_time_offset = false;
    /// 运行期卸载验证后门（--test-unload=<插件名>）：启动后 request
    /// PluginManager unload_atom，走完整退役链（quiesce → save_state
    /// 屏障 → unregister → 统一解绑广播 → 旧 actor 退役），验证广播。
    std::string test_unload;

    /// 任一验证后门标志被设置（--test-*）。caf_main 据此调用 exe 侧注册的
    /// 测试后门（set_test_backdoor，2026-08-31 从独立 DLL 移回 exe 编译）。
    bool has_test_flags() const {
        return test_auto_shutdown || test_quit || test_ctrl_c
            || test_time_offset || test_lua_script || test_py_script
            || test_ts_script || !test_cross_call.empty()
            || !test_cross_call_ex.empty() || !test_remote_reload.empty()
            || !test_bridge_call.empty() || !test_unload.empty();
    }

    framework_config();
};

/// 引导结果：内核 actor 句柄 + 最终加载顺序（含解析出的依赖）。
struct BootstrapResult {
    /// 核心日志服务（系统组件，最先 spawn）：spdlog console + logs/app.log，
    /// 注册进 ServiceRegistry（插件依赖可引用），关机链最后退出。
    caf::actor logging_service;
    caf::actor registry;
    caf::actor checkpoint_mgr;
    caf::actor plugin_mgr;
    caf::actor shutdown_mgr;
    /// 按加载顺序的插件名；空 = 引导失败
    std::vector<std::string> load_order;
};

/// 系统级组件引导（任何进程都执行）：spawn registry/checkpoint_mgr/plugin_mgr/
/// shutdown_mgr、注册信号处理、关联 shutdown_mgr。返回 false = spawn 失败。
bool bootstrap_system_components(caf::actor_system& sys,
                                 const framework_config& cfg,
                                 BootstrapResult& out);

/// 插件加载（可选；entry-plugins 非空时调用）：扫描/依赖解析/拓扑排序/
/// 按序加载/健康检查，成功通知 shutdown_mgr ready。失败已发起关机，返回 false。
bool bootstrap_plugins(caf::actor_system& sys, const framework_config& cfg,
                       BootstrapResult& out);

/// 阻塞直到关机完成（Ctrl+C / 插件请求 / 故障路径触发）。
/// 注意：集群节点（master/client）也由 shutdown_mgr 统一终止（经
/// register_cluster_atom 注册），main 只等 shutdown_mgr 退出即可。
void wait_for_shutdown(caf::actor_system& sys, const BootstrapResult& fw,
                       const framework_config& cfg);

/// TODO(leakfix-probe): 清空 shutdown_manager_ref() 静态持有——shutdown_mgr
/// actor 引用被进程级单例持有，DLL 常驻不析构 → 对象树级联泄漏。
void clear_shutdown_manager_ref();

/// stdin 管道 EOF 哨兵（WSL interop）：Ctrl+C 只杀 bash，exe 变孤儿；
/// 父进程/终端消失 → 管道写端关闭 → EOF → 发 shutdown_atom 给 shutdown_mgr
/// 触发统一关机（优雅关机链由 shutdown_mgr 全权负责）。
/// 仅监视 FIFO stdin；控制台/重定向/devnull 不监视。1.5s 宽限防误触发；
/// 默认只有"读过数据后 EOF"才触发（空管道不误报，后台启动不误关机）；
/// cfg.exit_on_stdin_eof = true 时 EOF 无条件触发（脚本 `prog < input`）。
void install_stdin_watchdog(caf::actor_system& sys,
                            const framework_config& cfg);

/// 控制台是否正在销毁（用户点窗口 X 触发 CTRL_CLOSE_EVENT 后为 true）。
/// 关机链的 stdout 输出必须检查此标志：写正在销毁的控制台句柄会阻塞，
/// 拖死关机链直到 5 秒窗口耗尽被系统强杀——日志应优先落盘（shutdown-trace.log）。
bool console_closing();

/// shutdown-trace.log 的跨文件写入锁：graceful_shutdown.cpp 的 trace_shutdown
/// 与 framework_bootstrap.cpp 的 trace_signal 并发写同一文件——MSVC ofstream
/// 默认 FILE_SHARE_READ 共享模式，并发打开会失败/阻塞（曾导致关机链卡住、
/// trace 缺行、5 秒窗口耗尽被强杀）。必须用同一把锁串行化。
std::mutex& trace_file_mutex();

/// 关机链完成信号（点窗口 X 场景专用）：GracefulShutdown::finish_shutdown
/// 在链走完（STOPPED + 数据已落盘）后调用 notify_shutdown_complete()。
///
/// 为什么需要：实测（2026-08-23，控制台关闭按钮 X）CTRL_CLOSE_EVENT 的
/// handler 若立即返回 TRUE，conhost 在 ~0ms 内 TerminateProcess 进程
///（退出码 0xC000013A）——异步关机链根本没时间跑完；只有 handler 阻塞时
/// conhost 才给约 5 秒宽限（对照实验：handler 阻塞 8s，进程存活 4.6s 才被杀）。
/// 因此 CTRL_CLOSE/LOGOFF/SHUTDOWN 的 handler 必须阻塞等关机链完成，然后
/// ExitProcess(0) 主动退出（此时 checkpoints 已落盘，不丢数据）。
void notify_shutdown_complete();

/// 阻塞等待关机链完成（点 X 的 handler 线程调用）；超时返回 false
///（调用方应 ExitProcess 兜底——总比被 conhost 0xC000013A 强杀干净）。
bool wait_for_shutdown_complete(std::chrono::milliseconds timeout);

} // namespace caf_plugin_system
