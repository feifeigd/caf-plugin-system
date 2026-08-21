#pragma once
// ------------------------------------------------------------------
// 集群节点协议类型（master 扁平注册 + 任意多子树）
//
// 从 warehouse-backend/distributed-nodes 移植的【集群机制】——
// 只移植协议与注册表/心跳逻辑，类型注册机制完全沿用本项目：
// 类型经 message_tags.def 显式 ID 注册（手写 meta，跨 DLL 一致），
// 不走 CAF_ADD_TYPE_ID（其静态注册时机在 DLL 动态加载场景下撞 UB）。
//
// 拓扑模型（master 扁平）：
//   - 所有节点向 master 注册 node_manifest（parent 是数据字段，非连接）
//   - master 维护扁平注册表（lease + monitor 双通道健康检测）
//   - 任意节点可向 master 查拓扑 / 子树 / 路由
// ------------------------------------------------------------------

#include <caf/actor.hpp>
#include <caf/default_enum_inspect.hpp>
#include <caf/fwd.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace caf_plugin_system {

/// 节点角色：master（注册中心）/ region（分支子树）/ worker（叶子业务）。
enum class node_kind : uint8_t { master = 0, region = 1, worker = 2 };

inline std::string to_string(node_kind x) {
  switch (x) {
    case node_kind::master:
      return "master";
    case node_kind::region:
      return "region";
    case node_kind::worker:
      return "worker";
  }
  return "unknown";
}

inline bool from_string(std::string_view str, node_kind& x) {
  if (str == "master") { x = node_kind::master; return true; }
  if (str == "region") { x = node_kind::region; return true; }
  if (str == "worker") { x = node_kind::worker; return true; }
  return false;
}

inline bool from_integer(uint8_t value, node_kind& x) {
  switch (value) {
    case 0: x = node_kind::master; return true;
    case 1: x = node_kind::region; return true;
    case 2: x = node_kind::worker; return true;
    default: return false;
  }
}

template <class Inspector>
bool inspect(Inspector& f, node_kind& x) {
  return caf::default_enum_inspect(f, x);
}

/// 关机请求来源：本地 / 父节点 / 子节点 / 外部。
enum class shutdown_source : uint8_t { local = 0, parent = 1, child = 2, external = 3 };

inline std::string to_string(shutdown_source x) {
  switch (x) {
    case shutdown_source::local:    return "local";
    case shutdown_source::parent:   return "parent";
    case shutdown_source::child:    return "child";
    case shutdown_source::external: return "external";
  }
  return "unknown";
}

inline bool from_string(std::string_view str, shutdown_source& x) {
  if (str == "local")    { x = shutdown_source::local;    return true; }
  if (str == "parent")   { x = shutdown_source::parent;   return true; }
  if (str == "child")    { x = shutdown_source::child;    return true; }
  if (str == "external") { x = shutdown_source::external; return true; }
  return false;
}

inline bool from_integer(uint8_t value, shutdown_source& x) {
  switch (value) {
    case 0: x = shutdown_source::local;    return true;
    case 1: x = shutdown_source::parent;   return true;
    case 2: x = shutdown_source::child;    return true;
    case 3: x = shutdown_source::external; return true;
    default: return false;
  }
}

template <class Inspector>
bool inspect(Inspector& f, shutdown_source& x) {
  return caf::default_enum_inspect(f, x);
}

/// 节点描述：注册到 master 的扁平清单条目。
struct node_manifest {
  node_kind kind = node_kind::worker;
  std::string node_name;           // 全局唯一
  std::string host;                // 对外可连地址（注册用）
  uint16_t port = 0;               // middleman 端口
  std::string parent;              // 父节点名（空 = 直属 master）
  std::vector<std::string> exported_actors;  // 本节点导出的命名 actor
};

template <class Inspector>
bool inspect(Inspector& f, node_manifest& x) {
  return f.object(x).fields(
    f.field("kind", x.kind),
    f.field("node_name", x.node_name),
    f.field("host", x.host),
    f.field("port", x.port),
    f.field("parent", x.parent),
    f.field("exported_actors", x.exported_actors)
  );
}

/// 节点注册请求：manifest + 监控句柄（master 用 down_msg 感知节点退出）。
struct node_registration {
  node_manifest manifest;
  caf::actor monitor_actor;
};

template <class Inspector>
bool inspect(Inspector& f, node_registration& x) {
  return f.object(x).fields(
    f.field("manifest", x.manifest),
    f.field("monitor_actor", x.monitor_actor)
  );
}

/// 注册/心跳/注销的统一应答。
struct register_reply {
  bool ok = false;
  std::string message;
};

template <class Inspector>
bool inspect(Inspector& f, register_reply& x) {
  return f.object(x).fields(
    f.field("ok", x.ok),
    f.field("message", x.message)
  );
}

/// 全量拓扑快照（扁平节点列表，按 parent 字段可还原任意树）。
struct topology_snapshot {
  std::vector<node_manifest> nodes;
};

template <class Inspector>
bool inspect(Inspector& f, topology_snapshot& x) {
  return f.object(x).fields(
    f.field("nodes", x.nodes)
  );
}

/// 指定父节点的子树快照。
struct child_snapshot {
  std::string parent;
  std::vector<node_manifest> children;
};

template <class Inspector>
bool inspect(Inspector& f, child_snapshot& x) {
  return f.object(x).fields(
    f.field("parent", x.parent),
    f.field("children", x.children)
  );
}

/// master 解析结果：到目标节点/actor 的连接信息。
struct actor_route {
  std::string node_name;
  node_kind kind = node_kind::worker;
  std::string host;
  uint16_t port = 0;
  std::string actor_name;
  std::string parent;
};

template <class Inspector>
bool inspect(Inspector& f, actor_route& x) {
  return f.object(x).fields(
    f.field("node_name", x.node_name),
    f.field("kind", x.kind),
    f.field("host", x.host),
    f.field("port", x.port),
    f.field("actor_name", x.actor_name),
    f.field("parent", x.parent)
  );
}

/// 节点关机请求（级联传播）。
struct shutdown_request {
  std::string initiator;    // 发起者（原始节点）
  std::string source_node;  // 上一跳节点
  std::string reason;
  shutdown_source source = shutdown_source::local;
};

template <class Inspector>
bool inspect(Inspector& f, shutdown_request& x) {
  return f.object(x).fields(
    f.field("initiator", x.initiator),
    f.field("source_node", x.source_node),
    f.field("reason", x.reason),
    f.field("source", x.source)
  );
}

} // namespace caf_plugin_system
