#include "cluster/remote_lookup_worker.hpp"

#include <caf/all.hpp>
#include <caf/io/middleman.hpp>

#include <string>

namespace caf_plugin_system::cluster {

namespace {

void remote_lookup_worker(caf::blocking_actor* self, caf::actor_addr owner) {
    self->monitor(owner);
    bool running = true;
    while (running) {
        self->receive(
          [self](const std::string& actor_name,
                 const caf::node_id& nid) -> caf::result<caf::actor> {
              auto ptr = self->system().middleman().remote_lookup(actor_name, nid);
              if (!ptr) {
                  return caf::make_error(caf::sec::remote_lookup_failed,
                                         "remote actor not registered: "
                                           + actor_name);
              }
              return caf::actor_cast<caf::actor>(std::move(ptr));
          },
          [&running, owner](const caf::down_msg& msg) {
              if (msg.source == owner)
                  running = false;
          });
    }
}

} // namespace

caf::actor spawn_remote_lookup_worker(caf::actor_system& sys,
                                      caf::actor_addr owner) {
    return sys.spawn(remote_lookup_worker, std::move(owner));
}

} // namespace caf_plugin_system::cluster
