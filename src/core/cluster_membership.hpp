#pragma once
// ------------------------------------------------------------------
// 集群成员表（monitored node registry）
//
// 移植自 warehouse-backend/distributed-nodes 的 monitored_node_registry。
// master（扁平注册表）与 region（子树成员表）共用同一份实现：
//   - 每个 slot 持有 manifest + monitor_actor（down_msg 感知退出）
//   - lease 过期时间由调用方注入（master/region 各自策略）
// ------------------------------------------------------------------

#include "common/cluster_types.hpp"

#include <caf/event_based_actor.hpp>
#include <caf/actor.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace caf_plugin_system {

using steady_clock_type = std::chrono::steady_clock;

inline steady_clock_type::time_point never_expires() {
  return steady_clock_type::time_point::max();
}

inline steady_clock_type::time_point lease_deadline(std::chrono::seconds ttl) {
  return steady_clock_type::now() + ttl;
}

inline void sort_manifests(std::vector<node_manifest>& v) {
  std::sort(v.begin(), v.end(),
            [](const node_manifest& a, const node_manifest& b) {
              return a.node_name < b.node_name;
            });
}

inline std::string join_strings(const std::vector<std::string>& v) {
  std::string s;
  for (const auto& item : v) {
    if (!s.empty()) s += ",";
    s += item;
  }
  return s;
}

/// 成员表：manifest + monitor + lease 过期，双通道健康检测。
class node_membership {
public:
  struct slot {
    node_manifest manifest;
    caf::actor monitor_actor;
    steady_clock_type::time_point expires_at = never_expires();
  };

  explicit node_membership(caf::event_based_actor* self) : self_(self) {}

  size_t size() const { return slots_.size(); }

  bool contains(const std::string& node_name) const {
    return slots_.find(node_name) != slots_.end();
  }

  /// 插入或更新成员；monitor 变化时自动 demonitor/monitor。
  void upsert(node_manifest manifest, caf::actor monitor_actor,
              std::chrono::seconds lease_ttl) {
    auto node_name = manifest.node_name;
    auto& s = slots_[node_name];
    auto same_monitor = s.monitor_actor && monitor_actor
                        && s.monitor_actor.address() == monitor_actor.address();
    if (s.monitor_actor && !same_monitor)
      self_->demonitor(s.monitor_actor);
    s.manifest = std::move(manifest);
    s.monitor_actor = monitor_actor;
    s.expires_at = lease_ttl.count() > 0 ? lease_deadline(lease_ttl)
                                         : never_expires();
    if (s.monitor_actor && !same_monitor)
      self_->monitor(s.monitor_actor);
  }

  /// 心跳续租；未知成员返回 false。
  bool touch(const std::string& node_name, std::chrono::seconds lease_ttl) {
    auto it = slots_.find(node_name);
    if (it == slots_.end())
      return false;
    it->second.expires_at = lease_ttl.count() > 0 ? lease_deadline(lease_ttl)
                                                  : never_expires();
    return true;
  }

  bool erase(const std::string& node_name, bool demonitor_actor) {
    auto it = slots_.find(node_name);
    if (it == slots_.end())
      return false;
    erase(it, demonitor_actor);
    return true;
  }

  /// 按 monitor 句柄删除（down_msg 路径）；返回是否命中。
  template <class LogFn>
  bool erase_by_monitor(const caf::actor_addr& source,
                        LogFn on_erase) {
    for (auto it = slots_.begin(); it != slots_.end(); ++it) {
      if (!it->second.monitor_actor)
        continue;
      if (it->second.monitor_actor.address() != source)
        continue;
      on_erase(it->second.manifest);
      erase(it, false);
      return true;
    }
    return false;
  }

  /// 清理过期成员（跳过 skip 名单，如 master 自身）。
  template <class SkipFn, class LogFn>
  void prune_expired(steady_clock_type::time_point now, SkipFn skip,
                     LogFn on_erase) {
    std::vector<std::string> expired;
    for (const auto& [name, s] : slots_) {
      if (skip(s.manifest))
        continue;
      if (s.expires_at <= now)
        expired.push_back(name);
    }
    for (const auto& name : expired) {
      auto it = slots_.find(name);
      if (it == slots_.end())
        continue;
      on_erase(it->second.manifest);
      erase(it, true);
    }
  }

  template <class Fn>
  void for_each_manifest(Fn fn) const {
    for (const auto& [_, s] : slots_)
      fn(s.manifest);
  }

  const node_manifest* find_manifest(const std::string& node_name) const {
    auto it = slots_.find(node_name);
    return it == slots_.end() ? nullptr : &it->second.manifest;
  }

private:
  using slots_map = std::unordered_map<std::string, slot>;

  void erase(slots_map::iterator it, bool demonitor_actor) {
    if (demonitor_actor && it->second.monitor_actor)
      self_->demonitor(it->second.monitor_actor);
    slots_.erase(it);
  }

  caf::event_based_actor* self_;
  slots_map slots_;
};

} // namespace caf_plugin_system
