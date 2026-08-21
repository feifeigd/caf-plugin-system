#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "graceful_shutdown.hpp"
#include "checkpoint_manager.hpp"
#include "plugin/plugin_manager.hpp"
#include "services/logging_service.hpp"
#include "common/plugin_envelope.hpp"
#include <caf/detail/make_meta_object.hpp>
#include <caf/detail/meta_object.hpp>
#include <caf/span.hpp>
#include <cstddef>
#include <iostream>
#include <cstring>
#include <memory>

// ------------------------------------------------------------------
// 插件私有消息类型示例：内核/框架协议标签见 common/message_tags.def，
// 私有类型从 1000 起自选互不重叠的 ID 段（约定见 plugin_interface.hpp）。
// 私有类型不收进内核的 message_tags.def，内核和其他插件都不用重编。
// ------------------------------------------------------------------
CAF_MESSAGE_TAG(biz_ping_atom, 1000)

// 可选导出：自注册私有类型的元对象。框架在构造任何 actor_system 之前
// 调用（CAF 规定之后注册是 UB）。少了这一步，携带 biz_ping_atom 的
// 消息析构时会通过空元对象指针崩溃（exec at 0x0）。
extern "C" PLUGIN_API void register_meta_objects() {
    static const caf::detail::meta_object xs[] = {
        caf::detail::make_meta_object<biz_ping_atom>("biz_ping_atom"),
    };
    caf::detail::set_global_meta_objects(1000, caf::make_span(xs));
}

// 信封子协议号：插件自行编号即可，别的插件用同样的号也不冲突——
// 信封的命名空间是"插件 × 子协议号"（见 common/plugin_envelope.hpp 利弊说明）
constexpr std::uint16_t biz_env_hello = 1;
// 子协议号 2 由热更新的 v2 新增：走信封加协议不需要新 type_id/元对象注册
constexpr std::uint16_t biz_env_v2_ping = 2;

// BIZ_HOT_V2 由 CMake 的 business_plugin_v2 目标定义：同一份源码编出
// 热更新演示用的 v2（旁路加载到新路径，行为差异即"新代码已生效"的证据）
#ifdef BIZ_HOT_V2
  #define BIZ_VERSION_STR "2.1.0-hot"
#else
  #define BIZ_VERSION_STR "2.0.0"
#endif

/// 一个 DLL 插件示例，提供一个简单的业务服务，并依赖于 logging_service
class BusinessPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        // acl_allow 演示：business_service 进入受限策略，只接受 LoggerPlugin
        // 的调用，其余 sender（包括外部/入口 actor）在代理处被 ACL 拦截
        return {"BusinessPlugin", BIZ_VERSION_STR, {"logging_service"}, {"business_service"},
                0, {"LoggerPlugin"}};
    }

    // 返回值代表整个 DLL 的 actor 实例，deps 是依赖的 actor 列表，最后一个参数是插件的配置字符串
    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = deps.empty() ? caf::actor{} : deps[0];

        return sys.spawn([logger](caf::stateful_actor<int>* self) -> caf::behavior {
            self->state() = 0;
            auto plugin_mgr = std::make_shared<caf::actor>();

            // 私有业务 handler 在前（高频消息一次命中），公共生命周期
            // behavior 兜底（plugin_lifecycle：drain 回执 / shutdown quit
            // 框架统一，状态序列化走 hooks 回调）。
            caf::message_handler business{
                [=](biz_ping_atom) {
                    LOG_INFO(logger, "Private meta round-trip OK");
                },
                // 信封 handler：失去类型匹配，统一入口 + 手工 switch 二级分发，
                // 这正是 plugin_envelope.hpp 头注释里"弊"的具体样子
                [=](const plugin_envelope& env) {
                    switch (env.sub_proto) {
                        case biz_env_hello: {
                            std::string text(
                                reinterpret_cast<const char*>(env.payload.data()),
                                env.payload.size());
                            LOG_INFO(logger, "Envelope round-trip OK: {}", text);
                            // 响应式：request 调用方（如跨节点 RemoteCaller）
                            // 拿回执；纯 send 调用无副作用（void handler）
                            if (self->current_message_id().is_request()) {
                                auto rp = self->make_response_promise<std::string>();
                                rp.deliver("cross-ok:" + text);
                            }
                            break;
                        }
#ifdef BIZ_HOT_V2
                        case biz_env_v2_ping:
                            // 热更新新增的子协议号：走信封不需要新 type_id/元对象注册
                            LOG_INFO(logger, "v2 envelope pong (hot-added sub_proto=2)");
                            break;
#endif
                        default:
                            LOG_INFO(logger, "Unknown envelope sub_proto={}",
                                     static_cast<int>(env.sub_proto));
                            break;
                    }
                },
                [=](const std::string& cmd) -> std::string {
                    self->state()++;
                    if (cmd == "shutdown") {
                        if (*plugin_mgr) {
                            self->send(*plugin_mgr, request_shutdown_atom{});
                            LOG_INFO(logger, "Shutdown requested by command");
                        }
                        return "shutdown requested";
                    }
                    LOG_DEBUG(logger, "Command received: {}", cmd);
#ifdef BIZ_HOT_V2
                    // v2 行为差异：热更新生效的运行时证据
                    return "processed by v2: " + cmd;
#else
                    return "processed: " + cmd;
#endif
                },
            };
            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                // 初始化：记录 manager + 私有消息自测（回调可捕获 self）
                .on_init = [logger, plugin_mgr, self](caf::actor manager,
                                                      const std::string&) {
                    *plugin_mgr = manager;
                    LOG_INFO(logger, "BusinessPlugin initialized ({})", BIZ_VERSION_STR);
                    // 方式一·私有类型：验证元对象自注册生效（创建→投递→析构
                    // 全链路，若 register_meta_objects 没被调用，这条消息会崩进程）
                    self->send(self, biz_ping_atom{});
                    // 方式二·公共信封：不占号段、不用自注册，但载荷要自己编码
                    {
                        const char* text = "hello-envelope";
                        plugin_envelope env;
                        env.sub_proto = biz_env_hello;
                        env.payload.assign(
                            reinterpret_cast<const std::byte*>(text),
                            reinterpret_cast<const std::byte*>(text) + std::strlen(text));
                        self->send(self, env);
                    }
                },
                // drain：无排空动作，回执由框架统一（不注册 on_drain）
                .on_save = [self]() -> std::vector<std::byte> {
                    std::vector<std::byte> data(sizeof(int));
                    std::memcpy(data.data(), &self->state(), sizeof(int));
                    return data;
                },
                .on_restore = [self, logger](const std::vector<std::byte>& data) {
                    if (data.size() >= sizeof(int)) {
                        std::memcpy(&self->state(), data.data(), sizeof(int));
                        LOG_INFO(logger, "Restored count={}", self->state());
                    }
                },
                // shutdown：无清理动作，quit 由框架统一
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new BusinessPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
