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
    // sinks 归本日志服务 actor 独占，跨模块（exe / 各插件 DLL）共享走 actor
    // 路径——各模块 current_logger() 都指向它、发 log_atom（契约见
    // services/logging_service.hpp："写日志只有一个地方"）。logger 用
    // make_shared 直建、存 logger_cache，不注册进 spdlog 全局 registry：
    // 插件 spdlog::get(name) 会拿空，绕过 actor 直写 sink 违反契约。
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink    = std::make_shared<spdlog::sinks::basic_file_sink_mt>(
        "logs/app.log", true);

    auto formatter = std::make_unique<spdlog::pattern_formatter>(
        std::string("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v"));
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

                // actor 级追踪：非匿名发送方 → 名字带 actor 唯一 ID
                //（如 shutdown_mgr#4、business#6——进程内自增，每个 actor 不同）。
                // 坑：caf::to_string(actor_addr) 渲染的是 node 级标识，同进程
                // 所有 actor 相同（实测恒等于进程 PID 段，如 #15316），不是
                // actor id！必须用 actor_addr::id()（actor_control_block 的 id）。
                auto who = self->current_sender();
                if (who) {
                    auto actor_id = who->id();
                    // 跨节点识别：actor_addr::node() 给出 actor 来源节点。
                    // 仅当来源节点非本节点时才显示 @node（本地日志保持干净）。
                    // 已知限制（CAF 1.1 实测）：同机多进程 node_id 相同
                    // （host_id 基于 IP），同机部署区分不了外部进程；跨进程
                    // sender 还可能解析失败 → current_sender() 为空 → 走
                    // else 退化分支（仅 source，无追踪）。
                    std::string origin;
                    if (who->node() != self->system().node())
                        origin = "@" + caf::to_string(who->node());
                    auto addr = caf::to_string(who);
                    if (addr_names->find(addr) == addr_names->end())
                        (*addr_names)[addr] = source;
                    do_log(source + "#" + std::to_string(actor_id) + origin,
                           level, msg);
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
