#pragma once
// ------------------------------------------------------------------
// 插件框架一站式引导（多进程复用）
//
// 每个基于 caf-plugin-system 的进程只需：
//
//   void caf_main(caf::actor_system& sys, const framework_config& cfg) {
//       BootstrapResult fw;
//       if (!bootstrap_plugin_framework(sys, cfg, fw)) return;  // 失败已触发关机
//       // ... 业务：用 fw.registry / fw.plugin_mgr / fw.shutdown_mgr ...
//       wait_for_shutdown(sys, fw);
//   }
//   CAF_MAIN()
//
// bootstrap_plugin_framework 完成：内核 actor 创建（registry/checkpoint/
// plugin_mgr/graceful shutdown）→ 信号处理注册 → 扫描/依赖解析/拓扑排序/
// 加载/健康检查 → ready。默认关机顺序 = 加载反序（依赖者先停）。
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

/// 完整引导：创建内核 actor、注册信号处理、扫描/解析/拓扑/加载/健康检查、
/// 通知 shutdown_mgr ready。失败时已向 shutdown_mgr 发起关机，返回 false。
bool bootstrap_plugin_framework(caf::actor_system& sys,
                                const framework_config& cfg,
                                BootstrapResult& out);

/// 阻塞直到关机流程完成（Ctrl+C / 插件请求 / 故障路径触发）。
void wait_for_shutdown(caf::actor_system& sys, const BootstrapResult& fw);

} // namespace caf_plugin_system
