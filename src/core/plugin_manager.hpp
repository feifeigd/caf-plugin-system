#pragma once
#include "plugin_interface.hpp"
#include "dynamic_library.hpp"
#include "service_registry.hpp"
#include "dependency_graph.hpp"
#include "graceful_shutdown.hpp"
#include <map>

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
    caf::actor shutdown_mgr_;              // ← GracefulShutdown 引用
    DependencyGraph dep_graph_;
    std::map<std::string, LoadedPlugin> plugins_;
};
