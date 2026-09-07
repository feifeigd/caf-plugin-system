#pragma once

// Database-neutral worker ownership and routing. Native connections and
// transaction/retry semantics belong to the adapter, never to this layer.
#include "templates/job_queue.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>
#include <stop_token>

namespace caf_plugin_system::db_backend {

struct WorkerOptions {
    size_t max_pending = 0; // Zero means unlimited; counts queued + executing.
    bool fail_pending_on_stop = false; // 停止时拒绝尚在队列中的任务；已取出的任务由 worker 处理取消。
};

// Shared by producer and worker, but owns no thread or slot. Keeping this
// separate from WorkerSlot prevents a slot -> thread -> slot ownership cycle.
template <class Job>
class WorkerState {
public:
    using job_ptr = std::shared_ptr<Job>;

    explicit WorkerState(WorkerOptions options = {}) : options_(options) {}

    bool enqueue(job_ptr job) {
        if (!job)
            throw std::invalid_argument("cannot enqueue a null database job");
        if (stopped()) {
            job->fail("database worker queue is stopped");
            return false;
        }
        auto count = pending_->load(std::memory_order_relaxed);
        do {
            if (options_.max_pending != 0 && count >= options_.max_pending) {
                job->fail("database worker request queue is full");
                return false;
            }
        } while (!pending_->compare_exchange_weak(count, count + 1,
                                                  std::memory_order_relaxed));  // CAS 失败后重新检查上限；weak 允许伪失败。

        // An aliasing shared_ptr ties admission to the queue/worker's reference,
        // not the caller's original Job reference. Adapters need no release hook.
        std::shared_ptr<PendingJob> owner;
        try {
            owner = std::make_shared<PendingJob>(job, pending_);
        } catch (...) {
            pending_->fetch_sub(1, std::memory_order_relaxed);
            throw;
        }

        // 别名 shared_ptr：访问 Job，但共享 PendingJob 的控制块。
        // 队列/worker 释放最后一份 admitted 引用时回收计数，不受调用方原始 job 引用影响。
        job_ptr admitted{owner, job.get()};
        return queue_.push(std::move(admitted)); // Rechecks stop under queue lock.
    }

    job_ptr next_job() { return queue_.pop(); }

    void fail_pending(const std::string& error) {
        stop_source_.request_stop();
        queue_.fail_all(error);
    }

    void stop() {
        stop_source_.request_stop();
        if (options_.fail_pending_on_stop)
            queue_.fail_all("database worker is stopping");
        else
            queue_.stop(); // Adapter consumes/rejects queued jobs before exit.
    }

    bool stopped() const {
        return stop_source_.stop_requested() || queue_.stopped();
    }
    std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }
    bool wait_for_stop(std::chrono::milliseconds timeout) {
        return queue_.wait_for_stop(timeout);
    }
    size_t pending() const noexcept { return pending_->load(std::memory_order_relaxed); }

private:
    struct PendingJob {
        PendingJob(job_ptr value, std::shared_ptr<std::atomic<size_t>> counter)
            : job(std::move(value)), pending(std::move(counter)) {}
        ~PendingJob() { pending->fetch_sub(1, std::memory_order_relaxed); }
        job_ptr job;
        std::shared_ptr<std::atomic<size_t>> pending;
    };

    WorkerOptions options_;
    JobQueue<Job> queue_;
    std::stop_source stop_source_;
    std::shared_ptr<std::atomic<size_t>> pending_ = std::make_shared<std::atomic<size_t>>(0); // 已准入且尚未释放的任务数，含排队与执行中。
};

// Lifecycle methods are called by the owning plugin actor (not concurrently).
// Worker entry receives only State, as its FIRST argument. State may be extended
// by an adapter, e.g. SQL transaction leases; it must never own the worker slot.
/// 工作线程槽：拥有一个工作线程 + 共享状态 + 队列。工作线程由插件 actor 启动，状态由工作线程和请求者共享。
template <class Job, class State = WorkerState<Job>>
class WorkerSlot {
public:
    using job_ptr = std::shared_ptr<Job>;
    using state_type = State;

    explicit WorkerSlot(WorkerOptions options = {})
        : state_(std::make_shared<State>(options)) {}
        
    ~WorkerSlot() {
        stop();
        join_worker();
        // Also settle submissions made before a worker could be started.
        state_->fail_pending("database worker slot destroyed");
    }
    WorkerSlot(const WorkerSlot&) = delete;
    WorkerSlot& operator=(const WorkerSlot&) = delete;

    std::shared_ptr<State> state() const noexcept { return state_; }
    bool enqueue(job_ptr job) { return state_->enqueue(std::move(job)); }
    job_ptr next_job() { return state_->next_job(); }
    void fail_pending(const std::string& error) { state_->fail_pending(error); }
    bool stopped() const { return state_->stopped(); }
    std::stop_token stop_token() const noexcept { return state_->stop_token(); }
    bool wait_for_stop(std::chrono::milliseconds timeout) { return state_->wait_for_stop(timeout); }
    size_t pending() const noexcept { return state_->pending(); }
    void stop() { state_->stop(); }

    template <class Function, class... Args>
    void start_worker(Function&& function, Args&&... args) {
        if (started_ || stopped())
            throw std::logic_error("database worker already started or stopped");
        worker_ = std::thread(
            [state = state_, function = std::forward<Function>(function),
             args = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                try {
                    std::apply([&](auto&&... values) {
                        std::invoke(std::move(function), state, std::move(values)...);
                    }, std::move(args));
                } catch (...) {
                    // The adapter settles its active job; pending jobs must not
                    // remain attached to an abandoned queue if a worker fails.
                    state->fail_pending("database worker terminated unexpectedly");
                }
                state->fail_pending("database worker has exited");
            });
        started_ = true;
    }

    bool join_worker() {
        if (!worker_.joinable())
            return false;
        // Never detach a database worker: native handles must finish cleanup.
        if (worker_.get_id() == std::this_thread::get_id())
            throw std::logic_error("database worker cannot join itself");
        worker_.join();
        return true;
    }

private:
    std::shared_ptr<State> state_;
    std::thread worker_;    // 工作线程
    bool started_ = false;
};

// Registry/routing operations belong to one plugin actor. Queue submission and
// worker state are thread-safe; adding slots concurrently with routing is not.
template <class Slot>
class WorkerPool {
public:
    using slot_ptr = std::shared_ptr<Slot>;
    using slots_type = std::vector<slot_ptr>;

    WorkerPool() = default;
    ~WorkerPool() { stop_and_join(); }
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    template <class... Args>
    slot_ptr add_slot(const std::string& name, Args&&... args) {
        if (stopped_)
            throw std::logic_error("cannot add a slot to a stopped database pool");
        auto slot = std::make_shared<Slot>(std::forward<Args>(args)...);
        pools_[name].push_back(slot);
        return slot;
    }

    const slots_type* find_slots(const std::string& name) const {
        auto it = pools_.find(name);
        return it == pools_.end() ? nullptr : &it->second;
    }

    template <class Predicate>
    slot_ptr route_round_robin(const std::string& name, Predicate accepts) {
        const auto* slots = find_slots(name);
        if (stopped_ || !slots || slots->empty())
            return {};
        const auto start = round_robin_++ % slots->size();
        for (size_t offset = 0; offset < slots->size(); ++offset) {
            const auto& slot = (*slots)[(start + offset) % slots->size()];
            if (!slot->stopped() && accepts(slot))
                return slot;
        }
        return {};
    }
    slot_ptr route_round_robin(const std::string& name) {
        return route_round_robin(name, [](const slot_ptr&) { return true; });
    }

    slot_ptr route_affine(const std::string& name, const std::string& key) const {
        const auto* slots = find_slots(name);
        if (stopped_ || !slots || slots->empty())
            return {};
        auto slot = (*slots)[std::hash<std::string>{}(key) % slots->size()];
        // Do not fail over to another slot: its earlier job may still be running.
        return slot->stopped() ? slot_ptr{} : slot;
    }

    size_t stop_and_join() {
        stopped_ = true;
        // Cancel ALL workers before waiting for any one driver's blocking I/O.
        for (const auto& [name, slots] : pools_)
            for (const auto& slot : slots)
                slot->stop();
        size_t joined = 0;
        for (const auto& [name, slots] : pools_)
            for (const auto& slot : slots)
                joined += slot->join_worker() ? 1 : 0;
        return joined;
    }

private:
    std::map<std::string, slots_type> pools_;
    size_t round_robin_ = 0;
    bool stopped_ = false;
};

} // namespace caf_plugin_system::db_backend
