#pragma once
// ------------------------------------------------------------------
// 线程安全任务队列（数据库插件 worker 池通用模板）
//
// 使用要求：Job 类型必须提供 `void fail(const std::string& err)` 成员，
// 定义"失败如何交付"——Redis 版实现为 rp.deliver(make_error(...))，
// SQL 系实现为构造 db_result 错误后调用 done(r)。队列本身只负责
// 存/取/停，零业务知识。
//
// 语义（四个数据库插件逐字复制的版本统一收敛于此）：
//   - push()：运行中入队；停止后立即回错误，绝不把任务留在无人消费的队列
//   - pop()：阻塞直到有 job 或 stop；返回 nullptr = 应退出
//   - fail_all()：清空队列 + 全部回错误 + 停（连接失败场景）
//   - stop()：拒绝新任务并唤醒；pop() 仍取出残留任务，由 worker 决定执行或拒绝
// ------------------------------------------------------------------

#include <condition_variable>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace caf_plugin_system {

template <class Job>
class JobQueue {
public:
    using job_ptr = std::shared_ptr<Job>;

    bool push(job_ptr j) {
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lk(m);
            if (running) {
                jobs.push_back(std::move(j));
                accepted = true;
            }
        }
        if (accepted) {
            cv.notify_one();
            return true;
        }
        if (j)
            j->fail("database worker queue is stopped");
        return false;
    }

    // 返回 nullptr = 应退出（stop 已调用且队列清空）
    job_ptr pop() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [this] { return !jobs.empty() || !running; });
        if (!jobs.empty()) {
            auto j = std::move(jobs.front());
            jobs.pop_front();
            return j;
        }
        return nullptr;
    }

    void fail_all(const std::string& err) {
        std::deque<job_ptr> rest;
        {
            std::lock_guard<std::mutex> lk(m);
            rest.swap(jobs);
            running = false;
        }
        cv.notify_all();
        for (auto& j : rest) {
            // One broken completion must not strand the remaining jobs or
            // prevent a worker/slot destructor from finishing shutdown.
            try { j->fail(err); }
            catch (...) { /* Completion callbacks must not throw. */ }
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(m);
            running = false;
        }
        cv.notify_all();
    }

    bool stopped() const {
        std::lock_guard<std::mutex> lk(m);
        return !running;
    }

    // New work must not shorten the reconnect backoff; shutdown must.
    bool wait_for_stop(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(m);
        return cv.wait_for(lk, timeout, [this] { return !running; });
    }

private:
    mutable std::mutex m;
    std::condition_variable cv;
    std::deque<job_ptr> jobs;
    bool running = true;
};

} // namespace caf_plugin_system
