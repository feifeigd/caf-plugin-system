#include "plugin_interface.hpp"
#include "services/logging_service.hpp"
#include <spdlog/spdlog.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <unordered_map>
#include <memory>
#include <cstddef>
#include <cstring>

using init_atom = caf::atom_constant<caf::atom("init")>;
using shutdown_atom = caf::atom_constant<caf::atom("shutd")>;
using drain_atom = caf::atom_constant<caf::atom("drain")>;
using save_state_atom = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

class LoggerPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"LoggerPlugin", "1.0.0", {}, {"logging_service"}, -100};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {

        // 共享 sinks：控制台 + 文件
        auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        auto file_sink    = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/app.log", true);

        // 统一日志格式: [时间] [插件名] [级别] 消息
        auto formatter = std::make_unique<spdlog::pattern_formatter>(
            "[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v"
        );
        console_sink->set_formatter(formatter->clone());
        file_sink->set_formatter(std::move(formatter));

        // 懒加载的命名 logger 缓存
        auto logger_cache = std::make_shared<
            std::unordered_map<std::string, std::shared_ptr<spdlog::logger>>
        >();

        return sys.spawn([console_sink, file_sink, logger_cache](
            caf::stateful_actor<int>* self) -> caf::behavior {

            self->state = 0;

            return caf::behavior{
                [=](init_atom, caf::actor, const std::string&) {
                    SPDLOG_INFO("LoggerPlugin initialized");
                },
                [=](log_atom, const std::string& source,
                    const std::string& level, const std::string& msg) {

                    self->state++;

                    // 懒加载：为每个 source 创建命名 logger
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
                    else                       logger->info(msg); // fallback
                },
                [=](drain_atom, caf::actor coordinator) {
                    spdlog::info("LoggerPlugin draining...");
                    self->send(coordinator, drain_atom::value, self->address());
                },
                [=](save_state_atom) -> std::vector<std::byte> {
                    int count = self->state;
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &count, sizeof(int));
                    return data;
                },
                [=](restore_state_atom, const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        std::memcpy(&self->state, data.data(), sizeof(int));
                        spdlog::info("LoggerPlugin restored count={}", self->state);
                    }
                },
                [=](shutdown_atom) {
                    spdlog::info("LoggerPlugin shutdown");
                    spdlog::shutdown();  // 刷盘
                    self->quit();
                }
            };
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new LoggerPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
