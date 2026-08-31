// src/app/main.cpp —— 程序入口（CAF_MAIN 宏生成 main）
// ------------------------------------------------------------------
// 2026-08-31：从 core 的 dll_main 手工样板改回 CAF_MAIN 宏（59c57cb
// 形态）：init_global_meta_objects / CLI parse / actor_system 构造与
// 析构 / 返回码全部由宏生成，exe 只保留注入与清理职责。
//
// 测试代码（--test-* 后门）编译在本 exe（test/app_backdoors.cpp +
// app_tests_fixed.cpp）：core 的 caf_main 经 set_test_backdoor 注册的
// 函数指针调用它们。从独立 test DLL 移回的原因：
//   1. 修复 8-31 崩溃根因——test DLL 在 run_test_backdoors 返回后被
//      FreeLibrary，后门 spawn 的 delayed 线程（2s 后触发关机）代码仍
//      在已卸载的 DLL 内 → 执行已卸载代码段 → 0xC0000005。
//   2. 改测试只重编本 exe，core.dll 与插件 DLL 全链不重建（插件全链
//      链接 caf_plugin_core，core 重编即全链重编，必须避免）。
// ------------------------------------------------------------------

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // 防 windows.h 的 max/min 宏污染 CAF timespan::max()
#endif
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

#include <caf/caf_main.hpp>

#include "app_config.hpp"
#include "plugin/plugin_interface.hpp"
#include "test/app_backdoors.hpp"

// ---- core（caf_plugin_core.dll）导出符号 ----
// caf_main：主逻辑（节点/插件引导 + 等待关机），actor_system 由
// CAF_MAIN 生成的 main 构造、exec_main 负责析构。
extern "C" __declspec(dllimport) int caf_main(
    caf::actor_system& sys, const caf_plugin_system::app_config& cfg);
extern "C" __declspec(dllimport) void set_test_backdoor(
    void (*fn)(caf::actor_system&, const caf_plugin_system::app_config&,
               const caf_plugin_system::cluster::BootstrapResult&,
               const caf_plugin_system::BootstrapResult&));

namespace {

// main() 之前的 static 初始化：注册测试后门 + 启用 CRT 泄漏检测。
// CAF_MAIN 宏生成的 main 没有插入点，故放这里（CRT 保证先于 main 执行；
// 单 TU 内 static 初始化顺序确定）。
struct EntryInit {
  EntryInit() {
    set_test_backdoor(&caf_plugin_system::run_test_backdoors);
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
  }
} s_entry_init;

} // namespace

// 入口：CAF_MAIN() 宏展开为 main() = core/模块 meta 初始化 + exec_main
//（parse → actor_system 构造 → 全局 caf_main（core 导出）→ sys 析构）。
// caf_main 定义在 core（src/core/__main__.cpp），全局命名空间。
CAF_MAIN()
