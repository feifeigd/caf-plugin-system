#include "cluster/remote_caller.hpp"

#include "common/cluster_types.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>
#include <caf/io/middleman.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include "services/logging_service.hpp"
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace caf_plugin_system { namespace cluster {

namespace {

/// 单次远程请求的超时（resolve / connect / call 共用）。
constexpr auto k_call_timeout = std::chrono::seconds(5);
/// 有界重试的轮间间隔。
constexpr auto k_retry_interval = std::chrono::seconds(1);

/// RemoteCaller actor：event-based，per-key 串行队列 + 异步调用状态机。
///
/// 为什么不用 blocking_actor：blocking 模型单线程串行处理全部消息，
/// 一个 key 的长重试（cross_call_ex 最多 attempts×1s）会堵住整个
/// RemoteCaller 线程——其他节点/服务的调用全部排队 = 单点故障放大为
/// 集群级故障。event-based + per-key 队列把"串行"缩小到同一个 key：
///   - 同 key（同服务/同节点）严格串行：in_flight + 等待队列，响应回调
///     才放行下一个 → 有序性保证（见 remote_caller.hpp 头注释）
///   - 不同 key 完全并行：一个节点的重试只阻塞它自己的队列
///   - 异步调用（request/then + delayed_send 重试 tick）不占任何线程，
///     等待期间 actor 继续处理其他 key 的消息
///
/// 状态机（每个 key 独立）：
///   enqueue → (in_flight ? 入队 : start)
///   start → pump
///   pump → routes 空 ? resolve : 试候选
///   候选尝试 → target 就绪 ? call : connect（异步）→ lookup（首连一次）
///   全部候选失败 / resolve 失败 → attempts_left > 0 ? delayed_send 重试
///                                        : deliver 错误 + finish
///   finish → in_flight=false → 队头出队 → start（同 key 串行推进）
///
/// 退出：event-based actor 默认处理 exit_msg；未 settle 的 response_promise
/// 随终止自动交付 sec::actor_died，调用方安全失败。delayed_send 定时器
/// 随 actor 死亡失效，无泄漏。
class RemoteCallerActor : public caf::event_based_actor {
public:
    RemoteCallerActor(caf::actor_config& cfg, caf::actor master,
                      std::string local_node_name)
      : caf::event_based_actor(cfg), master_(std::move(master)),
        local_node_name_(std::move(local_node_name)) {}

    caf::behavior make_behavior() override {
        return {
          // ---- 跨节点调用：任意节点（单次）----
          [this](cross_call_atom, const std::string& svc,
                 plugin_envelope env) -> caf::result<std::string> {
              return enqueue(svc, "", std::move(env), 1);
          },
          // ---- 跨节点调用：指定节点（单次）----
          [this](cross_call_atom, const std::string& svc,
                 const std::string& node_name,
                 plugin_envelope env) -> caf::result<std::string> {
              return enqueue(svc, node_name, std::move(env), 1);
          },
          // ---- 跨节点调用：任意节点 + 有界重试 ----
          [this](cross_call_ex_atom, const std::string& svc, int attempts,
                 plugin_envelope env) -> caf::result<std::string> {
              return enqueue(svc, "", std::move(env), std::max(1, attempts));
          },
          // ---- 跨节点调用：指定节点 + 有界重试 ----
          [this](cross_call_ex_atom, const std::string& svc,
                 const std::string& node_name, int attempts,
                 plugin_envelope env) -> caf::result<std::string> {
              return enqueue(svc, node_name, std::move(env),
                             std::max(1, attempts));
          },
          // ---- 重试定时器：继续推进指定 key 的状态机 ----
          [this](remote_retry_tick_atom, const std::string& key) {
              if (auto it = keys_.find(key); it != keys_.end())
                  pump(it->second);
          },
        };
    }

private:
    using response = caf::typed_response_promise<std::string>;

    /// 单个候选路由：信息 + 惰性建连句柄 + 健康标记。
    struct Route {
        actor_route info;
        caf::actor target;  // connect+lookup 成功后复用
        bool healthy = true;
    };

    /// 单个等待中的调用（同 key 队列元素）。
    struct PendingCall {
        response rp;
        plugin_envelope env;
        int attempts;
    };

    /// 每个 key（svc 或 svc@node）的独立状态。
    struct KeyState {
        std::string svc;
        std::string node_name;  // 空 = 任意节点
        std::vector<Route> routes;
        size_t cursor = 0;       // round-robin 游标
        bool in_flight = false;  // 当前是否有调用在处理
        response rp;             // 当前 in-flight 调用的响应承诺
        plugin_envelope env;     // 当前 in-flight 调用的信封
        int attempts_left = 0;   // 当前调用剩余尝试次数
        std::deque<PendingCall> queue;  // 同 key 等待队列
    };

    static std::string make_key(const std::string& svc,
                                const std::string& node_name) {
        return node_name.empty() ? svc : svc + "@" + node_name;
    }

    /// 入口：in_flight 则排队，否则直接启动。
    caf::result<std::string> enqueue(const std::string& svc,
                                     const std::string& node_name,
                                     plugin_envelope env, int attempts) {
        auto rp = make_response_promise<std::string>();
        auto& ks = keys_[make_key(svc, node_name)];
        ks.svc = svc;
        ks.node_name = node_name;
        if (ks.in_flight) {
            ks.queue.push_back(PendingCall{std::move(rp), std::move(env),
                                           attempts});
        } else {
            start(ks, std::move(rp), std::move(env), attempts);
        }
        return rp;
    }

    /// 启动一次调用：置 in_flight 并泵状态机。
    void start(KeyState& ks, response rp, plugin_envelope env, int attempts) {
        ks.in_flight = true;
        ks.rp = std::move(rp);
        ks.env = std::move(env);
        ks.attempts_left = attempts;
        pump(ks);
    }

    /// 当前调用结束：放行同 key 队头。
    void finish(KeyState& ks) {
        ks.in_flight = false;
        if (ks.queue.empty())
            return;
        auto next = std::move(ks.queue.front());
        ks.queue.pop_front();
        start(ks, std::move(next.rp), std::move(next.env), next.attempts);
    }

    /// 状态机主泵：routes 空则 resolve，否则试候选。
    void pump(KeyState& ks) {
        if (ks.routes.empty()) {
            resolve(ks);
            return;
        }
        try_route(ks);
    }

    /// 从游标起找第一个 healthy 候选；全 unhealthy → 清缓存重新 resolve。
    void try_route(KeyState& ks) {
        for (size_t tried = 0; tried < ks.routes.size(); ++tried) {
            auto idx = (ks.cursor + tried) % ks.routes.size();
            auto& route = ks.routes[idx];
            if (!route.healthy)
                continue;
            ks.cursor = (idx + 1) % ks.routes.size();
            if (route.target) {
                do_call(ks, idx);
                return;
            }
            do_connect(ks, idx);
            return;
        }
        // 全部候选 unhealthy：清缓存，下次尝试重新 resolve 拿最新拓扑
        LOG_INFO(std::string("[RemoteCaller] all candidates unhealthy for '") + ks.svc
                    + "', re-resolving");
        ks.routes.clear();
        resolve(ks);
    }

    /// resolve 失败或候选全败后：有剩余次数则延迟重试，否则交付错误。
    /// 调用方只有在所有尝试都失败后才会收到错误。
    void retry_or_fail(KeyState& ks, caf::error err) {
        if (--ks.attempts_left > 0) {
            LOG_INFO(std::string("[RemoteCaller] attempt failed for '") + ks.svc
                        + "' (" + caf::to_string(err) + "), retrying in "
                        + std::to_string(k_retry_interval.count()) + "s ("
                        + std::to_string(ks.attempts_left) + " left)");
            delayed_send(this, k_retry_interval, remote_retry_tick_atom_v,
                         make_key(ks.svc, ks.node_name));
            return;
        }
        ks.rp.deliver(std::move(err));
        finish(ks);
    }

    /// 异步 resolve：任意模式查服务路由，指定模式查节点路由。
    void resolve(KeyState& ks) {
        if (ks.node_name.empty())
            resolve_any(ks);
        else
            resolve_named(ks);
    }

    void resolve_any(KeyState& ks) {
        request(master_, k_call_timeout, service_resolve_atom_v, ks.svc)
            .then(
              [this, &ks](service_route& route) {
                  ks.routes.clear();
                  for (auto& r : route.routes)
                      if (r.node_name != local_node_name_)
                          ks.routes.push_back(
                            Route{std::move(r), {}, true});
                  if (ks.routes.empty()) {
                      retry_or_fail(
                        ks, caf::make_error(caf::sec::no_such_key,
                                            "service not registered on any node"));
                      return;
                  }
                  LOG_INFO(std::string("[RemoteCaller] resolved '") + ks.svc
                              + "' -> " + std::to_string(ks.routes.size())
                              + " candidate(s)");
                  try_route(ks);
              },
              [this, &ks](caf::error& err) {
                  LOG_INFO(std::string("[RemoteCaller] service resolve '") + ks.svc
                              + "' failed: " + caf::to_string(err));
                  retry_or_fail(ks, std::move(err));
              });
    }

    void resolve_named(KeyState& ks) {
        request(master_, k_call_timeout, node_resolve_atom_v, ks.node_name,
                ks.svc)
            .then(
              [this, &ks](actor_route& route) {
                  ks.routes.clear();
                  ks.routes.push_back(Route{std::move(route), {}, true});
                  LOG_INFO(std::string("[RemoteCaller] resolved '") + ks.svc
                              + "' -> node '" + ks.node_name + "'");
                  try_route(ks);
              },
              [this, &ks](caf::error& err) {
                  LOG_INFO(std::string("[RemoteCaller] resolve '") + ks.svc
                              + "' on node '" + ks.node_name + "' failed: "
                              + caf::to_string(err));
                  retry_or_fail(ks, std::move(err));
              });
    }

    /// 异步 connect + 同步 lookup（仅新节点首连一次，之后复用 target）。
    void do_connect(KeyState& ks, size_t idx) {
        auto& route = ks.routes[idx];
        auto& mm = this->system().middleman();
        request(mm.actor_handle(), k_call_timeout, caf::connect_atom_v,
                route.info.host, route.info.port)
            .then(
              [this, &ks, idx](const caf::node_id& nid,
                               const caf::strong_actor_ptr&,
                               const std::set<std::string>&) {
                  auto& r = ks.routes[idx];
                  // remote_lookup 阻塞，但只在新节点首连时调用一次；
                  // 之后 target 缓存复用，不再走这里。
                  auto ptr = this->system().middleman().remote_lookup(
                    r.info.actor_name, nid);
                  if (!ptr) {
                      LOG_INFO(std::string("[RemoteCaller] lookup '") + r.info.actor_name
                                  + "' at " + r.info.node_name + " failed");
                      r.healthy = false;
                      try_route(ks);
                      return;
                  }
                  r.target = caf::actor_cast<caf::actor>(std::move(ptr));
                  do_call(ks, idx);
              },
              [this, &ks, idx](caf::error& err) {
                  LOG_INFO(std::string("[RemoteCaller] connect to ") + ks.routes[idx].info.node_name
                              + " failed: " + caf::to_string(err));
                  ks.routes[idx].healthy = false;
                  try_route(ks);
              });
    }

    /// 异步调用远端 actor：成功交付，失败标记 unhealthy 换下一个候选。
    void do_call(KeyState& ks, size_t idx) {
        auto& route = ks.routes[idx];
        request(route.target, k_call_timeout, ks.env)
            .then(
              [this, &ks](std::string& res) {
                  ks.rp.deliver(std::move(res));
                  finish(ks);
              },
              [this, &ks, idx](caf::error& err) {
                  LOG_INFO(std::string("[RemoteCaller] call to '") + ks.routes[idx].info.node_name
                              + "' failed (" + caf::to_string(err) + "), failover");
                  ks.routes[idx].healthy = false;
                  try_route(ks);
              });
    }

    caf::actor master_;
    std::string local_node_name_;
    std::unordered_map<std::string, KeyState> keys_;
};

} // namespace

caf::actor spawn_remote_caller(caf::actor_system& sys, caf::actor master,
                               std::string local_node_name) {
    return sys.spawn<RemoteCallerActor>(std::move(master),
                                        std::move(local_node_name));
}

}} // namespace caf_plugin_system::cluster
