#include "templates/sql_entity_store_schema_provider.hpp"

#include <cassert>
#include <string>

using namespace caf_plugin_system::entity_store;
using namespace caf_plugin_system::entity_store::sql;
namespace db = caf_plugin_system::db;

namespace {

entity_schema unresolved() {
    entity_schema result;
    result.name = "order";
    result.table = "orders";
    result.discover_from_database = true;
    return result;
}

db::db_result metadata(std::string id_type = "TEXT",
                       std::string status_type = "TEXT",
                       std::string version_type = "BIGINT") {
    db::db_result result;
    result.ok = true;
    result.columns = {"column_name", "data_type", "type_detail", "nullable",
                      "key_order", "generated", "hidden"};
    result.rows = {
        {"id", id_type, id_type, "0", "1", "0", "0"},
        {"status", status_type, status_type, "0", "0", "0", "0"},
        {"version", version_type, version_type, "0", "0", "0", "0"},
    };
    return result;
}

caf::settings store_config(const caf::settings& entity,
                           const std::string& source = "database") {
    caf::settings entities;
    entities.emplace("order", entity);
    caf::settings store;
    store.emplace("schema_source", source);
    store.emplace("entities", std::move(entities));
    caf::settings stores;
    stores.emplace("commerce", std::move(store));
    return stores;
}

void verify_configuration() {
    std::string error;
    caf::settings entity;
    entity.emplace("table", "orders");
    auto catalog = schema_catalog::parse(store_config(entity),
                                         "sqlite_service", "sqlite_service", error);
    assert(catalog);
    assert(catalog->discovery_targets().size() == 1);
    assert(catalog->discovery_targets().front().store == "commerce");
    auto pending = catalog->mutable_entity("commerce", "order");
    assert(pending && pending->discover_from_database && pending->keys.empty());
    assert(!catalog->find_entity("commerce", "unlisted_table"));
    assert(!catalog->mutable_entity("missing", "order"));
    sqlite_schema_provider provider;
    assert(provider.apply(*pending, metadata(), error));
    assert(catalog->discovery_targets().empty());

    // Existing manual configurations still require keys and fields.
    assert(!schema_catalog::parse(store_config(entity, "manual"), "sqlite_service",
                                  "sqlite_service", error));
    assert(!schema_catalog::parse(store_config(entity, "unknown"), "sqlite_service",
                                  "sqlite_service", error));
    caf::settings keys;
    keys.emplace("id", caf::settings{});
    caf::settings fields;
    fields.emplace("status", caf::settings{});
    entity.emplace("keys", std::move(keys));
    entity.emplace("fields", std::move(fields));
    auto manual = schema_catalog::parse(store_config(entity, "manual"),
                                        "sqlite_service", "sqlite_service", error);
    assert(manual && manual->discovery_targets().empty());
    assert(manual->find_entity("commerce", "order")->fields.front().kind
           == value_kind::text);

    // Top-level default is inherited when a store does not override it.
    caf::settings minimal_entity;
    minimal_entity.emplace("table", "orders");
    caf::settings entities;
    entities.emplace("order", minimal_entity);
    caf::settings store;
    store.emplace("entities", entities);
    caf::settings stores;
    stores.emplace("commerce", store);
    assert(schema_catalog::parse(stores, "sqlite_service", "sqlite_service", error,
                                  "database"));
    assert(!schema_catalog::parse(stores, "sqlite_service", "sqlite_service", error));
}

void verify_query_plans() {
    auto entity = unresolved();
    sqlite_schema_provider sqlite;
    mysql_schema_provider mysql;
    postgres_schema_provider postgres;
    auto query = sqlite.metadata_query(entity);
    assert(query.text.find("pragma_table_xinfo(?, ?)") != std::string::npos);
    assert(query.params == std::vector<std::string>({"orders", "main"}));
    query = mysql.metadata_query(entity);
    assert(query.text.find("information_schema.COLUMNS") != std::string::npos);
    assert(query.text.find("AS `generated`") != std::string::npos);
    assert(query.text.find("AS `hidden`") != std::string::npos);
    assert(query.params == std::vector<std::string>({"", "orders"}));
    query = postgres.metadata_query(entity);
    assert(query.text.find("$1") != std::string::npos);
    assert(query.text.find("$2") != std::string::npos);
    assert(query.params == std::vector<std::string>({"", "orders"}));
    entity.table = "commerce.orders";
    assert(sqlite.metadata_query(entity).params
           == std::vector<std::string>({"orders", "commerce"}));
    assert(mysql.metadata_query(entity).params
           == std::vector<std::string>({"commerce", "orders"}));
    assert(postgres.metadata_query(entity).params
           == std::vector<std::string>({"commerce", "orders"}));
    for (auto dialect : {dialect_kind::sqlite, dialect_kind::mysql,
                         dialect_kind::postgres})
        assert(make_schema_provider(dialect));
}

void verify_inference_and_policy() {
    sqlite_schema_provider provider;
    std::string error;
    auto entity = unresolved();
    auto result = metadata();
    result.rows.push_back({"memo", "TEXT", "TEXT", "1", "0", "0", "0"});
    result.rows.push_back({"computed", "INTEGER", "INTEGER", "0", "0", "1", "0"});
    result.rows.push_back({"hidden", "TEXT", "TEXT", "0", "0", "0", "1"});
    assert(provider.apply(entity, result, error));
    assert(!entity.discover_from_database);
    assert(entity.keys.size() == 1 && entity.keys.front().name == "id");
    assert(!entity.keys.front().writable && !entity.keys.front().nullable);
    assert(entity.fields.size() == 3);
    assert(entity.find_field("memo")->nullable);
    assert(!entity.find_field("computed")->writable);
    assert(!entity.find_field("hidden"));
    assert(!entity.find_field("version"));

    // Explicit lists remain allowlists and support aliases plus policy tightening.
    entity = unresolved();
    entity.keys_explicit = true;
    entity.keys = {{"order_id", "id"}};
    entity.fields_explicit = true;
    field_schema memo;
    memo.name = "note";
    memo.column = "memo";
    memo.nullable = false;
    memo.nullable_explicit = true;
    memo.writable = false;
    memo.writable_explicit = true;
    entity.fields = {memo};
    assert(provider.apply(entity, result, error));
    assert(entity.keys.front().name == "order_id");
    assert(entity.fields.size() == 1 && !entity.find_field("status"));
    assert(!entity.find_field("note")->nullable && !entity.find_field("note")->writable);

    entity = unresolved();
    entity.fields_explicit = true;
    field_schema generated;
    generated.name = generated.column = "computed";
    generated.writable = true;
    generated.writable_explicit = true;
    entity.fields = {generated};
    assert(!provider.apply(entity, result, error));
    assert(error.find("cannot be writable") != std::string::npos);
    entity.fields.front().column = "hidden";
    assert(!provider.apply(entity, result, error));
    assert(error.find("hidden") != std::string::npos);

    entity = unresolved();
    entity.fields_explicit = true;
    field_schema nullable;
    nullable.name = nullable.column = "status";
    nullable.nullable = true;
    nullable.nullable_explicit = true;
    entity.fields = {nullable};
    assert(!provider.apply(entity, result, error));
    assert(error.find("cannot relax") != std::string::npos);
}

void verify_types() {
    std::string error;
    sqlite_schema_provider sqlite;
    mysql_schema_provider mysql;
    postgres_schema_provider postgres;
    auto entity = unresolved();
    auto result = metadata("TEXT", "NUMERIC");
    assert(!sqlite.apply(entity, result, error));
    assert(entity.discover_from_database && entity.fields.empty());
    assert(error.find("explicit kind") != std::string::npos);
    entity.fields_explicit = true;
    field_schema amount;
    amount.name = "amount";
    amount.column = "status";
    amount.kind = value_kind::decimal;
    amount.kind_explicit = true;
    entity.fields = {amount};
    assert(sqlite.apply(entity, result, error));
    assert(entity.find_field("amount")->kind == value_kind::decimal);

    entity = unresolved();
    result = metadata("bigint", "decimal", "bigint");
    result.rows.front()[2] = "bigint unsigned";
    assert(mysql.apply(entity, result, error));
    assert(entity.keys.front().kind == value_kind::unsigned_integer);
    assert(entity.fields.front().kind == value_kind::decimal);
    entity = unresolved();
    result = metadata("text", "jsonb", "bigint");
    result.rows.push_back({"enabled", "boolean", "bool", "0", "0", "0", "0"});
    assert(postgres.apply(entity, result, error));
    assert(entity.find_field("status")->kind == value_kind::json);
    assert(entity.find_field("enabled")->kind == value_kind::boolean);

    entity = unresolved();
    result = metadata("text", "USER-DEFINED", "bigint");
    assert(!postgres.apply(entity, result, error));
    result = metadata("text", "bytea", "bigint");
    entity.fields_explicit = true;
    amount.kind = value_kind::text;
    entity.fields = {amount};
    assert(!postgres.apply(entity, result, error));
    assert(error.find("binary") != std::string::npos);
}

void verify_rejections() {
    sqlite_schema_provider provider;
    std::string error;
    auto reject = [&](db::db_result result) {
        auto entity = unresolved();
        assert(!provider.apply(entity, result, error));
        assert(entity.discover_from_database && entity.keys.empty());
    };
    auto result = metadata();
    result.rows.clear();
    reject(result);
    result = metadata();
    result.rows.pop_back();
    reject(result);
    result = metadata();
    result.rows[2][3] = "1"; // Nullable version.
    reject(result);
    result = metadata();
    result.rows[2][5] = "1"; // Generated version.
    reject(result);
    reject(metadata("TEXT", "TEXT", "TEXT"));
    result = metadata();
    result.rows.front()[4] = "0"; // No primary key.
    reject(result);
    result = metadata();
    result.rows.push_back(result.rows.front());
    reject(result);
    result = metadata();
    result.columns.pop_back();
    reject(result);
    result = metadata();
    result.nulls = {{1}};
    reject(result);
    result = metadata();
    result.rows.front().pop_back();
    reject(result);

    result = metadata();
    result.rows.insert(result.rows.begin(),
                       {"tenant_id", "TEXT", "TEXT", "0", "2", "0", "0"});
    auto entity = unresolved();
    assert(provider.apply(entity, result, error));
    assert(entity.keys.size() == 2 && entity.keys.front().column == "id"
           && entity.keys.back().column == "tenant_id");
    entity = unresolved();
    entity.keys_explicit = true;
    entity.keys = {{"id", "id"}};
    assert(!provider.apply(entity, result, error));
    assert(error.find("complete database primary key") != std::string::npos);

    entity = unresolved();
    entity.fields_explicit = true;
    entity.fields = {{"id", "status"}}; // Alias collision with primary key.
    assert(!provider.apply(entity, metadata(), error));
    assert(error.find("duplicate schema field") != std::string::npos);
}

} // namespace

int main() {
    verify_configuration();
    verify_query_plans();
    verify_inference_and_policy();
    verify_types();
    verify_rejections();
}
