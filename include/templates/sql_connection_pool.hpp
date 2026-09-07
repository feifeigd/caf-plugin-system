#pragma once

// SQL 插件共享的连接池与事务路由。
// 驱动连接和语句执行仍由各插件负责；这里只管理 worker 队列、连接槽、
// 普通请求选路以及带代际号的短生命周期事务句柄。

#include "common/db_contract.hpp"
#include "templates/db_worker_pool.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace caf_plugin_system::sql_backend {

enum class Operation { Query, Exec, Begin, Commit, Rollback };

struct Job {
    Operation op = Operation::Query;
    uint64_t transaction = 0;
    std::string sql;
    std::vector<std::string> params;
    std::function<void(db::db_result&)> done;

    void fail(const std::string& error,
              db::error_code code = db::error_code::connection_unavailable) {
        if (!done)
            return;
        db::db_result result;
        result.ok = false;
        result.error = error;
        result.code = code;
        done(result);
    }
};

using JobQueue = caf_plugin_system::JobQueue<Job>;

enum class TransactionState : uint8_t { Idle, Active, Closing };

class TransactionSlot {
public:
    bool idle() const noexcept {
        return state_.load(std::memory_order_acquire) == TransactionState::Idle;
    }

    TransactionState state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    uint32_t generation() const noexcept {
        return generation_.load(std::memory_order_acquire);
    }

    // 返回 0 表示槽位正被占用；有效代际号永不为 0。
    uint32_t try_acquire() noexcept {
        auto expected = TransactionState::Idle;
        if (!state_.compare_exchange_strong(expected, TransactionState::Active,
                                            std::memory_order_acq_rel))
            return 0;
        auto next = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (next == 0)
            next = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        return next;
    }

    bool try_close(uint32_t expected_generation) noexcept {
        if (generation() != expected_generation)
            return false;
        auto expected = TransactionState::Active;
        return state_.compare_exchange_strong(expected, TransactionState::Closing,
                                              std::memory_order_acq_rel);
    }

    bool release(uint32_t expected_generation) noexcept {
        if (generation() != expected_generation)
            return false;
        state_.store(TransactionState::Idle, std::memory_order_release);
        return true;
    }

private:
    std::atomic<TransactionState> state_{TransactionState::Idle};
    std::atomic<uint32_t> generation_{0};
};

// Adapter state is safe to retain from jobs: it does not own a thread.
class ConnectionState : public db_backend::WorkerState<Job> {
public:
    using WorkerState::WorkerState;
    void enable_recovery() noexcept { recoverable_.store(true); }
    bool recoverable() const noexcept { return recoverable_.load(); }
    TransactionState transaction_state() const noexcept { return transaction_.state(); }
private:
    friend class ConnectionPool;
    friend class TransactionRegistry;
    TransactionSlot transaction_;
    std::atomic<bool> recoverable_{false};
};

class ConnectionSlot : public db_backend::WorkerSlot<Job, ConnectionState> {
public:
    using WorkerSlot::WorkerSlot;
    TransactionState transaction_state() const noexcept { return state()->transaction_state(); }
    void enable_recovery() noexcept { state()->enable_recovery(); }
    bool recoverable() const noexcept { return state()->recoverable(); }
};

// Completion callbacks must not own ConnectionPool/ConnectionSlot. Otherwise
// forced actor exit could release the last pool owner on its own worker thread.
// This registry contains weak state references only, never worker ownership.
class TransactionRegistry {
public:
    bool insert(uint64_t token, const std::shared_ptr<ConnectionState>& state,
                uint32_t generation, const std::string& request_key) {
        std::lock_guard lock{mutex_};
        if ((!request_key.empty() && keys_.contains(request_key)) || entries_.contains(token))
            return false;
        entries_.emplace(token, Entry{state, generation, request_key});
        try {
            if (!request_key.empty()) keys_.emplace(request_key, token);
        } catch (...) {
            entries_.erase(token);
            throw;
        }
        return true;
    }

    uint64_t find(const std::string& key) const {
        std::lock_guard lock{mutex_};
        auto it = keys_.find(key);
        return it == keys_.end() ? 0 : it->second;
    }

    bool release_transaction(uint64_t token) {
        std::lock_guard lock{mutex_};
        auto it = entries_.find(token);
        if (it == entries_.end()) return false;
        auto state = it->second.state.lock();
        const bool released = state && state->transaction_.release(it->second.generation);
        if (!it->second.key.empty()) keys_.erase(it->second.key);
        entries_.erase(it);
        return released;
    }

private:
    struct Entry {
        std::weak_ptr<ConnectionState> state;
        uint32_t generation;
        std::string key;
    };
    mutable std::mutex mutex_;
    std::map<uint64_t, Entry> entries_;
    std::map<std::string, uint64_t> keys_;
};

struct Route {
    std::shared_ptr<ConnectionSlot> slot;
    uint64_t transaction = 0;
    std::string error;
    db::error_code code = db::error_code::none;
    explicit operator bool() const noexcept { return slot != nullptr; }
    bool enqueue(std::shared_ptr<Job> job) const { return slot->enqueue(std::move(job)); }
};

class ConnectionPool {
public:
    using slot_ptr = std::shared_ptr<ConnectionSlot>;

    explicit ConnectionPool(std::string backend) : backend_(std::move(backend)) {}
    ~ConnectionPool() { stop_and_join(); }

    slot_ptr add_slot(const std::string& name) {
        if (indexes_.try_emplace(name, names_.size()).second) names_.push_back(name);
        return workers_.add_slot(name);
    }

    std::shared_ptr<TransactionRegistry> transactions() const noexcept { return transactions_; }

    Route route_idle(const std::string& name) {
        const auto* slots = workers_.find_slots(name);
        if (!slots || slots->empty())
            return failed("unknown " + backend_ + " connection: " + name);
        auto slot = workers_.route_round_robin(name, [](const slot_ptr& candidate) {
            return candidate->transaction_state() == TransactionState::Idle;
        });
        if (slot) return Route{std::move(slot), 0, {}};
        return failed(backend_ + " pool exhausted (all connections stopped or in transactions)");
    }

    Route acquire_transaction(const std::string& name, const std::string& request_key = {}) {
        if (!request_key.empty() && transactions_->find(request_key) != 0)
            return failed("duplicate " + backend_ + " transaction request key");
        const auto* slots = workers_.find_slots(name);
        const auto index = indexes_.find(name);
        if (!slots || slots->empty() || index == indexes_.end())
            return failed("unknown " + backend_ + " connection: " + name);
        if (index->second > max_index)
            return failed(backend_ + " transaction pool index exceeds handle capacity");

        uint32_t generation = 0;
        auto slot = workers_.route_round_robin(name, [&](const slot_ptr& candidate) {
            generation = candidate->state()->transaction_.try_acquire();
            return generation != 0;
        });
        if (!slot) return failed(backend_ + " transaction pool exhausted: " + name);
        const auto position = std::find(slots->begin(), slots->end(), slot);
        const auto slot_index = static_cast<size_t>(position - slots->begin());
        auto state = slot->state();
        if (slot_index > max_index) {
            state->transaction_.release(generation);
            return failed(backend_ + " transaction slot index exceeds handle capacity");
        }
        const auto token = encode(index->second, slot_index, generation);
        try {
            if (!transactions_->insert(token, state, generation, request_key)) {
                state->transaction_.release(generation);
                return failed("duplicate " + backend_ + " transaction request key");
            }
        } catch (...) {
            state->transaction_.release(generation);
            throw;
        }
        return Route{std::move(slot), token, {}};
    }

    Route route_transaction(uint64_t token, bool closing = false) {
        const auto parts = decode(token);
        if (parts.pool_index >= names_.size())
            return failed("invalid " + backend_ + " transaction: " + std::to_string(token));
        const auto* slots = workers_.find_slots(names_[parts.pool_index]);
        if (!slots || parts.slot_index >= slots->size())
            return failed("invalid " + backend_ + " transaction: " + std::to_string(token));
        auto slot = (*slots)[parts.slot_index];
        auto state = slot->state();
        auto& transaction = state->transaction_;
        if (transaction.generation() != parts.generation)
            return failed("inactive " + backend_ + " transaction: " + std::to_string(token));
        const auto status = transaction.state();
        if (closing) {
            if (status == TransactionState::Closing)
                return failed("closing " + backend_ + " transaction: " + std::to_string(token));
            if (status != TransactionState::Active || !transaction.try_close(parts.generation))
                return failed("inactive " + backend_ + " transaction: " + std::to_string(token));
        } else if (status != TransactionState::Active) {
            return failed("inactive " + backend_ + " transaction: " + std::to_string(token));
        }
        return Route{std::move(slot), token, {}};
    }

    // Request keys let cancellation find BEGIN even before its handle reply.
    Route route_transaction(const std::string& request_key, bool closing = false) {
        const auto token = transactions_->find(request_key);
        if (token == 0) return failed("inactive " + backend_ + " transaction request key");
        return route_transaction(token, closing);
    }

    bool release_transaction(uint64_t token) { return transactions_->release_transaction(token); }
    size_t stop_and_join() { return workers_.stop_and_join(); }

private:
    static constexpr size_t max_index = static_cast<size_t>(std::numeric_limits<uint16_t>::max());
    struct HandleParts { size_t pool_index; size_t slot_index; uint32_t generation; };
    static uint64_t encode(size_t pool_index, size_t slot_index, uint32_t generation) noexcept {
        return (static_cast<uint64_t>(generation) << 32)
               | (static_cast<uint64_t>(pool_index) << 16) | static_cast<uint64_t>(slot_index);
    }
    static HandleParts decode(uint64_t token) noexcept {
        return {static_cast<size_t>((token >> 16) & 0xffffu),
                static_cast<size_t>(token & 0xffffu), static_cast<uint32_t>(token >> 32)};
    }
    static Route failed(std::string error,
                        db::error_code code = db::error_code::connection_unavailable) {
        return Route{nullptr, 0, std::move(error), code};
    }

    std::string backend_;
    db_backend::WorkerPool<ConnectionSlot> workers_;
    std::map<std::string, size_t> indexes_;
    std::vector<std::string> names_;
    std::shared_ptr<TransactionRegistry> transactions_ = std::make_shared<TransactionRegistry>();
};

} // namespace caf_plugin_system::sql_backend
