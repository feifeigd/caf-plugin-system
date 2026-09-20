#include "common/message_meta.hpp"
#include "entity_crud_scenarios.hpp"
#include "plugin/dynamic_library.hpp"
#include "plugin/plugin_interface.hpp"
#include <caf/init_global_meta_objects.hpp>
#include <caf/actor_registry.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>
#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace entity = caf_plugin_system::entity_store;
using namespace std::chrono_literals;
using entity::value;
using entity::patch_op;

namespace {
struct DebugHeapCheck {
    DebugHeapCheck() {
#if defined(_WIN32) && defined(_DEBUG)
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    }
} heap_check;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class Plugin final {
public:
    explicit Plugin(const char* path) : library_(DynamicLibrary::open(path)) {
        require(library_.has_value(), "cannot load test plugin");
        auto create = library_->symbol<PluginEntry*(*)()>("create_plugin");
        destroy_ = library_->symbol<void(*)(PluginEntry*)>("destroy_plugin");
        require(create && destroy_, "plugin factory symbols missing");
        entry_ = create();
        require(entry_ != nullptr, "plugin factory returned null");
    }
    ~Plugin() { if (entry_) destroy_(entry_); }
    caf::actor spawn(caf::actor_system& system) { return entry_->spawn(system, {}, ""); }
private:
    std::optional<DynamicLibrary> library_;
    PluginEntry* entry_ = nullptr;
    void (*destroy_)(PluginEntry*) = nullptr;
};

caf::settings field(const std::string& name, const std::string& kind, bool nullable = true) {
    caf::settings result;
    result.emplace("column", name);
    result.emplace("kind", kind);
    result.emplace("nullable", nullable);
    return result;
}

caf::settings entity_config(const std::string& table) {
    caf::settings keys, fields, result;
    keys.emplace("id", field("business_id", "text", false));
    fields.emplace("status", field("status", "text", false));
    fields.emplace("amount", field("amount", "signed_integer", false));
    fields.emplace("balance", field("balance", "unsigned_integer"));
    fields.emplace("price", field("price", "decimal"));
    fields.emplace("payload", field("payload", "json"));
    fields.emplace("binary", field("binary", "bytes"));
    fields.emplace("note", field("note", "text"));
    fields.emplace("enabled", field("enabled", "boolean"));
    fields.emplace("ratio", field("ratio", "real"));
    auto locked = field("locked", "text"); locked.emplace("writable", false);
    fields.emplace("locked", locked);
    result.emplace("table", table);
    result.emplace("keys", keys);
    result.emplace("fields", fields);
    return result;
}

void configure(caf::actor_system_config& config, const std::string& uri,
               const std::string& reader_uri) {
    caf::settings uris, entities, store, stores, settings;
    uris.emplace("reader", uri);
    uris.emplace("writer", uri);
    uris.emplace("other_reader", reader_uri);
    caf::put(config.content, "caf-plugin-system.redis.uris", uris);
    caf::put(config.content, "caf-plugin-system.redis.pool_size", 2);
    entities.emplace("order", entity_config("orders"));
    entities.emplace("payment", entity_config("payments"));
    store.emplace("entities", entities);
    store.emplace("read_connection", "reader");
    store.emplace("write_connection", "writer");
    stores.emplace("commerce", store);
    store["read_connection"] = "other_reader";
    stores.emplace("routed", store);
    store["write_connection"] = "other_reader";
    stores.emplace("replica_seed", store);
    settings.emplace("dialect", "redis");
    settings.emplace("stores", stores);
    settings.emplace("request_timeout_ms", 6000);
    settings.emplace("retry_backoff_ms", 20);
    settings.emplace("save_retry_attempts", 3);
    caf::put(config.content, "caf-plugin-system.entity_store", settings);
}

entity::entity_ref target(std::string id, std::string name = "order") {
    entity::entity_ref result;
    result.store = "commerce";
    result.partition = "order-1";
    result.entity = std::move(name);
    result.key = {{"id", value::text(std::move(id))}};
    return result;
}

entity::save_request save_request(std::string id, entity::entity_ref key,
                                 std::vector<entity::field_patch> fields,
                                 bool create = false) {
    entity::entity_patch patch;
    patch.target = std::move(key);
    patch.fields = std::move(fields);
    patch.create_if_missing = create;
    return {std::move(id), {std::move(patch)}};
}

template<class Result, class Handle>
Result receive(Handle& request) {
    Result result;
    request.receive([&](Result value) { result = std::move(value); },
        [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
    return result;
}

class Harness final {
public:
    Harness(caf::actor_system& system, Plugin& redis, Plugin& frontend)
        : system_(system), self_(system), backend_(redis.spawn(system)), frontend_(frontend.spawn(system)) {
        system_.registry().put("redis_service", backend_);
        self_->send(backend_, init_atom_v, caf::actor{}, std::string{});
        self_->send(frontend_, init_atom_v, caf::actor{}, std::string{});
        auto barrier = self_->request(backend_, 3s, save_state_atom_v);
        receive<std::vector<std::byte>>(barrier);
    }
    ~Harness() {
        system_.registry().erase("redis_service");
        if (frontend_) caf::anon_send_exit(frontend_, caf::exit_reason::user_shutdown);
        if (backend_) caf::anon_send_exit(backend_, caf::exit_reason::user_shutdown);
    }
    entity::save_result save(const entity::save_request& input) {
        auto pending = self_->request(frontend_, 12s, entity_save_atom_v, input);
        return receive<entity::save_result>(pending);
    }
    entity::load_result load(entity::entity_ref key, std::vector<std::string> fields = {}) {
        entity::load_request input;
        input.target = std::move(key);
        if (!fields.empty()) { input.fields.all_fields = false; input.fields.names = std::move(fields); }
        auto pending = self_->request(frontend_, 12s, entity_load_atom_v, input);
        return receive<entity::load_result>(pending);
    }
    void ordering() {
        auto a = save_request("ordered-a", target("one"), {{patch_op::set, "status", value::text("first")}});
        auto b = save_request("ordered-b", target("one"), {{patch_op::set, "status", value::text("second")}});
        auto first = self_->request(frontend_, 12s, entity_save_atom_v, a);
        auto second = self_->request(frontend_, 12s, entity_save_atom_v, b);
        require(receive<entity::save_result>(first).committed, "first ordered save failed");
        require(receive<entity::save_result>(second).committed, "second ordered save failed");
    }
    caf_plugin_system::db::db_result raw(const std::string& command,
                                               std::vector<std::string> arguments = {}) {
        auto pending = self_->request(backend_, 5s, redis_cmd_atom_v, std::string{"writer"}, command, std::move(arguments));
        return receive<caf_plugin_system::db::db_result>(pending);
    }
    void independent_sessions() {
        require(raw("SELECT", {"1"}).ok && raw("MULTI").ok, "raw session setup failed");
        auto result = save(save_request("raw-isolation", target("one"),
            {{patch_op::increment, "amount", value::signed_integer(1)}}));
        require(result.committed, "raw MULTI/SELECT contaminated EntityStore");
        require(raw("DISCARD").ok && raw("SELECT", {"0"}).ok, "raw session cleanup failed");
        require(raw("PING").ok, "raw command regression");
        require(raw("CLIENT", {"KILL", "TYPE", "normal", "SKIPME", "yes"}).ok, "disconnect injection failed");
        require(load(target("one")).code == entity::result_code::ok, "load did not reconnect after disconnect");
        require(save(save_request("after-disconnect", target("one"),
            {{patch_op::increment, "amount", value::signed_integer(1)}})).committed, "save did not reconnect");
    }
    void concurrency(Plugin& driver) {
        auto second = driver.spawn(system_);
        self_->send(second, init_atom_v, caf::actor{}, std::string{});
        auto barrier = self_->request(second, 3s, save_state_atom_v);
        receive<std::vector<std::byte>>(barrier);
        try {
            require(save(save_request("race-create", target("race"), {
                {patch_op::set, "status", value::text("new")},
                {patch_op::set, "amount", value::signed_integer(0)}}, true)).committed, "race create failed");
            for (int i = 0; i < 20; ++i) {
                auto a = save_request("race-a-" + std::to_string(i), target("race"),
                    {{patch_op::increment, "amount", value::signed_integer(1)}});
                auto b = save_request("race-b-" + std::to_string(i), target("race"),
                    {{patch_op::increment, "amount", value::signed_integer(1)}});
                auto first = self_->request(backend_, 10s, entity_save_atom_v, std::string{"writer"}, a, uint64_t{6000});
                auto next = self_->request(second, 10s, entity_save_atom_v, std::string{"writer"}, b, uint64_t{6000});
                auto ar = receive<entity::save_result>(first);
                auto br = receive<entity::save_result>(next);
                require(ar.committed && br.committed, "concurrent atomic increment failed: " + ar.error + " / " + br.error);
            }
            stop(second);
        } catch (...) { caf::anon_send_exit(second, caf::exit_reason::user_shutdown); throw; }
    }
    void shutdown(bool forced = false) {
        self_->send(frontend_, drain_atom_v, caf::actor_cast<caf::actor>(self_));
        bool drained = false;
        self_->receive([&](drain_atom, const caf::actor_addr& address) {
            drained = address == frontend_.address();
        }, caf::after(8s) >> [] {});
        require(drained, "frontend drain timed out");
        auto started = std::chrono::steady_clock::now();
        stop(frontend_, forced);
        stop(backend_, forced);
        require(std::chrono::steady_clock::now() - started < 6s, "Redis worker join exceeded shutdown bound");
        system_.registry().erase("redis_service");
        std::puts("Redis workers joined; graceful shutdown confirmed");
    }
private:
    void stop(caf::actor& actor, bool forced = false) {
        self_->monitor(actor);
        if (forced) self_->send_exit(actor, caf::exit_reason::user_shutdown);
        else self_->send(actor, shutdown_atom_v);
        bool down = false;
        self_->receive([&](const caf::down_msg& message) { down = message.source == actor.address(); },
                      caf::after(8s) >> [] {});
        require(down, "plugin failed to exit");
        actor = caf::actor{};
    }
    caf::actor_system& system_;
    caf::scoped_actor self_;
    caf::actor backend_, frontend_;
};


const value& field_value(const entity::load_result& result, const std::string& name) {
    require(result.code == entity::result_code::ok, "load failed: " + result.error);
    for (const auto& field : result.fields) if (field.name == name) return field.data;
    throw std::runtime_error("missing returned field: " + name);
}

entity::save_request initial() {
    return save_request("create-one", target("one"), {
        {patch_op::set, "status", value::text("new")},
        {patch_op::set, "amount", value::signed_integer(100)},
        {patch_op::set, "balance", value::unsigned_integer(std::numeric_limits<uint64_t>::max())},
        {patch_op::set, "price", value::decimal("12.50")},
        {patch_op::set, "payload", value::json(R"({"items":[1,2],"nested":{"name":"stored"}})")},
        {patch_op::set, "binary", value::bytes({std::byte{0}, std::byte{255}})},
        {patch_op::set, "enabled", value::boolean(true)},
        {patch_op::set, "ratio", value::real(0.25)},
        {patch_op::set, "note", value::text(std::string{"a\0b", 3})}}, true);
}

void verify(Harness& test, Plugin& driver) {
    entity::test::strict_crud(initial(),
        [&](const auto& request) { return test.save(request); },
        [&](const auto& key) { return test.load(key); },
        [](bool ok, const char* message) { require(ok, message); return ok; });
    auto created = test.save(initial());
    require(created.committed && created.entities.front().version == 1, "create failed: " + created.error);
    auto all = test.load(target("one"));
    require(all.fields.size() == 10 && all.version == 1, "all fields/version mismatch");
    require(field_value(all, "balance").unsigned_value == std::numeric_limits<uint64_t>::max(), "uint64 precision lost");
    require(field_value(all, "binary").bytes_value == std::vector<std::byte>({std::byte{0}, std::byte{255}})
        && field_value(all, "note").text_value == std::string{"a\0b", 3}, "binary-safe encoding failed");
    require(field_value(all, "enabled").boolean_value && field_value(all, "ratio").real_value == 0.25, "bool/real mismatch");
    require(field_value(all, "payload").text_value.find("\"nested\"") != std::string::npos, "nested JSON lost");
    auto projected = test.load(target("one"), {"status", "id"});
    require(projected.fields.size() == 2 && field_value(projected, "id").text_value == "one", "projection failed");

    auto update = save_request("update-one", target("one"), {
        {patch_op::increment, "amount", value::signed_integer(5)},
        {patch_op::increment, "balance", value::signed_integer(-1)},
        {patch_op::increment, "price", value::decimal("0.01")},
        {patch_op::set, "note", value::null()}});
    update.changes.front().check_version = true; update.changes.front().expected_version = 1;
    auto changed = test.save(update);
    require(changed.committed && changed.entities.front().version == 2, "patch failed: " + changed.error);
    auto replay = test.save(update);
    require(replay.committed && replay.entities.front().version == 2, "replay applied twice");
    auto duplicate = update; duplicate.changes.front().target.partition = "another-partition";
    require(test.save(duplicate).code == entity::result_code::conflict, "request_id was partition-scoped");
    duplicate = update; duplicate.changes.front().target.store = "routed";
    require(test.save(duplicate).code == entity::result_code::conflict, "shared ledger id was store-scoped");
    duplicate = update; duplicate.request_id = "stale-version";
    require(test.save(duplicate).code == entity::result_code::conflict, "stale version accepted");
    all = test.load(target("one"));
    require(field_value(all, "amount").signed_value == 105
        && field_value(all, "balance").unsigned_value == std::numeric_limits<uint64_t>::max() - 1
        && field_value(all, "price").text_value == "12.51"
        && field_value(all, "status").text_value == "new"
        && field_value(all, "note").kind == entity::value_kind::null_value, "partial update corrupted fields");
    require(test.save(save_request("erase", target("one"), {{patch_op::erase, "note", {}}})).committed, "erase failed");
    require(field_value(test.load(target("one")), "note").kind == entity::value_kind::null_value, "erased field not absent/null");

    auto batch = save_request("rollback", target("one"), {{patch_op::increment, "amount", value::signed_integer(999)}});
    auto missing = save_request("unused", target("missing", "payment"), {{patch_op::set, "status", value::text("paid")}});
    batch.changes.push_back(missing.changes.front());
    require(!test.save(batch).committed && field_value(test.load(target("one")), "amount").signed_value == 105,
            "failed batch partially committed");
    batch.request_id = "batch";
    batch.changes.front().fields.front().data = value::signed_integer(1);
    batch.changes.back() = save_request("unused", target("payment", "payment"), {
        {patch_op::set, "status", value::text("paid")}, {patch_op::set, "amount", value::signed_integer(5)}}, true).changes.front();
    require(test.save(batch).committed && test.load(target("payment", "payment")).code == entity::result_code::ok, "atomic batch failed");
    auto repeated_entity = save_request("same-entity-batch", target("one"), {{patch_op::increment, "amount", value::signed_integer(1)}});
    repeated_entity.changes.push_back(repeated_entity.changes.front());
    auto repeated = test.save(repeated_entity);
    require(repeated.committed && repeated.entities[1].version == repeated.entities[0].version + 1,
            "same entity batch order broken");
    require(test.save(repeated_entity).entities[1].version == repeated.entities[1].version, "same entity batch replay failed");

    for (const auto& name : {"unlisted", "locked"}) {
        require(test.save(save_request(std::string{"bad-"}+name, target("one"),
            {{patch_op::set, name, value::text("bad")}})).code == entity::result_code::invalid_request, "field whitelist bypassed");
    }
    require(test.load(target("one"), {"unlisted"}).code == entity::result_code::invalid_request, "projection whitelist bypassed");
    require(test.save(save_request("bad-null", target("one"), {{patch_op::set, "status", value::null()}})).code
        == entity::result_code::invalid_request, "required null accepted");
    require(test.save(save_request("bad-erase", target("one"), {{patch_op::erase, "status", {}}})).code
        == entity::result_code::invalid_request, "required erase accepted");
    require(test.save(save_request("bad-type", target("one"), {{patch_op::set, "amount", value::text("1")}})).code
        == entity::result_code::invalid_request, "wrong type accepted");
    require(test.save(save_request("bad-json", target("one"), {{patch_op::set, "payload", value::json("1")}})).code
        == entity::result_code::invalid_request, "scalar JSON accepted");
    require(test.save(save_request("bad-real", target("one"), {{patch_op::set, "ratio", value::real(std::numeric_limits<double>::infinity())}})).code
        == entity::result_code::invalid_request, "infinite real accepted");
    require(test.save(save_request("overflow", target("one"), {{patch_op::increment, "balance", value::signed_integer(2)}})).code
        == entity::result_code::invalid_request, "uint64 overflow accepted");
    require(field_value(test.load(target("one")), "balance").unsigned_value == std::numeric_limits<uint64_t>::max() - 1,
        "overflow partially applied");
    require(test.save(save_request("signed-limit", target("limits"), {
        {patch_op::set, "status", value::text("limit")}, {patch_op::set, "amount", value::signed_integer(std::numeric_limits<int64_t>::max())},
        {patch_op::set, "price", value::decimal("9007199254740993.000000000000000001")}}, true)).committed, "large values create failed");
    auto overflow_batch = save_request("overflow-batch", target("one"), {{patch_op::increment, "amount", value::signed_integer(1000)}});
    overflow_batch.changes.push_back(save_request("unused", target("limits"),
        {{patch_op::increment, "amount", value::signed_integer(1)}}).changes.front());
    const auto before_overflow = field_value(test.load(target("one")), "amount").signed_value;
    require(!test.save(overflow_batch).committed && field_value(test.load(target("one")), "amount").signed_value == before_overflow,
        "late overflow leaked first change");
    require(test.save(save_request("exact-decimal", target("limits"), {
        {patch_op::increment, "price", value::decimal("0.000000000000000001")}})).committed, "decimal addition failed");
    require(field_value(test.load(target("limits")), "price").text_value == "9007199254740993.000000000000000002", "decimal precision lost");
    auto numeric = test.save(save_request("numeric-create", target("numeric"), {
        {patch_op::set, "status", value::text("new")}, {patch_op::increment, "amount", value::unsigned_integer(2)},
        {patch_op::increment, "balance", value::signed_integer(5)}, {patch_op::increment, "price", value::signed_integer(3)}}, true));
    require(numeric.committed && numeric.entities.front().version == 1, "numeric create failed");

    auto seed = target("read-route"); seed.store = "replica_seed";
    require(test.save(save_request("seed-replica", seed, {{patch_op::set, "status", value::text("replica")},
        {patch_op::set, "amount", value::signed_integer(1)}}, true)).committed, "reader seed failed");
    auto routed = target("read-route"); routed.store = "routed";
    require(field_value(test.load(routed), "status").text_value == "replica", "read connection routing failed");
    require(test.save(save_request("route-writer", routed, {{patch_op::set, "status", value::text("writer")},
        {patch_op::set, "amount", value::signed_integer(2)}}, true)).committed, "write connection routing failed");
    require(field_value(test.load(routed), "status").text_value == "replica"
        && field_value(test.load(target("read-route")), "status").text_value == "writer", "read/write connections mixed");

    test.ordering();
    require(field_value(test.load(target("one")), "status").text_value == "second", "FIFO order changed");
    test.concurrency(driver);
    require(field_value(test.load(target("race")), "amount").signed_value == 40, "concurrent increment lost updates");
    test.independent_sessions();
    test.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: test_redis_entity_store <redis plugin> <entity store plugin>");
        const char* input = std::getenv("REDIS_TEST_URI");
        require(input && std::string{input}.starts_with("redis://127.0.0.1:"), "disposable REDIS_TEST_URI is required");
        std::string uri{input};
        auto reader = uri.substr(0, uri.rfind('/')) + "/1";
        caf::core::init_global_meta_objects();
        app_meta::init();
        Plugin driver{argv[1]}, frontend{argv[2]};
        {
            caf::actor_system_config config; configure(config, uri, reader);
            caf::actor_system system{config};
            { Harness test{system, driver, frontend}; verify(test, driver); }
            // Persistent ledger must survive backend/front-end actor recreation.
            {
                Harness restarted{system, driver, frontend};
                auto replay = restarted.save(initial());
                require(replay.committed && replay.entities.front().version == 1, "restarted plugin lost idempotency ledger");
                require(field_value(restarted.load(target("one")), "status").text_value == "second", "restart replay overwrote newer state");
                restarted.shutdown(true);
            }
            system.await_all_actors_done();
        }
        std::puts("Redis EntityStore E2E passed; workers joined and persistent replay confirmed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Redis EntityStore E2E failed: %s\n", error.what());
        return 1;
    }
}
