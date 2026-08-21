#pragma once
// ------------------------------------------------------------------
// 跨节点服务调用客户端（模式 B：缓存句柄 + 失败自动重试）
//
// 与验证后门（main.cpp 每次全量 resolve）的区别：RemoteCaller 缓存
// 远端服务句柄，调用失败（超时/不可达/actor 死亡）才清缓存重新
// resolve。节点重启后 node_id 变化，resolve 总是拿到最新句柄，
// 因此调用方无需感知节点生命周期。
//
// 同步阻塞式：call() 内部用 scoped_actor 走完整流程（缓存命中直接
// request；未命中/失效则 master 拓扑 → connect → remote_lookup →
// 缓存 → request，失败重试一次）。多线程调用由内部互斥锁保护缓存。
// ------------------------------------------------------------------

#include "common/plugin_envelope.hpp"

#include <caf/actor.hpp>
#include <caf/expected.hpp>
#include <caf/actor_system.hpp>

#include <chrono>
#include <map>
#include <mutex>
#include <string>

namespace caf_plugin_system { namespace cluster {

class RemoteCaller {
public:
    RemoteCaller(caf::actor_system& sys, caf::actor master,
                 std::string local_node_name)
        : sys_(sys), master_(std::move(master)),
          local_node_name_(std::move(local_node_name)) {}

    /// 同步跨节点调用：返回远端响应字符串或错误。
    /// 失败自动重试一次（清缓存 → 重新 resolve）。
    caf::expected<std::string>
    call(const std::string& service, const plugin_envelope& env,
         caf::timespan timeout = std::chrono::seconds(5));

    /// 清空缓存（强制下次调用重新 resolve）。
    void clear_cache() {
        std::lock_guard<std::mutex> g(mtx_);
        cache_.clear();
    }

private:
    caf::expected<std::string> resolve_and_call(const std::string& service,
                                                const plugin_envelope& env,
                                                caf::timespan timeout);
    caf::expected<std::string> do_request(const caf::actor& target,
                                          const plugin_envelope& env,
                                          caf::timespan timeout);

    caf::actor_system& sys_;
    caf::actor master_;
    std::string local_node_name_;

    struct Entry {
        caf::actor target;
        std::string node_name;
    };
    std::map<std::string, Entry> cache_;
    std::mutex mtx_;
};

}} // namespace caf_plugin_system::cluster
