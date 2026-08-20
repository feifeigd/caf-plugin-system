#include "plugin_manager.hpp"
#include <caf/logger.hpp>
#include <iostream>
#include <cstring>

PluginManager::PluginManager(caf::actor registry, caf::actor checkpoint_mgr)
    : registry_(registry), checkpoint_mgr_(checkpoint_mgr) {}

auto PluginManager::make_behavior(caf::event_based_actor* self) {
    return caf::behavior{
        [=](load_atom, const std::string& name, const std::string& path) -> caf::result<bool> {
            CAF_LOG_INFO("Loading plugin: " << name);

            if (plugins_.count(name)) {
                CAF_LOG_ERROR("Plugin already loaded: " << name);
                return caf::make_error(caf::sec::invalid_argument, "Already loaded");
            }

            auto lib_opt = DynamicLibrary::open(path);
            if (!lib_opt) {
                CAF_LOG_ERROR("Failed to load library: " << path);
                return caf::make_error(caf::sec::invalid_argument, "Failed to load library");
            }

            auto create  = lib_opt->symbol<CreatePluginFunc>("create_plugin");
            auto destroy = lib_opt->symbol<DestroyPluginFunc>("destroy_plugin");
            if (!create || !destroy) {
                CAF_LOG_ERROR("Missing create/destroy symbols in: " << path);
                return false;
            }

            PluginEntry* plugin = create();
            auto manifest = plugin->manifest();
            CAF_LOG_INFO("Plugin manifest: " << manifest.name
                         << " deps=" << manifest.dependencies.size()
                         << " provides=" << manifest.provides.size());

            for (const auto& svc : manifest.provides) {
                dep_graph_.register_service(svc, name);
            }
            dep_graph_.add_plugin(name, manifest.dependencies);

            if (dep_graph_.has_cycle_from(name)) {
                CAF_LOG_ERROR("Circular dependency detected for: " << name);
                destroy(plugin);
                return caf::make_error(caf::sec::invalid_argument, "Circular dependency");
            }

            std::vector<caf::actor> deps;
            for (const auto& dep : manifest.dependencies) {
                CAF_LOG_INFO("Resolving dependency: " << dep << " for " << name);
                auto dep_actor = self->request(registry_, caf::infinite, resolve_atom::value, dep)
                    .receive([](const caf::actor& a) { return a; }, [](const caf::error&) { return caf::actor{}; });
                if (!dep_actor) {
                    CAF_LOG_ERROR("Missing dependency: " << dep << " for " << name);
                    destroy(plugin);
                    return caf::make_error(caf::sec::invalid_argument, "Missing dep: " + dep);
                }
                deps.push_back(dep_actor);
            }

            auto state_data = self->request(checkpoint_mgr_, std::chrono::seconds(2),
                                           restore_state_atom::value, name)
                .receive([](const std::vector<std::byte>& d) { return d; }, [](auto) { return std::vector<std::byte>{}; });

            auto actor = plugin->spawn(self->system(), deps, "");

            if (!state_data.empty()) {
                CAF_LOG_INFO("Restoring state for: " << name);
                self->send(actor, restore_state_atom::value, state_data);
            }

            self->monitor(actor);
            self->send(actor, init_atom::value, self, "");

            for (const auto& svc : manifest.provides) {
                self->send(registry_, register_atom::value, svc, actor, name);
            }

            plugins_[name] = LoadedPlugin{
                std::move(*lib_opt), plugin, actor, manifest, destroy
            };
            CAF_LOG_INFO("Plugin loaded successfully: " << name);
            return true;
        },

        [=](unload_atom, const std::string& name) -> bool {
            CAF_LOG_INFO("Unloading plugin: " << name);
            auto it = plugins_.find(name);
            if (it == plugins_.end()) {
                CAF_LOG_WARN("Plugin not found for unload: " << name);
                return false;
            }

            for (const auto& svc : it->second.manifest.provides) {
                self->send(registry_, unregister_atom::value, svc);
            }

            self->send_exit(it->second.actor, caf::exit_reason::user_shutdown);

            if (it->second.destroy) {
                it->second.destroy(it->second.instance);
            }
            plugins_.erase(it);
            CAF_LOG_INFO("Plugin unloaded: " << name);
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

        [=](shutdown_atom, caf::actor mgr) {
            shutdown_mgr_ = mgr;
            CAF_LOG_INFO("Shutdown manager registered");
        },

        [=](request_shutdown_atom) {
            if (shutdown_mgr_) {
                CAF_LOG_INFO("Forwarding shutdown request to GracefulShutdown");
                self->send(shutdown_mgr_, shutdown_atom::value);
            } else {
                CAF_LOG_ERROR("Shutdown manager not set");
            }
        },

        [=](const caf::down_msg& dm) {
            for (auto it = plugins_.begin(); it != plugins_.end(); ++it) {
                if (it->second.actor->address() == dm.source) {
                    CAF_LOG_ERROR("Plugin crashed: " << it->first);

                    for (const auto& svc : it->second.manifest.provides) {
                        self->send(registry_, unregister_atom::value, svc);
                    }

                    if (it->second.destroy) {
                        it->second.destroy(it->second.instance);
                    }
                    plugins_.erase(it);
                    break;
                }
            }
        }
    };
}
