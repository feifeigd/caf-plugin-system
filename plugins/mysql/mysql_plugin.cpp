// ------------------------------------------------------------------
// MySQL 插件（libmariadb，Phase 2 参考实现）
//
// 与 Redis 插件同骨架（阻塞 IO 模型），SQL 特有三处：
//   1. 连接池：每个命名连接 = db-pool-size 条连接（= worker 数），
//      非事务请求 round-robin 分配
//   2. 参数化查询：mysql_stmt_prepare + bind（全字符串参数，
//      空串 ≠ NULL；NULL 参数 v1 不支持）
//   3. 事务状态机：tx_begin 借空闲连接（busy 标记）→ 带 tx_handle 的
//      请求钉在该连接 → commit/rollback 归还。tx_begin 响应 db_result，
//      成功时 insert_id = tx_handle（十进制字符串），调用方 stoull 后
//      用于后续 commit/rollback 消息。
//      tx_handle 编码 = (命名连接在配置中的序号 << 32) | 槽序号：
//      路由零查找、零锁竞争（worker 回调只碰 atomic busy）。
//
// 配置（CAF 配置系统，同文件字段区分）：
//   caf-plugin-system {
//     mysql-uris = "main=mysql://root:pass@127.0.0.1:3306/appdb,cache=mysql://root@127.0.0.1:3306/cachedb"
//     db-pool-size = 2
//   }
// uri = mysql://user:pass@host:port/dbname（各段可省略）
//
// 断线重连：v1 不做（执行失败返回 error；驱动自动重连 MYSQL_OPT_RECONNECT
// 留 v2 决策）。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "templates/job_queue.hpp"

// libmariadb 依赖 winsock2（同 hiredis 坑），必须最先包含
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>

#include <caf/all.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
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
// 字段 + 默认值，读取路径 caf-plugin-system.mysql.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define MYSQL_FIELDS(X, XC)                                                   \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)
PLUGIN_CONFIG(MYSQL_FIELDS)
#undef MYSQL_FIELDS

/// 单个命名连接的解析结果（mysql://user:pass@host:port/dbname）。
struct SqlSpec {
    std::string name;
    std::string user = "root";
    std::string pass;
    std::string host = "127.0.0.1";
    int port = 3306;
    std::string dbname;
};

/// 从配置 dictionary 构造命名连接表（键 = 连接名，值 = uri 字符串）。
std::vector<SqlSpec> parse_uris(const caf::settings& uris) {
    std::vector<SqlSpec> out;
    for (const auto& [name, value] : uris) {
        auto uri_v = caf::get_if<std::string>(&value);
        if (!uri_v)
            continue;
        SqlSpec cs;
        cs.name = name;
        std::string uri = *uri_v;
        if (uri.rfind("mysql://", 0) == 0)
            uri = uri.substr(8);
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
        SqlSpec cs;
        cs.name = "default";
        out.push_back(std::move(cs));
    }
    return out;
}

/// 操作类型：事务命令（文本执行）与查询/写（参数化 stmt）。
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
db::db_result exec_text(MYSQL* c, const char* cmd) {
    db::db_result r;
    if (mysql_real_query(c, cmd, static_cast<unsigned long>(std::strlen(cmd))) != 0) {
        r.ok = false;
        r.error = mysql_error(c);
        return r;
    }
    r.ok = true;
    return r;
}

/// 参数化查询/写。参数全字符串（MYSQL_TYPE_STRING）；结果集用
/// mysql_stmt_fetch_column 两遍读取（先取长度再读），正确处理任意长度列。
db::db_result stmt_execute(MYSQL* c, const std::string& sql,
                           const std::vector<std::string>& params, bool want_rows) {
    db::db_result r;
    MYSQL_STMT* st = mysql_stmt_init(c);
    if (!st) {
        r.error = "mysql_stmt_init failed";
        return r;
    }
    if (mysql_stmt_prepare(st, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0) {
        r.error = mysql_stmt_error(st);
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
            r.error = mysql_stmt_error(st);
            mysql_stmt_close(st);
            return r;
        }
    }
    if (mysql_stmt_execute(st) != 0) {
        r.error = mysql_stmt_error(st);
        mysql_stmt_close(st);
        return r;
    }

    if (want_rows) {
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
                r.error = mysql_stmt_error(st);
                mysql_free_result(meta);
                mysql_stmt_close(st);
                return r;
            }
            while (mysql_stmt_fetch(st) == 0) {
                std::vector<std::string> row;
                row.reserve(ncols);
                for (unsigned i = 0; i < ncols; ++i) {
                    if (isnull[i]) {
                        row.emplace_back();
                        continue;
                    }
                    // 第一遍：只拿长度（buffer 置空，驱动只填 length）
                    MYSQL_BIND col = rbinds[i];
                    col.buffer = nullptr;
                    col.buffer_length = 0;
                    if (mysql_stmt_fetch_column(st, &col, i, 0) != 0) {
                        row.emplace_back();
                        continue;
                    }
                    // 第二遍：分配后真正读取
                    std::string cell(rlen[i], '\0');
                    col.buffer = cell.data();
                    col.buffer_length = static_cast<unsigned long>(cell.size());
                    if (mysql_stmt_fetch_column(st, &col, i, 0) == 0)
                        row.push_back(std::move(cell));
                    else
                        row.emplace_back();
                }
                r.rows.push_back(std::move(row));
            }
            mysql_free_result(meta);
        }
        r.ok = true;
    } else {
        r.affected = static_cast<int64_t>(mysql_stmt_affected_rows(st));
        unsigned long long id = mysql_insert_id(c);
        if (id != 0)
            r.insert_id = std::to_string(id);
        r.ok = true;
    }
    mysql_stmt_close(st);
    return r;
}

/// worker 主循环：独占一条连接，串行执行队列。
void sql_worker_main(const SqlSpec& spec, std::shared_ptr<ConnSlot> slot) {
    MYSQL* c = mysql_init(nullptr);
    if (!c) {
        slot->queue->fail_all("mysql_init failed");
        return;
    }
    if (!mysql_real_connect(c, spec.host.c_str(), spec.user.c_str(), spec.pass.c_str(),
                            spec.dbname.empty() ? nullptr : spec.dbname.c_str(),
                            spec.port, nullptr, 0)) {
        std::string err = mysql_error(c);
        mysql_close(c);
        LOG_ERROR("MySQL [{}] connect failed: {} ({}:{} user={} db={})",
                  spec.name, err, spec.host, spec.port, spec.user, spec.dbname);
        slot->queue->fail_all("mysql connect failed: " + err);
        return;
    }
    LOG_INFO("MySQL [{}] connected: {}:{}/{} user={}", spec.name, spec.host,
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

    mysql_close(c);
    LOG_INFO("MySQL [{}] worker exited", spec.name);
}

// tx_handle 编码：高 32 位 = specs 中的命名连接序号，低 32 位 = 槽序号
inline uint64_t make_tx_handle(size_t name_idx, size_t slot_idx) {
    return (static_cast<uint64_t>(name_idx) << 32) | static_cast<uint64_t>(slot_idx);
}
inline size_t tx_name_idx(uint64_t tx) { return static_cast<size_t>(tx >> 32); }
inline size_t tx_slot_idx(uint64_t tx) { return static_cast<size_t>(tx & 0xffffffffu); }

} // namespace

class MySqlPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"MySqlPlugin", "1.0.0",
                {"logging_service"},
                {"mysql_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = deps.empty() ? caf::actor{} : deps[0];
        caf_plugin_system::set_logger(logger);
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // 声明式配置读取（PLUGIN_CONFIG）：字段、默认值、解析全部自动
        auto cfg = load_plugin_config(sys.config());
        caf::settings uris = cfg.uris;
        int pool_size = cfg.pool_size < 1 ? 1 : cfg.pool_size;

        return sys.spawn([logger, uris, pool_size](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<SqlSpec>>(parse_uris(uris));
            // 池表：name → slots（按 specs 顺序构建，槽序号 = specs 内索引）
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
                        slot->thread = std::make_shared<std::thread>(sql_worker_main, s, slot);
                        slots.push_back(slot);
                    }
                    LOG_INFO_SELF(self, "pool launched: [{}] {}:{}/{} size={}",
                                  s.name, s.host, s.port, s.dbname, pool_size);
                }
            };

            // 找命名连接在 specs 中的序号（事务 handle 编码用）
            auto name_index = [=](const std::string& name) -> int {
                for (size_t i = 0; i < specs->size(); ++i)
                    if ((*specs)[i].name == name)
                        return static_cast<int>(i);
                return -1;
            };

            // 事务结束（commit/rollback 执行完，无论成败）释放槽；worker 线程调用
            auto release_slot = [=](uint64_t tx) {
                size_t name_idx = tx_name_idx(tx);
                size_t slot_idx = tx_slot_idx(tx);
                if (name_idx < specs->size()) {
                    auto it = pools->find((*specs)[name_idx].name);
                    if (it != pools->end() && slot_idx < it->second.size())
                        it->second[slot_idx]->busy = false;
                }
            };

            // 入队到指定连接槽（事务钉连接）
            auto enqueue_slot = [=](const std::string& name, size_t idx, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end() || idx >= it->second.size()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown mysql connection: " + name;
                    if (job->done)
                        job->done(r);
                    return;
                }
                it->second[idx]->queue->push(std::move(job));
            };

            // 非事务请求：round-robin 到该命名连接的池
            auto enqueue_rr = [=](const std::string& name, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown mysql connection: " + name;
                    if (job->done)
                        job->done(r);
                    return;
                }
                auto& slots = it->second;
                size_t idx = (rr->fetch_add(1)) % slots.size();
                slots[idx]->queue->push(std::move(job));
            };

            // 事务请求：按 tx_handle 解码路由
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

            // 事务 begin：借空闲连接 → 编码 tx_handle → BEGIN job
            auto do_begin = [=](const std::string& conn) {
                auto rp = self->make_response_promise<db::db_result>();
                auto it = pools->find(conn);
                if (it == pools->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown mysql connection: " + conn;
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
                        // 失败时释放槽再交付；成功时 insert_id 携带 tx_handle
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
                r.error = "mysql pool exhausted (all connections busy)";
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
                    sc("CREATE TABLE IF NOT EXISTS hermes_selfcheck (id INT AUTO_INCREMENT PRIMARY KEY, v VARCHAR(64))", {}, [=] {
                        sc("INSERT INTO hermes_selfcheck (v) VALUES (?)", {"db-ping"}, [=] {
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
                        release_slot(tx);  // 无论成败事务已结束
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
            };

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
