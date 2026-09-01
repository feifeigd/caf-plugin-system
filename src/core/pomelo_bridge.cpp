#include "pomelo_bridge.hpp"

#include "common/message_tags.hpp"
#include "common/plugin_envelope.hpp"
#include "pomelo_codec.hpp"
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
const char* k_handshake_json =
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

} // namespace

// ------------------------------------------------------------------
// spawn_pomelo_bridge —— broker 事件驱动（多连接版，2026-09-01）
//
// 多连接语义：每个 TCP 连接独立握手（各自 HANDSHAKE→ACK），REQUEST
// 的响应写回【发起请求的那个连接】（按 connection_handle 路由），
// 一个连接的协议错误/关闭不影响其他连接。
//
// 连接流程：accept → 发 HANDSHAKE → 等 ACK → DATA 就绪。
// REQUEST：route 查表 → resolve svc → envelope 调用 → RESPONSE 回原连接。
// NOTIFY：envelope 单向（fire-and-forget，无响应帧）。
// HEARTBEAT：原样互答。
// ------------------------------------------------------------------
caf::actor spawn_pomelo_bridge(caf::actor_system& sys, caf::actor registry,
                               std::uint16_t port,
                               const pomelo_route_table& routes,
                               const std::string& node_name) {
    auto bridge = sys.middleman().spawn_broker(
        [registry, port, routes, node_name](caf::io::broker* self) {
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
                     + std::to_string(routes.size()) + ")");

            // 每连接读缓冲（rx）；bad = 协议错误连接（忽略后续数据）
            auto rx = std::make_shared<std::map<
                caf::io::connection_handle, std::vector<std::byte>>>();
            auto bad = std::make_shared<std::set<caf::io::connection_handle>>();
            // 每连接握手状态：false = 等 HANDSHAKE_ACK，true = DATA 就绪
            auto ready = std::make_shared<std::map<
                caf::io::connection_handle, bool>>();

            // 写一帧到指定连接；连接已关/已坏则不写（响应回调到达时
            // 连接可能已断开——多连接下必须按目标 handle 判定存活）
            auto write_frame = [self, rx, bad](
                                   caf::io::connection_handle h,
                                   const std::string& frame) {
                if (h.invalid() || !rx->count(h) || bad->count(h))
                    return;
                self->write(h, caf::make_span(
                                   reinterpret_cast<const std::byte*>(
                                       frame.data()),
                                   frame.size()));
                self->flush(h);
            };

            auto send_handshake = [self, write_frame](
                                      caf::io::connection_handle h) {
                // char* 迭代器不能直接构造 vector<std::byte>（隐式转换
                // 不存在）——先分配再 memcpy。
                const size_t hlen = std::strlen(k_handshake_json);
                std::vector<std::byte> body(hlen);
                std::memcpy(body.data(), k_handshake_json, hlen);
                auto pkg = PomeloCodec::encode_pkg(
                    PomeloCodec::kPkgHandshake, body);
                auto frame = frame_string(PomeloCodec::kPkgHandshake, body);
                write_frame(h, frame);
            };

            return caf::behavior{
                [self, rx, bad, ready,
                 send_handshake](caf::io::new_connection_msg& msg) {
                    self->configure_read(
                        msg.handle, caf::io::receive_policy::at_least(1));
                    (*rx)[msg.handle].clear();
                    bad->erase(msg.handle);
                    (*ready)[msg.handle] = false;
                    LOG_INFO("[Pomelo] new connection: "
                             + std::to_string(msg.handle.id()));
                    send_handshake(msg.handle);
                },
                [self, rx, bad, ready, registry, routes, node_name,
                 write_frame](caf::io::new_data_msg& msg) {
                    if (bad->count(msg.handle))
                        return;
                    auto& buf = (*rx)[msg.handle];
                    buf.insert(buf.end(), msg.buf.begin(), msg.buf.end());
                    LOG_INFO("[Pomelo] rx " + std::to_string(msg.buf.size())
                             + "B from " + std::to_string(msg.handle.id()));

                    // 本连接的响应写回（REQUEST/未知 route 错误响应用）
                    auto respond_err = [self, rx, bad, write_frame](
                                           caf::io::connection_handle h,
                                           std::uint32_t id,
                                           const std::string& text) {
                        write_frame(h, response_frame(id, text));
                    };

                    while (true) {
                        PomeloCodec::ParsedPkg pkg;
                        long consumed = PomeloCodec::parse_pkg(buf, pkg);
                        if (consumed == 0)
                            break; // 数据不足，等更多
                        if (consumed < 0) {
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            ready->erase(msg.handle);
                            LOG_WARN("[Pomelo] protocol error, closing conn "
                                     + std::to_string(msg.handle.id()));
                            break;
                        }
                        buf.erase(buf.begin(), buf.begin() + consumed);

                        if (pkg.type == PomeloCodec::kPkgHandshakeAck) {
                            (*ready)[msg.handle] = true;
                            LOG_INFO("[Pomelo] conn "
                                     + std::to_string(msg.handle.id())
                                     + " handshake ack, DATA ready");
                            continue;
                        }
                        if (pkg.type == PomeloCodec::kPkgHeartbeat) {
                            // 心跳互答（空 body）回【本连接】
                            write_frame(
                                msg.handle,
                                frame_string(PomeloCodec::kPkgHeartbeat, {}));
                            continue;
                        }
                        if (pkg.type != PomeloCodec::kPkgData) {
                            LOG_WARN("[Pomelo] unexpected pkg type "
                                     + std::to_string(pkg.type));
                            continue; // KICK 等：忽略
                        }

                        // DATA 包：解析 Message
                        PomeloCodec::ParsedMsg msg2;
                        if (!PomeloCodec::parse_msg(pkg.body, msg2)
                            || !msg2.valid) {
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            ready->erase(msg.handle);
                            LOG_WARN("[Pomelo] bad message frame, closing");
                            break;
                        }

                        if (msg2.type == PomeloCodec::kMsgRequest) {
                            // REQUEST：route 查表 → 内部 envelope 调用，
                            // 响应写回【发起连接 msg.handle】
                            auto it = routes.find(msg2.route);
                            if (it == routes.end()) {
                                LOG_WARN("[Pomelo] unknown route: "
                                         + msg2.route);
                                respond_err(msg.handle, msg2.id,
                                            "unknown route: " + msg2.route);
                                continue;
                            }
                            std::string svc, function;
                            if (!split_svc_function(it->second, svc,
                                                    function)) {
                                respond_err(msg.handle, msg2.id,
                                            "bad route target: "
                                                + it->second);
                                continue;
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
                                     + std::to_string(msg.handle.id())
                                     + " REQUEST id="
                                     + std::to_string(msg2.id) + " route="
                                     + r + " svc=" + s + " function=" + f
                                     + " len="
                                     + std::to_string(body.size()));
                            const caf::io::connection_handle src =
                                msg.handle;
                            self->request(registry, std::chrono::seconds(2),
                                          resolve_atom_v, svc)
                                .then(
                                    [self, rx, write_frame, src, svc,
                                     function, body,
                                     msg_id = msg2.id](
                                        caf::actor& proxy) {
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
                                                [rx, write_frame, src,
                                                 msg_id](std::string& r2) {
                                                    write_frame(
                                                        src,
                                                        response_frame(
                                                            msg_id,
                                                            std::move(r2)));
                                                },
                                                [rx, write_frame, src,
                                                 msg_id](caf::error& e) {
                                                    write_frame(
                                                        src,
                                                        response_frame(
                                                            msg_id,
                                                            caf::to_string(
                                                                e)));
                                                });
                                    },
                                    [rx, write_frame, src,
                                     msg_id = msg2.id](caf::error& e) {
                                        write_frame(
                                            src,
                                            response_frame(msg_id,
                                                           caf::to_string(
                                                               e)));
                                    });
                        } else if (msg2.type == PomeloCodec::kMsgNotify) {
                            // NOTIFY：单向 envelope（fire-and-forget）
                            auto it = routes.find(msg2.route);
                            if (it == routes.end()) {
                                LOG_WARN("[Pomelo] unknown route (notify): "
                                         + msg2.route);
                                continue;
                            }
                            std::string svc, function;
                            if (!split_svc_function(it->second, svc,
                                                    function)) {
                                LOG_WARN("[Pomelo] bad route target: "
                                         + it->second);
                                continue;
                            }
                            std::string body(
                                reinterpret_cast<const char*>(
                                    msg2.body.data()),
                                msg2.body.size());
                            const std::string r = msg2.route;
                            const std::string s = svc;
                            const std::string f = function;
                            LOG_INFO("[Pomelo] conn "
                                     + std::to_string(msg.handle.id())
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
                    }
                },
                [self, rx, bad, ready](caf::io::connection_closed_msg& msg) {
                    rx->erase(msg.handle);
                    bad->erase(msg.handle);
                    ready->erase(msg.handle);
                    LOG_INFO("[Pomelo] connection closed: "
                             + std::to_string(msg.handle.id()));
                },
                [self, rx, write_frame](write_atom,
                                        const std::string& data) {
                    // 备用写通道：写最近存活连接（当前无外部调用者，
                    // 外部交互全走 REQUEST/NOTIFY 响应路径）
                    if (!rx->empty()) {
                        auto h = rx->rbegin()->first; // map 有序：最大 id
                        write_frame(h, data);
                    }
                },
                [self](caf::io::data_transferred_msg&) {
                    // ack_writes(true) 的写完成通知；无操作
                },
                [self](caf::exit_msg&) { self->quit(); }};
        });

    return bridge;
}

} // namespace caf_plugin_system
