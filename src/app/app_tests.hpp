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

} // namespace caf_plugin_system
