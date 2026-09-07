#pragma once

#include "templates/entity_store_schema.hpp"
#include <caf/json_value.hpp>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace caf_plugin_system::entity_store::redis {

class EntityError : public std::runtime_error {
public:
    EntityError(result_code code, std::string message)
        : std::runtime_error(std::move(message)), code_(code) {}
    result_code code() const noexcept { return code_; }
private:
    result_code code_;
};

[[noreturn]] inline void invalid(std::string message) {
    throw EntityError{result_code::invalid_request, std::move(message)};
}
[[noreturn]] inline void corrupt(std::string message) {
    throw EntityError{result_code::internal_error, std::move(message)};
}

// Exact, bounded base-10 arithmetic. Neither money nor uint64 passes through
// Redis Lua's double representation. Scientific notation is normalized.
class Decimal final {
public:
    explicit Decimal(std::string text) {
        if (text.empty() || text.size() > limit) invalid("invalid/oversized decimal");
        size_t pos = 0;
        if (text[pos] == '-' || text[pos] == '+') negative_ = text[pos++] == '-';
        bool dot = false, any = false;
        int fraction = 0;
        while (pos < text.size() && text[pos] != 'e' && text[pos] != 'E') {
            const char ch = text[pos++];
            if (ch == '.' && !dot) { dot = true; continue; }
            if (ch < '0' || ch > '9') invalid("invalid decimal");
            digits_ += ch; any = true;
            if (dot) ++fraction;
        }
        if (!any) invalid("invalid decimal");
        int exponent = 0;
        if (pos < text.size()) {
            ++pos;
            if (pos < text.size() && text[pos] == '+') ++pos;
            auto parsed = std::from_chars(text.data() + pos, text.data() + text.size(), exponent);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                || exponent < -int(limit) || exponent > int(limit)) invalid("decimal exponent out of range");
        }
        scale_ = fraction - exponent;
        if (scale_ < 0) { digits_.append(size_t(-scale_), '0'); scale_ = 0; }
        normalize();
    }

    std::string str() const {
        std::string out = digits_;
        if (scale_ > 0) {
            if (out.size() <= size_t(scale_)) out.insert(0, size_t(scale_) + 1 - out.size(), '0');
            out.insert(out.size() - size_t(scale_), 1, '.');
        }
        if (negative_) out.insert(out.begin(), '-');
        return out;
    }
    std::string integer() const {
        if (scale_ != 0) invalid("increment is not an exact integer");
        return str();
    }
    Decimal add(const Decimal& other) const {
        auto a = digits_, b = other.digits_;
        const auto scale = std::max(scale_, other.scale_);
        a.append(size_t(scale - scale_), '0');
        b.append(size_t(scale - other.scale_), '0');
        if (a.size() > limit || b.size() > limit) invalid("decimal precision exceeds 4096 digits");
        std::string digits;
        bool negative = negative_;
        if (negative_ == other.negative_) {
            int carry = 0;
            const size_t length = std::max(a.size(), b.size());
            for (size_t i = 0; i < length || carry; ++i) {
                const int sum = carry + (i < a.size() ? a[a.size()-1-i]-'0' : 0)
                    + (i < b.size() ? b[b.size()-1-i]-'0' : 0);
                digits += char('0' + sum % 10); carry = sum / 10;
            }
        } else {
            if (a.size() < b.size() || (a.size() == b.size() && a < b)) {
                std::swap(a, b); negative = other.negative_;
            }
            int borrow = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                int difference = a[a.size()-1-i]-'0' - borrow
                    - (i < b.size() ? b[b.size()-1-i]-'0' : 0);
                borrow = difference < 0;
                if (borrow) difference += 10;
                digits += char('0' + difference);
            }
        }
        std::reverse(digits.begin(), digits.end());
        Decimal result{"0"};
        result.digits_ = std::move(digits); result.scale_ = scale; result.negative_ = negative;
        result.normalize();
        return result;
    }
private:
    void normalize() {
        const auto first = digits_.find_first_not_of('0');
        if (first == std::string::npos) { digits_ = "0"; scale_ = 0; negative_ = false; return; }
        digits_.erase(0, first);
        while (scale_ > 0 && digits_.back() == '0') { digits_.pop_back(); --scale_; }
        if (digits_.size() > limit || scale_ > int(limit)) invalid("decimal precision exceeds 4096 digits");
    }
    static constexpr size_t limit = 4096;
    std::string digits_;
    int scale_ = 0;
    bool negative_ = false;
};

// Versioned, length-prefixed binary encoding, independent of native endianness,
// CAF metadata registration and C++ object layout.
class Writer final {
public:
    void number(uint64_t value) {
        for (int i = 7; i >= 0; --i) data_ += char((value >> (i * 8)) & 255);
    }
    void text(std::string_view value) {
        if (value.size() > max_bytes) invalid("entity payload exceeds 8 MiB");
        number(value.size()); data_.append(value); check();
    }
    void byte(uint8_t value) { data_ += char(value); }
    std::string take() { check(); return std::move(data_); }
    static constexpr size_t max_bytes = 8 * 1024 * 1024;
private:
    void check() const { if (data_.size() > max_bytes) invalid("entity payload exceeds 8 MiB"); }
    std::string data_;
};

class Reader final {
public:
    explicit Reader(std::string_view data) : data_(data) {
        if (data.size() > Writer::max_bytes) corrupt("stored entity exceeds size limit");
    }
    uint8_t byte() {
        if (pos_ == data_.size()) corrupt("truncated entity encoding");
        return static_cast<uint8_t>(data_[pos_++]);
    }
    uint64_t number() {
        uint64_t out = 0;
        for (int i = 0; i < 8; ++i) out = (out << 8) | byte();
        return out;
    }
    std::string text() {
        auto count = number();
        if (count > data_.size() - pos_) corrupt("invalid entity length");
        std::string out{data_.substr(pos_, size_t(count))}; pos_ += size_t(count); return out;
    }
    void finish() const { if (pos_ != data_.size()) corrupt("trailing entity data"); }
private:
    std::string_view data_;
    size_t pos_ = 0;
};

struct Document {
    uint64_t version = 0;
    std::map<std::string, value> fields; // physical schema column names
};

class EntityCodec final {
public:
    static std::string numeric_text(const value& item) {
        switch (item.kind) {
            case value_kind::signed_integer: return std::to_string(item.signed_value);
            case value_kind::unsigned_integer: return std::to_string(item.unsigned_value);
            case value_kind::decimal: return Decimal{item.text_value}.str();
            case value_kind::real: {
                if (!std::isfinite(item.real_value)) invalid("non-finite real");
                char text[64];
                auto result = std::to_chars(text, text + sizeof(text), item.real_value,
                    std::chars_format::general, std::numeric_limits<double>::max_digits10);
                if (result.ec != std::errc{}) invalid("invalid real");
                return {text, result.ptr};
            }
            default: invalid("increment requires a numeric value");
        }
    }
    static void validate_value(const value& item, const schema::field_schema& field, bool key = false) {
        if (item.kind == value_kind::null_value) {
            if (key || !field.nullable) invalid("field cannot be null: " + field.name);
        } else if (item.kind != field.kind) invalid("value kind does not match schema: " + field.name);
        Writer check; encode_value(check, item);
    }
    static value zero(value_kind kind) {
        switch (kind) {
            case value_kind::signed_integer: return value::signed_integer(0);
            case value_kind::unsigned_integer: return value::unsigned_integer(0);
            case value_kind::real: return value::real(0);
            case value_kind::decimal: return value::decimal("0");
            default: invalid("increment requires a numeric schema field");
        }
    }
    static value increment(const value& before, const value& delta, value_kind kind) {
        if (!detail::is_number(kind) || !detail::is_number(delta.kind)
            || before.kind != kind) invalid("cannot increment null or non-numeric field");
        if (kind == value_kind::real) {
            const auto text = numeric_text(delta);
            double amount = 0;
            const auto result = std::from_chars(text.data(), text.data()+text.size(), amount);
            if (result.ec != std::errc{} || result.ptr != text.data()+text.size()
                || !std::isfinite(amount) || !std::isfinite(before.real_value + amount))
                invalid("real increment overflow");
            return value::real(before.real_value + amount);
        }
        Decimal change{numeric_text(delta)};
        if (kind != value_kind::decimal) (void)change.integer();
        auto sum = Decimal{numeric_text(before)}.add(change);
        if (kind == value_kind::decimal) return value::decimal(sum.str());
        const auto text = sum.integer();
        if (kind == value_kind::signed_integer) {
            int64_t number = 0;
            auto parsed = std::from_chars(text.data(), text.data()+text.size(), number);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) invalid("signed increment overflow");
            return value::signed_integer(number);
        }
        uint64_t number = 0;
        auto parsed = std::from_chars(text.data(), text.data()+text.size(), number);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) invalid("unsigned increment overflow");
        return value::unsigned_integer(number);
    }

    static void encode_value(Writer& out, const value& item) {
        out.byte(static_cast<uint8_t>(item.kind));
        switch (item.kind) {
            case value_kind::null_value: break;
            case value_kind::boolean: out.byte(item.boolean_value ? 1 : 0); break;
            case value_kind::signed_integer: out.number(std::bit_cast<uint64_t>(item.signed_value)); break;
            case value_kind::unsigned_integer: out.number(item.unsigned_value); break;
            case value_kind::real:
                if (!std::isfinite(item.real_value)) invalid("non-finite real");
                out.number(std::bit_cast<uint64_t>(item.real_value == 0 ? 0.0 : item.real_value)); break;
            case value_kind::decimal: out.text(Decimal{item.text_value}.str()); break;
            case value_kind::text: out.text(item.text_value); break;
            case value_kind::json: {
                auto parsed = caf::json_value::parse(item.text_value);
                if (!parsed || (!parsed->is_object() && !parsed->is_array()))
                    invalid("JSON field must be one object or array");
                out.text(item.text_value); break;
            }
            case value_kind::bytes:
                out.text({reinterpret_cast<const char*>(item.bytes_value.data()), item.bytes_value.size()}); break;
            default: invalid("unknown value kind");
        }
    }
    static value decode_value(Reader& in) {
        switch (static_cast<value_kind>(in.byte())) {
            case value_kind::null_value: return value::null();
            case value_kind::boolean: {
                auto b = in.byte(); if (b > 1) corrupt("invalid boolean"); return value::boolean(b != 0);
            }
            case value_kind::signed_integer: return value::signed_integer(std::bit_cast<int64_t>(in.number()));
            case value_kind::unsigned_integer: return value::unsigned_integer(in.number());
            case value_kind::real: return value::real(std::bit_cast<double>(in.number()));
            case value_kind::decimal: return value::decimal(in.text());
            case value_kind::text: return value::text(in.text());
            case value_kind::json: return value::json(in.text());
            case value_kind::bytes: {
                auto text = in.text(); std::vector<std::byte> bytes(text.size());
                std::transform(text.begin(), text.end(), bytes.begin(), [](char c) { return std::byte(static_cast<unsigned char>(c)); });
                return value::bytes(std::move(bytes));
            }
            default: corrupt("unknown stored value kind");
        }
    }
    static std::string encode(const Document& document) {
        Writer out; out.text("redis-entity-v1"); out.number(document.version);
        out.number(document.fields.size());
        for (const auto& [column, item] : document.fields) { out.text(column); encode_value(out, item); }
        return out.take();
    }
    static Document decode(std::string_view bytes) {
        Reader in{bytes}; if (in.text() != "redis-entity-v1") corrupt("unsupported entity encoding");
        Document result; result.version = in.number();
        if (!result.version || result.version > uint64_t(std::numeric_limits<int64_t>::max()))
            corrupt("invalid entity version");
        auto count = in.number(); if (count > 65536) corrupt("too many stored fields");
        for (uint64_t i = 0; i < count; ++i) {
            auto column = in.text(); auto item = decode_value(in);
            if (!result.fields.emplace(std::move(column), std::move(item)).second) corrupt("duplicate stored field");
        }
        in.finish(); return result;
    }
    static void encode_target(Writer& out, const entity_ref& target) {
        out.text(target.store); out.text(target.partition); out.text(target.entity);
        out.number(target.key.size());
        for (const auto& key : target.key) { out.text(key.name); encode_value(out, key.data); }
    }
    static std::string signature(const save_request& request) {
        Writer out; out.text("redis-request-v1"); out.number(request.changes.size());
        for (const auto& change : request.changes) {
            encode_target(out, change.target);
            out.byte(change.create_if_missing); out.byte(change.check_version);
            out.number(change.expected_version); out.number(change.fields.size());
            for (const auto& field : change.fields) {
                out.text(field.name); out.byte(static_cast<uint8_t>(field.operation)); encode_value(out, field.data);
            }
        }
        return out.take();
    }
    static std::string object_field(const schema::entity_schema& mapped, const entity_ref& target) {
        if (target.key.size() != mapped.keys.size()) invalid("entity key count mismatch");
        Writer out; out.text("object"); out.text(mapped.table);
        std::map<std::string, const named_value*> keys;
        for (const auto& key : mapped.keys) {
            auto found = std::find_if(target.key.begin(), target.key.end(),
                [&](const auto& item) { return item.name == key.name; });
            if (found == target.key.end()) invalid("missing entity key: " + key.name);
            validate_value(found->data, key, true); keys.emplace(key.column, &*found);
        }
        for (const auto& [column, item] : keys) { out.text(column); encode_value(out, item->data); }
        return out.take();
    }
    static std::string ledger_field(const std::string& request_id) {
        Writer out; out.text("request"); out.text(request_id); return out.take();
    }
    static std::string hash_key(const schema::store_schema& store) {
        return "__caf_entity_store:v1:" + store.idempotency_table;
    }
    static std::string ledger(const std::string& signature, const save_result& result) {
        Writer out; out.text("redis-commit-v1"); out.text(signature); out.number(result.entities.size());
        for (const auto& item : result.entities) out.number(item.version);
        return out.take();
    }
    static save_result replay(const save_request& request, const std::string& signature, std::string_view bytes) {
        Reader in{bytes};
        if (in.text() != "redis-commit-v1") corrupt("unsupported commit record");
        if (in.text() != signature) throw EntityError{result_code::conflict, "request_id was already used with different payload"};
        if (in.number() != request.changes.size()) corrupt("invalid commit record count");
        save_result result; result.request_id = request.request_id;
        for (const auto& change : request.changes) {
            auto version = in.number();
            if (!version || version > uint64_t(std::numeric_limits<int64_t>::max())) corrupt("invalid committed version");
            result.entities.push_back({change.target, version});
        }
        in.finish(); result.code = result_code::ok; result.committed = true; return result;
    }
};

} // namespace caf_plugin_system::entity_store::redis

