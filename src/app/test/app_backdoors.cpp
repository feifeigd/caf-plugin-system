#include "app_backdoors.hpp"
#include "app_tests.hpp"
#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "services/logging_service.hpp"
#include "cluster/ops_actor.hpp"

#include <caf/actor_registry.hpp>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

namespace caf_plugin_system {

namespace {

/// ops 从 registry 取（每次瞬时查找，发送即弃——不存引用防环）。
caf::actor lookup_ops(caf::actor_system& sys) {
    return caf::actor_cast<caf::actor>(sys.registry().get("ops"));
}

/// 延迟投递 helper（LEAKFIX 2026-08-30）：
/// 不用 CAF 的 delayed_send——其 action 挂在 scheduler 时间队列，关机时
/// 残留 1 块 8B（default_action_impl 调度残留，反汇编定案）。改为
/// detach 线程 sleep 后从 system registry 现取目标发送（镜像
/// install_stdin_watchdog 修复模式：线程不捕获 actor 强引用，仅捕获
/// actor_system& + 值参数；进程退出时线程被 OS 回收，无残留）。
template <class... Ts>
void delayed_registry_send(caf::actor_system& sys, std::string_view key,
                           std::chrono::milliseconds delay, Ts... xs) {
    std::thread([&sys, key = std::string(key), delay, xs...]() mutable {
        std::this_thread::sleep_for(delay);
        if (auto target = sys.registry().get(key)) {
            caf::anon_send(caf::actor_cast<caf::actor>(target),
                           std::move(xs)...);
        }
    }).detach();
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
    delayed_registry_send(sys, "shutdown_mgr", std::chrono::seconds(2),
                          shutdown_atom{});
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
/// 延迟投递：给 worker 完成注册留时间（node_resolve 才能命中）。
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
    delayed_registry_send(sys, "ops", std::chrono::seconds(12),
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
    delayed_registry_send(sys, "ops", std::chrono::seconds(2),
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
    // TEMP-EXPERIMENT(2026-08-31): 崩溃对照——delayed_registry_send 模板
    // （std::move(xs)...）vs 显式 anon_send。stdin EOF 路径（watchdog
    // 线程同样 anon_send shutdown_atom）干净，delayed 线程触发必崩。
    std::thread([&sys]() {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        if (auto target = sys.registry().get("shutdown_mgr"))
            caf::anon_send(caf::actor_cast<caf::actor>(target),
                           shutdown_atom_v);
    }).detach();
}

/// 脚本插件验证后门（--test-lua-script）：resolve echo_service 并调
/// envelope + string，校验 lua_host 桥接层（脚本 on_call / on_string）。
void backdoor_lua_script(caf::actor_system& sys, const app_config& cfg,
                         const BootstrapResult& fw) {
    if (!cfg.test_lua_script)
        return;
    run_lua_script_test(sys, fw);
}

/// 脚本插件验证后门（--test-py-script）：resolve echo_service 并调
/// envelope + string + 热更，校验 py_host 桥接层。
void backdoor_py_script(caf::actor_system& sys, const app_config& cfg,
                        const BootstrapResult& fw) {
    if (!cfg.test_py_script)
        return;
    run_py_script_test(sys, fw);
}

/// 脚本插件验证后门（--test-ts-script）：resolve echo_service 并调
/// envelope + string + 热更，校验 ts_host 桥接层。
void backdoor_ts_script(caf::actor_system& sys, const app_config& cfg,
                        const BootstrapResult& fw) {
    if (!cfg.test_ts_script)
        return;
    run_ts_script_test(sys, fw);
}

/// 统一时间源验证后门（--test-time-offset）：校验全局业务时间偏移
/// 注入生效（business_now() - now == 配置值）。不依赖任何服务。
void backdoor_time_offset(const app_config& cfg) {
    if (!cfg.test_time_offset)
        return;
    run_time_offset_test();
}

/// 运行期卸载验证后门（--test-unload=<插件名>）：request PluginManager
/// unload_atom 走完整退役链——quiesce 断流 → save_state 排空屏障 →
/// unregister（proxy 退役 + 统一解绑广播 service_gone 给其他插件主
/// actor）→ 旧 actor 退役。验证广播链路 + proxy monitor 断环。
void backdoor_unload_plugin(caf::actor_system& sys, const app_config& cfg,
                            const BootstrapResult& fw) {
    if (cfg.test_unload.empty())
        return;
    std::cout << "[UnloadTest] unloading plugin: " << cfg.test_unload
              << std::endl;
    caf::scoped_actor self{sys};
    self->request(fw.plugin_mgr, std::chrono::seconds(10), unload_atom{},
                  cfg.test_unload)
        .receive(
            [](bool ok) {
                std::cout << "[UnloadTest] unload result: " << ok << std::endl;
            },
            [](caf::error& e) {
                std::cout << "[UnloadTest] unload error: "
                          << caf::to_string(e) << std::endl;
            });
}

/// Pomelo 出站 PUSH 验证后门（--test-pomelo-push）：延迟 3s 后
/// resolve("pomelo_push") 推两次——先定向 uid=player-1
///（game.event=private-msg），再广播（game.event=broadcast-msg）。
/// 验证定向投递 + 广播两条链路。
void backdoor_pomelo_push(caf::actor_system& sys, const app_config& cfg,
                          const BootstrapResult& fw) {
    if (!cfg.test_pomelo_push)
        return;
    std::thread([&sys, registry = fw.registry]() {
        auto push_once = [&](const std::string& payload) {
            caf::scoped_actor self{sys};
            self->request(registry, std::chrono::seconds(2),
                          resolve_atom_v, std::string("pomelo_push"))
                .receive(
                    [&](caf::actor& proxy) {
                        if (!proxy) {
                            std::cout << "[PomeloPush] resolve failed"
                                      << std::endl;
                            return;
                        }
                        plugin_envelope env;
                        env.function = "game.event";
                        env.format = payload_format::raw;
                        // string → vector<std::byte> 需显式转换
                        env.payload.resize(payload.size());
                        std::memcpy(env.payload.data(),
                                    payload.data(), payload.size());
                        caf::anon_send(proxy, std::move(env));
                        std::cout << "[PomeloPush] pushed: " << payload
                                  << std::endl;
                    },
                    [](caf::error& e) {
                        std::cout << "[PomeloPush] resolve error: "
                                  << caf::to_string(e) << std::endl;
                    });
        };
        std::this_thread::sleep_for(std::chrono::seconds(3));
        push_once("{\"uid\":\"player-1\",\"body\":\"private-msg\"}");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        push_once("broadcast-msg");
    }).detach();
}

/// 超时防护验证后门（--test-timeout）：spawn 一个不回任何消息的哑
/// actor，发带 2s 超时的 request，断言收到 caf::sec::request_timeout
/// 错误（而非无限挂起）——验证"消除无穷等待"的超时机制真实生效。
void backdoor_timeout_check(caf::actor_system& sys, const app_config& cfg,
                            const BootstrapResult& fw) {
    if (!cfg.test_timeout)
        return;
    std::cout << "[TimeoutTest] spawning slow actor (3s reply)..."
              << std::endl;
    auto dummy = sys.spawn([]() -> caf::behavior {
        // 慢响应 actor：收到请求后阻塞 3s 才回（超过 request 的 2s
        // 超时）→ 发送方必在 2s 处收到 request_timeout。
        // 注：不能写"匹配但 void 的 handler"（CAF 1.1 自动回空消息）
        // 也不能写"不匹配的 handler"（CAF 回 unexpected_message）——
        // 两者都不触发超时。sleep 阻塞调度线程仅限测试专用。
        return [](resolve_atom, const std::string&) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            return caf::message{};
        };
    });
    caf::scoped_actor self{sys};
    const auto t0 = std::chrono::steady_clock::now();
    self->request(dummy, std::chrono::seconds(2), resolve_atom_v,
                  std::string("ping"))
        .receive(
            [](caf::message&) {
                std::cout << "[TimeoutTest] FAIL: unexpected reply"
                          << std::endl;
            },
            [&](caf::error& e) {
                const auto ms = std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now()
                                    - t0)
                                    .count();
                const bool is_timeout =
                    e == caf::sec::request_timeout;
                std::cout << "[TimeoutTest] "
                          << (is_timeout ? "PASS" : "FAIL")
                          << ": error=" << caf::to_string(e) << " ("
                          << e.category()
                          << ") after " << ms << "ms" << std::endl;
                if (!is_timeout) {
                    std::cout << "[TimeoutTest] expected "
                                 "caf::sec::request_timeout"
                              << std::endl;
                }
            });
    // 测试完成：触发优雅退出（shutdown_atom → 统一关机链）
    caf::anon_send(fw.shutdown_mgr, shutdown_atom{});
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
    backdoor_lua_script(sys, cfg, fw);
    backdoor_py_script(sys, cfg, fw);
    backdoor_ts_script(sys, cfg, fw);
    backdoor_time_offset(cfg);
    backdoor_unload_plugin(sys, cfg, fw);
    backdoor_pomelo_push(sys, cfg, fw);
    backdoor_timeout_check(sys, cfg, fw);
}

} // namespace caf_plugin_system
