#pragma once
#include "plugin_interface.hpp"
#include <typeindex>
#include <functional>

using register_atom = caf::atom_constant<caf::atom("register")>;
using resolve_atom = caf::atom_constant<caf::atom("resolve")>;
using list_services_atom = caf::atom_constant<caf::atom("ls")>;
using hot_reload_atom = caf::atom_constant<caf::atom("reload")>;
using switch_target_atom = caf::atom_constant<caf::atom("swtch")>;
using drain_done_atom = caf::atom_constant<caf::atom("drain")>;
using force_cleanup_atom = caf::atom_constant<caf::atom("fcln")>;

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
