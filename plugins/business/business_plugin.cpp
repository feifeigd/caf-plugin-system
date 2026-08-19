#include "plugin_interface.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>

using init_atom = caf::atom_constant<caf::atom("init")>;
using shutdown_atom = caf::atom_constant<caf::atom("shutd")>;
using drain_atom = caf::atom_constant<caf::atom("drain")>;
using save_state_atom = caf::atom_constant<caf::atom("savest")>;
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

            return caf::behavior{
                [=](init_atom, caf::actor, const std::string&) {
                    std::cout << "[Business] Initialized" << std::endl;
                    if (logger) self->send(logger, caf::atom_constant<caf::atom("logmsg")>::value, "INFO", "Business started");
                },
                [=](const std::string& cmd) -> std::string {
                    self->state++;
                    if (logger) {
                        self->send(logger, caf::atom_constant<caf::atom("logmsg")>::value, "DEBUG", "Cmd: " + cmd);
                    }
                    return "processed: " + cmd;
                },
                [=](drain_atom, caf::actor coordinator) {
                    std::cout << "[Business] Draining..." << std::endl;
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
                        std::cout << "[Business] Restored count=" << self->state << std::endl;
                    }
                },
                [=](shutdown_atom) {
                    std::cout << "[Business] Shutdown" << std::endl;
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
