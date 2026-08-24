#pragma once
// ------------------------------------------------------------------
// OpsActor —— 每节点运维入口 actor
//
// 两个入口：
//   1. 本地控制台（stdin 线程 → console_cmd_atom）：
//        help / list / nodes / reload <plugin> <path> /
//        reload-node <node> <plugin> <path> / quit
//   2. 远程运维（master → remote_reload_atom{reload_request}）：
//        双模式——dll_bytes 字节流推送（节点落盘 updates/ 新路径）或
//        dll_path 已就位路径，落盘后转调 PluginManager 的 reload_atom
//        （完整热更流程：quiesce → save_state → LoadLibrary 新路径 →
//        spawn → 台账切换 → resume → send_exit，见 docs/hot-reload-procedure.md）
//
// 寻址：节点把 ops actor 放进 CAF registry（名 "ops"）并随 exported_actors
//       上报 master；master 侧 node_resolve(node, "ops") → connect →
//       remote_lookup("ops") 拿到远端句柄（与 RemoteCaller 同款寻址）。
//
// 安全模型：不做 sender 拒绝校验——CAF 同机多进程 node_id 相同（host_id
// 同 IP，实测 master+worker 同机相等），跨进程 sender 会被解析成本地 actor
// 查找而失效/为空，拒绝校验会误拒所有同机部署。安全边界 = middleman 端口
// 可达性 + 集群互信（能 lookup 到 "ops" 的只有已注册节点）。reload-node
// 命令仅 master 进程可用（需要 master_registry 句柄）。
// ------------------------------------------------------------------

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <string>

namespace caf_plugin_system { namespace cluster {

/// 派生每节点的运维 actor。
/// @param node_name        本节点名（日志）
/// @param plugin_mgr       PluginManager actor（纯节点进程传空 actor）
/// @param master_registry  master 注册表 actor（仅 master 进程非空；
///                         reload-node / nodes 命令需要它）
/// @param updates_dir      字节流推送的落盘根目录（默认 ./updates）
/// 注意：不传 shutdown_mgr——ops 注册进 shutdown_mgr 的 cluster_ctls_
/// （register_cluster_atom），若再存 shutdown_mgr 强引用成员会形成环
/// （control block 互相保活，进程退出不释放）。quit 命令需要时从
/// registry 瞬时查找（见 ops_actor.cpp quit handler）。
caf::actor spawn_ops_actor(caf::actor_system& sys, std::string node_name,
                           caf::actor plugin_mgr, caf::actor master_registry,
                           std::string updates_dir = "./updates");

/// 启动 stdin 控制台线程：读取命令行 → anon_send console_cmd_atom。
/// 管道 stdin（WSL interop / 重定向）自动跳过——install_stdin_watchdog
/// 接管 EOF 检测，避免两线程抢读同一管道。
/// 注意：线程内不得使用 scoped_actor（getline 阻塞导致析构挂起）。
void start_console_thread(caf::actor ops);

}} // namespace caf_plugin_system::cluster
