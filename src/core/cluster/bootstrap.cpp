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

namespace {

/// 当前节点 actor（master 或 client）——信号处理用。进程级，bootstrap 时赋值。
caf::actor& node_ctl_ref() {
    static caf::actor ctl;
    return ctl;
}

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT
        || signal == CTRL_CLOSE_EVENT) {
        if (node_ctl_ref()) {
            caf::anon_send_exit(node_ctl_ref(), caf::exit_reason::user_shutdown);
        }
        return TRUE;
    }
    return FALSE;
}
#else
void signal_handler(int) {
    if (node_ctl_ref()) {
        caf::anon_send_exit(node_ctl_ref(), caf::exit_reason::user_shutdown);
    }
}
#endif

} // namespace

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
    // local_monitor：master 监控它感知本进程退出（优雅关机时立即 down）。
    // 纯节点模式没有 shutdown_mgr 可传，spawn 一个进程哨兵 actor 兜底
    // （master 持其强引用，进程退出时必然收到 down_msg，不用等 lease 过期）。
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

// stdin 管道 EOF 哨兵：WSL interop 下 Ctrl+C 只杀 bash，exe 变孤儿；
// 父进程/终端消失 → 管道写端关闭 → 读 EOF → 触发优雅关机。
// 仅监视 FIFO stdin（WSL 管道）；控制台/重定向/devnull 不监视。
// 启动 1.5s 宽限，避免后台启动时空管道立即 EOF 误触发。
void install_stdin_watchdog(caf::actor target) {
#ifdef _WIN32
    struct _stat64 st;
    if (_fstat64(_fileno(stdin), &st) != 0
        || (st.st_mode & _S_IFMT) != _S_IFIFO)
        return;
    std::thread([target] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        char buf[64];
        size_t total = 0;
        while (fread(buf, 1, sizeof buf, stdin) > 0)
            total += sizeof buf;
        // 只有"读过数据后 EOF"才算父进程/终端消失；启动即 EOF 的空管道
        // （如 WSL→PowerShell 继承的空 stdin）不触发，避免误关机。
        if (total == 0)
            return;
        caf::anon_send_exit(target, caf::exit_reason::user_shutdown);
    }).detach();
#else
    struct stat st;
    if (fstat(STDIN_FILENO, &st) != 0 || !S_ISFIFO(st.st_mode))
        return;
    std::thread([target] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        char buf[64];
        while (fread(buf, 1, sizeof buf, stdin) > 0) {
        }
        caf::anon_send_exit(target, caf::exit_reason::user_shutdown);
    }).detach();
#endif
}

void wait_for_node_shutdown(caf::actor_system& sys, const BootstrapResult& nb) {
    // 信号 → send_exit 节点 actor → wait_for 返回（Ctrl+C / 关窗）
    node_ctl_ref() = nb.master ? nb.master : nb.client;
#ifdef _WIN32
    SetConsoleCtrlHandler(console_handler, TRUE);
#else
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
#endif
    install_stdin_watchdog(node_ctl_ref());

    caf::scoped_actor self{sys};
    // master/client 至少有一个（bootstrap_node 成功时）
    if (nb.master)
        self->wait_for(nb.master);
    else if (nb.client)
        self->wait_for(nb.client);
    CAF_LOG_INFO("node shutdown complete");
}

} } // namespace caf_plugin_system::cluster
