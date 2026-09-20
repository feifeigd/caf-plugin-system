#include "app_tests.hpp"
#include "../../../tests/entity_crud_scenarios.hpp"

#include "common/db_contract.hpp"
#include "common/entity_store_contract.hpp"
#include "common/message_tags.hpp"

#include <caf/all.hpp>

#include <chrono>
#include <charconv>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace caf_plugin_system {

namespace {

namespace entity = entity_store;

class EntityStoreIntegrationTest {
public:
    EntityStoreIntegrationTest(caf::actor_system& system,
                               const BootstrapResult& framework)
        : framework_(framework), self_(system),
          dialect_(caf::get_or<std::string>(
              system.config().content,
              "caf-plugin-system.entity_store.dialect", "sqlite")),
          schema_source_(caf::get_or<std::string>(
              system.config().content,
              "caf-plugin-system.entity_store.schema_source", "manual")) {
    }

    bool run() {
        const auto plugin = database_plugin();
        if (!require(!plugin.empty(), "unsupported SQL dialect: " + dialect_))
            return finish();
        database_ = resolve_plugin(plugin);
        store_ = resolve_plugin("EntityStorePlugin");
        if (!require(static_cast<bool>(database_), plugin + " is unavailable")
            || !require(static_cast<bool>(store_),
                        "EntityStorePlugin is unavailable"))
            return finish();
        if (!verify_server_version() || !prepare_tables()
            || !verify_cancel_before_begin_reply())
            return finish();
        if (dialect_ == "postgres"
            && (!verify_failed_postgres_commit()
                || !verify_postgres_nul_queries()))
            return finish();
        if (dialect_ == "mysql" && !verify_mysql_parameter_count())
            return finish();
        if (dialect_ != "sqlite" && !verify_disconnect_recovery())
            return finish();
        if (dialect_ == "postgres" && !verify_postgres_copy_recovery())
            return finish();

        if (!entity::test::strict_crud(create_order_request(),
                [this](const auto& input) {
                    auto result = save(input, "strict CRUD");
                    return result.value_or(entity::save_result{});
                },
                [this](const auto& key) {
                    entity::load_result result;
                    self_->request(store_, std::chrono::seconds(10), entity_load_atom_v,
                                   entity::load_request{.target = key})
                        .receive([&](entity::load_result value) { result = std::move(value); },
                                 [&](const caf::error&) {});
                    return result;
                },
                [this](bool ok, const char* message) { return require(ok, message); }))
            return finish();
        auto create = create_order_request();
        auto created = save(create, "create order");
        if (!require_ok(created, 1, "create order"))
            return finish();
        if (dialect_ == "postgres" && !verify_postgres_nul_key())
            return finish();

        // 同 request_id + 同内容必须返回首次提交结果，不能再执行 patch。
        auto replayed = save(create, "replay create order");
        if (!require_ok(replayed, 1, "replay create order"))
            return finish();

        auto reused_id = create;
        reused_id.changes.front().fields.front().data =
            entity::value::text("different-content");
        auto rejected_reuse = save(std::move(reused_id), "reused request id");
        if (!require(rejected_reuse
                         && rejected_reuse->code == entity::result_code::conflict
                         && !rejected_reuse->committed,
                     "same request id with different content was accepted"))
            return finish();

        auto patched = save(
            single_save(
                "request-partial", order_ref(), 1,
                {{entity::patch_op::set, "status",
                  entity::value::text("paid")},
                 {entity::patch_op::increment, "paid_cents",
                  entity::value::signed_integer(9950)}}),
            "partial update");
        if (!require_ok(patched, 2, "partial update")
            || !verify_order("paid", 9950, entity::value::text("keep"), 2,
                             "partial update load"))
            return finish();

        // 两个请求在等待前连续投递。同 sender FIFO + EntityStore 的
        // store/partition 队列应使 expected_version=2、3 依次成功，
        // 即使底层配置了多连接池也不能乱序。
        auto first_request = single_save(
            "request-sequence-1", order_ref(), 2,
            {{entity::patch_op::set, "status",
              entity::value::text("packed")}});
        auto second_request = single_save(
            "request-sequence-2", order_ref(), 3,
            {{entity::patch_op::set, "status",
              entity::value::text("shipped")}});
        auto first_pending = self_->request(
            store_, timeout_, entity_save_atom_v, first_request);
        auto second_pending = self_->request(
            store_, timeout_, entity_save_atom_v, second_request);
        auto first = await<entity::save_result>(std::move(first_pending),
                                                "sequence request 1");
        auto second = await<entity::save_result>(std::move(second_pending),
                                                 "sequence request 2");
        if (!require_ok(first, 3, "sequence request 1")
            || !require_ok(second, 4, "sequence request 2")
            || !verify_order("shipped", 9950, entity::value::text("keep"), 4,
                             "ordered load"))
            return finish();

        auto stale = save(
            single_save(
                "request-stale", order_ref(), 3,
                {{entity::patch_op::set, "status",
                  entity::value::text("cancelled")}}),
            "stale version");
        if (!require(stale && stale->code == entity::result_code::conflict
                         && !stale->committed,
                     "stale version was not rejected"))
            return finish();

        // 第一条 order 更新成功后，第二条 payment 因目标不存在而失败；
        // save_request 的整个数据库事务必须回滚，order 保持 version=4。
        entity::save_request atomic;
        atomic.request_id = "request-rollback";
        atomic.changes.push_back(make_patch(
            order_ref(), 4,
            {{entity::patch_op::set, "status",
              entity::value::text("refund_pending")}}));
        atomic.changes.push_back(make_patch(
            payment_ref("payment-missing"), std::nullopt,
            {{entity::patch_op::set, "state",
              entity::value::text("captured")}}));
        auto rolled_back = save(std::move(atomic), "atomic rollback");
        if (!require(rolled_back
                         && rolled_back->code == entity::result_code::not_found
                         && !rolled_back->committed,
                     "multi-entity save did not report rollback")
            || !verify_order("shipped", 9950, entity::value::text("keep"), 4,
                             "rollback verification"))
            return finish();

        auto erased = save(
            single_save(
                "request-null", order_ref(), 4,
                {{entity::patch_op::erase, "memo", entity::value::null()}}),
            "nullable erase");
        if (!require_ok(erased, 5, "nullable erase")
            || !verify_order("shipped", 9950, entity::value::null(), 5,
                             "NULL bitmap load"))
            return finish();

        if (!verify_bound_text_values())
            return finish();

        return finish();
    }

private:
    using FieldList = std::vector<entity::field_patch>;

    std::string database_plugin() const {
        if (dialect_ == "sqlite")
            return "SqlitePlugin";
        if (dialect_ == "mysql")
            return "MySqlPlugin";
        if (dialect_ == "postgres")
            return "PostgresPlugin";
        return {};
    }

    caf::actor resolve_plugin(const std::string& name) {
        caf::actor result;
        self_->request(framework_.plugin_mgr, timeout_, resolve_plugin_atom_v,
                       name)
            .receive(
                [&](const caf::actor& actor) { result = actor; },
                [&](caf::error& error) {
                    fail("resolve " + name + ": " + caf::to_string(error));
                });
        return result;
    }

    bool verify_server_version() {
        auto pending = self_->request(
            database_, timeout_, sql_query_atom_v, std::string{"writer"},
            dialect_ == "sqlite" ? std::string{"SELECT sqlite_version()"}
                                  : std::string{"SELECT version()"},
            std::vector<std::string>{});
        auto result = await<db::db_result>(std::move(pending), "server version");
        if (!require(result && result->ok && result->rows.size() == 1
                         && result->rows.front().size() == 1
                         && !result->rows.front().front().empty(),
                     "cannot query server version" + db_error(result)))
            return false;
        std::cout << "[EntityStoreTest] backend=" << dialect_
                  << " schema=" << schema_source_
                  << " server=" << result->rows.front().front() << std::endl;
        return true;
    }

    bool prepare_tables() {
        // MySQL does not allow an unbounded TEXT primary key. BIGINT keeps
        // monetary minor units and versions exact on every SQL backend.
        const std::string key_type =
            dialect_ == "mysql" ? "VARCHAR(255)" : "TEXT";
        const std::string table_options = dialect_ == "mysql"
            ? " ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin"
            : "";
        // The Docker runner keeps the same database for manual -> database
        // schema tests. Discovery happens during actor startup, so the second
        // pass must retain the first pass's tables and clear only test rows.
        const std::vector<std::string> statements = schema_source_ == "database"
            ? std::vector<std::string>{"DELETE FROM __entity_store_requests",
                                       "DELETE FROM payments", "DELETE FROM orders"}
            : std::vector<std::string>{
            "CREATE TABLE orders (order_id " + key_type
                + " PRIMARY KEY, status TEXT NOT NULL, paid_cents BIGINT NOT "
                  "NULL, memo TEXT NULL, version BIGINT NOT NULL)"
                + table_options,
            "CREATE TABLE payments (payment_id " + key_type
                + " PRIMARY KEY, state TEXT NOT NULL, version BIGINT NOT NULL)"
                + table_options,
        };
        for (const auto& statement : statements) {
            auto pending = self_->request(database_, timeout_, sql_exec_atom_v,
                                          std::string{"writer"}, statement,
                                          std::vector<std::string>{});
            auto result = await<db::db_result>(std::move(pending),
                                               "prepare database");
            if (!require(result && result->ok,
                         "cannot prepare " + dialect_ + " tables"
                             + db_error(result)))
                return false;
        }
        return true;
    }

    bool verify_cancel_before_begin_reply() {
        const std::string key{"cancel-before-begin-reply"};
        auto beginning = self_->request(database_, timeout_, tx_begin_atom_v,
                                         std::string{"writer"}, key);
        // 不等待 BEGIN 的 handle，直接按关联键取消，验证超时清理路径。
        auto cancelling = self_->request(database_, timeout_, tx_rollback_atom_v,
                                          key);
        auto begun = await<db::db_result>(std::move(beginning), "keyed BEGIN");
        auto cancelled = await<db::db_result>(std::move(cancelling),
                                               "keyed ROLLBACK");
        return require(begun && begun->ok && cancelled && cancelled->ok,
                       "cannot cancel transaction before BEGIN reply");
    }

    bool verify_bound_text_values() {
        const std::string special{"quoted 'value' \"double\" \\path\\ 中文订单"};
        auto bound = save(
            single_save(
                "request-bound-text", order_ref(), 5,
                {{entity::patch_op::set, "status", entity::value::text(special)},
                 {entity::patch_op::set, "memo", entity::value::text(special)}}),
            "quoted UTF-8 text");
        if (!require_ok(bound, 6, "quoted UTF-8 text")
            || !verify_order(special, 9950, entity::value::text(special), 6,
                             "quoted UTF-8 text load"))
            return false;

        // Exceed the MySQL driver's original 4096-byte result buffer. A suffix
        // containing UTF-8 and escaping-sensitive bytes detects both a dropped
        // MYSQL_DATA_TRUNCATED row and silent truncation of an otherwise valid row.
        const auto large = std::string(8192, 'x') + special;
        auto request = single_save(
            "request-large-text", order_ref(), 6,
            {{entity::patch_op::set, "memo", entity::value::text(large)}});
        auto saved = save(request, "large text");
        if (!require_ok(saved, 7, "large text")
            || !verify_order(special, 9950, entity::value::text(large), 7,
                             "large text load"))
            return false;
        // The durable idempotency payload also contains the complete long value.
        auto replayed = save(std::move(request), "large text replay");
        return require_ok(replayed, 7, "large text replay")
               && verify_order(special, 9950, entity::value::text(large), 7,
                               "large text replay load");
    }

    std::optional<uint64_t> begin_transaction(
        const std::string& step, const std::string& connection = "writer") {
        auto pending = self_->request(database_, timeout_, tx_begin_atom_v,
                                      connection);
        auto result = await<db::db_result>(std::move(pending), step);
        if (!require(result && result->ok, step + db_error(result)))
            return std::nullopt;
        uint64_t transaction = 0;
        const auto& handle = result->insert_id;
        const auto parsed = std::from_chars(
            handle.data(), handle.data() + handle.size(), transaction);
        if (!require(parsed.ec == std::errc{}
                         && parsed.ptr == handle.data() + handle.size()
                         && transaction != 0,
                     step + " returned an invalid transaction handle"))
            return std::nullopt;
        return transaction;
    }

    std::optional<db::db_result> query_connection(
        const std::string& connection, const std::string& text,
        const std::string& step, std::vector<std::string> parameters = {}) {
        auto pending = self_->request(database_, timeout_, sql_query_atom_v,
                                      connection, text, std::move(parameters));
        return await<db::db_result>(std::move(pending), step);
    }

    std::optional<uint64_t> recovery_session_id(const std::string& step) {
        auto result = query_connection("recovery",
            dialect_ == "mysql" ? "SELECT CONNECTION_ID()" : "SELECT pg_backend_pid()",
            step);
        if (!require(result && result->ok && result->rows.size() == 1
                         && result->rows.front().size() == 1,
                     step + db_error(result)))
            return std::nullopt;
        const auto& text = result->rows.front().front();
        uint64_t id = 0;
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        if (!require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size()
                         && id != 0, step + " returned an invalid native session id"))
            return std::nullopt;
        return id;
    }

    bool terminate_recovery_session(uint64_t session) {
        std::optional<db::db_result> result;
        if (dialect_ == "mysql") {
            // The only interpolated token is a validated numeric session ID
            // read from this test's dedicated single-slot connection.
            auto pending = self_->request(database_, timeout_, sql_exec_atom_v,
                std::string{"recovery_admin"},
                "KILL CONNECTION " + std::to_string(session), std::vector<std::string>{});
            result = await<db::db_result>(std::move(pending), "kill recovery session");
        } else {
            result = query_connection("recovery_admin",
                "SELECT pg_terminate_backend($1::integer, 5000)",
                "terminate recovery session", {std::to_string(session)});
            if (!require(result && result->ok && result->rows.size() == 1
                             && result->rows.front().size() == 1
                             && result->rows.front().front() == "t",
                         "PostgreSQL did not terminate the recovery session"
                             + db_error(result)))
                return false;
        }
        return require(result && result->ok,
                       "cannot terminate dedicated recovery session" + db_error(result));
    }

    std::optional<db::db_result> insert_recovery_payment(
        uint64_t transaction, const std::string& id, const std::string& step) {
        const std::string text = dialect_ == "mysql"
            ? "INSERT INTO payments (payment_id, state, version) VALUES (?, ?, 1)"
            : "INSERT INTO payments (payment_id, state, version) VALUES ($1, $2, 1)";
        auto pending = self_->request(database_, timeout_, sql_exec_atom_v,
            transaction, text, std::vector<std::string>{id, "recovery"});
        return await<db::db_result>(std::move(pending), step);
    }

    bool verify_recovery_payment_count(const std::string& id, unsigned expected) {
        const std::string text = dialect_ == "mysql"
            ? "SELECT COUNT(*) FROM payments WHERE payment_id = ?"
            : "SELECT COUNT(*) FROM payments WHERE payment_id = $1";
        auto result = query_connection("writer", text, "recovery persistence", {id});
        return require(result && result->ok && result->rows.size() == 1
                           && result->rows.front().size() == 1
                           && result->rows.front().front() == std::to_string(expected),
                       "unexpected persistence after session loss: " + id + db_error(result));
    }

    bool verify_disconnect_recovery() {
        auto original = recovery_session_id("initial recovery session");
        if (!original || !terminate_recovery_session(*original))
            return false;
        auto disconnected = query_connection("recovery", "SELECT 1", "idle disconnect");
        if (!require(disconnected && !disconnected->ok
                         && db::is_connection_error(disconnected->code),
                     "idle disconnect was not classified" + db_error(disconnected)))
            return false;
        auto replacement = recovery_session_id("reconnect after idle disconnect");
        if (!require(replacement && *replacement != *original,
                     "fresh request did not obtain a new physical session"))
            return false;

        auto old_transaction = begin_transaction("recovery transaction BEGIN", "recovery");
        if (!old_transaction)
            return false;
        auto inserted = insert_recovery_payment(*old_transaction, "recovery-rolled-back",
                                                "write before disconnect");
        if (!require(inserted && inserted->ok && inserted->affected == 1,
                     "cannot write before disconnect" + db_error(inserted))
            || !terminate_recovery_session(*replacement))
            return false;

        auto lost = insert_recovery_payment(*old_transaction, "recovery-never-written",
                                            "write detects transaction disconnect");
        if (!require(lost && !lost->ok && db::is_connection_error(lost->code),
                     "transaction disconnect was not classified" + db_error(lost)))
            return false;
        auto stale = insert_recovery_payment(*old_transaction, "recovery-never-written",
                                             "stale transaction write");
        auto committing = self_->request(database_, timeout_, tx_commit_atom_v, *old_transaction);
        auto committed = await<db::db_result>(std::move(committing), "stale transaction COMMIT");
        if (!require(stale && !stale->ok && stale->code == db::error_code::transaction_lost
                         && committed && !committed->ok
                         && committed->code == db::error_code::transaction_lost,
                     "old transaction was not rejected after session loss")
            || !verify_recovery_payment_count("recovery-rolled-back", 0)
            || !verify_recovery_payment_count("recovery-never-written", 0))
            return false;

        auto next = begin_transaction("new recovery transaction BEGIN", "recovery");
        if (!next)
            return false;
        auto cancelling = self_->request(database_, timeout_, tx_rollback_atom_v, *old_transaction);
        auto cancelled = await<db::db_result>(std::move(cancelling), "late old transaction ROLLBACK");
        if (!require(cancelled && !cancelled->ok
                         && cancelled->code == db::error_code::transaction_lost,
                     "old cleanup was not rejected while a new transaction was active"))
            return false;
        auto fresh = insert_recovery_payment(*next, "recovery-committed", "new transaction write");
        if (!require(fresh && fresh->ok && fresh->affected == 1,
                     "new transaction cannot write" + db_error(fresh)))
            return false;
        auto finishing = self_->request(database_, timeout_, tx_commit_atom_v, *next);
        auto finished = await<db::db_result>(std::move(finishing), "new recovery transaction COMMIT");
        if (!require(finished && finished->ok,
                     "new transaction cannot commit" + db_error(finished))
            || !verify_recovery_payment_count("recovery-committed", 1))
            return false;
        std::cout << "[EntityStoreTest] PASS: physical disconnect detection, reconnect, "
                     "transaction loss and stale cleanup isolation" << std::endl;
        return true;
    }

    bool verify_postgres_copy_recovery() {
        auto copied = query_connection("recovery", "COPY (SELECT 1) TO STDOUT",
                                       "unsupported PostgreSQL COPY");
        if (!require(copied && !copied->ok && db::is_connection_error(copied->code)
                         && copied->error.find("COPY") != std::string::npos,
                     "COPY did not fail promptly with an explicit unsupported error"
                         + db_error(copied)))
            return false;
        auto recovered = query_connection("recovery", "SELECT 1", "query after COPY rejection");
        return require(recovered && recovered->ok && recovered->rows.size() == 1
                           && recovered->rows.front().size() == 1
                           && recovered->rows.front().front() == "1",
                       "PostgreSQL did not recover after COPY rejection" + db_error(recovered));
    }

    bool verify_mysql_parameter_count() {
        const std::vector<std::vector<std::string>> invalid_parameters = {
            {}, {"first", "unexpected-second"}};
        for (const auto& parameters : invalid_parameters) {
            auto pending = self_->request(
                database_, timeout_, sql_query_atom_v, std::string{"writer"},
                std::string{"SELECT ?"}, parameters);
            auto result = await<db::db_result>(std::move(pending),
                                                "MySQL parameter count mismatch");
            if (!require(result && !result->ok && !result->error.empty(),
                         "MySQL accepted " + std::to_string(parameters.size())
                             + " parameters for one placeholder"))
                return false;
        }
        auto pending = self_->request(
            database_, timeout_, sql_query_atom_v, std::string{"writer"},
            std::string{"SELECT ?"}, std::vector<std::string>{"healthy"});
        auto result = await<db::db_result>(std::move(pending),
                                            "MySQL query after invalid parameters");
        return require(result && result->ok && result->rows.size() == 1
                           && result->rows.front().size() == 1
                           && result->rows.front().front() == "healthy",
                       "MySQL connection is unusable after invalid parameters"
                           + db_error(result));
    }

    bool verify_failed_postgres_commit() {
        auto transaction = begin_transaction("PostgreSQL failed-tx BEGIN");
        if (!transaction)
            return false;
        auto inserting = self_->request(
            database_, timeout_, sql_exec_atom_v, *transaction,
            std::string{"INSERT INTO payments (payment_id, state, version) "
                        "VALUES ($1, $2, 1)"},
            std::vector<std::string>{"failed-commit-payment", "pending"});
        auto inserted = await<db::db_result>(std::move(inserting),
                                             "PostgreSQL failed-tx INSERT");
        if (!require(inserted && inserted->ok && inserted->affected == 1,
                     "cannot insert failed-tx payment" + db_error(inserted)))
            return false;

        auto failing = self_->request(
            database_, timeout_, sql_query_atom_v, *transaction,
            std::string{"SELECT 1 / 0"}, std::vector<std::string>{});
        auto failed = await<db::db_result>(std::move(failing),
                                           "PostgreSQL deliberate tx error");
        if (!require(failed && !failed->ok && !failed->error.empty(),
                     "division by zero did not fail the transaction"))
            return false;
        auto committing = self_->request(database_, timeout_, tx_commit_atom_v,
                                          *transaction);
        auto committed = await<db::db_result>(std::move(committing),
                                              "PostgreSQL failed-tx COMMIT");
        if (!require(committed && !committed->ok && !committed->error.empty(),
                     "PostgreSQL ROLLBACK command tag was reported as commit"))
            return false;

        auto querying = self_->request(
            database_, timeout_, sql_query_atom_v, std::string{"writer"},
            std::string{"SELECT COUNT(*) FROM payments WHERE payment_id = $1"},
            std::vector<std::string>{"failed-commit-payment"});
        auto found = await<db::db_result>(std::move(querying),
                                          "PostgreSQL failed-tx persistence");
        if (!require(found && found->ok && found->rows.size() == 1
                         && found->rows.front().size() == 1
                         && found->rows.front().front() == "0",
                     "failed PostgreSQL transaction persisted data"
                         + db_error(found)))
            return false;

        auto next = begin_transaction("PostgreSQL connection reuse BEGIN");
        if (!next)
            return false;
        auto cancelling = self_->request(database_, timeout_, tx_rollback_atom_v,
                                          *next);
        auto cancelled = await<db::db_result>(std::move(cancelling),
                                               "PostgreSQL connection reuse ROLLBACK");
        return require(cancelled && cancelled->ok,
                       "PostgreSQL connection is unusable after failed COMMIT"
                           + db_error(cancelled));
    }

    bool verify_postgres_nul_queries() {
        auto bound = self_->request(
            database_, timeout_, sql_query_atom_v, std::string{"writer"},
            std::string{"SELECT $1::text"},
            std::vector<std::string>{std::string{"before\0after", 12}});
        auto bound_result = await<db::db_result>(std::move(bound),
                                                  "PostgreSQL NUL parameter");
        if (!require(bound_result && !bound_result->ok
                         && !bound_result->error.empty(),
                     "PostgreSQL silently truncated a NUL parameter"))
            return false;
        std::string sql{"SELECT 1"};
        sql.push_back('\0');
        sql += "; SELECT 2";
        auto text = self_->request(
            database_, timeout_, sql_query_atom_v, std::string{"writer"},
            std::move(sql), std::vector<std::string>{});
        auto text_result = await<db::db_result>(std::move(text),
                                                 "PostgreSQL NUL SQL");
        return require(text_result && !text_result->ok
                           && !text_result->error.empty(),
                       "PostgreSQL silently truncated SQL containing NUL");
    }

    bool verify_postgres_nul_key() {
        entity::load_request request;
        request.target = order_ref();
        std::string invalid_key{"order-42"};
        invalid_key.push_back('\0');
        invalid_key += "different-order";
        request.target.key.front().data = entity::value::text(invalid_key);
        auto pending = self_->request(store_, timeout_, entity_load_atom_v,
                                      std::move(request));
        auto result = await<entity::load_result>(std::move(pending),
                                                  "PostgreSQL NUL entity key");
        return require(result && result->code != entity::result_code::ok,
                       "NUL entity key matched the truncated order-42 key");
    }

    static entity::entity_ref order_ref() {
        return {
            .store = "commerce",
            .partition = "order-42",
            .entity = "order",
            .key = {{"order_id", entity::value::text("order-42")}},
        };
    }

    static entity::entity_ref payment_ref(std::string id) {
        return {
            .store = "commerce",
            .partition = "order-42",
            .entity = "payment",
            .key = {{"payment_id", entity::value::text(std::move(id))}},
        };
    }

    static entity::entity_patch make_patch(
        entity::entity_ref target, std::optional<uint64_t> expected_version,
        FieldList fields, bool create_if_missing = false) {
        entity::entity_patch result;
        result.target = std::move(target);
        result.fields = std::move(fields);
        result.create_if_missing = create_if_missing;
        result.check_version = expected_version.has_value();
        result.expected_version = expected_version.value_or(0);
        return result;
    }

    static entity::save_request single_save(
        std::string request_id, entity::entity_ref target,
        std::optional<uint64_t> version, FieldList fields) {
        entity::save_request result;
        result.request_id = std::move(request_id);
        result.changes.push_back(
            make_patch(std::move(target), version, std::move(fields)));
        return result;
    }

    static entity::save_request create_order_request() {
        auto result = single_save(
            "request-create", order_ref(), std::nullopt,
            {{entity::patch_op::set, "status", entity::value::text("pending")},
             {entity::patch_op::set, "paid_cents",
              entity::value::signed_integer(0)},
             {entity::patch_op::set, "memo", entity::value::text("keep")}});
        result.changes.front().create_if_missing = true;
        return result;
    }

    std::optional<entity::save_result> save(entity::save_request request,
                                            const std::string& step) {
        auto pending = self_->request(store_, timeout_, entity_save_atom_v,
                                      std::move(request));
        return await<entity::save_result>(std::move(pending), step);
    }

    std::optional<entity::load_result> load_order() {
        entity::load_request request;
        request.target = order_ref();
        request.fields.all_fields = false;
        request.fields.names = {"status", "paid_cents", "memo"};
        auto pending = self_->request(store_, timeout_, entity_load_atom_v,
                                      std::move(request));
        return await<entity::load_result>(std::move(pending), "load order");
    }

    bool verify_order(const std::string& status, int64_t paid_cents,
                      const entity::value& memo, uint64_t version,
                      const std::string& step) {
        auto loaded = load_order();
        if (!require(loaded && loaded->code == entity::result_code::ok,
                     step + " failed"))
            return false;
        const auto* actual_status = find_field(*loaded, "status");
        const auto* actual_paid = find_field(*loaded, "paid_cents");
        const auto* actual_memo = find_field(*loaded, "memo");
        return require(actual_status
                           && *actual_status == entity::value::text(status)
                           && actual_paid
                           && *actual_paid
                                  == entity::value::signed_integer(paid_cents)
                           && actual_memo && *actual_memo == memo
                           && loaded->version == version,
                       step + " returned unexpected entity state");
    }

    static const entity::value* find_field(const entity::load_result& result,
                                           const std::string& name) {
        for (const auto& field : result.fields)
            if (field.name == name)
                return &field.data;
        return nullptr;
    }

    bool require_ok(const std::optional<entity::save_result>& result,
                    uint64_t version, const std::string& step) {
        return require(result && result->code == entity::result_code::ok
                           && result->committed && result->entities.size() == 1
                           && result->entities.front().version == version,
                       step + " did not commit expected version "
                           + std::to_string(version)
                           + (result ? ": " + entity::to_string(result->code)
                                           + " " + result->error
                                     : " (no response)"));
    }

    template <class Result, class Handle>
    std::optional<Result> await(Handle handle, const std::string& step) {
        std::optional<Result> result;
        handle.receive(
            [&](Result& value) { result = std::move(value); },
            [&](caf::error& error) {
                fail(step + ": " + caf::to_string(error));
            });
        return result;
    }

    static std::string db_error(const std::optional<db::db_result>& result) {
        return result && !result->error.empty() ? ": " + result->error : "";
    }

    bool require(bool condition, std::string message) {
        if (!condition)
            fail(std::move(message));
        return condition;
    }

    void fail(std::string message) {
        ok_ = false;
        std::cout << "[EntityStoreTest] FAIL: " << message << std::endl;
    }

    bool finish() const {
        if (ok_)
            std::cout << "[EntityStoreTest] PASS: partial update, transaction, "
                         "idempotency, ordering, optimistic lock, rollback, "
                         "read/write routing, NULL, keyed cancellation, "
                         "quoted UTF-8 and large text verified"
                      << std::endl;
        return ok_;
    }

    const BootstrapResult& framework_;
    caf::scoped_actor self_;
    const std::string dialect_;
    const std::string schema_source_;
    caf::actor database_;
    caf::actor store_;
    bool ok_ = true;
    const std::chrono::seconds timeout_{10};
};

} // namespace

bool run_entity_store_test(caf::actor_system& sys,
                           const BootstrapResult& fw) {
    return EntityStoreIntegrationTest{sys, fw}.run();
}

} // namespace caf_plugin_system
