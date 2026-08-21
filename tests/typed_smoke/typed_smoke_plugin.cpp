// ------------------------------------------------------------------
// typed_actor 冒烟验证 —— 模拟插件 DLL 侧
//
// 实现 typed 服务 actor（生命周期 + 业务 echo + default handler 兜底
// 签名外消息），导出工厂函数。exe 链接本 DLL 后 actor_cast 调用，
// 验证 typed 接口跨 DLL 编译单元一致。
// ------------------------------------------------------------------

#include "typed_smoke_common.hpp"
#include <iostream>

// extern "C" 导出函数返回 caf::actor（C++ 句柄类）——功能正确，仅 MSVC
// 的 C4190 警告（C 链接 + C++ 返回值类型），无害，显式压制。
#ifdef _MSC_VER
  #pragma warning(disable : 4190)
#endif

namespace typed_smoke {

// typed 服务实现。注意：返回类型必须是 smoke_service::behavior_type，
// handler 参数/返回值与签名精确匹配（编译期强制，这正是 typed 的收益）。
smoke_service::behavior_type smoke_impl(smoke_service::pointer self) {
    // 签名外消息（如 health_check_atom，模拟插件私有消息/信封场景）
    // 走 default handler 兜底——typed 纯度损失点，用默认 handler 覆盖。
    self->set_default_handler(
        [](caf::scheduled_actor*, caf::message& msg) -> caf::skippable_result {
            if (msg.match_element<health_check_atom>(0)) {
                std::cout << "[plugin] default-handler: health_check "
                             "(out-of-signature)" << std::endl;
                return caf::make_message(true);
            }
            return caf::make_error(caf::sec::invalid_request);
        });

    return {
        [=](init_atom, caf::actor, const std::string&) {
            std::cout << "[plugin] init (typed)" << std::endl;
        },
        [=](drain_atom, caf::actor coordinator) {
            std::cout << "[plugin] drain (typed)" << std::endl;
            // typed actor 不能 self->send 到 untyped actor（静态检查禁止），
            // 与 untyped 通信必须 anon_send / request——改造摩擦点之一。
            caf::anon_send(coordinator, drain_atom{}, self->address());
        },
        [=](save_state_atom) -> std::vector<std::byte> {
            std::cout << "[plugin] save_state (typed)" << std::endl;
            return {};
        },
        [=](restore_state_atom, const std::vector<std::byte>&) {
            std::cout << "[plugin] restore_state (typed)" << std::endl;
        },
        [=](shutdown_atom) {
            std::cout << "[plugin] shutdown (typed)" << std::endl;
            self->quit();
        },
        [=](const std::string& cmd) -> std::string {
            std::cout << "[plugin] echo (typed): " << cmd << std::endl;
            return "echo: " + cmd;
        }};
}

} // namespace typed_smoke

// 跨 DLL 工厂：typed 句柄以 untyped caf::actor 形式跨 DLL 传递，
// exe 端 actor_cast 回 typed_smoke::smoke_service（类型一致性验证点）。
// CAF 1.1 的 spawn 从工厂函数签名推导句柄类型（infer_handle_from_fun_t），
// 返回 smoke_service（typed_actor）；转 untyped 必须显式 actor_cast。
extern "C" TYPED_SMOKE_API caf::actor create_smoke_service(caf::actor_system& sys) {
    auto typed = sys.spawn(typed_smoke::smoke_impl);
    return caf::actor_cast<caf::actor>(typed);
}
