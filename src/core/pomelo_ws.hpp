#pragma once
// ------------------------------------------------------------------
// WebSocket 服务器端编解码（RFC 6455 子集）—— Pomelo 的浏览器传输层
//
// 浏览器客户端（pomelo-jsclient-websocket）无法裸 TCP，只能走 WS：
// 本模块把 WS 帧编解码 + 握手独立出来，Pomelo 协议帧（Package 层）
// 作为 WS 消息 payload 原样传输——协议与通信分离。
//
// 支持子集（够浏览器用）：
//   - 握手：Sec-WebSocket-Accept = base64(SHA1(key + GUID))
//   - 数据帧：opcode 1(text)/2(binary)，FIN 分片合并，客户端 mask 解包
//   - 控制帧：PING(9) → PONG(10)、CLOSE(8)（外部决定关闭动作）
//   - 服务器→客户端：无 mask
//
// 纯函数，零依赖（手写 SHA1/base64，避免引入 OpenSSL/BCrypt）。
// ------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <vector>

namespace caf_plugin_system {

struct WsCodec {
    // ---- 帧 opcode ----
    static constexpr std::uint8_t kOpContinuation = 0x0;
    static constexpr std::uint8_t kOpText = 0x1;
    static constexpr std::uint8_t kOpBinary = 0x2;
    static constexpr std::uint8_t kOpClose = 0x8;
    static constexpr std::uint8_t kOpPing = 0x9;
    static constexpr std::uint8_t kOpPong = 0xA;

    /// HTTP 握手请求 → 101 响应（含 Sec-WebSocket-Accept）。
    /// key：请求头 Sec-WebSocket-Key 的值。
    static std::string handshake_response(const std::string& key);

    /// 从缓冲头部解析一个 WS 帧。
    /// 返回 0 = 数据不足；>0 = 消费字节数；-1 = 协议错误。
    /// fin：是否结束帧；opcode：帧类型；payload：解 mask 后的载荷。
    struct ParsedFrame {
        bool fin = true;
        std::uint8_t opcode = 0;
        std::vector<std::byte> payload;
    };
    static long parse_frame(const std::vector<std::byte>& buf,
                            ParsedFrame& out);

    /// 编码一帧（服务器 → 客户端，无 mask）。
    static std::vector<std::byte> encode_frame(std::uint8_t opcode,
                                               const std::vector<std::byte>&
                                                   payload);

    /// 判断缓冲头部是否以 HTTP 请求行开头（"GET "）——用于连接级
    /// transport 探测（同端口 TCP/WS 自动识别）。
    static bool looks_like_http_upgrade(const std::vector<std::byte>& buf);
};

} // namespace caf_plugin_system
