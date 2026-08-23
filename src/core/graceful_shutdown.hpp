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
    };

    ShutdownConfig config_;
    caf::actor plugin_mgr_;
    caf::actor registry_;
    caf::actor checkpoint_mgr_;
    /// 核心日志服务（系统组件）：关机链最后退出（flush 后 quit），
    /// 保证最后一刻的日志（STOPPED）也能落盘。
    caf::actor logging_service_;
    /// 集群节点 actor（master/client，经 register_cluster_atom 注册）：
    /// 关机时统一终止（插件保存后 → 集群 → 组件），main 不再手动杀。
    std::vector<caf::actor> cluster_ctls_;
    std::function<std::vector<std::string>()> get_stop_order_;
    State state_;
};
