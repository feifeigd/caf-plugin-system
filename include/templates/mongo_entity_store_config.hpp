#pragma once

#include "templates/sql_entity_store_schema.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace caf_plugin_system::entity_store::mongo {

// Shared by the routing actor and Mongo's native workers. BSON/driver types
// stay in the Mongo plugin; the public entity messages remain backend-neutral.
struct service_config {
    sql::schema_catalog catalog;
    std::chrono::milliseconds request_timeout{10000};
    size_t max_pending_requests = 1024;
    uint32_t save_retry_attempts = 3;
    uint32_t load_retry_attempts = 3;
    std::chrono::milliseconds retry_backoff{100};
    std::string config_error;
};

inline service_config parse_service_config(const caf::settings& config) {
    service_config result;
    result.request_timeout = std::chrono::milliseconds{
        std::max(caf::get_or(config, "request_timeout_ms", 10000), 100)};
    result.max_pending_requests = static_cast<size_t>(
        std::max(caf::get_or(config, "max_pending_requests", 1024), 1));
    result.save_retry_attempts = static_cast<uint32_t>(
        std::clamp(caf::get_or(config, "save_retry_attempts", 3), 1, 10));
    result.load_retry_attempts = static_cast<uint32_t>(
        std::clamp(caf::get_or(config, "load_retry_attempts", 3), 1, 10));
    result.retry_backoff = std::chrono::milliseconds{
        std::max(caf::get_or(config, "retry_backoff_ms", 100), 1)};
    const auto source = caf::get_or(config, "schema_source", std::string{"manual"});
    if (source != "manual") {
        result.config_error = "MongoDB requires schema_source=manual: collections do not have a fixed SQL schema";
        return result;
    }
    const auto* stores = caf::get_if<caf::settings>(&config, "stores");
    if (!stores) {
        result.config_error = "entity_store.stores is missing";
        return result;
    }
    auto backend = caf::get_or(config, "backend_service", std::string{});
    if (backend.empty())
        backend = "mongo_service";
    auto catalog = sql::schema_catalog::parse(
        *stores, backend, backend, result.config_error, "manual", true);
    if (!catalog)
        return result;
    for (const auto& [store_name, unused] : *stores) {
        const auto* store = catalog->find_store(store_name);
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
    result.catalog = std::move(*catalog);
    return result;
}

} // namespace caf_plugin_system::entity_store::mongo
