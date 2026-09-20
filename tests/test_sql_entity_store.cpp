#include "templates/sql_entity_store_statements.hpp"

#include <cassert>
#include <string>

using namespace caf_plugin_system::entity_store;
using namespace caf_plugin_system::entity_store::sql;

namespace {

entity_schema order_schema() {
    return entity_schema{
        .name = "order",
        .table = "orders",
        .version_column = "version",
        .keys = {{"order_id", "order_id", value_kind::text, false, false}},
        .fields = {
            {"status", "status", value_kind::text, true, false},
            {"paid_amount", "paid_amount", value_kind::decimal, true, false},
            {"memo", "memo", value_kind::text, true, true},
        },
    };
}

entity_ref order_ref() {
    return entity_ref{
        .store = "commerce",
        .partition = "order-42",
        .entity = "order",
        .key = {{"order_id", value::text("order-42")}},
    };
}

} // namespace

int main() {
    auto order = order_schema();
    store_schema commerce{
        .name = "commerce",
        .read_service = "sqlite_service",
        .write_service = "sqlite_service",
        .read_connection = "replica",
        .write_connection = "primary",
        .entities = {{"order", order}},
    };
    schema_catalog catalog;
    std::string error;
    assert(catalog.add_store(commerce, error));
    assert(catalog.find_store("commerce")->read_connection == "replica");
    assert(catalog.find_store("commerce")->write_connection == "primary");

    statement_builder sqlite{sql_dialect{dialect_kind::sqlite}};
    load_request load{
        .target = order_ref(),
        .fields = {.all_fields = false, .names = {"status", "memo"}},
    };
    auto select = sqlite.load(order, load);
    assert(select);
    assert(select.statement.text
           == "SELECT \"status\", \"memo\", \"version\" FROM \"orders\" "
              "WHERE \"order_id\" = ?");
    assert(select.statement.params == std::vector<std::string>{"order-42"});

    entity_patch patch{
        .target = order_ref(),
        .fields = {
            {patch_op::set, "status", value::text("paid")},
            {patch_op::increment, "paid_amount", value::decimal("99.50")},
        },
        .check_version = true,
        .expected_version = 7,
    };
    auto update = sqlite.update(order, patch);
    assert(update);
    assert(update.statement.text
           == "UPDATE \"orders\" SET \"status\" = ?, \"paid_amount\" = "
              "\"paid_amount\" + ?, \"version\" = \"version\" + 1 WHERE "
              "\"order_id\" = ? AND \"version\" = ?");
    assert(update.statement.params
           == std::vector<std::string>({"paid", "99.50", "order-42", "7"}));

    auto deletion = patch;
    deletion.fields.clear();
    deletion.operation = entity_operation::delete_entity;
    auto deleted = sqlite.delete_entity(order, deletion);
    assert(deleted && deleted.statement.text ==
        "DELETE FROM \"orders\" WHERE \"order_id\" = ? AND \"version\" = ?");
    assert(deleted.statement.params == std::vector<std::string>({"order-42", "7"}));
    auto key_only = deletion;
    key_only.operation = entity_operation::insert;
    assert(sqlite.insert(order, key_only));

    patch.create_if_missing = true;
    auto insert = sqlite.insert(order, patch);
    assert(insert);
    assert(insert.statement.text
           == "INSERT INTO \"orders\" (\"order_id\", \"status\", "
              "\"paid_amount\", \"version\") VALUES (?, ?, ?, 1)");

    statement_builder postgres{sql_dialect{dialect_kind::postgres}};
    auto pg_update = postgres.update(order, patch);
    assert(pg_update);
    assert(pg_update.statement.text.find("$1") != std::string::npos);
    assert(pg_update.statement.text.find("$4") != std::string::npos);

    sql_dialect mysql{dialect_kind::mysql};
    auto reserve = mysql.reserve_request(
        commerce, "request-1", "signature");
    assert(reserve.text.find("INSERT IGNORE INTO") == 0);
    assert(reserve.text.find("`__entity_store_requests`") != std::string::npos);
    assert(mysql.create_idempotency_table(commerce).text.find("VARBINARY(255)")
           != std::string::npos);

    patch.fields = {{patch_op::erase, "status", value::null()}};
    auto not_nullable = sqlite.update(order, patch);
    assert(!not_nullable);
    assert(not_nullable.error.find("not nullable") != std::string::npos);

    value decoded;
    assert(sqlite.decode("", true, order.fields[2], decoded, error));
    assert(decoded.kind == value_kind::null_value);

    auto null_key = order_ref();
    null_key.key.front().data = value::null();
    auto rejected_key = sqlite.load(
        order, load_request{.target = std::move(null_key)});
    assert(!rejected_key);
    assert(rejected_key.error.find("invalid key type") != std::string::npos);

    auto reserved = commerce;
    reserved.name = "reserved";
    reserved.entities.at("order").fields.front().column = "version";
    schema_catalog reserved_catalog;
    assert(!reserved_catalog.add_store(std::move(reserved), error));
    assert(error.find("reserved schema column") != std::string::npos);

    auto invalid = commerce;
    invalid.name = "unsafe";
    invalid.idempotency_table = "requests;DROP_TABLE";
    schema_catalog rejected;
    assert(!rejected.add_store(std::move(invalid), error));
    assert(error.find("invalid idempotency table") != std::string::npos);
}
