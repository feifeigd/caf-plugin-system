#pragma once
// ------------------------------------------------------------------
// ClusterMaster —— master 扁平注册表 actor
//
// 移植自 warehouse-backend/distributed-nodes 的 master_state：
// 所有节点在此注册 node_manifest（parent 是数据字段），维护
// lease + monitor 双通道健康检测，提供拓扑/子树/路由查询。
// 任意进程 spawn 它即成为 master（bootstrap 按节点配置决定）。
// ------------------------------------------------------------------

#include "cluster_membership.hpp"

#include <caf/event_based_actor.hpp>
#include <caf/stateful_actor.hpp>

#include <chrono>
#include <string>

namespace caf_plugin_system {

/// master 注册表 actor 的 state（stateful_actor<ClusterMasterState>）。
class ClusterMasterState {
public:
  explicit ClusterMasterState(caf::event_based_actor* self,
                              node_manifest self_manifest,
                              std::chrono::seconds lease_ttl);

  /// 返回完整 behavior（注册/心跳/注销/查询/维护）。
  caf::behavior make_behavior();

private:
  /// 维护周期 tick：清理 lease 过期成员。
  void schedule_maintenance();
  void prune_expired();
  void upsert(node_manifest manifest, caf::actor monitor_actor);
  bool touch(const std::string& node_name);

  caf::event_based_actor* self;
  std::chrono::seconds lease_ttl;
  node_membership nodes;
};

/// 以 stateful_actor 方式 spawn：sys.spawn(actor_from_state<ClusterMasterState>, ...)
caf::actor spawn_cluster_master(caf::actor_system& sys,
                                node_manifest self_manifest,
                                std::chrono::seconds lease_ttl);

} // namespace caf_plugin_system
