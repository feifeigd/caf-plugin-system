// ------------------------------------------------------------------
// typed_actor 冒烟验证 —— exe 侧
//
// 验证链：
//   1. DLL 工厂返回 untyped 句柄 → actor_cast 回 typed（跨 DLL 类型一致）
//   2. untyped 代理透明转发 → typed impl 签名匹配（模拟 spawn_service_proxy）
//   3. typed request 往返（直连 + 经代理）
//   4. 生命周期 request（save_state，模拟 PM 热更新/关机路径）
//   5. 签名外消息 → default handler 兜底（私有消息/信封场景）
// ------------------------------------------------------------------

#include "typed_smoke_common.hpp"
#include "common/message_meta.hpp"
#include <caf/all.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <iostream>

namespace {

// 模拟现有 spawn_service_proxy：untyped 透明转发一切消息到 target。
// typed 化后代理层不用动——它不知道也不关心签名，转发即可。
// 与 service_registry.cpp 的 spawn_service_proxy 同款（stateful + default
// handler + delegate），排除非 stateful 写法差异。
caf::actor spawn_untyped_proxy(caf::actor_system& sys, caf::actor target) {
    struct ProxyState {
        caf::actor current;
    };
    return sys.spawn(
        [target](caf::stateful_actor<ProxyState>* self) -> caf::behavior {
            self->state().current = target;
            self->set_default_handler(
                [self](caf::scheduled_actor*, caf::message& msg)
                    -> caf::skippable_result {
                    self->delegate(self->state().current, msg);
                    return caf::delegated<caf::message>();
                });
            // 注意：CAF 1.1 中纯空 behavior（caf::behavior{}）的 actor 会
            // 立即正常退出——必须至少有一个 handler（现有 spawn_service_proxy
            // 有 quiesce/resume/set_acl 三个，所以没踩到）。
            return caf::behavior{
                [=](quiesce_atom) -> bool { return true; },
            };
        });
}

} // namespace

int main() {
    caf::core::init_global_meta_objects();
    app_meta::init();

    caf::actor_system_config cfg;
    caf::actor_system sys{cfg};

    int failed = 0;
    auto check = [&](const char* name, bool ok) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
        if (!ok) ++failed;
    };

    // 1. 跨 DLL：工厂返回 untyped 句柄 → cast 回 typed
    auto raw = create_smoke_service(sys);
    auto svc = caf::actor_cast<typed_smoke::smoke_service>(raw);
    check("cross-DLL actor_cast to typed", static_cast<bool>(svc));

    // 2. untyped 代理（模拟 spawn_service_proxy）
    auto proxy = spawn_untyped_proxy(sys, raw);
    check("untyped proxy spawned", static_cast<bool>(proxy));

    caf::scoped_actor self{sys};

    // 3. typed request 往返（直连）
    self->request(svc, caf::infinite, std::string("hello"))
        .receive(
            [&](const std::string& r) {
                check(("typed echo direct: " + r).c_str(), r == "echo: hello");
            },
            [&](const caf::error& e) {
                check("typed echo direct", false);
                std::cerr << "  err: " << caf::to_string(e) << std::endl;
            });

    // 4. 经 untyped 代理转发（模拟 proxy delegate，热更新消息路径）
    self->request(proxy, caf::infinite, std::string("via-proxy"))
        .receive(
            [&](const std::string& r) {
                check(("typed echo via untyped proxy: " + r).c_str(),
                      r == "echo: via-proxy");
            },
            [&](const caf::error& e) {
                check("typed echo via untyped proxy", false);
                std::cerr << "  err: " << caf::to_string(e) << std::endl;
            });

    // 5. 生命周期 request（PM 的 save_state 场景——签名内，不能落空）
    self->request(svc, caf::infinite, save_state_atom{})
        .receive(
            [&](const std::vector<std::byte>&) {
                check("lifecycle save_state request", true);
            },
            [&](const caf::error& e) {
                check("lifecycle save_state request", false);
                std::cerr << "  err: " << caf::to_string(e) << std::endl;
            });

    // 6. 签名外消息 → default handler 兜底（插件私有消息/信封场景）
    //    注意：typed request 到 typed actor 是【编译期】检查，签名外消息
    //    直接编译不过（上面的 C2338）。私有协议只能经 untyped 代理发
    //    （代理不检查签名）或 anon_send——这是 typed 重构的硬约束。
    self->request(proxy, caf::infinite, health_check_atom{})
        .receive(
            [&](bool ok) { check("out-of-signature via untyped proxy + default handler", ok); },
            [&](const caf::error& e) {
                check("out-of-signature via untyped proxy + default handler", false);
                std::cerr << "  err: " << caf::to_string(e) << std::endl;
            });

    // 7. typed shutdown（send 语义）
    self->send(svc, shutdown_atom{});
    self->wait_for(raw); // raw 与 svc 同一 actor（untyped 句柄，wait_for 直接可用）

    if (failed == 0) {
        std::cout << "ALL TYPED_SMOKE TESTS PASSED" << std::endl;
        return 0;
    }
    std::cout << failed << " TYPED_SMOKE TEST(S) FAILED" << std::endl;
    return 1;
}
