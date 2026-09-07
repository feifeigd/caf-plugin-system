// ------------------------------------------------------------------
// MySQL 插件（libmariadb）
//
// 与 Redis 插件同骨架（阻塞 IO 模型），SQL 特有三处：
//   1. 连接池：每个命名连接 = mysql.pool_size 条连接（= worker 数），
//      非事务请求 round-robin 分配
//   2. 参数化查询：mysql_stmt_prepare + bind（全字符串参数，
//      空串 ≠ NULL；NULL 参数 v1 不支持）
//   3. 事务状态机：tx_begin 从公共连接池借空闲槽位→ 带 tx_handle 的
//      请求钉在该连接 → commit/rollback 归还。tx_begin 响应 db_result，
//      成功时 insert_id = tx_handle（十进制字符串），调用方 stoull 后
//      用于后续 commit/rollback 消息。
//      tx_handle 编码含连接序号、槽序号和代际号：
//      路由零查找，旧句柄不能误操作后来复用同一槽的新事务。
//
// 配置（CAF 配置系统，同文件字段区分）：
//   caf-plugin-system {
//     mysql {
//       uris { main = "mysql://root:pass@127.0.0.1:3306/appdb" }
//       pool_size = 2
//     }
//   }
// uri = mysql://user:pass@host:port/dbname（各段可省略）
//
// 断线由公共 worker 分类并有界重连；已发送 SQL 不自动重放，旧事务失效。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "templates/sql_service_handlers.hpp"
#include "templates/sql_uri_config.hpp"
#include "templates/sql_reconnecting_worker.hpp"
#include "templates/sql_socket_cancellation.hpp"

// libmariadb 依赖 winsock2（同 hiredis 坑），必须最先包含
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>
#include <errmsg.h>

#include <caf/all.hpp>

#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;

namespace {

using Op = caf_plugin_system::sql_backend::Operation;
using Job = caf_plugin_system::sql_backend::Job;
using ConnSlot = caf_plugin_system::sql_backend::ConnectionSlot;
using ConnState = caf_plugin_system::sql_backend::ConnectionState;
using SqlPool = caf_plugin_system::sql_backend::ConnectionPool;
using SqlDispatcher = caf_plugin_system::sql_backend::SqlServiceDispatcher;

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.mysql.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define MYSQL_FIELDS(X, XC)                                                   \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)                                                      \
    X(int, reconnect_attempts, 3)                                            \
    X(int, reconnect_delay_ms, 100)                                          \
    X(int, connect_timeout_seconds, 2)                                       \
    X(int, io_timeout_seconds, 5)
PLUGIN_CONFIG(MYSQL_FIELDS)
#undef MYSQL_FIELDS

using SqlSpec = caf_plugin_system::sql_backend::ConnectionSpec;

std::vector<SqlSpec> parse_uris(const caf::settings& uris) {
    const caf_plugin_system::sql_backend::ConnectionUriParser parser{
        {"mysql://"}, "root", 3306};
    return parser.parse(uris);
}

db::db_result mysql_failure(MYSQL* connection, MYSQL_STMT* statement = nullptr) {
    db::db_result result;
    const auto number = statement ? mysql_stmt_errno(statement)
                                  : mysql_errno(connection);
    result.native_code = std::to_string(number);
    result.error = statement ? mysql_stmt_error(statement) : mysql_error(connection);
    const char* state = statement ? mysql_stmt_sqlstate(statement)
                                  : mysql_sqlstate(connection);
    if (state)
        result.sql_state = state;
    result.code = number == CR_SERVER_GONE_ERROR || number == CR_SERVER_LOST
                          || number == CR_SERVER_LOST_EXTENDED
                          || number == CR_CONNECTION_ERROR || number == CR_CONN_HOST_ERROR
                          || number == CR_IPSOCK_ERROR
                          || result.sql_state.rfind("08", 0) == 0
                      ? db::error_code::connection_lost
                      : db::error_code::sql_error;
    return result;
}

db::db_result mysql_cancelled() {
    db::db_result result;
    result.code = db::error_code::connection_lost;
    result.sql_state = "08006";
    result.error = "MySQL operation interrupted by shutdown";
    return result;
}
/// 文本命令（BEGIN/COMMIT/ROLLBACK）。
db::db_result exec_text(MYSQL* c, const char* cmd) {
    db::db_result r;
    if (mysql_real_query(c, cmd, static_cast<unsigned long>(std::strlen(cmd))) != 0) {
        return mysql_failure(c);
    }
    r.ok = true;
    return r;
}

/// 参数化查询/写。参数全字符串（MYSQL_TYPE_STRING）；结果集用
/// mysql_stmt_fetch_column 两遍读取（先取长度再读），正确处理任意长度列。
db::db_result stmt_execute(MYSQL* c, const std::string& sql,
                           const std::vector<std::string>& params, bool want_rows,
                           std::stop_token stop) {
    db::db_result r;
    MYSQL_STMT* st = mysql_stmt_init(c);
    if (!st) {
        r.error = "mysql_stmt_init failed";
        return r;
    }
    if (mysql_stmt_prepare(st, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        r = mysql_failure(c, st);
        mysql_stmt_close(st);
        return r;
    }
    if (mysql_stmt_param_count(st) != params.size()) {
        r.error = "SQL parameter count does not match placeholders";
        mysql_stmt_close(st);
        return r;
    }
    if (!params.empty()) {
        std::vector<MYSQL_BIND> binds(params.size());
        for (size_t i = 0; i < params.size(); ++i) {
            std::memset(&binds[i], 0, sizeof(binds[i]));
            binds[i].buffer_type = MYSQL_TYPE_STRING;
            binds[i].buffer = const_cast<char*>(params[i].data());
            // length=NULL + buffer_length：驱动按 buffer_length 处理
            //（用 length 指针实测 libmariadb 3.4.7 在 stmt execute 时报
            //  CR_OUT_OF_MEMORY——发送长度异常，弃用指针方案）
            binds[i].buffer_length = static_cast<unsigned long>(params[i].size());
        }
        if (mysql_stmt_bind_param(st, binds.data()) != 0) {
            r = mysql_failure(c, st);
            mysql_stmt_close(st);
            return r;
        }
    }
    if (mysql_stmt_execute(st) != 0) {
        r = mysql_failure(c, st);
        mysql_stmt_close(st);
        return r;
    }

    if (want_rows) {    // Op::Query
        MYSQL_RES* meta = mysql_stmt_result_metadata(st);
        if (meta) {
            unsigned ncols = mysql_num_fields(meta);
            MYSQL_FIELD* fields = mysql_fetch_fields(meta);
            r.columns.reserve(ncols);
            for (unsigned i = 0; i < ncols; ++i)
                r.columns.emplace_back(fields[i].name);

            // 绑定 result：buffer=null（fetch_column 模式，官方 pattern）
            std::vector<MYSQL_BIND> rbinds(ncols);
            std::vector<my_bool> isnull(ncols);
            std::vector<unsigned long> rlen(ncols);
            for (unsigned i = 0; i < ncols; ++i) {
                std::memset(&rbinds[i], 0, sizeof(rbinds[i]));
                rbinds[i].buffer_type = MYSQL_TYPE_STRING;
                rbinds[i].length = &rlen[i];
                rbinds[i].is_null = &isnull[i];
            }
            if (mysql_stmt_bind_result(st, rbinds.data()) != 0) {
                r = mysql_failure(c, st);
                mysql_free_result(meta);
                mysql_stmt_close(st);
                return r;
            }
            for (;;) {
                // Socket shutdown wakes blocked fetches. This also handles
                // rows already buffered in the driver without another read.
                if (stop.stop_requested()) {
                    mysql_free_result(meta);
                    mysql_stmt_close(st);
                    return mysql_cancelled();
                }
                const auto fetched = mysql_stmt_fetch(st);
                if (fetched == MYSQL_NO_DATA)
                    break;
                // Length-only bindings intentionally report truncated data.
                // The complete values are read with fetch_column below.
                if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED) {
                    r = mysql_failure(c, st);
                    mysql_free_result(meta);
                    mysql_stmt_close(st);
                    return r;
                }
                std::vector<std::string> row;
                std::vector<uint8_t> nulls;
                row.reserve(ncols);
                nulls.reserve(ncols);
                for (unsigned i = 0; i < ncols; ++i) {
                    if (isnull[i]) {
                        row.emplace_back();
                        nulls.push_back(1u);
                        continue;
                    }
                    nulls.push_back(0u);
                    // fetch has already populated the full length and NULL bit.
                    MYSQL_BIND col = rbinds[i];
                    std::string cell(rlen[i], '\0');
                    col.buffer = cell.data();
                    col.buffer_length = static_cast<unsigned long>(cell.size());
                    if (!cell.empty() && mysql_stmt_fetch_column(st, &col, i, 0) != 0) {
                        r = mysql_failure(c, st);
                        mysql_free_result(meta);
                        mysql_stmt_close(st);
                        return r;
                    }
                    row.push_back(std::move(cell));
                }
                r.rows.push_back(std::move(row));
                r.nulls.push_back(std::move(nulls));
            }
            mysql_free_result(meta);
        }
        r.ok = true;
    } else { // Op::Exec
        r.affected = static_cast<int64_t>(mysql_stmt_affected_rows(st));
        unsigned long long id = mysql_insert_id(c);
        if (id != 0)
            r.insert_id = std::to_string(id);
        r.ok = true;
    }
    mysql_stmt_close(st);
    return r;
}

using ReconnectPolicy = caf_plugin_system::sql_backend::ReconnectPolicy;

class MySqlConnection final : public caf_plugin_system::sql_backend::SqlConnection {
public:
    MySqlConnection(SqlSpec spec, std::shared_ptr<ConnState> slot, ReconnectPolicy policy)
        : spec_(std::move(spec)), slot_(std::move(slot)), policy_(policy) {}
    ~MySqlConnection() override { close(); }

    db::db_result connect() override {
        close();
        handle_ = mysql_init(nullptr);
        if (!handle_) {
            db::db_result result;
            result.code = db::error_code::sql_error;
            result.error = "mysql_init failed";
            return result;
        }
        const my_bool automatic_reconnect = 0;
        // Never let the driver silently replace a session inside a transaction.
        if (mysql_options(handle_, MYSQL_OPT_RECONNECT, &automatic_reconnect) != 0
            || mysql_options(handle_, MYSQL_OPT_CONNECT_TIMEOUT,
                             &policy_.connect_timeout_seconds) != 0
            || mysql_options(handle_, MYSQL_OPT_READ_TIMEOUT,
                             &policy_.io_timeout_seconds) != 0
            || mysql_options(handle_, MYSQL_OPT_WRITE_TIMEOUT,
                             &policy_.io_timeout_seconds) != 0
            || mysql_options(handle_, MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0)
            return mysql_failure(handle_);
        if (!mysql_real_connect(handle_, spec_.host.c_str(), spec_.user.c_str(),
                                spec_.pass.c_str(),
                                spec_.dbname.empty() ? nullptr : spec_.dbname.c_str(),
                                spec_.port, nullptr, 0)) {
            auto result = mysql_failure(handle_);
            if (db::is_connection_error(result.code))
                result.code = db::error_code::connection_unavailable;
            LOG_WARN("MySQL [{}] connection unavailable: {}", spec_.name, result.error);
            return result;
        }
        // Register only after connect has returned on the owning worker. A
        // concurrently requested stop is delivered immediately on registration.
        cancellation_ = std::make_unique<caf_plugin_system::sql_backend::SocketCancellation>(
            mysql_get_socket(handle_), slot_->stop_token());
        if (!cancellation_->valid()) {
            db::db_result result;
            result.code = db::error_code::connection_unavailable;
            result.error = "cannot create MySQL I/O cancellation handle";
            return result;
        }
        if (slot_->stopped())
            return mysql_cancelled();
        LOG_INFO("MySQL [{}] connected: {}:{}/{} user={}", spec_.name,
                 spec_.host, spec_.port, spec_.dbname, spec_.user);
        db::db_result result;
        result.ok = true;
        return result;
    }

    void close() noexcept override {
        // Unregister/wait for the stop callback and close the duplicate first.
        // mysql_close and statement cleanup run only on this worker thread.
        cancellation_.reset();
        if (handle_) {
            mysql_close(handle_);
            handle_ = nullptr;
        }
    }

    db::db_result execute(const Job& job) override {
        if (slot_->stopped())
            return mysql_cancelled();
        db::db_result result;
        switch (job.op) {
            case Op::Begin: result = exec_text(handle_, "BEGIN"); break;
            case Op::Commit: result = exec_text(handle_, "COMMIT"); break;
            case Op::Rollback: result = exec_text(handle_, "ROLLBACK"); break;
            case Op::Query:
                result = stmt_execute(handle_, job.sql, job.params, true, slot_->stop_token());
                break;
            case Op::Exec:
                result = stmt_execute(handle_, job.sql, job.params, false, slot_->stop_token());
                break;
        }
        // Do not report partial rows or a racing driver success after cancel.
        // The common worker preserves outcome_unknown for Exec/COMMIT.
        return slot_->stopped() ? mysql_cancelled() : result;
    }

private:
    SqlSpec spec_;
    std::shared_ptr<ConnState> slot_;
    ReconnectPolicy policy_;
    MYSQL* handle_ = nullptr;
    std::unique_ptr<caf_plugin_system::sql_backend::SocketCancellation> cancellation_;
};

void sql_worker_main(std::shared_ptr<ConnState> slot, const SqlSpec& spec,
                     ReconnectPolicy policy) {
    caf_plugin_system::sql_backend::ReconnectingSqlWorker worker{
        slot, std::make_unique<MySqlConnection>(spec, slot, policy), policy};
    worker.run();
    LOG_INFO("MySQL [{}] worker exited", spec.name);
}


} // namespace

class MySqlPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"MySqlPlugin", "1.0.0",
                 {},
                {"mysql_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // 声明式配置读取（PLUGIN_CONFIG）：字段、默认值、解析全部自动
        auto cfg = load_plugin_config(sys.config());
        caf::settings uris = cfg.uris;
        int pool_size = cfg.pool_size < 1 ? 1 : cfg.pool_size;
        ReconnectPolicy reconnect;
        reconnect.max_attempts = static_cast<unsigned>(std::clamp(cfg.reconnect_attempts, 1, 10));
        reconnect.initial_delay = std::chrono::milliseconds{std::clamp(cfg.reconnect_delay_ms, 1, 1000)};
        reconnect.connect_timeout_seconds = static_cast<unsigned>(std::clamp(cfg.connect_timeout_seconds, 1, 10));
        reconnect.io_timeout_seconds = static_cast<unsigned>(std::clamp(cfg.io_timeout_seconds, 1, 30));

        return sys.spawn([logger, uris, pool_size, reconnect](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<SqlSpec>>(parse_uris(uris));
            // 池表：name → slots（按 specs 顺序构建，槽序号 = specs 内索引）
            auto pools = std::make_shared<SqlPool>("mysql");
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto default_conn = std::make_shared<std::string>("default");

            auto launch_workers = [=] {
                if (started->exchange(true))
                    return;
                if (!specs->empty())
                    *default_conn = specs->front().name;
                for (const auto& s : *specs) {
                    for (int i = 0; i < pool_size; ++i) {
                        auto slot = pools->add_slot(s.name);
                        slot->start_worker(sql_worker_main, s, reconnect);
                    }
                    LOG_INFO_SELF(self, "pool launched: [{}] {}:{}/{} size={}",
                                  s.name, s.host, s.port, s.dbname, pool_size);
                }
            };

            // 自检：默认连接上 SELECT 1 + 建表写读（串行链，顺序确定）
            auto selfcheck = [=] {
                auto sc = [self](const std::string& sql, const std::vector<std::string>& params,
                                 std::function<void()> next) {
                    // 注意：request 首参必须显式 actor 句柄（裸指针推导失败）
                    self->request(caf::actor{self}, std::chrono::seconds(5), sql_query_atom_v,
                                  sql, params)
                        .then(
                            [self, sql, next](const db::db_result& r) {
                                LOG_INFO_SELF(self, "selfcheck {} -> ok={} err={} rows={}",
                                              sql, r.ok, r.error, r.rows.size());
                                if (next)
                                    next();
                            },
                            [self, sql, next](caf::error& e) {
                                LOG_ERROR_SELF(self, "selfcheck {} failed: {}", sql,
                                               caf::to_string(e));
                                if (next)
                                    next();
                            });
                };
                // 串行链：CREATE → INSERT → SELECT（建表写读一轮）
                sc("SELECT 1", {}, [=] {
                    sc("CREATE TABLE IF NOT EXISTS hermes_selfcheck (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64))", {}, [=] {
                        sc("INSERT INTO hermes_selfcheck (v) VALUES (?)", {"db-ping"}, [=] {
                            sc("SELECT id, v FROM hermes_selfcheck ORDER BY id DESC LIMIT 1", {}, nullptr);
                        });
                    });
                });
            };

            auto dispatcher = SqlDispatcher::create(
                self, pools, default_conn);
            auto business = dispatcher->handlers();
            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "MySqlPlugin initialized, conns={}", uris.size());
                    launch_workers();
                    selfcheck();
                },
                // 无业务状态：连接配置归 CAF 配置文件管
                .on_save = []() -> std::vector<std::byte> {
                    return {};
                },
                .on_shutdown = [=]() {
                    auto joined = pools->stop_and_join();
                    std::cout << "[MySqlPlugin] shutdown hook: " << joined
                              << " workers joined" << std::endl;
                    LOG_INFO_SELF(self, "MySqlPlugin shutdown, {} workers joined", joined);
                    // libmariadb 全局清理：释放 mysql_init 的内部缓存（否则 5 块泄露）
                    mysql_library_end();
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new MySqlPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
