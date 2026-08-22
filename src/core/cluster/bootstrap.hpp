#pragma once
// ------------------------------------------------------------------
// 集群节点引导 —— src/distributed-nodes 模块入口
//
// 独立于插件框架（caf_plugin_core）：本模块只依赖 CAF(core+io)
// 与 common 协议类型。任何进程想成为集群节点只需：
//
//   struct app_config : caf::actor_system_config {
//       app_config() {
//           cluster::init_node_io(*this);           // middleman 元对象+加载
//           cluster::add_node_options(*this, node_cfg);
//       }
//       cluster::node_settings node_cfg;
//   };
//
//   void caf_main(caf::actor_system& sys, const app_config& cfg) {
//       cluster::BootstrapResult nb;
//       if (!cluster::bootstrap_node(sys, cfg.node_cfg, {}, nb)) return;
//       cluster::wait_for_node_shutdown(sys, nb);
//   }
//   CAF_MAIN()
//
// 拓扑模型（master 扁平 + 任意多子树）：
//   - 所有节点向 master 注册 node_manifest（parent 是数据字段）
//   - master 维护扁平注册表（lease + monitor 双通道健康检测）
//   - region/worker 由节点客户端后台接入（注册/心跳/断线自愈）
// ------------------------------------------------------------------

#include "common/cluster_types.hpp"

#include <caf/actor_system_config.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace caf_plugin_system { namespace cluster {

/// 节点 CLI 选项（注册进 --caf-plugin-system. 组；空 node_kind = 非节点进程）。
struct node_settings {
    std::string node_kind;        ///< "master" / "region" / "worker"（--node-kind）
    std::string node_name;        ///< 全局唯一节点名（--node-name）
    std::string node_host = "127.0.0.1";  ///< 注册用 host（--node-host）
    uint16_t node_port = 0;       ///< middleman 监听端口，0 = 自动（--node-port）
    std::string master_host = "127.0.0.1";  ///< master 地址（--master-host）
    uint16_t master_port = 0;     ///< master 端口（--master-port）
    int lease_seconds = 10;       ///< lease TTL，0 = 永不过期（--lease-seconds）
    std::string parent;           ///< 父节点名（--parent，空 = 直属 master）
    /// 本节点导出到 CAF registry 的服务名（节点注册时上报，master 供
    /// node_resolve 路由）。运行时从 ServiceRegistry 台账自动填充。
    std::vector<std::string> exported_actors;
    /// master 注册表 actor 的命名（跨进程 remote_lookup 用）
    std::string master_registry_name = "cluster.master";

    /// 是否节点模式（node-kind 非空）。
    bool is_node() const { return !node_kind.empty(); }
};

/// 节点引导结果。
struct BootstrapResult {
    caf::actor master;  ///< node-kind=master 时非空（本进程注册表 actor）
    caf::actor client;  ///< 非 master 节点时非空（节点客户端 actor）
    uint16_t port = 0;  ///< middleman 实际监听端口（0 = 未开端口）
};

/// middleman 元对象注册 + 加载模块到 cfg。必须在 actor_system 构造前调用
/// （CAF 禁止后置注册；CAF_MAIN 的模块参数机制做的就是这个）。
void init_node_io(caf::actor_system_config& cfg);

/// 注册节点 CLI 选项到 config（opt_group "caf-plugin-system"，值绑定
/// settings 字段）。必须在 parse 前调用（config 构造函数内）。
void add_node_options(caf::actor_system_config& cfg, node_settings& settings);

/// 节点引导：开 middleman 端口 → master：spawn 注册表 + 命名；
/// region/worker：spawn 客户端接入 master（注册/心跳/自愈后台进行）。
/// local_monitor 非空时随注册上报给 master（如插件框架的 shutdown_mgr，
/// master 监控它感知本进程退出）。返回 false = 配置非法（已打日志）。
/// 注：OpsActor 是系统组件，由 main 统一 spawn（不在此函数内），
/// master 的注册表句柄经 BootstrapResult::master 返回供其注入。
bool bootstrap_node(caf::actor_system& sys, const node_settings& settings,
                    caf::actor local_monitor, BootstrapResult& out);

} } // namespace caf_plugin_system::cluster
