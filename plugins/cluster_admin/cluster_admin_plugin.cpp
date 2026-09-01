// cluster_admin_plugin.cpp —— 集群管理插件（只读运维命令）
//
// provides 三个管理服务，经 bridge 行协议 CALL 信封调用（TUI 零改动）：
//   system.ping      → "pong"
//   system.services  → 本节点 registry 服务清单（空格分隔）
//   system.nodes     → master 成员表拓扑（每行: kind name host:port parent）
//
// 句柄来源（core 已暴露，见 framework_bootstrap.cpp / cluster/client.cpp）：
//   sys.registry().get("service_registry") —— ServiceRegistry actor
//   sys.registry().get("cluster.master")   —— master 本地 actor（master 节点）
//                                          或远程 proxy（worker/region 节点）
//
// 跨节点查询：worker 侧对 master proxy 发 node_topology_atom 走 CAF mesh
// 远程调用；master 不可达/非集群节点时返回明确的 ERROR 文本。

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "common/cluster_types.hpp"

#include <caf/event_based_actor.hpp>
#include <caf/actor_registry.hpp>
#include <caf/stateful_actor.hpp>

#include <chrono>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string join_services(const std::vector<std::string>& v) {
    std::ostringstream os;
    for (size_t i = 0; i < v.size(); ++i)
        os << (i ? " " : "") << v[i];
    return os.str();
}

std::string format_topology(const caf_plugin_system::topology_snapshot& snap) {
    std::ostringstream os;
    for (const auto& m : snap.nodes) {
        os << caf_plugin_system::to_string(m.kind) << " " << m.node_name
           << " " << m.host << ":" << m.port
           << " parent=" << (m.parent.empty() ? "-" : m.parent) << "\n";
    }
    return os.str();
}

} // namespace

class ClusterAdminPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        // acl_allow 空 = 开放策略：任何 sender（含 bridge 外部调用）可调
        return {"ClusterAdminPlugin", "0.1.0",  {},
                {"system.nodes", "system.services", "system.ping"},
                0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        // 日志句柄走 DLL 单一实体（exe 已注入），无需 deps 注入
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // registry().get() 返回 strong_actor_ptr，升级为 actor 再 request
        caf::actor reg = caf::actor_cast<caf::actor>(
            sys.registry().get("service_registry"));
        LOG_INFO("ClusterAdminPlugin: service_registry={}",
                 reg ? "ok" : "unavailable");

        return sys.spawn(
            [](caf::event_based_actor* self) -> caf::behavior {
                // 业务 handler 在前；plugin_lifecycle 兜底（drain/save/
                // shutdown → quit）——缺失会让 shutdown_atom 无人处理，
                // 插件 actor 不退出 → 关机后 LeakCheck 残留（实测 +2）。
                caf::message_handler admin{
                    // bridge CALL 转发：plugin_envelope 信封，payload = 命令文本
                    [=](const plugin_envelope& env) {
                        auto rp = self->make_response_promise<std::string>();
                        auto text = plugin_wire::decode_text(env);
                        if (!text) {
                            rp.deliver("ERROR: unsupported payload format");
                            return;
                        }
                        auto cmd = trim(*text);
                        // 句柄每次实时取：node_client 异步连 master，
                        // spawn 时 cluster.master 可能还没 put（插件加载
                        // 早于节点注册成功）。
                        auto reg = caf::actor_cast<caf::actor>(
                            self->system().registry().get("service_registry"));
                        auto master = caf::actor_cast<caf::actor>(
                            self->system().registry().get("cluster.master"));
                        if (cmd == "ping") {
                            rp.deliver(std::string("pong"));
                        } else if (cmd == "services") {
                            if (!reg) {
                                rp.deliver(
                                    std::string("ERROR: service_registry unavailable"));
                                return;
                            }
                            self->request(reg, std::chrono::seconds(5),
                                          list_services_atom_v)
                                .then(
                                    [rp](std::vector<std::string>& svcs) mutable {
                                        rp.deliver(join_services(svcs));
                                    },
                                    [rp](caf::error& e) mutable {
                                        rp.deliver("ERROR: "
                                                   + caf::to_string(e));
                                    });
                        } else if (cmd == "nodes") {
                            if (!master) {
                                rp.deliver(
                                    std::string("ERROR: cluster.master unavailable "
                                                "(not a cluster node?)"));
                                return;
                            }
                            self->request(master, std::chrono::seconds(3),
                                          node_topology_atom_v)
                                .then(
                                    [rp](caf_plugin_system::topology_snapshot& snap) mutable {
                                        rp.deliver(format_topology(snap));
                                    },
                                    [rp](caf::error& e) mutable {
                                        rp.deliver("ERROR: "
                                                   + caf::to_string(e));
                                    });
                        } else {
                            rp.deliver(
                                "unknown admin cmd (ping|services|nodes): "
                                + cmd);
                        }
                    },
                };
                return caf::behavior{
                    admin.or_else(plugin_lifecycle(self, PluginLifecycleHooks{}))};
            });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new ClusterAdminPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
