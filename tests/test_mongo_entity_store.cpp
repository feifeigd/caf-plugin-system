#include "common/message_meta.hpp"
#include "entity_crud_scenarios.hpp"
#include "plugin/dynamic_library.hpp"
#include "plugin/plugin_interface.hpp"
#include <caf/init_global_meta_objects.hpp>
#include <caf/actor_registry.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/json.hpp>
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
    caf::put(config.content, "caf-plugin-system.mongo.uris", uris);
    caf::put(config.content, "caf-plugin-system.mongo.pool_size", 2);
    entities.emplace("order", entity_config("orders"));
    entities.emplace("payment", entity_config("payments"));
    store.emplace("entities", entities);
    store.emplace("read_connection", "reader");
    store.emplace("write_connection", "writer");
    stores.emplace("commerce", store);
    store["read_connection"] = "other_reader";
    stores.emplace("routed", store);
    settings.emplace("dialect", "mongodb");
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
    Harness(caf::actor_system& system, Plugin& mongo, Plugin& frontend)
        : system_(system), self_(system), backend_(mongo.spawn(system)), frontend_(frontend.spawn(system)) {
        system_.registry().put("mongo_service", backend_);
        self_->send(backend_, init_atom_v, caf::actor{}, std::string{});
        self_->send(frontend_, init_atom_v, caf::actor{}, std::string{});
        auto barrier = self_->request(backend_, 3s, save_state_atom_v);
        receive<std::vector<std::byte>>(barrier);
    }
    ~Harness() {
        system_.registry().erase("mongo_service");
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
    void raw_api() {
        auto pending = self_->request(backend_, 5s, mongo_op_atom_v, std::string{"writer"},
            std::string{"orders"}, std::string{"count"}, std::string{"{}"});
        auto result = receive<caf_plugin_system::db::db_result>(pending);
        require(result.ok && result.affected >= 1, "raw mongo_op regression");
    }
    void shutdown() {
        self_->send(frontend_, drain_atom_v, caf::actor_cast<caf::actor>(self_));
        bool drained = false;
        self_->receive([&](drain_atom, const caf::actor_addr& address) {
            drained = address == frontend_.address();
        }, caf::after(8s) >> [] {});
        require(drained, "frontend drain timed out");
        auto started = std::chrono::steady_clock::now();
        stop(frontend_);
        stop(backend_);
        require(std::chrono::steady_clock::now() - started < 6s, "Mongo worker join exceeded shutdown bound");
        system_.registry().erase("mongo_service");
        std::puts("Mongo workers joined; graceful shutdown confirmed");
    }
private:
    void stop(caf::actor& actor) {
        self_->monitor(actor);
        self_->send(actor, shutdown_atom_v);
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

class FailPoint final {
public:
    FailPoint(mongocxx::client& client, const std::string& data) : client_(client) {
        client_["admin"].run_command(bsoncxx::from_json(
            "{\"configureFailPoint\":\"failCommand\",\"mode\":{\"times\":1},\"data\":" + data + "}"));
    }
    ~FailPoint() {
        try { client_["admin"].run_command(bsoncxx::from_json(
            R"({"configureFailPoint":"failCommand","mode":"off"})")); } catch (...) {}
    }
private:
    mongocxx::client& client_;
};

const value& field_value(const entity::load_result& result, const std::string& name) {
    require(result.code == entity::result_code::ok, "load failed: " + result.error);
    for (const auto& field : result.fields) if (field.name == name) return field.data;
    throw std::runtime_error("missing returned field: " + name);
}

void verify(Harness& test, mongocxx::client& observer, const std::string& database) {
    auto orders = observer[database]["orders"];
    auto create = save_request("create-one", target("one"), {
        {patch_op::set, "status", value::text("new")},
        {patch_op::set, "amount", value::signed_integer(100)},
        {patch_op::set, "balance", value::unsigned_integer(std::numeric_limits<uint64_t>::max())},
        {patch_op::set, "price", value::decimal("12.50")},
        {patch_op::set, "payload", value::json(R"({"items":[1,2],"label":"stored"})")},
        {patch_op::set, "binary", value::bytes({std::byte{0}, std::byte{255}})},
        {patch_op::set, "note", value::text("keep")}}, true);
    entity::test::strict_crud(create,
        [&](const auto& request) { return test.save(request); },
        [&](const auto& key) { return test.load(key); },
        [](bool ok, const char* message) { require(ok, message); return ok; });
    auto result = test.save(create);
    require(result.committed && result.entities.front().version == 1, "create failed: " + result.error);
    auto all = test.load(target("one"));
    require(all.fields.size() == 7 && all.version == 1, "default fields/version differ from contract");
    auto projection = test.load(target("one"), {"status", "id"});
    require(projection.fields.size() == 2 && field_value(projection, "id").text_value == "one", "projection failed");
    auto raw = orders.find_one(bsoncxx::from_json(R"({"business_id":"one"})"));
    require(raw.has_value(), "independent observer cannot see committed entity");
    auto doc = raw->view();
    require(doc["balance"].type() == bsoncxx::type::k_decimal128
        && doc["price"].type() == bsoncxx::type::k_decimal128
        && doc["payload"].type() == bsoncxx::type::k_document
        && doc["binary"].type() == bsoncxx::type::k_binary, "BSON native types were not preserved");
    auto update = save_request("update-one", target("one"), {
        {patch_op::increment, "amount", value::signed_integer(5)},
        {patch_op::increment, "balance", value::signed_integer(-1)},
        {patch_op::increment, "price", value::signed_integer(1)},
        {patch_op::set, "note", value::null()}});
    update.changes.front().check_version = true;
    update.changes.front().expected_version = 1;
    result = test.save(update);
    require(result.committed, "partial update failed: " + result.error);
    auto replay = test.save(update);
    require(replay.committed && replay.entities.front().version == result.entities.front().version,
            "idempotent replay changed version");
    auto duplicate = update;
    duplicate.changes.front().target.partition = "another-partition";
    require(test.save(duplicate).code == entity::result_code::conflict, "request_id reused across partitions");
    auto stale = update;
    stale.request_id = "stale-version";
    require(test.save(stale).code == entity::result_code::conflict, "optimistic conflict ignored");
    all = test.load(target("one"));
    require(field_value(all, "amount").signed_value == 105
        && field_value(all, "balance").unsigned_value == std::numeric_limits<uint64_t>::max() - 1
        && field_value(all, "status").text_value == "new"
        && field_value(all, "note").kind == entity::value_kind::null_value, "partial update/replay corrupted data");
    require(test.save(save_request("erase-note", target("one"), {{patch_op::erase, "note", {}}})).committed, "erase failed");
    raw = orders.find_one(bsoncxx::from_json(R"({"business_id":"one"})"));
    require(!raw->view()["note"], "erase did not remove BSON property");
    auto rollback = save_request("rollback", target("one"), {{patch_op::increment, "amount", value::signed_integer(999)}});
    auto missing = save_request("unused", target("missing", "payment"), {{patch_op::set, "status", value::text("paid")}});
    rollback.changes.push_back(missing.changes.front());
    require(!test.save(rollback).committed && field_value(test.load(target("one")), "amount").signed_value == 105,
            "multi-collection failure did not rollback first write");
    auto payment = save_request("unused", target("pay-one", "payment"), {
        {patch_op::set, "status", value::text("paid")}, {patch_op::set, "amount", value::signed_integer(5)}}, true);
    rollback.request_id = "two-collections";
    rollback.changes.front().fields.front().data = value::signed_integer(1);
    rollback.changes.back() = payment.changes.front();
    require(test.save(rollback).committed && test.load(target("pay-one", "payment")).code == entity::result_code::ok,
            "multi-collection commit failed");
    test.ordering();
    require(field_value(test.load(target("one")), "status").text_value == "second", "same-partition ordering failed");
    {
        FailPoint fail{observer, R"({"failCommands":["findAndModify"],"errorCode":112,"errorLabels":["TransientTransactionError"]})"};
        auto retry = test.save(save_request("retry-transaction", target("one"), {{patch_op::increment, "amount", value::signed_integer(1)}}));
        require(retry.committed, "transient transaction retry failed: " + retry.error);
    }
    {
        FailPoint fail{observer, R"({"failCommands":["commitTransaction"],"writeConcernError":{"code":64,"errmsg":"test lost commit acknowledgement"},"errorLabels":["UnknownTransactionCommitResult"]})"};
        auto retry = test.save(save_request("retry-commit", target("one"), {{patch_op::increment, "amount", value::signed_integer(1)}}));
        require(retry.committed, "same-commit confirmation retry failed: " + retry.error);
    }
    require(field_value(test.load(target("one")), "amount").signed_value == 108, "transaction retry double-applied increment");
    auto overflow = test.save(save_request("overflow", target("one"),
        {{patch_op::increment, "balance", value::signed_integer(2)}}));
    require(!overflow.committed && overflow.code == entity::result_code::invalid_request
        && field_value(test.load(target("one")), "balance").unsigned_value == std::numeric_limits<uint64_t>::max() - 1,
        "unsigned overflow was not rolled back");
    auto numeric_create = test.save(save_request("numeric-create", target("numeric"), {
        {patch_op::set, "status", value::text("new")},
        {patch_op::increment, "amount", value::unsigned_integer(2)},
        {patch_op::increment, "balance", value::signed_integer(5)},
        {patch_op::increment, "price", value::signed_integer(3)}}, true));
    require(numeric_create.committed && numeric_create.entities.front().version == 1
        && field_value(test.load(target("numeric")), "balance").unsigned_value == 5,
        "mixed numeric increment failed on entity creation");
    require(test.save(save_request("invalid-field", target("one"),
        {{patch_op::set, "unlisted", value::text("blocked")}})).code == entity::result_code::invalid_request,
        "unknown field bypassed whitelist");
    orders.insert_one(bsoncxx::from_json(R"({"business_id":["array-key"],"version":{"$numberLong":"1"},"status":"bad","amount":{"$numberLong":"1"}})"));
    require(test.load(target("array-key")).code == entity::result_code::not_found, "array element matched scalar entity key");
    observer[database + "_read"]["orders"].insert_one(bsoncxx::from_json(
        R"({"business_id":"read-route","version":{"$numberLong":"1"},"status":"replica","amount":{"$numberLong":"1"}})"));
    auto routed = target("read-route");
    routed.store = "routed";
    require(field_value(test.load(routed), "status").text_value == "replica", "load did not use read connection");
    auto routed_write = save_request("write-route", routed, {
        {patch_op::set, "status", value::text("writer")}, {patch_op::set, "amount", value::signed_integer(2)}}, true);
    require(test.save(routed_write).committed, "write route failed");
    require(field_value(test.load(routed), "status").text_value == "replica", "save accidentally used reader connection");
    require(orders.count_documents(bsoncxx::from_json(R"({"business_id":"read-route","status":"writer"})")) == 1, "writer document missing");
    test.raw_api();
    test.shutdown();
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 3, "usage: test_mongo_entity_store <mongo plugin> <entity store plugin>");
        const auto* input = std::getenv("MONGO_TEST_URI");
        require(input && *input, "MONGO_TEST_URI is required");
        std::string uri{input};
        mongocxx::instance driver;
        mongocxx::uri parsed{uri};
        std::string database{parsed.database()};
        require(database == "caf_entity_test", "test must use disposable caf_entity_test database");
        auto reader_uri = uri;
        auto offset = reader_uri.find("/" + database);
        require(offset != std::string::npos, "test URI database segment missing");
        reader_uri.insert(offset + database.size() + 1, "_read");
        caf::core::init_global_meta_objects();
        app_meta::init();
        Plugin mongo{argv[1]}, frontend{argv[2]};
        {
            caf::actor_system_config config;
            configure(config, uri, reader_uri);
            caf::actor_system system{config};
            {
                mongocxx::client observer{parsed};
                Harness test{system, mongo, frontend};
                verify(test, observer, database);
            }
            system.await_all_actors_done();
        }
        std::puts("Mongo EntityStore E2E passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mongo EntityStore E2E failed: %s\n", error.what());
        return 1;
    }
}
