#include "common/message_meta.hpp"
#include "templates/sql_entity_store_actor.hpp"

#include <caf/init_global_meta_objects.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace db = caf_plugin_system::db;
namespace entity = caf_plugin_system::entity_store;
namespace sql = caf_plugin_system::entity_store::sql;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

db::db_result good() {
    db::db_result result;
    result.ok = true;
    return result;
}

template <class Result, class Handle>
Result await(Handle pending) {
    Result result;
    pending.receive(
        [&](Result value) { result = std::move(value); },
        [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
    return result;
}

enum class MetadataMode { held, second_fails, connection_lost };

// Metadata replies are released by explicit test commands, not by timers.
// This makes partial startup, publication and failure boundaries observable.
class MetadataBackend final : public caf::event_based_actor {
public:
    MetadataBackend(caf::actor_config& config, MetadataMode mode)
        : caf::event_based_actor(config), mode_(mode) {}

    caf::behavior make_behavior() override {
        return {
            [this](sql_query_atom, const std::string& connection,
                   const std::string& text, const std::vector<std::string>& params)
                -> caf::result<db::db_result> {
                if (text == "__stats__")
                    return stats();
                if (text == "__wait_metadata__") {
                    const auto expected = static_cast<size_t>(std::stoull(params.at(0)));
                    if (metadata_queries_ >= expected)
                        return stats();
                    auto promise = make_response_promise<db::db_result>();
                    watchers_.push_back({expected, promise});
                    return promise;
                }
                if (text == "__release__") {
                    auto held = held_.find(params.at(0));
                    require(held != held_.end(), "test released unknown metadata request");
                    auto promise = held->second;
                    held_.erase(held);
                    promise.deliver(metadata(params.at(0)));
                    return good();
                }
                if (text.find("pragma_table_xinfo") != std::string::npos) {
                    ++metadata_queries_;
                    if (connection != "writer")
                        ++wrong_connections_;
                    notify_watchers();
                    if (mode_ == MetadataMode::connection_lost) {
                        db::db_result error;
                        error.code = db::error_code::connection_lost;
                        error.error = "injected metadata disconnect";
                        return error;
                    }
                    if (mode_ == MetadataMode::second_fails && params.at(0) == "payments") {
                        db::db_result error;
                        error.code = db::error_code::sql_error;
                        error.error = "injected metadata permission failure";
                        return error;
                    }
                    auto promise = make_response_promise<db::db_result>();
                    held_.emplace(params.at(0), promise);
                    return promise;
                }
                ++business_queries_;
                if (connection != "reader")
                    ++wrong_connections_;
                auto result = good();
                result.rows = {{"ready", "7"}};
                return result;
            },
        };
    }

    void on_exit() override {
        held_.clear();
        watchers_.clear();
    }

private:
    static db::db_result metadata(const std::string& table) {
        auto result = good();
        const bool order = table == "orders";
        result.columns = {"column_name", "data_type", "type_detail", "nullable",
                          "key_order", "generated", "hidden"};
        result.rows = {
            {order ? "order_id" : "payment_id", "TEXT", "TEXT", "0", "1", "0", "0"},
            {order ? "status" : "state", "TEXT", "TEXT", "0", "0", "0", "0"},
            {"secret", "TEXT", "TEXT", "1", "0", "0", "0"},
            {"version", "INTEGER", "INTEGER", "0", "0", "0", "0"},
        };
        return result;
    }

    db::db_result stats() const {
        auto result = good();
        result.rows = {{std::to_string(metadata_queries_),
                        std::to_string(business_queries_),
                        std::to_string(wrong_connections_)}};
        return result;
    }

    void notify_watchers() {
        for (auto it = watchers_.begin(); it != watchers_.end();) {
            if (metadata_queries_ >= it->first) {
                it->second.deliver(stats());
                it = watchers_.erase(it);
            } else {
                ++it;
            }
        }
    }

    MetadataMode mode_;
    size_t metadata_queries_ = 0;
    size_t business_queries_ = 0;
    size_t wrong_connections_ = 0;
    std::map<std::string, caf::typed_response_promise<db::db_result>> held_;
    std::vector<std::pair<size_t, caf::typed_response_promise<db::db_result>>> watchers_;
};

class ObservedStore final : public sql::entity_store_actor {
public:
    ObservedStore(caf::actor_config& config, sql::service_config settings,
                  std::shared_ptr<std::atomic<unsigned>> destructions)
        : sql::entity_store_actor(config, std::move(settings)),
          destructions_(std::move(destructions)) {}

    ~ObservedStore() override { ++*destructions_; }

private:
    std::shared_ptr<std::atomic<unsigned>> destructions_;
};

entity::load_request load_request(std::string field = "status") {
    entity::load_request result;
    result.target = {.store = "commerce", .partition = "one-order", .entity = "order",
                     .key = {{"order_id", entity::value::text("one-order")}}};
    result.fields.all_fields = false;
    result.fields.names = {std::move(field)};
    return result;
}

class Harness {
public:
    Harness(caf::actor_system& system, MetadataMode mode, unsigned index,
             std::chrono::milliseconds timeout = 1500ms,
             std::chrono::milliseconds backoff = 5ms, bool register_backend = true)
        : system_(system), name_("metadata_mock_" + std::to_string(index)) {
        backend = system_.spawn<MetadataBackend>(mode);
        if (register_backend)
            system_.registry().put(name_, backend);
        sql::entity_schema order;
        order.name = "order";
        order.table = "orders";
        order.discover_from_database = true;
        order.fields_explicit = true;
        order.fields = {{"status", "status"}};
        sql::entity_schema payment;
        payment.name = "payment";
        payment.table = "payments";
        payment.discover_from_database = true;
        sql::store_schema commerce;
        commerce.name = "commerce";
        commerce.read_service = commerce.write_service = name_;
        commerce.read_connection = "reader";
        commerce.write_connection = "writer";
        commerce.entities = {{"order", order}, {"payment", payment}};
        sql::service_config settings;
        settings.request_timeout = timeout;
        settings.retry_backoff = backoff;
        std::string error;
        require(settings.catalog.add_store(std::move(commerce), error), error);
        store = system_.spawn<ObservedStore>(std::move(settings), destructions);
    }

    ~Harness() {
        system_.registry().erase(name_);
        if (store)
            caf::anon_send_exit(store, caf::exit_reason::user_shutdown);
        caf::anon_send_exit(backend, caf::exit_reason::user_shutdown);
    }

    db::db_result control(std::string command, std::vector<std::string> params = {}) {
        caf::scoped_actor self{system_};
        return await<db::db_result>(self->request(
            backend, 2s, sql_query_atom_v, std::string{"control"},
            std::move(command), std::move(params)));
    }

    entity::load_result load(std::string field = "status") {
        caf::scoped_actor self{system_};
        return await<entity::load_result>(self->request(
            store, 2s, entity_load_atom_v, load_request(std::move(field))));
    }

    void wait_metadata(unsigned count) {
        control("__wait_metadata__", {std::to_string(count)});
    }

    void release(const std::string& table) { control("__release__", {table}); }

    caf::actor backend;
    caf::actor store;
    std::shared_ptr<std::atomic<unsigned>> destructions =
        std::make_shared<std::atomic<unsigned>>(0);

private:
    caf::actor_system& system_;
    std::string name_;
};

void verify_startup_gate_and_cache(caf::actor_system& system) {
    Harness test{system, MetadataMode::held, 1};
    test.wait_metadata(1);
    caf::scoped_actor self{system};
    auto pending = self->request(test.store, 50ms, entity_load_atom_v, load_request());
    // Same-sender lifecycle barrier proves the load handler has run, while its
    // response must still be deferred behind the deliberately held metadata.
    await<std::vector<std::byte>>(self->request(test.store, 1s, save_state_atom_v));
    bool timed_out = false;
    pending.receive(
        [](const entity::load_result&) { throw std::runtime_error("business ran before schema publication"); },
        [&](const caf::error& error) { timed_out = error == caf::sec::request_timeout; });
    require(timed_out, "held metadata did not defer the business response");
    require(test.control("__stats__").rows.at(0).at(1) == "0",
            "a business SQL escaped incomplete schema initialization");
    test.release("orders");
    test.wait_metadata(2);
    require(test.control("__stats__").rows.at(0).at(1) == "0",
            "first entity was exposed while the second schema was unresolved");
    test.release("payments");
    require(test.load().code == entity::result_code::ok, "discovered schema did not serve data");
    require(test.load().version == 7, "cached schema returned wrong version");
    const auto before = test.control("__stats__");
    require(before.rows.at(0).at(0) == "2", "metadata was queried again after startup");
    require(before.rows.at(0).at(2) == "0", "metadata did not use writer or loads did not use reader");
    require(test.load("secret").code == entity::result_code::invalid_request,
            "discovery exposed a field outside the configured allowlist");
    require(test.control("__stats__").rows.at(0).at(1) == before.rows.at(0).at(1),
            "forbidden field lookup reached the database");
}

void verify_failed_snapshot_is_not_exposed(caf::actor_system& system) {
    Harness test{system, MetadataMode::second_fails, 2};
    test.wait_metadata(1);
    caf::scoped_actor self{system};
    auto pending = self->request(test.store, 2s, entity_load_atom_v, load_request());
    await<std::vector<std::byte>>(self->request(test.store, 1s, save_state_atom_v));
    test.release("orders");
    const auto result = await<entity::load_result>(std::move(pending));
    require(result.code != entity::result_code::ok
                && result.error.find("permission failure") != std::string::npos,
            "metadata failure was not delivered to waiting business requests");
    require(test.load().code != entity::result_code::ok,
            "a partially initialized catalog was used after failure");
    const auto stats = test.control("__stats__");
    require(stats.rows.at(0).at(0) == "2" && stats.rows.at(0).at(1) == "0",
            "failed schema publication executed business SQL or restarted discovery");
}

void verify_initialization_deadlines(caf::actor_system& system) {
    for (bool missing : {false, true}) {
        Harness test{system, MetadataMode::connection_lost, missing ? 4u : 3u,
                     60ms, 5000ms, !missing};
        const auto started = std::chrono::steady_clock::now();
        const auto result = test.load();
        require(result.code != entity::result_code::ok
                    && result.error.find("deadline") != std::string::npos,
                "missing backend/retryable metadata failure ignored startup deadline");
        require(std::chrono::steady_clock::now() - started < 1s,
                "large retry backoff extended the initialization budget");
        require(test.control("__stats__").rows.at(0).at(1) == "0",
                "expired initialization allowed business SQL");
    }
}

void verify_force_exit_releases_pending_promises() {
    std::shared_ptr<std::atomic<unsigned>> destructions;
    {
        caf::actor_system_config config;
        caf::actor_system system{config};
        {
            Harness test{system, MetadataMode::held, 5};
            destructions = test.destructions;
            test.wait_metadata(1);
            {
                caf::scoped_actor self{system};
                self->monitor(test.store);
                auto pending = self->request(test.store, 1s, entity_load_atom_v, load_request());
                await<std::vector<std::byte>>(self->request(test.store, 1s, save_state_atom_v));
                caf::anon_send_exit(test.store, caf::exit_reason::user_shutdown);
                bool stopped = false;
                self->receive([&](const caf::down_msg&) { stopped = true; },
                              caf::after(1s) >> [] {});
                require(stopped, "forced exit did not terminate EntityStore");
                pending.receive(
                    [](const entity::load_result& result) {
                        require(result.code != entity::result_code::ok,
                                "forced exit claimed a pending request succeeded");
                    },
                    [](const caf::error&) {});
            }
            test.store = caf::actor{};
            test.release("orders");
        } // Unregister/stop the backend and release every test-owned handle.
        system.await_all_actors_done();
    }
    // CAF 1.1 request_response_timeout schedules a strong actor reference.
    // Forced quit clears response handlers without disposing that clock entry.
    // Destroying this isolated actor_system drains the timer-owned references;
    // unlike a sleep, this distinguishes a permanent promise/self cycle from
    // bounded runtime retention until the original metadata deadline.
    require(destructions->load() == 1,
            "forced exit retained EntityStore through a pending-promise self-cycle");
}

} // namespace

int main() {
    caf::core::init_global_meta_objects();
    app_meta::init();
    caf::actor_system_config config;
    caf::actor_system system{config};
    try {
        verify_startup_gate_and_cache(system);
        verify_failed_snapshot_is_not_exposed(system);
        verify_initialization_deadlines(system);
        verify_force_exit_releases_pending_promises();
        system.await_all_actors_done();
        std::cout << "entity store schema startup tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
