#include "common/message_meta.hpp"
#include "templates/mongo_entity_store_actor.hpp"

#include <caf/init_global_meta_objects.hpp>

#include <chrono>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace entity = caf_plugin_system::entity_store;
namespace mongo = entity::mongo;
namespace schema = entity::schema;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

template<class Result, class Handle>
Result receive_result(Handle& handle) {
    Result result;
    handle.receive(
        [&](Result value) { result = std::move(value); },
        [](const caf::error& error) {
            throw std::runtime_error(caf::to_string(error));
        });
    return result;
}

// Responses are released explicitly, making in-flight requests deterministic
// without a database, timing-dependent query, or production test hook.
class MockBackend final : public caf::event_based_actor {
public:
    explicit MockBackend(caf::actor_config& config) : caf::event_based_actor(config) {}

    caf::behavior make_behavior() override {
        return {
            [this](entity_load_atom, const std::string& connection,
                   entity::load_request input, uint64_t budget)
                -> caf::result<entity::load_result> {
                const auto id = input.target.key.front().data.text_value;
                events_.push_back("load:" + id + ":" + connection
                                  + (budget == 0 ? ":zero-budget" : ""));
                auto promise = make_response_promise<entity::load_result>();
                loads_.emplace(id, PendingLoad{std::move(input), promise});
                return promise;
            },
            [this](entity_save_atom, const std::string& connection,
                   entity::save_request input, uint64_t budget)
                -> caf::result<entity::save_result> {
                const auto id = input.request_id;
                events_.push_back("save:" + id + ":" + connection
                                  + (budget == 0 ? ":zero-budget" : ""));
                auto promise = make_response_promise<entity::save_result>();
                saves_.emplace(id, PendingSave{std::move(input), promise});
                return promise;
            },
            [this](const std::string&) { return events_; },
            [this](const std::string&, const std::string& id) { return release(id); },
        };
    }

    void on_exit() override {
        for (auto& [id, pending] : loads_) {
            entity::load_result result;
            result.target = pending.input.target;
            result.code = entity::result_code::unavailable;
            pending.promise.deliver(std::move(result));
        }
        for (auto& [id, pending] : saves_) {
            entity::save_result result;
            result.request_id = id;
            result.code = entity::result_code::commit_unknown;
            pending.promise.deliver(std::move(result));
        }
        loads_.clear();
        saves_.clear();
    }

private:
    struct PendingLoad {
        entity::load_request input;
        caf::typed_response_promise<entity::load_result> promise;
    };
    struct PendingSave {
        entity::save_request input;
        caf::typed_response_promise<entity::save_result> promise;
    };

    bool release(const std::string& id) {
        if (auto found = saves_.find(id); found != saves_.end()) {
            auto pending = std::move(found->second);
            saves_.erase(found);
            entity::save_result result;
            result.request_id = id;
            result.code = entity::result_code::ok;
            result.committed = true;
            result.entities.push_back({pending.input.changes.front().target, 7});
            pending.promise.deliver(std::move(result));
            return true;
        }
        if (auto found = loads_.find(id); found != loads_.end()) {
            auto pending = std::move(found->second);
            loads_.erase(found);
            entity::load_result result;
            result.target = pending.input.target;
            result.code = entity::result_code::ok;
            result.version = 7;
            result.fields = {{"status", entity::value::text("paid")}};
            pending.promise.deliver(std::move(result));
            return true;
        }
        return false;
    }

    std::vector<std::string> events_;
    std::map<std::string, PendingLoad> loads_;
    std::map<std::string, PendingSave> saves_;
};

class Harness {
public:
    Harness(caf::actor_system& system, int sequence,
            size_t limit = 16, std::chrono::milliseconds timeout = 2s)
        : system_(system), read_name_("mock_mongo_read_" + std::to_string(sequence)),
          write_name_("mock_mongo_write_" + std::to_string(sequence)) {
        reader = system.spawn<MockBackend>();
        writer = system.spawn<MockBackend>();
        system.registry().put(read_name_, reader);
        system.registry().put(write_name_, writer);
        schema::entity_schema order;
        order.name = "order";
        order.table = "orders";
        order.keys = {{"id", "order_id", entity::value_kind::text, false, false}};
        order.fields = {{"status", "state", entity::value_kind::text, true, false}};
        schema::store_schema store;
        store.name = "commerce";
        store.read_service = read_name_;
        store.write_service = write_name_;
        store.read_connection = "reader";
        store.write_connection = "writer";
        store.entities.emplace(order.name, std::move(order));
        mongo::service_config config;
        config.max_pending_requests = limit;
        config.request_timeout = timeout;
        std::string error;
        require(config.catalog.add_store(std::move(store), error), error);
        frontend = system.spawn<mongo::entity_store_actor>(std::move(config));
    }

    ~Harness() {
        system_.registry().erase(read_name_);
        system_.registry().erase(write_name_);
        if (frontend)
            caf::anon_send_exit(frontend, caf::exit_reason::user_shutdown);
        caf::anon_send_exit(reader, caf::exit_reason::user_shutdown);
        caf::anon_send_exit(writer, caf::exit_reason::user_shutdown);
    }

    caf::actor frontend;
    caf::actor reader;
    caf::actor writer;

private:
    caf::actor_system& system_;
    std::string read_name_;
    std::string write_name_;
};

entity::entity_ref target(std::string id, std::string partition = "same") {
    entity::entity_ref result;
    result.store = "commerce";
    result.partition = std::move(partition);
    result.entity = "order";
    result.key = {{"id", entity::value::text(std::move(id))}};
    return result;
}

entity::save_request save(std::string id, std::string partition = "same") {
    entity::save_request result;
    result.request_id = id;
    entity::entity_patch patch;
    patch.target = target(std::move(id), std::move(partition));
    patch.fields = {{entity::patch_op::set, "status", entity::value::text("paid")}};
    result.changes.push_back(std::move(patch));
    return result;
}

entity::load_request load(std::string id) {
    entity::load_request result;
    result.target = target(std::move(id));
    return result;
}

void barrier(caf::scoped_actor& self, const caf::actor& frontend) {
    auto request = self->request(frontend, 2s, save_state_atom_v);
    receive_result<std::vector<std::byte>>(request);
}

std::vector<std::string> snapshot(caf::scoped_actor& self, const caf::actor& backend) {
    auto request = self->request(backend, 2s, std::string{"snapshot"});
    return receive_result<std::vector<std::string>>(request);
}

void release(caf::scoped_actor& self, const caf::actor& backend, const std::string& id) {
    auto request = self->request(backend, 2s, std::string{"release"}, id);
    require(receive_result<bool>(request), "missing pending backend request: " + id);
}

bool receive_drain(caf::scoped_actor& coordinator, const caf::actor_addr& frontend,
                   std::chrono::milliseconds timeout) {
    bool received = false;
    coordinator->receive(
        [&](drain_atom, const caf::actor_addr& address) {
            require(address == frontend, "drain acknowledged another actor");
            received = true;
        },
        caf::after(timeout) >> [] {});
    return received;
}

void verify_queue_parallelism_and_routing(caf::actor_system& system) {
    Harness test{system, 1};
    caf::scoped_actor self{system};
    auto first = self->request(test.frontend, 3s, entity_save_atom_v, save("first"));
    auto second = self->request(test.frontend, 3s, entity_save_atom_v, save("second"));
    auto parallel = self->request(test.frontend, 3s, entity_save_atom_v, save("parallel", "other"));
    auto read = self->request(test.frontend, 3s, entity_load_atom_v, load("read"));
    barrier(self, test.frontend);
    require(snapshot(self, test.writer)
                == std::vector<std::string>{"save:first:writer", "save:parallel:writer"},
            "same partition was not serialized, or independent partition was blocked");
    require(snapshot(self, test.reader) == std::vector<std::string>{"load:read:reader"},
            "load did not use its read service/connection");
    release(self, test.writer, "parallel");
    require(receive_result<entity::save_result>(parallel).committed,
            "independent partition did not complete while first was blocked");
    release(self, test.reader, "read");
    require(receive_result<entity::load_result>(read).version == 7, "load reply lost");
    release(self, test.writer, "first");
    require(receive_result<entity::save_result>(first).committed, "first save failed");
    barrier(self, test.frontend);
    require(snapshot(self, test.writer)
                == std::vector<std::string>{"save:first:writer", "save:parallel:writer",
                                             "save:second:writer"},
            "next same-partition save was not dispatched after completion");
    release(self, test.writer, "second");
    require(receive_result<entity::save_result>(second).committed, "queued save failed");
}

void verify_pending_limit(caf::actor_system& system) {
    Harness test{system, 2, 1};
    caf::scoped_actor self{system};
    auto held = self->request(test.frontend, 3s, entity_load_atom_v, load("held"));
    barrier(self, test.frontend);
    auto denied = self->request(test.frontend, 2s, entity_save_atom_v, save("denied"));
    const auto result = receive_result<entity::save_result>(denied);
    require(result.code == entity::result_code::unavailable && !result.committed,
            "pending request limit did not reject an additional save");
    require(snapshot(self, test.writer).empty(), "rejected request reached backend");
    release(self, test.reader, "held");
    require(receive_result<entity::load_result>(held).code == entity::result_code::ok,
            "admitted request was lost under overload");
}

void verify_drain_waits_for_accepted_requests(caf::actor_system& system) {
    Harness test{system, 3};
    caf::scoped_actor self{system};
    caf::scoped_actor coordinator{system};
    auto first = self->request(test.frontend, 3s, entity_save_atom_v, save("first"));
    auto queued = self->request(test.frontend, 3s, entity_save_atom_v, save("queued"));
    auto read = self->request(test.frontend, 3s, entity_load_atom_v, load("read"));
    self->send(test.frontend, drain_atom_v, caf::actor_cast<caf::actor>(coordinator));
    barrier(self, test.frontend);
    require(!receive_drain(coordinator, test.frontend.address(), 20ms),
            "drain acknowledged while requests were pending");
    auto denied = self->request(test.frontend, 2s, entity_load_atom_v, load("denied"));
    require(receive_result<entity::load_result>(denied).code
                == entity::result_code::unavailable,
            "draining frontend admitted a new load");
    release(self, test.reader, "read");
    require(receive_result<entity::load_result>(read).code == entity::result_code::ok,
            "drain discarded an accepted load");
    release(self, test.writer, "first");
    require(receive_result<entity::save_result>(first).committed,
            "drain discarded an active save");
    barrier(self, test.frontend);
    require(!receive_drain(coordinator, test.frontend.address(), 20ms),
            "drain ignored its accepted queued save");
    release(self, test.writer, "queued");
    require(receive_result<entity::save_result>(queued).committed,
            "drain discarded a queued save");
    require(receive_drain(coordinator, test.frontend.address(), 2s),
            "drain failed to acknowledge after all accepted requests completed");
}

void verify_timeout_classification(caf::actor_system& system) {
    Harness test{system, 4, 16, 100ms};
    caf::scoped_actor self{system};
    auto write = self->request(test.frontend, 2s, entity_save_atom_v, save("timeout"));
    auto read = self->request(test.frontend, 2s, entity_load_atom_v, load("timeout"));
    const auto write_result = receive_result<entity::save_result>(write);
    const auto read_result = receive_result<entity::load_result>(read);
    require(write_result.code == entity::result_code::commit_unknown
                && !write_result.committed && write_result.request_id == "timeout",
            "lost save reply was not classified as commit_unknown");
    require(read_result.code == entity::result_code::unavailable,
            "lost load reply was not classified as unavailable");
    require(snapshot(self, test.writer) == std::vector<std::string>{"save:timeout:writer"},
            "frontend replayed a possibly committed save");
    release(self, test.writer, "timeout");
    release(self, test.reader, "timeout");
}

void verify_forced_exit_releases_promises() {
    caf::actor_addr weak_frontend;
    {
        caf::actor_system_config config;
        caf::actor_system system{config};
        {
            Harness test{system, 5};
            caf::scoped_actor self{system};
            auto first = self->request(test.frontend, 3s, entity_save_atom_v, save("active"));
            auto queued = self->request(test.frontend, 3s, entity_save_atom_v, save("queued"));
            auto read = self->request(test.frontend, 3s, entity_load_atom_v, load("active"));
            barrier(self, test.frontend);
            require(snapshot(self, test.writer) == std::vector<std::string>{"save:active:writer"},
                    "forced-exit setup did not leave one queued save");
            weak_frontend = test.frontend.address();
            self->monitor(test.frontend);
            caf::anon_send_exit(test.frontend, caf::exit_reason::user_shutdown);
            const auto active_result = receive_result<entity::save_result>(first);
            const auto queued_result = receive_result<entity::save_result>(queued);
            const auto read_result = receive_result<entity::load_result>(read);
            require(active_result.code == entity::result_code::commit_unknown,
                    "forced exit claimed certainty about an in-flight save");
            require(queued_result.code == entity::result_code::unavailable,
                    "forced exit did not fail an unsent queued save");
            require(read_result.code == entity::result_code::unavailable,
                    "forced exit did not fail an in-flight load");
            bool down = false;
            self->receive(
                [&](const caf::down_msg& message) { down = message.source == weak_frontend; },
                caf::after(2s) >> [] {});
            require(down, "forced-exit frontend did not terminate");
            test.frontend = caf::actor{};
            // Native response promises legitimately retain their source until settled.
            // Release those external anchors before checking for a frontend self-cycle.
            release(self, test.writer, "active");
            release(self, test.reader, "active");
        }
        system.await_all_actors_done();
    }
    // CAF 1.1 request_response_timeout schedules a strong actor reference.
    // Forced quit clears response handlers without disposing their timers.
    // Destroying this isolated system also destroys its clock, eliminating
    // those legitimate external anchors before testing for a true self-cycle.
    require(!caf::actor_cast<caf::actor>(weak_frontend),
            "terminated frontend is retained by an operation/promise self-cycle");
}

} // namespace

int main() {
    try {
        caf::core::init_global_meta_objects();
        app_meta::init();
        {
            caf::actor_system_config config;
            caf::actor_system system{config};
            verify_queue_parallelism_and_routing(system);
            verify_pending_limit(system);
            verify_drain_waits_for_accepted_requests(system);
            verify_timeout_classification(system);
            system.await_all_actors_done();
        }
        verify_forced_exit_releases_promises();
        std::puts("Mongo EntityStore actor tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mongo EntityStore actor test failed: %s\n", error.what());
        return 1;
    }
}
