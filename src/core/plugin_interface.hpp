#pragma once
#include <string>
#include <vector>
#include <caf/all.hpp>

// ------------------------------------------------------------------
// 跨平台导出宏
// ------------------------------------------------------------------
#ifdef _WIN32
    #define PLUGIN_API __declspec(dllexport)
#else
    #define PLUGIN_API __attribute__((visibility("default")))
#endif

// ------------------------------------------------------------------
// 插件元数据
// ------------------------------------------------------------------
struct plugin_manifest {
    std::string name;
    std::string version;
    std::vector<std::string> dependencies;   // 依赖的服务名
    std::vector<std::string> provides;       // 提供的服务名
    int priority = 0;                        // ← 加载优先级（越小越先加载，负数给基础设施）
};
    std::string name;
    std::string version;
    std::vector<std::string> dependencies;   // 依赖的服务名
    std::vector<std::string> provides;       // 提供的服务名
};

// ------------------------------------------------------------------
// 最小插件接口：只做"创建"和"描述"，不做业务
// ------------------------------------------------------------------
class PluginEntry {
public:
    virtual ~PluginEntry() = default;

    // 获取元数据（加载时调用）
    virtual plugin_manifest manifest() const = 0;

    // 创建插件 Actor（这是唯一的工厂方法）
    virtual caf::actor spawn(
        caf::actor_system& sys,
        const std::vector<caf::actor>& injected_deps,
        const std::string& config) = 0;
};

// ------------------------------------------------------------------
// C ABI 导出（避免 C++ 符号 mangling 问题）
// ------------------------------------------------------------------
extern "C" {
    PLUGIN_API PluginEntry* create_plugin();
    PLUGIN_API void destroy_plugin(PluginEntry* p);
}

// ------------------------------------------------------------------
// 跨平台动态库扩展名
// ------------------------------------------------------------------
#ifdef _WIN32
    constexpr const char* PLUGIN_EXT = ".dll";
#elif __APPLE__
    constexpr const char* PLUGIN_EXT = ".dylib";
#else
    constexpr const char* PLUGIN_EXT = ".so";
#endif
