#pragma once
// ------------------------------------------------------------------
// 跨节点服务调用客户端（event-based actor，per-key 串行队列）
//
// CAF 惯用姿势：RemoteCaller 是一个 event-based actor，缓存/流程全部
// 走消息传递。调用方（任意 actor/scoped_actor）以 request 方式发送
// cross_call_atom / cross_call_ex_atom，响应为远端服务回执字符串。
//
// 两种调用形态（arity 重载，可共存）：
//   cross_call(svc, env)             → 任意节点：master 返回导出该服务的
//                                       全部候选（service_resolve_atom，
//                                       只回匹配节点，O(k) 非 O(N)），
//                                       round-robin 负载 + 失败 failover
//   cross_call(svc, node, env)       → 指定节点：node_resolve_atom 精确路由
//   cross_call_ex(svc, attempts, env)       → 同 cross_call + 有界重试
//   cross_call_ex(svc, node, attempts, env) → 同指定节点 + 有界重试
//
// ★ 有序性保证（调用方必须遵守）：
//   - 同 key（svc 或 svc@node）严格串行：per-key in_flight + 等待队列，
//     响应回调才放行下一个 → 同一远端 actor 的消息 FIFO 有序
//   - 不同 key 完全并行：一个节点的重试不阻塞其他节点的调用
//   - 需要严格有序时用【指定节点】模式（cross_call(svc, node, env)）；
//     任意节点模式会 round-robin 分散到多副本，节点间无顺序保证
//
// 重试语义：cross_call_ex 在失败（resolve/connect/call 任一步）后按
// 1s 间隔重试，attempts 次内成功则返回；全部失败返回最后错误。重试
// 期间同 key 的其他调用排队等待（有序），不同 key 不受影响。
// ------------------------------------------------------------------

#include "common/plugin_envelope.hpp"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <string>

namespace caf_plugin_system { namespace cluster {

/// 派生 RemoteCaller actor。master 为拓扑查询目标（node_resolve 路由），
/// local_node_name 用于跳过本进程节点（不调自己）。
caf::actor spawn_remote_caller(caf::actor_system& sys, caf::actor master,
                               std::string local_node_name);

}} // namespace caf_plugin_system::cluster
