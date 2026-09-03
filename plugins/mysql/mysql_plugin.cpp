// ------------------------------------------------------------------
// MySQL 插件（libmariadb，Phase 2 参考实现）
//
// 与 Redis 插件同骨架（阻塞 IO 模型），SQL 特有三处：
//   1. 连接池：每个命名连接 = db-pool-size 条连接（= worker 数），
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
#include "templates/sql_service_handlers.hpp"
#include "templates/sql_uri_config.hpp"

// libmariadb 依赖 winsock2（同 hiredis 坑），必须最先包含
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mysql.h>

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
using SqlPool = caf_plugin_system::sql_backend::ConnectionPool;
using SqlDispatcher = caf_plugin_system::sql_backend::SqlServiceDispatcher;

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.mysql.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define MYSQL_FIELDS(X, XC)                                                   \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)
PLUGIN_CONFIG(MYSQL_FIELDS)
#undef MYSQL_FIELDS

using SqlSpec = caf_plugin_system::sql_backend::ConnectionSpec;

std::vector<SqlSpec> parse_uris(const caf::settings& uris) {
    const caf_plugin_system::sql_backend::ConnectionUriParser parser{
        {"mysql://"}, "root", 3306};
    return parser.parse(uris);
}
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
        slot->fail_pending("mysql_init failed");
        return;
    }
    if (!mysql_real_connect(c, spec.host.c_str(), spec.user.c_str(), spec.pass.c_str(),
                            spec.dbname.empty() ? nullptr : spec.dbname.c_str(),
                            spec.port, nullptr, 0)) {
        std::string err = mysql_error(c);
        mysql_close(c);
        LOG_ERROR("MySQL [{}] connect failed: {} ({}:{} user={} db={})",
                  spec.name, err, spec.host, spec.port, spec.user, spec.dbname);
        slot->fail_pending("mysql connect failed: " + err);
        return;
    }
    LOG_INFO("MySQL [{}] connected: {}:{}/{} user={}", spec.name, spec.host,
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

    mysql_close(c);
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

        return sys.spawn([logger, uris, pool_size](caf::event_based_actor* self) -> caf::behavior {
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
                        slot->start_worker(sql_worker_main, s, slot);
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
