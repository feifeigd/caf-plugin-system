#pragma once
// ------------------------------------------------------------------
// Pomelo 协议端点 —— 兼容 Pomelo 游戏客户端（pomelo-protocol 0.1.6）
//
// 背景：外部游戏客户端（原 Pomelo 生态）无法直接跑 CAF actor，本端点
// 以标准 Pomelo TCP 协议接受客户端连接，把 REQUEST/NOTIFY 翻译成内部
// plugin_envelope 调用（走集群服务），响应按 Pomelo 帧格式回。
//
// 与行协议 bridge（bridge_actor）并存：行协议给外部语言 sidecar
//（Python/Go 进程，协议号契约），Pomelo 端点给游戏客户端
//（字符串 route 契约）。两者都注册到 shutdown_mgr 统一关机。
//
// 完整握手链：连接 → 服务器发 HANDSHAKE（JSON code=200）→ 客户端回
// HANDSHAKE_ACK → DATA 就绪；HEARTBEAT 自动互答。
//
// route 映射（安全边界）：客户端只发外部 route 字符串（如
// "connector.entryHandler.entry"），经 pomelo_route_table 翻译成
// "svc:function"（内部 MFA）——内部函数名永不上线，未知 route 拒绝。
// ------------------------------------------------------------------

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <cstdint>
#include <map>
#include <string>

namespace caf_plugin_system {

/// Pomelo route 映射表：外部 route 字符串 → "svc:function"（内部 MFA）。
using pomelo_route_table = std::map<std::string, std::string>;

/// 启动 Pomelo 协议端点：TCP 监听 port，完整握手链，REQUEST/NOTIFY
/// 按 route 表翻译成内部 envelope 调用。registry：ServiceRegistry
///（resolve 目标服务）；node_name 仅日志。返回端点主 actor（broker）；
/// 关机由调用方注册给 shutdown_mgr（register_cluster_atom）。
caf::actor spawn_pomelo_bridge(caf::actor_system& sys, caf::actor registry,
                               std::uint16_t port,
                               const pomelo_route_table& routes,
                               const std::string& node_name);

} // namespace caf_plugin_system
