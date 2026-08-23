#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "graceful_shutdown.hpp"
#include "checkpoint_manager.hpp"
#include "plugin/plugin_manager.hpp"
#include "framework_bootstrap.hpp"
#include "services/logging_service.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <unordered_map>
#include <memory>
#include <cstddef>
#include <cstring>

class LoggerPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"LoggerPlugin", "1.0.0", {}, {"logging_service"}, -100};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {

        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file_sink    = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", true);

        auto formatter = std::make_unique<spdlog::pattern_formatter>(
            "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v"
        );
        console_sink->set_formatter(formatter->clone());
        file_sink->set_formatter(std::move(formatter));

        auto logger_cache = std::make_shared<
            std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>
        >();

        return sys.spawn([console_sink, file_sink, logger_cache](
            caf::stateful_actor<int>* self) -> caf::behavior {

            self->state() = 0;

            // 私有业务（log_atom）在前，公共生命周期 behavior 兜底
            caf::message_handler business{
                [=](log_atom, const std::string& source,
                    const std::string& level, const std::string& msg) {

                    // 控制台销毁中（用户点窗口 X）：spdlog 写销毁中的控制台
                    // 句柄会永久阻塞——占住调度线程 → 拖死整个关机链（曾实测
                    // shutdown_atom 延迟 36 秒才被处理、关机链卡死被强杀）。
                    // 此时直接丢弃日志（关机链关键信息在 shutdown-trace.log）。
                    if (caf_plugin_system::console_closing())
                        return;

                    self->state()++;

                    auto it = logger_cache->find(source);
                    if (it == logger_cache->end()) {
                        auto logger = std::make_shared<spdlog::logger>(source);
                        logger->sinks().push_back(console_sink);
                        logger->sinks().push_back(file_sink);
                        (*logger_cache)[source] = logger;
                        it = logger_cache->find(source);
                    }

                    const auto& logger = it->second;
                    if (level == "INFO")       logger->info(msg);
                    else if (level == "DEBUG") logger->debug(msg);
                    else if (level == "WARN")  logger->warn(msg);
                    else if (level == "ERROR") logger->error(msg);
                    else                       logger->info(msg);
                },
            };
            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [](caf::actor, const std::string&) {
                    SPDLOG_INFO("LoggerPlugin initialized");
                },
                // drain：无排空动作，回执由框架统一
                .on_save = [self]() -> std::vector<std::byte> {
                    int count = self->state();
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &count, sizeof(int));
                    return data;
                },
                .on_restore = [self](const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        std::memcpy(&self->state(), data.data(), sizeof(int));
                        spdlog::info("LoggerPlugin restored count={}", self->state());
                    }
                },
                .on_shutdown = []() {
                    spdlog::info("LoggerPlugin shutdown");
                    spdlog::shutdown();   // 插件特有的清理：spdlog 全局关闭
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new LoggerPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
