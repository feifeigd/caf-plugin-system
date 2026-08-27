// ------------------------------------------------------------------
// caf_plugin_core 跨模块导出宏（方案 B：core 动态库化，2026-08-27）
//
// 背景：caf_plugin_core 由 STATIC 改为 SHARED（Windows DLL / Linux .so）
// 后，exe 与所有插件 DLL 链接同一个 core 动态库——库内定义的全局存储
//（g_time_offset、current_logger 等）是进程级单一实体，跨模块天然共享，
// 不再需要"每模块副本 + 同步推送"（方案 A 的 DLL 副本税，已废除）。
//
// 用法：
//   - 头文件声明：extern CORE_API <type> <symbol>;   —— 消费侧 dllimport
//   - 库内定义：  CORE_API <type> <symbol>{...};      —— 构建侧 dllexport
//   - 普通函数/类成员：无需手标——CMake 对 SHARED 目标开启
//     WINDOWS_EXPORT_ALL_SYMBOLS，自动导出全部非 inline 符号。
//     CORE_API 只用于 inline 变量/必须显式控制的符号。
// ------------------------------------------------------------------
#pragma once

#ifdef _WIN32
    #ifdef caf_plugin_core_EXPORTS
        #define CORE_API __declspec(dllexport)
    #else
        #define CORE_API __declspec(dllimport)
    #endif
#else
    #define CORE_API __attribute__((visibility("default")))
#endif
