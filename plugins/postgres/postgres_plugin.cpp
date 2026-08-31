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
#include "templates/job_queue.hpp"

// libpq 头（Windows 上同样依赖 winsock2 先行，统一模式）
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <libpq-fe.h>

#include <caf/all.hpp>

#include <atomic>
#include <fstream>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;

namespace {

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.postgres.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define PG_FIELDS(X, XC)                                                      \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)
PLUGIN_CONFIG(PG_FIELDS)
#undef PG_FIELDS

/// 单个命名连接的解析结果（postgres://user:pass@host:port/dbname）。
struct PgSpec {
    std::string name;
    std::string user = "postgres";
    std::string pass;
    std::string host = "127.0.0.1";
    int port = 5432;
    std::string dbname;
};

/// 从配置 dictionary 构造命名连接表（键 = 连接名，值 = uri 字符串）。
std::vector<PgSpec> parse_uris(const caf::settings& uris) {
    std::vector<PgSpec> out;
    for (const auto& [name, value] : uris) {
        auto uri_v = caf::get_if<std::string>(&value);
        if (!uri_v)
            continue;
        PgSpec cs;
        cs.name = name;
        std::string uri = *uri_v;
        if (uri.rfind("postgres://", 0) == 0)
            uri = uri.substr(11);
        else if (uri.rfind("postgresql://", 0) == 0)
            uri = uri.substr(13);
        auto at = uri.find('@');
        if (at != std::string::npos) {
            std::string up = uri.substr(0, at);
            uri = uri.substr(at + 1);
            auto colon = up.find(':');
            if (colon != std::string::npos) {
                cs.user = up.substr(0, colon);
                cs.pass = up.substr(colon + 1);
            } else {
                cs.user = up;
            }
        }
        auto slash = uri.find('/');
        std::string hostport = (slash == std::string::npos) ? uri : uri.substr(0, slash);
        auto colon = hostport.find(':');
        if (colon != std::string::npos) {
            cs.host = hostport.substr(0, colon);
            cs.port = std::atoi(hostport.substr(colon + 1).c_str());
        } else if (!hostport.empty()) {
            cs.host = hostport;
        }
        if (slash != std::string::npos)
            cs.dbname = uri.substr(slash + 1);
        if (cs.name.empty())
            cs.name = "default";
        out.push_back(std::move(cs));
    }
    if (out.empty()) {
        PgSpec cs;
        cs.name = "default";
        out.push_back(std::move(cs));
    }
    return out;
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

enum class Op { Query, Exec, Begin, Commit, Rollback };

struct Job {
    Op op = Op::Query;
    std::string sql;
    std::vector<std::string> params;
    std::function<void(db::db_result&)> done;  // worker 执行完回调（deliver）

    /// 失败交付方式（JobQueue::fail_all 统一调用）。
    void fail(const std::string& err) {
        if (done) {
            db::db_result r;
            r.ok = false;
            r.error = err;
            done(r);
        }
    }
};

using JobQueue = caf_plugin_system::JobQueue<Job>;

/// 连接槽：一条连接 + 专属队列 + worker 线程 + 事务占用标记。
struct ConnSlot {
    std::shared_ptr<JobQueue> queue = std::make_shared<JobQueue>();
    std::shared_ptr<std::thread> thread;
    std::atomic<bool> busy{false};
};

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
        slot->queue->fail_all("postgres connect failed: " + err);
        return;
    }
    LOG_INFO("Postgres [{}] connected: {}:{}/{} user={}", spec.name, spec.host,
             spec.port, spec.dbname, spec.user);

    for (;;) {
        auto job = slot->queue->pop();
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

// tx_handle 编码：高 32 位 = specs 中的命名连接序号，低 32 位 = 槽序号
inline uint64_t make_tx_handle(size_t name_idx, size_t slot_idx) {
    return (static_cast<uint64_t>(name_idx) << 32) | static_cast<uint64_t>(slot_idx);
}
inline size_t tx_name_idx(uint64_t tx) { return static_cast<size_t>(tx >> 32); }
inline size_t tx_slot_idx(uint64_t tx) { return static_cast<size_t>(tx & 0xffffffffu); }

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
            auto pools = std::make_shared<std::map<std::string, std::vector<std::shared_ptr<ConnSlot>>>>();
            auto rr = std::make_shared<std::atomic<size_t>>(0);
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto default_conn = std::make_shared<std::string>("default");

            auto launch_workers = [=] {
                if (started->exchange(true))
                    return;
                if (!specs->empty())
                    *default_conn = specs->front().name;
                for (const auto& s : *specs) {
                    auto& slots = (*pools)[s.name];
                    slots.reserve(pool_size);
                    for (int i = 0; i < pool_size; ++i) {
                        auto slot = std::make_shared<ConnSlot>();
                        slot->thread = std::make_shared<std::thread>(pg_worker_main, s, slot);
                        slots.push_back(slot);
                    }
                    LOG_INFO_SELF(self, "pool launched: [{}] {}:{}/{} size={}",
                                  s.name, s.host, s.port, s.dbname, pool_size);
                }
            };

            auto name_index = [=](const std::string& name) -> int {
                for (size_t i = 0; i < specs->size(); ++i)
                    if ((*specs)[i].name == name)
                        return static_cast<int>(i);
                return -1;
            };

            auto release_slot = [=](uint64_t tx) {
                size_t name_idx = tx_name_idx(tx);
                size_t slot_idx = tx_slot_idx(tx);
                if (name_idx < specs->size()) {
                    auto it = pools->find((*specs)[name_idx].name);
                    if (it != pools->end() && slot_idx < it->second.size())
                        it->second[slot_idx]->busy = false;
                }
            };

            auto enqueue_slot = [=](const std::string& name, size_t idx, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end() || idx >= it->second.size()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown postgres connection: " + name;
                    if (job->done)
                        job->done(r);
                    return;
                }
                it->second[idx]->queue->push(std::move(job));
            };

            auto enqueue_rr = [=](const std::string& name, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown postgres connection: " + name;
                    if (job->done)
                        job->done(r);
                    return;
                }
                auto& slots = it->second;
                size_t idx = (rr->fetch_add(1)) % slots.size();
                slots[idx]->queue->push(std::move(job));
            };

            auto enqueue_tx = [=](uint64_t tx, std::shared_ptr<Job> job) {
                size_t name_idx = tx_name_idx(tx);
                size_t slot_idx = tx_slot_idx(tx);
                if (name_idx >= specs->size()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "invalid transaction: " + std::to_string(tx);
                    if (job->done)
                        job->done(r);
                    return;
                }
                enqueue_slot((*specs)[name_idx].name, slot_idx, std::move(job));
            };

            auto do_begin = [=](const std::string& conn) {
                auto rp = self->make_response_promise<db::db_result>();
                auto it = pools->find(conn);
                if (it == pools->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown postgres connection: " + conn;
                    rp.deliver(std::move(r));
                    return;
                }
                int name_idx = name_index(conn);
                if (name_idx < 0) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "internal: connection not in specs";
                    rp.deliver(std::move(r));
                    return;
                }
                auto& slots = it->second;
                for (size_t i = 0; i < slots.size(); ++i) {
                    size_t idx = (rr->fetch_add(1)) % slots.size();
                    bool expected = false;
                    if (slots[idx]->busy.compare_exchange_strong(expected, true)) {
                        uint64_t tx = make_tx_handle(static_cast<size_t>(name_idx), idx);
                        auto job = std::make_shared<Job>();
                        job->op = Op::Begin;
                        job->done = [=](db::db_result& r) mutable {
                            if (!r.ok)
                                slots[idx]->busy = false;
                            else
                                r.insert_id = std::to_string(tx);
                            rp.deliver(std::move(r));
                        };
                        slots[idx]->queue->push(std::move(job));
                        return;
                    }
                }
                db::db_result r;
                r.ok = false;
                r.error = "postgres pool exhausted (all connections busy)";
                rp.deliver(std::move(r));
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

            caf::message_handler business{
                [=](sql_query_atom, const std::string& conn, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Query;
                    job->sql = sql;
                    job->params = params;
                    job->done = [rp](db::db_result& r) mutable { rp.deliver(std::move(r)); };
                    enqueue_rr(conn, std::move(job));
                },
                [=](sql_query_atom, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Query;
                    job->sql = sql;
                    job->params = params;
                    job->done = [rp](db::db_result& r) mutable { rp.deliver(std::move(r)); };
                    enqueue_rr(*default_conn, std::move(job));
                },
                [=](sql_exec_atom, const std::string& conn, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Exec;
                    job->sql = sql;
                    job->params = params;
                    job->done = [rp](db::db_result& r) mutable { rp.deliver(std::move(r)); };
                    enqueue_rr(conn, std::move(job));
                },
                [=](sql_exec_atom, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Exec;
                    job->sql = sql;
                    job->params = params;
                    job->done = [rp](db::db_result& r) mutable { rp.deliver(std::move(r)); };
                    enqueue_rr(*default_conn, std::move(job));
                },
                [=](tx_begin_atom, const std::string& conn) {
                    if (!self->current_message_id().is_request())
                        return;
                    do_begin(conn);
                },
                [=](tx_begin_atom) {
                    if (!self->current_message_id().is_request())
                        return;
                    do_begin(*default_conn);
                },
                [=](tx_commit_atom, uint64_t tx) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Commit;
                    job->done = [=](db::db_result& r) mutable {
                        release_slot(tx);
                        rp.deliver(std::move(r));
                    };
                    enqueue_tx(tx, std::move(job));
                },
                [=](tx_rollback_atom, uint64_t tx) {
                    if (!self->current_message_id().is_request())
                        return;
                    auto rp = self->make_response_promise<db::db_result>();
                    auto job = std::make_shared<Job>();
                    job->op = Op::Rollback;
                    job->done = [=](db::db_result& r) mutable {
                        release_slot(tx);
                        rp.deliver(std::move(r));
                    };
                    enqueue_tx(tx, std::move(job));
                },
                // 跨节点信封入口（RemoteCaller 直接把 plugin_envelope 发给
                // 目标服务，见 remote_caller.cpp do_call）。子协议号插件自管：
                //   1 = hello：回显 pg:hello:<payload>（跨节点链路自检）
                [=](plugin_envelope env) -> caf::result<std::string> {
                    switch (env.sub_proto) {
                    case 1: {
                        auto in = plugin_wire::decode_text(env);
                        if (!in)
                            return caf::make_error(caf::sec::invalid_argument,
                                                   "pg_service: unsupported payload format");
                        return std::string("pg:hello:") + *in;
                    }
                    default:
                        return caf::make_error(
                            caf::sec::invalid_argument,
                            "pg_service: unknown sub_proto: "
                                + std::to_string(env.sub_proto));
                    }
                },
            };

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
                    for (auto& [name, slots] : *pools) {
                        for (auto& s : slots)
                            s->queue->stop();
                    }
                    size_t joined = 0;
                    for (auto& [name, slots] : *pools) {
                        for (auto& s : slots) {
                            if (s->thread->joinable()) {
                                s->thread->join();
                                ++joined;
                            }
                        }
                    }
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
