#pragma once

// ------------------------------------------------------------------
// 消息标签（atom）基础设施 —— CAF 1.1 迁移
//
// CAF 1.1 移除了 caf::atom / caf::atom_constant（旧 atom API）。
// 同时 CAF 要求消息中的每个自定义类型都有编译期 type_id
// （见 caf/message.hpp 的 static_assert: is_complete<type_id<T>>）。
//
// 本头文件只放机制，不放数据：
//   - CAF_MESSAGE_TAG  定义空标签结构体 + 显式 type_id 特化
//   - CAF_REGISTER_TYPE 为已有类型（枚举 / STL 容器）补 type_id 特化
// 全部标签与类型的清单在 message_tags.def（X-macro 唯一数据源），
// 由本文件和 message_meta.hpp 分别展开。新增标签只改 message_tags.def。
//
// 显式 ID 的理由（不要用顺序敏感的 CAF_ADD_TYPE_ID/__COUNTER__）：
//   本头文件被 exe 与每个插件 DLL 各自包含、各自编译；显式 ID 与
//   包含顺序/子集无关，跨 DLL 永远一致；重复 ID 编译期报错。
// ------------------------------------------------------------------

#include <caf/type_id.hpp>
#include <caf/detail/pp.hpp>

#include <map>
#include <string>
#include <string_view>
#include <vector>

// SystemState 等协议类型必须先于 def 展开完成声明（X_REG 要拿完整类型）
#include "common/system_state.hpp"
#include "common/plugin_envelope.hpp"

/// 定义一个空标签结构体并注册显式 type_id（旧 atom 的替代品）。
#define CAF_MESSAGE_TAG(tag_name, tag_id)                                      \
  struct tag_name {};                                                          \
  constexpr tag_name tag_name##_v = tag_name{};                                \
  [[maybe_unused]] constexpr bool operator==(tag_name, tag_name) {             \
    return true;                                                               \
  }                                                                            \
  [[maybe_unused]] constexpr bool operator!=(tag_name, tag_name) {             \
    return false;                                                              \
  }                                                                            \
  template <class Inspector>                                                   \
  bool inspect(Inspector& f, tag_name& x) {                                    \
    return f.object(x).fields();                                               \
  }                                                                            \
  namespace caf {                                                              \
  template <>                                                                  \
  struct type_id<::tag_name> {                                                 \
    static constexpr type_id_t value = tag_id;                                 \
  };                                                                           \
  template <>                                                                  \
  struct type_by_id<tag_id> {                                                  \
    using type = ::tag_name;                                                   \
  };                                                                           \
  template <>                                                                  \
  struct type_name<::tag_name> {                                               \
    static constexpr std::string_view value = #tag_name;                       \
  };                                                                           \
  template <>                                                                  \
  struct type_name_by_id<tag_id> : type_name<::tag_name> {};                   \
  }

/// 为已有类型（枚举、STL 容器等）注册显式 type_id，不定义新类型。
/// type_expr 必须用括号包起来，以容纳带逗号的模板类型，如 (std::map<int, int>)。
#define CAF_REGISTER_TYPE(type_expr, tag_id, display_name)                     \
  namespace caf {                                                              \
  template <>                                                                  \
  struct type_id<CAF_PP_EXPAND type_expr> {                                    \
    static constexpr type_id_t value = tag_id;                                 \
  };                                                                           \
  template <>                                                                  \
  struct type_by_id<tag_id> {                                                  \
    using type = CAF_PP_EXPAND type_expr;                                      \
  };                                                                           \
  template <>                                                                  \
  struct type_name<CAF_PP_EXPAND type_expr> {                                  \
    static constexpr std::string_view value = display_name;                    \
  };                                                                           \
  template <>                                                                  \
  struct type_name_by_id<tag_id>                                               \
    : type_name<CAF_PP_EXPAND type_expr> {};                                   \
  }

// ------------------------------------------------------------------
// 展开唯一数据源 message_tags.def：定义全部标签结构体并完成类型注册
// ------------------------------------------------------------------
#define X_TAG(name, id) CAF_MESSAGE_TAG(name, id)
#define X_REG(key, type_expr, id, display_name) \
  CAF_REGISTER_TYPE(type_expr, id, display_name)
#include "message_tags.def"
#undef X_TAG
#undef X_REG
