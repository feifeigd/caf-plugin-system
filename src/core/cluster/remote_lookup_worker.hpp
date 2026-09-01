#pragma once

#include <caf/actor.hpp>
#include <caf/actor_addr.hpp>
#include <caf/actor_system.hpp>

namespace caf_plugin_system::cluster {

/// 启动 named remote lookup worker。owner 终止后 worker 自动退出。
caf::actor spawn_remote_lookup_worker(caf::actor_system& sys,
                                      caf::actor_addr owner);

} // namespace caf_plugin_system::cluster
