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

// libpq 头（Windows 上同样依赖 winsock2 先行，统一模式）
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <libpq-fe.h>

#include <caf/all.hpp>

#include <atomic>
#include <fstream>
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
using SqlPool = caf_plugin_system::sql_backend::ConnectionPool;
using SqlDispatcher = caf_plugin_system::sql_backend::SqlServiceDispatcher;

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.postgres.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define PG_FIELDS(X, XC)                                                      \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)
PLUGIN_CONFIG(PG_FIELDS)
#undef PG_FIELDS

using PgSpec = caf_plugin_system::sql_backend::ConnectionSpec;

std::vector<PgSpec> parse_uris(const caf::settings& uris) {
    const caf_plugin_system::sql_backend::ConnectionUriParser parser{
        {"postgres://", "postgresql://"}, "postgres", 5432};
    return parser.parse(uris);
}
/// 拼 conninfo 文本（host/port/user/password/dbname 各段）。
std::string make_conninfo(const PgSpec& s) {
    std::string ci = "host=" + s.host + " port=" + std::to_string(s.port)
                     + " user=" + s.user;
    if (!s.pass.empty())
        ci += " password=" + s.pass;
    if (!s.dbname.empty())
        ci += " dbname=" + s.dbname;
    return ci;
}

/// 文本命令（BEGIN/COMMIT/ROLLBACK）。
db::db_result exec_text(PGconn* c, const char* cmd) {
    db::db_result r;
    PGresult* res = PQexec(c, cmd);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r.ok = false;
        r.error = PQerrorMessage(c);
    } else {
        r.ok = true;
    }
    PQclear(res);
    return r;
}

/// 参数化查询/写。结果集天然字符串（PQgetvalue），NULL → 空串。
db::db_result stmt_execute(PGconn* c, const std::string& sql,
                           const std::vector<std::string>& params, bool want_rows) {
    db::db_result r;
    std::vector<const char*> pvalues;
    pvalues.reserve(params.size());
    for (const auto& p : params)
        pvalues.push_back(p.c_str());

    PGresult* res = PQexecParams(c, sql.c_str(), static_cast<int>(params.size()),
                                 nullptr, pvalues.empty() ? nullptr : pvalues.data(),
                                 nullptr, nullptr, 0);
    ExecStatusType st = PQresultStatus(res);
    if (st != PGRES_TUPLES_OK && st != PGRES_COMMAND_OK) {
        r.ok = false;
        r.error = PQerrorMessage(c);
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
            rrow.reserve(ncols);
            for (int i = 0; i < ncols; ++i) {
                char* v = PQgetvalue(res, row, i);
                rrow.emplace_back(v ? v : "");
            }
            r.rows.push_back(std::move(rrow));
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
                rrow.reserve(ncols);
                for (int i = 0; i < ncols; ++i) {
                    char* v = PQgetvalue(res, row, i);
                    rrow.emplace_back(v ? v : "");
                }
                r.rows.push_back(std::move(rrow));
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

/// worker 主循环：独占一条连接，串行执行队列。
void pg_worker_main(const PgSpec& spec, std::shared_ptr<ConnSlot> slot) {
    std::string ci = make_conninfo(spec);
    PGconn* c = PQconnectdb(ci.c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        std::string err = PQerrorMessage(c);
        PQfinish(c);
        LOG_ERROR("Postgres [{}] connect failed: {} ({}:{}/{} user={})",
                  spec.name, err, spec.host, spec.port, spec.dbname, spec.user);
        slot->fail_pending("postgres connect failed: " + err);
        return;
    }
    LOG_INFO("Postgres [{}] connected: {}:{}/{} user={}", spec.name, spec.host,
             spec.port, spec.dbname, spec.user);

    for (;;) {
        auto job = slot->next_job();
        if (!job)
            break;
        auto t0 = std::chrono::steady_clock::now();
        db::db_result r;
        switch (job->op) {
            case Op::Begin:    r = exec_text(c, "BEGIN"); break;
            case Op::Commit:   r = exec_text(c, "COMMIT"); break;
            case Op::Rollback: r = exec_text(c, "ROLLBACK"); break;
            case Op::Query:    r = stmt_execute(c, job->sql, job->params, true); break;
            case Op::Exec:     r = stmt_execute(c, job->sql, job->params, false); break;
        }
        r.duration_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (job->done)
            job->done(r);
    }

    PQfinish(c);
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

        return sys.spawn([logger, uris, pool_size, this](caf::event_based_actor* self) -> caf::behavior {
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
                        slot->start_worker(pg_worker_main, s, slot);
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
