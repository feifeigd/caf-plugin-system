#pragma once
#include "templates/document_entity_store_config.hpp"

namespace caf_plugin_system::entity_store::redis {
using service_config = document::service_config;

inline service_config parse_service_config(const caf::settings& config) {
    return document::parse_service_config(config, "redis_service", "Redis");
}
} // namespace caf_plugin_system::entity_store::redis
