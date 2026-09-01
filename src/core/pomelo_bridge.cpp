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
// spawn_pomelo_bridge —— broker 事件驱动（照 bridge_actor 模式）
//
// v1 单连接语义（与行协议 bridge 相同）：写"最近连接"。
// 连接流程：accept → 发 HANDSHAKE → 等 ACK → DATA 就绪。
// REQUEST：route 查表 → resolve svc → envelope 调用 → RESPONSE 回。
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

            // 每连接读缓冲；bad = 协议错误连接（忽略后续数据，等对端关）
            auto rx = std::make_shared<std::map<
                caf::io::connection_handle, std::vector<std::byte>>>();
            auto bad = std::make_shared<std::set<caf::io::connection_handle>>();
            // 当前连接（v1 单连接语义：写最近连接）
            auto current = std::make_shared<caf::io::connection_handle>();
            // 连接握手状态：false = 等 HANDSHAKE_ACK，true = DATA 就绪
            auto ready = std::make_shared<bool>(false);

            auto send_handshake = [self, current] {
                if (current->invalid())
                    return;
                // char* 迭代器不能直接构造 vector<std::byte>（隐式转换
                // 不存在）——先分配再 memcpy。
                const size_t hlen = std::strlen(k_handshake_json);
                std::vector<std::byte> body(hlen);
                std::memcpy(body.data(), k_handshake_json, hlen);
                auto pkg = PomeloCodec::encode_pkg(
                    PomeloCodec::kPkgHandshake, body);
                self->write(*current,
                            caf::make_span(pkg.data(), pkg.size()));
                self->flush(*current);
            };

            return caf::behavior{
                [self, rx, bad, current, ready,
                 send_handshake](caf::io::new_connection_msg& msg) {
                    self->configure_read(
                        msg.handle, caf::io::receive_policy::at_least(1));
                    *current = msg.handle;
                    (*rx)[msg.handle].clear();
                    bad->erase(msg.handle);
                    *ready = false;
                    LOG_INFO("[Pomelo] new connection: "
                             + std::to_string(msg.handle.id()));
                    send_handshake();
                },
                [self, rx, bad, current, ready, registry, routes,
                 node_name](caf::io::new_data_msg& msg) {
                    if (bad->count(msg.handle))
                        return;
                    auto& buf = (*rx)[msg.handle];
                    buf.insert(buf.end(), msg.buf.begin(), msg.buf.end());
                    LOG_INFO("[Pomelo] rx " + std::to_string(msg.buf.size())
                             + "B from " + std::to_string(msg.handle.id()));

                    auto respond_err = [self, current](
                                           std::uint32_t id,
                                           const std::string& text) {
                        if (current->invalid())
                            return;
                        auto frame = response_frame(id, text);
                        self->write(
                            *current,
                            caf::make_span(
                                reinterpret_cast<const std::byte*>(
                                    frame.data()),
                                frame.size()));
                        self->flush(*current);
                    };

                    while (true) {
                        PomeloCodec::ParsedPkg pkg;
                        long consumed = PomeloCodec::parse_pkg(buf, pkg);
                        if (consumed == 0)
                            break; // 数据不足，等更多
                        if (consumed < 0) {
                            bad->insert(msg.handle);
                            rx->erase(msg.handle);
                            LOG_WARN("[Pomelo] protocol error, closing conn "
                                     + std::to_string(msg.handle.id()));
                            break;
                        }
                        buf.erase(buf.begin(), buf.begin() + consumed);

                        if (pkg.type == PomeloCodec::kPkgHandshakeAck) {
                            *ready = true;
                            LOG_INFO("[Pomelo] handshake ack, DATA ready");
                            continue;
                        }
                        if (pkg.type == PomeloCodec::kPkgHeartbeat) {
                            // 心跳互答（空 body）
                            auto frame = frame_string(
                                PomeloCodec::kPkgHeartbeat, {});
                            self->write(
                                *current,
                                caf::make_span(
                                    reinterpret_cast<const std::byte*>(
                                        frame.data()),
                                    frame.size()));
                            self->flush(*current);
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
                            LOG_WARN("[Pomelo] bad message frame, closing");
                            break;
                        }

                        if (msg2.type == PomeloCodec::kMsgRequest) {
                            // REQUEST：route 查表 → 内部 envelope 调用
                            auto it = routes.find(msg2.route);
                            if (it == routes.end()) {
                                LOG_WARN("[Pomelo] unknown route: "
                                         + msg2.route);
                                respond_err(msg2.id,
                                            "unknown route: " + msg2.route);
                                continue;
                            }
                            std::string svc, function;
                            if (!split_svc_function(it->second, svc,
                                                    function)) {
                                respond_err(
                                    msg2.id,
                                    "bad route target: " + it->second);
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
                            LOG_INFO("[Pomelo] REQUEST id="
                                     + std::to_string(msg2.id) + " route="
                                     + r + " svc=" + s + " function=" + f
                                     + " len="
                                     + std::to_string(body.size()));
                            self->request(registry, std::chrono::seconds(2),
                                          resolve_atom_v, svc)
                                .then(
                                    [self, registry, svc, function, body,
                                     msg_id = msg2.id, respond_err](
                                        caf::actor& proxy) {
                                        if (!proxy) {
                                            respond_err(
                                                msg_id,
                                                "service not found: " + svc);
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
                                                [msg_id, respond_err](
                                                    std::string& r) {
                                                    respond_err(msg_id,
                                                                std::move(r));
                                                },
                                                [msg_id, respond_err](
                                                    caf::error& e) {
                                                    respond_err(
                                                        msg_id,
                                                        caf::to_string(e));
                                                });
                                    },
                                    [msg_id = msg2.id, respond_err](
                                        caf::error& e) {
                                        respond_err(msg_id,
                                                    caf::to_string(e));
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
                            LOG_INFO("[Pomelo] NOTIFY route=" + r + " svc="
                                     + s + " function=" + f + " len="
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
                [self, rx, bad, current](caf::io::connection_closed_msg& msg) {
                    rx->erase(msg.handle);
                    bad->erase(msg.handle);
                    if (!current->invalid() && *current == msg.handle)
                        *current = caf::io::connection_handle{};
                },
                [self, current](write_atom, const std::string& data) {
                    // 备用写通道（v1 单连接语义）
                    if (!current->invalid()) {
                        self->write(*current,
                                    caf::make_span(
                                        reinterpret_cast<const std::byte*>(
                                            data.data()),
                                        data.size()));
                        self->flush(*current);
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
