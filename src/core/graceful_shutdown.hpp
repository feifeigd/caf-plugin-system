#pragma once
#include <caf/all.hpp>
#include <vector>
#include <chrono>

using shutdown_atom         = caf::atom_constant<caf::atom("shutd")>;
using drain_atom            = caf::atom_constant<caf::atom("drain")>;
using force_exit_atom       = caf::atom_constant<caf::atom("force")>;
using ready_atom            = caf::atom_constant<caf::atom("ready")>;
using health_check_atom     = caf::atom_constant<caf::atom("hcheck")>;
using plugin_saved_atom     = caf::atom_constant<caf::atom("plgsaved")>;
using request_shutdown_atom = caf::atom_constant<caf::atom("rqshut")>;  // ← 插件请求关机

enum class SystemState { initializing, ready, shutting_down, stopped };

struct ShutdownConfig {
    std::chrono::seconds drain_timeout{30};
    std::chrono::seconds plugin_stop_timeout{10};
    std::chrono::seconds force_kill_timeout{5};
};

class GracefulShutdown {
public:
    explicit GracefulShutdown(ShutdownConfig cfg = {});

    auto make_behavior(caf::event_based_actor* self,
                       caf::actor plugin_mgr,
                       caf::actor registry,
                       caf::actor checkpoint_mgr,
                       std::function<std::vector<std::string>()> get_stop_order);

private:
    ShutdownConfig config_;
};
