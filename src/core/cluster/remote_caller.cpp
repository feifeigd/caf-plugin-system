#include "cluster/remote_caller.hpp"

#include "common/cluster_types.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>
#include <caf/io/middleman.hpp>

#include <algorithm>
#include <iostream>

namespace caf_plugin_system { namespace cluster {

caf::expected<std::string>
RemoteCaller::call(const std::string& service, const plugin_envelope& env,
                   caf::timespan timeout) {
    // 1. 缓存命中：直接 request；失败 → 清缓存重新 resolve（重试一次）
    {
        std::lock_guard<std::mutex> g(mtx_);
        auto it = cache_.find(service);
        if (it != cache_.end()) {
            auto r = do_request(it->second.target, env, timeout);
            if (r)
                return r;
            std::cout << "[RemoteCaller] cached handle for '" << service
                      << "' failed (" << caf::to_string(r.error())
                      << "), re-resolving" << std::endl;
            cache_.erase(it);
        }
    }
    // 2. 未命中/失效：resolve 后调用
    return resolve_and_call(service, env, timeout);
}

caf::expected<std::string>
RemoteCaller::resolve_and_call(const std::string& service,
                               const plugin_envelope& env,
                               caf::timespan timeout) {
    caf::scoped_actor self{sys_};
    caf::actor target;
    std::string node_name;
    // 查 master 拓扑：找到导出该服务、且非本进程的节点
    self->request(master_, std::chrono::seconds(5), node_topology_atom_v)
        .receive(
          [&](const topology_snapshot& snap) {
              for (const auto& m : snap.nodes) {
                  if (m.node_name == local_node_name_)
                      continue;
                  if (std::find(m.exported_actors.begin(),
                                m.exported_actors.end(),
                                service) == m.exported_actors.end())
                      continue;
                  node_name = m.node_name;
                  auto nid = sys_.middleman().connect(m.host, m.port);
                  if (!nid) {
                      std::cout << "[RemoteCaller] connect to " << m.node_name
                                << " failed: " << caf::to_string(nid.error())
                                << std::endl;
                      return;
                  }
                  auto ptr = sys_.middleman().remote_lookup(service, *nid);
                  if (!ptr) {
                      std::cout << "[RemoteCaller] lookup '" << service
                                << "' at " << m.node_name << " failed"
                                << std::endl;
                      return;
                  }
                  target = caf::actor_cast<caf::actor>(ptr);
                  return;
              }
              std::cout << "[RemoteCaller] service '" << service
                        << "' not registered on any node" << std::endl;
          },
          [&](caf::error& err) {
              std::cout << "[RemoteCaller] topology query failed: "
                        << caf::to_string(err) << std::endl;
          });
    if (!target)
        return caf::make_error(caf::sec::no_such_key,
                               "service not reachable");
    {
        std::lock_guard<std::mutex> g(mtx_);
        cache_[service] = Entry{target, node_name};
    }
    std::cout << "[RemoteCaller] resolved '" << service << "' -> node '"
              << node_name << "' (cached)" << std::endl;
    return do_request(target, env, timeout);
}

caf::expected<std::string>
RemoteCaller::do_request(const caf::actor& target, const plugin_envelope& env,
                         caf::timespan timeout) {
    caf::scoped_actor self{sys_};
    caf::expected<std::string> result =
        caf::make_error(caf::sec::runtime_error, "no response");
    self->request(target, timeout, env)
        .receive(
          [&](std::string& s) { result = std::move(s); },
          [&](caf::error& err) { result = std::move(err); });
    return result;
}

}} // namespace caf_plugin_system::cluster
