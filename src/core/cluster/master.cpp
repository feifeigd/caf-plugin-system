#include "master.hpp"
#include "common/message_tags.hpp"
#include "services/logging_service.hpp"

#include <caf/actor_from_state.hpp>
#include <caf/logger.hpp>

#include <algorithm>
#include <iostream>

namespace caf_plugin_system { namespace cluster {

namespace {

/// 维护周期：lease 过期检查步长。
constexpr auto k_maintenance_step = std::chrono::seconds(1);

} // namespace

ClusterMasterState::ClusterMasterState(caf::event_based_actor* selfptr,
                                       node_manifest self_manifest,
                                       std::chrono::seconds ttl)
  : self(selfptr), lease_ttl(ttl), nodes(selfptr) {
  upsert(std::move(self_manifest), {});
  schedule_maintenance();
}

caf::behavior ClusterMasterState::make_behavior() {
  return {
    [this](node_register_atom, node_registration registration) {
      prune_expired();
      auto manifest = std::move(registration.manifest);
      auto existed = nodes.contains(manifest.node_name);
      auto node_name = manifest.node_name;
      auto kind = manifest.kind;
      auto parent = manifest.parent.empty() ? "<root>" : manifest.parent;
      upsert(std::move(manifest), std::move(registration.monitor_actor));
      LOG_INFO(std::string("[ClusterMaster] ") + (existed ? "updated" : "registered")
                  + " node '" + node_name + "' (" + to_string(kind)
                  + ") parent=" + parent);
      return register_reply{true, existed ? "node updated" : "node registered"};
    },

    [this](node_heartbeat_atom, const std::string& node_name) {
      prune_expired();
      if (touch(node_name))
        return register_reply{true, "node refreshed"};
      return register_reply{false, "node not found"};
    },

    [this](node_unregister_atom, const std::string& node_name) {
      if (nodes.erase(node_name, true)) {
        LOG_INFO(std::string("[ClusterMaster] unregistered node '") + node_name + "'");
        return register_reply{true, "node removed"};
      }
      return register_reply{false, "node not found"};
    },

    [this](const caf::down_msg& msg) {
      nodes.erase_by_monitor(msg.source, [this, msg](const node_manifest& manifest) {
        LOG_INFO(std::string("[ClusterMaster] node '") + manifest.node_name
                    + "' (" + to_string(manifest.kind) + ") went down: "
                    + caf::to_string(msg.reason));
      });
    },

    [this](maintenance_tick_atom) {
      prune_expired();
      schedule_maintenance();
    },

    [this](node_topology_atom) {
      prune_expired();
      topology_snapshot snapshot;
      nodes.for_each_manifest([&](const node_manifest& m) {
        snapshot.nodes.push_back(m);
      });
      sort_manifests(snapshot.nodes);
      return snapshot;
    },

    [this](node_children_atom, const std::string& parent_name) {
      prune_expired();
      child_snapshot snapshot;
      snapshot.parent = parent_name;
      nodes.for_each_manifest([&](const node_manifest& m) {
        if (m.parent == parent_name)
          snapshot.children.push_back(m);
      });
      sort_manifests(snapshot.children);
      return snapshot;
    },

    // 查找节点的连接地址
    [this](node_resolve_atom, const std::string& node_name,
           const std::string& actor_name) -> caf::result<actor_route> {
      prune_expired();
      auto manifest = nodes.find_manifest(node_name);
      if (!manifest)
        return caf::make_error(caf::sec::no_such_key);
      auto found = std::find(manifest->exported_actors.begin(),
                             manifest->exported_actors.end(), actor_name);
      if (found == manifest->exported_actors.end())
        return caf::make_error(caf::sec::no_such_key);
      
      return actor_route{manifest->node_name, manifest->kind, manifest->host,
                         manifest->port, actor_name, manifest->parent};
    },

    // 按服务名解析：返回导出该服务的全部节点路由（同名服务多副本）。
    // 只回匹配节点，不返回全量拓扑——上千节点时响应体 O(k) 而非 O(N)。
    [this](service_resolve_atom, const std::string& svc)
      -> caf::result<service_route> {
      prune_expired();
      service_route route;
      nodes.for_each_manifest([&](const node_manifest& m) {
        if (std::find(m.exported_actors.begin(), m.exported_actors.end(), svc)
            == m.exported_actors.end())
          return;
        route.routes.push_back(
          actor_route{m.node_name, m.kind, m.host, m.port, svc, m.parent});
      });
      if (route.routes.empty())
        return caf::make_error(caf::sec::no_such_key);
      return route;
    },
  };
}

void ClusterMasterState::schedule_maintenance() {
  self->delayed_send(self, k_maintenance_step, maintenance_tick_atom_v);
}

void ClusterMasterState::prune_expired() {
  if (lease_ttl.count() == 0)
    return;
  nodes.prune_expired(
    steady_clock_type::now(),
    [](const node_manifest& m) { return m.kind == node_kind::master; },
    [this](const node_manifest& m) {
      LOG_INFO(std::string("[ClusterMaster] expired node '") + m.node_name
                  + "' (" + to_string(m.kind) + ")");
    });
}

void ClusterMasterState::upsert(node_manifest manifest,
                                caf::actor monitor_actor) {
  nodes.upsert(std::move(manifest), std::move(monitor_actor), lease_ttl);
}

bool ClusterMasterState::touch(const std::string& node_name) {
  return nodes.touch(node_name, lease_ttl);
}

caf::actor spawn_cluster_master(caf::actor_system& sys,
                                node_manifest self_manifest,
                                std::chrono::seconds lease_ttl) {
  return sys.spawn(caf::actor_from_state<ClusterMasterState>,
                   std::move(self_manifest), lease_ttl);
}

} } // namespace caf_plugin_system::cluster
