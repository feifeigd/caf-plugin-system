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
//     且天然可序列化 → 跨节点调用可直接塞 plugin_envelope（sub_proto
//     由各插件自管编码）。
//
// 事务：tx_handle = uint64（CAF 内置类型，无需注册）。v1 语义：
//   - MySQL/PG：begin 时从连接池借一条连接并标记占用（按调用方
//     actor 维度），后续带 tx_handle 的请求钉在该连接上，
//     commit/rollback 归还；
//   - Redis：MULTI/EXEC 本身就是命令流，无需显式事务状态机；
//   - MongoDB：多文档事务 v1 不支持（调用方自行承担）。
// ------------------------------------------------------------------

#include <caf/fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace caf_plugin_system::db {

/// 统一结果集。
/// - 查询：columns + rows（每行 = 每列字符串化的 cell）
/// - 写：affected（影响行数）+ insert_id（自增主键）
/// - 失败：ok=false + error（驱动错误串）
struct db_result {
    bool ok = false;
    std::string error;
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    int64_t affected = 0;
    std::string insert_id;
    int64_t duration_ms = 0;   // 执行耗时（驱动侧计时）
};

template <class Inspector>
bool inspect(Inspector& f, db_result& x) {
    return f.object(x).fields(
        f.field("ok", x.ok),
        f.field("error", x.error),
        f.field("columns", x.columns),
        f.field("rows", x.rows),
        f.field("affected", x.affected),
        f.field("insert_id", x.insert_id),
        f.field("duration_ms", x.duration_ms)
    );
}

} // namespace caf_plugin_system::db
