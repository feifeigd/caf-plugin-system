// ------------------------------------------------------------------
// OpsActor —— 每节点运维入口 actor（实现）
// ------------------------------------------------------------------

#include "cluster/ops_actor.hpp"

#include "common/cluster_types.hpp"
#include "common/message_tags.hpp"
#include "services/logging_service.hpp"

#include <caf/actor_cast.hpp>
#include <caf/actor_registry.hpp> // registry() 完整定义（pimpl，all.hpp 不含）
#include <caf/all.hpp>
#include <caf/io/middleman.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace caf_plugin_system { namespace cluster {

namespace {

constexpr auto k_ops_timeout = std::chrono::seconds(10);

/// 控制台命令输出统一走 log_info（logging_service 系统组件）。
void ops_print(const std::string& msg) {
    LOG_INFO("[Ops] " + msg);
}

std::vector<std::string> split_ws(const std::string& s) {
    std::istringstream iss(s);
    std::vector<std::string> out;
    std::string tok;
    while (iss >> tok)
        out.push_back(tok);
    return out;
}

/// 简单 glob 匹配：* 任意串，? 单字符（批量热更的节点名匹配用）。
bool match_pattern(const std::string& name, const std::string& pat) {
    size_t ni = 0, pi = 0;
    size_t star = std::string::npos, mark = 0;
    while (ni < name.size()) {
        if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni])) {
            ++ni;
            ++pi;
        } else if (pi < pat.size() && pat[pi] == '*') {
            star = pi++;
            mark = ni;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ni = ++mark;
        } else {
            return false;
        }
    }
    while (pi < pat.size() && pat[pi] == '*')
        ++pi;
    return pi == pat.size();
}

std::string timestamp() {
    using clock_type = std::chrono::system_clock;
    auto t = clock_type::to_time_t(clock_type::now());
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d%H%M%S", &tm);
    return buf;
}

/// 进程号（落盘目录唯一性：同机多 worker 共享 updates/ 时避免秒级撞路径）。
long process_id() {
#ifdef _WIN32
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(::getpid());
#endif
}

/// 落盘唯一标签：时间戳 + PID（同机多进程同一秒落盘不冲突）。
std::string unique_tag() {
    return timestamp() + "-" + std::to_string(process_id());
}

/// 读取本地文件为字节流（master 侧字节流推送模式）。
bool read_file_bytes(const std::string& path, std::vector<std::byte>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        return false;
    auto sz = static_cast<std::size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    out.resize(sz);
    if (sz > 0)
        f.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(sz));
    return static_cast<bool>(f) || sz == 0;
}

/// 把推送的 DLL 字节流落盘到 updates/<plugin>/<ts>-<pid>/ 新路径。
/// 绝不覆盖已加载 DLL：新路径天然避开 Windows 文件锁与 LoadLibrary 路径缓存。
/// 时间戳带 PID 后缀：同机多 worker 共享 updates/ 时同一秒落盘互不冲突。
bool stage_dll(const std::string& updates_dir, const reload_request& req,
               std::string& out_path) {
    namespace fs = std::filesystem;
    auto dir = fs::path(updates_dir) / req.plugin_name / unique_tag();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        out_path = ec.message();
        return false;
    }
    auto fname =
        req.file_name.empty() ? (req.plugin_name + ".dll") : req.file_name;
    out_path = (dir / fname).string();
    std::ofstream f(out_path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(req.dll_bytes.data()),
            static_cast<std::streamsize>(req.dll_bytes.size()));
    if (!f) {
        out_path = "write failed: " + out_path;
        return false;
    }
    return true;
}

/// OpsActor：事件 actor，两个入口（本地控制台 / master 远程热更）。
class OpsActor : public caf::event_based_actor {
public:
    OpsActor(caf::actor_config& cfg, std::string node_name,
             caf::actor plugin_mgr, caf::actor master_registry,
             std::string updates_dir)
        : event_based_actor(cfg),
          node_name_(std::move(node_name)),
          plugin_mgr_(std::move(plugin_mgr)),
          master_registry_(std::move(master_registry)),
          updates_dir_(std::move(updates_dir)) {}

    caf::behavior make_behavior() override {
        return {
            [this](console_cmd_atom, const std::string& line) {
                handle_console(line);
            },
            [this](remote_reload_atom, reload_request req) {
                handle_remote_reload(std::move(req));
            },
            // 插件列表查询（本地 list 命令 + 将来 master 远程巡检共用）
            [this](list_plugins_atom) -> caf::result<std::vector<std::string>> {
                if (!plugin_mgr_)
                    return std::vector<std::string>{};
                auto rp = make_response_promise<std::vector<std::string>>();
                request(plugin_mgr_, k_ops_timeout, list_plugins_atom_v)
                    .then(
                        [rp](std::vector<std::string>& names) mutable {
                            rp.deliver(std::move(names));
                        },
                        [rp](caf::error&) mutable {
                            rp.deliver(std::vector<std::string>{});
                        });
                return rp;
            },
        };
    }

private:
    // ---- 本地控制台命令 ----
    void handle_console(const std::string& line) {
        auto parts = split_ws(line);
        if (parts.empty())
            return;
        const auto& cmd = parts[0];
        if (cmd == "help") {
            print_help();
        } else if (cmd == "list") {
            console_list_plugins();
        } else if (cmd == "nodes") {
            console_list_nodes();
        } else if (cmd == "reload" && parts.size() >= 3) {
            console_local_reload(parts[1], parts[2]);
        } else if (cmd == "reload-node" && parts.size() >= 4) {
            console_remote_reload(parts[1], parts[2], parts[3]);
        } else if (cmd == "reload-nodes" && parts.size() >= 4) {
            console_reload_nodes(parts[1], parts[2], parts[3]);
        } else if (cmd == "reload-all" && parts.size() >= 3) {
            console_reload_all(parts[1], parts[2]);
        } else if (cmd == "quit" || cmd == "exit") {
            ops_print("shutting down...");
            // 统一关机：quit 只发 shutdown_atom 给 shutdown_mgr，由它负责
            // 停插件（drain→save→shutdown）+ 杀集群（master/client）+
            // 杀组件（plugin_mgr/registry/checkpoint）——main 只等它退出。
            // ops 自己立即退出：main return 后 actor_system 析构会等常驻
            // actor，ops 不退则进程永不退出（quit 卡死根因之一）。
            // 不能存 shutdown_mgr 成员：ops 注册进 shutdown_mgr 的
            // cluster_ctls_（register_cluster_atom），回存强引用形成环
            // （两个 control block 互相保活 → 进程退出不释放 → dump 报
            // "泄露"）。每次 quit 从 registry 瞬时查找，发送即弃。
            auto shutdown_mgr = caf::actor_cast<caf::actor>(
                system().registry().get("shutdown_mgr"));
            if (shutdown_mgr)
                caf::anon_send(shutdown_mgr, shutdown_atom{});
            this->quit();
        } else {
            ops_print("unknown command: " + cmd);
            print_help();
        }
    }

    void print_help() {
        ops_print("commands:");
        ops_print("  list                            - list local plugins");
        ops_print("  reload <plugin> <dll_path>      - hot-reload a local plugin");
        if (master_registry_) {
            ops_print("  nodes                           - show cluster topology");
            ops_print("  reload-node <node> <plugin> <p> - remote hot-reload one node");
            ops_print("  reload-nodes <pat> <plugin> <p> - remote hot-reload matching nodes");
            ops_print("                                   (glob: * and ?; excludes master)");
            ops_print("  reload-all <plugin> <p>         - remote hot-reload all nodes");
            ops_print("                                   p = local file (push bytes)");
            ops_print("                                   or path already staged on nodes");
        }
        ops_print("  quit                            - graceful shutdown");
    }

    void console_list_plugins() {
        if (!plugin_mgr_) {
            ops_print("no plugin framework on this node");
            return;
        }
        request(plugin_mgr_, k_ops_timeout, list_plugins_atom_v)
            .then(
                [](std::vector<std::string>& names) {
                    if (names.empty()) {
                        ops_print("plugins: (none)");
                        return;
                    }
                    std::ostringstream oss;
                    for (const auto& n : names)
                        oss << "  " << n;
                    ops_print("plugins:" + oss.str());
                },
                [](caf::error& err) {
                    ops_print("list failed: " + caf::to_string(err));
                });
    }

    void console_list_nodes() {
        if (!master_registry_) {
            ops_print("nodes command is master-only");
            return;
        }
        request(master_registry_, k_ops_timeout, node_topology_atom_v)
            .then(
                [](topology_snapshot& snap) {
                    std::ostringstream oss;
                    for (const auto& m : snap.nodes)
                        oss << "  " << m.node_name << "[" << to_string(m.kind)
                            << "] " << m.host << ":" << m.port;
                    ops_print("nodes (" + std::to_string(snap.nodes.size())
                              + "):" + oss.str());
                },
                [](caf::error& err) {
                    ops_print("topology failed: " + caf::to_string(err));
                });
    }

    void console_local_reload(const std::string& plugin, const std::string& path) {
        if (!plugin_mgr_) {
            ops_print("no plugin framework on this node");
            return;
        }
        ops_print("reloading '" + plugin + "' from " + path);
        request(plugin_mgr_, caf::infinite, reload_atom_v, plugin, path)
            .then(
                [plugin](bool ok) {
                    ops_print("reload '" + plugin + "': "
                              + (ok ? "OK" : "FAILED"));
                },
                [plugin](caf::error& err) {
                    ops_print("reload '" + plugin + "' error: "
                              + caf::to_string(err));
                });
    }

    // ---- master 侧：远程热更（单节点，带完成回调） ----
    // on_done(ok, message)：单节点命令用它打印，批量命令用它汇总。
    void do_remote_reload(const std::string& node, const std::string& plugin,
                          const std::string& path,
                          std::function<void(bool, const std::string&)> on_done) {
        if (!master_registry_) {
            on_done(false, "remote reload is master-only");
            return;
        }
        reload_request req;
        req.plugin_name = plugin;
        namespace fs = std::filesystem;
        if (fs::exists(path) && fs::is_regular_file(path)) {
            // 字节流推送模式：master 读本地文件发给节点
            if (!read_file_bytes(path, req.dll_bytes)) {
                on_done(false, "failed to read local file: " + path);
                return;
            }
            req.file_name = fs::path(path).filename().string();
            ops_print("pushing " + std::to_string(req.dll_bytes.size())
                      + " bytes of " + req.file_name + " to node '" + node + "'");
        } else {
            // 已就位路径模式：DLL 已通过其他渠道到达节点本地
            req.dll_path = path;
            ops_print("path mode: '" + path + "' must already exist on node '"
                      + node + "'");
        }
        // resolve → connect → lookup → remote_reload（RemoteCaller 同款寻址）
        request(master_registry_, k_ops_timeout, node_resolve_atom_v, node,
                std::string("ops"))
            .then(
                [this, req = std::move(req), on_done = std::move(on_done),
                 node](actor_route& route) mutable {
                    auto& mm = this->system().middleman();
                    request(mm.actor_handle(), k_ops_timeout, caf::connect_atom_v,
                            route.host, route.port)
                        .then(
                            [this, req = std::move(req), route,
                             on_done = std::move(on_done)](
                                const caf::node_id& nid,
                                const caf::strong_actor_ptr&,
                                const std::set<std::string>&) mutable {
                                auto ptr = this->system().middleman()
                                               .remote_lookup(route.actor_name, nid);
                                if (!ptr) {
                                    on_done(false, "lookup 'ops' at node '"
                                                   + route.node_name + "' failed");
                                    return;
                                }
                                auto target =
                                    caf::actor_cast<caf::actor>(std::move(ptr));
                                request(target, k_ops_timeout, remote_reload_atom_v,
                                        std::move(req))
                                    .then(
                                        [on_done](reload_result& r) {
                                            on_done(r.ok, r.message);
                                        },
                                        [on_done](caf::error& err) {
                                            on_done(false, caf::to_string(err));
                                        });
                            },
                            [route, on_done](caf::error& err) {
                                on_done(false, "connect to node '" + route.node_name
                                               + "' failed: " + caf::to_string(err));
                            });
                },
                [node, on_done](caf::error& err) {
                    on_done(false, "resolve node '" + node + "' failed: "
                                   + caf::to_string(err));
                });
    }

    void console_remote_reload(const std::string& node, const std::string& plugin,
                               const std::string& path) {
        do_remote_reload(node, plugin, path,
                         [node](bool ok, const std::string& msg) {
                             ops_print(std::string("remote reload '") + node + "': "
                                       + (ok ? "OK" : "FAILED") + " - " + msg);
                         });
    }

    // ---- master 侧：批量远程热更（reload-nodes / reload-all） ----
    // 批量状态（共享计数）：逐节点并行发起，全部完成打印汇总。
    struct BatchState {
        size_t total = 0;
        size_t ok = 0;
        size_t fail = 0;
    };

    void console_reload_batch(std::vector<std::string> nodes,
                              const std::string& plugin,
                              const std::string& path) {
        if (nodes.empty()) {
            ops_print("batch reload: no matching nodes");
            return;
        }
        ops_print("batch reload '" + plugin + "' -> " + std::to_string(nodes.size())
                  + " node(s)");
        auto state = std::make_shared<BatchState>();
        state->total = nodes.size();
        for (const auto& node : nodes) {
            do_remote_reload(
                node, plugin, path,
                [state, node](bool ok, const std::string& msg) {
                    if (ok)
                        ++state->ok;
                    else
                        ++state->fail;
                    ops_print("  " + node + ": " + (ok ? "OK" : "FAILED") + " - "
                              + msg);
                    if (state->ok + state->fail == state->total) {
                        ops_print("batch done: " + std::to_string(state->ok)
                                  + " OK, " + std::to_string(state->fail)
                                  + " FAILED (of " + std::to_string(state->total)
                                  + ")");
                    }
                });
        }
    }

    void console_reload_nodes(const std::string& pattern, const std::string& plugin,
                              const std::string& path) {
        if (!master_registry_) {
            ops_print("reload-nodes is master-only");
            return;
        }
        request(master_registry_, k_ops_timeout, node_topology_atom_v)
            .then(
                [this, pattern, plugin, path](topology_snapshot& snap) {
                    std::vector<std::string> matched;
                    for (const auto& m : snap.nodes) {
                        if (m.node_name == node_name_)
                            continue;  // 排除 master 自己（本地 reload 命令负责）
                        if (match_pattern(m.node_name, pattern))
                            matched.push_back(m.node_name);
                    }
                    console_reload_batch(std::move(matched), plugin, path);
                },
                [](caf::error& err) {
                    ops_print("topology query failed: " + caf::to_string(err));
                });
    }

    void console_reload_all(const std::string& plugin, const std::string& path) {
        console_reload_nodes("*", plugin, path);
    }

    // ---- 节点侧：处理 master 的远程热更 ----
    void handle_remote_reload(reload_request req) {
        auto rp = make_response_promise<reload_result>();
        auto sender = current_sender();
        // 不做 sender 拒绝校验：CAF 同机多进程 node_id 相同（host_id 同 IP），
        // 跨进程消息的 sender 会被解析成本地 actor 查找 → 失效/空（实测），
        // 拒绝校验会误拒所有同机部署。安全边界 = middleman 端口可达性 +
        // 集群互信（能 lookup 到 "ops" 的只有已注册节点）。sender 仅记日志审计。
        if (sender)
            ops_print("remote reload request from " + caf::to_string(sender));
        if (!plugin_mgr_) {
            rp.deliver(reload_result{false, "no plugin framework on this node"});
            return;
        }
        std::string dll_path;
        if (!req.dll_bytes.empty()) {
            if (!stage_dll(updates_dir_, req, dll_path)) {
                rp.deliver(reload_result{false, "stage failed: " + dll_path});
                return;
            }
        } else {
            dll_path = req.dll_path;
            if (dll_path.empty()) {
                rp.deliver(reload_result{false, "empty dll path (staged mode)"});
                return;
            }
        }
        ops_print("remote reload '" + req.plugin_name + "' from "
                  + (sender ? caf::to_string(sender->node())
                            : std::string("(unknown sender)"))
                  + " -> " + dll_path);
        request(plugin_mgr_, caf::infinite, reload_atom_v, req.plugin_name,
                dll_path)
            .then(
                [rp, plugin = req.plugin_name](bool ok) mutable {
                    rp.deliver(reload_result{ok, ok ? "reloaded" : "reload failed"});
                    ops_print("remote reload '" + plugin + "': "
                              + (ok ? "OK" : "FAILED"));
                },
                [rp, plugin = req.plugin_name](caf::error& err) mutable {
                    rp.deliver(reload_result{false, caf::to_string(err)});
                    ops_print("remote reload '" + plugin + "' error: "
                              + caf::to_string(err));
                });
    }

    std::string node_name_;
    caf::actor plugin_mgr_;
    caf::actor master_registry_;
    std::string updates_dir_;
};

} // namespace

caf::actor spawn_ops_actor(caf::actor_system& sys, std::string node_name,
                           caf::actor plugin_mgr, caf::actor master_registry,
                           std::string updates_dir) {
    return sys.spawn<OpsActor>(std::move(node_name), std::move(plugin_mgr),
                               std::move(master_registry),
                               std::move(updates_dir));
}

void start_console_thread(caf::actor ops) {
    // 交互判定：GetConsoleMode 成功 = 真实控制台或 ConPTY 伪终端（VS Code
    // external/integratedTerminal 都是 ConPTY，stdin 底层是命名管道，fstat
    // 会误报 FIFO 而跳过——ConPTY 同样支持 console API，必须按交互处理）。
    // 非交互（WSL interop 管道 / 文件重定向）跳过：install_stdin_watchdog
    // 接管 EOF 检测，避免两线程抢读同一管道。
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode)) {
        ops_print("console input disabled (stdin not interactive)");
        return;
    }
#else
    struct stat st;
    if (fstat(STDIN_FILENO, &st) != 0 || S_ISFIFO(st.st_mode))
        return;
#endif
    ops_print("console ready: type 'help' for commands");
    // 线程只捕获 actor_addr（弱引用）：线程阻塞在 getline，交互模式下会
    // 活到进程退出——若捕获强引用 caf::actor，ops 的 control block 被线程
    // 攥着，关机后也不释放（CRT dump 报泄露）。addr 不保活，ops 死后
    // anon_send 自动丢弃消息。绝不能用 scoped_actor——线程阻塞在 getline
    // 等键盘输入，其持有的 scoped_actor（本身是 CAF actor）永不退出，
    // actor_system 析构等它导致进程挂起（quit 能退而 Ctrl+C 卡死的根因
    // 之一）。anon_send 不需要调用者上下文，任意线程安全。
    auto ops_addr = caf::actor_cast<caf::actor_addr>(ops);
    std::thread([ops_addr] {
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty())
                continue;
            // addr 不保活 control block；发送前升级为强引用（get_locked，
            // actor 已死返回空则跳过）。自由函数 anon_send(addr) 在 CAF 1.1
            // 编译不过（模板要求 operator->），必须手动升级。
            auto strong = caf::actor_cast<caf::actor>(ops_addr);
            if (strong)
                caf::anon_send(strong, console_cmd_atom_v, line);
            else
                break;
        }
        // 走到这里 = stdin EOF/错误，线程自然结束（唯一自愿退出路径）。
        // 交互模式（Ctrl+C 关机）永远看不到这行——线程是被进程退出强杀的。
        // 单次 fprintf 原子写（多线程环境不用 std::cout 链式输出）。
        fprintf(stderr, "[console] thread exiting (stdin EOF)\n");
        fflush(stderr);
    }).detach();
}

}} // namespace caf_plugin_system::cluster
