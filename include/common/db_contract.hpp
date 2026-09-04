#pragma once
// ------------------------------------------------------------------
// 数据库插件统一契约（Redis / MySQL / PostgreSQL / MongoDB 共用）
//
// 与集群协议同级的【内核级协议】：
//   - 类型注册进 message_tags.def（ID 269~276），exe 与全部插件 DLL
//     编译同一份头，ID 跨 DLL 永远一致；
//   - 四个插件提供不同服务名（redis_service / mysql_service /
//     pg_service / mongo_service），消息类型统一，调用方代码一次写通；
//   - db_result 的 cell 全字符串化（JDBC 风格）：避免类型注册地狱，
//     且天然可序列化 → 跨节点调用可直接塞 plugin_envelope（function
//     由各插件自管命名）。
//
// SQL 消息形态：
//   - 普通请求：(sql, params) / (conn, sql, params)
//   - 事务请求：(tx_handle, sql, params)
//
// 事务：tx_handle = uint64（CAF 内置类型，无需注册）。v1 语义：
//   - SQLite/MySQL/PG：begin 时从连接池借一条连接并标记占用，
//     后续带 tx_handle 的 query/exec 钉在该连接上，commit/rollback
//     归还；handle 是短生命周期不透明令牌，不应交给业务层持久化；
//   - Redis：MULTI/EXEC 本身就是命令流，无需显式事务状态机；
//   - MongoDB：多文档事务 v1 不支持（调用方自行承担）。
// ------------------------------------------------------------------

#include <caf/default_enum_inspect.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace caf_plugin_system::db {

// Classify recovery decisions without parsing localized driver messages.
// A retryable error never authorizes replay of an individual SQL statement.
enum class error_code : uint8_t {
    none, sql_error, connection_unavailable, connection_lost,
    transaction_lost, outcome_unknown,
};

inline std::string to_string(error_code code) {
    switch (code) {
        case error_code::none: return "none";
        case error_code::sql_error: return "sql_error";
        case error_code::connection_unavailable: return "connection_unavailable";
        case error_code::connection_lost: return "connection_lost";
        case error_code::transaction_lost: return "transaction_lost";
        case error_code::outcome_unknown: return "outcome_unknown";
    }
    return "unknown";
}

inline bool from_integer(uint8_t raw, error_code& code) {
    if (raw > static_cast<uint8_t>(error_code::outcome_unknown))
        return false;
    code = static_cast<error_code>(raw);
    return true;
}

inline bool from_string(std::string_view text, error_code& code) {
    for (uint8_t raw = 0; raw <= static_cast<uint8_t>(error_code::outcome_unknown); ++raw) {
        auto candidate = static_cast<error_code>(raw);
        if (text == to_string(candidate)) {
            code = candidate;
            return true;
        }
    }
    return false;
}

template <class Inspector>
bool inspect(Inspector& f, error_code& code) {
    return caf::default_enum_inspect(f, code);
}

inline bool is_connection_error(error_code code) noexcept {
    return code == error_code::connection_unavailable
           || code == error_code::connection_lost
           || code == error_code::transaction_lost
           || code == error_code::outcome_unknown;
}

/// 统一结果集。
/// - 查询：columns + rows（每行 = 每列字符串化的 cell）
/// - 写：affected（影响行数）+ insert_id（自增主键）
/// - 失败：ok=false + error（驱动错误串）
struct db_result {
    bool ok = false;
    std::string error;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    // 与 rows 同形状：1 表示 SQL NULL，0 表示普通值。保留 rows 的字符串
    // 契约，同时让上层区分 NULL 与空字符串；旧生产者不填时按非 NULL。
    std::vector<std::vector<uint8_t>> nulls;
    int64_t affected = 0;
    std::string insert_id;
    int64_t duration_ms = 0;   // 执行耗时（驱动侧计时）
    error_code code = error_code::none;
    std::string native_code;
    std::string sql_state;

    bool is_null(size_t row, size_t column) const noexcept {
        return row < nulls.size() && column < nulls[row].size()
               && nulls[row][column] != 0;
    }
};

template <class Inspector>
bool inspect(Inspector& f, db_result& x) {
    return f.object(x).fields(
        f.field("ok", x.ok),
        f.field("error", x.error),
        f.field("columns", x.columns),
        f.field("rows", x.rows),
        f.field("nulls", x.nulls),
        f.field("affected", x.affected),
        f.field("insert_id", x.insert_id),
        f.field("duration_ms", x.duration_ms),
        f.field("code", x.code),
        f.field("native_code", x.native_code),
        f.field("sql_state", x.sql_state)
    );
}

} // namespace caf_plugin_system::db
