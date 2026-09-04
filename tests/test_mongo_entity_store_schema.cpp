#include "templates/entity_store_schema.hpp"
#include "templates/mongo_entity_store_config.hpp"
#include "templates/sql_entity_store_schema.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>

namespace entity_store = caf_plugin_system::entity_store;
namespace schema = entity_store::schema;
namespace sql = entity_store::sql;
namespace mongo = entity_store::mongo;

static_assert(std::is_same_v<schema::field_schema, sql::field_schema>);
static_assert(std::is_same_v<schema::entity_schema, sql::entity_schema>);
static_assert(std::is_same_v<schema::store_schema, sql::store_schema>);
static_assert(std::is_same_v<schema::schema_discovery_target,
                             sql::schema_discovery_target>);
static_assert(std::is_same_v<schema::schema_catalog, sql::schema_catalog>);

namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

class ConfigurationFixture {
public:
    ConfigurationFixture() {
        entity.emplace("table", "orders");
        keys.emplace("id", field("order_id", "text", false, false));
        fields.emplace("status", field("state", "text", true, false));
        fields.emplace("created_at", field("created_at", "signed_integer", false, false));
    }

    static caf::settings field(const std::string& column,
                               const std::string& kind = "text",
                               bool writable = true, bool nullable = true) {
        caf::settings result;
        result.emplace("column", column);
        result.emplace("kind", kind);
        result.emplace("writable", writable);
        result.emplace("nullable", nullable);
        return result;
    }

    void status(const std::string& column, const std::string& kind = "text",
                bool writable = true) {
        fields.find("status")->second = field(column, kind, writable);
    }

    void id(const std::string& column) {
        keys.find("id")->second = field(column, "text", false, false);
    }

    caf::settings stores() const {
        auto entity_config = entity;
        if (include_keys)
            entity_config.emplace("keys", keys);
        if (include_fields)
            entity_config.emplace("fields", fields);
        caf::settings entities;
        entities.emplace("order", std::move(entity_config));
        auto store_config = store;
        store_config.emplace("entities", std::move(entities));
        caf::settings result;
        result.emplace("commerce", std::move(store_config));
        return result;
    }

    caf::settings config() const {
        auto result = options;
        result.emplace("stores", stores());
        return result;
    }

    mongo::service_config parse() const {
        return mongo::parse_service_config(config());
    }

    caf::settings options;
    caf::settings store;
    caf::settings entity;
    caf::settings keys;
    caf::settings fields;
    bool include_keys = true;
    bool include_fields = true;
};

void require_rejected(const mongo::service_config& config,
                      const std::string& scenario) {
    require(!config.config_error.empty(), scenario + ": expected an error");
    require(config.catalog.empty(), scenario + ": invalid catalog was published");
}

void verify_defaults_and_allowlist() {
    ConfigurationFixture fixture;
    auto config = fixture.parse();
    require(config.config_error.empty(), config.config_error);
    require(config.catalog.size() == 1, "expected exactly one allowed store");
    const auto* store = config.catalog.find_store("commerce");
    require(store != nullptr, "commerce store missing");
    require(store->read_service == "mongo_service"
                && store->write_service == "mongo_service",
            "default Mongo backend was not selected");
    require(store->read_connection == "default"
                && store->write_connection == "default",
            "default connection routing changed");
    require(store->schema_source == "manual", "Mongo schema must default to manual");
    require(store->idempotency_table == "__entity_store_requests",
            "default request collection changed");
    const auto* entity = config.catalog.find_entity("commerce", "order");
    require(entity != nullptr, "order entity missing");
    require(entity->table == "orders" && entity->version_column == "version",
            "collection/version mapping changed");
    const auto* key = entity->find_key("id");
    require(key && key->column == "order_id" && !key->writable && !key->nullable,
            "key mapping or policy changed");
    const auto* status = entity->find_writable_field("status");
    require(status && status->column == "state" && !status->nullable,
            "writable field alias was not preserved");
    require(!entity->find_writable_field("created_at"),
            "read-only field became writable");
    require(!entity->find_writable_field("id"), "key became writable");
    require(!entity->find_field("state"), "physical name bypassed logical whitelist");
    require(!entity->find_field("unlisted"), "unlisted field became visible");
    require(!config.catalog.find_entity("commerce", "unlisted"),
            "unlisted entity became visible");
    require(!config.catalog.find_store("unlisted"), "unlisted store became visible");
    require(config.catalog.discovery_targets().empty(), "manual catalog needs discovery");
    require(config.request_timeout.count() == 10000
                && config.max_pending_requests == 1024
                && config.save_retry_attempts == 3
                && config.load_retry_attempts == 3
                && config.retry_backoff.count() == 100,
            "service defaults changed");
}

void verify_routing_and_limits() {
    ConfigurationFixture fixture;
    fixture.options.emplace("backend_service", "shared_mongo");
    auto config = fixture.parse();
    const auto* inherited = config.catalog.find_store("commerce");
    require(config.config_error.empty() && inherited, config.config_error);
    require(inherited->read_service == "shared_mongo"
                && inherited->write_service == "shared_mongo",
            "custom default backend was not inherited");

    fixture.store.emplace("read_service", "read_mongo");
    fixture.store.emplace("write_service", "write_mongo");
    fixture.store.emplace("read_connection", "replica");
    fixture.store.emplace("write_connection", "primary");
    fixture.store.emplace("idempotency_table", "order_requests");
    fixture.entity.emplace("version_column", "revision");
    fixture.options.emplace("request_timeout_ms", -1);
    fixture.options.emplace("max_pending_requests", 0);
    fixture.options.emplace("save_retry_attempts", 0);
    fixture.options.emplace("load_retry_attempts", 100);
    fixture.options.emplace("retry_backoff_ms", -1);
    config = fixture.parse();
    const auto* store = config.catalog.find_store("commerce");
    require(config.config_error.empty() && store, config.config_error);
    require(store->read_service == "read_mongo"
                && store->write_service == "write_mongo"
                && store->read_connection == "replica"
                && store->write_connection == "primary",
            "explicit read/write routes were not preserved");
    require(store->idempotency_table == "order_requests"
                && config.catalog.find_entity("commerce", "order")->version_column
                       == "revision",
            "custom collection/version mapping changed");
    require(config.request_timeout.count() == 100
                && config.max_pending_requests == 1
                && config.save_retry_attempts == 1
                && config.load_retry_attempts == 10
                && config.retry_backoff.count() == 1,
            "unsafe service limits were not clamped");
}

void verify_discovery_and_required_schema() {
    ConfigurationFixture global;
    global.options.emplace("schema_source", "database");
    require_rejected(global.parse(), "global database discovery");

    ConfigurationFixture local;
    local.store.emplace("schema_source", "database");
    require_rejected(local.parse(), "per-store database discovery");

    ConfigurationFixture unknown;
    unknown.options.emplace("schema_source", "unknown");
    require_rejected(unknown.parse(), "unknown schema source");
    require_rejected(mongo::parse_service_config({}), "missing stores");
    caf::settings wrong_type;
    wrong_type.emplace("stores", "not an object");
    require_rejected(mongo::parse_service_config(wrong_type), "malformed stores");
    caf::settings empty;
    empty.emplace("stores", caf::settings{});
    require_rejected(mongo::parse_service_config(empty), "empty stores");

    ConfigurationFixture missing_keys;
    missing_keys.include_keys = false;
    require_rejected(missing_keys.parse(), "missing explicit keys");
    ConfigurationFixture empty_keys;
    empty_keys.keys.clear();
    require_rejected(empty_keys.parse(), "empty explicit keys");
    ConfigurationFixture missing_fields;
    missing_fields.include_fields = false;
    require_rejected(missing_fields.parse(), "missing explicit fields");
    ConfigurationFixture empty_fields;
    empty_fields.fields.clear();
    require_rejected(empty_fields.parse(), "empty explicit fields");
}

void verify_mapping_rejections() {
    ConfigurationFixture key_version;
    key_version.id("version");
    require_rejected(key_version.parse(), "primary key/version collision");
    ConfigurationFixture field_version;
    field_version.status("version");
    require_rejected(field_version.parse(), "field/version collision");
    ConfigurationFixture key_field;
    key_field.status("order_id");
    require_rejected(key_field.parse(), "field/primary key collision");
    ConfigurationFixture logical_collision;
    logical_collision.fields.emplace("id", ConfigurationFixture::field("other_id"));
    require_rejected(logical_collision.parse(), "duplicate logical key/field");

    for (const auto& column : {std::string{"payload.value"}, std::string{"$set"},
                              std::string{}, std::string{"1status"},
                              std::string{"status-name"}, std::string{"state\0x", 7}}) {
        ConfigurationFixture invalid;
        invalid.status(column);
        require_rejected(invalid.parse(), "invalid field storage name");
    }
    ConfigurationFixture invalid_key;
    invalid_key.id("nested.id");
    require_rejected(invalid_key.parse(), "invalid key storage name");
    ConfigurationFixture invalid_version;
    invalid_version.entity.emplace("version_column", "$version");
    require_rejected(invalid_version.parse(), "invalid version storage name");
    ConfigurationFixture invalid_kind;
    invalid_kind.status("state", "unknown");
    require_rejected(invalid_kind.parse(), "unknown field kind");
}

void verify_mongo_reserved_names() {
    ConfigurationFixture id_key;
    id_key.id("_id");
    auto accepted = id_key.parse();
    require(accepted.config_error.empty(), accepted.config_error);
    require(accepted.catalog.find_entity("commerce", "order")->find_key("id")->column
                == "_id",
            "Mongo _id mapping as a key was rejected");

    ConfigurationFixture id_version;
    id_version.entity.emplace("version_column", "_id");
    require_rejected(id_version.parse(), "_id as version");
    for (bool writable : {false, true}) {
        ConfigurationFixture id_field;
        id_field.status("_id", "text", writable);
        require_rejected(id_field.parse(), "_id as non-key field");
    }
    for (const auto& collection : {"__entity_store_requests", "system.orders"}) {
        ConfigurationFixture reserved;
        reserved.entity.find("table")->second = std::string{collection};
        require_rejected(reserved.parse(), "reserved entity collection");
    }
    ConfigurationFixture system_requests;
    system_requests.store.emplace("idempotency_table", "system.requests");
    require_rejected(system_requests.parse(), "system request collection");
}

void verify_binary_is_opt_in() {
    ConfigurationFixture fixture;
    fixture.fields.emplace("payload", ConfigurationFixture::field("payload", "bytes"));
    auto config = fixture.parse();
    require(config.config_error.empty(), config.config_error);
    const auto* entity = config.catalog.find_entity("commerce", "order");
    require(entity && entity->find_field("payload")->kind == entity_store::value_kind::bytes,
            "Mongo binary field was not accepted");

    std::string error;
    require(!sql::schema_catalog::parse(fixture.stores(), "sqlite_service",
                                       "sqlite_service", error),
            "SQL default parser unexpectedly accepted binary");
    require(!error.empty(), "SQL binary rejection needs a diagnostic");
    require(!schema::schema_catalog::validate_entity_schema(*entity, error),
            "default schema validator unexpectedly accepted binary");
    require(schema::schema_catalog::validate_entity_schema(*entity, error, true),
            "explicit binary schema validation failed");

    const auto* store = config.catalog.find_store("commerce");
    schema::schema_catalog default_catalog = {};
    require(!default_catalog.add_store(*store, error),
            "default catalog unexpectedly accepted binary");
    schema::schema_catalog binary_catalog{true};
    require(binary_catalog.add_store(*store, error),
            "binary-enabled catalog rejected its store");

    auto null_schema = *entity;
    null_schema.fields.front().kind = entity_store::value_kind::null_value;
    require(!schema::schema_catalog::validate_entity_schema(null_schema, error, true),
            "binary policy must not allow null as a declared field kind");
}

} // namespace

int main() {
    try {
        verify_defaults_and_allowlist();
        verify_routing_and_limits();
        verify_discovery_and_required_schema();
        verify_mapping_rejections();
        verify_mongo_reserved_names();
        verify_binary_is_opt_in();
        std::puts("Mongo EntityStore schema/configuration tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Mongo EntityStore schema/configuration test failed: %s\n",
                     error.what());
        return 1;
    }
}
