#pragma once

// 通用实体仓储契约。业务 actor 只看逻辑实体，不暴露 SQL、连接或
// 长生命周期事务句柄：load 支持字段投影；save 使用字段 patch，且一条
// save_request 内的全部 change 必须在同一 store/shard 原子提交。

#include <caf/default_enum_inspect.hpp>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace caf_plugin_system::entity_store {

enum class value_kind : uint8_t {
    null_value, boolean, signed_integer, unsigned_integer, real,
    decimal, text, json, bytes,
};

inline std::string to_string(value_kind x) {
    switch (x) {
        case value_kind::null_value: return "null";
        case value_kind::boolean: return "boolean";
        case value_kind::signed_integer: return "signed_integer";
        case value_kind::unsigned_integer: return "unsigned_integer";
        case value_kind::real: return "real";
        case value_kind::decimal: return "decimal";
        case value_kind::text: return "text";
        case value_kind::json: return "json";
        case value_kind::bytes: return "bytes";
    }
    return "unknown";
}

inline bool from_string(std::string_view str, value_kind& x) {
    if (str == "null") x = value_kind::null_value;
    else if (str == "boolean") x = value_kind::boolean;
    else if (str == "signed_integer") x = value_kind::signed_integer;
    else if (str == "unsigned_integer") x = value_kind::unsigned_integer;
    else if (str == "real") x = value_kind::real;
    else if (str == "decimal") x = value_kind::decimal;
    else if (str == "text") x = value_kind::text;
    else if (str == "json") x = value_kind::json;
    else if (str == "bytes") x = value_kind::bytes;
    else return false;
    return true;
}

inline bool from_integer(uint8_t raw, value_kind& x) {
    if (raw > static_cast<uint8_t>(value_kind::bytes))
        return false;
    x = static_cast<value_kind>(raw);
    return true;
}

template <class Inspector>
bool inspect(Inspector& f, value_kind& x) {
    return caf::default_enum_inspect(f, x);
}

/// 可跨数据库、跨节点传输的值。只有 kind 对应的成员有意义。
/// decimal 用字符串保存，避免货币或大数经 double 丢精度。
struct value {
    value_kind kind = value_kind::null_value;
    bool boolean_value = false;
    int64_t signed_value = 0;
    uint64_t unsigned_value = 0;
    double real_value = 0.0;
    std::string text_value;
    std::vector<std::byte> bytes_value;

    static value null() { return {}; }
    static value boolean(bool x) {
        value r; r.kind = value_kind::boolean; r.boolean_value = x; return r;
    }
    static value signed_integer(int64_t x) {
        value r; r.kind = value_kind::signed_integer; r.signed_value = x; return r;
    }
    static value unsigned_integer(uint64_t x) {
        value r; r.kind = value_kind::unsigned_integer; r.unsigned_value = x; return r;
    }
    static value real(double x) {
        value r; r.kind = value_kind::real; r.real_value = x; return r;
    }
    static value decimal(std::string x) {
        value r; r.kind = value_kind::decimal; r.text_value = std::move(x); return r;
    }
    static value text(std::string x) {
        value r; r.kind = value_kind::text; r.text_value = std::move(x); return r;
    }
    static value json(std::string x) {
        value r; r.kind = value_kind::json; r.text_value = std::move(x); return r;
    }
    static value bytes(std::vector<std::byte> x) {
        value r; r.kind = value_kind::bytes; r.bytes_value = std::move(x); return r;
    }
    bool operator==(const value&) const = default;
};

template <class Inspector>
bool inspect(Inspector& f, value& x) {
    return f.object(x).fields(
        f.field("kind", x.kind), f.field("boolean_value", x.boolean_value),
        f.field("signed_value", x.signed_value),
        f.field("unsigned_value", x.unsigned_value),
        f.field("real_value", x.real_value),
        f.field("text_value", x.text_value),
        f.field("bytes_value", x.bytes_value));
}

struct named_value {
    std::string name;
    value data;
    bool operator==(const named_value&) const = default;
};

template <class Inspector>
bool inspect(Inspector& f, named_value& x) {
    return f.object(x).fields(f.field("name", x.name), f.field("data", x.data));
}

/// 实体记录的定位信息，不包含记录数据。
/// entity 和 key 的字段名须经服务端 schema 白名单映射，不能直接拼成 SQL 标识符。
struct entity_ref {
    /// 逻辑数据源名称：选择读写连接及实体映射配置，不等于物理数据库名。
    std::string store = "default";

    /// 保存分组键：同一 store + partition 的保存请求按序处理。
    /// 一次 save_request 中所有操作必须属于同一 store 和 partition。
    /// 不作为 SQL 查询条件，也不会自动执行物理分库分表。
    std::string partition;

    /// 逻辑实体名称：通过 schema 映射到实际数据表、集合或存储对象。
    std::string entity;

    /// 主键字段及其值：支持单主键和复合主键，字段名通过 schema 映射到实际列。
    /// 用于定位记录，不能通过 entity_patch.fields 修改主键。
    std::vector<named_value> key;

    bool operator==(const entity_ref&) const = default;
};

template <class Inspector>
bool inspect(Inspector& f, entity_ref& x) {
    return f.object(x).fields(
        f.field("store", x.store), f.field("partition", x.partition),
        f.field("entity", x.entity), f.field("key", x.key));
}

/// all_fields=true 时 names 必须为空；否则 names 是明确的字段投影。
struct field_mask {
    bool all_fields = true;
    std::vector<std::string> names;
};

template <class Inspector>
bool inspect(Inspector& f, field_mask& x) {
    return f.object(x).fields(f.field("all_fields", x.all_fields),
                              f.field("names", x.names));
}

struct load_request {
    entity_ref target;
    field_mask fields;
};

template <class Inspector>
bool inspect(Inspector& f, load_request& x) {
    return f.object(x).fields(f.field("target", x.target),
                              f.field("fields", x.fields));
}

enum class result_code : uint8_t {
    ok, not_found, conflict, invalid_request, unavailable,
    commit_unknown, internal_error,
};

inline std::string to_string(result_code x) {
    switch (x) {
        case result_code::ok: return "ok";
        case result_code::not_found: return "not_found";
        case result_code::conflict: return "conflict";
        case result_code::invalid_request: return "invalid_request";
        case result_code::unavailable: return "unavailable";
        case result_code::commit_unknown: return "commit_unknown";
        case result_code::internal_error: return "internal_error";
    }
    return "unknown";
}

inline bool from_string(std::string_view str, result_code& x) {
    if (str == "ok") x = result_code::ok;
    else if (str == "not_found") x = result_code::not_found;
    else if (str == "conflict") x = result_code::conflict;
    else if (str == "invalid_request") x = result_code::invalid_request;
    else if (str == "unavailable") x = result_code::unavailable;
    else if (str == "commit_unknown") x = result_code::commit_unknown;
    else if (str == "internal_error") x = result_code::internal_error;
    else return false;
    return true;
}

inline bool from_integer(uint8_t raw, result_code& x) {
    if (raw > static_cast<uint8_t>(result_code::internal_error))
        return false;
    x = static_cast<result_code>(raw);
    return true;
}

template <class Inspector>
bool inspect(Inspector& f, result_code& x) {
    return caf::default_enum_inspect(f, x);
}

struct load_result {
    result_code code = result_code::internal_error;
    std::string error;
    entity_ref target;
    std::vector<named_value> fields;
    uint64_t version = 0;
};

template <class Inspector>
bool inspect(Inspector& f, load_result& x) {
    return f.object(x).fields(
        f.field("code", x.code), f.field("error", x.error),
        f.field("target", x.target), f.field("fields", x.fields),
        f.field("version", x.version));
}

enum class patch_op : uint8_t {
    set,       ///< 写入 data；data=null_value 表示显式 NULL。
    erase,     ///< 删除文档字段；SQL 后端映射为 NULL。
    increment, ///< 在数据库内原子加 data；只接受数字类型。
};

inline std::string to_string(patch_op x) {
    switch (x) {
        case patch_op::set: return "set";
        case patch_op::erase: return "erase";
        case patch_op::increment: return "increment";
    }
    return "unknown";
}

inline bool from_string(std::string_view str, patch_op& x) {
    if (str == "set") x = patch_op::set;
    else if (str == "erase") x = patch_op::erase;
    else if (str == "increment") x = patch_op::increment;
    else return false;
    return true;
}

inline bool from_integer(uint8_t raw, patch_op& x) {
    if (raw > static_cast<uint8_t>(patch_op::increment))
        return false;
    x = static_cast<patch_op>(raw);
    return true;
}

template <class Inspector>
bool inspect(Inspector& f, patch_op& x) {
    return caf::default_enum_inspect(f, x);
}

struct field_patch {
    patch_op operation = patch_op::set;
    std::string name;
    value data;
};

template <class Inspector>
bool inspect(Inspector& f, field_patch& x) {
    return f.object(x).fields(f.field("operation", x.operation),
                              f.field("name", x.name),
                              f.field("data", x.data));
}

// legacy_patch preserves existing callers; strict operations never upsert.
enum class entity_operation : uint8_t { legacy_patch, insert, update, delete_entity };

inline std::string to_string(entity_operation x) {
    switch (x) {
        case entity_operation::legacy_patch: return "legacy_patch";
        case entity_operation::insert: return "insert";
        case entity_operation::update: return "update";
        case entity_operation::delete_entity: return "delete_entity";
    }
    return "unknown";
}
inline bool from_string(std::string_view s, entity_operation& x) {
    for (uint8_t i = 0; i <= 3; ++i) {
        auto candidate = static_cast<entity_operation>(i);
        if (s == to_string(candidate)) { x = candidate; return true; }
    }
    return false;
}
inline bool from_integer(uint8_t raw, entity_operation& x) {
    if (raw > 3) return false;
    x = static_cast<entity_operation>(raw); return true;
}
template <class Inspector>
bool inspect(Inspector& f, entity_operation& x) {
    return caf::default_enum_inspect(f, x);
}

struct entity_patch {
    entity_ref target;
    std::vector<field_patch> fields;
    bool create_if_missing = false;
    bool check_version = false;
    uint64_t expected_version = 0;
    entity_operation operation = entity_operation::legacy_patch;
};

template <class Inspector>
bool inspect(Inspector& f, entity_patch& x) {
    return f.object(x).fields(
        f.field("target", x.target), f.field("fields", x.fields),
        f.field("create_if_missing", x.create_if_missing),
        f.field("check_version", x.check_version),
        f.field("expected_version", x.expected_version),
        f.field("operation", x.operation));
}

/// 原子保存单元。request_id 是幂等键；所有 changes 全部提交或全部回滚。
struct save_request {
    std::string request_id;
    std::vector<entity_patch> changes;
};

template <class Inspector>
bool inspect(Inspector& f, save_request& x) {
    return f.object(x).fields(f.field("request_id", x.request_id),
                              f.field("changes", x.changes));
}

struct committed_entity {
    entity_ref target;
    uint64_t version = 0; // No live version for delete_entity (zero).
    entity_operation operation = entity_operation::legacy_patch;
};

template <class Inspector>
bool inspect(Inspector& f, committed_entity& x) {
    return f.object(x).fields(f.field("target", x.target),
                              f.field("version", x.version),
                              f.field("operation", x.operation));
}

struct save_result {
    result_code code = result_code::internal_error;
    std::string error;
    std::string request_id;
    bool committed = false;
    std::vector<committed_entity> entities;
};

template <class Inspector>
bool inspect(Inspector& f, save_result& x) {
    return f.object(x).fields(
        f.field("code", x.code), f.field("error", x.error),
        f.field("request_id", x.request_id),
        f.field("committed", x.committed), f.field("entities", x.entities));
}

namespace detail {
inline std::string validate_target(const entity_ref& target) {
    if (target.store.empty())
        return "target.store is empty";
    if (target.entity.empty())
        return "target.entity is empty";
    if (target.key.empty())
        return "target.key is empty";
    std::unordered_set<std::string> names;
    for (const auto& key : target.key) {
        if (key.name.empty())
            return "target.key contains an empty field name";
        if (!names.insert(key.name).second)
            return "target.key contains duplicate field: " + key.name;
    }
    return {};
}

inline bool is_number(value_kind kind) {
    return kind == value_kind::signed_integer
           || kind == value_kind::unsigned_integer
           || kind == value_kind::real || kind == value_kind::decimal;
}
} // namespace detail

/// 返回空串表示结构合法；schema、类型、权限和大小限制仍由实现校验。
inline std::string validate(const load_request& request) {
    if (auto error = detail::validate_target(request.target); !error.empty())
        return error;
    if (request.fields.all_fields && !request.fields.names.empty())
        return "field mask cannot contain names when all_fields is true";
    if (!request.fields.all_fields && request.fields.names.empty())
        return "field mask must contain names when all_fields is false";
    std::unordered_set<std::string> names;
    for (const auto& name : request.fields.names) {
        if (name.empty())
            return "field mask contains an empty field name";
        if (!names.insert(name).second)
            return "field mask contains duplicate field: " + name;
    }
    return {};
}

inline std::string validate(const save_request& request) {
    if (request.request_id.empty())
        return "request_id is empty";
    if (request.changes.empty())
        return "changes is empty";
    
    // 当前实现要求一次 save_request 中所有操作属于同一个 store 和同一个 partition，否则校验失败。
    const auto& store = request.changes.front().target.store;
    const auto& partition = request.changes.front().target.partition;
    std::vector<const entity_patch*> seen;
    for (const auto& change : request.changes) {
        for (const auto* prior : seen) {
            if (change.operation == entity_operation::legacy_patch
                && prior->operation == entity_operation::legacy_patch) continue;
            if (prior->target.store != change.target.store
                || prior->target.entity != change.target.entity
                || prior->target.key.size() != change.target.key.size()) continue;
            const auto same_key = std::all_of(change.target.key.begin(), change.target.key.end(),
                [&](const auto& key) {
                    return std::find(prior->target.key.begin(), prior->target.key.end(), key)
                           != prior->target.key.end();
                });
            if (same_key) return "duplicate entity target in strict batch";
        }
        seen.push_back(&change);
        if (auto error = detail::validate_target(change.target); !error.empty())
            return error;
        if (change.target.store != store)
            return "all changes must use the same store";
        if (change.target.partition != partition)
            return "all changes must use the same partition";
        if (static_cast<uint8_t>(change.operation) > 3)
            return "unknown entity operation";
        if (change.operation != entity_operation::legacy_patch && change.create_if_missing)
            return "strict operation cannot use create_if_missing";
        if (change.operation != entity_operation::legacy_patch
            && change.expected_version > uint64_t(std::numeric_limits<int64_t>::max()))
            return "strict version exceeds int64";
        if (change.operation == entity_operation::insert
            && (change.check_version || change.expected_version != 0))
            return "insert cannot specify an existing version";
        if ((change.operation == entity_operation::update
             || change.operation == entity_operation::delete_entity)
            && (!change.check_version || change.expected_version == 0))
            return "strict update/delete requires a positive expected version";
        if (change.operation == entity_operation::delete_entity && !change.fields.empty())
            return "delete entity cannot contain field patches";
        if (change.fields.empty() && change.operation != entity_operation::insert
            && change.operation != entity_operation::delete_entity)
            return "entity patch has no fields";
        std::unordered_set<std::string> key_names;
        for (const auto& key : change.target.key)
            key_names.insert(key.name);
        std::unordered_set<std::string> patch_names;
        for (const auto& field : change.fields) {
            if (static_cast<uint8_t>(field.operation) > static_cast<uint8_t>(patch_op::increment))
                return "unknown field patch operation";
            if (change.operation == entity_operation::insert && field.operation != patch_op::set)
                return "strict insert requires set fields";
            if (field.name.empty())
                return "entity patch contains an empty field name";
            if (!patch_names.insert(field.name).second)
                return "entity patch contains duplicate field: " + field.name;
            if (key_names.contains(field.name))
                return "entity patch cannot modify key field: " + field.name;
            if (field.operation == patch_op::increment
                && !detail::is_number(field.data.kind))
                return "increment requires a numeric value: " + field.name;
        }
    }
    return {};
}

} // namespace caf_plugin_system::entity_store
