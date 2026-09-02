#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "templates/job_queue.hpp"

#include <caf/all.hpp>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;

namespace {

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

enum class Op { Query, Exec, Begin, Commit, Rollback };

struct Job {
    Op op = Op::Query;
    std::string sql;
    std::vector<std::string> params;
    std::function<void(db::db_result&)> done;

    void fail(const std::string& message) {
        db::db_result result;
        result.error = message;
        if (done)
            done(result);
    }
};

using JobQueue = caf_plugin_system::JobQueue<Job>;

struct ConnSlot {
    std::shared_ptr<JobQueue> queue = std::make_shared<JobQueue>();
    std::shared_ptr<std::thread> thread;
    std::atomic<bool> busy{false};
};

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
            row.reserve(static_cast<size_t>(count));
            for (int i = 0; i < count; ++i) {
                auto text = sqlite3_column_text(stmt, i);
                auto bytes = sqlite3_column_bytes(stmt, i);
                row.emplace_back(text ? reinterpret_cast<const char*>(text) : "",
                                 text ? static_cast<size_t>(bytes) : 0);
            }
            result.rows.push_back(std::move(row));
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

    while (auto job = slot->queue->pop()) {
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

uint64_t make_tx(size_t db_index, size_t slot_index) {
    return (static_cast<uint64_t>(db_index) << 32) | slot_index;
}
size_t tx_db(uint64_t tx) { return static_cast<size_t>(tx >> 32); }
size_t tx_slot(uint64_t tx) { return static_cast<size_t>(tx & 0xffffffffu); }

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

        return sys.spawn([database_config, pool_size, timeout_ms](
                             caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<DbSpec>>(
                parse_databases(database_config));
            auto pools = std::make_shared<
                std::map<std::string, std::vector<std::shared_ptr<ConnSlot>>>>();
            auto round_robin = std::make_shared<std::atomic<size_t>>(0);
            auto default_db = std::make_shared<std::string>(specs->front().name);

            auto launch = [=] {
                for (const auto& spec : *specs) {
                    auto count = spec.path == ":memory:" ? 1 : pool_size;
                    auto& slots = (*pools)[spec.name];
                    for (int i = 0; i < count; ++i) {
                        auto slot = std::make_shared<ConnSlot>();
                        slot->thread = std::make_shared<std::thread>(
                            worker_main, spec, timeout_ms, slot);
                        slots.push_back(std::move(slot));
                    }
                }
            };

            auto spec_index = [=](const std::string& name) -> int {
                for (size_t i = 0; i < specs->size(); ++i)
                    if ((*specs)[i].name == name)
                        return static_cast<int>(i);
                return -1;
            };

            auto enqueue_slot = [=](const std::string& name, size_t index,
                                    std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end() || index >= it->second.size()) {
                    job->fail("unknown sqlite database: " + name);
                    return;
                }
                it->second[index]->queue->push(std::move(job));
            };

            auto enqueue = [=](const std::string& name, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end() || it->second.empty()) {
                    job->fail("unknown sqlite database: " + name);
                    return;
                }
                auto index = round_robin->fetch_add(1) % it->second.size();
                it->second[index]->queue->push(std::move(job));
            };

            auto request_job = [=](Op op, const std::string& sql,
                                   const std::vector<std::string>& params) {
                auto promise = self->make_response_promise<db::db_result>();
                auto job = std::make_shared<Job>();
                job->op = op;
                job->sql = sql;
                job->params = params;
                job->done = [promise](db::db_result& result) mutable {
                    promise.deliver(std::move(result));
                };
                return job;
            };

            auto begin = [=](const std::string& name) {
                auto promise = self->make_response_promise<db::db_result>();
                auto it = pools->find(name);
                auto db_index = spec_index(name);
                if (it == pools->end() || db_index < 0) {
                    db::db_result result;
                    result.error = "unknown sqlite database: " + name;
                    promise.deliver(std::move(result));
                    return;
                }
                for (size_t i = 0; i < it->second.size(); ++i) {
                    bool expected = false;
                    if (!it->second[i]->busy.compare_exchange_strong(expected, true))
                        continue;
                    auto tx = make_tx(static_cast<size_t>(db_index), i);
                    auto job = std::make_shared<Job>();
                    job->op = Op::Begin;
                    job->done = [=](db::db_result& result) mutable {
                        if (result.ok)
                            result.insert_id = std::to_string(tx);
                        else
                            it->second[i]->busy = false;
                        promise.deliver(std::move(result));
                    };
                    it->second[i]->queue->push(std::move(job));
                    return;
                }
                db::db_result result;
                result.error = "sqlite transaction pool exhausted: " + name;
                promise.deliver(std::move(result));
            };

            auto finish = [=](Op op, uint64_t tx) {
                auto promise = self->make_response_promise<db::db_result>();
                auto db_index = tx_db(tx);
                auto slot_index = tx_slot(tx);
                if (db_index >= specs->size()) {
                    db::db_result result;
                    result.error = "invalid sqlite transaction: " + std::to_string(tx);
                    promise.deliver(std::move(result));
                    return;
                }
                auto it = pools->find((*specs)[db_index].name);
                if (it == pools->end() || slot_index >= it->second.size() ||
                    !it->second[slot_index]->busy.load()) {
                    db::db_result result;
                    result.error = "inactive sqlite transaction: " + std::to_string(tx);
                    promise.deliver(std::move(result));
                    return;
                }
                auto job = std::make_shared<Job>();
                job->op = op;
                job->done = [=](db::db_result& result) mutable {
                    it->second[slot_index]->busy = false;
                    promise.deliver(std::move(result));
                };
                it->second[slot_index]->queue->push(std::move(job));
            };

            caf::message_handler business{
                [=](sql_query_atom, const std::string& name, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (self->current_message_id().is_request())
                        enqueue(name, request_job(Op::Query, sql, params));
                },
                [=](sql_query_atom, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (self->current_message_id().is_request())
                        enqueue(*default_db, request_job(Op::Query, sql, params));
                },
                [=](sql_exec_atom, const std::string& name, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (self->current_message_id().is_request())
                        enqueue(name, request_job(Op::Exec, sql, params));
                },
                [=](sql_exec_atom, const std::string& sql,
                    const std::vector<std::string>& params) {
                    if (self->current_message_id().is_request())
                        enqueue(*default_db, request_job(Op::Exec, sql, params));
                },
                [=](tx_begin_atom, const std::string& name) {
                    if (self->current_message_id().is_request())
                        begin(name);
                },
                [=](tx_begin_atom) {
                    if (self->current_message_id().is_request())
                        begin(*default_db);
                },
                [=](tx_commit_atom, uint64_t tx) {
                    if (self->current_message_id().is_request())
                        finish(Op::Commit, tx);
                },
                [=](tx_rollback_atom, uint64_t tx) {
                    if (self->current_message_id().is_request())
                        finish(Op::Rollback, tx);
                },
                [=](plugin_envelope env) -> caf::result<std::string> {
                    if (env.function == "hello") {
                        if (auto input = plugin_wire::decode_text(env))
                            return std::string("sqlite:hello:") + *input;
                    }
                    return caf::make_error(caf::sec::invalid_argument,
                                           "sqlite_service: unknown function");
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(
                self, PluginLifecycleHooks{
                          .on_init = [=](caf::actor, const std::string&) {
                              launch();
                              LOG_INFO_SELF(self,
                                            "SqlitePlugin initialized, databases={}, pool={}",
                                            specs->size(), pool_size);
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
                              for (auto& [name, slots] : *pools)
                                  for (auto& slot : slots)
                                      slot->queue->stop();
                              size_t joined = 0;
                              for (auto& [name, slots] : *pools)
                                  for (auto& slot : slots)
                                      if (slot->thread && slot->thread->joinable()) {
                                          slot->thread->join();
                                          ++joined;
                                      }
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
