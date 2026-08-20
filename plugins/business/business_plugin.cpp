#include "plugin_interface.hpp"
#include "graceful_shutdown.hpp"
#include "services/logging_service.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>

#define PLUGIN_NAME "business"

using init_atom          = caf::atom_constant<caf::atom("init")>;
using drain_atom         = caf::atom_constant<caf::atom("drain")>;
using save_state_atom    = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

class BusinessPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"BusinessPlugin", "2.0.0", {"logging_service"}, {"business_service"}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = deps.empty() ? caf::actor{} : deps[0];

        return sys.spawn([logger](caf::stateful_actor<int>* self) -> caf::behavior {
            self->state = 0;
            caf::actor plugin_mgr;

            return caf::behavior{
                [=](init_atom, caf::actor manager, const std::string&) {
                    plugin_mgr = manager;
                    LOG_INFO(logger, "BusinessPlugin initialized");
                },
                [=](const std::string& cmd) -> std::string {
                    self->state++;
                    if (cmd == "shutdown") {
                        if (plugin_mgr) {
                            self->send(plugin_mgr, request_shutdown_atom::value);
                            LOG_INFO(logger, "Shutdown requested by command");
                        }
                        return "shutdown requested";
                    }
                    LOG_DEBUG(logger, "Command received: {}", cmd);
                    return "processed: " + cmd;
                },
                [=](drain_atom, caf::actor coordinator) {
                    LOG_INFO(logger, "Draining...");
                    self->send(coordinator, drain_atom::value, self->address());
                },
                [=](save_state_atom) -> std::vector<std::byte> {
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &self->state, sizeof(int));
                    return data;
                },
                [=](restore_state_atom, const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        std::memcpy(&self->state, data.data(), sizeof(int));
                        LOG_INFO(logger, "Restored count={}", self->state);
                    }
                },
                [=](shutdown_atom) {
                    LOG_INFO(logger, "Shutdown");
                    self->quit();
                }
            };
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new BusinessPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
