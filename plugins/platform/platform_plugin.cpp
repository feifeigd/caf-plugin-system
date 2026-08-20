#include "plugin_interface.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>
#include <unordered_map>

// ------------------------------------------------------------------
// 三个服务的 atom
// ------------------------------------------------------------------
using log_atom     = caf::atom_constant<caf::atom("logmsg")>;
using get_config_atom  = caf::atom_constant<caf::atom("getcfg")>;
using set_config_atom  = caf::atom_constant<caf::atom("setcfg")>;
using report_metric_atom = caf::atom_constant<caf::atom("rptmet")>;
using get_metrics_atom = caf::atom_constant<caf::atom("getmet")>;
using init_atom    = caf::atom_constant<caf::atom("init")>;
using shutdown_atom = caf::atom_constant<caf::atom("shutd")>;
using drain_atom   = caf::atom_constant<caf::atom("drain")>;
using save_state_atom = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

// ------------------------------------------------------------------
// PlatformPlugin：一个插件提供三个基础设施服务
//
// 内部维护三个状态（日志计数器、配置表、指标表），
// 通过同一个 actor 的 behavior 统一处理所有服务的消息。
//
// 这样 ServiceRegistry 可以把 logging_service / config_service / metrics_service
// 都注册到同一个 actor 句柄，由 CAF 的 pattern matching 自动分发。
// ------------------------------------------------------------------
struct PlatformState {
    int log_count = 0;
    std::unordered_map<std::string, std::string> configs{
        {"app.name", "caf-plugin-system"},
        {"app.version", "1.0.0"},
        {"log.level", "INFO"}
    };
    std::unordered_map<std::string, int> metrics;
};

class PlatformPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {
            "PlatformPlugin",
            "1.0.0",
            {},
            {
                "logging_service",
                "config_service",
                "metrics_service"
            },
            -100
        };
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {

        return sys.spawn([](caf::stateful_actor<PlatformState>* self) -> caf::behavior {
            return caf::behavior{
                // ---------- logging_service ----------
                [=](log_atom, const std::string& level, const std::string& msg) {
                    self->state.log_count++;
                    std::cout << "[PlatformLog][" << level << "] " << msg << std::endl;
                },

                // ---------- config_service ----------
                [=](get_config_atom, const std::string& key) -> std::string {
                    auto it = self->state.configs.find(key);
                    return (it != self->state.configs.end()) ? it->second : "";
                },
                [=](set_config_atom, const std::string& key, const std::string& val) {
                    self->state.configs[key] = val;
                    std::cout << "[PlatformConfig] Set: " << key << " = " << val << std::endl;
                },

                // ---------- metrics_service ----------
                [=](report_metric_atom, const std::string& key, int delta) {
                    self->state.metrics[key] += delta;
                },
                [=](get_metrics_atom) -> std::unordered_map<std::string, int> {
                    return self->state.metrics;
                },

                // ---------- lifecycle ----------
                [=](init_atom, caf::actor, const std::string&) {
                    std::cout << "[Platform] logging + config + metrics ready ("
                              << self->state.configs.size() << " default configs)" << std::endl;
                },
                [=](drain_atom, caf::actor coordinator) {
                    std::cout << "[Platform] Draining... (handled "
                              << self->state.log_count << " log messages)" << std::endl;
                    self->send(coordinator, drain_atom::value, self->address());
                },
                [=](save_state_atom) -> std::vector<std::byte> {
                    // 简单序列化：log_count(4) + metrics count + metrics data
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &self->state.log_count, sizeof(int));
                    return data;
                },
                [=](restore_state_atom, const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        std::memcpy(&self->state.log_count, data.data(), sizeof(int));
                        std::cout << "[Platform] Restored log_count=" << self->state.log_count << std::endl;
                    }
                },
                [=](shutdown_atom) {
                    std::cout << "[Platform] Shutdown" << std::endl;
                    self->quit();
                }
            };
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new PlatformPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
