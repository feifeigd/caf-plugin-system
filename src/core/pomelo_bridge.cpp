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
#include <caf/json_object.hpp>
#include <caf/json_value.hpp>

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
// HANDSHAKE 响应载荷（服务器 → 客户端；客户端解析 code==200 后回 ACK）
// 最小合法体：sys.heartbeat 告诉客户端心跳间隔，客户端自动互答。
// 编译期生成完整 Package 帧（type=1 + 3B 长度 + JSON body），运行时
// 只做一次 string 转换（Meyers 静态），每次握手零重复打包。
// ------------------------------------------------------------------
constexpr std::string_view k_handshake_body =
    "{\"code\":200,\"sys\":{\"heartbeat\":60}}";

struct HsFrame {
    // 帧字节全为 ASCII（type/长度头 + JSON body）——char 数组便于
    // 直接构造 string（std::copy 免 byte↔char 转换）。
    char data[4 + 128]; // 4B 头 + body（预留，实际长度由 size 定）
    size_t size = 0;
};

constexpr HsFrame make_hs_frame() {
    HsFrame f{};
    f.size = 4 + k_handshake_body.size();
    f.data[0] = static_cast<char>(PomeloCodec::kPkgHandshake);
    const size_t n = k_handshake_body.size();
    f.data[1] = static_cast<char>((n >> 16) & 0xFF);
    f.data[2] = static_cast<char>((n >> 8) & 0xFF);
    f.data[3] = static_cast<char>(n & 0xFF);
    for (size_t i = 0; i < n; ++i)
        f.data[4 + i] = static_cast<char>(k_handshake_body[i]);
    return f;
}

constexpr HsFrame k_hs_frame = make_hs_frame();

// 完整握手帧（string 形态，Meyers 静态：进程内只构造一次）。
// 帧内容编译期生成（constexpr 循环——C++17 的 std::copy 非 constexpr，
// 编译期求值只能用循环）；运行时转 string 用 std::copy，零重复打包。
const std::string& hs_frame_string() {
    static const std::string s = [] {
        std::string out;
        out.reserve(k_hs_frame.size);
        std::copy(k_hs_frame.data, k_hs_frame.data + k_hs_frame.size,
                  std::back_inserter(out));
        return out;
    }();
    return s;
}

// 编码一帧写回客户端：Package(DATA, Message(...))——string 直通零转换
std::string frame_string(std::uint8_t pkg_type,
                         const std::string& msg) {
    return PomeloCodec::encode_pkg(pkg_type, msg);
}

std::string response_frame(std::uint32_t id, const std::string& body) {
    auto msg = PomeloCodec::encode_msg(id, PomeloCodec::kMsgResponse, "",
                                       body);
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
    // 外部 PUSH route（客户端在听的频道名）。payload 约定 JSON：
    //   {"uid":"player-1","body":"..."} → 定向推给该 uid 的连接
    //   {"body":"..."} 或纯文本 → 广播给所有已握手连接
    // 转发给 broker（定向走 write_atom+uid，广播走 write_atom）。
    auto impl = sys.spawn(
        [broker_ref](caf::event_based_actor* self) {
            return caf::behavior{
                [self, broker_ref](const plugin_envelope& env) {
                    if (env.function.empty()) {
                        LOG_WARN("[Pomelo] push with empty route, ignored");
                        return;
                    }
                    std::string payload(
                        reinterpret_cast<const char*>(
                            env.payload.data()),
                        env.payload.size());
                    // 解析 JSON payload：uid 目标 + body 内容
                    std::string uid, body;
                    if (auto jv = caf::json_value::parse(payload)) {
                        if (jv->is_object()) {
                            auto u = jv->to_object().value("uid");
                            if (u.is_string())
                                uid = std::string(u.to_string());
                            auto b = jv->to_object().value("body");
                            if (b.is_string())
                                body = std::string(b.to_string());
                        }
                    }
                    if (body.empty())
                        body = payload; // 非 JSON：整体当内容
                    auto msg = PomeloCodec::encode_msg(
                        0, PomeloCodec::kMsgPush, env.function, body);
                    auto frame = frame_string(PomeloCodec::kPkgData, msg);
                    auto strong =
                        caf::actor_cast<caf::actor>(*broker_ref);
                    if (!strong)
                        return;
                    if (!uid.empty()) {
                        // 定向：写目标 uid 的连接
                        caf::anon_send(strong, write_atom_v, uid,
                                       std::move(frame));
                        LOG_INFO("[Pomelo] push uid={} route={} len={}",
                                 uid, env.function, body.size());
                    } else {
                        // 广播：所有已握手连接
                        caf::anon_send(strong, write_atom_v,
                                       std::move(frame));
                        LOG_INFO("[Pomelo] push broadcast route={} len={}",
                                 env.function, body.size());
                    }
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
                caf::io::connection_handle, std::string>>();
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
                caf::io::connection_handle, std::string>>();
            auto ws_frag_op = std::make_shared<std::map<
                caf::io::connection_handle, std::uint8_t>>();
            // 连接身份绑定：uid → connection（握手 user.uid 建立），
            // 供定向 PUSH 投递；连接关闭时清理。
            auto uid_map = std::make_shared<std::map<
                std::string, caf::io::connection_handle>>();

            // 裸写字节到连接（握手响应/非 WS 包装路径用）。
            // CAF write 有 (handle, size, const void*) 重载——string
            // 的 data() 直传，零转换。
            auto write_raw = [self, rx, bad](
                                 caf::io::connection_handle h,
                                 const std::string& bytes) {
                if (h.invalid() || !rx->count(h) || bad->count(h))
                    return;
                self->write(h, bytes.size(), bytes.data());
                self->flush(h);
            };

            // 写一帧到指定连接：按 transport 决定包装（WS 帧 or 原始
            // Package 帧）；连接已关/已坏则不写。
            auto write_frame = [self, rx, bad, transport, write_raw](
                                   caf::io::connection_handle h,
                                   const std::string& frame) {
                if (h.invalid() || !rx->count(h) || bad->count(h))
                    return;
                if (transport->count(h)
                    && (*transport)[h] == Transport::Ws) {
                    auto ws = WsCodec::encode_frame(WsCodec::kOpBinary,
                                                    frame);
                    write_raw(h, ws);
                } else {
                    write_raw(h, frame);
                }
            };

            // ---- 共享协议处理：一个 Pomelo Package 帧 ----
            // TCP/WS 两传输解析出 Package 后都走这里（协议与通信分离）。
            auto handle_package =
                [self, rx, bad, ready, transport, registry, routes,
                 uid_map,
                 write_frame](caf::io::connection_handle h,
                              const PomeloCodec::ParsedPkg& pkg) {
                    if (pkg.type == PomeloCodec::kPkgHandshake) {
                        // 客户端主动握手（官方语义：pomelo-jsclient
                        // connect 后先发 HANDSHAKE）→ 回 code:200 响应
                        //（编译期生成的完整帧，零重复打包），客户端随后
                        // 发 HANDSHAKE_ACK 进入 DATA。
                        // 提取握手 user.uid（可选）→ uid_map 绑定，
                        // 供定向 PUSH 投递。pkg.body 已是 string（二进制
                        // 安全），直接解析。
                        std::string uid;
                        if (auto jv =
                                caf::json_value::parse(pkg.body)) {
                            if (jv->is_object()) {
                                auto user =
                                    jv->to_object().value("user");
                                if (user.is_object()) {
                                    auto u =
                                        user.to_object().value("uid");
                                    if (u.is_string())
                                        uid = std::string(
                                            u.to_string());
                                }
                            }
                        }
                        if (!uid.empty()) {
                            (*uid_map)[uid] = h;
                            LOG_INFO("[Pomelo] conn "
                                     + std::to_string(h.id())
                                     + " bound uid=" + uid);
                        }
                        write_frame(h, hs_frame_string());
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
                        // body 已是 string；拷贝给 then 回调捕获
                        std::string body = msg2.body;
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
                        // body 已是 string；拷贝给 then 回调捕获
                        std::string body = msg2.body;
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
                    // msg.buf 是 byte_buffer = vector<std::byte>，显式转
                    // char 后入 string 缓冲（仅此处一次转换）
                    for (std::byte b : msg.buf)
                        buf.push_back(static_cast<char>(b));
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
                            buf.erase(0, static_cast<size_t>(consumed));
                            handle_package(msg.handle, pkg);
                        }
                        return;
                    }

                    // ---- WS：先握手，再按 WS 帧解析 ----
                    if (!(*ws_hs_done)[msg.handle]) {
                        // 找完整 HTTP 头（\r\n\r\n）
                        auto hs_end = buf.find("\r\n\r\n");
                        if (hs_end == std::string::npos)
                            return; // 头未完整，等更多数据
                        std::string head = buf.substr(0, hs_end + 4);
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
                        write_raw(msg.handle, resp); // string 直传
                        (*ws_hs_done)[msg.handle] = true;
                        buf.erase(0, hs_end + 4);
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
                        buf.erase(0, static_cast<size_t>(consumed));

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
                            std::string payload = frame.payload;
                            if (!(*ws_frag)[msg.handle].empty()) {
                                payload.insert(
                                    0, (*ws_frag)[msg.handle]);
                                (*ws_frag)[msg.handle].clear();
                            }
                            // payload 内可能多个 Pomelo Package
                            std::string tmp = std::move(payload);
                            while (!tmp.empty()) {
                                PomeloCodec::ParsedPkg pkg;
                                long n = PomeloCodec::parse_pkg(
                                    tmp, pkg);
                                if (n <= 0)
                                    break; // 残余忽略（坏帧防御）
                                tmp.erase(0,
                                          static_cast<size_t>(n));
                                handle_package(msg.handle, pkg);
                            }
                            continue;
                        }
                        if (frame.opcode == WsCodec::kOpContinuation) {
                            // 续帧：合并到已累积分片
                            if (!(*ws_frag)[msg.handle].empty()) {
                                auto& frag =
                                    (*ws_frag)[msg.handle];
                                frag += frame.payload;
                                if (frame.fin) {
                                    std::string tmp =
                                        std::move(frag);
                                    (*ws_frag)[msg.handle].clear();
                                    while (!tmp.empty()) {
                                        PomeloCodec::ParsedPkg pkg;
                                        long n =
                                            PomeloCodec::parse_pkg(
                                                tmp, pkg);
                                        if (n <= 0)
                                            break;
                                        tmp.erase(
                                            0,
                                            static_cast<size_t>(n));
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
                 ws_frag_op, uid_map](caf::io::connection_closed_msg& msg) {
                    rx->erase(msg.handle);
                    bad->erase(msg.handle);
                    ready->erase(msg.handle);
                    transport->erase(msg.handle);
                    ws_hs_done->erase(msg.handle);
                    ws_frag->erase(msg.handle);
                    ws_frag_op->erase(msg.handle);
                    // 清理该连接的 uid 绑定（定向 PUSH 不再投递）
                    for (auto it = uid_map->begin();
                         it != uid_map->end();) {
                        if (it->second == msg.handle)
                            it = uid_map->erase(it);
                        else
                            ++it;
                    }
                    LOG_INFO("[Pomelo] connection closed: "
                             + std::to_string(msg.handle.id()));
                },
                [self, rx, ready, uid_map, write_frame](
                    write_atom, std::string uid,
                    const std::string& data) {
                    // 定向 PUSH：写指定 uid 的连接
                    auto it = uid_map->find(uid);
                    if (it == uid_map->end()) {
                        LOG_WARN("[Pomelo] push to unknown uid=" + uid
                                 + ", dropped");
                        return;
                    }
                    write_frame(it->second, data);
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
