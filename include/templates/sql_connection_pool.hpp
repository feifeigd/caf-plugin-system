#pragma once

// SQL 插件共享的连接池与事务路由。
// 驱动连接和语句执行仍由各插件负责；这里只管理 worker 队列、连接槽、
// 普通请求选路以及带代际号的短生命周期事务句柄。

#include "common/db_contract.hpp"
#include "templates/job_queue.hpp"

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

class ConnectionSlot {
public:
    using job_ptr = std::shared_ptr<Job>;

    bool enqueue(job_ptr job) {
        return queue_->push(std::move(job));
    }

    job_ptr next_job() {
        return queue_->pop();
    }

    void fail_pending(const std::string& error) {
        stop_source_.request_stop();
        queue_->fail_all(error);
    }

    bool stopped() const { return stop_source_.stop_requested() || queue_->stopped(); }

    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    bool wait_for_stop(std::chrono::milliseconds timeout) {
        return queue_->wait_for_stop(timeout);
    }

    void enable_recovery() noexcept { recoverable_.store(true); }
    bool recoverable() const noexcept { return recoverable_.load(); }

    template <class Function, class... Args>
    void start_worker(Function&& function, Args&&... args) {
        if (worker_ && worker_->joinable())
            throw std::logic_error("connection slot worker already started");
        worker_ = std::make_shared<std::thread>(
            std::forward<Function>(function), std::forward<Args>(args)...);
    }

    void stop() {
        // Callbacks only interrupt I/O; the owning worker still performs all
        // native connection cleanup. Do not hold queue/transaction locks here.
        stop_source_.request_stop();
        queue_->stop();
    }

    bool join_worker() {
        if (!worker_ || !worker_->joinable())
            return false;
        if (worker_->get_id() == std::this_thread::get_id()) {
            worker_->detach();
            return false;
        }
        worker_->join();
        return true;
    }

    TransactionState transaction_state() const noexcept {
        return transaction_.state();
    }

private:
    friend class ConnectionPool;

    std::shared_ptr<JobQueue> queue_ = std::make_shared<JobQueue>();
    std::stop_source stop_source_;
    std::shared_ptr<std::thread> worker_;
    TransactionSlot transaction_;
    std::atomic<bool> recoverable_{false};
};

struct Route {
    std::shared_ptr<ConnectionSlot> slot;
    uint64_t transaction = 0;
    std::string error;
    db::error_code code = db::error_code::none;

    explicit operator bool() const noexcept { return slot != nullptr; }

    bool enqueue(std::shared_ptr<Job> job) const {
        return slot->enqueue(std::move(job));
    }
};

class ConnectionPool {
public:
    using slot_ptr = std::shared_ptr<ConnectionSlot>;

    explicit ConnectionPool(std::string backend)
        : backend_(std::move(backend)) {
    }

    ~ConnectionPool() {
        stop_and_join();
    }

    slot_ptr add_slot(const std::string& name) {
        const auto inserted = indexes_.try_emplace(name, names_.size()).second;
        if (inserted) {
            names_.push_back(name);
            pools_.try_emplace(name);
        }
        auto slot = std::make_shared<ConnectionSlot>();
        pools_[name].push_back(slot);
        return slot;
    }

    Route route_idle(const std::string& name) {
        auto it = pools_.find(name);
        if (it == pools_.end() || it->second.empty())
            return failed("unknown " + backend_ + " connection: " + name);
        auto& slots = it->second;
        auto start = round_robin_.fetch_add(1, std::memory_order_relaxed);
        for (size_t offset = 0; offset < slots.size(); ++offset) {
            auto index = (start + offset) % slots.size();
            if (!slots[index]->stopped() && slots[index]->transaction_.idle())
                return Route{slots[index], 0, {}};
        }
        return failed(backend_
                      + " pool exhausted (all connections in transactions)");
    }

    Route acquire_transaction(const std::string& name,
                              const std::string& request_key = {}) {
        std::lock_guard lock{transaction_keys_mutex_};
        if (!request_key.empty() && keyed_transactions_.contains(request_key))
            return failed("duplicate " + backend_ + " transaction request key");
        auto pool_it = pools_.find(name);
        auto index_it = indexes_.find(name);
        if (pool_it == pools_.end() || index_it == indexes_.end()
            || pool_it->second.empty())
            return failed("unknown " + backend_ + " connection: " + name);
        const auto pool_index = index_it->second;
        if (pool_index > max_index)
            return failed(backend_ + " transaction pool index exceeds handle capacity");
        auto& slots = pool_it->second;
        auto start = round_robin_.fetch_add(1, std::memory_order_relaxed);
        for (size_t offset = 0; offset < slots.size(); ++offset) {
            auto slot_index = (start + offset) % slots.size();
            if (slots[slot_index]->stopped())
                continue;
            auto generation = slots[slot_index]->transaction_.try_acquire();
            if (generation == 0)
                continue;
            if (slot_index > max_index) {
                slots[slot_index]->transaction_.release(generation);
                return failed(backend_
                              + " transaction slot index exceeds handle capacity");
            }
            auto tx = encode(pool_index, slot_index, generation);
            if (!request_key.empty()) {
                keyed_transactions_.emplace(request_key, tx);
                transaction_keys_.emplace(tx, request_key);
            }
            return Route{slots[slot_index], tx, {}};
        }
        return failed(backend_ + " transaction pool exhausted: " + name);
    }

    Route route_transaction(uint64_t tx, bool closing = false) {
        const auto parts = decode(tx);
        if (parts.pool_index >= names_.size())
            return failed("invalid " + backend_ + " transaction: "
                          + std::to_string(tx));
        auto pool_it = pools_.find(names_[parts.pool_index]);
        if (pool_it == pools_.end() || parts.slot_index >= pool_it->second.size())
            return failed("invalid " + backend_ + " transaction: "
                          + std::to_string(tx));
        auto slot = pool_it->second[parts.slot_index];
        if (slot->transaction_.generation() != parts.generation)
            return failed("inactive " + backend_ + " transaction: "
                          + std::to_string(tx));
        const auto state = slot->transaction_.state();
        if (closing) {
            if (state == TransactionState::Closing)
                return failed("closing " + backend_ + " transaction: "
                              + std::to_string(tx));
            if (state != TransactionState::Active
                || !slot->transaction_.try_close(parts.generation))
                return failed("inactive " + backend_ + " transaction: "
                              + std::to_string(tx));
        } else if (state != TransactionState::Active) {
            return failed("inactive " + backend_ + " transaction: "
                          + std::to_string(tx));
        }
        return Route{std::move(slot), tx, {}};
    }

    // BEGIN 回执可能晚于调用方超时。关联键在投递 BEGIN 前已登记，调用方
    // 无须知道 tx_handle 即可取消；ROLLBACK 会排在同一连接的 BEGIN 后。
    Route route_transaction(const std::string& request_key,
                            bool closing = false) {
        std::lock_guard lock{transaction_keys_mutex_};
        const auto it = keyed_transactions_.find(request_key);
        if (it == keyed_transactions_.end())
            return failed("inactive " + backend_ + " transaction request key");
        return route_transaction(it->second, closing);
    }

    bool release_transaction(uint64_t tx) {
        std::lock_guard lock{transaction_keys_mutex_};
        const auto parts = decode(tx);
        if (parts.pool_index >= names_.size())
            return false;
        auto pool_it = pools_.find(names_[parts.pool_index]);
        if (pool_it == pools_.end() || parts.slot_index >= pool_it->second.size())
            return false;
        const auto released =
            pool_it->second[parts.slot_index]->transaction_.release(
                parts.generation);
        if (released) {
            const auto key = transaction_keys_.find(tx);
            if (key != transaction_keys_.end()) {
                keyed_transactions_.erase(key->second);
                transaction_keys_.erase(key);
            }
        }
        return released;
    }

    size_t stop_and_join() {
        for (auto& [name, slots] : pools_)
            for (auto& slot : slots)
                slot->stop();
        size_t joined = 0;
        for (auto& [name, slots] : pools_)
            for (auto& slot : slots)
                if (slot->join_worker())
                    ++joined;
        return joined;
    }

private:
    static constexpr size_t max_index =
        static_cast<size_t>(std::numeric_limits<uint16_t>::max());

    /// 事务句柄的组成部分，用于从 64 位句柄中解码。
    struct HandleParts {
        size_t pool_index;     ///< 连接池索引
        size_t slot_index;     ///< 插槽索引
        uint32_t generation;   ///< 生成号
    };

    static uint64_t encode(size_t pool_index, size_t slot_index,
                           uint32_t generation) noexcept {
        return (static_cast<uint64_t>(generation) << 32)
               | (static_cast<uint64_t>(pool_index) << 16)
               | static_cast<uint64_t>(slot_index);
    }

    static HandleParts decode(uint64_t tx) noexcept {
        return {
            static_cast<size_t>((tx >> 16) & 0xffffu),
            static_cast<size_t>(tx & 0xffffu),
            static_cast<uint32_t>(tx >> 32),
        };
    }

    static Route failed(std::string error,
                        db::error_code code = db::error_code::connection_unavailable) {
        return Route{nullptr, 0, std::move(error), code};
    }

    std::string backend_;
    std::map<std::string, std::vector<slot_ptr>> pools_;
    std::map<std::string, size_t> indexes_;
    std::vector<std::string> names_;
    std::atomic<size_t> round_robin_{0};
    std::mutex transaction_keys_mutex_;
    std::map<std::string, uint64_t> keyed_transactions_;
    std::map<uint64_t, std::string> transaction_keys_;
};

} // namespace caf_plugin_system::sql_backend
