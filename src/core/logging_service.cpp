#include "services/logging_service.hpp"
#include "framework_bootstrap.hpp"  // console_closing()

#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <unordered_map>

namespace caf_plugin_system {

caf::actor spawn_logging_service(caf::actor_system& sys) {
    // 幂等：重复 bootstrap（理论上不会）直接返回既有实例
    if (current_logger())
        return current_logger();

    // spdlog sinks：console（stdout 颜色）+ 文件（logs/app.log，每次启动重写）。
    // spdlog 以共享 DLL 链接，sinks 跨 DLL 可共享（插件若直接 spdlog::get
    // 也能拿到同名 logger——见 services/logging_service.hpp 的契约）。
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink    = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/app.log", true);

    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        std::string("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v"));
    console_sink->set_formatter(formatter->clone());
    file_sink->set_formatter(std::move(formatter));

    // per-source logger 缓存（"core"/"business"/...）：sink 共享，避免每次新建
    auto logger_cache = std::make_shared<
        std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>>();

    // actor 级追踪：发送方地址 → source 绑定（首次见到时记录）
    auto addr_names = std::make_shared<std::unordered_map<std::string, std::string>>();

    auto svc = sys.spawn([console_sink, file_sink, logger_cache, addr_names](
                             caf::event_based_actor* self) -> caf::behavior {
        auto do_log = [=](const std::string& name, const std::string& level,
                          const std::string& msg) {
            auto it = logger_cache->find(name);
            if (it == logger_cache->end()) {
                auto logger = std::make_shared<spdlog::logger>(name);
                logger->sinks().push_back(console_sink);
                logger->sinks().push_back(file_sink);
                (*logger_cache)[name] = logger;
                it = logger_cache->find(name);
            }
            const auto& logger = it->second;
            if (level == "INFO")       logger->info(msg);
            else if (level == "DEBUG") logger->debug(msg);
            else if (level == "WARN")  logger->warn(msg);
            else if (level == "ERROR") logger->error(msg);
            else                       logger->info(msg);
        };

        return caf::behavior{
            // 统一日志入口：(log_atom, source, level, msg)
            [=](log_atom, const std::string& source, const std::string& level,
                const std::string& msg) {
                // 控制台销毁中（点窗口 X）：spdlog 写销毁中的控制台句柄会
                // 永久阻塞——占住本 actor 的调度线程 → 拖死整个关机链
                //（曾实测 shutdown_atom 延迟 36 秒）。此时丢弃日志
                //（关机链关键信息在 shutdown-trace.log，落盘不受影响）。
                if (console_closing())
                    return;

                // actor 级追踪：非匿名发送方 → 名字带 actor 短 ID
                //（如 shutdown_mgr#12345）。anon_send 匿名发送则仅用 source。
                auto who = self->current_sender();
                if (who) {
                    auto id = caf::to_string(who);
                    auto pos = id.rfind('#');
                    if (pos != std::string::npos)
                        id = id.substr(pos + 1);
                    if (id.empty())
                        id = caf::to_string(who);
                    auto addr = caf::to_string(who);
                    if (addr_names->find(addr) == addr_names->end())
                        (*addr_names)[addr] = source;
                    do_log(source + "#" + id, level, msg);
                } else {
                    do_log(source, level, msg);
                }
            },

            // 关机链最后杀本 actor：flush 所有 sink 再退出（数据不丢）
            [=](caf::exit_msg& em) {
                file_sink->flush();
                console_sink->flush();
                self->quit(em.reason);
            },
        };
    });

    // 注册为本模块（exe）的日志单例：bootstrap 后续 LOG_* 宏立即生效
    set_logger(svc);
    return svc;
}

} // namespace caf_plugin_system
