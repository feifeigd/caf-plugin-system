// ------------------------------------------------------------------
// 统一时间源（time_service）：全局业务时间偏移（单一时钟源）
//
// 需求（用户拍板 2026-08-25）：测试"未来的时间"但不能修改机器时钟。
// 设计：不做"多插件各自同步偏移"（时间戳必然发散），而是全局唯一
// 偏移，所有模块从同一处读：
//
//   business_now()          —— 业务时间（真实时间 + 全局偏移）
//   set_time_offset(sec)    —— 控制路径（框架启动时从配置注入）
//   time_offset()           —— 查询当前偏移（秒）
//
// 存储（方案 B，2026-08-27，用户拍板）：g_time_offset 定义在
// caf_plugin_core 动态库内部（time_service.cpp），**不导出**——数据
// 封装在 DLL 内（规避导出数据在 Windows 上的 thunk/跨编译器
// ABI/初始化顺序坑），所有访问经 CORE_API 导出函数。exe 与各插件
// DLL 链接同一动态库 → 进程内只有一份实体，无副本、无需同步：
// exe 设置一次，所有插件 business_now() 立即读到相同偏移。
//
// 边界（铁律）：
//   - 业务时间（过期判断/对账/日志时间戳）→ business_now()
//   - 基础设施时间（CAF 调度/delayed_send/集群 lease/心跳）→ 保持
//     真实时钟（steady_clock/system_clock），偏移绝不参与——否则定时
//     器会被偏移提前触发、lease 续租语义错乱
// ------------------------------------------------------------------
#pragma once

#include "common/core_export.hpp"

#include <chrono>
#include <string>

namespace caf_plugin_system {

/// 设置全局时间偏移（秒）。控制路径：框架启动 / ops 命令。
/// 数据实体在 caf_plugin_core 动态库内（不导出），本函数为导出 API。
CORE_API void set_time_offset(std::chrono::seconds off) noexcept;

/// 查询当前偏移（秒）。
CORE_API std::chrono::seconds time_offset() noexcept;

/// 业务时间 = 真实时间 + 全局偏移。插件业务代码一律用它，
/// 禁止直接 system_clock::now()（代码评审纪律项）。
CORE_API std::chrono::system_clock::time_point business_now() noexcept;

/// 业务时间格式化串（默认 "%Y-%m-%d %H:%M:%S"，本地时区）。
/// 供日志/checkpoint 头/自检输出使用，保证测试证据与业务时间一致。
CORE_API std::string format_business_now(
    const char* fmt = "%Y-%m-%d %H:%M:%S");

} // namespace caf_plugin_system
