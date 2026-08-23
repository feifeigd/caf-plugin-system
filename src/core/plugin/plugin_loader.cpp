#include "plugin_loader.hpp"
#include "services/logging_service.hpp"
#include <caf/logger.hpp>
#include <deque>
#include <iostream>
#include <unordered_set>
#include <algorithm>
#include <cctype>

namespace {
/// 注册了元对象的插件 DLL 句柄池（进程级常驻）。
/// 元对象里的 destroy/copy 函数指针指向 DLL 代码段，FreeLibrary 后
/// 消息析构会跳到已卸载代码（0xC0000005）。进程退出时由 OS 回收。
std::deque<DynamicLibrary>& meta_lib_pool() {
    static std::deque<DynamicLibrary> pool;
    return pool;
}
} // namespace

// 泄露测试专用：卸载全部 meta 探测库（FreeLibrary 触发 DLL_PROCESS_DETACH，
// 析构 DLL 内静态对象）。调用时机必须晚于所有 actor 消亡（关机链完成、
// actor_system 析构后），否则 meta 函数指针会指向已卸载代码段。
void unload_all_meta_libs() {
    meta_lib_pool().clear();
}

void preregister_plugin_meta(const std::filesystem::path& root) {
    if (!std::filesystem::exists(root)) return;

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory()) continue;
        for (const auto& file : std::filesystem::directory_iterator(entry.path())) {
            auto ext = file.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            std::string expected(PLUGIN_EXT);
            std::transform(expected.begin(), expected.end(), expected.begin(), ::tolower);
            if (ext != expected) continue;

            auto lib_opt = DynamicLibrary::open(file.path());
            if (!lib_opt) continue;  // 打不开的留给 scan_all_plugins 报告

            auto reg = lib_opt->symbol<void (*)()>("register_meta_objects");
            if (!reg) continue;      // 无该导出：插件没有私有消息类型，句柄随作用域释放

            reg();  // 插件自注册私有类型的元对象（此时任何 actor_system 都还不存在）
            // 注意：此刻 CAF logger 尚未创建、fw_logger 未注入，fw_log 走 cout 兜底
            LOG_INFO("[Loader] Plugin self-registered meta objects: "
                                           + file.path().string());
            meta_lib_pool().push_back(std::move(*lib_opt));
        }
    }
}

std::optional<PluginInfo> probe_plugin(const std::filesystem::path& path) {
    auto lib_opt = DynamicLibrary::open(path);
    if (!lib_opt) return std::nullopt;

    auto create  = lib_opt->symbol<CreatePluginFunc>("create_plugin");
    auto destroy = lib_opt->symbol<DestroyPluginFunc>("destroy_plugin");
    if (!create || !destroy) return std::nullopt;

    PluginEntry* plugin = create();
    auto manifest = plugin->manifest();
    destroy(plugin);

    LOG_INFO("Probed plugin: " + manifest.name + " version=" + manifest.version
                + " deps=" + std::to_string(manifest.dependencies.size())
                + " provides=" + std::to_string(manifest.provides.size()));

    return PluginInfo{manifest.name, path, manifest};
}

std::vector<PluginInfo> scan_all_plugins(const std::filesystem::path& root) {
    std::vector<PluginInfo> result;
    if (!std::filesystem::exists(root)) {
        LOG_INFO("Plugin directory not found: " + root.string());
        return result;
    }

    LOG_INFO("Scanning plugin directory: " + root.string());

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

    LOG_INFO("Scan complete: " + std::to_string(result.size()) + " plugin(s) found");
    return result;
}

std::vector<PluginInfo> resolve_dependencies(
    const std::vector<std::string>& entry_plugins,
    const std::vector<PluginInfo>& all_plugins) {

    // 核心内置服务（非插件提供，注册在 ServiceRegistry）：插件 manifest
    // 依赖可直接引用，解析时视为已满足（provider 标记 @core，不加载插件）。
    static const std::unordered_map<std::string, std::string> core_services = {
        {"logging_service", "@core"},
    };

    std::unordered_map<std::string, std::string> service_to_plugin;
    for (const auto& [svc, provider] : core_services)
        service_to_plugin[svc] = provider;
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
            LOG_ERROR("Unknown plugin: " + name);
            continue;
        }

        resolved.push_back(*it->second);

        for (const auto& dep_svc : it->second->manifest.dependencies) {
            auto svc_it = service_to_plugin.find(dep_svc);
            if (svc_it == service_to_plugin.end()) {
                LOG_ERROR("No provider for service: " + dep_svc
                             + " (required by " + name + ")");
                continue;
            }
            if (svc_it->second == "@core")
                continue;  // 核心内置服务：无需加载插件
            stack.push_back(svc_it->second);
        }
    }

    LOG_INFO("Resolved " + std::to_string(resolved.size()) + " plugin(s) from "
                + std::to_string(entry_plugins.size()) + " entry point(s)");
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
            LOG_ERROR("Circular dependency detected involving: " + p.name);
            return {};
        }
    }

    auto order = graph.topological_order();
    LOG_INFO("Computed load order: " + std::to_string(order.size()) + " plugin(s)");
    return order;
}
