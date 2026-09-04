#include "common/message_meta.hpp"
#include "templates/sql_entity_store_actor.hpp"

#include <caf/init_global_meta_objects.hpp>

#include <algorithm>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>

namespace db = caf_plugin_system::db;
namespace entity = caf_plugin_system::entity_store;
namespace sql = caf_plugin_system::entity_store::sql;
using namespace std::chrono_literals;

namespace {

void require(bool condition, const std::string& message) {
    if (!condition)
        throw std::runtime_error(message);
}

db::db_result good(int64_t affected = 0) {
    db::db_result result;
    result.ok = true;
    result.affected = affected;
    return result;
}

db::db_result bad(db::error_code code) {
    db::db_result result;
    result.code = code;
    result.error = "injected " + db::to_string(code);
    return result;
}

enum class Scenario {
    commit_ack_lost, write_always_lost, syntax_error,
    ddl_once_lost, begin_once_lost, load_once_lost,
};

// A transactional fake, not a canned success responder: records become
// visible only at commit, and rollback discards all pending business changes.
class MockBackend final : public caf::event_based_actor {
public:
    MockBackend(caf::actor_config& config, Scenario scenario)
        : caf::event_based_actor(config), scenario_(scenario) {}

    caf::behavior make_behavior() override {
        return {
            [this](sql_exec_atom, const std::string&, const std::string&,
                   const std::vector<std::string>&) {
                ++ddl_;
                if (scenario_ == Scenario::ddl_once_lost && ddl_ == 1)
                    return bad(db::error_code::connection_lost);
                return good();
            },
            [this](tx_begin_atom, const std::string&, const std::string& key) {
                ++begins_;
                const auto transaction = ++next_transaction_;
                pending_[transaction].key = key;
                events_.push_back("begin:" + key);
                if (scenario_ == Scenario::begin_once_lost && begins_ == 1)
                    return bad(db::error_code::connection_lost);
                auto result = good();
                result.insert_id = std::to_string(transaction);
                return result;
            },
            [this](sql_exec_atom, uint64_t transaction, const std::string& text,
                   const std::vector<std::string>& params) {
                auto found = pending_.find(transaction);
                if (found == pending_.end())
                    return bad(db::error_code::transaction_lost);
                auto& pending = found->second;
                if (text.rfind("INSERT", 0) == 0) {
                    ++reserves_;
                    pending.request_id = params.at(0);
                    pending.signature = params.at(1);
                    events_.push_back("reserve:" + pending.request_id);
                    return good(records_.contains(pending.request_id) ? 0 : 1);
                }
                if (text.rfind("UPDATE \"orders\"", 0) == 0) {
                    ++updates_;
                    events_.push_back("update:" + pending.request_id);
                    if (scenario_ == Scenario::write_always_lost)
                        return bad(db::error_code::connection_lost);
                    if (scenario_ == Scenario::syntax_error)
                        return bad(db::error_code::sql_error);
                    pending.version = committed_version_ + 1;
                    return good(1);
                }
                if (text.rfind("UPDATE \"__entity_store_requests\"", 0) == 0) {
                    pending.result = params.at(0);
                    return good(1);
                }
                return bad(db::error_code::sql_error);
            },
            [this](sql_query_atom, uint64_t transaction, const std::string& text,
                   const std::vector<std::string>& params) {
                auto found = pending_.find(transaction);
                if (found == pending_.end())
                    return bad(db::error_code::transaction_lost);
                auto result = good();
                if (text.find("request_signature") != std::string::npos) {
                    const auto record = records_.find(params.at(0));
                    if (record != records_.end())
                        result.rows = {{record->second.signature, record->second.result}};
                    events_.push_back("replay:" + params.at(0));
                } else {
                    result.rows = {{std::to_string(found->second.version)}};
                }
                return result;
            },
            [this](sql_query_atom, const std::string&, const std::string& text,
                   const std::vector<std::string>&) {
                if (text == "__stats__")
                    return stats();
                ++loads_;
                if (scenario_ == Scenario::load_once_lost && loads_ == 1)
                    return bad(db::error_code::connection_lost);
                auto result = good();
                result.rows = {{"paid", "7"}};
                return result;
            },
            [this](tx_commit_atom, uint64_t transaction) {
                auto found = pending_.find(transaction);
                if (found == pending_.end())
                    return bad(db::error_code::transaction_lost);
                ++commits_;
                const auto pending = found->second;
                records_[pending.request_id] = {pending.signature, pending.result};
                committed_version_ = pending.version;
                pending_.erase(found);
                events_.push_back("commit:" + pending.request_id);
                if (scenario_ == Scenario::commit_ack_lost && commits_ == 1)
                    return bad(db::error_code::outcome_unknown);
                return good();
            },
            [this](tx_rollback_atom, const std::string& key) {
                ++rollbacks_;
                events_.push_back("rollback:" + key);
                for (auto it = pending_.begin(); it != pending_.end(); ++it) {
                    if (it->second.key == key) {
                        pending_.erase(it);
                        return good();
                    }
                }
                return bad(db::error_code::transaction_lost);
            },
        };
    }

private:
    struct Pending {
        std::string key;
        std::string request_id;
        std::string signature;
        std::string result;
        uint64_t version = 0;
    };
    struct Record { std::string signature; std::string result; };

    db::db_result stats() const {
        auto result = good();
        result.columns = {"ddl", "begin", "reserve", "update", "commit", "rollback", "load", "pending"};
        result.rows = {{std::to_string(ddl_), std::to_string(begins_),
                        std::to_string(reserves_), std::to_string(updates_),
                        std::to_string(commits_), std::to_string(rollbacks_),
                        std::to_string(loads_), std::to_string(pending_.size())}};
        for (const auto& event : events_)
            result.rows.push_back({event});
        return result;
    }

    Scenario scenario_;
    uint64_t next_transaction_ = 0;
    uint64_t committed_version_ = 0;
    size_t ddl_ = 0, begins_ = 0, reserves_ = 0, updates_ = 0;
    size_t commits_ = 0, rollbacks_ = 0, loads_ = 0;
    std::map<uint64_t, Pending> pending_;
    std::map<std::string, Record> records_;
    std::vector<std::string> events_;
};

entity::entity_ref target() {
    return {.store = "commerce", .partition = "one-order", .entity = "order",
            .key = {{"order_id", entity::value::text("one-order")}}};
}

entity::save_request save_request(std::string id) {
    return {.request_id = std::move(id),
            .changes = {{.target = target(),
                         .fields = {{entity::patch_op::set, "status", entity::value::text("paid")}}}}};
}

class Harness {
public:
    Harness(caf::actor_system& system, Scenario scenario, unsigned index)
        : system_(system), name_("mock_sql_" + std::to_string(index)) {
        backend = system_.spawn<MockBackend>(scenario);
        system_.registry().put(name_, backend);
        sql::entity_schema order{
            .name = "order", .table = "orders", .version_column = "version",
            .keys = {{"order_id", "order_id", entity::value_kind::text, false, false}},
            .fields = {{"status", "status", entity::value_kind::text, true, false}},
        };
        sql::store_schema commerce{
            .name = "commerce", .read_service = name_, .write_service = name_,
            .entities = {{"order", std::move(order)}},
        };
        sql::service_config settings;
        settings.request_timeout = 2s;
        settings.save_retry_attempts = 3;
        settings.load_retry_attempts = 3;
        settings.retry_backoff = 10ms;
        std::string error;
        require(settings.catalog.add_store(std::move(commerce), error), error);
        store = system_.spawn<sql::entity_store_actor>(std::move(settings));
    }

    ~Harness() {
        system_.registry().erase(name_);
        caf::anon_send_exit(store, caf::exit_reason::user_shutdown);
        caf::anon_send_exit(backend, caf::exit_reason::user_shutdown);
    }

    entity::save_result save(const std::string& id) {
        caf::scoped_actor self{system_};
        entity::save_result result;
        self->request(store, 3s, entity_save_atom_v, save_request(id)).receive(
            [&](entity::save_result value) { result = std::move(value); },
            [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
        return result;
    }

    db::db_result snapshot() {
        caf::scoped_actor self{system_};
        db::db_result result;
        self->request(backend, 1s, sql_query_atom_v, std::string{"default"},
                       std::string{"__stats__"}, std::vector<std::string>{}).receive(
            [&](db::db_result value) { result = std::move(value); },
            [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
        return result;
    }

    caf::actor backend;
    caf::actor store;

private:
    caf::actor_system& system_;
    std::string name_;
};

uint64_t count(const db::db_result& stats, const std::string& name) {
    const auto found = std::find(stats.columns.begin(), stats.columns.end(), name);
    require(found != stats.columns.end(), "unknown counter " + name);
    return std::stoull(stats.rows.front().at(static_cast<size_t>(found - stats.columns.begin())));
}

std::vector<std::string> events(const db::db_result& stats) {
    std::vector<std::string> result;
    for (size_t i = 1; i < stats.rows.size(); ++i)
        result.push_back(stats.rows[i].front());
    return result;
}

} // namespace

int main() {
    caf::core::init_global_meta_objects();
    app_meta::init();
    caf::actor_system_config config;
    caf::actor_system system{config};
    try {
        {
            Harness test{system, Scenario::commit_ack_lost, 1};
            caf::scoped_actor self{system};
            auto first = self->request(test.store, 3s, entity_save_atom_v, save_request("first"));
            auto second = self->request(test.store, 3s, entity_save_atom_v, save_request("second"));
            entity::save_result a, b;
            first.receive([&](entity::save_result result) { a = std::move(result); },
                          [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
            second.receive([&](entity::save_result result) { b = std::move(result); },
                           [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
            require(a.committed && b.committed && a.entities.at(0).version == 1
                        && b.entities.at(0).version == 2,
                    "commit acknowledgement loss did not recover by replaying its record");
            const auto stats = test.snapshot();
            require(count(stats, "update") == 2 && count(stats, "reserve") == 3,
                    "commit recovery repeated a business update");
            const auto order = events(stats);
            require(std::count(order.begin(), order.end(), "update:first") == 1,
                    "first request's business SQL executed twice");
            const auto replay = std::find(order.begin(), order.end(), "replay:first");
            const auto following = std::find(order.begin(), order.end(), "reserve:second");
            require(replay != order.end() && following != order.end() && replay < following,
                    "same-partition request overtook a retrying save");
        }
        {
            Harness test{system, Scenario::write_always_lost, 2};
            const auto result = test.save("bounded");
            require(!result.committed, "permanent outage must not claim success");
            const auto stats = test.snapshot();
            require(count(stats, "begin") == 3 && count(stats, "update") == 3
                        && count(stats, "rollback") == 3 && count(stats, "pending") == 0,
                    "save retries or transaction cleanup are not bounded");
        }
        {
            Harness test{system, Scenario::syntax_error, 3};
            require(!test.save("syntax").committed, "SQL error unexpectedly succeeded");
            const auto stats = test.snapshot();
            require(count(stats, "begin") == 1 && count(stats, "update") == 1
                        && count(stats, "rollback") == 1,
                    "ordinary SQL error must not be retried");
        }
        {
            Harness test{system, Scenario::ddl_once_lost, 4};
            require(test.save("ddl").committed, "DDL disconnect did not recover");
            const auto stats = test.snapshot();
            require(count(stats, "ddl") == 2 && count(stats, "begin") == 1,
                    "DDL recovery should retry before acquiring a transaction");
        }
        {
            Harness test{system, Scenario::begin_once_lost, 5};
            require(test.save("begin").committed, "lost BEGIN response did not recover");
            const auto stats = test.snapshot();
            require(count(stats, "begin") == 2 && count(stats, "rollback") == 1
                        && count(stats, "pending") == 0,
                    "lost BEGIN was not cancelled by request key");
            std::set<std::string> keys;
            for (const auto& event : events(stats))
                if (event.rfind("begin:", 0) == 0)
                    keys.insert(event);
            require(keys.size() == 2, "retries reused the previous BEGIN key");
        }
        {
            Harness test{system, Scenario::load_once_lost, 6};
            caf::scoped_actor self{system};
            entity::load_result result;
            self->request(test.store, 3s, entity_load_atom_v,
                           entity::load_request{.target = target()}).receive(
                [&](entity::load_result value) { result = std::move(value); },
                [](const caf::error& error) { throw std::runtime_error(caf::to_string(error)); });
            require(result.code == entity::result_code::ok && result.version == 7,
                    "pure SELECT did not retry after connection loss");
            require(count(test.snapshot(), "load") == 2, "load attempt count is wrong");
        }
        system.await_all_actors_done();
        std::cout << "entity store recovery tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
