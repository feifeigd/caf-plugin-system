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

// 扫描插件目录，预加载所有插件获取 manifest
std::vector<PluginInfo> scan_all_plugins(const std::filesystem::path& root);

// 从入口插件出发，递归解析依赖链，返回所有需要加载的插件（含依赖）
std::vector<PluginInfo> resolve_dependencies(
    const std::vector<std::string>& entry_plugins,
    const std::vector<PluginInfo>& all_plugins);

// 对插件集合做拓扑排序，返回加载顺序（依赖在前，被依赖在后）
std::vector<std::string> compute_load_order(const std::vector<PluginInfo>& plugins);
