// ------------------------------------------------------------------
// 集群节点引导实现 —— src/distributed-nodes/bootstrap.cpp
// ------------------------------------------------------------------

#include "bootstrap.hpp"

#include "client.hpp"
#include "master.hpp"
#include "common/message_tags.hpp"

#include <caf/actor_registry.hpp>
#include <caf/actor_system.hpp>
#include <caf/io/middleman.hpp>
#include <caf/logger.hpp>
#include <caf/scoped_actor.hpp>
#include <caf/send.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <sys/stat.h>
#endif

namespace caf_plugin_system { namespace cluster {

// 关机统一由 shutdown_mgr 处理（停插件 + 集群 + 组件），cluster 层不再
// 注册自己的信号处理/stdin 哨兵——Ctrl+C / EOF 都经 framework 的
// console_handler / install_stdin_watchdog 发 shutdown_atom 给 shutdown_mgr。

void init_node_io(caf::actor_system_config& cfg) {
    // CAF 1.1 的 middleman（caf::io）不是默认加载的模块：必须先注册其
    // 元对象（node_id 等类型），再 load。遗漏前者即 FATAL：
    // "I/O module loaded without calling caf::io::middleman::init_global_meta_objects()"
    caf::io::middleman::init_global_meta_objects();
    cfg.load<caf::io::middleman>();
}

void add_node_options(caf::actor_system_config& cfg, node_settings& s) {
    // custom_options() 是公开访问器；opt_group 即 config_option_adder 的别名
    caf::config_option_adder{cfg.custom_options(), "caf-plugin-system"}
        .add(s.node_kind, "node-kind", "node role: master/region/worker (empty = not a cluster node)")
        .add(s.node_name, "node-name", "unique node name")
        .add(s.node_host, "node-host", "host registered with master (default 127.0.0.1)")
        .add(s.node_port, "node-port", "middleman listen port (0 = auto)")
        .add(s.master_host, "master-host", "master host (default 127.0.0.1)")
        .add(s.master_port, "master-port", "master middleman port")
        .add(s.lease_seconds, "lease-seconds", "lease TTL in seconds (0 = never expire, default 10)")
        .add(s.parent, "parent", "parent node name (empty = direct child of master)");
}

bool bootstrap_node(caf::actor_system& sys, const node_settings& settings,
                    caf::actor local_monitor, BootstrapResult& out) {
    if (!settings.is_node())
        return true;  // 非节点模式：调用方自行决定后续

    node_kind kind;
    if (!from_string(settings.node_kind, kind)) {
        CAF_LOG_ERROR("Invalid node-kind: " << settings.node_kind);
        return false;
    }
    if (settings.node_name.empty()) {
        CAF_LOG_ERROR("node-name is required when node-kind is set");
        return false;
    }

    // 开 middleman 端口（所有节点模式；master 自身也需要被 connect）
    auto port = sys.middleman().open(settings.node_port, nullptr, false);
    if (!port) {
        CAF_LOG_ERROR("Failed to open middleman port: "
                      << caf::to_string(port.error()));
        return false;
    }
    out.port = *port;
    CAF_LOG_INFO("Node '" << settings.node_name << "' listening on port " << *port);

    if (kind == node_kind::master) {
        // master 扁平注册表：自身先注册，命名后供客户端 remote_lookup
        node_manifest self_manifest{kind, settings.node_name, settings.node_host,
                                    *port, "", {}};
        out.master = spawn_cluster_master(
            sys, std::move(self_manifest),
            std::chrono::seconds(settings.lease_seconds));
        sys.registry().put(settings.master_registry_name, out.master);
        return true;
    }

    // region/worker：接入 master（客户端 actor 后台自愈注册 + 心跳）
    node_client_config cc;
    cc.node_name = settings.node_name;
    cc.kind = kind;
    cc.host = settings.node_host;
    cc.port = *port;
    cc.parent = settings.parent;
    cc.master_host = settings.master_host;
    cc.master_port = settings.master_port;
    cc.lease_ttl = std::chrono::seconds(settings.lease_seconds);
    cc.master_registry_name = settings.master_registry_name;
    cc.exported_actors = settings.exported_actors;
    // 运维 actor 随节点导出：master 可 node_resolve(node, "ops") 远程热更
    cc.exported_actors.push_back("ops");
    // local_monitor：master 监控它感知本进程退出（优雅关机时立即 down）。
    // 常规调用方传 shutdown_mgr（系统组件，所有进程都有）；未提供时
    // spawn 进程哨兵 actor 兜底（master 持其强引用，进程退出时必然收到
    // down_msg，不用等 lease 过期）。
    caf::actor monitor = std::move(local_monitor);
    bool is_local_sentinel = !monitor;
    if (!monitor) {
        // 进程哨兵：master 监控它感知本进程退出；同时监控 client，
        // client 退出（优雅关机）时哨兵收到 down_msg 自动 quit——
        // 否则 actor_system 析构会等常驻哨兵永久挂起。
        monitor = sys.spawn([](caf::event_based_actor* self) -> caf::behavior {
            return {
                [self](caf::actor target) { self->monitor(target); },
                [self](const caf::down_msg&) { self->quit(); },
                [](int) {}  // 兜底 handler：不匹配消息默认丢弃
            };
        });
    }
    out.client = spawn_node_client(sys, std::move(cc), monitor);
    if (is_local_sentinel)
        caf::anon_send(monitor, out.client);  // 哨兵监控 client
    caf::anon_send(out.client, init_atom_v);
    return true;
}

} } // namespace caf_plugin_system::cluster
