#pragma once

// ------------------------------------------------------------------
// 自定义消息类型的【运行时元对象】注册 —— CAF 1.1 必需
//
// CAF 1.1 的消息存储/销毁完全通过全局元对象表做类型擦除
// （见 caf/detail/message_data.cpp：meta.destroy(ptr) 逐元素调用）。
// 只有编译期 type_id（message_tags.def 的显式特化）是不够的：
// 未注册的类型在表里是全零 meta_object，消息析构时调用空指针
// destroy → 进程崩溃（0xC0000005 exec at 0x0）。
//
// 因此在创建任何 actor_system 之前，必须调用 app_meta::init()
// 把元对象填进全局表。元对象表与 message_tags.def 同一份列表展开，
// 位置 i 对应 ID first_custom_type_id + i，detail_check 命名空间里的
// static_assert 在编译期强制校验这个对应关系。
// ------------------------------------------------------------------

#include "common/message_tags.hpp" // 经 common/system_state.hpp 拿到 SystemState 完整定义

#include <caf/detail/make_meta_object.hpp>
#include <caf/detail/meta_object.hpp>
#include <caf/span.hpp>

namespace app_meta {

namespace detail_check {

// 枚举值 = 条目在 message_tags.def 中的位置（0 起）
enum : caf::type_id_t {
#define X_TAG(name, id) idx_##name,
#define X_REG(key, type_expr, id, display_name) idx_##key,
#include "common/message_tags.def"
#undef X_TAG
#undef X_REG
};

// 位置必须与显式 ID 一致，否则元对象表错位（等价于未注册 → 运行时崩溃）
#define X_TAG(name, id)                                                        \
  static_assert(id == caf::first_custom_type_id + idx_##name,                  \
                "message_tags.def: " #name " 的列表位置与显式 ID 不一致");
#define X_REG(key, type_expr, id, display_name)                                \
  static_assert(id == caf::first_custom_type_id + idx_##key,                   \
                "message_tags.def: " #key " 的列表位置与显式 ID 不一致");
#include "common/message_tags.def"
#undef X_TAG
#undef X_REG

} // namespace detail_check

/// 注册本项目全部自定义消息类型的元对象（ID 200 起，顺序 = message_tags.def）。
/// @warning 必须在构造任何 caf::actor_system 之前调用。
inline void init() {
    using caf::detail::make_meta_object;
    const caf::detail::meta_object xs[] = {
#define X_TAG(name, id) make_meta_object<name>(#name),
#define X_REG(key, type_expr, id, display_name) \
        make_meta_object<CAF_PP_EXPAND type_expr>(display_name),
#include "common/message_tags.def"
#undef X_TAG
#undef X_REG
    };
    caf::detail::set_global_meta_objects(caf::first_custom_type_id,
                                         caf::make_span(xs));
}

} // namespace app_meta
