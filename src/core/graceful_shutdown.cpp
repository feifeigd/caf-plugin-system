#include "graceful_shutdown.hpp"
#include "framework_bootstrap.hpp"
#include "services/logging_service.hpp"
#include "plugin/plugin_manager.hpp"
#include "checkpoint_manager.hpp"
#include <ctime>
#include <fstream>
#include <iostream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

// 关机轨迹落盘：点窗口 X / 系统关机时控制台正在销毁，stdout 输出看不见，
// 这是唯一能证明"走了优雅关机链"的文件证据（用户红线：优雅关机必须完整
// 保存数据——trace 记录 + checkpoints/ 落盘双证明；强杀则两者都没有）。
// 实现用 Windows API（CreateFile FILE_APPEND_DATA + FILE_SHARE_READ|WRITE）：
// MSVC ofstream 共享模式不可控，曾实测并发/顺序打开同一文件时阻塞——拖死
// 关机链直到 5 秒窗口耗尽被强杀（checkpoints 不落盘，数据丢失）。
void trace_shutdown(const std::string& msg) {
    std::lock_guard<std::mutex> lock(caf_plugin_system::trace_file_mutex());
#ifdef _WIN32
    HANDLE h = CreateFileA("shutdown-trace.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return;
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    std::string line = "[" + std::string(buf) + "] " + msg + "\r\n";
    DWORD written = 0;
    WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    CloseHandle(h);
#else
    std::ofstream f("shutdown-trace.log", std::ios::app);
    if (!f)
        return;
    char buf[32];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof buf, "%H:%M:%S", std::localtime(&t));
    f << "[" << buf << "] " << msg << std::endl;
#endif
}

// 关机链统一输出：文件永远写（点 X 时唯一可见日志），console 走 fw_log_from
//（logging_service > CAF log > cout；控制台销毁时自动跳过）。用 self->send
// 而非 anon_send：日志服务可记录发送方（shutdown_mgr），且与随后同 sender
// 的 send_exit 形成 FIFO——STOPPED 日志保证先落盘再退出日志服务。
void shutdown_out(caf::event_based_actor* self, const std::string& msg) {
    trace_shutdown(msg);
    trace_shutdown(std::string("  [out] console_closing=")
                   + (caf_plugin_system::console_closing() ? "1" : "0"));
    LOG_FROM(self, "INFO", msg);
    trace_shutdown("  [out] fw_log returned");
}

} // namespace

GracefulShutdown::GracefulShutdown(caf::actor_config& cfg,
                                   ShutdownConfig shutdown_cfg,
                                   caf::actor plugin_mgr,
                                   caf::actor registry,
                                   caf::actor checkpoint_mgr,
                                   caf::actor logging_service,
                                   std::function<std::vector<std::string>()> get_stop_order)
    : caf::event_based_actor(cfg),
      config_(shutdown_cfg),
      plugin_mgr_(plugin_mgr),
      registry_(registry),
      checkpoint_mgr_(checkpoint_mgr),
      logging_service_(logging_service),
      get_stop_order_(std::move(get_stop_order)) {}

caf::behavior GracefulShutdown::make_behavior() {
    caf::event_based_actor* self = this;
    auto plugin_mgr = plugin_mgr_;
    auto registry = registry_;
    auto checkpoint_mgr = checkpoint_mgr_;
    auto get_stop_order = get_stop_order_;

    // 统一终止集群节点 actor（master/client，register_cluster_atom 注册的）
    auto stop_cluster = [=, this] {
        for (auto& ctl : cluster_ctls_)
            if (ctl)
                self->send_exit(ctl, caf::exit_reason::user_shutdown);
    };

    // 完成关机：杀集群 → 杀组件 → 标记 stopped → 日志服务最后退 → quit。
    // 插件必须在调用前已全部保存（无插件时 remaining 为空即视为完成）。
    auto finish_shutdown = [=, this] {
        stop_cluster();
        self->send_exit(plugin_mgr, caf::exit_reason::user_shutdown);
        self->send_exit(registry, caf::exit_reason::user_shutdown);
        self->send_exit(checkpoint_mgr, caf::exit_reason::user_shutdown);
        state_.state = SystemState::stopped;
        // STOPPED 用 self->send（fw_log_from）：与随后 send_exit(logging_service)
        // 同 sender → FIFO，保证这行日志先被处理、再退出日志服务。
        shutdown_out(self, "[System] State: STOPPED");
        // 日志服务最后退出：flush sinks 后 quit（其 exit_msg handler 负责），
        // 最后一刻的日志（STOPPED）能收到。
        if (logging_service_)
            self->send_exit(logging_service_, caf::exit_reason::user_shutdown);
        self->quit();
        // 通知点 X 时阻塞在 cv 上的 CTRL_CLOSE handler：链已走完、数据已
        // 落盘，它随即 ExitProcess(0) 主动退出（handler 立即返回会被 conhost
        // ~0ms 强杀成 0xC000013A，见 framework_bootstrap.cpp 注释）。
        caf_plugin_system::notify_shutdown_complete();
    };

    // 逐个停插件的公共流程：resolve -> drain -> save_state -> checkpoint -> shutdown
    auto stop_next = [=, this](const std::string& name) {
        trace_shutdown("stop_next: resolving " + name);
        self->request(plugin_mgr, caf::infinite, resolve_plugin_atom{}, name)
            .then([=, this](const caf::actor& actor) {
                trace_shutdown("stop_next: resolved " + name);
                if (!actor) {
                    // resolve 失败也要推进停机链，避免卡死
                    self->send(self, plugin_saved_atom{}, name, false);
                    return;
                }
                self->send(actor, drain_atom{}, self);
                trace_shutdown("stop_next: draining " + name);
                self->request(actor, config_.plugin_stop_timeout, save_state_atom{})
                    .then(
                        [=, this](const std::vector<std::byte>& data) {
                            trace_shutdown("stop_next: saved " + name);
                            if (!data.empty()) {
                                self->request(checkpoint_mgr, std::chrono::seconds(10),
                                             save_state_atom{}, name, data)
                                    .then([=, this](bool) {
                                        trace_shutdown("stop_next: checkpointed " + name);
                                        self->send(actor, shutdown_atom{});
                                        self->send(self, plugin_saved_atom{}, name, true);
                                    });
                            } else {
                                self->send(actor, shutdown_atom{});
                                self->send(self, plugin_saved_atom{}, name, true);
                            }
                        },
                        [=, this](const caf::error&) {
                            trace_shutdown("stop_next: save FAILED for " + name);
                            self->send(actor, shutdown_atom{});
                            self->send(self, plugin_saved_atom{}, name, false);
                        }
                    );
            });
    };

    return caf::behavior{
        [=, this](ready_atom) {
            state_.state = SystemState::ready;
            shutdown_out(self, "[System] State: READY");
        },

        [=, this](shutdown_atom) {
            // 诊断：确认 shutdown_atom 是否真的到达 shutdown_mgr
            //（点 X 只看到 [signal] 行 = 断在投递环节）
            trace_shutdown("shutdown_atom RECEIVED");
            trace_shutdown(std::string("  [state] current state=")
                           + std::to_string(static_cast<int>(state_.state)));
            // 重复触发保护：关机链已在执行（如用户 Ctrl+C 后立刻点 X、或
            // watchdog+信号双触发），直接忽略——否则会走下面的 "Not ready,
            // forcing exit" 打断正在保存的插件（数据丢失）。
            if (state_.state == SystemState::shutting_down) {
                trace_shutdown("  [state] already shutting down, ignore duplicate");
                return;
            }
            if (state_.state != SystemState::ready) {
                // 未就绪（启动早期）也强制退出：Ctrl+C 任何阶段必须能响应，
                // 否则 actor_system 析构会等核心 actor 永久挂起。
                if (state_.state != SystemState::stopped) {
                    shutdown_out(self, "[Shutdown] Not ready, forcing exit...");
                    finish_shutdown();
                }
                return;
            }
            state_.state = SystemState::shutting_down;
            shutdown_out(self, "[System] Graceful shutdown initiated...");
            trace_shutdown("  -> sending registry shutdown");
            self->send(registry, shutdown_atom{});
            trace_shutdown("  -> computing stop order");
            state_.remaining_plugins = get_stop_order();
            trace_shutdown("  -> remaining plugins: "
                           + std::to_string(state_.remaining_plugins.size()));

            if (!state_.remaining_plugins.empty()) {
                stop_next(state_.remaining_plugins.back());
                self->delayed_send(self, config_.drain_timeout, force_exit_atom{});
            } else {
                // 无插件（纯节点/组件进程）：立即完成，不等 force_exit 兜底
                shutdown_out(self, "[Shutdown] No plugins to stop. Stopping cluster + registry...");
                finish_shutdown();
            }
        },

        [=, this](plugin_saved_atom, const std::string& plugin_name, bool) {
            auto& remaining = state_.remaining_plugins;
            remaining.erase(std::remove(remaining.begin(), remaining.end(), plugin_name), remaining.end());

            if (!remaining.empty()) {
                stop_next(remaining.back());
            } else {
                shutdown_out(self, "[Shutdown] All plugins saved. Stopping cluster + registry...");
                // 统一关机链：插件已保存 → 集群节点（master/client）→ 核心组件。
                // 全部停掉，否则 actor_system 析构会一直等它们退出。
                finish_shutdown();
            }
        },

        // 插件 drain 完成后的回执 (drain_atom, actor_addr)。本实现对回执
        // 不做等待（drain 与 save_state 并行发出），但必须显式吞掉：
        // CAF 1.1 里意外消息会经 print_and_drop 产生 error 并让 actor quit，
        // 曾在关机中途杀死本 actor 导致停机链断裂。
        [=](drain_atom, const caf::actor_addr&) {
        },

        [=, this](force_exit_atom) {
            if (state_.state == SystemState::stopped) return;
            shutdown_out(self, "[Shutdown] TIMEOUT! Force exiting...");
            finish_shutdown();
        },

        // 集群节点 actor 注册（main 在 bootstrap_node 后调用）：关机统一终止
        [=, this](register_cluster_atom, const caf::actor& ctl) {
            if (ctl)
                cluster_ctls_.push_back(ctl);
        },

        [=, this](health_check_atom) -> SystemState {
            return state_.state;
        }
    };
}
