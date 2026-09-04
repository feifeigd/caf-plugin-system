#pragma once

// Metadata reads use the configured writer connection and an explicit table
// allowlist. Providers do no I/O: the actor owns asynchronous startup and retry.
#include "common/db_contract.hpp"
#include "templates/sql_entity_store_statements.hpp"

#include <algorithm>
#include <charconv>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace caf_plugin_system::entity_store::sql {

struct schema_query {
    std::string text;
    std::vector<std::string> params;
};

class schema_provider {
public:
    virtual ~schema_provider() = default;
    virtual schema_query metadata_query(const entity_schema& entity) const = 0;

    // Publish only a completely validated snapshot. Failure leaves the original
    // unresolved schema unchanged; callers must not serve it before discovery.
    bool apply(entity_schema& entity, const db::db_result& result,
               std::string& error) const {
        if (!entity.discover_from_database) {
            error = "schema is not awaiting database discovery: " + entity.name;
            return false;   // 不用从数据库读取数据表的元数据，元数据可能在配置文件里面
        }
        std::vector<column_metadata> columns;
        if (!decode_metadata(result, columns, error))
            return false;
        std::unordered_map<std::string, const column_metadata*> by_name;
        std::vector<const column_metadata*> primary_key;
        for (const auto& column : columns) {
            if (!by_name.emplace(column.name, &column).second) {
                error = "duplicate metadata column: " + column.name;
                return false;
            }
            if (column.key_order > 0)
                primary_key.push_back(&column);
        }
        std::sort(primary_key.begin(), primary_key.end(), [](auto lhs, auto rhs) {
            return lhs->key_order < rhs->key_order;
        });
        if (primary_key.empty()) {
            error = "database entity requires a primary key: " + entity.table;
            return false;
        }
        for (size_t i = 0; i < primary_key.size(); ++i) {
            if (primary_key[i]->key_order != i + 1) {
                error = "invalid primary key metadata: " + entity.table;
                return false;
            }
        }
        auto version = by_name.find(entity.version_column);
        if (version == by_name.end()) {
            error = "version column is missing: " + entity.version_column;
            return false;
        }
        const auto& version_column = *version->second;
        const auto version_kind = infer_kind(version_column);
        if (version_column.nullable || version_column.generated
            || version_column.hidden || version_column.key_order != 0
            || !version_kind
            || (*version_kind != value_kind::signed_integer
                && *version_kind != value_kind::unsigned_integer)) {
            error = "version column must be a non-null, non-generated integer: "
                    + entity.version_column;
            return false;
        }

        auto completed = entity;
        if (!entity.keys_explicit) {
            completed.keys.clear();
            for (const auto* key : primary_key) {
                field_schema field;
                field.name = field.column = key->name;
                completed.keys.push_back(std::move(field));
            }
        }
        if (completed.keys.size() != primary_key.size()) {
            error = "configured keys must include the complete database primary key";
            return false;
        }
        std::unordered_set<std::string> key_columns;
        for (auto& field : completed.keys) {
            const auto found = by_name.find(field.column);
            if (found == by_name.end() || found->second->key_order == 0
                || !key_columns.insert(field.column).second) {
                error = "configured key is not a unique primary key column: "
                        + field.column;
                return false;
            }
            if (!complete_field(field, *found->second, true, error))
                return false;
        }
        if (!entity.fields_explicit) {
            completed.fields.clear();
            for (const auto& column : columns) {
                if (column.key_order > 0 || column.hidden
                    || column.name == entity.version_column)
                    continue;
                field_schema field;
                field.name = field.column = column.name;
                completed.fields.push_back(std::move(field));
            }
        }
        for (auto& field : completed.fields) {
            auto found = by_name.find(field.column);
            if (found == by_name.end()) {
                error = "configured field is absent from database: " + field.column;
                return false;
            }
            if (found->second->key_order > 0
                || field.column == entity.version_column) {
                error = "keys/version cannot also be exposed as ordinary fields: "
                        + field.column;
                return false;
            }
            if (!complete_field(field, *found->second, false, error))
                return false;
        }
        completed.discover_from_database = false;
        if (!schema_catalog::validate_entity_schema(completed, error))
            return false;
        entity = std::move(completed);
        return true;
    }

protected:
    struct column_metadata {
        std::string name;
        std::string type;
        std::string detail;
        bool nullable = false;
        size_t key_order = 0;
        bool generated = false;
        bool hidden = false;
    };

    virtual std::optional<value_kind> infer_kind(const column_metadata& column) const = 0;

    static std::string lowercase(std::string text) {
        for (char& ch : text)
            if (ch >= 'A' && ch <= 'Z')
                ch = static_cast<char>(ch - 'A' + 'a');
        return text;
    }

    static std::string base_type(std::string type) {
        type = lowercase(std::move(type));
        const auto length = type.find('('); // base_type (length)
        if (length != std::string::npos)
            type.resize(length);
        while (!type.empty() && type.back() == ' ')
            type.pop_back();
        const auto first = type.find_first_not_of(' ');
        return first == std::string::npos ? std::string{} : type.substr(first);
    }

    /// @return (schema, table) pair. If no schema is specified, the fallback is used. 数据库名->表名
    static std::pair<std::string, std::string> table_parts(
        const entity_schema& entity, const std::string& fallback_schema) {
        const auto dot = entity.table.find('.');
        return dot == std::string::npos
            ? std::pair{fallback_schema, entity.table} // 没有点 .
            : std::pair{entity.table.substr(0, dot), entity.table.substr(dot + 1)};
    }

private:
    bool complete_field(field_schema& field, const column_metadata& column,
                        bool key, std::string& error) const {
        if (column.hidden) {
            error = "hidden database columns cannot be exposed: " + column.name;
            return false;
        }
        const auto type = base_type(column.type);
        // The current SQL value contract does not safely transport binary data.
        if (type == "blob" || type == "tinyblob" || type == "mediumblob"
            || type == "longblob" || type == "binary" || type == "varbinary"
            || type == "bytea" || type == "bit" || type == "bit varying") {
            error = "binary database column is unsupported: " + column.name;
            return false;
        }
        if (!field.kind_explicit) {
            auto inferred = infer_kind(column);
            if (!inferred) {
                error = "ambiguous database type '" + column.type
                        + "' for " + column.name + "; configure an explicit kind";
                return false;
            }
            field.kind = *inferred;
        }
        if (field.nullable_explicit && field.nullable && !column.nullable) {
            error = "nullable override cannot relax database NOT NULL: " + column.name;
            return false;
        }
        if (!field.nullable_explicit)
            field.nullable = column.nullable;
        if (key && field.nullable) {
            error = "primary keys must not be nullable: " + column.name;
            return false;
        }
        if (field.writable_explicit && field.writable && (key || column.generated)) {
            error = "keys/generated columns cannot be writable: " + column.name;
            return false;
        }
        if (key || column.generated)
            field.writable = false;
        return true;
    }

    static bool decode_metadata(const db::db_result& result,
                                std::vector<column_metadata>& columns,
                                std::string& error) {
        if (!result.ok) {
            error = "schema metadata query failed: " + result.error;
            return false;
        }
        if (result.rows.empty()) {
            error = "table is missing, inaccessible, or has no visible columns";
            return false;
        }
        const std::vector<std::string> names = {
            "column_name", "data_type", "type_detail", "nullable",
            "key_order", "generated", "hidden"};
        std::vector<size_t> positions;
        for (const auto& name : names) {
            const auto found = std::find(result.columns.begin(), result.columns.end(), name);
            if (found == result.columns.end()) {
                error = "schema metadata result is missing column: " + name;
                return false;
            }
            positions.push_back(static_cast<size_t>(found - result.columns.begin()));
        }
        for (size_t row_index = 0; row_index < result.rows.size(); ++row_index) {
            const auto& row = result.rows[row_index];
            for (const auto position : positions) {
                if (position >= row.size() || result.is_null(row_index, position)) {
                    error = "invalid NULL/truncated schema metadata row";
                    return false;
                }
            }
            column_metadata column;
            column.name = row[positions[0]];
            column.type = row[positions[1]];
            column.detail = row[positions[2]];
            auto boolean = [&](size_t index, bool& output) {
                const auto& value = row[positions[index]];
                if (value != "0" && value != "1")
                    return false;
                output = value == "1";
                return true;
            };
            const auto& key = row[positions[4]];
            auto parsed = std::from_chars(key.data(), key.data() + key.size(),
                                          column.key_order);
            if (column.name.empty() || parsed.ec != std::errc{}
                || parsed.ptr != key.data() + key.size()
                || !boolean(3, column.nullable) || !boolean(5, column.generated)
                || !boolean(6, column.hidden)) {
                error = "invalid schema metadata value";
                return false;
            }
            columns.push_back(std::move(column));
        }
        return true;
    }
};

class sqlite_schema_provider final : public schema_provider {
public:
    schema_query metadata_query(const entity_schema& entity) const override {
        // string -> string
        auto [schema, table] = table_parts(entity, "main");
        return {
            "SELECT name AS column_name, type AS data_type, type AS type_detail, "
            "CASE WHEN \"notnull\" = 0 AND pk = 0 THEN 1 ELSE 0 END AS nullable, "
            "pk AS key_order, CASE WHEN hidden IN (2, 3) THEN 1 ELSE 0 END AS generated, "
            "CASE WHEN hidden = 1 THEN 1 ELSE 0 END AS hidden "
            "FROM pragma_table_xinfo(?, ?) ORDER BY cid", {table, schema}};
    }

protected:
    std::optional<value_kind> infer_kind(const column_metadata& column) const override {
        const auto type = base_type(column.type);
        if (type == "integer" || type == "int" || type == "tinyint"
            || type == "smallint" || type == "mediumint" || type == "bigint"
            || type == "int2" || type == "int8")
            return value_kind::signed_integer;
        if (type == "real" || type == "double" || type == "double precision"
            || type == "float")
            return value_kind::real;
        if (type == "text" || type == "varchar" || type == "char"
            || type == "character" || type == "clob" || type == "nvarchar"
            || type == "nchar")
            return value_kind::text;
        // SQLite affinity is not a strict declared-type guarantee. In particular
        // DECIMAL/NUMERIC, BOOLEAN and untyped columns need a conscious override.
        return std::nullopt;
    }
};

class mysql_schema_provider final : public schema_provider {
public:
    schema_query metadata_query(const entity_schema& entity) const override {
        // 数据库 -> 数据表
        auto [schema, table] = table_parts(entity, "");
        return {
            "SELECT c.COLUMN_NAME AS column_name, c.DATA_TYPE AS data_type, "
            "c.COLUMN_TYPE AS type_detail, CASE WHEN c.IS_NULLABLE = 'YES' "
            "THEN 1 ELSE 0 END AS nullable, COALESCE(k.ORDINAL_POSITION, 0) AS key_order, "
            "CASE WHEN c.GENERATION_EXPRESSION <> '' OR c.EXTRA LIKE '%auto_increment%' "
            "THEN 1 ELSE 0 END AS `generated`, CASE WHEN c.EXTRA LIKE '%INVISIBLE%' "
            "THEN 1 ELSE 0 END AS `hidden` FROM information_schema.COLUMNS c "
            "LEFT JOIN information_schema.KEY_COLUMN_USAGE k ON k.TABLE_SCHEMA = c.TABLE_SCHEMA "
            "AND k.TABLE_NAME = c.TABLE_NAME AND k.COLUMN_NAME = c.COLUMN_NAME "
            "AND k.CONSTRAINT_NAME = 'PRIMARY' WHERE c.TABLE_SCHEMA = "
            "COALESCE(NULLIF(?, ''), DATABASE()) AND c.TABLE_NAME = ? ORDER BY c.ORDINAL_POSITION",
            {schema, table}};
    }

protected:
    std::optional<value_kind> infer_kind(const column_metadata& column) const override {
        const auto type = base_type(column.type);
        if (type == "tinyint" || type == "smallint" || type == "mediumint"
            || type == "int" || type == "integer" || type == "bigint")
            return lowercase(column.detail).find("unsigned") != std::string::npos
                ? value_kind::unsigned_integer : value_kind::signed_integer;
        if (type == "decimal" || type == "numeric")
            return value_kind::decimal;
        if (type == "float" || type == "double" || type == "real")
            return value_kind::real;
        if (type == "json")
            return value_kind::json;
        if (type == "char" || type == "varchar" || type == "tinytext"
            || type == "text" || type == "mediumtext" || type == "longtext"
            || type == "date" || type == "datetime" || type == "timestamp"
            || type == "time")
            return value_kind::text;
        return std::nullopt;
    }
};

class postgres_schema_provider final : public schema_provider {
public:
    schema_query metadata_query(const entity_schema& entity) const override {
        auto [schema, table] = table_parts(entity, "");
        return {
            "SELECT c.column_name AS column_name, c.data_type AS data_type, "
            "c.udt_name AS type_detail, CASE WHEN c.is_nullable = 'YES' THEN 1 ELSE 0 END AS nullable, "
            "COALESCE(k.ordinal_position, 0) AS key_order, CASE WHEN c.is_generated <> 'NEVER' "
            "OR c.is_identity = 'YES' OR c.column_default LIKE 'nextval(%' THEN 1 ELSE 0 END AS generated, "
            "0 AS hidden FROM information_schema.columns c LEFT JOIN "
            "(SELECT kcu.table_schema, kcu.table_name, kcu.column_name, kcu.ordinal_position "
            "FROM information_schema.key_column_usage kcu JOIN information_schema.table_constraints tc "
            "ON tc.constraint_catalog = kcu.constraint_catalog AND tc.constraint_schema = kcu.constraint_schema "
            "AND tc.constraint_name = kcu.constraint_name AND tc.table_schema = kcu.table_schema "
            "AND tc.table_name = kcu.table_name WHERE tc.constraint_type = 'PRIMARY KEY') k "
            "ON k.table_schema = c.table_schema AND k.table_name = c.table_name AND k.column_name = c.column_name "
            "WHERE c.table_schema = COALESCE(NULLIF($1, ''), current_schema()) AND c.table_name = $2 "
            "ORDER BY c.ordinal_position", {schema, table}};
    }

protected:
    std::optional<value_kind> infer_kind(const column_metadata& column) const override {
        const auto type = base_type(column.type);
        if (type == "smallint" || type == "integer" || type == "bigint")
            return value_kind::signed_integer;
        if (type == "numeric" || type == "decimal")
            return value_kind::decimal;
        if (type == "real" || type == "double precision")
            return value_kind::real;
        if (type == "boolean")
            return value_kind::boolean;
        if (type == "json" || type == "jsonb")
            return value_kind::json;
        if (type == "text" || type == "character varying" || type == "character"
            || type == "uuid" || type == "date" || type == "timestamp without time zone"
            || type == "timestamp with time zone" || type == "time without time zone"
            || type == "time with time zone")
            return value_kind::text;
        return std::nullopt;
    }
};

inline std::unique_ptr<schema_provider> make_schema_provider(dialect_kind dialect) {
    switch (dialect) {
        case dialect_kind::sqlite:   return std::make_unique<sqlite_schema_provider>();
        case dialect_kind::mysql:    return std::make_unique<mysql_schema_provider>();
        case dialect_kind::postgres: return std::make_unique<postgres_schema_provider>();
    }
    return nullptr;
}

} // namespace caf_plugin_system::entity_store::sql
