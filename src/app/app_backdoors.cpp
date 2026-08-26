#include "app_backdoors.hpp"
#include "app_tests.hpp"
#include "services/logging_service.hpp"
#include "cluster/ops_actor.hpp"

#include <caf/actor_registry.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>

namespace caf_plugin_system {

namespace {

/// ops 从 registry 取（每次瞬时查找，发送即弃——不存引用防环）。
caf::actor lookup_ops(caf::actor_system& sys) {
    return caf::actor_cast<caf::actor>(sys.registry().get("ops"));
}

/// 组装 reload 命令：完整命令前缀（reload / reload-node / reload-nodes /
/// reload-all）原样透传，否则按单节点 reload-node <node>,<plugin>,<path> 拼装。
std::string build_reload_command(const std::string& spec) {
    const bool full_cmd = spec.rfind("reload ", 0) == 0
                          || spec.rfind("reload-node ", 0) == 0
                          || spec.rfind("reload-nodes ", 0) == 0
                          || spec.rfind("reload-all ", 0) == 0;
    std::string cmd = full_cmd ? spec : ("reload-node " + spec);
    std::replace(cmd.begin(), cmd.end(), ',', ' ');
    return cmd;
}

/// 冒烟测试后自动关机（--test-auto-shutdown）：插件引导后触发。
/// 先跑冒烟测试（ACL 拦截、缓冲冲刷、热更新、新代码生效），再走
/// shutdown_mgr 统一关机链（插件保存 → 集群 → 组件 → logging_service
/// 最后退出）。注意：此触发必须显式发 shutdown_atom——曾漏接导致冒烟
/// 测试跑完进程挂死在等输入。
void backdoor_auto_shutdown(caf::actor_system& sys, const BootstrapResult& fw) {
    run_smoke_tests(sys, fw);
    std::cout << "[OpsTest] auto shutdown after smoke tests (delayed 2s)"
              << std::endl;
    caf::scoped_actor self{sys};
    self->delayed_send(fw.shutdown_mgr, std::chrono::seconds(2), shutdown_atom{});
}

/// 集群验证后门：跨节点调用（resolve → connect → lookup → call）。
/// --test-cross-call=<服务名>，master 进程执行。
void backdoor_cross_call(caf::actor_system& sys, const app_config& cfg,
                         const cluster::BootstrapResult& nb) {
    if (!cfg.test_cross_call.empty() && nb.master)
        run_cross_call_test(sys, nb.master, cfg.node_cfg.node_name,
                            cfg.test_cross_call);
}

/// 集群验证后门：跨节点调用 + 有界重试（重启窗口期不丢）。
/// --test-cross-call-ex=<服务名>，master 进程执行。
void backdoor_cross_call_ex(caf::actor_system& sys, const app_config& cfg,
                            const cluster::BootstrapResult& nb) {
    if (!cfg.test_cross_call_ex.empty() && nb.master)
        run_cross_call_ex_test(sys, nb.master, cfg.node_cfg.node_name,
                               cfg.test_cross_call_ex);
}

/// bridge 验证后门：跨节点调外部节点 external_echo。
/// --test-bridge-call=<节点名>，master 进程执行——验证
/// 集群→bridge→外部进程 完整链路。
void backdoor_bridge_call(caf::actor_system& sys, const app_config& cfg,
                          const cluster::BootstrapResult& nb,
                          caf::actor shutdown_mgr) {
    if (!cfg.test_bridge_call.empty() && nb.master)
        run_bridge_call_test(sys, nb.master, cfg.node_cfg.node_name,
                             cfg.test_bridge_call, std::move(shutdown_mgr));
}

/// 运维验证后门：远程热更（--test-remote-reload=<node>,<plugin>,<path>）。
/// 复用控制台命令路径：把 reload-node 命令字符串发给本机 OpsActor，
/// 走与交互控制台完全相同的解析/寻址/热更链路。
/// delayed_send：给 worker 完成注册留时间（node_resolve 才能命中）。
void backdoor_remote_reload(caf::actor_system& sys, const app_config& cfg) {
    if (cfg.test_remote_reload.empty())
        return;
    auto ops = lookup_ops(sys);
    if (!ops) {
        std::cout << "[OpsTest] ops actor not found in registry" << std::endl;
        return;
    }
    auto cmd = build_reload_command(cfg.test_remote_reload);
    std::cout << "[OpsTest] " << cmd
              << " (delayed 12s for worker registration)" << std::endl;
    caf::scoped_actor self{sys};
    self->delayed_send(ops, std::chrono::seconds(12),
                       console_cmd_atom_v, cmd);
}

/// 运维验证后门：自动 quit（--test-quit，任何模式）。
/// 复用控制台 quit 命令路径，验证优雅关机链自然退出（进程必须自己
/// 退出且 EXIT 0，不能靠外部强杀——优雅关机回归测试）。
void backdoor_quit(caf::actor_system& sys, const app_config& cfg) {
    if (!cfg.test_quit)
        return;
    auto ops = lookup_ops(sys);
    if (!ops) {
        std::cout << "[OpsTest] ops actor not found in registry" << std::endl;
        return;
    }
    std::cout << "[OpsTest] triggering quit (delayed 2s)" << std::endl;
    caf::scoped_actor self{sys};
    self->delayed_send(ops, std::chrono::seconds(2),
                       console_cmd_atom_v, std::string("quit"));
}

/// 运维验证后门：模拟 Ctrl+C（--test-ctrl-c，任何模式）。
/// 直接发 shutdown_atom 给 shutdown_mgr（不经 ops），等价 Ctrl+C 的
/// console_handler 路径；验证 ops 注册给 shutdown_mgr 后也能自然退出。
void backdoor_ctrl_c(caf::actor_system& sys, const app_config& cfg,
                     caf::actor shutdown_mgr) {
    if (!cfg.test_ctrl_c)
        return;
    std::cout << "[OpsTest] simulating Ctrl+C (direct shutdown_atom, delayed 2s)"
              << std::endl;
    caf::scoped_actor self{sys};
    self->delayed_send(shutdown_mgr, std::chrono::seconds(2), shutdown_atom{});
}

} // namespace

void run_test_backdoors(caf::actor_system& sys, const app_config& cfg,
                        const cluster::BootstrapResult& nb,
                        const BootstrapResult& fw) {
    if (cfg.test_auto_shutdown)
        backdoor_auto_shutdown(sys, fw);
    backdoor_cross_call(sys, cfg, nb);
    backdoor_cross_call_ex(sys, cfg, nb);
    backdoor_bridge_call(sys, cfg, nb, fw.shutdown_mgr);
    backdoor_remote_reload(sys, cfg);
    backdoor_quit(sys, cfg);
    backdoor_ctrl_c(sys, cfg, fw.shutdown_mgr);
}

} // namespace caf_plugin_system
