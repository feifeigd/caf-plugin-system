#pragma once

#include <caf/all.hpp>
#include <filesystem>
#include <string>
#include <vector>

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
    int priority = 0;                        // 加载优先级：越小越先加载
    // 服务代理 ACL：声明后，本插件提供的所有服务只接受清单内插件的调用，
    // 其余 sender 的消息在代理处被拦截（见 docs/plugin-guide.md §6）。
    // 空 = 开放策略（默认，兼容旧行为）。
    std::vector<std::string> acl_allow;
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
        const std::vector<caf::actor>& injected_deps,   // manifest.dependencies 的 actor 实例，injected_deps 顺序与 manifest.dependencies 一致
        const std::string& config) = 0;

    // ------------------------------------------------------------------
    // 资源目录（框架注入，插件只读）
    // ------------------------------------------------------------------
    // 框架在 load/reload 时自动注入"插件目录"（绝对路径，默认 DLL 所在
    // 目录的父级布局 plugins/<name>/，见 plugin_manager.cpp）。
    // 热更新时新实例【继承】旧实例的资源目录——DLL 换位置（如 stage/），
    // 资源仍读原插件目录，零复制。插件名热更时不可改（manifest 检查），
    // 目录天然稳定。
    // 读资源的插件只需：auto p = asset_path("config.json");
    // 不读资源的插件零改动（默认实现）。
    void set_asset_dir(std::string dir) { asset_dir_ = std::move(dir); }
    const std::string& asset_dir() const { return asset_dir_; }
    // 拼资源绝对路径（Windows 分隔符由 std::filesystem 处理）
    std::string asset_path(const std::string& rel) const {
        return (std::filesystem::path(asset_dir_) / rel).string();
    }

private:
    std::string asset_dir_;
};

// ------------------------------------------------------------------
// C ABI 导出（避免 C++ 符号 mangling 问题）
// ------------------------------------------------------------------
extern "C" {
    PLUGIN_API PluginEntry* create_plugin();
    PLUGIN_API void destroy_plugin(PluginEntry* p);

    // 可选导出：插件私有消息类型的元对象自注册（CAF 1.1 必需，没有私有
    // 消息类型的插件可以不实现）。
    //
    // 若插件定义了自己的消息类型（含自定义标签），在函数体内调用：
    //     caf::detail::set_global_meta_objects(段起点, 元对象表);
    // 框架在构造任何 actor_system 之前调用本导出（CAF 规定之后再注册
    // 是未定义行为；重复注册同一段会 abort，可用于发现段冲突）。
    //
    // type_id 段约定（详见 docs/plugin-guide.md §2/§6）：
    //   200 ~ 999    内核/框架（common/message_tags.def）
    //   1000 ~ 4999  插件私有·系统级（受信组件间通信，默认信任）
    //   5000+        插件私有·用户级（源头可达用户/外部输入，handler
    //                须在副作用前自行校验调用方权限）
    // 每个插件自选互不重叠的段（CAF 支持 ID 空洞，多段可分多次注册）。
    PLUGIN_API void register_meta_objects();
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
