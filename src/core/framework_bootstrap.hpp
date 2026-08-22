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

    framework_config();
};

/// 引导结果：内核 actor 句柄 + 最终加载顺序（含解析出的依赖）。
struct BootstrapResult {
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
void wait_for_shutdown(caf::actor_system& sys, const BootstrapResult& fw);

/// stdin 管道 EOF 哨兵（WSL interop）：Ctrl+C 只杀 bash，exe 变孤儿；
/// 父进程/终端消失 → 管道写端关闭 → EOF → 发 shutdown_atom 给 shutdown_mgr
/// 触发统一关机（优雅关机链由 shutdown_mgr 全权负责）。
/// 仅监视 FIFO stdin；控制台/重定向/devnull 不监视。1.5s 宽限防误触发；
/// 只有"读过数据后 EOF"才触发（空管道不误报，后台启动不误关机）。
void install_stdin_watchdog(caf::actor shutdown_mgr);

} // namespace caf_plugin_system
