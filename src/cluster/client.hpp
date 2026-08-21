#pragma once
// ------------------------------------------------------------------
// 节点客户端 actor —— 非 master 节点的集群接入
//
// 移植自 warehouse-backend/distributed-nodes 的 cluster + node_heartbeat：
// 状态机：connect master → 注册（带重试）→ 周期性心跳续租；
// master 不可达时延迟重连（自愈），心跳失败不退出。
// ------------------------------------------------------------------

#include "common/cluster_types.hpp"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <chrono>
#include <string>

namespace caf_plugin_system { namespace cluster {

/// 节点客户端配置（从 framework_config 的节点选项组装）。
struct node_client_config {
  std::string node_name;
  node_kind kind = node_kind::worker;
  std::string host = "127.0.0.1";
  uint16_t port = 0;
  std::string parent;
  std::vector<std::string> exported_actors;
  std::string master_host = "127.0.0.1";
  uint16_t master_port = 0;
  std::chrono::seconds lease_ttl{10};
  /// master 注册表 actor 的命名（master 进程 registry 中的名字）
  std::string master_registry_name = "cluster.master";
};

/// 解析节点角色字符串（空字符串返回 false）。
bool parse_node_kind(const std::string& s, node_kind& out);

/// spawn 节点客户端 actor（返回 actor 句柄；注册/心跳全程后台自愈）。
caf::actor spawn_node_client(caf::actor_system& sys,
                             node_client_config config,
                             caf::actor local_monitor);

} } // namespace caf_plugin_system::cluster
