#pragma once
#include "plugin_interface.hpp"
#include <typeindex>
#include <functional>

enum class service_lifetime { singleton, transient, scoped };

struct VersionedEntry {
    std::string name;
    caf::actor proxy;
    caf::actor impl;
    int version = 1;
    std::string plugin_name;
};

caf::actor spawn_service_proxy(caf::actor_system& sys, caf::actor initial_target);

class ServiceRegistry {
public:
    auto make_behavior(caf::event_based_actor* self);
};
