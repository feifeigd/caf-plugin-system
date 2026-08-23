#include "framework_log.hpp"
#include "framework_bootstrap.hpp"
#include "common/message_tags.hpp"

#include <caf/logger.hpp>

#include <iostream>

namespace caf_plugin_system {

namespace {

caf::actor g_logger;

} // namespace

void fw_set_logger(caf::actor logger) {
    g_logger = std::move(logger);
}

void fw_log(const std::string& level, const std::string& msg) {
    // 控制台销毁中（点窗口 X）：任何 console 写（spdlog stdout sink /
    // cout / fprintf）都会阻塞在销毁中的控制台句柄上，拖死调用线程
    //（事件循环/调度器/关机链）。此场景日志只依赖 shutdown-trace.log
    // 落盘，这里直接跳过。
    if (console_closing())
        return;

    // 1. logging_service（优先通道）：spdlog console+file，单 actor 串行，
    //    console 上唯一写者 → 全局有序。source 统一 "core"。
    if (g_logger) {
        caf::anon_send(g_logger, log_atom{}, std::string("core"), level, msg);
        return;
    }

    // 2. CAF log（文件兜底）：console verbosity 已配 quiet，不进 console，
    //    避免与 logging_service 双写者乱序；文件（logs/caf-framework.log）
    //    保留完整记录。
    if (level == "ERROR")
        CAF_LOG_ERROR(msg);
    else
        CAF_LOG_INFO(msg);

    // 3. cout 兜底：CAF logger 输出目标不定会静默丢日志（已知坑），
    //    无 logging_service 时直接打印保证可见。
    if (level == "ERROR")
        std::cerr << msg << std::endl;
    else
        std::cout << msg << std::endl;
}

} // namespace caf_plugin_system
