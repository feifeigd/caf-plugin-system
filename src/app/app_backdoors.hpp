#pragma once
// ------------------------------------------------------------------
// 应用级验证后门统一入口（--test-* 选项触发）
//
// 从 main.cpp 分离：保持入口文件干净，验证逻辑集中在此模块。
// 时序约定：必须在【ops 注册 + 集群节点引导完成】之后调用
//（内部依赖 sys.registry().get("ops") 与 nb.master）。
// 不进框架（caf_plugin_core）：属于 app 特有的验证代码。
// ------------------------------------------------------------------

#include "app_config.hpp"
#include "framework_bootstrap.hpp"
#include "cluster/bootstrap.hpp"

#include <caf/actor_system.hpp>

namespace caf_plugin_system {

// 所有 --test-* 验证后门的统一入口。内部按 cfg 的字段分发：
//   test_auto_shutdown / test_cross_call / test_cross_call_ex /
//   test_bridge_call / test_remote_reload / test_quit / test_ctrl_c
//   test_unload（运行期卸载 + 统一解绑广播验证）
void run_test_backdoors(caf::actor_system& sys, const app_config& cfg,
                        const cluster::BootstrapResult& nb,
                        const BootstrapResult& fw);

} // namespace caf_plugin_system
