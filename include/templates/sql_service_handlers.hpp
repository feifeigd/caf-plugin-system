#pragma once

// SQL 插件共享的 CAF 消息分发对象。
// SqlServiceDispatcher 将 query/exec/transaction 协议统一翻译为 Job，
// ConnectionPool 负责连接和事务路由，各数据库插件只实现驱动执行函数。

#include "common/message_tags.hpp"
#include "templates/sql_connection_pool.hpp"

#include <caf/all.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace caf_plugin_system::sql_backend {

class SqlServiceDispatcher
    : public std::enable_shared_from_this<SqlServiceDispatcher> {
public:
    using pointer = std::shared_ptr<SqlServiceDispatcher>;

    static pointer create(caf::event_based_actor* actor,
                          std::shared_ptr<ConnectionPool> pools,
                          std::shared_ptr<std::string> default_connection) {
        return pointer{new SqlServiceDispatcher{
            actor, std::move(pools), std::move(default_connection)}};
    }

    caf::message_handler handlers() {
        auto dispatcher = shared_from_this();
        return caf::message_handler{
            [dispatcher](::sql_query_atom, const std::string& name,
                         const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_regular(
                    name, Operation::Query, sql, params);
            },
            [dispatcher](::sql_query_atom, const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_regular(
                    dispatcher->default_connection(), Operation::Query, sql,
                    params);
            },
            [dispatcher](::sql_query_atom, uint64_t tx,
                         const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_transaction(
                    tx, Operation::Query, sql, params);
            },
            [dispatcher](::sql_exec_atom, const std::string& name,
                         const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_regular(
                    name, Operation::Exec, sql, params);
            },
            [dispatcher](::sql_exec_atom, const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_regular(
                    dispatcher->default_connection(), Operation::Exec, sql,
                    params);
            },
            [dispatcher](::sql_exec_atom, uint64_t tx,
                         const std::string& sql,
                         const std::vector<std::string>& params) {
                dispatcher->dispatch_transaction(
                    tx, Operation::Exec, sql, params);
            },
            [dispatcher](::tx_begin_atom, const std::string& name) {
                dispatcher->begin_transaction(name);
            },
            [dispatcher](::tx_begin_atom) {
                dispatcher->begin_transaction(
                    dispatcher->default_connection());
            },
            [dispatcher](::tx_commit_atom, uint64_t tx) {
                dispatcher->finish_transaction(tx, Operation::Commit);
            },
            [dispatcher](::tx_rollback_atom, uint64_t tx) {
                dispatcher->finish_transaction(tx, Operation::Rollback);
            },
        };
    }

private:
    SqlServiceDispatcher(caf::event_based_actor* actor,
                         std::shared_ptr<ConnectionPool> pools,
                         std::shared_ptr<std::string> default_connection)
        : actor_(actor),
          pools_(std::move(pools)),
          default_connection_(std::move(default_connection)) {
    }

    bool is_request() const noexcept {
        return actor_->current_message_id().is_request();
    }

    const std::string& default_connection() const noexcept {
        return *default_connection_;
    }

    std::shared_ptr<Job>
    make_request_job(Operation operation, const std::string& sql = {},
                     const std::vector<std::string>& params = {}) {
        auto promise = actor_->make_response_promise<db::db_result>();
        auto job = std::make_shared<Job>();
        job->op = operation;
        job->sql = sql;
        job->params = params;
        job->done = [promise](db::db_result& result) mutable {
            promise.deliver(std::move(result));
        };
        return job;
    }

    void dispatch_regular(const std::string& name, Operation operation,
                          const std::string& sql,
                          const std::vector<std::string>& params) {
        if (!is_request())
            return;
        auto job = make_request_job(operation, sql, params);
        auto route = pools_->route_idle(name);
        if (!route) {
            job->fail(route.error);
            return;
        }
        route.enqueue(std::move(job));
    }

    void dispatch_transaction(uint64_t transaction, Operation operation,
                              const std::string& sql,
                              const std::vector<std::string>& params) {
        if (!is_request())
            return;
        auto job = make_request_job(operation, sql, params);
        auto route = pools_->route_transaction(transaction);
        if (!route) {
            job->fail(route.error);
            return;
        }
        route.enqueue(std::move(job));
    }

    void begin_transaction(const std::string& name) {
        if (!is_request())
            return;
        auto promise = actor_->make_response_promise<db::db_result>();
        auto route = pools_->acquire_transaction(name);
        if (!route) {
            deliver_error(promise, std::move(route.error));
            return;
        }
        const auto transaction = route.transaction;
        auto job = std::make_shared<Job>();
        job->op = Operation::Begin;
        job->done = [promise, pools = pools_,
                     transaction](db::db_result& result) mutable {
            if (result.ok)
                result.insert_id = std::to_string(transaction);
            else
                pools->release_transaction(transaction);
            promise.deliver(std::move(result));
        };
        route.enqueue(std::move(job));
    }

    void finish_transaction(uint64_t transaction, Operation operation) {
        if (!is_request())
            return;
        auto promise = actor_->make_response_promise<db::db_result>();
        auto route = pools_->route_transaction(transaction, true);
        if (!route) {
            deliver_error(promise, std::move(route.error));
            return;
        }
        auto job = std::make_shared<Job>();
        job->op = operation;
        job->done = [promise, pools = pools_,
                     transaction](db::db_result& result) mutable {
            pools->release_transaction(transaction);
            promise.deliver(std::move(result));
        };
        route.enqueue(std::move(job));
    }

    template <class Promise>
    static void deliver_error(Promise promise, std::string error) {
        db::db_result result;
        result.error = std::move(error);
        promise.deliver(std::move(result));
    }

    caf::event_based_actor* actor_;
    std::shared_ptr<ConnectionPool> pools_;
    std::shared_ptr<std::string> default_connection_;
};

} // namespace caf_plugin_system::sql_backend
