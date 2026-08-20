#pragma once
#include "plugin_interface.hpp"
#include "dynamic_library.hpp"
#include "service_registry.hpp"
#include "dependency_graph.hpp"
#include <map>

using load_atom          = caf::atom_constant<caf::atom("load")>;
using unload_atom        = caf::atom_constant<caf::atom("unload")>;
using list_atom          = caf::atom_constant<caf::atom("list")>;
using resolve_plugin_atom = caf::atom_constant<caf::atom("resplug")>;
using ping_atom          = caf::atom_constant<caf::atom("ping")>;
using pong_atom          = caf::atom_constant<caf::atom("pong")>;
using init_atom          = caf::atom_constant<caf::atom("init")>;

// 插件 C ABI 函数签名
using CreatePluginFunc  = PluginEntry* (*)();
using DestroyPluginFunc = void (*)(PluginEntry*);

struct LoadedPlugin {
    DynamicLibrary lib;
    PluginEntry* instance = nullptr;
    caf::actor actor;
    plugin_manifest manifest;
    DestroyPluginFunc destroy = nullptr;   // 缓存，避免重复 dlsym
};

class PluginManager {
public:
    explicit PluginManager(caf::actor registry, caf::actor checkpoint_mgr);
    auto make_behavior(caf::event_based_actor* self);

private:
    caf::actor registry_;
    caf::actor checkpoint_mgr_;
    DependencyGraph dep_graph_;
    std::map<std::string, LoadedPlugin> plugins_;
};
