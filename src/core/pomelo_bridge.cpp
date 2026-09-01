#include "pomelo_bridge.hpp"

#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "pomelo_codec.hpp"
#include "pomelo_ws.hpp"
#include "service_registry.hpp"
#include "services/logging_service.hpp"

#include <caf/all.hpp>
#include <caf/actor_registry.hpp>
#include <caf/io/all.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace caf_plugin_system {

namespace {

// ------------------------------------------------------------------
// HANDSHAKE 载荷（服务器 → 客户端；客户端解析 code==200 后回 ACK）
// 最小合法体：sys.heartbeat 告诉客户端心跳间隔，客户端自动互答。
// ------------------------------------------------------------------
constexpr std::string_view k_handshake_json =
    "{\"code\":200,\"sys\":{\"heartbeat\":60}}";

// 编码一帧写回客户端：Package(DATA, Message(...))
std::string frame_string(std::uint8_t pkg_type,
                         const std::vector<std::byte>& msg) {
    auto pkg = PomeloCodec::encode_pkg(pkg_type, msg);
    std::string s;
    s.resize(pkg.size());
    std::memcpy(s.data(), pkg.data(), pkg.size());
    return s;
}

std::string response_frame(std::uint32_t id, const std::string& body) {
    std::vector<std::byte> bytes(body.size());
    std::memcpy(bytes.data(), body.data(), body.size());
    auto msg = PomeloCodec::encode_msg(id, PomeloCodec::kMsgResponse, "",
                                       bytes);
    return frame_string(PomeloCodec::kPkgData, msg);
}

// "svc:function" → 拆分；无 ':' 返回 false
bool split_svc_function(const std::string& target, std::string& svc,
                        std::string& function) {
    auto pos = target.find(':');
    if (pos == std::string::npos || pos == 0
        || pos + 1 >= target.size())
        return false;
    svc = target.substr(0, pos);
    function = target.substr(pos + 1);
    return true;
}

// 从 HTTP 握手请求头提取 Sec-WebSocket-Key（找不到返回空串）
std::string extract_ws_key(const std::string& head) {
    const char* marker = "Sec-WebSocket-Key:";
    auto pos = head.find(marker);
    if (pos == std::string::npos)
        return {};
    pos += std::strlen(marker);
    while (pos < head.size() && (head[pos] == ' ' || head[pos] == '\t'))
        ++pos;
    auto end = head.find("\r\n", pos);
    if (end == std::string::npos)
        end = head.size();
    return head.substr(pos, end - pos);
}

} // namespace

// ------------------------------------------------------------------
// spawn_pomelo_bridge —— broker 事件驱动（多连接 + TCP/WS 双传输，
// 2026-09-01）
//
// 协议与通信分离：Pomelo 协议帧（Package/Message）可跑在两种传输上，
// 同端口自动探测（首包 "GET " 开头 → HTTP Upgrade → WebSocket；否则
// TCP Pomelo）。浏览器客户端（pomelo-jsclient-websocket）经 WS 接入，
// WS 消息 payload = 单个 Pomelo Package 帧，协议层逻辑完全复用。
//
// 多连接语义：每个连接独立 transport 状态与握手，REQUEST 响应写回
// 发起连接，一连接故障不影响其他连接。
//
// 连接流程（TCP）：accept → 发 HANDSHAKE → 等 ACK → DATA 就绪。
// 连接流程（WS）：accept → 探测 → 回 101 →（同 TCP 协议流程）。
// REQUEST：route 查表 → resolve svc → envelope 调用 → RESPONSE 回原连接。
// NOTIFY：envelope 单向。HEARTBEAT：原样互答。
// ------------------------------------------------------------------
caf::actor spawn_pomelo_bridge(caf::actor_system& sys, caf::actor registry,
                               std::uint16_t port,
                               const pomelo_route_table& routes,
                               const std::string& node_name) {
    // broker 句柄间接层：impl 先 spawn，broker 后 spawn，运行时填充
    auto broker_ref = std::make_shared<caf::actor>();

    // ---- impl：出站 PUSH 服务（集群 → 客户端主动推送）----
    // 注册为服务 "pomelo_push"：调用方发 plugin_envelope，function =
    // 外部 PUSH route（客户端在听的频道名），payload = 推送内容。
    // 转发给 broker 广播（PUSH 帧，无 id）。
    auto impl = sys.spawn(
        [broker_ref](caf::event_based_actor* self) {
            return caf::behavior{
                [self, broker_ref](const plugin_envelope& env) {
                    if (env.function.empty()) {
                        LOG_WARN("[Pomelo] push with empty route, ignored");
                        return;
                    }
                    // byte 迭代器不能直接构造 string（无隐式转换）
                    std::string body(
                        reinterpret_cast<const char*>(
                            env.payload.data()),
                        env.payload.size());
                    auto msg = PomeloCodec::encode_msg(
                        0, PomeloCodec::kMsgPush, env.function,
                        std::vector<std::byte>(env.payload.begin(),
                                               env.payload.end()));
                    auto frame = frame_string(PomeloCodec::kPkgData, msg);
                    auto strong =
                        caf::actor_cast<caf::actor>(*broker_ref);
                    if (strong)
                        caf::anon_send(strong, write_atom_v,
                                       std::move(frame));
                    LOG_INFO("[Pomelo] push route={} len={}",
                             env.function, body.size());
                }};
        });

    auto bridge = sys.middleman().spawn_broker(
        [registry, port, routes, node_name,
         broker_ref](caf::io::broker* self) {
            // 运行时填充 broker 地址（impl 广播目标）
            *broker_ref = caf::actor_cast<caf::actor>(self);
            auto doorman = self->add_tcp_doorman(port, nullptr, true);
            if (!doorman) {
                LOG_ERROR("[Pomelo] add_tcp_doorman on port "
                          + std::to_string(port)
                          + " failed, pomelo endpoint disabled");
                self->quit();
                return caf::behavior{};
            }
            LOG_INFO("[Pomelo] " + node_name + " listening on port "
                     + std::to_string(port) + " (routes="
                     + std::to_string(routes.size())
                     + ", tcp+ws auto-detect)");

            // 每连接读缓冲（rx）；bad = 协议错误连接（忽略后续数据）
            auto rx = std::make_shared<std::map<
                caf::io::connection_handle, std::vector<std::byte>>>();
            auto bad = std::make_shared<std::set<caf::io::connection_handle>>();
            // 每连接握手状态：false = 等 HANDSHAKE_ACK，true = DATA 就绪
            auto ready = std::make_shared<std::map<
                caf::io::connection_handle, bool>>();
            // 每连接传输类型（首包探测后确定）
            enum class Transport { Tcp, Ws };
            auto transport = std::make_shared<std::map<
                caf::io::connection_handle, Transport>>();
            // WS 握手完成标志（true 后走 WS 帧解析）
            auto ws_hs_done = std::make_shared<std::map<
                caf::io::connection_handle, bool>>();
            // WS 分片合并（opcode 0 continuation 累积；首帧 opcode 记录）
            auto ws_frag = std::make_shared<std::map<
                caf::io::connection_handle, std::vector<std::byte>>>();
            auto ws_frag_op = std::make_shared<std::map<
                caf::io::connection_handle, std::uint8_t>>();

            // 裸写字节到连接（握手响应/非 WS 包装路径用）
            auto write_raw = [self, rx, bad](
                                 caf::io::connection_handle h,
                                 const std::vector<std::byte>& bytes) {
                if (h.invalid() || !rx->count(h) || bad->count(h))
                    return;
                self->write(h, caf::make_span(bytes.data(), bytes.size()));
                self->flush(h);
            };

            // 写一帧到指定连接：按 transport 决定包装（WS 帧 or 原始
            // Package 帧）；连接已关/已坏则不写。
            auto write_frame = [self, rx, bad, transport, write_raw](
                                   caf::io::connection_handle h,
                                   const std::string& frame) {
                if (h.invalid() || !rx->count(h) || bad->count(h))
                    return;
                std::vector<std::byte> bytes(frame.size());
                std::memcpy(bytes.data(), frame.data(), frame.size());
                if (transport->count(h)
                    && (*transport)[h] == Transport::Ws) {
                    auto ws = WsCodec::encode_frame(WsCodec::kOpBinary,
                                                    bytes);
                    write_raw(h, ws);
                } else {
                    write_raw(h, bytes);
                }
            };

            // ---- 共享协议处理：一个 Pomelo Package 帧 ----
            // TCP/WS 两传输解析出 Package 后都走这里（协议与通信分离）。
            auto handle_package =
                [self, rx, bad, ready, transport, registry, routes,
                 write_frame](caf::io::connection_handle h,
                              const PomeloCodec::ParsedPkg& pkg) {
                    if (pkg.type == PomeloCodec::kPkgHandshake) {
                        // 客户端主动握手（官方语义：pomelo-jsclient
                        // connect 后先发 HANDSHAKE）→ 回 code:200 响应，
                        // 客户端随后发 HANDSHAKE_ACK 进入 DATA。
                        std::vector<std::byte> body(
                            k_handshake_json.size());
                        std::memcpy(body.data(), k_handshake_json.data(),
                                    k_handshake_json.size());
                        write_frame(
                            h,
                            frame_string(PomeloCodec::kPkgHandshake,
                                         body));
                        LOG_INFO("[Pomelo] conn "
                                 + std::to_string(h.id())
                                 + " handshake request, code=200 sent");
                        return;
                    }
                    if (pkg.type == PomeloCodec::kPkgHandshakeAck) {
                        (*ready)[h] = true;
                        LOG_INFO("[Pomelo] conn "
                                 + std::to_string(h.id())
                                 + " handshake ack, DATA ready");
                        return;
                    }
                    if (pkg.type == PomeloCodec::kPkgHeartbeat) {
                        // 心跳互答（空 body）回【本连接】
                        write_frame(
                            h,
                            frame_string(PomeloCodec::kPkgHeartbeat, {}));
                        return;
                    }
                    if (pkg.type != PomeloCodec::kPkgData) {
                        LOG_WARN("[Pomelo] unexpected pkg type "
                                 + std::to_string(pkg.type));
                        return; // KICK 等：忽略
                    }

                    // DATA 包：解析 Message
                    PomeloCodec::ParsedMsg msg2;
                    if (!PomeloCodec::parse_msg(pkg.body, msg2)
                        || !msg2.valid) {
                        bad->insert(h);
                        rx->erase(h);
                        ready->erase(h);
                        LOG_WARN("[Pomelo] bad message frame, closing");
                        return;
                    }

                    if (msg2.type == PomeloCodec::kMsgRequest) {
                        // REQUEST：route 查表 → 内部 envelope 调用，
                        // 响应写回【发起连接 h】
                        auto it = routes.find(msg2.route);
                        if (it == routes.end()) {
                            LOG_WARN("[Pomelo] unknown route: "
                                     + msg2.route);
                            write_frame(h, response_frame(
                                               msg2.id,
                                               "unknown route: "
                                                   + msg2.route));
                            return;
                        }
                        std::string svc, function;
                        if (!split_svc_function(it->second, svc,
                                                function)) {
                            write_frame(h, response_frame(
                                               msg2.id,
                                               "bad route target: "
                                                   + it->second));
                            return;
                        }
                        std::string body(
                            reinterpret_cast<const char*>(
                                msg2.body.data()),
                            msg2.body.size());
                        // MSVC 对 LOG_* 宏参数里的 string 变量直接拼接
                        // 偶发推导失败（C2678/C2672）——先拷中间变量。
                        const std::string r = msg2.route;
                        const std::string s = svc;
                        const std::string f = function;
                        LOG_INFO("[Pomelo] conn "
                                 + std::to_string(h.id())
                                 + " REQUEST id="
                                 + std::to_string(msg2.id) + " route="
                                 + r + " svc=" + s + " function=" + f
                                 + " len="
                                 + std::to_string(body.size()));
                        const caf::io::connection_handle src = h;
                        self->request(registry, std::chrono::seconds(2),
                                      resolve_atom_v, svc)
                            .then(
                                [self, write_frame, src, svc, function,
                                 body,
                                 msg_id = msg2.id](caf::actor& proxy) {
                                    if (!proxy) {
                                        write_frame(
                                            src,
                                            response_frame(
                                                msg_id,
                                                "service not found: "
                                                    + svc));
                                        return;
                                    }
                                    plugin_envelope env;
                                    env.function = function;
                                    env.format = payload_format::raw;
                                    // string → vector<std::byte> 无隐式
                                    // 转换（char→byte），先分配再 memcpy
                                    env.payload.resize(body.size());
                                    std::memcpy(env.payload.data(),
                                                body.data(),
                                                body.size());
                                    self->request(
                                        proxy, std::chrono::seconds(5),
                                        std::move(env))
                                        .then(
                                            [write_frame, src,
                                             msg_id](std::string& r2) {
                                                write_frame(
                                                    src,
                                                    response_frame(
                                                        msg_id,
                                                        std::move(r2)));
                                            },
                                            [write_frame, src,
                                             msg_id](caf::error& e) {
                                                write_frame(
                                                    src,
                                                    response_frame(
                                                        msg_id,
                                                        caf::to_string(e)));
                                            });
                                },
                                [write_frame, src,
                                 msg_id = msg2.id](caf::error& e) {
                                    write_frame(
                                        src,
                                        response_frame(msg_id,
                                                       caf::to_string(e)));
                                });
                    } else if (msg2.type == PomeloCodec::kMsgNotify) {
                        // NOTIFY：单向 envelope（fire-and-forget）
                        auto it = routes.find(msg2.route);
                        if (it == routes.end()) {
                            LOG_WARN("[Pomelo] unknown route (notify): "
                                     + msg2.route);
                            return;
                        }
                        std::string svc, function;
                        if (!split_svc_function(it->second, svc,
                                                function)) {
                            LOG_WARN("[Pomelo] bad route target: "
                                     + it->second);
                            return;
                        }
                        std::string body(
                            reinterpret_cast<const char*>(
                                msg2.body.data()),
                            msg2.body.size());
                        const std::string r = msg2.route;
                        const std::string s = svc;
                        const std::string f = function;
                        LOG_INFO("[Pomelo] conn "
                                 + std::to_string(h.id())
                                 + " NOTIFY route=" + r + " svc=" + s
                                 + " function=" + f + " len="
                                 + std::to_string(body.size()));
                        self->request(registry, std::chrono::seconds(2),
                                      resolve_atom_v, svc)
                            .then(
                                [self, function, body](
                                    caf::actor& proxy) {
                                    if (!proxy)
                                        return; // 单向无响应
                                    plugin_envelope env;
                                    env.function = function;
                                    env.format = payload_format::raw;
                                    env.payload.resize(body.size());
                                    std::memcpy(env.payload.data(),
                                                body.data(),
                                                body.size());
                                    caf::anon_send(proxy,
                                                   std::move(env));
                                },
                                [](caf::error&) {}); // 单向忽略错误
                    } else if (msg2.type == PomeloCodec::kMsgResponse) {
                        // 客户端 RESPONSE：本期无出站 pending，忽略
                        LOG_WARN("[Pomelo] unexpected RESPONSE from "
                                 "client, ignored");
                    } else {
                        // PUSH：客户端不应主动发
                        LOG_WARN("[Pomelo] unexpected PUSH from "
                                 "client, ignored");
                    }
                };

            return caf::behavior{
                [self, rx, bad, ready, transport, ws_hs_done](
                    caf::io::new_connection_msg& msg) {
                    self->configure_read(
                        msg.handle, caf::io::receive_policy::at_least(1));
                    (*rx)[msg.handle].clear();
                    bad->erase(msg.handle);
                    (*ready)[msg.handle] = false;
                    transport->erase(msg.handle);
                    ws_hs_done->erase(msg.handle);
                    LOG_INFO("[Pomelo] new connection: "
                             + std::to_string(msg.handle.id()));
                    // 握手由客户端主动发起（官方语义）：TCP 客户端首包
                    // = HANDSHAKE 包；WS 客户端首包 = HTTP Upgrade（探测
                    // 切 WS 后 101，其 HANDSHAKE 包在 WS 帧内）。服务器
                    // 不做任何主动发送，传输探测因此天然无时序冲突。
                },
                [self, rx, bad, ready, transport, ws_hs_done, ws_frag,
                 ws_frag_op, registry, routes, node_name, write_frame,
                 write_raw, handle_package](caf::io::new_data_msg& msg) {
                    if (bad->count(msg.handle))
                        return;
                    auto& buf = (*rx)[msg.handle];
                    buf.insert(buf.end(), msg.buf.begin(), msg.buf.end());
                    LOG_INFO("[Pomelo] rx " + std::to_string(msg.buf.size())
                             + "B from " + std::to_string(msg.handle.id()));

                    // ---- 首包传输探测（同端口 TCP/WS 自动识别）----
                    if (!transport->count(msg.handle)) {
                        if (WsCodec::looks_like_http_upgrade(buf)) {
                            (*transport)[msg.handle] = Transport::Ws;
                            (*ws_hs_done)[msg.handle] = false;
                            (*ws_frag)[msg.handle].clear();
                            LOG_INFO("[Pomelo] conn "
                                     + std::to_string(msg.handle.id())
                                     + " transport=WebSocket");
                        } else {
                            (*transport)[msg.handle] = Transport::Tcp;
                        }
                    }

                    const bool is_ws =
                        (*transport)[msg.handle] == Transport::Ws;

                    if (!is_ws) {
                        // ---- TCP：直接按 Package 帧解析 ----
                        while (true) {
                            PomeloCodec::ParsedPkg pkg;
                            long consumed =
                                PomeloCodec::parse_pkg(buf, pkg);
                            if (consumed == 0)
                                break; // 数据不足，等更多
                            if (consumed < 0) {
                                bad->insert(msg.handle);
                                rx->erase(msg.handle);
                                ready->erase(msg.handle);
                                LOG_WARN(
                                    "[Pomelo] protocol error, closing conn "
                                    + std::to_string(msg.handle.id()));
                                break;
                            }
                            buf.erase(buf.begin(),
                                      buf.begin() + consumed);
                            handle_package(msg.handle, pkg);
                        }
                        return;
                    }

                    // ---- WS：先握手，再按 WS 帧解析 ----
                    if (!(*ws_hs_done)[msg.handle]) {
                        // 找完整 HTTP 头（\r\n\r\n）
                        static const std::vector<std::byte> k_crlfcrlf = {
                            std::byte('\r'), std::byte('\n'),
                            std::byte('\r'), std::byte('\n')};
                        auto it = std::search(
                            buf.begin(), buf.end(),
                            k_crlfcrlf.begin(), k_crlfcrlf.end());
                        if (it == buf.end())
                            return; // 头未完整，等更多数据
                        std::string head(
                            reinterpret_cast<const char*>(buf.data()),
                            it + 4 - buf.begin());
                        std::string key = extract_ws_key(head);
                        if (key.empty()) {
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            LOG_WARN(
                                "[Pomelo] WS handshake missing key, "
                                "closing conn "
                                + std::to_string(msg.handle.id()));
                            return;
                        }
                        std::string resp =
                            WsCodec::handshake_response(key);
                        std::vector<std::byte> resp_bytes(resp.size());
                        std::memcpy(resp_bytes.data(), resp.data(),
                                    resp.size());
                        write_raw(msg.handle, resp_bytes);
                        (*ws_hs_done)[msg.handle] = true;
                        buf.erase(buf.begin(), it + 4);
                        LOG_INFO("[Pomelo] conn "
                                 + std::to_string(msg.handle.id())
                                 + " WS handshake done");
                    }

                    // WS 帧循环（可能有多个帧）
                    while (true) {
                        WsCodec::ParsedFrame frame;
                        long consumed =
                            WsCodec::parse_frame(buf, frame);
                        if (consumed == 0)
                            break;
                        if (consumed < 0) {
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            ready->erase(msg.handle);
                            LOG_WARN(
                                "[Pomelo] WS frame error, closing conn "
                                + std::to_string(msg.handle.id()));
                            break;
                        }
                        buf.erase(buf.begin(), buf.begin() + consumed);

                        if (frame.opcode == WsCodec::kOpPing) {
                            auto pong = WsCodec::encode_frame(
                                WsCodec::kOpPong, frame.payload);
                            write_raw(msg.handle, pong);
                            continue;
                        }
                        if (frame.opcode == WsCodec::kOpPong) {
                            continue; // 客户端 pong，忽略
                        }
                        if (frame.opcode == WsCodec::kOpClose) {
                            // 回 close 帧 + 标记关闭（等对端断开）
                            auto close = WsCodec::encode_frame(
                                WsCodec::kOpClose, {});
                            write_raw(msg.handle, close);
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            ready->erase(msg.handle);
                            LOG_INFO("[Pomelo] conn "
                                     + std::to_string(msg.handle.id())
                                     + " WS close");
                            break;
                        }
                        if (frame.opcode == WsCodec::kOpBinary
                            || frame.opcode == WsCodec::kOpText) {
                            // 数据帧：payload = Pomelo Package 帧
                            if (!frame.fin) {
                                // 分片开始：累积，等 continuation
                                (*ws_frag)[msg.handle] = frame.payload;
                                (*ws_frag_op)[msg.handle] =
                                    frame.opcode;
                                continue;
                            }
                            // 无分片：直接处理（有残留 frag 则合并）
                            std::vector<std::byte> payload =
                                std::move(frame.payload);
                            if (!(*ws_frag)[msg.handle].empty()) {
                                payload.insert(
                                    payload.begin(),
                                    (*ws_frag)[msg.handle].begin(),
                                    (*ws_frag)[msg.handle].end());
                                (*ws_frag)[msg.handle].clear();
                            }
                            // payload 内可能多个 Pomelo Package
                            std::vector<std::byte> tmp =
                                std::move(payload);
                            while (!tmp.empty()) {
                                PomeloCodec::ParsedPkg pkg;
                                long n = PomeloCodec::parse_pkg(
                                    tmp, pkg);
                                if (n <= 0)
                                    break; // 残余忽略（坏帧防御）
                                tmp.erase(tmp.begin(),
                                          tmp.begin() + n);
                                handle_package(msg.handle, pkg);
                            }
                            continue;
                        }
                        if (frame.opcode == WsCodec::kOpContinuation) {
                            // 续帧：合并到已累积分片
                            if (!(*ws_frag)[msg.handle].empty()) {
                                auto& frag =
                                    (*ws_frag)[msg.handle];
                                frag.insert(frag.end(),
                                            frame.payload.begin(),
                                            frame.payload.end());
                                if (frame.fin) {
                                    std::vector<std::byte> payload =
                                        std::move(frag);
                                    (*ws_frag)[msg.handle].clear();
                                    std::vector<std::byte> tmp =
                                        std::move(payload);
                                    while (!tmp.empty()) {
                                        PomeloCodec::ParsedPkg pkg;
                                        long n =
                                            PomeloCodec::parse_pkg(
                                                tmp, pkg);
                                        if (n <= 0)
                                            break;
                                        tmp.erase(tmp.begin(),
                                                  tmp.begin() + n);
                                        handle_package(msg.handle,
                                                       pkg);
                                    }
                                }
                            }
                            continue;
                        }
                        // 未知 opcode：忽略
                        LOG_WARN("[Pomelo] unknown WS opcode "
                                 + std::to_string(frame.opcode));
                    }
                },
                [self, rx, bad, ready, transport, ws_hs_done, ws_frag,
                 ws_frag_op](caf::io::connection_closed_msg& msg) {
                    rx->erase(msg.handle);
                    bad->erase(msg.handle);
                    ready->erase(msg.handle);
                    transport->erase(msg.handle);
                    ws_hs_done->erase(msg.handle);
                    ws_frag->erase(msg.handle);
                    ws_frag_op->erase(msg.handle);
                    LOG_INFO("[Pomelo] connection closed: "
                             + std::to_string(msg.handle.id()));
                },
                [self, rx, ready, write_frame](write_atom,
                                                const std::string& data) {
                    // PUSH 广播：所有已握手（ready）连接都收到；TCP 裸写
                    // Package 帧、WS 按 transport 自动包 WS 帧。
                    size_t sent = 0;
                    for (auto& kv : *ready) {
                        if (kv.second) {
                            write_frame(kv.first, data);
                            ++sent;
                        }
                    }
                    LOG_INFO("[Pomelo] write_atom broadcast: " +
                                 std::to_string(sent) + " conn(s)");
                },
                [self](caf::io::data_transferred_msg&) {
                    // ack_writes(true) 的写完成通知；无操作
                },
                [self](caf::exit_msg&) { self->quit(); }};
        });

    // 注册出站 PUSH 服务：集群服务 resolve("pomelo_push") 后发
    // plugin_envelope 即可向所有已握手客户端广播 PUSH 帧。
    caf::scoped_actor self{sys};
    self->send(registry, register_atom_v, std::string("pomelo_push"),
               impl, std::string("pomelo"),
               external_protocol_table{}); // 无外部协议号（PUSH 用字符串 route）
    bool registered = false;
    self->request(registry, std::chrono::seconds(2), list_services_atom_v)
        .receive(
            [&](std::vector<std::string>& names) {
                registered =
                    std::find(names.begin(), names.end(), "pomelo_push")
                    != names.end();
            },
            [&](caf::error& e) {
                LOG_ERROR("[Pomelo] list_services failed: "
                          + caf::to_string(e));
            });
    if (!registered) {
        LOG_ERROR("[Pomelo] pomelo_push registration not confirmed");
    }

    return bridge;
}

} // namespace caf_plugin_system
