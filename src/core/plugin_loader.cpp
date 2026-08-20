#include "plugin_loader.hpp"
#include <iostream>
#include <unordered_set>
#include <algorithm>
#include <cctype>

std::optional<PluginInfo> probe_plugin(const std::filesystem::path& path) {
    auto lib_opt = DynamicLibrary::open(path);
    if (!lib_opt) return std::nullopt;

    auto create  = lib_opt->symbol<CreatePluginFunc>("create_plugin");
    auto destroy = lib_opt->symbol<DestroyPluginFunc>("destroy_plugin");
    if (!create || !destroy) return std::nullopt;

    PluginEntry* plugin = create();
    auto manifest = plugin->manifest();
    destroy(plugin);

    return PluginInfo{manifest.name, path, manifest};
}

std::vector<PluginInfo> scan_all_plugins(const std::filesystem::path& root) {
    std::vector<PluginInfo> result;
    if (!std::filesystem::exists(root)) return result;

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
            auto ext = file.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            std::string expected(PLUGIN_EXT);
            std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
            if (ext != expected) continue;

            if (auto info = probe_plugin(file.path())) {
                result.push_back(std::move(*info));
            }
        }
    }
    return result;
}

std::vector<PluginInfo> resolve_dependencies(
    const std::vector<std::string>& entry_plugins,
    const std::vector<PluginInfo>& all_plugins) {

    std::unordered_map<std::string, std::string> service_to_plugin;
    for (const auto& p : all_plugins) {
        for (const auto& svc : p.manifest.provides) {
            service_to_plugin[svc] = p.name;
        }
    }

    std::unordered_map<std::string, const PluginInfo*> plugin_map;
    for (const auto& p : all_plugins) {
        plugin_map[p.name] = &p;
    }

    std::unordered_set<std::string> visited;
    std::vector<PluginInfo> resolved;
    std::vector<std::string> stack = entry_plugins;

    while (!stack.empty()) {
        auto name = stack.back();
        stack.pop_back();

        if (visited.count(name)) continue;
        visited.insert(name);

        auto it = plugin_map.find(name);
        if (it == plugin_map.end()) {
            std::cerr << "[Deps] Unknown plugin: " << name << std::endl;
            continue;
        }

        resolved.push_back(*it->second);

        for (const auto& dep_svc : it->second->manifest.dependencies) {
            auto svc_it = service_to_plugin.find(dep_svc);
            if (svc_it == service_to_plugin.end()) {
                std::cerr << "[Deps] No provider for service: " << dep_svc
                          << " (required by " << name << ")" << std::endl;
                continue;
            }
            stack.push_back(svc_it->second);
        }
    }

    return resolved;
}

std::vector<std::string> compute_load_order(const std::vector<PluginInfo>& plugins) {
    DependencyGraph graph;
    for (const auto& p : plugins) {
        for (const auto& svc : p.manifest.provides) {
            graph.register_service(svc, p.name);
        }
    }
    for (const auto& p : plugins) {
        graph.add_plugin(p.name, p.manifest.dependencies);
        graph.set_priority(p.name, p.manifest.priority);
    }

    for (const auto& p : plugins) {
        if (graph.has_cycle_from(p.name)) {
            std::cerr << "[Init] Circular dependency: " << p.name << std::endl;
            return {};
        }
    }

    return graph.topological_order();
}
