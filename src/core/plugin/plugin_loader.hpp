#pragma once
#include "plugin_manager.hpp"
#include "dependency_graph.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>

// ------------------------------------------------------------------
// 插件信息（预加载阶段获取）
// ------------------------------------------------------------------
struct PluginInfo {
    std::string name;
    std::filesystem::path path;
    plugin_manifest manifest;
};

// 预加载插件：打开动态库 → create_plugin() → manifest() → destroy()，不创建 actor
std::optional<PluginInfo> probe_plugin(const std::filesystem::path& path);

// 元对象预注册：扫描插件目录，调用各插件的可选导出 register_meta_objects()
// （见 plugin_interface.hpp）。CAF 1.1 禁止在 actor_system 构造之后注册
// 元对象（UB），因此必须在 main() 里、构造 actor_system 之前调用本函数。
// 含有该导出的 DLL 会常驻进程（元对象函数指针指向 DLL 代码段）。
void preregister_plugin_meta(const std::filesystem::path& root);

// 卸载全部 meta 探测库（泄露测试专用后门：DLL_PROCESS_DETACH 析构 DLL
// 静态对象，让 CRT 报告区分"DLL 常驻分配"与"真泄露"。必须在所有 actor
// 死光后调用，否则元对象函数指针会指向已卸载代码段）。
void unload_all_meta_libs();

// 扫描插件目录，预加载所有插件获取 manifest
std::vector<PluginInfo> scan_all_plugins(const std::filesystem::path& root);

// 从入口插件出发，递归解析依赖链，返回所有需要加载的插件（含依赖）
std::vector<PluginInfo> resolve_dependencies(
    const std::vector<std::string>& entry_plugins,
    const std::vector<PluginInfo>& all_plugins);

// 对插件集合做拓扑排序，返回加载顺序（依赖在前，被依赖在后）
std::vector<std::string> compute_load_order(const std::vector<PluginInfo>& plugins);
