// hello_plugin.cpp — caf-plugin-sdk 最小插件示例
//
// 演示：
//   1. PluginEntry 子类 + extern "C" 导出（create/destroy/register_meta）
//   2. plugin_lifecycle 挂生命周期骨架（drain 回执 / shutdown quit 由框架统一）
//   3. LOG_INFO 宏日志（插件名经 PLUGIN_NAME 编译定义 + set_log_source 注入）
//   4. 公共信封 plugin_envelope 收/发跨插件消息（不占 type_id 号段）
//
// 部署：把 hello_plugin.dll 放到主程序 run/<Config>/plugins/hello/ 下，
// 并在 caf-application.conf 的 entry-plugins 中加入 "HelloPlugin"。

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/plugin_envelope.hpp"

#include <caf/all.hpp>

#include <cstring>
#include <string>
#include <vector>

// 插件私有消息标签示例：插件私有段从 1000 起自选互不重叠的号段
//（约定见 plugin_interface.hpp 头内注释；内核/框架占 200~999）。
// 注意：必须定义在全局作用域（CAF_MESSAGE_TAG 会展开出 namespace caf，
// 放进匿名命名空间会造成 caf 二义性）。
CAF_MESSAGE_TAG(hello_ping_atom, 1000)

// 可选导出：自注册私有类型的元对象。框架在构造任何 actor_system 之前
// 调用（CAF 规定之后注册是 UB）。没有私有消息类型的插件可不实现。
extern "C" PLUGIN_API void register_meta_objects() {
    static const caf::detail::meta_object xs[] = {
        caf::detail::make_meta_object<hello_ping_atom>("hello_ping_atom"),
    };
    caf::detail::set_global_meta_objects(1000, caf::make_span(xs));
}

namespace {

constexpr const char* k_hello_fn_ping = "ping";

class HelloPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {
            .name = "HelloPlugin",
            .version = "1.0.0",
            // dependencies：启动时由 ServiceRegistry 注入对应服务的 actor，
            // 顺序与这里一致。空 = 不依赖其他插件服务。
            // 注意：日志句柄是 core 动态库单一实体（exe 启动时已注入），
            // 插件无需再声明/注入 logging_service。
            .dependencies = {},
            // provides：本插件提供的服务名，其他插件经 manifest.dependencies
            // 引用后拿到的是这些服务的代理 actor。
            .provides = {"HelloService"},
            .priority = 0,
            // acl_allow：非空时，本插件的服务只接受清单内来源的调用。
            // 空 = 开放策略。核心服务名可入清单（如 {"logging_service"}）。
            .acl_allow = {},
        };
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& injected_deps,
                     const std::string& config) override {
        // 日志来源名：每模块独立副本，只影响本插件 DLL 的日志行
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        return sys.spawn([config](caf::event_based_actor* self) {

            auto count = std::make_shared<int>(0);

            // 纯业务 handler 列表（caf::message_handler，or_else 需要它）
            caf::message_handler business{
                // 私有消息示例
                [=](hello_ping_atom) {
                    ++*count;
                    LOG_INFO("hello_ping received, count={}", *count);
                },
                // 公共信封示例：跨插件调用不占号段，载荷自己编码
                [=](const plugin_envelope& env) {
                    if (env.function == k_hello_fn_ping) {
                        LOG_INFO("envelope ping, payload_bytes={}", env.payload.size());
                        if (self->current_message_id().is_request()) {
                            auto rp = self->make_response_promise<std::string>();
                            rp.deliver(std::string("pong"));
                        }
                    }
                },
            };

            // 生命周期骨架：or_else 组合，框架统一 drain 回执 / shutdown quit
            return caf::behavior{business.or_else(plugin_lifecycle(
                self, PluginLifecycleHooks{
                          .on_init =
                              [config](caf::actor, const std::string&) {
                                  LOG_INFO("HelloPlugin initialized, config='{}'",
                                           config);
                              },
                          .on_save =
                              [count]() -> std::vector<std::byte> {
                                  std::vector<std::byte> data(sizeof(int));
                                  std::memcpy(data.data(), count.get(), sizeof(int));
                                  return data;
                              },
                          .on_restore =
                              [count](const std::vector<std::byte>& data) {
                                  if (data.size() >= sizeof(int))
                                      std::memcpy(count.get(), data.data(), sizeof(int));
                              },
                          // on_drain / on_shutdown 不注册 = 无动作，框架兜底
                      }))};
        });
    }
};

} // namespace

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new HelloPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
