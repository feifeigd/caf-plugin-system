#pragma once
#include <caf/all.hpp>

// ------------------------------------------------------------------
// 生命周期与框架级 atom 集中定义
//
// 插件通过 #include "plugin_interface.hpp" 间接获得这些定义
// 框架代码可直接 #include "common/lifecycle_atoms.hpp"
// ------------------------------------------------------------------

// ---------- 插件生命周期 ----------
using init_atom          = caf::atom_constant<caf::atom("init")>;
using shutdown_atom      = caf::atom_constant<caf::atom("shutd")>;
using drain_atom         = caf::atom_constant<caf::atom("drain")>;
using save_state_atom    = caf::atom_constant<caf::atom("savest")>;
using restore_state_atom = caf::atom_constant<caf::atom("restore")>;

// ---------- 系统级控制 ----------
using ready_atom            = caf::atom_constant<caf::atom("ready")>;
using request_shutdown_atom = caf::atom_constant<caf::atom("rqshut")>;
using force_exit_atom       = caf::atom_constant<caf::atom("force")>;
using health_check_atom     = caf::atom_constant<caf::atom("hcheck")>;
using plugin_saved_atom     = caf::atom_constant<caf::atom("plgsaved")>;

// ---------- 插件管理 ----------
using load_atom           = caf::atom_constant<caf::atom("load")>;
using unload_atom         = caf::atom_constant<caf::atom("unload")>;
using list_atom           = caf::atom_constant<caf::atom("list")>;
using resolve_plugin_atom = caf::atom_constant<caf::atom("resplug")>;
using ping_atom           = caf::atom_constant<caf::atom("ping")>;
using pong_atom           = caf::atom_constant<caf::atom("pong")>;

// ---------- 服务注册 ----------
using register_atom      = caf::atom_constant<caf::atom("register")>;
using resolve_atom       = caf::atom_constant<caf::atom("resolve")>;
using list_services_atom = caf::atom_constant<caf::atom("ls")>;
using hot_reload_atom    = caf::atom_constant<caf::atom("reload")>;
using unregister_atom    = caf::atom_constant<caf::atom("unreg")>;
using switch_target_atom = caf::atom_constant<caf::atom("swtch")>;
