#pragma once

// SQL EntityStore 的数据库方言与语句生成器。
// 所有标识符来自 schema_catalog；所有业务值使用参数绑定，NULL 是唯一
// 由生成器写入的值字面量。

#include "templates/sql_entity_store_schema.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace caf_plugin_system::entity_store::sql {

enum class dialect_kind { sqlite, mysql, postgres };

inline std::optional<dialect_kind> parse_dialect(std::string_view name) {
    if (name == "sqlite")
        return dialect_kind::sqlite;
    if (name == "mysql")
        return dialect_kind::mysql;
    if (name == "postgres" || name == "postgresql")
        return dialect_kind::postgres;
    return std::nullopt;
}

struct sql_statement {
    std::string text;
    std::vector<std::string> params;
    std::vector<const field_schema*> result_fields;
};

struct statement_result {
    sql_statement statement;
    std::string error;

    explicit operator bool() const noexcept { return error.empty(); }
};

class sql_dialect {
public:
    explicit sql_dialect(dialect_kind kind) : kind_(kind) {}

    dialect_kind kind() const noexcept { return kind_; }

    std::string quote(std::string_view identifier) const {
        const char mark = kind_ == dialect_kind::mysql ? '`' : '"';
        std::string result;
        result.reserve(identifier.size() + 4);
        size_t begin = 0;
        while (begin <= identifier.size()) {
            auto end = identifier.find('.', begin);
            auto part = identifier.substr(
                begin, end == std::string_view::npos
                           ? identifier.size() - begin
                           : end - begin);
            if (!result.empty())
                result.push_back('.');
            result.push_back(mark);
            result.append(part);
            result.push_back(mark);
            if (end == std::string_view::npos)
                break;
            begin = end + 1;
        }
        return result;
    }

    std::string placeholder(size_t index) const {
        return kind_ == dialect_kind::postgres
                   ? "$" + std::to_string(index)
                   : "?";
    }

    std::string boolean_parameter(bool value) const {
        if (kind_ == dialect_kind::postgres)
            return value ? "true" : "false";
        return value ? "1" : "0";
    }

    sql_statement create_idempotency_table(
        const store_schema& store) const {
        const auto table = quote(store.idempotency_table);
        const auto request_id = quote("request_id");
        const auto signature = quote("request_signature");
        const auto result_data = quote("result_data");
        const auto text_type = kind_ == dialect_kind::mysql ? "LONGTEXT" : "TEXT";
        // MySQL 默认文本排序规则可能不区分大小写；幂等键必须按原始字节
        // 区分，否则 request-A 与 request-a 会错误地被视为同一次请求。
        const auto key_type = kind_ == dialect_kind::mysql
                                  ? "VARBINARY(255)"
                                  : "VARCHAR(255)";
        return {"CREATE TABLE IF NOT EXISTS " + table + " (" + request_id
                    + " " + key_type + " PRIMARY KEY, " + signature + " "
                    + text_type + " NOT NULL, " + result_data + " "
                    + text_type + " NOT NULL)",
                {}, {}};
    }

    sql_statement reserve_request(const store_schema& store,
                                  const std::string& request_id,
                                  const std::string& signature) const {
        const auto table = quote(store.idempotency_table);
        const auto columns = quote("request_id") + ", "
                             + quote("request_signature") + ", "
                             + quote("result_data");
        std::string prefix;
        std::string suffix;
        if (kind_ == dialect_kind::mysql)
            prefix = "INSERT IGNORE INTO ";
        else {
            prefix = "INSERT INTO ";
            suffix = " ON CONFLICT (" + quote("request_id") + ") DO NOTHING";
        }
        return {prefix + table + " (" + columns + ") VALUES ("
                    + placeholder(1) + ", " + placeholder(2) + ", "
                    + placeholder(3) + ")" + suffix,
                {request_id, signature, ""}, {}};
    }

    sql_statement load_request_record(const store_schema& store,
                                      const std::string& request_id) const {
        return {"SELECT " + quote("request_signature") + ", "
                    + quote("result_data") + " FROM "
                    + quote(store.idempotency_table) + " WHERE "
                    + quote("request_id") + " = " + placeholder(1),
                {request_id}, {}};
    }

    sql_statement finish_request_record(const store_schema& store,
                                        const std::string& request_id,
                                        const std::string& result_data) const {
        return {"UPDATE " + quote(store.idempotency_table) + " SET "
                    + quote("result_data") + " = " + placeholder(1)
                    + " WHERE " + quote("request_id") + " = "
                    + placeholder(2),
                {result_data, request_id}, {}};
    }

private:
    dialect_kind kind_;
};

class statement_builder {
public:
    explicit statement_builder(sql_dialect dialect)
        : dialect_(std::move(dialect)) {}

    const sql_dialect& dialect() const noexcept { return dialect_; }

    statement_result load(const entity_schema& schema,
                          const load_request& request) const {
        statement_result result;
        if (!validate_keys(schema, request.target, result.error))
            return result;

        std::vector<const field_schema*> selected;
        if (request.fields.all_fields) {
            for (const auto& field : schema.fields)
                selected.push_back(&field);
        } else {
            for (const auto& name : request.fields.names) {
                auto field = schema.find_field(name);
                if (!field) {
                    result.error = "unknown entity field: " + name;
                    return result;
                }
                selected.push_back(field);
            }
        }

        auto& out = result.statement;
        out.text = "SELECT ";
        for (size_t i = 0; i < selected.size(); ++i) {
            if (i != 0)
                out.text += ", ";
            out.text += dialect_.quote(selected[i]->column);
        }
        if (!selected.empty())
            out.text += ", ";
        out.text += dialect_.quote(schema.version_column) + " FROM "
                    + dialect_.quote(schema.table) + " WHERE ";
        out.result_fields = selected;
        append_where(out, schema, request.target);
        return result;
    }

    statement_result update(const entity_schema& schema,
                            const entity_patch& patch) const {
        statement_result result;
        if (!validate_keys(schema, patch.target, result.error)
            || !validate_patch(schema, patch, result.error))
            return result;
        auto& out = result.statement;
        out.text = "UPDATE " + dialect_.quote(schema.table) + " SET ";
        for (size_t i = 0; i < patch.fields.size(); ++i) {
            if (i != 0)
                out.text += ", ";
            const auto& change = patch.fields[i];
            auto field = schema.find_writable_field(change.name);
            const auto column = dialect_.quote(field->column);
            out.text += column + " = ";
            if (change.operation == patch_op::erase
                || change.data.kind == value_kind::null_value) {
                out.text += "NULL";
            } else {
                if (change.operation == patch_op::increment)
                    out.text += column + " + ";
                append_parameter(out, change.data);
            }
        }
        out.text += ", " + dialect_.quote(schema.version_column) + " = "
                    + dialect_.quote(schema.version_column) + " + 1 WHERE ";
        append_where(out, schema, patch.target);
        if (patch.check_version) {
            out.text += " AND " + dialect_.quote(schema.version_column)
                        + " = ";
            append_parameter(out, value::unsigned_integer(patch.expected_version));
        }
        return result;
    }

    statement_result insert(const entity_schema& schema,
                            const entity_patch& patch) const {
        statement_result result;
        if (!validate_keys(schema, patch.target, result.error)
            || !validate_patch(schema, patch, result.error))
            return result;
        auto& out = result.statement;
        std::vector<std::pair<std::string, const value*>> columns;
        for (const auto& key_schema : schema.keys) {
            auto key = find_named(patch.target.key, key_schema.name);
            columns.emplace_back(key_schema.column, &key->data);
        }
        for (const auto& change : patch.fields) {
            auto field = schema.find_writable_field(change.name);
            columns.emplace_back(field->column, &change.data);
        }

        out.text = "INSERT INTO " + dialect_.quote(schema.table) + " (";
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i != 0)
                out.text += ", ";
            out.text += dialect_.quote(columns[i].first);
        }
        if (!columns.empty())
            out.text += ", ";
        out.text += dialect_.quote(schema.version_column) + ") VALUES (";
        for (size_t i = 0; i < columns.size(); ++i) {
            if (i != 0)
                out.text += ", ";
            if (i < schema.keys.size()) {
                append_parameter(out, *columns[i].second);
                continue;
            }
            const auto& change = patch.fields[i - schema.keys.size()];
            if (change.operation == patch_op::erase
                || columns[i].second->kind == value_kind::null_value)
                out.text += "NULL";
            else
                append_parameter(out, *columns[i].second);
        }
        if (!columns.empty())
            out.text += ", ";
        out.text += "1)";
        return result;
    }

    statement_result current_version(const entity_schema& schema,
                                     const entity_ref& target) const {
        statement_result result;
        if (!validate_keys(schema, target, result.error))
            return result;
        auto& out = result.statement;
        out.text = "SELECT " + dialect_.quote(schema.version_column)
                   + " FROM " + dialect_.quote(schema.table) + " WHERE ";
        append_where(out, schema, target);
        return result;
    }

    bool decode(std::string_view cell, bool is_null, const field_schema& schema,
                value& output, std::string& error) const {
        if (is_null) {
            output = value::null();
            return true;
        }
        switch (schema.kind) {
            case value_kind::null_value:
                output = value::null();
                return true;
            case value_kind::boolean:
                if (cell == "1" || cell == "true" || cell == "t") {
                    output = value::boolean(true);
                    return true;
                }
                if (cell == "0" || cell == "false" || cell == "f") {
                    output = value::boolean(false);
                    return true;
                }
                break;
            case value_kind::signed_integer: {
                int64_t number = 0;
                auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(),
                                              number);
                if (parsed.ec == std::errc{} && parsed.ptr == cell.data() + cell.size()) {
                    output = value::signed_integer(number);
                    return true;
                }
                break;
            }
            case value_kind::unsigned_integer: {
                uint64_t number = 0;
                auto parsed = std::from_chars(cell.data(), cell.data() + cell.size(),
                                              number);
                if (parsed.ec == std::errc{} && parsed.ptr == cell.data() + cell.size()) {
                    output = value::unsigned_integer(number);
                    return true;
                }
                break;
            }
            case value_kind::real: {
                double number = 0;
                const auto parsed = std::from_chars(
                    cell.data(), cell.data() + cell.size(), number);
                if (parsed.ec == std::errc{}
                    && parsed.ptr == cell.data() + cell.size()
                    && std::isfinite(number)) {
                    output = value::real(number);
                    return true;
                }
                break;
            }
            case value_kind::decimal:
                output = value::decimal(std::string{cell});
                return true;
            case value_kind::text:
                output = value::text(std::string{cell});
                return true;
            case value_kind::json:
                output = value::json(std::string{cell});
                return true;
            case value_kind::bytes:
                break;
        }
        error = "cannot decode field '" + schema.name + "' as "
                + to_string(schema.kind);
        return false;
    }

private:
    bool validate_keys(const entity_schema& schema, const entity_ref& target,
                       std::string& error) const {
        if (target.key.size() != schema.keys.size()) {
            error = "entity key does not match schema: " + target.entity;
            return false;
        }
        for (const auto& expected : schema.keys) {
            auto actual = find_named(target.key, expected.name);
            if (!actual) {
                error = "missing entity key: " + expected.name;
                return false;
            }
            if (actual->data.kind == value_kind::null_value
                || !compatible(expected.kind, actual->data.kind, false)) {
                error = "invalid key type: " + expected.name;
                return false;
            }
        }
        return true;
    }

    bool validate_patch(const entity_schema& schema, const entity_patch& patch,
                        std::string& error) const {
        for (const auto& change : patch.fields) {
            auto field = schema.find_writable_field(change.name);
            if (!field) {
                error = "unknown or read-only entity field: " + change.name;
                return false;
            }
            if ((change.operation == patch_op::erase
                 || change.data.kind == value_kind::null_value)
                && !field->nullable) {
                error = "entity field is not nullable: " + change.name;
                return false;
            }
            if (change.operation != patch_op::erase
                && !compatible(field->kind, change.data.kind,
                               change.operation == patch_op::increment)) {
                error = "invalid entity field type: " + change.name;
                return false;
            }
        }
        return true;
    }

    static bool numeric(value_kind kind) noexcept {
        return kind == value_kind::signed_integer
               || kind == value_kind::unsigned_integer
               || kind == value_kind::real || kind == value_kind::decimal;
    }

    static bool compatible(value_kind expected, value_kind actual,
                           bool increment) noexcept {
        if (actual == value_kind::null_value)
            return true;
        if (increment)
            return numeric(expected) && numeric(actual);
        return expected == actual
               || (numeric(expected) && numeric(actual));
    }

    static const named_value* find_named(const std::vector<named_value>& values,
                                         std::string_view name) noexcept {
        for (const auto& value : values)
            if (value.name == name)
                return &value;
        return nullptr;
    }

    void append_where(sql_statement& out, const entity_schema& schema,
                      const entity_ref& target) const {
        for (size_t i = 0; i < schema.keys.size(); ++i) {
            if (i != 0)
                out.text += " AND ";
            const auto& key_schema = schema.keys[i];
            const auto* key = find_named(target.key, key_schema.name);
            out.text += dialect_.quote(key_schema.column) + " = ";
            append_parameter(out, key->data);
        }
    }

    void append_parameter(sql_statement& out, const value& input) const {
        out.text += dialect_.placeholder(out.params.size() + 1);
        switch (input.kind) {
            case value_kind::boolean:
                out.params.push_back(
                    dialect_.boolean_parameter(input.boolean_value));
                break;
            case value_kind::signed_integer:
                out.params.push_back(std::to_string(input.signed_value));
                break;
            case value_kind::unsigned_integer:
                out.params.push_back(std::to_string(input.unsigned_value));
                break;
            case value_kind::real: {
                std::ostringstream text;
                text.imbue(std::locale::classic());
                text.precision(std::numeric_limits<double>::max_digits10);
                text << input.real_value;
                out.params.push_back(text.str());
                break;
            }
            case value_kind::decimal:
            case value_kind::text:
            case value_kind::json:
                out.params.push_back(input.text_value);
                break;
            case value_kind::null_value:
            case value_kind::bytes:
                out.params.emplace_back();
                break;
        }
    }

    sql_dialect dialect_;
};

} // namespace caf_plugin_system::entity_store::sql
