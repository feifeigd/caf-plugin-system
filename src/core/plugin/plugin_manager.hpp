#pragma once
#include "plugin_interface.hpp"
#include "dynamic_library.hpp"
#include "service_registry.hpp"
#include "dependency_graph.hpp"
#include "graceful_shutdown.hpp"
#include <map>

// 消息标签集中定义于 common/message_tags.def（X-macro 唯一数据源）

// 插件 C ABI 函数签名
using CreatePluginFunc  = PluginEntry* (*)();
using DestroyPluginFunc = void (*)(PluginEntry*);

struct LoadedPlugin {
    // 非拥有指针，指向 plugin_manager.cpp 的进程级句柄池。
    // 插件内 sys.spawn 的 actor 实现类/lambda 在 DLL 代码段实例化，
    // CAF 的引用计数释放是异步的，FreeLibrary 过早会把 vtable 变成野指针。
    // 因此 DLL 句柄常驻到进程退出，由 OS 回收。
    DynamicLibrary* lib = nullptr;
    PluginEntry* instance = nullptr;
    caf::actor actor;
    plugin_manifest manifest;
    DestroyPluginFunc destroy = nullptr;
};

// 卸载全部插件库（与 unload_all_meta_libs 同目的：泄露测试时在 actor_system
// 析构后调用，让 DLL 静态对象随 detach 释放）。调用时机必须晚于所有插件
// actor 消亡，否则 vtable/函数指针指向已卸载代码段。
void unload_all_plugin_libs();

class PluginManager : public caf::event_based_actor {
public:
    PluginManager(caf::actor_config& cfg, caf::actor registry, caf::actor checkpoint_mgr);

    caf::behavior make_behavior() override;

private:
    caf::actor registry_;
    caf::actor checkpoint_mgr_;
    DependencyGraph dep_graph_;
    std::map<std::string, LoadedPlugin> plugins_;
    // 热更新退役的旧实例：等旧 actor 排空退出（down_msg）后再销毁。
    // 旧 DLL 句柄仍由句柄池持有，不随实例卸载。
    std::vector<LoadedPlugin> retired_;
};
