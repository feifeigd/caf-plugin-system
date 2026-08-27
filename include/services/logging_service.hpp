#pragma once
// ------------------------------------------------------------------
// 日志服务契约 —— 全项目唯一日志头（核心 + 插件共用）
//
// 为什么只有这一个头（2026-08-23 重构，用户拍板"写日志只有一个地方"）：
//   - logging_service 是 bootstrap 最先 spawn 的系统组件（进程第一个 actor），
//     spdlog console + logs/app.log 归它独占；关机链最后退出（flush 后 quit）。
//   - 调用方一律用本头的 LOG_INFO/LOG_WARN/LOG_ERROR 宏：变参 fmt::format
//     调用点格式化（fmt::runtime，兼容运行时拼接串和字面量格式串）+ 自动
//     带 [文件:行号]（__FILE__/__LINE__）。
//   - actor 目标经 current_logger() 解析——DLL 单一实体（caf_plugin_core
//     动态库内定义，方案 B：exe 与各插件 DLL 链接同一动态库共享一份）：
//       exe 侧：bootstrap_system_components 最先 spawn 后 set_logger()；
//       插件侧：无需 set_logger（DLL 实体已由 exe 注入）→ manifest 不再
//       依赖 logging_service；set_log_source(PLUGIN_NAME) 仍保留（每模块
//       inline 副本，日志来源区分）。
//   - 开关机流程单独落 shutdown-trace.log（控制台销毁时唯一可靠证据），
//     与 app.log 并存，见 docs/windows-shutdown-experience.md。
// ------------------------------------------------------------------

#include "common/message_tags.hpp"
#include "common/core_export.hpp"

#include <caf/all.hpp>
#include <fmt/format.h>

#include <string>
#include <string_view>

namespace caf_plugin_system {

// ------------------------------------------------------------------
// 日志 actor 句柄单例（方案 B，2026-08-27：core 动态库化后为 DLL 单一
// 实体）。定义在 logging_service.cpp（caf_plugin_core.dll 内），exe 与
// 各插件 DLL 链接同一动态库 → 进程内只有一份：
//   - exe 侧：bootstrap_system_components 最先 spawn 后 set_logger()；
//   - 插件侧：**无需再 set_logger**（DLL 实体已由 exe 注入）——这就是
//     插件 manifest 不再依赖 logging_service 的根基（旧实现里每个插件
//     DLL 有独立 inline 副本，必须 set_logger(deps[0]) 自注入）。
// 空 = 尚未初始化（理论上不会发生：exe 侧 spawn 在最前）。
// ------------------------------------------------------------------
extern CORE_API caf::actor& current_logger();

extern CORE_API void set_logger(const caf::actor& a);

/// 日志来源名：exe 侧默认 "core"；插件侧 spawn 时设为 PLUGIN_NAME。
/// 注意：log_source 刻意保持"每模块 inline 副本"（非 DLL 实体）——
/// 插件模块内 set_log_source(PLUGIN_NAME) 只影响本模块，日志里才能
/// 区分来源；若也做成单一实体，插件一设置全局 source 都会变。
inline std::string& log_source() {
    static std::string source = "core";
    return source;
}

inline void set_log_source(const std::string& s) {
    log_source() = s;
}

/// 位置前缀（日志宏内用）：[文件名:行号]（仅取 basename，避免全路径噪音）
inline std::string log_loc(const char* file, int line) {
    std::string_view f(file);
    auto pos = f.find_last_of("/\\");
    if (pos != std::string_view::npos)
        f = f.substr(pos + 1);
    return "[" + std::string(f) + ":" + std::to_string(line) + "] ";
}

/// 变参格式化（日志宏内用）：__VA_ARGS__ 无法在预处理器里拆分，
/// 用模板函数拆——0 参 = 原样返回（运行时拼接串）；1+ 参 = 首参是格式串、
/// 其余是参数。fmt::runtime 绕过 consteval（fmt v12 要求编译期常量格式串），
/// 参数类型不匹配在运行时抛异常 → 由宏的 try/catch 兜底为 "<log format error>"。
template <typename T, typename... Args>
inline std::string log_fmt(const T& first, Args&&... args) {
    if constexpr (sizeof...(Args) == 0)
        return std::string(first);
    else
        return fmt::format(fmt::runtime(first), std::forward<Args>(args)...);
}

// 核心实现（src/core/logging_service.cpp）：spawn 日志服务 actor + 注册。
caf::actor spawn_logging_service(caf::actor_system& sys);

} // namespace caf_plugin_system

// ------------------------------------------------------------------
// 统一日志宏：LOG_INFO(...) / LOG_WARN(...) / LOG_ERROR(...)
//   - 变参：fmt::format(fmt::runtime(...)) 调用点格式化。fmt::runtime 使
//     运行时拼接串（"x" + var）与字面量格式串（"count={}", n）都能用；
//     注意：运行时串里出现 {} 占位符会抛异常 → try/catch 兜底为
//     "<log format error>"。
//   - 行号：宏自动嵌入 [file:line] 前缀。
//   - 发送：anon_send（匿名发送方，不做 actor 追踪；需追踪用 LOG_*_SELF）。
// ------------------------------------------------------------------
#define LOG_IMPL(level, ...)                                                       \
    do {                                                                           \
        if (auto& svc = caf_plugin_system::current_logger()) {                     \
            try {                                                                  \
                caf::anon_send(                                                    \
                    svc, log_atom{}, caf_plugin_system::log_source(), level,       \
                    caf_plugin_system::log_loc(__FILE__, __LINE__)                 \
                        + caf_plugin_system::log_fmt(__VA_ARGS__));                \
            } catch (...) {                                                        \
                caf::anon_send(                                                    \
                    svc, log_atom{}, caf_plugin_system::log_source(), level,       \
                    caf_plugin_system::log_loc(__FILE__, __LINE__)                 \
                        + std::string("<log format error>"));                      \
            }                                                                      \
        }                                                                          \
    } while (0)

#define LOG_INFO(...)  LOG_IMPL("INFO",  __VA_ARGS__)
#define LOG_WARN(...)  LOG_IMPL("WARN",  __VA_ARGS__)
#define LOG_ERROR(...) LOG_IMPL("ERROR", __VA_ARGS__)

// ------------------------------------------------------------------
// 带 sender 的日志宏（actor 上下文内调用，如关机链）：
// LOG_FROM(self, "INFO", msg_expr) —— self->send → 日志服务记录发送方 actor
//（追踪）；且与随后同 sender 的 send_exit 形成 FIFO（关机链 STOPPED 日志
// 保证先落盘、再退出日志服务）。msg 为预格式化字符串表达式，不经 fmt。
// ------------------------------------------------------------------
#define LOG_FROM(self, level, msg)                                                 \
    do {                                                                           \
        if (auto& svc = caf_plugin_system::current_logger())                       \
            (self)->send(svc, log_atom{}, caf_plugin_system::log_source(), level,  \
                         caf_plugin_system::log_loc(__FILE__, __LINE__) + (msg));  \
    } while (0)

// ------------------------------------------------------------------
// 插件便捷变体（actor 上下文内用 self->send，日志服务记录发送方 actor）：
// LOG_INFO_SELF(self, "count={}", n) —— 需要 self 在作用域内。
// ------------------------------------------------------------------
#define LOG_IMPL_SELF(self, level, ...)                                            \
    do {                                                                           \
        if (auto& svc = caf_plugin_system::current_logger()) {                     \
            try {                                                                  \
                if (self)                                                          \
                    (self)->send(svc, log_atom{}, caf_plugin_system::log_source(), \
                                 level,                                            \
                                 caf_plugin_system::log_loc(__FILE__, __LINE__)    \
                                     + caf_plugin_system::log_fmt(__VA_ARGS__));   \
                else                                                               \
                    caf::anon_send(                                                \
                        svc, log_atom{}, caf_plugin_system::log_source(), level,   \
                        caf_plugin_system::log_loc(__FILE__, __LINE__)             \
                            + caf_plugin_system::log_fmt(__VA_ARGS__));            \
            } catch (...) {                                                        \
                caf::anon_send(                                                    \
                    svc, log_atom{}, caf_plugin_system::log_source(), level,       \
                    caf_plugin_system::log_loc(__FILE__, __LINE__)                 \
                        + std::string("<log format error>"));                      \
            }                                                                      \
        }                                                                          \
    } while (0)

#define LOG_INFO_SELF(self, ...)  LOG_IMPL_SELF(self, "INFO",  __VA_ARGS__)
#define LOG_DEBUG_SELF(self, ...) LOG_IMPL_SELF(self, "DEBUG", __VA_ARGS__)
#define LOG_WARN_SELF(self, ...)  LOG_IMPL_SELF(self, "WARN",  __VA_ARGS__)
#define LOG_ERROR_SELF(self, ...) LOG_IMPL_SELF(self, "ERROR", __VA_ARGS__)
