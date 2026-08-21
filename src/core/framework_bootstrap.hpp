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

    // ---- 集群节点模式（空 = 纯插件进程，不开节点）----
    /// 节点角色："master" / "region" / "worker"（--node-kind）
    std::string node_kind = "";
    /// 节点名（全局唯一，--node-name）
    std::string node_name = "";
    /// 注册用 host（--node-host，默认 127.0.0.1）
    std::string node_host = "127.0.0.1";
    /// middleman 监听端口，0 = 自动分配（--node-port）
    uint16_t node_port = 0;
    /// master 地址（--master-host / --master-port，worker/region 必填）
    std::string master_host = "127.0.0.1";
    uint16_t master_port = 0;
    /// lease 秒数，0 = 永不过期（--lease-seconds，默认 10）
    int lease_seconds = 10;
    /// 父节点名（--parent，region/worker 挂到哪个子树下，空 = 直属 master）
    std::string parent = "";

    framework_config();
};

/// 引导结果：内核 actor 句柄 + 最终加载顺序（含解析出的依赖）。
struct BootstrapResult {
    caf::actor registry;
    caf::actor checkpoint_mgr;
    caf::actor plugin_mgr;
    caf::actor shutdown_mgr;
    /// 本进程的 master 注册表 actor（node-kind=master 时非空）
    caf::actor cluster_master;
    /// 本进程的节点客户端 actor（非 master 节点模式时非空）
    caf::actor node_client;
    /// 实际监听的 middleman 端口（0 = 未开端口）
    uint16_t node_port = 0;
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
