#pragma once
// ------------------------------------------------------------------
// Pomelo 协议编解码器（pomelo-protocol 0.1.6 兼容，仅字符串 route）
//
// 官方协议文档：
//   - Communication Protocol（帧格式权威说明）:
//     https://github.com/NetEase/pomelo/wiki/Communication-Protocol
//   - npm 包源码（本实现以其为准）:
//     https://www.npmjs.com/package/pomelo-protocol
//
// 两层封装：
//   Package 层：4B 头 = type(1B) + body 长度(3B 大端)
//     type: 1=HANDSHAKE 2=HANDSHAKE_ACK 3=HEARTBEAT 4=DATA 5=KICK
//   Message 层（DATA 包的 body 内）：
//     flag(1B) [msgid varint] [route] [body]
//     flag: bit0=compressRoute, bit1-3=type, bit4=compressGzip
//     type: 0=REQUEST(带id+route) 1=NOTIFY(无id+route)
//           2=RESPONSE(带id无route) 3=PUSH(无id+route)
//     msgid: 7 位 varint（小端序组，高位 1 表示续组，≤5B）
//     route: 1B 长度 + UTF-8 字符串（≤255B）或 2B 大端 route code
//
// 本实现只支持字符串 route：compressRoute / compressGzip 置位视为
// 协议错误（decode 返回 false），防御式拒绝未知变体。
//
// 字节容器统一用 std::string（二进制安全，size() 跟踪长度；CAF
// write(void*, size) 直传 data() 零转换；帧拼接用 + 直接拼）。
// ------------------------------------------------------------------

#include <cstdint>
#include <string>

namespace caf_plugin_system {

struct PomeloCodec {
    // ---- Package 层 ----
    enum PkgType : std::uint8_t {
        kPkgHandshake = 1,
        kPkgHandshakeAck = 2,
        kPkgHeartbeat = 3,
        kPkgData = 4,
        kPkgKick = 5,
    };

    // ---- Message 层 ----
    enum MsgType : std::uint8_t {
        kMsgRequest = 0,
        kMsgNotify = 1,
        kMsgResponse = 2,
        kMsgPush = 3,
    };

    static constexpr std::uint32_t kMaxBody = 16u * 1024u * 1024u; // 16MB 上限

    /// 编码 Package 帧：4B 头 + body（body 可为任意二进制）。
    static std::string encode_pkg(std::uint8_t type,
                                  const std::string& body) {
        std::string out;
        out.reserve(4 + body.size());
        out.push_back(static_cast<char>(type & 0xff));
        std::uint32_t len = static_cast<std::uint32_t>(body.size());
        out.push_back(static_cast<char>((len >> 16) & 0xff));
        out.push_back(static_cast<char>((len >> 8) & 0xff));
        out.push_back(static_cast<char>(len & 0xff));
        out += body;
        return out;
    }

    /// 编码 Message 帧（字符串 route；compressRoute/gzip 恒 false）。
    static std::string encode_msg(std::uint32_t id, std::uint8_t type,
                                  const std::string& route,
                                  const std::string& body) {
        std::string out;
        // flag = (type << 1) | 0（不压缩）
        out.push_back(static_cast<char>((type << 1) & 0xff));
        // id：REQUEST/RESPONSE 才有
        if (type == kMsgRequest || type == kMsgResponse) {
            std::uint32_t v = id;
            do {
                std::uint8_t tmp = static_cast<std::uint8_t>(v % 128);
                v /= 128;
                if (v != 0)
                    tmp |= 0x80;
                out.push_back(static_cast<char>(tmp));
            } while (v != 0);
        }
        // route：REQUEST/NOTIFY/PUSH 才有（1B 长度 + UTF-8）
        if (type == kMsgRequest || type == kMsgNotify || type == kMsgPush) {
            if (route.size() > 255)
                return {}; // route 超长，编码失败
            out.push_back(static_cast<char>(route.size() & 0xff));
            out += route;
        }
        out += body;
        return out;
    }

    struct ParsedPkg {
        std::uint8_t type = 0;
        std::string body;
    };

    struct ParsedMsg {
        bool valid = false;
        std::uint32_t id = 0;
        std::uint8_t type = 0;
        std::string route;
        std::string body;
    };

    /// 从缓冲头部尝试解析一个 Package 帧。
    /// 返回 0 = 数据不足；>0 = 消费字节数；-1 = 协议错误（断开）。
    static long parse_pkg(const std::string& buf, ParsedPkg& out) {
        if (buf.size() < 4)
            return 0;
        std::uint32_t len = (static_cast<std::uint32_t>(
                                 static_cast<unsigned char>(buf[1]))
                             << 16)
                          | (static_cast<std::uint32_t>(
                                 static_cast<unsigned char>(buf[2]))
                             << 8)
                          | static_cast<std::uint32_t>(
                                static_cast<unsigned char>(buf[3]));
        if (len > kMaxBody)
            return -1;
        if (buf.size() < 4u + len)
            return 0;
        out.type = static_cast<std::uint8_t>(buf[0]);
        out.body.assign(buf, 4, len);
        return 4 + static_cast<long>(len);
    }

    /// 解析 Message（DATA 包的 body）。
    static bool parse_msg(const std::string& data, ParsedMsg& out) {
        size_t off = 0;
        if (data.size() < 1)
            return false;
        std::uint8_t flag = static_cast<std::uint8_t>(data[off++]);
        const bool compress_route = (flag & 0x01) != 0;
        const bool compress_gzip = (flag & 0x10) != 0;
        out.type = static_cast<std::uint8_t>((flag >> 1) & 0x07);
        if (compress_route || compress_gzip)
            return false; // 不支持压缩变体
        if (out.type > kMsgPush)
            return false; // 未知消息类型
        // id：REQUEST/RESPONSE
        if (out.type == kMsgRequest || out.type == kMsgResponse) {
            std::uint32_t id = 0;
            std::uint32_t shift = 0;
            for (int i = 0; i < 5; ++i) { // 最多 5 字节
                if (off >= data.size())
                    return false;
                std::uint8_t m =
                    static_cast<std::uint8_t>(data[off++]);
                id |= static_cast<std::uint32_t>(m & 0x7f) << shift;
                if ((m & 0x80) == 0) {
                    out.id = id;
                    break;
                }
                shift += 7;
                if (i == 4)
                    return false; // 超过 5 字节 = 错误
            }
        }
        // route：REQUEST/NOTIFY/PUSH
        if (out.type == kMsgRequest || out.type == kMsgNotify
            || out.type == kMsgPush) {
            if (off >= data.size())
                return false;
            std::uint8_t rlen = static_cast<std::uint8_t>(data[off++]);
            if (off + rlen > data.size())
                return false;
            out.route.assign(data, off, rlen);
            off += rlen;
        }
        out.body.assign(data, off, std::string::npos);
        out.valid = true;
        return true;
    }
};

} // namespace caf_plugin_system
