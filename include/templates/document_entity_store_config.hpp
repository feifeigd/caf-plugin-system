#pragma once
#include "templates/entity_store_schema.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

namespace caf_plugin_system::entity_store::document {

// A backend-neutral actor routes the whole atomic save; native workers own
// transaction/session details. SQL transaction-handle routing stays separate.
struct service_config {
    // 结构目录：服务/连接路由，以及实体的键、字段和存储映射；不存业务数据。
    schema::schema_catalog catalog;
    std::chrono::milliseconds request_timeout{10000};
    size_t max_pending_requests = 1024;
    uint32_t save_retry_attempts = 3;
    uint32_t load_retry_attempts = 3;
    std::chrono::milliseconds retry_backoff{100};
    std::string config_error;
    std::string backend_name = "Document";
};

inline service_config parse_service_config(const caf::settings& config,
                                          const std::string& default_service,
                                          const std::string& backend_name) {
    service_config result;
    result.backend_name = backend_name;
    result.request_timeout = std::chrono::milliseconds{std::max(caf::get_or(config, "request_timeout_ms", 10000), 100)};
    result.max_pending_requests = static_cast<size_t>(std::max(caf::get_or(config, "max_pending_requests", 1024), 1));
    result.save_retry_attempts = static_cast<uint32_t>(std::clamp(caf::get_or(config, "save_retry_attempts", 3), 1, 10));
    result.load_retry_attempts = static_cast<uint32_t>(std::clamp(caf::get_or(config, "load_retry_attempts", 3), 1, 10));
    result.retry_backoff = std::chrono::milliseconds{std::max(caf::get_or(config, "retry_backoff_ms", 100), 1)};
    if (caf::get_or(config, "schema_source", std::string{"manual"}) != "manual") {
        result.config_error = backend_name + " requires schema_source=manual";
        return result;
    }
    auto stores = caf::get_if<caf::settings>(&config, "stores");
    if (!stores) { result.config_error = "entity_store.stores is missing"; return result; }
    auto service = caf::get_or(config, "backend_service", std::string{});
    if (service.empty()) service = default_service;
    auto catalog = schema::schema_catalog::parse(*stores, service, service, result.config_error, "manual", true);
    if (!catalog) return result;
    for (const auto& [name, unused] : *stores) {
        if (catalog->find_store(name)->schema_source != "manual") {
            result.config_error = backend_name + " store requires schema_source=manual: " + name;
            return result;
        }
    }
    result.catalog = std::move(*catalog);
    return result;
}
} // namespace caf_plugin_system::entity_store::document
