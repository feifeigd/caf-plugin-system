#pragma once
// 消息标签全部集中定义于 common/message_tags.def（X-macro 唯一数据源）；
// SystemState 枚举本体前置在 common/system_state.hpp（经 message_tags.hpp 传入），
// 其 CAF 类型注册在 def 里（ID 207）。
#include "common/message_tags.hpp"
#include <caf/all.hpp>
#include <vector>
#include <chrono>
#include <functional>

struct ShutdownConfig {
    std::chrono::seconds drain_timeout{30};
    std::chrono::seconds plugin_stop_timeout{10};
    std::chrono::seconds force_kill_timeout{5};
};

class GracefulShutdown : public caf::event_based_actor {
public:
    GracefulShutdown(caf::actor_config& cfg,
                     ShutdownConfig shutdown_cfg,
                     caf::actor plugin_mgr,
                     caf::actor registry,
                     caf::actor checkpoint_mgr,
                     caf::actor logging_service,
                     std::function<std::vector<std::string>()> get_stop_order);

    caf::behavior make_behavior() override;

private:
    struct State {
        SystemState state = SystemState::initializing;
        std::vector<std::string> remaining_plugins;
        // The shutdown coordinator stops plugins sequentially. Keep the
        // current actor weakly so that we can wait for its down_msg without
        // extending its lifetime. Saving state is not an exit barrier: CAF
        // may still be cleaning the actor mailbox after shutdown_atom.
        caf::actor_addr stopping_plugin;
        std::string stopping_plugin_name;
        bool stopping_plugin_saved = false;
        // 关机最后阶段（finish_shutdown）的组件 barrier：plugin_mgr /
        // registry / checkpoint_mgr / logging_service 逐个 send_exit 后
        // 必须等各自 down_msg——否则组件 mailbox 清理（含插件 down_msg
        // 触发的 unregister 等残留消息）与 actor_system 析构竞态，
        // message_data 双释放（8-31 实锤：trace 显示关机链走完 STOPPED
        // 后仍崩）。同样覆盖集群 actor（ops/master/client）。
        std::vector<caf::actor_addr> stopping_components;
        size_t cluster_ctls_pending = 0;
    };

    ShutdownConfig config_;
    caf::actor plugin_mgr_;
    caf::actor registry_;
    caf::actor checkpoint_mgr_;
    /// 核心日志服务（系统组件）：关机链最后退出（flush 后 quit），
    /// 保证最后一刻的日志（STOPPED）也能落盘。
    caf::actor logging_service_;
    /// 集群/运维控制面 actor 集合（非插件 actor），经 register_cluster_atom
    /// 注册（__main.cpp 共 4 处）：
    ///   - bridge：--bridge-port>0 时外部语言节点 sidecar（external_echo）；
    ///   - master/client：--node-kind 集群节点引导成功时的集群控制 actor；
    ///   - ops：本地运维控制台（任何进程都有，registry "ops"）。
    /// 关机顺序固定：插件保存后 → 集群（本集合）→ 组件（plugin_mgr/
    /// registry/checkpoint_mgr/logging_service），main 不再手动 send_exit。
    /// ops 必须在此：否则 Ctrl+C 路径下它不退出 → actor_system 析构永久
    /// 挂起（历史根因：quit 能退而 Ctrl+C 不能退）。
    std::vector<caf::actor> cluster_ctls_;
    std::function<std::vector<std::string>()> get_stop_order_;
    State state_;
};
