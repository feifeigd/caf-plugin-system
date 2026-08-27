#pragma once
// ------------------------------------------------------------------
// 应用级验证后门（--test-* 选项触发）
//
// 从 main.cpp 分离：保持入口文件干净，验证逻辑集中在此模块。
// 不进框架（caf_plugin_core）：属于 app 特有的验证代码。
// ------------------------------------------------------------------

#include "framework_bootstrap.hpp"

#include <caf/actor.hpp>
#include <caf/actor_system.hpp>

#include <string>

namespace caf_plugin_system {

/// 冒烟测试（--test-auto-shutdown）：ACL 拦截、缓冲冲刷、热更新、新代码生效。
void run_smoke_tests(caf::actor_system& sys, const BootstrapResult& fw);

/// 脚本插件验证（--test-lua-script）：resolve echo_service，发 plugin_envelope
/// 与 std::string，校验 on_call / on_string 回执（验证 lua_host 桥接层）。
void run_lua_script_test(caf::actor_system& sys, const BootstrapResult& fw);

/// 脚本插件验证（--test-py-script）：同 lua 版，验证 py_host 桥接层 + 热更状态交接。
void run_py_script_test(caf::actor_system& sys, const BootstrapResult& fw);

/// 脚本插件验证（--test-ts-script）：同 lua 版，验证 ts_host 桥接层 + 热更状态交接。
void run_ts_script_test(caf::actor_system& sys, const BootstrapResult& fw);

/// 统一时间源验证（--test-time-offset）：校验 business_now() - 真实 now
/// == 配置偏移（time-offset），打印业务时间串与真实时间串对比。
void run_time_offset_test();

/// 跨节点调用验证（--test-cross-call=<服务名>，master 进程执行）。
/// RemoteCaller 缓存句柄 + 失败自动重试；循环调用观察杀/重启目标节点时
/// "失败 → 自动恢复"（缓存失效 → 重新 resolve）。
void run_cross_call_test(caf::actor_system& sys, caf::actor master,
                         const std::string& local_node_name,
                         const std::string& actor_name);

/// 跨节点调用 + 有界重试（--test-cross-call-ex=<服务名>）。
/// 配合"先启 master 后启 worker"验证重启窗口期的调用不丢失。
void run_cross_call_ex_test(caf::actor_system& sys, caf::actor master,
                            const std::string& local_node_name,
                            const std::string& actor_name);

/// bridge 验证（--test-bridge-call=<节点名>，master 进程执行）：
/// 跨节点调用指定节点的 external_echo（服务 handler 在外部进程，
/// bridge 转发）——验证 集群→bridge→外部进程 完整链路。
/// shutdown_mgr：caller 注册进去，关机时统一 send_exit——
/// 重试循环若撞上 EOF 关机（最长 4 分钟），request 立即失败
/// 快速退出，避免 RemoteCaller 残活导致 actor_system 析构 join 挂起
/// （实测：无外部客户端时循环跑满，LeakCheck actors remaining: 1）。
void run_bridge_call_test(caf::actor_system& sys, caf::actor master,
                          const std::string& local_node_name,
                          const std::string& node_name,
                          caf::actor shutdown_mgr);

} // namespace caf_plugin_system
