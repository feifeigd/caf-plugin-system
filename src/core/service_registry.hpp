#pragma once
#include "plugin/plugin_interface.hpp"
#include "graceful_shutdown.hpp" // drain_atom
#include <functional>
#include <map>
#include <string>
#include <typeindex>

// 消息标签集中定义于 common/message_tags.def（X-macro 唯一数据源）

enum class service_lifetime { singleton, transient, scoped };

struct VersionedEntry {
    std::string name;
    caf::actor proxy;   // 通过 spawn_service_proxy 生成的代理 actor，负责转发消息到 impl
    caf::actor impl;
    int version = 1;    // 每次热更新时递增，供调用方检查是否过期
    std::string plugin_name;    // dll 注册的名字
};

caf::actor spawn_service_proxy(caf::actor_system& sys, caf::actor initial_target);

class ServiceRegistry : public caf::event_based_actor {
public:
    explicit ServiceRegistry(caf::actor_config& cfg) : caf::event_based_actor(cfg) {}

    caf::behavior make_behavior() override;

private:
    std::map<std::string, VersionedEntry> services_;
};

