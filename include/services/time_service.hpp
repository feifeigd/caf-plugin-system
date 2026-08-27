// ------------------------------------------------------------------
// 统一时间源（time_service）：全局业务时间偏移（单一时钟源）
//
// 需求（用户拍板 2026-08-25）：测试"未来的时间"但不能修改机器时钟。
// 设计：不做"多插件各自同步偏移"（时间戳必然发散），而是全局唯一
// 偏移 + inline 快路径，所有插件从同一处读：
//
//   business_now()          —— 业务时间（真实时间 + 全局偏移），inline
//                              读原子变量，零开销，任意热路径可调
//   set_time_offset(sec)    —— 控制路径（框架启动时从配置注入）
//   time_offset()           —— 查询当前偏移（秒）
//
// 边界（铁律）：
//   - 业务时间（过期判断/对账/日志时间戳）→ business_now()
//   - 基础设施时间（CAF 调度/delayed_send/集群 lease/心跳）→ 保持
//     真实时钟（steady_clock/system_clock），偏移绝不参与——否则定时
//     器会被偏移提前触发、lease 续租语义错乱
//   - 偏移是每进程一份（进程级原子）；集群多节点测试时各节点配置
//     文件写同一个 time-offset 值即天然一致，无需运行时同步
// ------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>

namespace caf_plugin_system {

/// 全局业务时间偏移（秒）。启动早期由 framework_bootstrap 从配置注入；
/// 运行期 ops 控制命令（time-offset <sec>）也可改。relaxed 足够——
/// 只要求"最终可见"，不要求跨线程顺序（读偏移本身无依赖序）。
inline std::atomic<std::chrono::seconds> g_time_offset{
    std::chrono::seconds{0}};

/// 设置全局时间偏移（秒）。控制路径：框架启动 / ops 命令。
inline void set_time_offset(std::chrono::seconds off) noexcept {
    g_time_offset.store(off, std::memory_order_relaxed);
}

/// 查询当前偏移（秒）。
inline std::chrono::seconds time_offset() noexcept {
    return g_time_offset.load(std::memory_order_relaxed);
}

/// 业务时间 = 真实时间 + 全局偏移。插件业务代码一律用它，
/// 禁止直接 system_clock::now()（代码评审纪律项）。
inline std::chrono::system_clock::time_point business_now() noexcept {
    return std::chrono::system_clock::now()
           + g_time_offset.load(std::memory_order_relaxed);
}

/// 业务时间格式化串（默认 "%Y-%m-%d %H:%M:%S"，本地时区）。
/// 供日志/checkpoint 头/自检输出使用，保证测试证据与业务时间一致。
inline std::string format_business_now(const char* fmt = "%Y-%m-%d %H:%M:%S") {
    const auto t = std::chrono::system_clock::to_time_t(business_now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[64];
    std::strftime(buf, sizeof buf, fmt, &tm);
    return std::string{buf};
}

} // namespace caf_plugin_system
