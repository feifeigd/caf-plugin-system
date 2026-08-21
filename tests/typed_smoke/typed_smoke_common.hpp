#pragma once
// ------------------------------------------------------------------
// typed_actor 跨 DLL 冒烟验证 —— 共享契约头
//
// 模拟"服务契约头"（对应 include/services/*.hpp）：typed_actor 签名
// = 生命周期（内核协议）+ 业务接口。标签复用内核 message_tags.def 的
// 既有 type_id（跨 DLL 一致，无需新注册元对象）。
//
// 验证目标（对应 typed 重构的关键风险）：
//   1. CAF 1.1 typed_actor 定义/spawn/request 全链路可行
//   2. typed 句柄跨 DLL 传递 + actor_cast 类型一致
//   3. untyped 代理透明转发 → typed impl 签名匹配
//   4. 生命周期消息（save_state 等）进签名，PM 的 request 不落空
//   5. 签名外消息（私有协议/信封场景）走 set_default_handler 兜底
// ------------------------------------------------------------------

#include "common/message_tags.hpp"
#include <caf/all.hpp>
#include <cstddef>
#include <string>
#include <vector>

// extern "C" 导出函数返回 caf::actor（C++ 句柄类）——功能正确，仅 MSVC
// 的 C4190 警告（C 链接 + C++ 返回值类型），无害，显式压制。
#ifdef _MSC_VER
  #pragma warning(disable : 4190)
#endif

#ifdef _WIN32
  #define TYPED_SMOKE_API __declspec(dllexport)
#else
  #define TYPED_SMOKE_API __attribute__((visibility("default")))
#endif

namespace typed_smoke {

using smoke_service = caf::typed_actor<
    // 生命周期（对应 PluginEntry 契约，PM/GS 会 request 这些）
    caf::result<void>(init_atom, caf::actor, std::string),
    caf::result<void>(drain_atom, caf::actor),
    caf::result<std::vector<std::byte>>(save_state_atom),
    caf::result<void>(restore_state_atom, std::vector<std::byte>),
    caf::result<void>(shutdown_atom),
    // 业务接口（模拟 business 的 string 命令）
    caf::result<std::string>(std::string)>;

} // namespace typed_smoke

// 跨 DLL 工厂（模拟插件 DLL 的 create_plugin 导出）：返回 untyped 句柄，
// 调用方 actor_cast 回 typed 接口使用。TYPED_SMOKE_API 在 DLL 侧展开为
// dllexport，在 exe 侧声明为 dllimport。
extern "C" TYPED_SMOKE_API caf::actor create_smoke_service(caf::actor_system& sys);
