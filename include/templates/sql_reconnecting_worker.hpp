#pragma once

#include "templates/sql_connection_pool.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>

namespace caf_plugin_system::sql_backend {

struct ReconnectPolicy {
    unsigned max_attempts = 3;
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{1000};
    unsigned connect_timeout_seconds = 2;
    unsigned io_timeout_seconds = 5;
};

// Implementations own their native handle (RAII) and never enable native
// automatic reconnect: a new physical session must invalidate its transaction.
class SqlConnection {
public:
    virtual ~SqlConnection() = default;
    virtual db::db_result connect() = 0;
    virtual void close() noexcept = 0;
    virtual db::db_result execute(const Job& job) = 0;
};

/// 带自动重连的工作循环
class ReconnectingSqlWorker {
public:
    ReconnectingSqlWorker(std::shared_ptr<ConnectionSlot> slot,
                          std::unique_ptr<SqlConnection> connection,
                          ReconnectPolicy policy = {})
        : slot_(std::move(slot)), connection_(std::move(connection)),
          policy_(policy) {
        slot_->enable_recovery();
        policy_.max_attempts = std::clamp(policy_.max_attempts, 1u, 10u);
    }

    void run() {
        while (auto job = slot_->next_job()) {
            const auto started = std::chrono::steady_clock::now();
            auto result = process(*job);
            result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (job->done)
                job->done(result);
        }
        connection_->close();
    }

    // Public for deterministic fault-injection tests with a fake connection.
    // Called only by the owning worker thread in production.
    db::db_result process(const Job& job) {
        if (slot_->stopped())
            return failure(db::error_code::connection_unavailable,
                           "database worker is stopping");
        if (job.transaction != 0 && job.op != Operation::Begin
            && job.transaction != active_transaction_)
            return failure(db::error_code::transaction_lost,
                           "transaction no longer belongs to a live database session");
        if (!connected_) {
            auto connected = reconnect();
            if (!connected.ok)
                return connected;
        }

        auto result = connection_->execute(job);
        if (!result.ok && result.code == db::error_code::none)
            result.code = db::error_code::sql_error;
        const bool lost = !result.ok && db::is_connection_error(result.code);
        if (lost || (job.op == Operation::Rollback && !result.ok)) {
            // Closing the native handle rolls back any still-live transaction.
            // Never replay the failed job on the replacement connection.
            connection_->close();
            connected_ = false;
            active_transaction_ = 0;
            if (lost && (job.op == Operation::Commit
                         || (job.transaction == 0 && job.op == Operation::Exec)))
                result.code = db::error_code::outcome_unknown;
        } else if (result.ok) {
            if (job.op == Operation::Begin)
                active_transaction_ = job.transaction;
            else if (job.op == Operation::Commit || job.op == Operation::Rollback)
                active_transaction_ = 0;
        }
        return result;
    }

private:
    static db::db_result failure(db::error_code code, const char* message) {
        db::db_result result;
        result.code = code;
        result.error = message;
        return result;
    }

    db::db_result reconnect() {
        auto delay = policy_.initial_delay;
        auto result = failure(db::error_code::connection_unavailable,
                              "database connection unavailable");
        for (unsigned attempt = 0; attempt < policy_.max_attempts; ++attempt) {
            if (slot_->stopped())
                return failure(db::error_code::connection_unavailable,
                               "database reconnect interrupted by shutdown");
            result = connection_->connect();
            if (result.ok) {
                connected_ = true;
                return result;
            }
            connection_->close();
            if (!db::is_connection_error(result.code))
                return result;
            result.code = db::error_code::connection_unavailable;
            if (attempt + 1 < policy_.max_attempts) {
                if (slot_->wait_for_stop(delay))
                    return failure(db::error_code::connection_unavailable,
                                   "database reconnect interrupted by shutdown");
                delay = std::min(delay * 2, policy_.max_delay);
            }
        }
        return result;
    }

    std::shared_ptr<ConnectionSlot> slot_;
    std::unique_ptr<SqlConnection> connection_;
    ReconnectPolicy policy_;
    bool connected_ = false;
    uint64_t active_transaction_ = 0;
};

} // namespace caf_plugin_system::sql_backend
