#pragma once

// ------------------------------------------------------------------
// SystemState —— 生命周期协议类型（health_check_atom 的返回类型）
//
// 单独放这里是因为消息标签注册（message_tags.def 的展开）需要
// 类型的完整声明，而展开点 message_tags.hpp 必须先于
// graceful_shutdown.hpp 被编译。协议类型前置到 common/ 后，
// 两个头文件都能拿到它。
// ------------------------------------------------------------------

enum class SystemState { initializing, ready, shutting_down, stopped };

// CAF 1.1 元对象注册需要 inspect 支持（见 common/message_meta.hpp）。
// 直接按底层整数序列化，避免 default_enum_inspect 的 to_string/from_string 要求。
template <class Inspector>
bool inspect(Inspector& f, SystemState& x) {
    if constexpr (Inspector::is_loading) {
        int v = 0;
        if (!f.apply(v)) return false;
        x = static_cast<SystemState>(v);
        return true;
    } else {
        auto v = static_cast<int>(x);
        return f.apply(v);
    }
}
