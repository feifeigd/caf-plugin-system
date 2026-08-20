#include "plugin_interface.hpp"
#include <cstddef>
#include <iostream>
#include <cstring>

using log_atom = caf::atom_constant<caf::atom("logmsg")>;
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
        return {"LoggerPlugin", "1.0.0", {}, {"logging_service"}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {
        return sys.spawn([](caf::stateful_actor<int>* self) -> caf::behavior {
            self->state = 0;

            return caf::behavior{
                [=](init_atom, caf::actor, const std::string&) {
                    std::cout << "[Logger] Initialized" << std::endl;
                },
                [=](log_atom, const std::string& level, const std::string& msg) {
                    self->state++;
                    std::cout << "[" << level << "] " << msg << std::endl;
                },
                [=](drain_atom, caf::actor coordinator) {
                    std::cout << "[Logger] Draining..." << std::endl;
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
                        std::cout << "[Logger] Restored count=" << self->state << std::endl;
                    }
                },
                [=](shutdown_atom) {
                    std::cout << "[Logger] Shutdown" << std::endl;
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
