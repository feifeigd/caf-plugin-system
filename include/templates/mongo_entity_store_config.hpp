#pragma once
#include "templates/document_entity_store_config.hpp"
#include "templates/sql_entity_store_schema.hpp" // existing Mongo source compatibility

namespace caf_plugin_system::entity_store::mongo {
using service_config = document::service_config;

inline service_config parse_service_config(const caf::settings& config) {
    auto result = document::parse_service_config(config, "mongo_service", "MongoDB");
    if (!result.config_error.empty()) return result;
    // Publish no catalog until Mongo-specific policy checks also succeed.
    auto catalog = std::move(result.catalog);
    for (const auto& [store_name, unused] : *caf::get_if<caf::settings>(&config, "stores")) {
        const auto* store = catalog.find_store(store_name);
        if (store->schema_source != "manual") {
            result.config_error = "MongoDB store requires schema_source=manual: " + store_name;
            return result;
        }
        if (store->idempotency_table.starts_with("system.")) {
            result.config_error = "MongoDB cannot use a system idempotency collection";
            return result;
        }
        for (const auto& [entity_name, entity] : store->entities) {
            if (entity.table == store->idempotency_table
                || entity.table.starts_with("system.")) {
                result.config_error = "MongoDB entity collection is reserved: " + entity.table;
                return result;
            }
            if (entity.version_column == "_id") {
                result.config_error = "MongoDB version column cannot be _id";
                return result;
            }
            for (const auto& field : entity.fields) {
                if (field.column == "_id") {
                    result.config_error = "MongoDB _id may only be mapped as an entity key";
                    return result;
                }
            }
        }
    }
    result.catalog = std::move(catalog);
    return result;
}
} // namespace caf_plugin_system::entity_store::mongo
