#pragma once
#include <caf/all.hpp>
#include <string>
#include <fmt/format.h>

// ------------------------------------------------------------------
// 日志服务契约
// 提供方：LoggerPlugin
// 消费方：任何需要打日志的插件
// ------------------------------------------------------------------
using log_atom = caf::atom_constant<caf::atom("logmsg")>;

namespace logging_service {

// 底层发送接口：发送已格式化的日志消息
// 参数: logger_actor, source_plugin, level, message
inline void send_log(const caf::actor& logger,
                     const std::string& source,
                     const std::string& level,
                     const std::string& msg) {
    if (logger) {
        caf::anon_send(logger, log_atom::value, source, level, msg);
    }
}

} // namespace logging_service

// ------------------------------------------------------------------
// 变参日志宏（基于 fmt::format，支持 {} 占位符）
//
// 使用方式：
//   #define PLUGIN_NAME "business"
//   #include "services/logging_service.hpp"
//
//   LOG_INFO(logger,  "order {} created, price={}", order_id, price);
//   LOG_DEBUG(logger, "current state: {}", state);
//   LOG_WARN(logger,  "connection slow: {}ms", latency);
//   LOG_ERROR(logger, "failed to open {}", path);
// ------------------------------------------------------------------

#define LOG_IMPL(logger, level, ...) \
    do { \
        if (logger) { \
            try { \
                logging_service::send_log( \
                    logger, PLUGIN_NAME, level, fmt::format(__VA_ARGS__)); \
            } catch (...) { \
                logging_service::send_log( \
                    logger, PLUGIN_NAME, level, "<log format error>"); \
            } \
        } \
    } while (0)

#define LOG_INFO(logger, ...)  LOG_IMPL(logger, "INFO",  __VA_ARGS__)
#define LOG_DEBUG(logger, ...) LOG_IMPL(logger, "DEBUG", __VA_ARGS__)
#define LOG_WARN(logger, ...)  LOG_IMPL(logger, "WARN",  __VA_ARGS__)
#define LOG_ERROR(logger, ...) LOG_IMPL(logger, "ERROR", __VA_ARGS__)
