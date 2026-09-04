// ------------------------------------------------------------------
// PostgreSQL 插件（libpq，Phase 2）
//
// 与 MySQL 插件同骨架（连接池 + 参数化 + 事务），差异仅在驱动 API：
//   - 连接：PQconnectdb（conninfo 文本）
//   - 参数化：PQexecParams（文本格式参数，paramValues 数组）
//   - 结果集：PQgetvalue 天生字符串（NULL → 空串，与 db_result 的
//     cell 字符串化模型零转换）
//   - 事务：PQexec("BEGIN"/"COMMIT"/"ROLLBACK")
//
// 配置（CAF 配置系统，同文件字段区分）：
//   caf-plugin-system {
//     pg-uris = "main=postgres://postgres:pass@127.0.0.1:5432/appdb"
//     db-pool-size = 2
//   }
// uri = postgres://user:pass@host:port/dbname（各段可省略）
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "common/plugin_envelope.hpp"
#include "templates/sql_service_handlers.hpp"
#include "templates/sql_uri_config.hpp"
#include "templates/sql_reconnecting_worker.hpp"

// libpq 头（Windows 上同样依赖 winsock2 先行，统一模式）
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/select.h>
#include <cerrno>
#endif
#include <libpq-fe.h>

#include <caf/all.hpp>

#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;

namespace {

using Op = caf_plugin_system::sql_backend::Operation;
using Job = caf_plugin_system::sql_backend::Job;
using ConnSlot = caf_plugin_system::sql_backend::ConnectionSlot;
using SqlPool = caf_plugin_system::sql_backend::ConnectionPool;
using SqlDispatcher = caf_plugin_system::sql_backend::SqlServiceDispatcher;

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.postgres.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define PG_FIELDS(X, XC)                                                      \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)                                                      \
    X(int, reconnect_attempts, 3)                                            \
    X(int, reconnect_delay_ms, 100)                                          \
    X(int, connect_timeout_seconds, 2)                                       \
    X(int, io_timeout_seconds, 5)
PLUGIN_CONFIG(PG_FIELDS)
#undef PG_FIELDS

using PgSpec = caf_plugin_system::sql_backend::ConnectionSpec;

std::vector<PgSpec> parse_uris(const caf::settings& uris) {
    const caf_plugin_system::sql_backend::ConnectionUriParser parser{
        {"postgres://", "postgresql://"}, "postgres", 5432};
    return parser.parse(uris);
}
db::db_result pg_failure(PGconn* connection, PGresult* native = nullptr) {
    db::db_result result;
    const auto* state = native ? PQresultErrorField(native, PG_DIAG_SQLSTATE) : nullptr;
    if (state) {
        result.sql_state = state;
        result.native_code = state;
    }
    result.error = native ? PQresultErrorMessage(native) : PQerrorMessage(connection);
    const auto& sql_state = result.sql_state;
    result.code = PQstatus(connection) != CONNECTION_OK
                          || sql_state.rfind("08", 0) == 0
                          || sql_state == "57P01" || sql_state == "57P02"
                          || sql_state == "57P03" || sql_state == "57P05"
                          || sql_state == "25P03"
                      ? db::error_code::connection_lost
                      : db::error_code::sql_error;
    return result;
}

// libpq's blocking calls have no client-side query timeout. Poll in short
// intervals instead, so broken networks and shutdown cannot pin a worker forever.
bool pg_wait(PGconn* connection, bool read,
             std::chrono::steady_clock::time_point deadline,
             const std::shared_ptr<ConnSlot>& slot, db::db_result& error) {
    for (;;) {
        if (slot->stopped() || std::chrono::steady_clock::now() >= deadline) {
            error.code = db::error_code::connection_lost;
            error.sql_state = "08006";
            error.error = slot->stopped() ? "PostgreSQL I/O interrupted by shutdown"
                                         : "PostgreSQL I/O deadline expired";
            return false;
        }
        const auto socket = PQsocket(connection);
        if (socket < 0) {
            error = pg_failure(connection);
            error.code = db::error_code::connection_lost;
            return false;
        }
        fd_set sockets;
        FD_ZERO(&sockets);
        FD_SET(socket, &sockets);
        timeval interval{0, 50000};
        const int ready = select(socket + 1, read ? &sockets : nullptr,
                                 read ? nullptr : &sockets, nullptr, &interval);
        if (ready > 0)
            return true;
        if (ready < 0) {
#ifndef _WIN32
            if (errno == EINTR)
                continue;
#endif
            error.code = db::error_code::connection_lost;
            error.sql_state = "08006";
            error.error = "PostgreSQL socket wait failed";
            return false;
        }
    }
}

PGresult* pg_exchange(PGconn* connection, const char* sql, int count,
                       const char* const* values,
                       const std::shared_ptr<ConnSlot>& slot, unsigned timeout,
                       db::db_result& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeout};
    if (PQsendQueryParams(connection, sql, count, nullptr, values,
                          nullptr, nullptr, 0) == 0) {
        error = pg_failure(connection);
        return nullptr;
    }
    for (;;) {
        const auto flushed = PQflush(connection);
        if (flushed == 0)
            break;
        if (flushed < 0) {
            error = pg_failure(connection);
            return nullptr;
        }
        if (!pg_wait(connection, false, deadline, slot, error))
            return nullptr;
    }
    std::unique_ptr<PGresult, decltype(&PQclear)> result{nullptr, &PQclear};
    for (;;) {
        if (slot->stopped() || std::chrono::steady_clock::now() >= deadline) {
            error.code = db::error_code::connection_lost;
            error.sql_state = "08006";
            error.error = "PostgreSQL result drain interrupted or timed out";
            return nullptr;
        }
        while (PQisBusy(connection)) {
            if (!pg_wait(connection, true, deadline, slot, error))
                return nullptr;
            if (PQconsumeInput(connection) == 0) {
                error = pg_failure(connection);
                return nullptr;
            }
        }
        auto* next = PQgetResult(connection);
        if (!next)
            break;
        result.reset(next);
        const auto status = PQresultStatus(next);
        if (status == PGRES_COPY_IN || status == PGRES_COPY_OUT
            || status == PGRES_COPY_BOTH) {
            // COPY has a different wire-protocol state. The SQL service does
            // not expose that protocol; ordinary result draining cannot exit
            // it. Force a fresh session instead of hanging this worker.
            error.code = db::error_code::connection_lost;
            error.error = "PostgreSQL COPY streaming is unsupported by the SQL service";
            return nullptr;
        }
    }
    if (!result)
        error = pg_failure(connection);
    return result.release();
}

/// 文本命令（BEGIN/COMMIT/ROLLBACK）。
db::db_result exec_text(PGconn* c, const char* cmd,
                        const std::shared_ptr<ConnSlot>& slot, unsigned timeout) {
    db::db_result r;
    PGresult* res = pg_exchange(c, cmd, 0, nullptr, slot, timeout, r);
    if (!res)
        return r;
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r = pg_failure(c, res);
    } else if (std::string_view{cmd} == "COMMIT"
               && std::string_view{PQcmdStatus(res)} != "COMMIT") {
        // PostgreSQL accepts COMMIT in an aborted transaction but reports
        // ROLLBACK. Do not tell callers that their writes were committed.
        r.error = "transaction was rolled back instead of committed";
    } else {
        r.ok = true;
    }
    PQclear(res);
    return r;
}

/// 参数化查询/写。结果集天然字符串（PQgetvalue），NULL → 空串。
db::db_result stmt_execute(PGconn* c, const std::string& sql,
                           const std::vector<std::string>& params, bool want_rows,
                           const std::shared_ptr<ConnSlot>& slot, unsigned timeout) {
    db::db_result r;
    // libpq text parameters are NUL-terminated; paramLengths is ignored for
    // text format. PostgreSQL text cannot contain NUL, so reject instead of
    // silently truncating values (especially entity keys).
    if (sql.find('\0') != std::string::npos) {
        r.error = "PostgreSQL SQL text cannot contain NUL bytes";
        return r;
    }
    std::vector<const char*> pvalues;
    pvalues.reserve(params.size());
    for (const auto& p : params) {
        if (p.find('\0') != std::string::npos) {
            r.error = "PostgreSQL text parameters cannot contain NUL bytes";
            return r;
        }
        pvalues.push_back(p.c_str());
    }

    PGresult* res = pg_exchange(c, sql.c_str(), static_cast<int>(params.size()),
                                pvalues.empty() ? nullptr : pvalues.data(),
                                slot, timeout, r);
    if (!res)
        return r;
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        r = pg_failure(c, res);
        PQclear(res);
        return r;
    }
    if (want_rows && st == PGRES_TUPLES_OK) {
        int ncols = PQnfields(res);
        int nrows = PQntuples(res);
        r.columns.reserve(ncols);
        for (int i = 0; i < ncols; ++i)
            r.columns.emplace_back(PQfname(res, i));
        r.rows.reserve(nrows);
        for (int row = 0; row < nrows; ++row) {
            std::vector<std::string> rrow;
            std::vector<uint8_t> nulls;
            rrow.reserve(ncols);
            nulls.reserve(ncols);
            for (int i = 0; i < ncols; ++i) {
                const auto is_null = PQgetisnull(res, row, i) != 0;
                char* v = PQgetvalue(res, row, i);
                rrow.emplace_back(v ? v : "");
                nulls.push_back(is_null ? 1u : 0u);
            }
            r.rows.push_back(std::move(rrow));
            r.nulls.push_back(std::move(nulls));
        }
        r.ok = true;
    } else {
        // 写操作
        if (st == PGRES_TUPLES_OK) {
            // 带 RETURNING 的写：也产出结果集
            int ncols = PQnfields(res);
            int nrows = PQntuples(res);
            r.columns.reserve(ncols);
            for (int i = 0; i < ncols; ++i)
                r.columns.emplace_back(PQfname(res, i));
            for (int row = 0; row < nrows; ++row) {
                std::vector<std::string> rrow;
                std::vector<uint8_t> nulls;
                rrow.reserve(ncols);
                nulls.reserve(ncols);
                for (int i = 0; i < ncols; ++i) {
                    const auto is_null = PQgetisnull(res, row, i) != 0;
                    char* v = PQgetvalue(res, row, i);
                    rrow.emplace_back(v ? v : "");
                    nulls.push_back(is_null ? 1u : 0u);
                }
                r.rows.push_back(std::move(rrow));
                r.nulls.push_back(std::move(nulls));
            }
        }
        char* affected = PQcmdTuples(res);
        if (affected && *affected)
            r.affected = std::atoll(affected);
        r.ok = true;
    }
    PQclear(res);
    return r;
}

using ReconnectPolicy = caf_plugin_system::sql_backend::ReconnectPolicy;

class PostgresConnection final : public caf_plugin_system::sql_backend::SqlConnection {
public:
    PostgresConnection(PgSpec spec, std::shared_ptr<ConnSlot> slot, ReconnectPolicy policy)
        : spec_(std::move(spec)), slot_(std::move(slot)), policy_(policy) {}
    ~PostgresConnection() override { close(); }

    db::db_result connect() override {
        close();
        const auto started = std::chrono::steady_clock::now();
        const auto port = std::to_string(spec_.port);
        const auto timeout = std::to_string(policy_.connect_timeout_seconds);
        const char* keywords[] = {"host", "port", "user", "password", "dbname",
                                  "connect_timeout", "client_encoding", nullptr};
        const char* values[] = {spec_.host.c_str(), port.c_str(), spec_.user.c_str(),
                                spec_.pass.c_str(), spec_.dbname.c_str(),
                                timeout.c_str(), "UTF8", nullptr};
        handle_ = PQconnectStartParams(keywords, values, 0);
        auto poll_status = PGRES_POLLING_WRITING;
        auto report_failure = [&](db::db_result error) {
            const char* poll_name = "UNKNOWN";
            switch (poll_status) {
                case PGRES_POLLING_READING: poll_name = "READING"; break;
                case PGRES_POLLING_WRITING: poll_name = "WRITING"; break;
                case PGRES_POLLING_FAILED: poll_name = "FAILED"; break;
                case PGRES_POLLING_OK: poll_name = "OK"; break;
                case PGRES_POLLING_ACTIVE: poll_name = "ACTIVE"; break;
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            error.code = db::error_code::connection_unavailable;
            error.error += " [poll=" + std::string{poll_name}
                           + ", PQstatus="
                           + std::to_string(handle_ ? static_cast<int>(PQstatus(handle_)) : -1)
                           + ", elapsed_ms=" + std::to_string(elapsed) + "]";
            // Diagnostics intentionally omit connection strings and credentials.
            LOG_WARN("Postgres [{}] connection unavailable: {}", spec_.name, error.error);
            return error;
        };
        if (!handle_) {
            db::db_result error;
            error.error = "PQconnectStartParams failed";
            poll_status = PGRES_POLLING_FAILED;
            return report_failure(std::move(error));
        }
        if (PQstatus(handle_) == CONNECTION_BAD) {
            poll_status = PGRES_POLLING_FAILED;
            return report_failure(pg_failure(handle_));
        }
        const auto deadline = std::chrono::steady_clock::now()
                              + std::chrono::seconds{policy_.connect_timeout_seconds};
        db::db_result result;
        // libpq requires the initial poll to wait as though the previous
        // result were WRITING. Polling an in-progress TCP connect early can
        // advance its state before the socket has actually become writable.
        if (!pg_wait(handle_, false, deadline, slot_, result))
            return report_failure(std::move(result));
        for (;;) {
            if (slot_->stopped() || std::chrono::steady_clock::now() >= deadline) {
                result.error = "PostgreSQL connection attempt interrupted or timed out";
                return report_failure(std::move(result));
            }
            poll_status = PQconnectPoll(handle_);
            if (poll_status == PGRES_POLLING_OK)
                break;
            if (poll_status == PGRES_POLLING_FAILED)
                return report_failure(pg_failure(handle_));
            if (poll_status == PGRES_POLLING_ACTIVE)
                continue;
            if (!pg_wait(handle_, poll_status == PGRES_POLLING_READING, deadline, slot_, result))
                return report_failure(std::move(result));
        }
        if (PQsetnonblocking(handle_, 1) != 0)
            return report_failure(pg_failure(handle_));
        LOG_INFO("Postgres [{}] connected: {}:{}/{} user={}", spec_.name,
                 spec_.host, spec_.port, spec_.dbname, spec_.user);
        result.ok = true;
        return result;
    }

    void close() noexcept override {
        if (handle_) {
            PQfinish(handle_);
            handle_ = nullptr;
        }
    }

    db::db_result execute(const Job& job) override {
        const auto timeout = policy_.io_timeout_seconds;
        switch (job.op) {
            case Op::Begin: return exec_text(handle_, "BEGIN", slot_, timeout);
            case Op::Commit: return exec_text(handle_, "COMMIT", slot_, timeout);
            case Op::Rollback: return exec_text(handle_, "ROLLBACK", slot_, timeout);
            case Op::Query: return stmt_execute(handle_, job.sql, job.params, true, slot_, timeout);
            case Op::Exec: return stmt_execute(handle_, job.sql, job.params, false, slot_, timeout);
        }
        return {};
    }

private:
    PgSpec spec_;
    std::shared_ptr<ConnSlot> slot_;
    ReconnectPolicy policy_;
    PGconn* handle_ = nullptr;
};

void pg_worker_main(const PgSpec& spec, std::shared_ptr<ConnSlot> slot,
                    ReconnectPolicy policy) {
    caf_plugin_system::sql_backend::ReconnectingSqlWorker worker{
        slot, std::make_unique<PostgresConnection>(spec, slot, policy), policy};
    worker.run();
    LOG_INFO("Postgres [{}] worker exited", spec.name);
}


} // namespace

class PostgresPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"PostgresPlugin", "1.0.0",
                 {},
                {"pg_service"}, 0, {}};
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

        return sys.spawn([logger, uris, pool_size, reconnect, this](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<PgSpec>>(parse_uris(uris));
            auto pools = std::make_shared<SqlPool>("postgres");
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
                        slot->start_worker(pg_worker_main, s, slot, reconnect);
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
                    sc("CREATE TABLE IF NOT EXISTS hermes_selfcheck (id SERIAL PRIMARY KEY, v VARCHAR(64))", {}, [=] {
                        sc("INSERT INTO hermes_selfcheck (v) VALUES ($1)", {"db-ping"}, [=] {
                            sc("SELECT id, v FROM hermes_selfcheck ORDER BY id DESC LIMIT 1", {}, nullptr);
                        });
                    });
                });
            };

            caf::message_handler plugin_handlers{
                // 跨节点信封入口（RemoteCaller 直接把 plugin_envelope 发给
                // 目标服务，见 remote_caller.cpp do_call）。
                [=](plugin_envelope env) -> caf::result<std::string> {
                    if (env.function == "hello") {
                        auto in = plugin_wire::decode_text(env);
                        if (!in)
                            return caf::make_error(caf::sec::invalid_argument,
                                                   "pg_service: unsupported payload format");
                        return std::string("pg:hello:") + *in;
                    }
                    return caf::make_error(
                        caf::sec::invalid_argument,
                        "pg_service: unknown function: " + env.function);
                },
            };
            auto dispatcher = SqlDispatcher::create(
                self, pools, default_conn);
            auto business = dispatcher->handlers().or_else(plugin_handlers);
            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=, this](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "PostgresPlugin initialized, conns={}", uris.size());
                    // 可选资源读取（asset_dir 注入机制）：读插件目录下
                    // resource.json（部署时与 DLL 同目录，见 docs/plugin-assets.md）。
                    // 文件不存在则跳过——资源对插件始终是可选的。
                    auto rp = asset_path("resource.json");
                    std::ifstream f(rp);
                    if (f) {
                        std::string line;
                        std::getline(f, line);
                        LOG_INFO_SELF(self, "PostgresPlugin asset {} -> {}", rp, line);
                    }
                    launch_workers();
                    selfcheck();
                },
                .on_save = []() -> std::vector<std::byte> {
                    return {};
                },
                .on_shutdown = [=]() {
                    auto joined = pools->stop_and_join();
                    std::cout << "[PostgresPlugin] shutdown hook: " << joined
                              << " workers joined" << std::endl;
                    LOG_INFO_SELF(self, "PostgresPlugin shutdown, {} workers joined", joined);
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new PostgresPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
