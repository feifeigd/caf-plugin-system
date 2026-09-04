#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "templates/sql_service_handlers.hpp"

#include <caf/all.hpp>
#include <sqlite3.h>

#include <charconv>
#include <chrono>
#include <filesystem>
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

// caf-plugin-system { sqlite { databases { default = "./data/app.db" }
// pool_size = 1 busy_timeout_ms = 5000 } }
#define SQLITE_FIELDS(X, XC)                                                 \
    X(caf::settings, databases, {})                                          \
    X(int, pool_size, 1)                                                     \
    X(int, busy_timeout_ms, 5000)
PLUGIN_CONFIG(SQLITE_FIELDS)
#undef SQLITE_FIELDS

struct DbSpec {
    std::string name;
    std::string path;
};

std::vector<DbSpec> parse_databases(const caf::settings& values) {
    std::vector<DbSpec> result;
    for (const auto& [name, value] : values)
        if (auto path = caf::get_if<std::string>(&value))
            result.push_back({name.empty() ? "default" : name, *path});
    if (result.empty())
        result.push_back({"default", "./data/app.db"});
    return result;
}

db::db_result error_result(sqlite3* conn, const std::string& prefix) {
    db::db_result result;
    result.error = prefix + ": " + (conn ? sqlite3_errmsg(conn) : "no connection");
    return result;
}

db::db_result execute(sqlite3* conn, const Job& job) {
    db::db_result result;
    const char* sql = job.sql.c_str();
    if (job.op == Op::Begin)
        sql = "BEGIN IMMEDIATE";
    else if (job.op == Op::Commit)
        sql = "COMMIT";
    else if (job.op == Op::Rollback)
        sql = "ROLLBACK";

    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return error_result(conn, "sqlite prepare failed");
    struct StatementGuard {
        sqlite3_stmt* value;
        ~StatementGuard() { sqlite3_finalize(value); }
    } guard{stmt};

    if (static_cast<int>(job.params.size()) != sqlite3_bind_parameter_count(stmt)) {
        result.error = "sqlite parameter count mismatch";
        return result;
    }
    for (size_t i = 0; i < job.params.size(); ++i) {
        const auto& value = job.params[i];
        if (sqlite3_bind_text(stmt, static_cast<int>(i + 1), value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
            return error_result(conn, "sqlite bind failed");
    }

    bool columns_added = false;
    for (;;) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            if (job.op != Op::Query)
                continue;
            int count = sqlite3_column_count(stmt);
            if (!columns_added) {
                for (int i = 0; i < count; ++i)
                    result.columns.emplace_back(sqlite3_column_name(stmt, i));
                columns_added = true;
            }
            std::vector<std::string> row;
            std::vector<uint8_t> nulls;
            row.reserve(static_cast<size_t>(count));
            nulls.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                const auto is_null = sqlite3_column_type(stmt, i) == SQLITE_NULL;
                auto text = sqlite3_column_text(stmt, i);
                auto bytes = sqlite3_column_bytes(stmt, i);
                row.emplace_back(text ? reinterpret_cast<const char*>(text) : "",
                                 text ? static_cast<size_t>(bytes) : 0);
                nulls.push_back(is_null ? 1u : 0u);
            }
            result.rows.push_back(std::move(row));
            result.nulls.push_back(std::move(nulls));
        } else if (rc == SQLITE_DONE) {
            break;
        } else {
            return error_result(conn, "sqlite step failed");
        }
    }
    result.ok = true;
    if (job.op == Op::Exec) {
        result.affected = sqlite3_changes64(conn);
        auto id = sqlite3_last_insert_rowid(conn);
        if (id != 0)
            result.insert_id = std::to_string(id);
    }
    return result;
}

void worker_main(DbSpec spec, int timeout_ms, std::shared_ptr<ConnSlot> slot) {
    sqlite3* conn = nullptr;
    std::string open_error;
    if (spec.path != ":memory:" && spec.path.rfind("file:", 0) != 0) {
        std::error_code ec;
        auto parent = std::filesystem::path(spec.path).parent_path();
        if (!parent.empty())
            std::filesystem::create_directories(parent, ec);
        if (ec)
            open_error = "cannot create sqlite directory: " + ec.message();
    }
    if (open_error.empty()) {
        auto flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI |
                     SQLITE_OPEN_FULLMUTEX;
        if (sqlite3_open_v2(spec.path.c_str(), &conn, flags, nullptr) != SQLITE_OK)
            open_error = conn ? sqlite3_errmsg(conn) : "sqlite open failed";
        else
            sqlite3_busy_timeout(conn, timeout_ms);
    }
    if (open_error.empty())
        LOG_INFO("SQLite [{}] opened: {}", spec.name, spec.path);
    else
        LOG_ERROR("SQLite [{}] open failed: {}", spec.name, open_error);

    while (auto job = slot->next_job()) {
        auto started = std::chrono::steady_clock::now();
        db::db_result result;
        if (open_error.empty())
            result = execute(conn, *job);
        else
            result.error = open_error;
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        if (job->done)
            job->done(result);
    }
    if (conn)
        sqlite3_close(conn);
    LOG_INFO("SQLite [{}] worker exited", spec.name);
}



} // namespace

class SqlitePlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"SqlitePlugin", "1.0.0", {}, {"sqlite_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys, const std::vector<caf::actor>&,
                     const std::string&) override {
        caf_plugin_system::set_log_source(PLUGIN_NAME);
        auto config = load_plugin_config(sys.config());
        int pool_size = config.pool_size < 1 ? 1 : config.pool_size;
        int timeout_ms = config.busy_timeout_ms < 0 ? 0 : config.busy_timeout_ms;
        auto database_config = config.databases;
        bool run_transaction_selfcheck = caf::get_or(
            sys.config(), "caf-plugin-system.test-auto-shutdown", false);

        return sys.spawn([database_config, pool_size, timeout_ms,
                          run_transaction_selfcheck](
                             caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<DbSpec>>(
                parse_databases(database_config));
            auto pools = std::make_shared<SqlPool>("sqlite");
            auto default_db = std::make_shared<std::string>(specs->front().name);

            auto launch = [=] {
                for (const auto& spec : *specs) {
                    auto count = spec.path == ":memory:" ? 1 : pool_size;
                    for (int i = 0; i < count; ++i) {
                        auto slot = pools->add_slot(spec.name);
                        slot->start_worker(
                            worker_main, spec, timeout_ms, slot);
                    }
                }
            };

            auto transaction_selfcheck = [=] {
                const auto service = caf::actor{self};
                const auto create_sql = std::string{
                    "CREATE TABLE IF NOT EXISTS hermes_tx_selfcheck ("
                    "id INTEGER PRIMARY KEY, stable_value TEXT, changed_value TEXT)"};
                self->request(service, std::chrono::seconds(5), sql_exec_atom_v,
                              create_sql, std::vector<std::string>{})
                    .then(
                        [=](db::db_result& created) {
                            if (!created.ok) {
                                LOG_ERROR_SELF(self, "SQLite transaction selfcheck create: {}",
                                               created.error);
                                return;
                            }
                            self->request(
                                    service, std::chrono::seconds(5), sql_exec_atom_v,
                                    std::string{
                                        "INSERT INTO hermes_tx_selfcheck "
                                        "(id, stable_value, changed_value) VALUES (1, ?, ?) "
                                        "ON CONFLICT(id) DO UPDATE SET "
                                        "stable_value=excluded.stable_value, "
                                        "changed_value=excluded.changed_value"},
                                    std::vector<std::string>{"keep", "before"})
                                .then(
                                    [=](db::db_result& seeded) {
                                        if (!seeded.ok) {
                                            LOG_ERROR_SELF(self, "SQLite transaction selfcheck seed: {}", seeded.error);
                                            return;
                                        }
                                        self->request(service, std::chrono::seconds(5),
                                                      tx_begin_atom_v)
                                            .then(
                                                [=](db::db_result& begun) {
                                                    uint64_t tx = 0;
                                                    auto first = begun.insert_id.data();
                                                    auto last = first + begun.insert_id.size();
                                                    auto parsed = std::from_chars(first, last, tx);
                                                    if (!begun.ok
                                                        || parsed.ec != std::errc{}
                                                        || parsed.ptr != last) {
                                                        LOG_ERROR_SELF(self, "SQLite transaction selfcheck begin: {}", begun.error);
                                                        return;
                                                    }
                                                    self->request(
                                                            service, std::chrono::seconds(5),
                                                            sql_exec_atom_v, tx,
                                                            std::string{
                                                                "UPDATE hermes_tx_selfcheck "
                                                                "SET changed_value=? WHERE id=1"},
                                                            std::vector<std::string>{"after"})
                                                        .then(
                                                            [=](db::db_result& patched) {
                                                                if (!patched.ok) {
                                                                    LOG_ERROR_SELF(self, "SQLite transaction selfcheck patch: {}", patched.error);
                                                                    return;
                                                                }
                                                                self->request(
                                                                        service, std::chrono::seconds(5),
                                                                        tx_commit_atom_v, tx)
                                                                    .then(
                                                                        [=](db::db_result& committed) {
                                                                            if (!committed.ok) {
                                                                                LOG_ERROR_SELF(self, "SQLite transaction selfcheck commit: {}", committed.error);
                                                                                return;
                                                                            }
                                                                            self->request(
                                                                                    service, std::chrono::seconds(5),
                                                                                    sql_query_atom_v,
                                                                                    std::string{"SELECT stable_value, changed_value FROM hermes_tx_selfcheck WHERE id=1"},
                                                                                    std::vector<std::string>{})
                                                                                .then(
                                                                                    [=](db::db_result& checked) {
                                                                                        const bool ok = checked.ok && checked.rows.size() == 1 && checked.rows[0].size() == 2 && checked.rows[0][0] == "keep" && checked.rows[0][1] == "after";
                                                                                        LOG_INFO_SELF(self, "SQLite transaction selfcheck ok={}", ok);
                                                                                    },
                                                                                    [=](caf::error& error) {
                                                                                        LOG_ERROR_SELF(self, "SQLite transaction selfcheck query failed: {}", caf::to_string(error));
                                                                                    });
                                                                        },
                                                                        [=](caf::error& error) {
                                                                            LOG_ERROR_SELF(self, "SQLite transaction selfcheck commit failed: {}", caf::to_string(error));
                                                                        });
                                                            },
                                                            [=](caf::error& error) {
                                                                LOG_ERROR_SELF(self, "SQLite transaction selfcheck patch failed: {}", caf::to_string(error));
                                                            });
                                                },
                                                [=](caf::error& error) {
                                                    LOG_ERROR_SELF(self, "SQLite transaction selfcheck begin failed: {}", caf::to_string(error));
                                                });
                                    },
                                    [=](caf::error& error) {
                                        LOG_ERROR_SELF(self, "SQLite transaction selfcheck seed failed: {}", caf::to_string(error));
                                    });
                        },
                        [=](caf::error& error) {
                            LOG_ERROR_SELF(self, "SQLite transaction selfcheck create failed: {}",
                                           caf::to_string(error));
                        });
            };

            caf::message_handler plugin_handlers{
                [=](plugin_envelope env) -> caf::result<std::string> {
                    if (env.function == "hello") {
                        if (auto input = plugin_wire::decode_text(env))
                            return std::string("sqlite:hello:") + *input;
                    }
                    return caf::make_error(caf::sec::invalid_argument,
                                           "sqlite_service: unknown function");
                },
            };
            auto dispatcher = SqlDispatcher::create(
                self, pools, default_db);
            auto business = dispatcher->handlers().or_else(plugin_handlers);
            return caf::behavior{business.or_else(plugin_lifecycle(
                self, PluginLifecycleHooks{
                          .on_init = [=](caf::actor, const std::string&) {
                              launch();
                              LOG_INFO_SELF(self,
                                            "SqlitePlugin initialized, databases={}, pool={}",
                                            specs->size(), pool_size);
                              if (run_transaction_selfcheck)
                                  transaction_selfcheck();
                              self->request(caf::actor{self}, std::chrono::seconds(5),
                                            sql_query_atom_v,
                                            std::string{"SELECT sqlite_version()"},
                                            std::vector<std::string>{})
                                  .then(
                                      [=](db::db_result& result) {
                                          LOG_INFO_SELF(
                                              self,
                                              "SQLite selfcheck ok={} rows={} err={}",
                                              result.ok, result.rows.size(), result.error);
                                      },
                                      [=](caf::error& error) {
                                          LOG_ERROR_SELF(self, "SQLite selfcheck failed: {}",
                                                         caf::to_string(error));
                                      });
                          },
                          .on_save = [] { return std::vector<std::byte>{}; },
                          .on_shutdown = [=] {
                              auto joined = pools->stop_and_join();
                              LOG_INFO_SELF(self, "SqlitePlugin shutdown, {} workers joined",
                                            joined);
                          },
                      }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new SqlitePlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* plugin) {
    delete plugin;
}
