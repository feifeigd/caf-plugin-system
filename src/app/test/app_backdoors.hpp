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
#include "plugin/plugin_interface.hpp"

#include <caf/actor_system.hpp>

namespace caf_plugin_system {

// 所有 --test-* 验证后门的统一入口。内部按 cfg 的字段分发：
//   test_auto_shutdown / test_cross_call / test_cross_call_ex /
//   test_bridge_call / test_remote_reload / test_quit / test_ctrl_c
//   test_unload（运行期卸载 + 统一解绑广播验证）
// extern "C" + PLUGIN_API：core 经 dll_main 的 test_hook 函数指针调用
//（2026-08-31：从独立 DLL 移回 exe 编译——修复 test DLL 卸载后
// delayed 线程执行已卸载代码段 → 0xC0000005 的根因；改测试只重编 exe，
// core/插件全链不重建）。
extern "C" PLUGIN_API void run_test_backdoors(
    caf::actor_system& sys, const app_config& cfg,
    const cluster::BootstrapResult& nb, const BootstrapResult& fw);

} // namespace caf_plugin_system
