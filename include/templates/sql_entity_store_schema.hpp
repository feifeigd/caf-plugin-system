#pragma once

// Compatibility aliases for existing SQL EntityStore users.
#include "templates/entity_store_schema.hpp"

namespace caf_plugin_system::entity_store::sql {

using schema::field_schema;
using schema::entity_schema;
using schema::store_schema;
using schema::schema_discovery_target;
using schema::schema_catalog;

} // namespace caf_plugin_system::entity_store::sql
