#include "cluster_client.hpp"
#include "cluster_membership.hpp"
#include "common/message_tags.hpp"

#include <caf/io/middleman.hpp>
#include <caf/logger.hpp>

#include <iostream>
#include <optional>

namespace caf_plugin_system {

bool parse_node_kind(const std::string& s, node_kind& out) {
  if (s == "master") { out = node_kind::master; return true; }
  if (s == "region") { out = node_kind::region; return true; }
  if (s == "worker") { out = node_kind::worker; return true; }
  return false;
}

namespace {

/// 状态机阶段：连接中 / 已注册 / 重连等待。
enum class client_state { connecting, registered, reconnecting };

/// 尝试 connect + remote_lookup master 注册表 actor。
caf::actor connect_master(caf::event_based_actor* self,
                          const node_client_config& cfg) {
  auto nid = self->system().middleman().connect(cfg.master_host, cfg.master_port);
  if (!nid) {
    self->println("[NodeClient:{}] connect master {}:{} failed: {}",
                  cfg.node_name, cfg.master_host, cfg.master_port,
                  caf::to_string(nid.error()));
    return {};
  }
  // CAF 1.1：remote_lookup 返回 strong_actor_ptr（空 = 无此命名 actor）
  auto remote = self->system().middleman().remote_lookup(cfg.master_registry_name, *nid);
  if (!remote) {
    self->println("[NodeClient:{}] lookup '{}' failed (not registered)",
                  cfg.node_name, cfg.master_registry_name);
    return {};
  }
  return caf::actor_cast<caf::actor>(std::move(remote));
}

/// 重试定时器：延迟 retry_delay 后触发 retry_tick。
void schedule_retry(caf::event_based_actor* self,
                    std::chrono::milliseconds retry_delay) {
  self->delayed_send(self, retry_delay, retry_tick_atom_v);
}

} // namespace

caf::actor spawn_node_client(caf::actor_system& sys,
                             node_client_config config,
                             caf::actor local_monitor) {
  return sys.spawn([cfg = std::move(config),
                    local_monitor](caf::event_based_actor* self) -> caf::behavior {
    // 状态（随 actor 存活）
    auto state = std::make_shared<client_state>(client_state::connecting);
    auto master = std::make_shared<caf::actor>();
    auto heartbeat_period = std::chrono::milliseconds(3000);
    if (cfg.lease_ttl.count() > 0)
      heartbeat_period = cfg.lease_ttl / 3;

    auto do_register = [self, cfg, master, state, local_monitor, heartbeat_period]() {
      *master = connect_master(self, cfg);
      if (!*master) {
        *state = client_state::reconnecting;
        self->println("[NodeClient:{}] master unreachable, retrying in 1s",
                      cfg.node_name);
        schedule_retry(self, std::chrono::seconds(1));
        return;
      }
      node_manifest manifest{cfg.kind, cfg.node_name, cfg.host, cfg.port,
                             cfg.parent, cfg.exported_actors};
      self->request(*master, std::chrono::seconds(5), node_register_atom_v,
                    node_registration{manifest, local_monitor})
        .then(
          [self, cfg, master, state, heartbeat_period](const register_reply& reply) {
            self->println("[NodeClient:{}] register: {} ({})", cfg.node_name,
                          reply.ok ? "OK" : "REJECTED", reply.message);
            *state = client_state::registered;
            // 注册成功后立即进入心跳循环
            self->delayed_send(self, heartbeat_period, heartbeat_tick_atom_v);
          },
          [self, cfg, master, state, heartbeat_period](const caf::error& err) {
            self->println("[NodeClient:{}] register failed: {}", cfg.node_name,
                          caf::to_string(err));
            *state = client_state::reconnecting;
            schedule_retry(self, std::chrono::seconds(1));
          });
    };

    return caf::behavior{
      // 启动：先尝试注册（连接 + lookup + register 全链路）
      [self, cfg, state, do_register](init_atom) {
        if (*state == client_state::connecting)
          do_register();
      },

      // 心跳 tick：向 master 续租
      [self, cfg, master, state, heartbeat_period](heartbeat_tick_atom) {
        if (*state != client_state::registered || !*master) {
          *state = client_state::reconnecting;
          schedule_retry(self, std::chrono::seconds(1));
          return;
        }
        self->request(*master, std::chrono::seconds(3), node_heartbeat_atom_v,
                      cfg.node_name)
          .then(
            [self, cfg, master, state, heartbeat_period](const register_reply& reply) {
              if (!reply.ok) {
                self->println("[NodeClient:{}] heartbeat rejected: {}",
                              cfg.node_name, reply.message);
                *state = client_state::reconnecting;
                schedule_retry(self, std::chrono::seconds(1));
                return;
              }
              self->delayed_send(self, heartbeat_period, heartbeat_tick_atom_v);
            },
            [self, cfg, master, state, heartbeat_period](const caf::error&) {
              // master 不可达：清句柄，进入重连
              *master = caf::actor{};
              *state = client_state::reconnecting;
              self->println("[NodeClient:{}] master lost, reconnecting",
                            cfg.node_name);
              schedule_retry(self, std::chrono::seconds(1));
            });
      },

      // 重连 tick：master 恢复后自动重新注册
      [self, cfg, state, do_register](retry_tick_atom) {
        if (*state == client_state::connecting
            || *state == client_state::reconnecting)
          do_register();
      },
    };
  });
}

} // namespace caf_plugin_system
