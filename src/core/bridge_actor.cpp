#include "bridge_actor.hpp"

#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "service_registry.hpp"
#include "services/logging_service.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <caf/io/all.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace caf_plugin_system {

namespace {

// ------------------------------------------------------------------
// 行协议常量
// ------------------------------------------------------------------
constexpr std::uint16_t k_bridge_sub_proto = 1; // external_echo 信封子协议号

struct BridgeImplState {
    int seq = 0;                            // REQ 序号（rid）
    // 用 typed_response_promise（public 可拷贝）——caf::response_promise
    // 的拷贝构造是 private（friend typed_response_promise），map 存不了。
    std::map<int, caf::typed_response_promise<std::string>> pending;
};

struct BridgeState {}; // bridge 主 actor 无状态（全在 lambda 捕获里）

// "REQ <rid> <len>" / "RESULT <id> OK|ERR <len>" 的头部行
std::string header_line(const std::string& kind, int id,
                        const std::string& status, size_t len) {
    std::string out = kind + " " + std::to_string(id);
    if (!status.empty())
        out += " " + status;
    out += " " + std::to_string(len) + "\n";
    return out;
}

std::string bytes_to_string(const std::vector<std::byte>& v) {
    std::string s;
    s.resize(v.size());
    std::memcpy(s.data(), v.data(), v.size());
    return s;
}

std::vector<std::byte> string_to_bytes(const std::string& s) {
    std::vector<std::byte> v(s.size());
    std::memcpy(v.data(), s.data(), s.size());
    return v;
}

// ------------------------------------------------------------------
// 行协议帧解析（broker 事件驱动版）
//
// 帧格式：<KIND> <id> [<status>] <len>\n <payload> \n
//   KIND = CALL（id, svc, payload）/ RESULT（rid, OK|ERR, payload）
// 驱动方式：new_data_msg 的 buf 追加到连接缓冲，循环 try_parse_frame
// 消费完整帧；缓冲不足返回 false（等更多数据）。
// ------------------------------------------------------------------
struct ParsedFrame {
    // Invalid 放第 0 位：声明后未赋值时默认即 Invalid（防御性安全）
    enum class Kind { Invalid, Call, Result };
    Kind kind = Kind::Invalid;
    int id = 0;
    std::string svc;  // CALL 的服务名
    bool ok = false;  // RESULT 的 OK/ERR
    std::string payload;
};

// 尝试从缓冲头部解析一帧。返回 false = 数据不足；true = 消费一帧
//（kind == Invalid 表示协议错误，调用方决定断开该连接）。
bool try_parse_frame(std::string& buf, ParsedFrame& out) {
    // 头部行（到 \n）
    auto nl = buf.find('\n');
    if (nl == std::string::npos)
        return false;
    std::string head = buf.substr(0, nl);
    // 用最后一个空格切出 len，前面按空格分 token
    auto last_space = head.rfind(' ');
    if (last_space == std::string::npos) {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    std::string len_str = head.substr(last_space + 1);
    while (!len_str.empty() && (len_str.back() == '\n' || len_str.back() == '\r'))
        len_str.pop_back();
    std::string prefix = head.substr(0, last_space);
    auto sp1 = prefix.find(' ');
    if (sp1 == std::string::npos) {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    std::string kind = prefix.substr(0, sp1);
    std::string rest = prefix.substr(sp1 + 1);
    auto sp2 = rest.find(' ');
    if (sp2 == std::string::npos) {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    std::string id_str = rest.substr(0, sp2);
    std::string third = rest.substr(sp2 + 1);
    size_t len = 0;
    try {
        len = static_cast<size_t>(std::stoull(len_str));
    } catch (...) {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    if (len > 64u * 1024u * 1024u) { // 64MB 上限防恶意长度
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    // 等 payload + 尾部行终止符
    if (buf.size() < nl + 1 + len + 1)
        return false;
    std::string payload = buf.substr(nl + 1, len);
    if (buf[nl + 1 + len] != '\n') {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    buf.erase(0, nl + 1 + len + 1);
    int id = 0;
    try {
        id = std::stoi(id_str);
    } catch (...) {
        out.kind = ParsedFrame::Kind::Invalid;
        return true;
    }
    out.id = id;
    out.payload = std::move(payload);
    if (kind == "CALL") {
        out.kind = ParsedFrame::Kind::Call;
        out.svc = third;
    } else if (kind == "RESULT") {
        out.kind = ParsedFrame::Kind::Result;
        out.ok = (third == "OK");
    } else {
        out.kind = ParsedFrame::Kind::Invalid;
    }
    return true;
}

} // namespace（匿名结束：辅助类型；spawn_bridge_actor 必须外部链接）

// ------------------------------------------------------------------
// spawn_bridge_actor —— broker 版（2026-08-26 重写）
//
// 替换点：手写 socket（WSAStartup/bind/listen/accept/recv/send）+ 每
// 连接读写线程 + WriteQueue/ActiveConn 全部移除，改为 caf::io::broker
// 事件驱动：
//   - add_tcp_doorman(port) 监听（CAF 内部建 socket，Winsock 由
//     middleman 初始化，无需 WSAStartup）
//   - new_connection_msg → configure_read(at_least(1)) 收数据
//   - new_data_msg → 行协议解析（try_parse_frame）→ 转发 CALL/RESULT
//   - write_atom → write(handle, span) 写外部（v1 单连接语义写当前连接）
// 收益：零手写线程/零 socket 管理/零 join 时序问题；actor 退出时
// middleman 自动关闭 doorman/scribe，无 detached 线程泄露面。
// ------------------------------------------------------------------
caf::actor spawn_bridge_actor(caf::actor_system& sys, caf::actor registry,
                              std::uint16_t port,
                              const std::string& node_name) {
    // broker 句柄间接层：impl/bridge 先 spawn，broker 后 spawn，运行时填充
    auto broker_ref = std::make_shared<caf::actor>();

    // ---- external_echo 实现 actor：集群调用（envelope）→ REQ 出站，
    //      外部 RESULT 到达后 deliver 给调用方 ----
    auto impl = sys.spawn(
        [broker_ref](caf::stateful_actor<BridgeImplState>* self) {
            return caf::behavior{
                // 发给外部进程
                [self, broker_ref](plugin_envelope& env) -> caf::result<std::string> {
                    auto rid = ++self->state().seq;
                    std::string payload = bytes_to_string(env.payload);
                    auto rp = self->make_response_promise<std::string>();
                    self->state().pending.emplace(rid, rp);
                    if (*broker_ref) {
                        caf::anon_send(*broker_ref, write_atom_v,
                                       header_line("REQ", rid, "", payload.size())
                                           + payload + "\n");
                    } else {
                        // 理论上不可达（broker 先于任何请求 spawn）；
                        // 兜底防 rp 悬空
                        rp.deliver(caf::make_error(caf::sec::runtime_error,
                                                   "bridge broker not ready"));
                    }
                    return rp;
                },
                // Python 回复
                [self](ext_result_atom, int rid, bool ok,
                       const std::string& payload) {
                    auto it = self->state().pending.find(rid);
                    if (it == self->state().pending.end()) {
                        LOG_WARN("[Bridge] stale external result for rid="
                                 + std::to_string(rid));
                        return;
                    }
                    if (ok) {
                        it->second.deliver(payload);
                    } else {
                        it->second.deliver(caf::make_error(
                            caf::sec::runtime_error, payload));
                    }
                    self->state().pending.erase(it);
                },
                [self](caf::exit_msg&) { self->quit(); }};
        });

    // ---- bridge 主 actor：外部 CALL → resolve 本地服务 → 信封调用 ----
    // exit 时连 impl 一起杀：registry 关机只 send_exit proxy 不杀 impl，
    // 若 impl 无人 send_exit → actor_system 析构 join 它 → 进程挂起。
    auto bridge = sys.spawn(
        [registry, broker_ref, impl](caf::stateful_actor<BridgeState>* self) {
            return caf::behavior{
                // 外部进程发来的
                [self, registry, broker_ref](ext_call_atom, int id,
                                             const std::string& svc,
                                             const std::string& payload) {
                    auto respond = [broker_ref, id](bool ok,
                                                    std::string body) {
                        if (*broker_ref)
                            caf::anon_send(*broker_ref, write_atom_v,
                                           header_line("RESULT", id,
                                                       ok ? "OK" : "ERR",
                                                       body.size())
                                               + body + "\n");
                    };
                    self->request(registry, std::chrono::seconds(2),
                                  resolve_atom_v, svc)
                        .then(
                            [self, svc, payload,
                             respond](caf::actor& proxy) {
                                if (!proxy) {
                                    respond(false,
                                            "service not found: " + svc);
                                    return;
                                }
                                plugin_envelope env;
                                env.sub_proto = k_bridge_sub_proto;
                                env.payload = string_to_bytes(payload);
                                self->request(proxy, std::chrono::seconds(5),
                                              std::move(env))
                                    .then(
                                        [respond](std::string& r) {
                                            respond(true, std::move(r));
                                        },
                                        [respond](caf::error& e) {
                                            respond(false,
                                                    caf::to_string(e));
                                        });
                            },
                            [respond](caf::error& e) {
                                respond(false, caf::to_string(e));
                            });
                },
                [self, impl, broker_ref](caf::exit_msg&) {
                    self->send_exit(impl, caf::exit_reason::user_shutdown);
                    // broker 退出时 middleman 自动关闭 doorman/连接
                    if (*broker_ref)
                        self->send_exit(*broker_ref,
                                        caf::exit_reason::user_shutdown);
                    self->quit();
                }};
        });

    // ---- bridge broker：监听 + 读事件 + 行协议解析 + 写转发 ----
    auto bridge_addr = caf::actor_cast<caf::actor_addr>(bridge);
    auto impl_addr = caf::actor_cast<caf::actor_addr>(impl);
    // broker 不能 sys.spawn()（prohibit_top_level_spawn_marker）；
    // 官方入口是 middleman::spawn_broker（CAF 1.1，middleman.hpp:204）
    *broker_ref = sys.middleman().spawn_broker(
        [bridge_addr, impl_addr, port, node_name](caf::io::broker* self) {
            // reuse_addr=true：强杀/崩溃后 TIME_WAIT 残留端口，bind 立即
            // 复用（原手写 socket 版实测：Stop-Process 强杀 → 立刻重跑
            // → WSAEADDRINUSE，SO_REUSEADDR 治本）。
            auto doorman = self->add_tcp_doorman(port, nullptr, true);  // 监听端口
            if (!doorman) {
                LOG_ERROR("[Bridge] add_tcp_doorman on port "
                          + std::to_string(port) + " failed, bridge disabled");
                self->quit();
                return caf::behavior{};
            }
            LOG_INFO("[Bridge] " + node_name + " listening on port "
                     + std::to_string(port) + " (external_echo registered)");

            // 每连接读缓冲；bad = 协议错误连接（忽略后续数据，等对端关）
            auto rx = std::make_shared<std::map<caf::io::connection_handle,
                                                std::string>>();
            auto bad = std::make_shared<std::set<caf::io::connection_handle>>();
            // 当前连接（v1 单连接语义：外部只有一个进程，写它）
            auto current = std::make_shared<caf::io::connection_handle>();

            return caf::behavior{
                [self, rx, bad, current](caf::io::new_connection_msg& msg) {
                    self->configure_read(msg.handle,
                                         caf::io::receive_policy::at_least(1));
                    *current = msg.handle;
                    (*rx)[msg.handle].clear();
                    bad->erase(msg.handle);
                    LOG_INFO("[Bridge] new connection: "
                             + std::to_string(msg.handle.id()));
                },
                [self, rx, bad, current, bridge_addr,
                 impl_addr](caf::io::new_data_msg& msg) {
                    if (bad->count(msg.handle))
                        return;
                    auto& buf = (*rx)[msg.handle];
                    buf.append(reinterpret_cast<const char*>(msg.buf.data()),
                               msg.buf.size());
                    LOG_INFO("[Bridge] rx " + std::to_string(msg.buf.size())
                             + "B from " + std::to_string(msg.handle.id()));
                    ParsedFrame frame;
                    while (try_parse_frame(buf, frame)) {
                        if (frame.kind == ParsedFrame::Kind::Invalid) {
                            // 协议错误：标记该连接，忽略后续数据
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            break;
                        }
                        if (frame.kind == ParsedFrame::Kind::Call) {
                            auto strong = caf::actor_cast<caf::actor>(bridge_addr);
                            LOG_INFO("[Bridge] CALL " + std::to_string(frame.id)
                                     + " svc=" + frame.svc + " len="
                                     + std::to_string(frame.payload.size()));
                            if (strong)
                                caf::anon_send(strong, ext_call_atom_v, frame.id,
                                               frame.svc, frame.payload);
                        } else if (frame.kind == ParsedFrame::Kind::Result) {
                            auto strong = caf::actor_cast<caf::actor>(impl_addr);
                            if (strong)
                                caf::anon_send(strong, ext_result_atom_v, frame.id,
                                               frame.ok, frame.payload);
                        }
                    }
                },
                [self, rx, bad, current](caf::io::connection_closed_msg& msg) {
                    rx->erase(msg.handle);
                    bad->erase(msg.handle);
                    if (!current->invalid() && *current == msg.handle)
                        *current = caf::io::connection_handle{};
                },
                [self, current](write_atom, const std::string& line) {
                    // v1 单连接语义：写当前连接
                    if (!current->invalid()) {
                        LOG_INFO("[Bridge] tx " + std::to_string(line.size())
                                 + "B to " + std::to_string(current->id()));
                        // CAF 1.1 写模型：write 进缓冲 + flush 发送。
                        // new_data_msg 处理路径 multiplexer 会自动 flush，
                        // 但 scheduler 线程（write_atom 消息）必须显式
                        // flush，否则缓冲滞留（实测 2026-08-26：写后 1s
                        // 无响应，客户端再发 1B 唤醒才收到缓冲数据）。
                        self->write(*current,
                                    caf::make_span(
                                        reinterpret_cast<const std::byte*>(
                                            line.data()),
                                        line.size()));
                        self->flush(*current);
                    } else {
                        LOG_WARN("[Bridge] tx dropped: no current connection");
                    }
                },
                [self](caf::io::data_transferred_msg&) {
                    // ack_writes(true) 的写完成通知；无操作（写缓冲由
                    // multiplexer 管理，无需逐字节确认）
                },
                [self](caf::exit_msg&) { self->quit(); }};
        });

    // 注册服务：registry 持有 impl（锚），proxy 自动生成并导出。
    // 必须【同步确认】注册完成再返回——main 的节点引导随后会查
    // exported_actors（只查一次，跨 sender 无顺序保证），若 register 还没
    // 被 registry 处理，manifest 上报不含 external_echo → master 路由
    // no_such_key。同 sender FIFO：register(send) 先于 list(request) 到达。
    caf::scoped_actor self{sys};
    self->send(registry, register_atom_v, std::string("external_echo"), impl,
               std::string("bridge"));
    bool registered = false;
    self->request(registry, std::chrono::seconds(2), list_services_atom_v)
        .receive(
            [&](std::vector<std::string>& names) {
                registered = std::find(names.begin(), names.end(),
                                       "external_echo")
                             != names.end();
            },
            [&](caf::error& e) {
                LOG_ERROR("[Bridge] list_services failed: "
                          + caf::to_string(e));
            });
    if (!registered) {
        LOG_ERROR("[Bridge] external_echo registration not confirmed");
    }

    return bridge;
}

} // namespace caf_plugin_system
