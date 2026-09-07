#include "templates/db_worker_pool.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {
using namespace std::chrono_literals;
namespace backend = caf_plugin_system::db_backend;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Job {
    int id = 0;
    std::function<void(const std::string&)> failed;
    void fail(const std::string& message) { if (failed) failed(message); }
};
using State = backend::WorkerState<Job>;
using Slot = backend::WorkerSlot<Job>;
using Pool = backend::WorkerPool<Slot>;

class PoolTests {
public:
    void run() {
        routing();
        capacity_and_lifetime();
        stop_and_callbacks();
        fifo_and_join();
        ownership();
        stop_all_before_join();
        worker_failure();
        unstarted_cleanup();
        concurrent_stop();
    }

private:
    static std::shared_ptr<Job> job(int id = 0) {
        auto result = std::make_shared<Job>();
        result->id = id;
        return result;
    }

    static void routing() {
        Pool pool;
        auto first = pool.add_slot("main");
        auto second = pool.add_slot("main");
        auto isolated = pool.add_slot("other");
        require(pool.find_slots("main")->size() == 2, "named pool size");
        require(!pool.route_round_robin("missing"), "unknown pool accepted");
        require(pool.route_round_robin("main") == first, "round robin first");
        require(pool.route_round_robin("main") == second, "round robin second");
        require(pool.route_round_robin("main", [&](const auto& slot) {
            return slot == second;
        }) == second, "adapter eligibility predicate ignored");
        auto pinned = pool.route_affine("main", "store:partition");
        for (int i = 0; i < 100; ++i)
            require(pool.route_affine("main", "store:partition") == pinned,
                    "key affinity changed");
        pinned->stop();
        require(!pool.route_affine("main", "store:partition"), "affinity failed over");
        require(pool.route_round_robin("main") != pinned, "stopped slot selected");
        require(pool.route_round_robin("other") == isolated, "named pool isolation");
        pool.stop_and_join();
        require(!pool.route_round_robin("other"), "stopped pool routed work");
        bool rejected = false;
        try { pool.add_slot("late"); } catch (const std::logic_error&) { rejected = true; }
        require(rejected, "stopped pool restarted");
    }

    static void capacity_and_lifetime() {
        State state{{2, false}};
        auto external = job(1);
        require(state.enqueue(external), "first admission failed");
        require(state.enqueue(job(2)), "second admission failed");
        auto executing = state.next_job();
        require(executing->id == 1 && state.pending() == 2, "active work not counted");
        int rejected = 0;
        auto overflow = job();
        overflow->failed = [&](const auto& error) {
            require(error.find("full") != std::string::npos, "wrong overflow error");
            ++rejected;
        };
        require(!state.enqueue(overflow) && rejected == 1, "capacity not enforced");
        executing.reset();
        require(state.pending() == 1, "external original Job retained admission");
        require(state.enqueue(job(3)), "capacity not restored");
        state.fail_pending("test cleanup");
        require(state.pending() == 0, "pending admission leaked");
        require(!state.next_job(), "failed queue not emptied");
    }

    static void stop_and_callbacks() {
        State state{{0, true}};
        int failures = 0;
        auto throwing = job();
        throwing->failed = [&](const auto&) { ++failures; throw std::runtime_error("test"); };
        auto reentrant = job();
        reentrant->failed = [&](const auto&) {
            require(state.stopped(), "stop callback ran before stop");
            auto nested = job();
            nested->failed = [&](const auto&) { ++failures; };
            require(!state.enqueue(nested), "reentrant submission accepted after stop");
            ++failures;
        };
        state.enqueue(throwing);
        state.enqueue(reentrant);
        state.stop();
        require(failures == 3, "throwing/reentrant completion stranded other work");
        require(state.pending() == 0 && !state.next_job(), "stop leaked queued jobs");
        require(state.wait_for_stop(1s), "stopped wait did not finish");
        state.stop();
        require(failures == 3, "stop delivered twice");
    }

    static void fifo_and_join() {
        Pool pool;
        auto slot = pool.add_slot("main");
        std::vector<int> seen;
        for (int i = 0; i < 100; ++i) slot->enqueue(job(i));
        slot->start_worker([&](std::shared_ptr<State> state) {
            while (auto current = state->next_job()) seen.push_back(current->id);
        });
        bool duplicate = false;
        try { slot->start_worker([](auto) {}); }
        catch (const std::logic_error&) { duplicate = true; }
        require(duplicate, "double-start accepted");
        require(pool.stop_and_join() == 1, "worker not joined");
        require(pool.stop_and_join() == 0, "worker joined twice");
        require(seen.size() == 100, "drain lost queued jobs");
        for (int i = 0; i < 100; ++i) require(seen[i] == i, "FIFO order changed");
        require(slot->pending() == 0, "drained admission leaked");
    }

    static void ownership() {
        std::weak_ptr<Slot> weak_slot;
        std::weak_ptr<State> weak_state;
        std::atomic<int> exited{0};
        {
            Pool pool;
            auto slot = pool.add_slot("owned");
            weak_slot = slot;
            weak_state = slot->state();
            slot->start_worker([&](auto state) {
                while (state->next_job()) {}
                ++exited;
            });
        }
        require(exited == 1, "pool destructor did not join worker");
        require(weak_slot.expired() && weak_state.expired(), "worker ownership cycle");
    }

    static void stop_all_before_join() {
        Pool pool;
        auto first = pool.add_slot("first");
        auto last = pool.add_slot("last");
        std::atomic<bool> all_stopped{false};
        first->start_worker([&all_stopped, other = last->state()](auto) {
            all_stopped = other->wait_for_stop(2s);
        });
        last->start_worker([](auto state) { while (state->next_job()) {} });
        require(pool.stop_and_join() == 2, "multi-worker join count");
        require(all_stopped, "joined a worker before stopping all workers");
    }

    static void worker_failure() {
        Slot slot;
        std::atomic<int> failures{0};
        for (int i = 0; i < 3; ++i) {
            auto pending = job();
            pending->failed = [&](const auto&) { ++failures; };
            slot.enqueue(pending);
        }
        slot.start_worker([](auto) { throw std::runtime_error("injected worker failure"); });
        slot.join_worker();
        require(failures == 3 && slot.pending() == 0 && slot.stopped(),
                "failed worker stranded its queue");
    }

    static void unstarted_cleanup() {
        int failures = 0;
        {
            Slot slot;
            auto pending = job();
            pending->failed = [&](const auto&) { ++failures; };
            slot.enqueue(pending);
        }
        require(failures == 1, "unstarted slot stranded a submission");
    }

    static void concurrent_stop() {
        for (int round = 0; round < 20; ++round) {
            State state{{4, true}};
            std::atomic<int> failures{0};
            std::vector<std::thread> producers;
            for (int producer = 0; producer < 4; ++producer) {
                producers.emplace_back([&] {
                    for (int i = 0; i < 100; ++i) {
                        auto pending = job();
                        pending->failed = [&](const auto&) { ++failures; };
                        state.enqueue(pending);
                    }
                });
            }
            state.stop();
            for (auto& producer : producers) producer.join();
            require(failures == 400 && state.pending() == 0,
                    "enqueue/stop race lost or double-failed a job");
        }
    }
};
} // namespace

int main() {
#if defined(_WIN32) && defined(_DEBUG)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    try {
        PoolTests{}.run();
        std::puts("Database worker pool tests passed; all workers joined");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Database worker pool test failed: %s\n", error.what());
        return 1;
    }
}
