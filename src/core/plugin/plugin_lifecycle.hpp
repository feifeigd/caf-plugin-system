#pragma once
// ------------------------------------------------------------------
// 插件公共生命周期协议（框架固化）
//
// 插件最常见的样板代码：5 个生命周期 handler（init/drain/save/
// restore/shutdown）——每个插件都要写一遍，且容易写错（drain 忘回执、
// shutdown 忘 quit）。本文件把生命周期消息的处理骨架固化在框架侧：
//
//   - init       ：分发到 hooks.on_init（插件初始化、记录 manager）
//   - drain      ：框架统一回执（必须带 self->address()，忘回执会卡死
//                  关机链）；hooks.on_drain 可选
//   - save_state ：分发到 hooks.on_save（状态序列化是插件业务）
//   - restore    ：分发到 hooks.on_restore
//   - shutdown   ：hooks.on_shutdown（可选清理）→ 框架统一 self->quit()
//
// 使用方式（插件 spawn 内）：
//
//   caf::message_handler business{ /* 私有业务 handler */ };
//   return caf::behavior{business.or_else(plugin_lifecycle(self, hooks))};
//
//   - 私有在前：业务消息一次命中（高频路径最优）；
//   - 插件想在业务列表里写同名生命周期 handler 时自动优先
//     （or_else 语义：先匹配业务），覆盖框架默认——统一性和灵活性兼得；
//   - hooks 回调全部默认空实现，插件只注册自己关心的。
//
// 注意：CAF 1.1 中 or_else 是 caf::message_handler 的成员（behavior 不是
// message_handler 的子类），因此本函数返回 message_handler，由插件包进
// caf::behavior{...}。
//
// 与热更新/退役的屏障语义无关：save_state 仍是邮箱到达序屏障，
// PM/GS 的消息协议完全不变，本文件只重组插件侧的实现骨架。
// ------------------------------------------------------------------

#include "plugin_interface.hpp"
#include "graceful_shutdown.hpp"
#include <caf/all.hpp>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

/// 插件生命周期回调（全部默认空实现，按需注册）。
struct PluginLifecycleHooks {
    /// 初始化：manager = PluginManager actor（可用来发 request_shutdown），
    /// cfg = 插件配置字符串。
    std::function<void(caf::actor manager, const std::string& cfg)> on_init;
    /// 排空（可选）：框架在回执前调用。默认空 = 无排空动作，直接回执。
    std::function<void()> on_drain;
    /// 保存状态（可选）：返回序列化状态；默认空 = 返回空（无状态插件）。
    std::function<std::vector<std::byte>()> on_save;
    /// 恢复状态（可选）。
    std::function<void(const std::vector<std::byte>&)> on_restore;
    /// 关机清理（可选）：框架在 self->quit() 之前调用。
    std::function<void()> on_shutdown;
    /// 服务注销通知（可选）：其他插件 unload 时，PM 广播 service_gone——
    /// 本插件声明的服务名列表。持有者在此释放缓存的代理强引用（跨插件
    /// 引用环的协作式断点）；子 actor 的弱引用（actor_addr）无需通知。
    std::function<void(const std::vector<std::string>&)> on_service_removed;
};

/// 框架公共生命周期 behavior。与插件私有业务 handler 用 or_else 组合：
///   caf::behavior{business.or_else(plugin_lifecycle(self, hooks))}
/// 注意：本函数返回的 message_handler 必须由插件 actor 持有（self 指向
/// 该 actor），且要包进 caf::behavior 才能作为 actor 的最终行为。
inline caf::message_handler plugin_lifecycle(caf::event_based_actor* self,
                                             const PluginLifecycleHooks& hooks) {
    return caf::message_handler{
        [=](init_atom, caf::actor manager, const std::string& cfg) {
            if (hooks.on_init)
                hooks.on_init(std::move(manager), cfg);
        },
        // drain 回执是框架协议：必须回 coordinator 并带上自己的 address，
        // 否则 GracefulShutdown 的停机链卡死。框架统一处理。
        [=](drain_atom, caf::actor coordinator) {
            if (hooks.on_drain)
                hooks.on_drain();
            self->send(coordinator, drain_atom{}, self->address());
        },
        [=](save_state_atom) -> std::vector<std::byte> {
            return hooks.on_save ? hooks.on_save()
                                 : std::vector<std::byte>{};
        },
        [=](restore_state_atom, const std::vector<std::byte>& data) {
            if (hooks.on_restore)
                hooks.on_restore(data);
        },
        [=](shutdown_atom) {
            if (hooks.on_shutdown)
                hooks.on_shutdown();
            // 退出语义框架统一：插件忘 quit 的坑消失（不 quit 会导致
            // 退役流程等不到 down_msg，retired_ 泄漏）。
            self->quit();
        },
        // 服务注销通知（框架统一接住，防 print_and_drop 噪音）：PM 统一
        // 解绑广播（其他插件 unload 时）——插件在 on_service_removed 钩子
        // 里释放缓存的代理强引用；不注册钩子 = 无操作（广播静默消化）。
        [=](service_gone_atom, const std::vector<std::string>& gone) {
            if (hooks.on_service_removed)
                hooks.on_service_removed(gone);
        }};
}
