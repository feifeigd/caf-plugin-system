#include "plugin_interface.hpp"
#include "services/config_service.hpp"
#include "services/metrics_service.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>
#include <map>

using init_atom    = caf::atom_constant<caf::atom("init")>;
using shutdown_atom = caf::atom_constant<caf::atom("shutd")>;
using drain_atom   = caf::atom_constant<caf::atom("drain")>;
using save_state_atom = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

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
            {},
            {"config_service", "metrics_service"},
            -100
        };
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {

        return sys.spawn([](caf::stateful_actor<PlatformState>* self) -> caf::behavior {
            return caf::behavior{
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
                [=](get_metrics_atom) -> std::map<std::string, int> {
                    return self->state.metrics;
                },

                // ---------- lifecycle ----------
                [=](init_atom, caf::actor, const std::string&) {
                    std::cout << "[Platform] config + metrics ready ("
                              << self->state.configs.size() << " default configs)" << std::endl;
                },
                [=](drain_atom, caf::actor coordinator) {
                    std::cout << "[Platform] Draining..." << std::endl;
                    self->send(coordinator, drain_atom::value, self->address());
                },
                [=](save_state_atom) -> std::vector<std::byte> {
                    int cfg_count = static_cast<int>(self->state.configs.size());
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &cfg_count, sizeof(int));
                    return data;
                },
                [=](restore_state_atom, const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        int cfg_count = 0;
                        std::memcpy(&cfg_count, data.data(), sizeof(int));
                        std::cout << "[Platform] Restored " << cfg_count << " configs" << std::endl;
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
