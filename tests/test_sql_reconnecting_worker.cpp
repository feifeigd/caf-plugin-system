#include "templates/sql_reconnecting_worker.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <iostream>
#include <stdexcept>

namespace db = caf_plugin_system::db;
namespace sql = caf_plugin_system::sql_backend;
using namespace std::chrono_literals;

static void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

static db::db_result success() {
    db::db_result result;
    result.ok = true;
    return result;
}

static db::db_result failure(db::error_code code) {
    db::db_result result;
    result.code = code;
    result.error = db::to_string(code);
    result.native_code = "injected";
    result.sql_state = "08006";
    return result;
}

class FakeConnection final : public sql::SqlConnection {
public:
    db::db_result connect() override {
        ++connections;
        if (first_connection) {
            first_connection->set_value();
            first_connection = nullptr;
        }
        return take(connection_results);
    }
    void close() noexcept override { ++closes; }
    db::db_result execute(const sql::Job& job) override {
        ++executions;
        executed.push_back(job.op);
        return take(operation_results);
    }

    std::deque<db::db_result> connection_results;
    std::deque<db::db_result> operation_results;
    std::atomic<unsigned> connections{0};
    unsigned executions = 0;
    unsigned closes = 0;
    std::vector<sql::Operation> executed;
    std::promise<void>* first_connection = nullptr;

private:
    static db::db_result take(std::deque<db::db_result>& results) {
        if (results.empty())
            return success();
        auto result = std::move(results.front());
        results.pop_front();
        return result;
    }
};

class Fixture {
public:
    Fixture()
        : slot(std::make_shared<sql::ConnectionSlot>()),
          native(std::make_unique<FakeConnection>()), fake(native.get()),
          worker(slot, std::move(native), quick_policy()) {}

    db::db_result run(sql::Operation operation, uint64_t transaction = 0) {
        sql::Job job;
        job.op = operation;
        job.transaction = transaction;
        return worker.process(job);
    }

    std::shared_ptr<sql::ConnectionSlot> slot;
    std::unique_ptr<FakeConnection> native;
    FakeConnection* fake;
    sql::ReconnectingSqlWorker worker;

private:
    static sql::ReconnectPolicy quick_policy() {
        sql::ReconnectPolicy result;
        result.initial_delay = 0ms;
        result.max_delay = 0ms;
        return result;
    }
};

int main() {
    try {
        {
            Fixture test;
            test.fake->connection_results = {
                failure(db::error_code::connection_unavailable),
                failure(db::error_code::connection_unavailable), success()};
            require(test.run(sql::Operation::Query).ok, "bounded reconnect should recover");
            require(test.fake->connections == 3 && test.fake->executions == 1,
                    "connection retries must not replay SQL");
        }
        {
            Fixture test;
            test.fake->connection_results = {
                failure(db::error_code::connection_unavailable),
                failure(db::error_code::connection_unavailable),
                failure(db::error_code::connection_unavailable)};
            require(test.run(sql::Operation::Query).code == db::error_code::connection_unavailable,
                    "exhausted reconnect must report unavailable");
            require(test.fake->connections == 3 && test.fake->executions == 0,
                    "connect attempts must be bounded");
            require(!test.slot->stopped() && test.run(sql::Operation::Query).ok,
                    "temporary outage must not permanently stop the slot");
        }
        {
            Fixture test;
            test.fake->operation_results = {failure(db::error_code::connection_lost)};
            const auto lost = test.run(sql::Operation::Exec);
            require(lost.code == db::error_code::outcome_unknown,
                    "lost autocommit write must have uncertain outcome");
            require(lost.native_code == "injected" && lost.sql_state == "08006",
                    "driver diagnostics must survive classification");
            require(test.fake->executions == 1, "failed write was replayed");
            require(test.run(sql::Operation::Query).ok && test.fake->connections == 2,
                    "a subsequent fresh request should reconnect");
        }
        {
            Fixture test;
            require(test.run(sql::Operation::Begin, 41).ok, "begin failed");
            test.fake->operation_results = {failure(db::error_code::connection_lost)};
            require(test.run(sql::Operation::Exec, 41).code == db::error_code::connection_lost,
                    "transaction disconnect was not classified");
            require(test.run(sql::Operation::Query, 41).code == db::error_code::transaction_lost,
                    "old transaction must not reconnect and execute");
            require(test.run(sql::Operation::Begin, 42).ok, "replacement begin failed");
            const auto before_stale = test.fake->executions;
            require(test.run(sql::Operation::Rollback, 41).code == db::error_code::transaction_lost,
                    "late rollback must reject the previous session token");
            require(test.fake->executions == before_stale,
                    "old rollback must not touch the new session");
            require(test.run(sql::Operation::Commit, 42).ok, "new transaction was damaged");
        }
        {
            Fixture test;
            require(test.run(sql::Operation::Begin, 51).ok, "begin failed");
            test.fake->operation_results = {failure(db::error_code::connection_lost)};
            require(test.run(sql::Operation::Commit, 51).code == db::error_code::outcome_unknown,
                    "lost COMMIT acknowledgement must be uncertain");
            require(test.run(sql::Operation::Rollback, 51).code == db::error_code::transaction_lost,
                    "cleanup must not restore a lost transaction");
            require(test.run(sql::Operation::Begin, 52).ok, "slot failed to recover after commit loss");
        }
        {
            Fixture test;
            require(test.run(sql::Operation::Begin, 61).ok, "begin failed");
            test.fake->operation_results = {failure(db::error_code::sql_error)};
            require(!test.run(sql::Operation::Rollback, 61).ok, "injected rollback should fail");
            require(!test.slot->stopped(), "failed rollback permanently stopped the queue");
            require(test.run(sql::Operation::Begin, 62).ok && test.fake->connections == 2,
                    "rollback failure must close and replace the physical session");
        }
        {
            Fixture test;
            test.fake->operation_results = {failure(db::error_code::sql_error)};
            require(test.run(sql::Operation::Query).code == db::error_code::sql_error,
                    "ordinary SQL failure was reclassified");
            require(test.run(sql::Operation::Query).ok && test.fake->connections == 1,
                    "ordinary SQL errors must not cause reconnect/replay");
        }
        {
            auto slot = std::make_shared<sql::ConnectionSlot>();
            auto fake = std::make_unique<FakeConnection>();
            std::promise<void> attempted;
            auto signal = attempted.get_future();
            fake->first_connection = &attempted;
            fake->connection_results = {failure(db::error_code::connection_unavailable)};
            sql::ReconnectPolicy policy;
            policy.initial_delay = 5s;
            policy.max_delay = 5s;
            sql::ReconnectingSqlWorker worker{slot, std::move(fake), policy};
            auto done = std::async(std::launch::async, [&] { return worker.process(sql::Job{}); });
            require(signal.wait_for(1s) == std::future_status::ready, "connect did not start");
            slot->stop();
            require(done.wait_for(500ms) == std::future_status::ready,
                    "shutdown did not interrupt reconnect backoff");
            require(done.get().code == db::error_code::connection_unavailable,
                    "shutdown must not execute waiting SQL");
        }
        std::cout << "sql reconnecting worker tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
