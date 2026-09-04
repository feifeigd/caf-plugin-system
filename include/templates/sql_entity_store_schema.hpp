#pragma once

// SQL EntityStore 的服务端 Schema 与读写路由。
// 业务请求中的实体名/字段名只用于查表，真正进入 SQL 的标识符全部来自
// 这份受信配置，避免把外部输入直接拼进 SQL。

#include "common/entity_store_contract.hpp"

#include <caf/all.hpp>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace caf_plugin_system::entity_store::sql {

struct field_schema {
    std::string name;
    std::string column;
    value_kind kind = value_kind::text;
    bool writable = true;
    bool nullable = true;
    // Discovery distinguishes omitted attributes from explicit policy overrides.
    bool kind_explicit = false;
    bool nullable_explicit = false;
    bool writable_explicit = false;
};

struct entity_schema {
    std::string name;
    std::string table;
    std::string version_column = "version";
    std::vector<field_schema> keys;
    std::vector<field_schema> fields;
    bool discover_from_database = false;
    bool keys_explicit = false;
    bool fields_explicit = false;

    const field_schema* find_key(std::string_view logical_name) const noexcept {
        return find(keys, logical_name);
    }

    const field_schema* find_field(std::string_view logical_name) const noexcept {
        if (auto key = find_key(logical_name))
            return key;
        return find(fields, logical_name);
    }

    const field_schema* find_writable_field(
        std::string_view logical_name) const noexcept {
        auto result = find(fields, logical_name);
        return result && result->writable ? result : nullptr;
    }

private:
    static const field_schema* find(const std::vector<field_schema>& values,
                                    std::string_view name) noexcept {
        for (const auto& value : values)
            if (value.name == name)
                return &value;
        return nullptr;
    }
};

struct store_schema {
    std::string name;
    std::string read_service;
    std::string write_service;
    std::string read_connection = "default";
    std::string write_connection = "default";
    std::string idempotency_table = "__entity_store_requests";
    std::unordered_map<std::string, entity_schema> entities;
    std::string schema_source = "manual";
};

struct schema_discovery_target {
    std::string store;
    std::string entity;
};

class schema_catalog {
public:
    bool add_store(store_schema store, std::string& error) {
        if (store.name.empty()) {
            error = "entity store name is empty";
            return false;
        }
        if (store.read_service.empty() || store.write_service.empty()) {
            error = "entity store services are empty: " + store.name;
            return false;
        }
        if (store.read_connection.empty() || store.write_connection.empty()) {
            error = "entity store connection names are empty: " + store.name;
            return false;
        }
        if (!valid_qualified_identifier(store.idempotency_table)) {
            error = "invalid idempotency table: " + store.idempotency_table;
            return false;
        }
        for (const auto& [name, entity] : store.entities) {
            if (name != entity.name || entity.name.empty()) {
                error = "entity schema name mismatch in store: " + store.name;
                return false;
            }
            if (!validate_entity(entity, error)) {
                error += " (store=" + store.name + ", entity=" + name + ")";
                return false;
            }
        }
        if (store.entities.empty()) {
            error = "entity store has no entities: " + store.name;
            return false;
        }
        if (!stores_.emplace(store.name, std::move(store)).second) {
            error = "duplicate entity store: " + store.name;
            return false;
        }
        return true;
    }

    const store_schema* find_store(std::string_view name) const noexcept {
        auto it = stores_.find(std::string{name});
        return it == stores_.end() ? nullptr : &it->second;
    }

    const entity_schema* find_entity(std::string_view store,
                                     std::string_view entity) const noexcept {
        auto owner = find_store(store);
        if (!owner)
            return nullptr;
        auto it = owner->entities.find(std::string{entity});
        return it == owner->entities.end() ? nullptr : &it->second;
    }

    bool empty() const noexcept { return stores_.empty(); }
    size_t size() const noexcept { return stores_.size(); }

    std::vector<schema_discovery_target> discovery_targets() const {
        std::vector<schema_discovery_target> result;
        for (const auto& [store_name, store] : stores_)
            for (const auto& [entity_name, entity] : store.entities)
                if (entity.discover_from_database)
                    result.push_back({store_name, entity_name});
        std::sort(result.begin(), result.end(), [](const auto& lhs, const auto& rhs) {
            return lhs.store < rhs.store
                   || (lhs.store == rhs.store && lhs.entity < rhs.entity);
        });
        return result;
    }

    entity_schema* mutable_entity(std::string_view store,
                                   std::string_view entity) noexcept {
        auto owner = stores_.find(std::string{store});
        if (owner == stores_.end())
            return nullptr;
        auto found = owner->second.entities.find(std::string{entity});
        return found == owner->second.entities.end() ? nullptr : &found->second;
    }

    static bool validate_entity_schema(const entity_schema& entity,
                                       std::string& error) {
        return validate_entity(entity, error);
    }

    static std::optional<schema_catalog>
    parse(const caf::settings& stores, const std::string& default_read_service,
          const std::string& default_write_service, std::string& error,
          const std::string& default_schema_source = "manual") {
        schema_catalog result;
        for (const auto& [store_name, raw_store] : stores) {
            auto config = caf::get_if<caf::settings>(&raw_store);
            if (!config) {
                error = "store config must be an object: " + store_name;
                return std::nullopt;
            }
            store_schema store;
            store.name = store_name;
            store.schema_source = get_or(*config, "schema_source", default_schema_source);
            if (store.schema_source != "manual" && store.schema_source != "database") {
                error = "unknown schema_source: " + store.schema_source;
                return std::nullopt;
            }
            store.read_service = get_or(*config, "read_service",
                                        default_read_service);
            store.write_service = get_or(*config, "write_service",
                                         default_write_service);
            store.read_connection = get_or(*config, "read_connection",
                                           std::string{"default"});
            store.write_connection = get_or(*config, "write_connection",
                                            std::string{"default"});
            store.idempotency_table = get_or(
                *config, "idempotency_table",
                std::string{"__entity_store_requests"});

            auto entities = child(*config, "entities");
            if (!entities) {
                error = "store.entities is missing: " + store_name;
                return std::nullopt;
            }
            for (const auto& [entity_name, raw_entity] : *entities) {
                auto entity_config = caf::get_if<caf::settings>(&raw_entity);
                if (!entity_config) {
                    error = "entity config must be an object: " + entity_name;
                    return std::nullopt;
                }
                entity_schema entity;
                entity.name = entity_name;
                entity.discover_from_database = store.schema_source == "database";
                entity.keys_explicit = entity_config->find("keys") != entity_config->end();
                entity.fields_explicit = entity_config->find("fields") != entity_config->end();
                entity.table = get_or(*entity_config, "table", std::string{});
                entity.version_column = get_or(
                    *entity_config, "version_column", std::string{"version"});
                if (!parse_fields(*entity_config, "keys", false, entity.keys,
                                  error, !entity.discover_from_database)
                    || !parse_fields(*entity_config, "fields", true,
                                     entity.fields, error, !entity.discover_from_database))
                    return std::nullopt;
                if (!store.entities.emplace(entity.name, std::move(entity)).second) {
                    error = "duplicate entity: " + entity_name;
                    return std::nullopt;
                }
            }
            if (!result.add_store(std::move(store), error))
                return std::nullopt;
        }
        if (result.empty()) {
            error = "entity_store.stores is empty";
            return std::nullopt;
        }
        return result;
    }

private:
    static bool valid_identifier_part(std::string_view value) noexcept {
        if (value.empty())
            return false;
        const auto first = static_cast<unsigned char>(value.front());
        if (!(std::isalpha(first) || value.front() == '_'))
            return false;
        for (char ch : value) {
            const auto current = static_cast<unsigned char>(ch);
            if (!(std::isalnum(current) || ch == '_'))
                return false;
        }
        return true;
    }

    static bool valid_qualified_identifier(std::string_view value) noexcept {
        size_t begin = 0;
        while (begin <= value.size()) {
            auto end = value.find('.', begin);
            auto part = value.substr(
                begin, end == std::string_view::npos ? value.size() - begin
                                                     : end - begin);
            if (!valid_identifier_part(part))
                return false;
            if (end == std::string_view::npos)
                return true;
            begin = end + 1;
        }
        return false;
    }

    static bool validate_entity(const entity_schema& entity,
                                std::string& error) {
        if (!valid_qualified_identifier(entity.table)) {
            error = "invalid entity table: " + entity.table;
            return false;
        }
        if (!valid_identifier_part(entity.version_column)) {
            error = "invalid version column: " + entity.version_column;
            return false;
        }
        if (entity.keys.empty()
            && (!entity.discover_from_database || entity.keys_explicit)) {
            error = "entity has no keys";
            return false;
        }
        if (entity.fields.empty()
            && (!entity.discover_from_database || entity.fields_explicit)) {
            error = "entity has no fields";
            return false;
        }
        std::unordered_map<std::string, bool> names;
        std::unordered_set<std::string> columns{entity.version_column};
        for (const auto& key : entity.keys) {
            if (!validate_field(key, error))
                return false;
            if (!names.emplace(key.name, true).second) {
                error = "duplicate schema field: " + key.name;
                return false;
            }
            if (!columns.insert(key.column).second) {
                error = "duplicate or reserved schema column: " + key.column;
                return false;
            }
        }
        for (const auto& field : entity.fields) {
            if (!validate_field(field, error))
                return false;
            if (!names.emplace(field.name, true).second) {
                error = "duplicate schema field: " + field.name;
                return false;
            }
            if (!columns.insert(field.column).second) {
                error = "duplicate or reserved schema column: " + field.column;
                return false;
            }
        }
        return true;
    }

    static bool validate_field(const field_schema& field,
                               std::string& error) {
        if (field.name.empty()) {
            error = "schema field name is empty";
            return false;
        }
        if (!valid_identifier_part(field.column)) {
            error = "invalid schema column: " + field.column;
            return false;
        }
        if (field.kind == value_kind::bytes
            || field.kind == value_kind::null_value) {
            error = "unsupported SQL entity schema kind: " + field.name;
            return false;
        }
        return true;
    }

    static const caf::settings* child(const caf::settings& parent,
                                      const std::string& name) {
        auto it = parent.find(name);
        return it == parent.end() ? nullptr
                                  : caf::get_if<caf::settings>(&it->second);
    }

    template <class T>
    static T get_or(const caf::settings& parent, const std::string& name,
                    T fallback) {
        auto it = parent.find(name);
        if (it == parent.end())
            return fallback;
        auto value = caf::get_if<T>(&it->second);
        return value ? *value : fallback;
    }

    static bool parse_fields(const caf::settings& entity,
                             const std::string& group, bool writable_default,
                             std::vector<field_schema>& output,
                             std::string& error, bool required = true) {
        auto fields = child(entity, group);
        if (!fields) {
            if (!required && entity.find(group) == entity.end())
                return true;
            error = "entity." + group + " is missing";
            return false;
        }
        for (const auto& [name, raw] : *fields) {
            auto config = caf::get_if<caf::settings>(&raw);
            if (!config) {
                error = "field config must be an object: " + name;
                return false;
            }
            field_schema field;
            field.name = name;
            field.column = get_or(*config, "column", name);
            field.writable = get_or(*config, "writable", writable_default);
            field.nullable = get_or(*config, "nullable", true);
            field.kind_explicit = config->find("kind") != config->end();
            field.nullable_explicit = config->find("nullable") != config->end();
            field.writable_explicit = config->find("writable") != config->end();
            const auto kind_name = get_or(*config, "kind", std::string{"text"});
            if (!from_string(kind_name, field.kind)) {
                error = "unknown field kind '" + kind_name + "': " + name;
                return false;
            }
            output.push_back(std::move(field));
        }
        return true;
    }

    std::unordered_map<std::string, store_schema> stores_;
};

} // namespace caf_plugin_system::entity_store::sql
