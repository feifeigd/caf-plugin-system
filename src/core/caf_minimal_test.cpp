// CAF 最小关机测试 v2（2026-08-31）：最小程序（EXIT=0 已验证干净）+
// 项目自定义消息类型注册（app_meta::init）+ 自定义类型消息收发。
// 判定：崩溃是否由自定义类型元对象注册/析构路径触发。
#include <caf/all.hpp>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "common/message_meta.hpp"
#include "common/message_tags.hpp"

int main(int argc, char** argv) {
    int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    caf::core::init_global_meta_objects();
    app_meta::init();
    caf::actor_system_config cfg;
    if (mode >= 4) {
        // 复制项目 framework_config 的 CAF 配置
        cfg.set("caf.logger.console.verbosity", "quiet");
        cfg.set("caf.logger.file.verbosity", "info");
        cfg.set("caf.logger.file.path", "logs/caf-framework.log");
        cfg.set("caf.logger.console.colored", true);
    }
    caf::actor_system sys(cfg);
    if (mode >= 6) {
        // 复刻 stop_next 模式：request(worker, resolve).then → 发 shutdown
        // → 等 down_msg → quit。request 的 response promise 生命周期
        // 与 actor quit 的交互是 shutdown_mgr 的核心模式。
        auto worker = sys.spawn([](caf::event_based_actor* self) -> caf::behavior {
            return {
                [=](resolve_plugin_atom, const std::string&) -> std::string {
                    return "ok";
                },
                [=](shutdown_atom) { self->quit(); },
            };
        });
        auto coord = sys.spawn([worker](caf::event_based_actor* self) -> caf::behavior {
            return {
                [=](shutdown_atom) {
                    self->request(worker, std::chrono::seconds(5),
                                  resolve_plugin_atom_v, std::string("x"))
                        .then(
                            [=](const std::string&) {
                                self->monitor(worker);
                                self->send(worker, shutdown_atom_v);
                            },
                            [=](const caf::error&) { self->quit(); });
                },
                [=](const caf::down_msg& dm) {
                    if (dm.source == worker->address())
                        self->quit();
                },
            };
        });
        caf::anon_send(coord, shutdown_atom_v);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (mode >= 5) {
        // 复刻 shutdown_mgr 模式：协调者收到 shutdown → 给 worker
        // send_exit + monitor → 等 down_msg → 自己 quit
        auto worker = sys.spawn([](caf::event_based_actor* self) -> caf::behavior {
            return {[](int) {}};
        });
        auto coord = sys.spawn([worker](caf::event_based_actor* self) -> caf::behavior {
            return {
                [=](shutdown_atom) {
                    self->monitor(worker);
                    self->send_exit(worker, caf::exit_reason::user_shutdown);
                },
                [=](const caf::down_msg& dm) {
                    if (dm.source == worker->address())
                        self->quit();
                },
            };
        });
        caf::anon_send(coord, shutdown_atom_v);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    if (mode >= 1) {
        auto a = sys.spawn([](caf::event_based_actor* self) -> caf::behavior {
            return {
                [=](shutdown_atom) { self->quit(); },
                [=](plugin_saved_atom, const std::string&, bool) {
                    self->quit();
                },
            };
        });
        if (mode == 1)
            caf::anon_send(a, shutdown_atom_v);
        else
            caf::anon_send(a, plugin_saved_atom_v, std::string("x"), true);
        if (mode <= 2)
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    std::cout << "[minimal2] mode=" << mode << " done, destroying actor_system"
              << std::endl;
    return 0;
}
