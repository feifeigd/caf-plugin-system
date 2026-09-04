#pragma once
#include "templates/mongo_entity_store_config.hpp"
#include <bsoncxx/builder/basic/array.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/decimal128.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <iterator>
#include <stdexcept>

namespace caf_plugin_system::entity_store::mongo {
namespace bson = bsoncxx::builder::basic;

class EntityError : public std::runtime_error {
public:
    EntityError(result_code code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}
    result_code code() const noexcept { return code_; }
private:
    result_code code_;
};

// Requests supply typed values, never Mongo query/update documents.
class EntityCodec {
public:
    static void append(bson::document& out, const std::string& name, const value& item) {
        using bson::kvp;    // key-value pair
        switch (item.kind) {
            case value_kind::null_value: out.append(kvp(name, bsoncxx::types::b_null{})); break;
            case value_kind::boolean: out.append(kvp(name, item.boolean_value)); break;
            case value_kind::signed_integer: out.append(kvp(name, item.signed_value)); break;
            case value_kind::unsigned_integer:
                // Always decimal: increments crossing INT64_MAX must not become doubles.
                out.append(kvp(name, bsoncxx::decimal128{std::to_string(item.unsigned_value)})); break;
            case value_kind::real:
                if (!std::isfinite(item.real_value)) invalid("non-finite real");
                out.append(kvp(name, item.real_value)); break;
            case value_kind::decimal: {
                bsoncxx::decimal128 number{item.text_value};
                auto text = number.to_string();
                if (text.find("NaN") != std::string::npos || text.find("Infinity") != std::string::npos)
                    invalid("non-finite decimal");
                out.append(kvp(name, number)); break;
            }
            case value_kind::text: out.append(kvp(name, item.text_value)); break;
            case value_kind::json: {
                auto wrapper = bsoncxx::from_json("{\"value\":" + item.text_value + "}");
                auto view = wrapper.view();
                auto element = view["value"];
                if (std::distance(view.begin(), view.end()) != 1 || !element
                    || (element.type() != bsoncxx::type::k_document && element.type() != bsoncxx::type::k_array))
                    invalid("JSON field must be one document or array");
                out.append(kvp(name, element.get_value())); break;
            }
            case value_kind::bytes:
                if (item.bytes_value.size() > 8 * 1024 * 1024) invalid("binary field exceeds 8 MiB");
                out.append(kvp(name, bsoncxx::types::b_binary{bsoncxx::binary_sub_type::k_binary,
                    static_cast<uint32_t>(item.bytes_value.size()),
                    reinterpret_cast<const uint8_t*>(item.bytes_value.data())})); break;
            default: invalid("unknown value kind");
        }
    }

    static value decode(bsoncxx::document::element item, const sql::field_schema& field) {
        if (!item || item.type() == bsoncxx::type::k_null) {
            if (!field.nullable) corrupt("missing/null required field: " + field.name);
            return value::null();
        }
        switch (field.kind) {
            case value_kind::boolean:
                if (item.type() == bsoncxx::type::k_bool) return value::boolean(item.get_bool().value);
                break;
            case value_kind::signed_integer:
                if (item.type() == bsoncxx::type::k_int64) return value::signed_integer(item.get_int64().value);
                if (item.type() == bsoncxx::type::k_int32) return value::signed_integer(item.get_int32().value);
                break;
            case value_kind::unsigned_integer:
                if (item.type() == bsoncxx::type::k_int64 && item.get_int64().value >= 0)
                    return value::unsigned_integer(static_cast<uint64_t>(item.get_int64().value));
                if (item.type() == bsoncxx::type::k_int32 && item.get_int32().value >= 0)
                    return value::unsigned_integer(static_cast<uint64_t>(item.get_int32().value));
                if (item.type() == bsoncxx::type::k_decimal128)
                    return value::unsigned_integer(parse_unsigned(item.get_decimal128().value.to_string()));
                break;
            case value_kind::real:
                if (item.type() == bsoncxx::type::k_double && std::isfinite(item.get_double().value))
                    return value::real(item.get_double().value);
                break;
            case value_kind::decimal:
                if (item.type() == bsoncxx::type::k_decimal128) {
                    auto text = item.get_decimal128().value.to_string();
                    if (text.find("NaN") == std::string::npos && text.find("Infinity") == std::string::npos)
                        return value::decimal(std::move(text));
                }
                break;
            case value_kind::text:
                if (item.type() == bsoncxx::type::k_string) return value::text(std::string{item.get_string().value});
                break;
            case value_kind::json:
                if (item.type() == bsoncxx::type::k_document) return value::json(bsoncxx::to_json(item.get_document().value));
                if (item.type() == bsoncxx::type::k_array) return value::json(bsoncxx::to_json(item.get_array().value));
                break;
            case value_kind::bytes:
                if (item.type() == bsoncxx::type::k_binary) {
                    auto binary = item.get_binary();
                    if (binary.sub_type != bsoncxx::binary_sub_type::k_binary) break;
                    std::vector<std::byte> bytes(binary.size);
                    if (binary.size) std::memcpy(bytes.data(), binary.bytes, binary.size);
                    return value::bytes(std::move(bytes));
                }
                break;
            default: break;
        }
        corrupt("BSON type/range does not match schema field: " + field.name);
    }

    static void validate_value(const value& item, const sql::field_schema& field, bool key = false) {
        if (item.kind == value_kind::null_value) {
            if (key || !field.nullable) invalid("field cannot be null: " + field.name);
        } else if (item.kind != field.kind) invalid("value kind does not match schema field: " + field.name);
        bson::document check;
        append(check, "value", item);
        if (key && check.view()["value"].type() == bsoncxx::type::k_array)
            invalid("Mongo entity keys cannot be JSON arrays (multikey uniqueness is not entity identity)");
    }

    /// @return a zero value of the given kind, for incrementing a missing field.
    static value zero(value_kind kind) {
        switch (kind) {
            case value_kind::signed_integer: return value::signed_integer(0);
            case value_kind::unsigned_integer: return value::unsigned_integer(0);
            case value_kind::real: return value::real(0);
            case value_kind::decimal: return value::decimal("0");
            default: invalid("increment requires a numeric schema field");
        }
    }

    static void append_increment(bson::document& out, const std::string& name,
                                 const value& input, value_kind target) {
        if (!detail::is_number(input.kind) || !detail::is_number(target))
            invalid("increment requires numeric input and schema field");
        // A signed target needs an integral int64 delta; never round via a
        // double merely to convert an integer or Decimal128 input.
        if (target == value_kind::signed_integer) {
            int64_t delta = 0;
            if (input.kind == value_kind::signed_integer) delta = input.signed_value;
            else if (input.kind == value_kind::unsigned_integer) {
                if (input.unsigned_value > uint64_t(std::numeric_limits<int64_t>::max()))
                    invalid("increment delta exceeds signed int64");
                delta = static_cast<int64_t>(input.unsigned_value);
            } else if (input.kind == value_kind::real) {
                if (!std::isfinite(input.real_value) || std::trunc(input.real_value) != input.real_value
                    || input.real_value < -9223372036854775808.0 || input.real_value >= 9223372036854775808.0)
                    invalid("increment delta is not an exact signed int64");
                delta = static_cast<int64_t>(input.real_value);
            } else {
                auto text = bsoncxx::decimal128{input.text_value}.to_string();
                bool negative = !text.empty() && text.front() == '-';
                if (negative) text.erase(0, 1);
                uint64_t magnitude = 0;
                try { magnitude = parse_unsigned(std::move(text)); }
                catch (const EntityError&) { invalid("increment delta is not an exact signed int64"); }
                const auto maximum = uint64_t(std::numeric_limits<int64_t>::max());
                if (magnitude > maximum + (negative ? 1u : 0u)) invalid("increment delta exceeds signed int64");
                delta = negative && magnitude == maximum + 1u ? std::numeric_limits<int64_t>::min()
                    : (negative ? -static_cast<int64_t>(magnitude) : static_cast<int64_t>(magnitude));
            }
            append(out, name, value::signed_integer(delta));
            return;
        }
        if (target == value_kind::real) {
            // Floating-point rounding is intentional only for a REAL target.
            double delta = 0;
            if (input.kind == value_kind::real) delta = input.real_value;
            else if (input.kind == value_kind::signed_integer) delta = static_cast<double>(input.signed_value);
            else if (input.kind == value_kind::unsigned_integer) delta = static_cast<double>(input.unsigned_value);
            else {
                auto text = bsoncxx::decimal128{input.text_value}.to_string();
                auto parsed = std::from_chars(text.data(), text.data() + text.size(), delta);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                    invalid("increment delta cannot be represented as real");
            }
            append(out, name, value::real(delta));
            return;
        }
        std::string decimal;
        if (input.kind == value_kind::decimal) decimal = input.text_value;
        else if (input.kind == value_kind::signed_integer) decimal = std::to_string(input.signed_value);
        else if (input.kind == value_kind::unsigned_integer) decimal = std::to_string(input.unsigned_value);
        else {
            if (!std::isfinite(input.real_value)) invalid("non-finite increment");
            char text[64];
            auto formatted = std::to_chars(text, text + sizeof(text), input.real_value,
                std::chars_format::general, std::numeric_limits<double>::max_digits10);
            if (formatted.ec != std::errc{}) invalid("invalid real increment");
            decimal.assign(text, formatted.ptr);
        }
        append(out, name, value::decimal(std::move(decimal)));
    }

    static bsoncxx::document::value key_filter(const sql::entity_schema& schema, const entity_ref& target) {
        if (target.key.size() != schema.keys.size()) invalid("entity key field count mismatch");
        bson::document filter;
        bson::array exact_keys;
        for (const auto& field : schema.keys) {
            auto found = std::find_if(target.key.begin(), target.key.end(),
                [&](const auto& item) { return item.name == field.name; });
            if (found == target.key.end()) invalid("missing entity key: " + field.name);
            validate_value(found->data, field, true);
            bson::document equality;
            append(equality, "$eq", found->data);
            filter.append(bson::kvp(field.column, equality.extract()));
            // A query predicate $eq also matches an array element. $expr
            // compares the whole field value, and $literal prevents values
            // such as "$other" or {"$gt": 1} from becoming expressions.
            bson::document literal;
            append(literal, "$literal", found->data);
            bson::array operands;
            operands.append("$" + field.column, literal.extract());
            exact_keys.append(bson::make_document(bson::kvp("$eq", operands.extract())));
        }
        filter.append(bson::kvp("$expr", bson::make_document(bson::kvp("$and", exact_keys.extract()))));
        return filter.extract();
    }

    static int64_t version(bsoncxx::document::view doc, const sql::entity_schema& schema) {
        auto item = doc[schema.version_column];
        if (!item || item.type() != bsoncxx::type::k_int64 || item.get_int64().value <= 0)
            corrupt("entity version must be a positive BSON int64");
        return item.get_int64().value;
    }

    static bsoncxx::document::value signature(const save_request& request) {
        bson::array changes;
        for (const auto& change : request.changes) {
            bson::array keys, fields;
            for (const auto& key : change.target.key) {
                bson::document part;
                part.append(bson::kvp("name", key.name), bson::kvp("kind", int32_t(key.data.kind)));
                append(part, "value", key.data);
                keys.append(part.extract());
            }
            for (const auto& field : change.fields) {
                bson::document part;
                part.append(bson::kvp("name", field.name), bson::kvp("op", int32_t(field.operation)),
                            bson::kvp("kind", int32_t(field.data.kind)));
                append(part, "value", field.data);
                fields.append(part.extract());
            }
            changes.append(bson::make_document(bson::kvp("store", change.target.store),
                bson::kvp("partition", change.target.partition), bson::kvp("entity", change.target.entity),
                bson::kvp("keys", keys.extract()), bson::kvp("fields", fields.extract()),
                bson::kvp("create", change.create_if_missing), bson::kvp("check", change.check_version),
                bson::kvp("version", std::to_string(change.expected_version))));
        }
        auto result = bson::make_document(bson::kvp("changes", changes.extract()));
        if (result.view().length() > 8 * 1024 * 1024) invalid("save exceeds 8 MiB");
        return result;
    }

    [[noreturn]] static void invalid(std::string message) { throw EntityError{result_code::invalid_request, std::move(message)}; }
    [[noreturn]] static void corrupt(std::string message) { throw EntityError{result_code::internal_error, std::move(message)}; }
private:
    static uint64_t parse_unsigned(std::string text) {
        // Normalize Decimal128 scientific notation with digits, never doubles.
        int exponent = 0;
        if (auto e = text.find_first_of("eE"); e != std::string::npos) {
            auto suffix = text.substr(e + 1);
            if (!suffix.empty() && suffix.front() == '+') suffix.erase(0, 1);
            auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), exponent);
            if (parsed.ec != std::errc{} || parsed.ptr != suffix.data() + suffix.size()) corrupt("invalid integer exponent");
            text.resize(e);
        }
        if (!text.empty() && text.front() == '+') text.erase(0, 1);
        if (auto dot = text.find('.'); dot != std::string::npos) {
            exponent -= static_cast<int>(text.size() - dot - 1); text.erase(dot, 1);
        }
        if (text.empty() || text.find_first_not_of("0123456789") != std::string::npos) corrupt("invalid BSON unsigned integer");
        while (exponent < 0 && !text.empty() && text.back() == '0') { text.pop_back(); ++exponent; }
        if (text.find_first_not_of('0') == std::string::npos) return 0;
        if (exponent < 0 || exponent > 20) corrupt("BSON unsigned integer out of range");
        text.append(static_cast<size_t>(exponent), '0');
        uint64_t result = 0;
        auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) corrupt("BSON unsigned integer out of range");
        return result;
    }
};
} // namespace caf_plugin_system::entity_store::mongo
