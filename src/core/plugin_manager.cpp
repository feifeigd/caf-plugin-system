#include "plugin_manager.hpp"
#include <iostream>
#include <cstring>

PluginManager::PluginManager(caf::actor registry, caf::actor checkpoint_mgr)
    : registry_(registry), checkpoint_mgr_(checkpoint_mgr) {}

auto PluginManager::make_behavior(caf::event_based_actor* self) {
    return caf::behavior{
        [=](load_atom, const std::string& name, const std::string& path) -> caf::result<bool> {
            if (plugins_.count(name)) {
                return caf::make_error(caf::sec::invalid_argument, "Already loaded");
            }

            auto lib_opt = DynamicLibrary::open(path);
            if (!lib_opt) {
                return caf::make_error(caf::sec::invalid_argument, "Failed to load library");
            }

            auto create = lib_opt->symbol<PluginEntry>("create_plugin");
            auto destroy = lib_opt->symbol<void>("destroy_plugin");
            if (!create || !destroy) {
                return false;
            }

            PluginEntry* plugin = create();
            auto manifest = plugin->manifest();

            for (const auto& svc : manifest.provides) {
                dep_graph_.register_service(svc, name);
            }
            dep_graph_.add_plugin(name, manifest.dependencies);

            if (dep_graph_.has_cycle_from(name)) {
                destroy(plugin);
                return caf::make_error(caf::sec::invalid_argument, "Circular dependency");
            }

            std::vector<caf::actor> deps;
            for (const auto& dep : manifest.dependencies) {
                auto dep_actor = self->request(registry_, caf::infinite, resolve_atom::value, dep)
                    .receive([](const caf::actor& a) { return a; }, [](const caf::error&) { return caf::actor{}; });
                if (!dep_actor) {
                    destroy(plugin);
                    return caf::make_error(caf::sec::invalid_argument, "Missing dep: " + dep);
                }
                deps.push_back(dep_actor);
            }

            auto state_data = self->request(checkpoint_mgr_, std::chrono::seconds(2),
                                           restore_state_atom::value, name)
                .receive([](const std::vector<uint8_t>& d) { return d; }, [](auto) { return std::vector<uint8_t>{}; });

            auto actor = plugin->spawn(self->system(), deps, "");

            if (!state_data.empty()) {
                self->send(actor, restore_state_atom::value, state_data);
            }

            self->monitor(actor);
            self->send(actor, init_atom::value, self, "");

            for (const auto& svc : manifest.provides) {
                self->send(registry_, register_atom::value, svc, actor, name);
            }

            plugins_[name] = LoadedPlugin{std::move(*lib_opt), plugin, actor, manifest};
            std::cout << "[PluginManager] Loaded: " << name << std::endl;
            return true;
        },

        [=](unload_atom, const std::string& name) -> bool {
            auto it = plugins_.find(name);
            if (it == plugins_.end()) return false;

            self->send_exit(it->second.actor, caf::exit_reason::user_shutdown);

            auto destroy = it->second.lib.symbol<void>("destroy_plugin");
            if (destroy) destroy(it->second.instance);
            plugins_.erase(it);
            return true;
        },

        [=](list_atom) -> std::vector<std::string> {
            std::vector<std::string> names;
            for (const auto& [n, _] : plugins_) names.push_back(n);
            return names;
        },

        [=](resolve_plugin_atom, const std::string& name) -> caf::actor {
            auto it = plugins_.find(name);
            return (it != plugins_.end()) ? it->second.actor : caf::actor{};
        },

        [=](const caf::down_msg& dm) {
            for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
                if (it->second.actor->address() == dm.source) {
                    std::cout << "[PluginManager] Plugin crashed: " << it->first << std::endl;
                    auto destroy = it->second.lib.symbol<void>("destroy_plugin");
                    if (destroy) destroy(it->second.instance);
                    plugins_.erase(it);
                    break;
                }
            }
        }
    };
}
