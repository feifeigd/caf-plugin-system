#include "templates/sql_connection_pool.hpp"
#include "templates/sql_uri_config.hpp"

#include <cassert>
#include <chrono>
#include <future>
#include <memory>
#include <string>

using caf_plugin_system::sql_backend::ConnectionPool;
using caf_plugin_system::sql_backend::ConnectionUriParser;
using caf_plugin_system::sql_backend::TransactionState;

int main() {
    ConnectionPool pools{"test"};
    auto first = pools.add_slot("main");
    auto second = pools.add_slot("main");

    auto transaction = pools.acquire_transaction("main");
    assert(transaction);
    assert(transaction.transaction != 0);
    assert(transaction.slot->transaction_state() == TransactionState::Active);

    auto pinned = pools.route_transaction(transaction.transaction);
    assert(pinned);
    assert(pinned.slot == transaction.slot);

    auto ordinary = pools.route_idle("main");
    assert(ordinary);
    assert(ordinary.slot != transaction.slot);
    assert(ordinary.slot == first || ordinary.slot == second);

    auto closing = pools.route_transaction(transaction.transaction, true);
    assert(closing);
    assert(closing.slot->transaction_state() == TransactionState::Closing);

    auto duplicate_close = pools.route_transaction(transaction.transaction, true);
    assert(!duplicate_close);
    assert(duplicate_close.error.find("closing") != std::string::npos);

    assert(pools.release_transaction(transaction.transaction));
    auto stale = pools.route_transaction(transaction.transaction);
    assert(!stale);
    assert(stale.error.find("inactive") != std::string::npos);

    auto next = pools.acquire_transaction("main");
    assert(next);
    assert(next.transaction != transaction.transaction);
    assert(!pools.route_transaction(transaction.transaction));
    assert(pools.route_transaction(next.transaction));
    assert(pools.release_transaction(next.transaction));

    // 即使 BEGIN 回执尚未返回，关联键也能定位并取消同一笔事务。
    auto cancellable = pools.acquire_transaction("main", "begin-request-1");
    assert(cancellable);
    assert(!pools.acquire_transaction("main", "begin-request-1"));
    auto cancel_route = pools.route_transaction(std::string{"begin-request-1"},
                                               true);
    assert(cancel_route);
    assert(cancel_route.transaction == cancellable.transaction);
    assert(cancel_route.slot == cancellable.slot);
    assert(pools.release_transaction(cancellable.transaction));
    assert(!pools.route_transaction(std::string{"begin-request-1"}));

    auto missing = pools.route_idle("missing");
    assert(!missing);
    assert(missing.error.find("unknown") != std::string::npos);

    ConnectionPool worker_pool{"worker-test"};
    auto worker_slot = worker_pool.add_slot("main");
    worker_slot->start_worker([worker_slot] {
        while (auto job = worker_slot->next_job()) {
            caf_plugin_system::db::db_result result;
            result.ok = true;
            job->done(result);
        }
    });
    auto completion = std::make_shared<std::promise<bool>>();
    auto completed = completion->get_future();
    auto job = std::make_shared<caf_plugin_system::sql_backend::Job>();
    job->done = [completion](caf_plugin_system::db::db_result& result) {
        completion->set_value(result.ok);
    };
    auto worker_route = worker_pool.route_idle("main");
    assert(worker_route);
    worker_route.enqueue(std::move(job));
    assert(completed.wait_for(std::chrono::seconds{2})
           == std::future_status::ready);
    assert(completed.get());
    assert(worker_pool.stop_and_join() == 1);
    assert(worker_pool.stop_and_join() == 0);

    // 连接失败后队列已停止：后续请求必须立刻失败，不能无限堆积。
    auto stopped = std::make_shared<caf_plugin_system::sql_backend::Job>();
    bool stopped_failed = false;
    stopped->done = [&stopped_failed](
                        caf_plugin_system::db::db_result& result) {
        stopped_failed = !result.ok
                         && result.error.find("stopped") != std::string::npos;
    };
    assert(!worker_slot->enqueue(std::move(stopped)));
    assert(stopped_failed);

    caf::settings uri_settings{
        {"main", caf::config_value{std::string{
                     "mysql://app:secret@db.internal:3307/orders"}}},
    };
    const ConnectionUriParser mysql_parser{{"mysql://"}, "root", 3306};
    auto mysql = mysql_parser.parse(uri_settings);
    assert(mysql.size() == 1);
    assert(mysql[0].name == "main");
    assert(mysql[0].user == "app");
    assert(mysql[0].pass == "secret");
    assert(mysql[0].host == "db.internal");
    assert(mysql[0].port == 3307);
    assert(mysql[0].dbname == "orders");

    const ConnectionUriParser postgres_parser{
        {"postgres://", "postgresql://"}, "postgres", 5432};
    auto postgres = postgres_parser.parse(caf::settings{});
    assert(postgres.size() == 1);
    assert(postgres[0].name == "default");
    assert(postgres[0].user == "postgres");
    assert(postgres[0].port == 5432);
}
