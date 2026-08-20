#pragma once

// ------------------------------------------------------------------
// plugin_envelope —— 插件私有子协议的【公共信封】（内核协议类型，ID 231）
//
// 动机：插件私有消息类型要占全局 type_id 段并自注册元对象（见
// plugin_interface.hpp 的 register_meta_objects）。信封模式让所有
// 插件私有消息共用这一个内核类型：子协议号 sub_proto 由各插件自行编号，
// 不同插件可以用相同的子协议号，互不冲突，零号段协调。
//
// ┌─ 利 ────────────────────────────────────────────────┐
//   · 插件不占 type_id 段、不需要 register_meta_objects 导出，
//     真正的"扔 DLL 进 plugins/ 即用"
//   · 内核只增加这一个类型，message_tags.def 稳定不变
//   · payload 只用 CAF 内置类型（std::vector<std::byte>），免注册
// └─────────────────────────────────────────────────────┘
// ┌─ 弊 ────────────────────────────────────────────────┐
//   · 失去 CAF 的类型匹配：所有私有消息都长成同一个信封，
//     handler 退化为统一入口 + 手工 switch(sub_proto) 二级分发
//   · request/response 类型安全变弱（响应只能是信封或约定类型）
//   · payload 是字节流：结构化数据要插件自己序列化/反序列化
//   · 框架日志/tracing 只能看到"一个信封"，看不到子协议细节
// └─────────────────────────────────────────────────────┘
//
// 与"私有类型 + 自注册"（business_plugin.cpp 里的 biz_ping_atom 示例）
// 不是替代关系：内部高频结构化消息用私有类型（类型安全）；
// 松散耦合、结构简单、不想占号段的消息用本信封。
// ------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>

struct plugin_envelope {
    std::uint16_t sub_proto = 0;        // 插件内部子协议号（各插件自行管理，可跨插件重复）
    std::vector<std::byte> payload;     // 载荷（CAF 内置类型，无需额外注册）
};

// CAF 1.1 元对象注册需要 inspect 支持（见 common/message_meta.hpp）
template <class Inspector>
bool inspect(Inspector& f, plugin_envelope& x) {
    return f.object(x).fields(f.field("sub_proto", x.sub_proto),
                              f.field("payload", x.payload));
}
