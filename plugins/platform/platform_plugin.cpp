#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "graceful_shutdown.hpp"
#include "checkpoint_manager.hpp"
#include "plugin/plugin_manager.hpp"
#include "services/config_service.hpp"
#include "services/metrics_service.hpp"
#include "services/logging_service.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>
#include <map>

struct PlatformState {
    std::map<std::string, std::string> configs{
        {"app.name", "caf-plugin-system"},
        {"app.version", "1.0.0"},
        {"log.level", "INFO"}
    };
    std::map<std::string, int> metrics;
};

class PlatformPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {
            "PlatformPlugin",
            "1.0.0",
             {},   // 日志走 logging_service（不再直接 std::cout）
            {"config_service", "metrics_service"},
            -100
        };
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        // 日志句柄走 DLL 单一实体（exe 已注入），无需 deps 注入
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        return sys.spawn([](caf::stateful_actor<PlatformState>* self) -> caf::behavior {
            // 私有业务在前（config/metrics 高频），公共生命周期兜底
            caf::message_handler business{
                [=](get_config_atom, const std::string& key) -> std::string {
                    auto it = self->state().configs.find(key);
                    return (it != self->state().configs.end()) ? it->second : "";
                },
                [=](set_config_atom, const std::string& key, const std::string& val) {
                    self->state().configs[key] = val;
                    LOG_INFO_SELF(self, "[PlatformConfig] Set: {} = {}", key, val);
                },
                [=](report_metric_atom, const std::string& key, int delta) {
                    self->state().metrics[key] += delta;
                },
                [=](get_metrics_atom) -> std::map<std::string, int> {
                    return self->state().metrics;
                },
            };
            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [self](caf::actor, const std::string&) {
                    LOG_INFO("[Platform] config + metrics ready ({} default configs)",
                             self->state().configs.size());
                },
                // drain：无排空动作，回执由框架统一
                .on_save = [self]() -> std::vector<std::byte> {
                    int cfg_count = static_cast<int>(self->state().configs.size());
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &cfg_count, sizeof(int));
                    return data;
                },
                .on_restore = [self](const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        int cfg_count = 0;
                        std::memcpy(&cfg_count, data.data(), sizeof(int));
                        LOG_INFO("[Platform] Restored {} configs", cfg_count);
                    }
                },
                // shutdown：无清理动作，quit 由框架统一
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new PlatformPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
