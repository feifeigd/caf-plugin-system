// Exercise the real driver/worker, not a fake SqlConnection. The Docker runner
// supplies one disposable MySQL instance and invokes this executable normally.
#include "../plugins/mysql/mysql_plugin.cpp"

#include <chrono>
#include <cstdlib>
#include <future>
#include <stdexcept>
#include <string_view>

#if defined(_WIN32) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

struct DebugHeapCheck {
    DebugHeapCheck() {
#if defined(_WIN32) && defined(_DEBUG)
        _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    }
} debug_heap_check;

void require(bool condition, const std::string& error) {
    if (!condition)
        throw std::runtime_error(error);
}

std::string environment(const char* name, bool allow_empty = false) {
    const auto* value = std::getenv(name);
    require(value != nullptr && (allow_empty || *value != '\0'),
            std::string{"missing environment variable: "} + name);
    return value;
}

SqlSpec connection_spec() {
    SqlSpec result;
    result.name = "mysql-cancellation";
    const auto* host = std::getenv("MYSQL_TEST_HOST");
    result.host = host && *host ? host : "127.0.0.1";
    const auto port = environment("MYSQL_TEST_PORT");
    size_t parsed = 0;
    const auto number = std::stoul(port, &parsed);
    require(parsed == port.size() && number > 0 && number <= 65535,
            "MYSQL_TEST_PORT must be a valid TCP port");
    result.port = static_cast<unsigned>(number);
    result.user = environment("MYSQL_TEST_USER");
    result.pass = environment("MYSQL_TEST_PASSWORD", true);
    result.dbname = environment("MYSQL_TEST_DATABASE");
    return result;
}

class MySqlLibrary final {
public:
    MySqlLibrary() {
        require(mysql_library_init(0, nullptr, nullptr) == 0,
                "mysql_library_init failed");
    }
    ~MySqlLibrary() { mysql_library_end(); }
    MySqlLibrary(const MySqlLibrary&) = delete;
    MySqlLibrary& operator=(const MySqlLibrary&) = delete;
};

// This independent observer sees only sessions belonging to the same user, so
// no PROCESS privilege or administrative KILL permission is required.
class SessionObserver final {
public:
    explicit SessionObserver(const SqlSpec& spec)
        : handle_(mysql_init(nullptr), mysql_close) {
        require(handle_ != nullptr, "observer mysql_init failed");
        const unsigned timeout = 2;
        mysql_options(handle_.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
        mysql_options(handle_.get(), MYSQL_OPT_READ_TIMEOUT, &timeout);
        mysql_options(handle_.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout);
        if (!mysql_real_connect(handle_.get(), spec.host.c_str(), spec.user.c_str(),
                                spec.pass.c_str(), spec.dbname.c_str(), spec.port,
                                nullptr, 0))
            throw std::runtime_error("observer connection failed: "
                                     + std::string{mysql_error(handle_.get())});
    }

    std::string scalar(const std::string& statement) {
        if (mysql_real_query(handle_.get(), statement.data(),
                             static_cast<unsigned long>(statement.size())) != 0)
            throw std::runtime_error("observer query failed: "
                                     + std::string{mysql_error(handle_.get())});
        std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> result{
            mysql_store_result(handle_.get()), mysql_free_result};
        require(result != nullptr, "observer query returned no result set");
        const auto row = mysql_fetch_row(result.get());
        require(row && row[0], "observer query returned no scalar");
        const auto* lengths = mysql_fetch_lengths(result.get());
        require(lengths != nullptr, "observer query returned no column lengths");
        return {row[0], lengths[0]};
    }

    void await_active(uint64_t id, std::future<db::db_result>& completion) {
        const auto deadline = Clock::now() + 2s;
        do {
            require(completion.wait_for(0ms) != std::future_status::ready,
                    "slow query finished before its execution was observed");
            if (scalar("SELECT COUNT(*) FROM information_schema.PROCESSLIST WHERE ID = "
                       + std::to_string(id) + " AND COMMAND <> 'Sleep'") == "1")
                return;
            std::this_thread::sleep_for(20ms);
        } while (Clock::now() < deadline);
        throw std::runtime_error("slow query never became active on the real server");
    }

    void await_closed(uint64_t id) {
        // Closing a client socket need not interrupt server-side SLEEP. The
        // server may only notice the disconnect on its next attempted write.
        // This budget is separate from the strict client stop/join budget.
        const auto deadline = Clock::now() + 10s;
        do {
            if (scalar("SELECT COUNT(*) FROM information_schema.PROCESSLIST WHERE ID = "
                       + std::to_string(id)) == "0")
                return;
            std::this_thread::sleep_for(50ms);
        } while (Clock::now() < deadline);
        throw std::runtime_error("stopped worker left its physical server connection alive");
    }

private:
    std::unique_ptr<MYSQL, decltype(&mysql_close)> handle_;
};

class WorkerFixture final {
public:
    explicit WorkerFixture(const SqlSpec& spec) : slot_(pool_.add_slot(spec.name)) {
        ReconnectPolicy policy;
        policy.max_attempts = 1;
        // Deliberately longer than our stop/join budget. A passing test cannot
        // be explained by the driver's ordinary read timeout expiring.
        policy.io_timeout_seconds = 5;
        slot_->start_worker(sql_worker_main, spec, policy);
    }

    std::future<db::db_result> submit(Op operation, const std::string& statement) {
        auto completion = std::make_shared<std::promise<db::db_result>>();
        auto future = completion->get_future();
        auto job = std::make_shared<Job>();
        job->op = operation;
        job->sql = statement;
        job->done = [completion](db::db_result& result) {
            completion->set_value(std::move(result));
        };
        require(slot_->enqueue(std::move(job)), "worker rejected a test job");
        return future;
    }

    uint64_t warmup() {
        auto completion = submit(Op::Query, "SELECT CONNECTION_ID()");
        require(completion.wait_for(5s) == std::future_status::ready,
                "worker warmup timed out");
        const auto result = completion.get();
        require(result.ok && result.rows.size() == 1 && result.rows.front().size() == 1,
                "worker warmup failed: " + result.error);
        return std::stoull(result.rows.front().front());
    }

    size_t stop_and_join() {
        slot_->stop();
        return pool_.stop_and_join();
    }

private:
    SqlPool pool_{"mysql"};
    std::shared_ptr<ConnSlot> slot_;
};

class CancellationRegression final {
public:
    explicit CancellationRegression(SqlSpec spec)
        : spec_(std::move(spec)), observer_(spec_) {}

    void run() {
        std::cout << "[MySqlCancellationTest] server="
                  << observer_.scalar("SELECT VERSION()") << '\n';
        verify("continuous-stream", Op::Query,
               "WITH RECURSIVE seq(n) AS (SELECT 1 UNION ALL "
               "SELECT n + 1 FROM seq WHERE n < 80) "
               "SELECT REPEAT('x', 32768), SLEEP(0.1) FROM seq", 700ms);
        verify("sleep-query", Op::Query, "SELECT SLEEP(8)", 100ms);
        verify("sleep-exec", Op::Exec, "DO SLEEP(8)", 100ms);
    }

private:
    void verify(std::string_view name, Op operation, const std::string& statement,
                std::chrono::milliseconds receive_window) {
        WorkerFixture worker{spec_};
        const auto id = worker.warmup();
        auto completion = worker.submit(operation, statement);
        observer_.await_active(id, completion);
        // The streaming case keeps producing network data for eight seconds;
        // receiving some rows must not reset or defeat the shutdown budget.
        std::this_thread::sleep_for(receive_window);
        require(completion.wait_for(0ms) != std::future_status::ready,
                std::string{name} + ": query was not pending at stop");
        const auto started = Clock::now();
        const auto joined = worker.stop_and_join();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - started);
        require(joined == 1, std::string{name} + ": worker was not joined exactly once");
        require(completion.wait_for(0ms) == std::future_status::ready,
                std::string{name} + ": worker exited without completing its active request");
        const auto result = completion.get();
        require(elapsed <= 2s,
                std::string{name} + ": stop/join exceeded 2000 ms (actual "
                    + std::to_string(elapsed.count()) + " ms)");
        require(!result.ok, std::string{name} + ": cancelled SQL incorrectly reported success");
        require(db::is_connection_error(result.code),
                std::string{name} + ": cancellation was not classified as a connection error");
        if (operation == Op::Exec)
            require(result.code == db::error_code::outcome_unknown,
                    "cancelled autocommit execution must report outcome_unknown");
        require(worker.stop_and_join() == 0,
                std::string{name} + ": repeated stop joined a worker twice");
        observer_.await_closed(id);
        require(observer_.scalar("SELECT 1") == "1",
                std::string{name} + ": stopping one worker damaged another connection");
        std::cout << "[MySqlCancellationTest] " << name
                  << " PASS join_ms=" << elapsed.count()
                  << " result=" << db::to_string(result.code)
                  << " server_connection_closed=1\n";
    }

    SqlSpec spec_;
    SessionObserver observer_;
};

} // namespace

int main() {
    try {
        {
            MySqlLibrary library;
            CancellationRegression regression{connection_spec()};
            regression.run();
        } // Native connections, workers, and the client library are released.
        std::cout << "[MySqlCancellationTest] PASS natural exit\n";
        return 0; // Keep CRT teardown active: never ExitProcess/quick_exit.
    } catch (const std::exception& error) {
        std::cerr << "[MySqlCancellationTest] FAIL " << error.what() << '\n';
        return 1;
    }
}
