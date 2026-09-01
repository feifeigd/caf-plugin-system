#include "pomelo_ws.hpp"

#include <cstring>

namespace caf_plugin_system {

namespace {

// ------------------------------------------------------------------
// SHA1（RFC 3174）—— 手写实现，避免 OpenSSL/BCrypt 依赖
// ------------------------------------------------------------------
struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu,
                          0x10325476u, 0xC3D2E1F0u};
    std::uint64_t len = 0; // 已处理字节数
    std::uint8_t buf[64];
    size_t buflen = 0;

    static std::uint32_t rol(std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void block(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(p[i * 4]) << 24)
                 | (std::uint32_t(p[i * 4 + 1]) << 16)
                 | (std::uint32_t(p[i * 4 + 2]) << 8)
                 | std::uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            std::uint32_t tmp = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = tmp;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }

    void update(const std::uint8_t* data, size_t n) {
        len += n;
        while (n > 0) {
            size_t take = 64 - buflen;
            if (take > n)
                take = n;
            std::memcpy(buf + buflen, data, take);
            buflen += take;
            data += take;
            n -= take;
            if (buflen == 64) {
                block(buf);
                buflen = 0;
            }
        }
    }

    void finish(std::uint8_t digest[20]) {
        // 填充：0x80 + 零 + 64bit 位长度（大端）
        std::uint64_t bits = len * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        std::uint8_t zeros[64];
        std::memset(zeros, 0, sizeof(zeros));
        // 剩余空间 = 64 - buflen；最后 8 字节放长度（写 buf+buflen 处）
        size_t remain = 64 - buflen;
        if (remain < 8) {
            update(zeros, remain);
            remain = 64;
        }
        update(zeros, remain - 8);
        for (int i = 7; i >= 0; --i) {
            buf[buflen + i] = static_cast<std::uint8_t>(bits & 0xFF);
            bits >>= 8;
        }
        buflen += 8;
        block(buf);
        buflen = 0;
        for (int i = 0; i < 5; ++i) {
            digest[i * 4] = static_cast<std::uint8_t>(h[i] >> 24);
            digest[i * 4 + 1] = static_cast<std::uint8_t>(h[i] >> 16);
            digest[i * 4 + 2] = static_cast<std::uint8_t>(h[i] >> 8);
            digest[i * 4 + 3] = static_cast<std::uint8_t>(h[i]);
        }
    }
};

// ------------------------------------------------------------------
// base64（标准字母表，无 padding 变体给握手用也可——握手用标准带 =）
// ------------------------------------------------------------------
const char* k_b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                    "0123456789+/";

std::string base64(const std::uint8_t* data, size_t n) {
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        std::uint32_t v = std::uint32_t(data[i]) << 16;
        if (i + 1 < n)
            v |= std::uint32_t(data[i + 1]) << 8;
        if (i + 2 < n)
            v |= std::uint32_t(data[i + 2]);
        out.push_back(k_b64[(v >> 18) & 0x3F]);
        out.push_back(k_b64[(v >> 12) & 0x3F]);
        out.push_back(i + 1 < n ? k_b64[(v >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < n ? k_b64[v & 0x3F] : '=');
    }
    return out;
}

} // namespace

std::string WsCodec::handshake_response(const std::string& key) {
    // Sec-WebSocket-Accept = base64(SHA1(key + GUID))
    const char* k_ws_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    std::string joined = key + k_ws_guid;
    Sha1 sha;
    sha.update(reinterpret_cast<const std::uint8_t*>(joined.data()),
               joined.size());
    std::uint8_t digest[20];
    sha.finish(digest);
    std::string accept = base64(digest, 20);

    std::string resp = "HTTP/1.1 101 Switching Protocols\r\n"
                       "Upgrade: websocket\r\n"
                       "Connection: Upgrade\r\n"
                       "Sec-WebSocket-Accept: "
                       + accept + "\r\n\r\n";
    return resp;
}

long WsCodec::parse_frame(const std::string& buf, ParsedFrame& out) {
    if (buf.size() < 2)
        return 0;
    const std::uint8_t b0 =
        static_cast<std::uint8_t>(static_cast<unsigned char>(buf[0]));
    const std::uint8_t b1 =
        static_cast<std::uint8_t>(static_cast<unsigned char>(buf[1]));
    out.fin = (b0 & 0x80) != 0;
    out.opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    std::uint64_t len = b1 & 0x7F;
    size_t off = 2;
    if (len == 126) {
        if (buf.size() < 4)
            return 0;
        len = (std::uint64_t(static_cast<unsigned char>(buf[2])) << 8)
            | std::uint64_t(static_cast<unsigned char>(buf[3]));
        off = 4;
    } else if (len == 127) {
        if (buf.size() < 10)
            return 0;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8)
                | std::uint64_t(
                      static_cast<unsigned char>(buf[2 + i]));
        off = 10;
    }
    if (len > 64u * 1024u * 1024u)
        return -1; // 64MB 上限
    std::uint8_t mask_key[4] = {0, 0, 0, 0};
    if (masked) {
        if (buf.size() < off + 4)
            return 0;
        for (int i = 0; i < 4; ++i)
            mask_key[i] =
                static_cast<unsigned char>(buf[off + i]);
        off += 4;
    }
    if (buf.size() < off + len)
        return 0;
    out.payload.resize(static_cast<size_t>(len));
    for (size_t i = 0; i < len; ++i) {
        unsigned char v = static_cast<unsigned char>(buf[off + i]);
        if (masked)
            v ^= mask_key[i & 3];
        out.payload[i] = static_cast<char>(v);
    }
    return static_cast<long>(off + len);
}

std::string WsCodec::encode_frame(std::uint8_t opcode,
                                  const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(0x80 | (opcode & 0x0F))); // FIN=1
    const size_t len = payload.size();
    if (len < 126) {
        out.push_back(static_cast<char>(len)); // 服务器不 mask
    } else if (len < 65536) {
        out.push_back(static_cast<char>(126));
        out.push_back(static_cast<char>((len >> 8) & 0xFF));
        out.push_back(static_cast<char>(len & 0xFF));
    } else {
        out.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i)
            out.push_back(
                static_cast<char>((len >> (8 * i)) & 0xFF));
    }
    out += payload;
    return out;
}

bool WsCodec::looks_like_http_upgrade(const std::string& buf) {
    if (buf.size() < 4)
        return false;
    return std::memcmp(buf.data(), "GET ", 4) == 0;
}

} // namespace caf_plugin_system
