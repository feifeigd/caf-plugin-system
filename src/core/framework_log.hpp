#pragma once

#include <caf/all.hpp>

#include <string>

namespace caf_plugin_system {

/// 设置 logging_service actor（bootstrap 完成插件加载后注入一次；
/// 空 actor = LoggerPlugin 未加载，后续走 CAF log / cout 兜底）。
/// 线程安全：仅启动期写入一次，之后所有线程并发只读（caf::actor 是
/// shared_ptr 封装，并发读安全）。
void fw_set_logger(caf::actor logger);

/// 统一日志入口（核心组件用，替代散落的 std::cout）：
///   1. logging_service 可用 → anon_send log_atom（spdlog console+file，
///      console 上唯一写者 → 全局有序，不与 CAF log 乱序）
///   2. 否则 CAF_LOG_*（文件，console 已关 → 不打扰单写者）
///   3. 否则 std::cout/std::cerr（保证可见性兜底）
///   控制台销毁中（点窗口 X，console_closing()==true）→ 直接跳过：
///   写销毁中的控制台句柄会阻塞，拖死调度线程/关机链（已实测）。
void fw_log(const std::string& level, const std::string& msg);

inline void fw_log_info(const std::string& msg) {
    fw_log("INFO", msg);
}

inline void fw_log_error(const std::string& msg) {
    fw_log("ERROR", msg);
}

} // namespace caf_plugin_system
