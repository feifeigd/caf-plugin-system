#include "common/message_meta.hpp"
#include "plugin/dynamic_library.hpp"
#include "plugin/plugin_interface.hpp"
#include <caf/all.hpp>
#include <caf/init_global_meta_objects.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

using namespace std::chrono_literals;
namespace db = caf_plugin_system::db;

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class Client final {
public:
    Client(caf::actor_system& system, PluginEntry& plugin)
        : self_(system), service_(plugin.spawn(system, {}, "")) {
        self_->send(service_, init_atom_v, caf::actor{}, std::string{});
    }
    ~Client() {
        if (service_) caf::anon_send_exit(service_, caf::exit_reason::user_shutdown);
    }
    db::db_result command(std::string command, std::vector<std::string> args = {},
                          std::string connection = "main") {
        auto response = self_->request(service_, 5s, redis_cmd_atom_v,
                                      std::move(connection), std::move(command), std::move(args));
        return receive(response);
    }
    void exercise() {
        require(command("PING").ok, "Redis PING failed");
        const std::string binary{"a\0b", 3};
        require(command("SET", {"pool:binary", binary}).ok, "binary SET failed");
        auto got = command("GET", {"pool:binary"});
        require(got.ok && got.rows.size() == 1 && got.rows[0][0] == binary, "binary GET mismatch");
        require(!command("PING", {}, "missing").ok, "unknown connection accepted");
        auto first = self_->request(service_, 5s, redis_cmd_atom_v, std::string{"main"},
                                   std::string{"SET"}, std::vector<std::string>{"pool:ordered", "first"});
        auto second = self_->request(service_, 5s, redis_cmd_atom_v, std::string{"main"},
                                    std::string{"SET"}, std::vector<std::string>{"pool:ordered", "second"});
        require(receive(first).ok && receive(second).ok, "queued SET failed");
        got = command("GET", {"pool:ordered"});
        require(got.ok && got.rows[0][0] == "second", "Redis FIFO order changed");
    }
    void stop(bool forced) {
        self_->monitor(service_);
        // Include pending worker work; the mailbox barrier proves it was enqueued.
        auto pending = self_->request(service_, 5s, redis_cmd_atom_v, std::string{"main"},
                                     std::string{"BLPOP"},
                                     std::vector<std::string>{"pool:empty-list", "1"});
        bool barrier = false;
        self_->request(service_, 5s, save_state_atom_v).receive(
            [&](const std::vector<std::byte>&) { barrier = true; }, [](const caf::error&) {});
        require(barrier, "Redis mailbox barrier failed");
        if (forced) self_->send_exit(service_, caf::exit_reason::user_shutdown);
        else self_->send(service_, shutdown_atom_v);
        bool down = false;
        self_->receive([&](const caf::down_msg& message) {
            down = message.source == service_.address();
        }, caf::after(6s) >> [] {});
        require(down, "Redis plugin did not exit");
        require(receive(pending).ok, "Redis drain lost accepted work");
        service_ = {};
    }

private:
    template <class Response>
    static db::db_result receive(Response& response) {
        db::db_result result;
        response.receive([&](const db::db_result& value) { result = value; },
                         [&](const caf::error& error) { result.error = caf::to_string(error); });
        return result;
    }
    caf::scoped_actor self_;
    caf::actor service_;
};
} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    try {
        require(argc == 2, "expected Redis plugin DLL path");
        const auto* uri = std::getenv("REDIS_TEST_URI");
        require(uri && std::string{uri}.starts_with("redis://127.0.0.1:"),
                "REDIS_TEST_URI must name the disposable loopback Redis instance");
        auto library = DynamicLibrary::open(argv[1]);
        require(library.has_value(), "Redis plugin load failed");
        auto create = library->symbol<PluginEntry*(*)()>("create_plugin");
        auto destroy = library->symbol<void(*)(PluginEntry*)>("destroy_plugin");
        require(create && destroy, "Redis plugin exports missing");
        std::unique_ptr<PluginEntry, decltype(destroy)> plugin{create(), destroy};
        caf::core::init_global_meta_objects();
        app_meta::init();
        caf::actor_system_config config;
        caf::settings uris{{"main", caf::config_value{std::string{uri}}}};
        caf::put(config.content, "caf-plugin-system.redis.uris", uris);
        {
            caf::actor_system system{config};
            for (bool forced : {false, true}) {
                Client client{system, *plugin};
                client.exercise();
                client.stop(forced);
            }
        }
        std::puts("Redis worker pool E2E passed; normal and forced exits joined workers");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Redis worker pool E2E failed: %s\n", error.what());
        return 1;
    }
}
