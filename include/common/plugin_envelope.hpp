#pragma once

// ------------------------------------------------------------------
// plugin_envelope —— 插件私有子协议的【公共信封】（内核协议类型，ID 231）
//
// 动机：插件私有消息类型要占全局 type_id 段并自注册元对象（见
// plugin_interface.hpp 的 register_meta_objects）。信封模式让所有
// 插件私有消息共用这一个内核类型：方法名 function 由各插件自行命名，
// 不同插件可用同名方法，互不冲突，零号段协调。
//
// 消息模型：MFA（Erlang 语义，2026-08-31 重构定稿）
//   - M（module）：服务名 svc，在路由层（service_registry resolve、
//     bridge 行协议 CALL 的 svc 字段、集群 cross_call(svc, env)）——
//     envelope 不带 module，本地已 resolve、集群成对传递，冗余即风险。
//   - F（function）：方法名字符串（"echo"/"reload"/"bridge"……），
//     替代旧 sub_proto 数字协议号。日志/tracing 直接可读，插件间
//     零协议号协调。
//   - A（arguments）：args 载荷（JSON/raw 字节流）。
//
// 外部边界安全（协议号不暴露函数名）：
//   外部客户端（bridge 行协议）用【协议号】（数字，服务注册时声明
//   的 协议号→function 契约表），bridge 翻译成内部 function 后转发。
//   内部消息/集群/脚本一律 MFA，函数名不出内网线上协议。
//
// ┌─ 利 ────────────────────────────────────────────────┐
//   · 插件不占 type_id 段、不需要 register_meta_objects 导出，
//     真正的"扔 DLL 进 plugins/ 即用"
//   · 内核只增加这一个类型，message_tags.def 稳定不变
//   · args 只用 CAF 内置类型（std::vector<std::byte>），免注册
//   · function 自描述：日志/抓包/跨语言对接直接可见方法名
// └─────────────────────────────────────────────────────┘
// ┌─ 弊 ────────────────────────────────────────────────┐
//   · 失去 CAF 的类型匹配：所有私有消息都长成同一个信封，
//     handler 退化为统一入口 + 手工分发（按 function 名）
//   · request/response 类型安全变弱（响应只能是信封或约定类型）
//   · args 是字节流：结构化数据要插件自己序列化/反序列化
// └─────────────────────────────────────────────────────┘
//
// 与"私有类型 + 自注册"（business_plugin.cpp 里的 biz_ping_atom 示例）
// 不是替代关系：内部高频结构化消息用私有类型（类型安全）；
// 松散耦合、结构简单、不想占号段的业务调用用本信封。
// ------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// args 的编解码方式。新增格式只追加枚举值；未实现的格式必须拒绝，不能
/// 猜测为字符串处理。
enum class payload_format : std::uint8_t {
    raw = 0,       ///< 不解释的原始字节。
    utf8_json = 1, ///< UTF-8 JSON；脚本插件的默认格式。
    msgpack = 2,   ///< 预留：MessagePack。
    protobuf = 3,  ///< 预留：Protocol Buffers。
};

/// CAF 不会自动序列化 enum class；在线上传输其稳定的单字节表示。未知值
/// 保留到接收端，由具体 codec 返回"不支持"，从而允许以后只追加新格式。
template <class Inspector>
bool inspect(Inspector& f, payload_format& x) {
    auto value = static_cast<std::uint8_t>(x);
    if (!f.apply(value))
        return false;
    x = static_cast<payload_format>(value);
    return true;
}

/// 外部协议号 → 内部方法名 契约表（服务注册时声明，bridge 翻译用）。
/// 协议号是外部客户端的稳定契约（可版本化/废弃），function 是内部实现
/// 细节（可重构改名）——两边独立演进，函数名永不出现于外部行协议。
using external_protocol_table = std::map<std::uint16_t, std::string>;

struct plugin_envelope {
    std::string function;          // MFA 的 F：方法名（替代旧 sub_proto）
    payload_format format = payload_format::utf8_json;
    std::uint16_t version = 1;     // function 对应业务契约的版本
    std::vector<std::byte> payload; // MFA 的 A：参数载荷（CAF 内置类型）
};

// CAF 1.1 元对象注册需要 inspect 支持（见 common/message_meta.hpp）
template <class Inspector>
bool inspect(Inspector& f, plugin_envelope& x) {
    return f.object(x).fields(f.field("function", x.function),
                              f.field("format", x.format),
                              f.field("version", x.version),
                              f.field("payload", x.payload));
}

// ------------------------------------------------------------------
// 公共载荷编解码入口
//
// 所有 C++ 插件和脚本 host 都应经由这些函数处理文本载荷，禁止自行
// reinterpret_cast/assign。当前实现 raw 与 utf8_json；JSON 的语法/业务
// schema 校验属于各 function 的契约层，不在传输层重复实现。
// ------------------------------------------------------------------
namespace plugin_wire {

inline bool is_text_format(payload_format format) noexcept {
    return format == payload_format::raw || format == payload_format::utf8_json;
}

inline std::optional<plugin_envelope>
encode_text(std::string_view function, std::string_view text,
            payload_format format = payload_format::utf8_json,
            std::uint16_t version = 1) {
    if (!is_text_format(format))
        return std::nullopt;
    plugin_envelope env;
    env.function = std::string(function);
    env.format = format;
    env.version = version;
    env.args.assign(reinterpret_cast<const std::byte*>(text.data()),
                    reinterpret_cast<const std::byte*>(text.data()) + text.size());
    return env;
}

inline std::optional<std::string> decode_text(const plugin_envelope& env) {
    if (!is_text_format(env.format))
        return std::nullopt;
    return std::string(reinterpret_cast<const char*>(env.args.data()),
                       env.args.size());
}

} // namespace plugin_wire
