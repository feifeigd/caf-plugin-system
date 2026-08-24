#pragma once
// ------------------------------------------------------------------
// Bridge —— 外部语言节点（Python/Go）的 sidecar 适配器
//
// 背景（2026-08-24）：集群是纯 CAF（BASP）节点。外部语言进程无法直接
// 跑 CAF actor，方案 B（sidecar）：外部进程旁边跑一个 bridge（本模块），
// bridge 以正常集群节点身份注册（worker/master），外部进程经本地 TCP
// 行协议与 bridge 通信，协议层完全复用 CAF，外部只管业务。
//
// 行协议（\n 分隔；payload 用长度前缀，可含任意字节含 \n）：
//   外部 → bridge:  CALL <id> <svc> <len>\n<payload>\n   调本地服务
//   bridge → 外部:  RESULT <id> OK|ERR <len>\n<payload>\n  CALL 的响应
//   bridge → 外部:  REQ <rid> <len>\n<payload>\n         集群→外部请求
//   外部 → bridge:  RESULT <rid> OK|ERR <len>\n<payload>\n REQ 的响应
//
// payload 语义：CALL 的 payload 被包成 plugin_envelope{sub_proto=1,
// payload} 发给目标服务（与跨节点信封同语义）；REQ 的 payload 是
// external_echo 服务收到的信封载荷原样，外部回 RESULT 后由 bridge
// 作为该调用的响应（std::string）返回给集群内调用方。
//
// bridge 注册的服务（固定名 external_echo，handler 在外部进程）：
// 集群内任意节点（经 master 路由 / RemoteCaller）可跨节点调用它。
// ------------------------------------------------------------------

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <cstdint>
#include <string>

namespace caf_plugin_system {

/// 启动 bridge 模式：TCP listener（port）+ 注册 external_echo 服务。
/// registry：ServiceRegistry（注册 external_echo）；node_name 仅日志。
/// 返回 bridge 主 actor（外部 CALL 的处理者）；关机由调用方注册给
/// shutdown_mgr（register_cluster_atom）。
caf::actor spawn_bridge_actor(caf::actor_system& sys, caf::actor registry,
                              std::uint16_t port,
                              const std::string& node_name);

} // namespace caf_plugin_system
