#pragma once

// 独立 EntityStore actor：把实体协议翻译成底层 SQL 服务协议。
// load 走读服务/读连接；save 走写服务/写连接并在单个数据库事务中完成。
// 同 store+partition 的 save 串行，不同 partition 可并行等待数据库响应。

#include "common/db_contract.hpp"
#include "common/message_tags.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "templates/sql_entity_store_statements.hpp"
#include "templates/sql_entity_store_schema_provider.hpp"

#include <caf/actor_registry.hpp>
#include <caf/all.hpp>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace caf_plugin_system::entity_store::sql {

struct service_config {
    schema_catalog catalog;
    sql_dialect dialect{dialect_kind::sqlite};
    std::chrono::milliseconds request_timeout{10000};
    size_t max_pending_requests = 1024;
    uint32_t save_retry_attempts = 3;
    uint32_t load_retry_attempts = 3;
    std::chrono::milliseconds retry_backoff{100};
    std::string config_error;
};

class entity_store_actor : public caf::event_based_actor {
public:
    entity_store_actor(caf::actor_config& config, service_config settings)
        : caf::event_based_actor(config), settings_(std::move(settings)),
          builder_(settings_.dialect) {}

    caf::behavior make_behavior() override {
        initialize_schema();
        caf::message_handler business{
            [this](entity_load_atom, load_request request)
                -> caf::result<load_result> {
                return load(std::move(request));
            },
            [this](entity_save_atom, save_request request)
                -> caf::result<save_result> {
                return save(std::move(request));
            },
            // plugin_lifecycle 的默认 drain 会立刻回执，无法等待异步 SQL。
            // EntityStore 覆盖它：代理已 quiesce 后，等所有已接收请求完成再回执。
            [this](drain_atom, caf::actor coordinator) {
                draining_ = true;
                if (in_flight_ == 0 && !schema_loading_)
                    send(coordinator, drain_atom{}, address());
                else
                    drain_waiters_.push_back(std::move(coordinator));
            },
        };
        PluginLifecycleHooks hooks;
        hooks.on_save = [] { return std::vector<std::byte>{}; };
        hooks.on_shutdown = [this] { draining_ = true; };
        return caf::behavior{business.or_else(plugin_lifecycle(this, hooks))};
    }

    void on_exit() override {
        // CAF response promises retain their owner. Explicitly break the
        // member -> promise -> self cycle even on forced exit without drain.
        draining_ = true;
        settings_.config_error = "entity store exited before request completed";
        schema_loading_ = false;
        auto waiters = std::move(schema_waiters_);
        schema_waiters_.clear();
        for (auto& waiter : waiters)
            waiter(); // ready paths fail immediately on config_error.
        for (auto& [key, queue] : save_queues_) {
            for (auto& context : queue) {
                if (context->completed)
                    continue;
                context->completed = true;
                context->promise.deliver(save_error(
                    context->request,
                    context->begin_sent ? result_code::commit_unknown
                                        : result_code::unavailable,
                    "entity store exited; retry with the same request_id"));
            }
        }
        save_queues_.clear();
        drain_waiters_.clear();
    }

private:
    using load_promise = caf::typed_response_promise<load_result>;
    using save_promise = caf::typed_response_promise<save_result>;

    struct load_context {
        load_request request;
        const entity_schema* schema = nullptr;
        sql_statement statement;
        load_promise promise;
        const store_schema* store = nullptr;
        std::chrono::steady_clock::time_point deadline;
        uint32_t attempt = 0;
    };

    struct save_context {
        save_request request;
        const store_schema* store = nullptr;
        std::vector<const entity_schema*> schemas;
        caf::actor backend;
        save_promise promise;
        std::string partition_key;
        std::string signature;
        std::vector<uint64_t> versions;
        std::chrono::steady_clock::time_point deadline;
        uint64_t transaction = 0;
        std::string transaction_key;
        bool begin_sent = false;
        size_t change_index = 0;
        bool completed = false;
        uint32_t attempt = 0;
        bool retry_requested = false;
        bool commit_uncertain = false;
    };

    caf::result<load_result> load(load_request request) {
        auto promise = make_response_promise<load_result>();
        const auto deadline = std::chrono::steady_clock::now() + settings_.request_timeout;
        if (auto error = admission_error(); !error.empty()) {
            promise.deliver(load_error(request.target, result_code::unavailable,
                                       std::move(error)));
            return promise;
        }
        if (schema_loading_) {
            schema_waiters_.emplace_back(
                [this, request = std::move(request), promise, deadline]() mutable {
                    load_ready(std::move(request), promise, deadline);
                });
            return promise;
        }
        return load_ready(std::move(request), promise, deadline);
    }

    caf::result<load_result> load_ready(load_request request,
                                       load_promise promise,
                                       std::chrono::steady_clock::time_point deadline) {
        if (!settings_.config_error.empty()) {
            promise.deliver(load_error(request.target,
                                       result_code::invalid_request,
                                       settings_.config_error));
            return promise;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            promise.deliver(load_error(request.target, result_code::unavailable,
                                       "load deadline expired while awaiting schema"));
            return promise;
        }
        if (auto error = entity_store::validate(request); !error.empty()) {
            promise.deliver(load_error(request.target,
                                       result_code::invalid_request,
                                       std::move(error)));
            return promise;
        }
        const auto* store = settings_.catalog.find_store(request.target.store);
        const auto* schema = settings_.catalog.find_entity(
            request.target.store, request.target.entity);
        if (!store || !schema) {
            promise.deliver(load_error(request.target,
                                       result_code::invalid_request,
                                       "unknown entity store or entity"));
            return promise;
        }
        auto statement = builder_.load(*schema, request);
        if (!statement) {
            promise.deliver(load_error(request.target,
                                       result_code::invalid_request,
                                       std::move(statement.error)));
            return promise;
        }
        auto backend = resolve_backend(store->read_service);
        if (!backend) {
            promise.deliver(load_error(request.target, result_code::unavailable,
                                       "read service unavailable: "
                                           + store->read_service));
            return promise;
        }

        auto context = std::make_shared<load_context>(load_context{
            std::move(request), schema, std::move(statement.statement), promise});
        context->store = store;
        context->deadline = deadline;
        ++in_flight_;
        start_load(context);
        return promise;
    }

    void start_load(const std::shared_ptr<load_context>& context);
    void retry_load(const std::shared_ptr<load_context>& context,
                    std::string error, bool retryable);

    void finish_load(const std::shared_ptr<load_context>& context,
                     const db::db_result& db_result) {
        if (!db_result.ok) {
            context->promise.deliver(load_error(
                context->request.target, result_code::unavailable,
                db_result.error));
            operation_finished();
            return;
        }
        if (db_result.rows.empty()) {
            auto result = load_error(context->request.target,
                                     result_code::not_found, {});
            context->promise.deliver(std::move(result));
            operation_finished();
            return;
        }
        const auto& row = db_result.rows.front();
        const auto expected = context->statement.result_fields.size() + 1;
        if (row.size() != expected) {
            context->promise.deliver(load_error(
                context->request.target, result_code::internal_error,
                "entity query returned an unexpected column count"));
            operation_finished();
            return;
        }

        load_result output;
        output.code = result_code::ok;
        output.target = context->request.target;
        for (size_t i = 0; i < context->statement.result_fields.size(); ++i) {
            value decoded;
            std::string error;
            const auto is_null = db_result.is_null(0, i);
            const auto* field = context->statement.result_fields[i];
            if (!builder_.decode(row[i], is_null, *field, decoded, error)) {
                context->promise.deliver(load_error(
                    context->request.target, result_code::internal_error,
                    std::move(error)));
                operation_finished();
                return;
            }
            output.fields.push_back({field->name, std::move(decoded)});
        }
        if (!parse_uint64(row.back(), output.version)) {
            context->promise.deliver(load_error(
                context->request.target, result_code::internal_error,
                "entity version is not an unsigned integer"));
            operation_finished();
            return;
        }
        context->promise.deliver(std::move(output));
        operation_finished();
    }

    caf::result<save_result> save(save_request request) {
        auto promise = make_response_promise<save_result>();
        const auto deadline = std::chrono::steady_clock::now() + settings_.request_timeout;
        if (auto error = admission_error(); !error.empty()) {
            promise.deliver(save_error(request, result_code::unavailable,
                                       std::move(error)));
            return promise;
        }
        if (schema_loading_) {
            schema_waiters_.emplace_back(
                [this, request = std::move(request), promise, deadline]() mutable {
                    save_ready(std::move(request), promise, deadline);
                });
            return promise;
        }
        return save_ready(std::move(request), promise, deadline);
    }

    caf::result<save_result> save_ready(save_request request,
                                       save_promise promise,
                                       std::chrono::steady_clock::time_point deadline) {
        if (!settings_.config_error.empty()) {
            promise.deliver(save_error(request, result_code::invalid_request,
                                       settings_.config_error));
            return promise;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            promise.deliver(save_error(request, result_code::unavailable,
                                       "save deadline expired while awaiting schema"));
            return promise;
        }
        if (auto error = entity_store::validate(request); !error.empty()) {
            promise.deliver(save_error(request, result_code::invalid_request,
                                       std::move(error)));
            return promise;
        }
        if (request.request_id.size() > 255) {
            promise.deliver(save_error(request, result_code::invalid_request,
                                       "request_id exceeds 255 bytes"));
            return promise;
        }

        const auto* store = settings_.catalog.find_store(
            request.changes.front().target.store);
        if (!store) {
            promise.deliver(save_error(request, result_code::invalid_request,
                                       "unknown entity store"));
            return promise;
        }
        std::vector<const entity_schema*> schemas;
        schemas.reserve(request.changes.size());
        for (const auto& change : request.changes) {
            auto schema = settings_.catalog.find_entity(
                change.target.store, change.target.entity);
            if (!schema) {
                promise.deliver(save_error(request,
                                           result_code::invalid_request,
                                           "unknown entity: "
                                               + change.target.entity));
                return promise;
            }
            auto update = builder_.update(*schema, change);
            if (!update) {
                promise.deliver(save_error(request,
                                           result_code::invalid_request,
                                           std::move(update.error)));
                return promise;
            }
            if (change.create_if_missing) {
                auto insert = builder_.insert(*schema, change);
                if (!insert) {
                    promise.deliver(save_error(request,
                                               result_code::invalid_request,
                                               std::move(insert.error)));
                    return promise;
                }
            }
            schemas.push_back(schema);
        }
        auto backend = resolve_backend(store->write_service);
        if (!backend) {
            promise.deliver(save_error(request, result_code::unavailable,
                                       "write service unavailable: "
                                           + store->write_service));
            return promise;
        }

        auto context = std::make_shared<save_context>();
        context->request = std::move(request);
        context->store = store;
        context->schemas = std::move(schemas);
        context->backend = std::move(backend);
        context->promise = promise;
        context->partition_key = store->name + "\n"
                                 + context->request.changes.front().target.partition;
        context->signature = request_signature(context->request);
        context->deadline = deadline;
        auto& queue = save_queues_[context->partition_key];
        const auto start_now = queue.empty();
        queue.push_back(context);
        ++in_flight_;
        if (start_now)
            start_save(context);
        return promise;
    }

    caf::actor resolve_backend(const std::string& name) {
        return caf::actor_cast<caf::actor>(system().registry().get(name));
    }

    std::string admission_error() const {
        if (draining_)
            return "entity store is draining";
        if (in_flight_ + schema_waiters_.size() >= settings_.max_pending_requests)
            return "entity store pending request limit reached";
        return {};
    }

    static load_result load_error(entity_ref target, result_code code,
                                  std::string error) {
        load_result result;
        result.code = code;
        result.error = std::move(error);
        result.target = std::move(target);
        return result;
    }

    static save_result save_error(const save_request& request, result_code code,
                                  std::string error) {
        save_result result;
        result.code = code;
        result.error = std::move(error);
        result.request_id = request.request_id;
        return result;
    }

    static bool parse_uint64(std::string_view text, uint64_t& output) {
        auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                      output);
        return parsed.ec == std::errc{}
               && parsed.ptr == text.data() + text.size();
    }

    void operation_finished() {
        if (in_flight_ > 0)
            --in_flight_;
        notify_drained();
    }

    void notify_drained() {
        if (!draining_ || in_flight_ != 0 || schema_loading_)
            return;
        for (auto& waiter : drain_waiters_)
            send(waiter, drain_atom{}, address());
        drain_waiters_.clear();
    }

    // save 状态机实现在本类后半段，所有回调仍在本 actor 的调度线程执行。
    void start_save(const std::shared_ptr<save_context>& context);
    void begin_transaction(const std::shared_ptr<save_context>& context);
    void reserve_request(const std::shared_ptr<save_context>& context);
    void replay_request(const std::shared_ptr<save_context>& context);
    void apply_next_change(const std::shared_ptr<save_context>& context);
    void insert_change(const std::shared_ptr<save_context>& context);
    void load_change_version(const std::shared_ptr<save_context>& context);
    void finish_request_record(const std::shared_ptr<save_context>& context);
    void commit(const std::shared_ptr<save_context>& context);
    void rollback(const std::shared_ptr<save_context>& context,
                  result_code code, std::string error);
    void rollback_replay(const std::shared_ptr<save_context>& context,
                         save_result result);
    void finish_save(const std::shared_ptr<save_context>& context,
                     save_result result);
    void fail_database(const std::shared_ptr<save_context>& context,
                       const db::db_result& result, result_code fallback,
                       std::string error = {});
    void fail_transport(const std::shared_ptr<save_context>& context,
                        const caf::error& error,
                        result_code fallback = result_code::unavailable);
    void retry_or_finish(const std::shared_ptr<save_context>& context,
                         save_result result);
    void initialize_schema();
    void discover_next_schema();
    void finish_schema(std::string error = {});

    std::chrono::milliseconds remaining(
        const std::shared_ptr<save_context>& context) const {
        auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
            context->deadline - std::chrono::steady_clock::now());
        return value > std::chrono::milliseconds{0}
                   ? value
                   : std::chrono::milliseconds{1};
    }

    bool expired(const std::shared_ptr<save_context>& context) const {
        return std::chrono::steady_clock::now() >= context->deadline;
    }

    static std::string request_signature(const save_request& request);
    static std::string encode_versions(const std::vector<uint64_t>& versions);
    static bool decode_versions(std::string_view text, size_t expected,
                                std::vector<uint64_t>& output);

    service_config settings_;
    statement_builder builder_;
    bool draining_ = false;
    size_t in_flight_ = 0;
    uint64_t next_transaction_key_ = 0;
    std::vector<caf::actor> drain_waiters_;
    bool schema_loading_ = false;
    size_t discovery_index_ = 0;
    std::vector<schema_discovery_target> discovery_targets_;
    std::unique_ptr<schema_provider> schema_provider_;
    std::chrono::steady_clock::time_point schema_deadline_;
    std::vector<std::function<void()>> schema_waiters_;
    std::unordered_map<std::string,
                       std::deque<std::shared_ptr<save_context>>>
        save_queues_;
};

inline void entity_store_actor::start_save(
    const std::shared_ptr<save_context>& context) {
    ++context->attempt;
    context->transaction = 0;
    context->begin_sent = false;
    context->retry_requested = false;
    context->change_index = 0;
    context->versions.clear();
    context->transaction_key = caf::to_string(node()) + ":"
                               + std::to_string(id()) + ":"
                               + std::to_string(++next_transaction_key_);
    if (expired(context)) {
        rollback(context, result_code::unavailable,
                 "save deadline expired while queued");
        return;
    }
    auto ddl = settings_.dialect.create_idempotency_table(*context->store);
    request(context->backend, remaining(context), sql_exec_atom_v,
            context->store->write_connection, ddl.text, ddl.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok) {
                    fail_database(context, result, result_code::unavailable,
                             "cannot initialize idempotency table: "
                                 + result.error);
                    return;
                }
                begin_transaction(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::begin_transaction(
    const std::shared_ptr<save_context>& context) {
    if (expired(context)) {
        rollback(context, result_code::unavailable,
                 "save deadline expired before BEGIN");
        return;
    }
    context->begin_sent = true;
    request(context->backend, remaining(context), tx_begin_atom_v,
            context->store->write_connection, context->transaction_key)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok
                    || !parse_uint64(result.insert_id, context->transaction)) {
                    fail_database(context, result, result_code::unavailable,
                             result.error.empty()
                                 ? "database did not return a transaction handle"
                                 : result.error);
                    return;
                }
                reserve_request(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::reserve_request(
    const std::shared_ptr<save_context>& context) {
    if (expired(context)) {
        rollback(context, result_code::unavailable,
                 "save deadline expired after BEGIN");
        return;
    }
    auto command = settings_.dialect.reserve_request(
        *context->store, context->request.request_id, context->signature);
    request(context->backend, remaining(context), sql_exec_atom_v,
            context->transaction, command.text, command.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok) {
                    fail_database(context, result, result_code::internal_error,
                             "cannot reserve request id: " + result.error);
                    return;
                }
                if (result.affected == 0)
                    replay_request(context);
                else
                    apply_next_change(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::replay_request(
    const std::shared_ptr<save_context>& context) {
    auto command = settings_.dialect.load_request_record(
        *context->store, context->request.request_id);
    request(context->backend, remaining(context), sql_query_atom_v,
            context->transaction, command.text, command.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok || result.rows.empty()
                    || result.rows.front().size() != 2) {
                    fail_database(context, result, result_code::commit_unknown,
                             result.error.empty()
                                 ? "idempotent request result is unavailable"
                                 : result.error);
                    return;
                }
                const auto& row = result.rows.front();
                if (row[0] != context->signature) {
                    rollback(context, result_code::conflict,
                             "request_id was already used by another request");
                    return;
                }
                std::vector<uint64_t> versions;
                if (!decode_versions(row[1], context->request.changes.size(),
                                     versions)) {
                    rollback(context, result_code::commit_unknown,
                             "stored idempotent result is incomplete");
                    return;
                }
                save_result replay;
                replay.code = result_code::ok;
                replay.request_id = context->request.request_id;
                replay.committed = true;
                for (size_t i = 0; i < versions.size(); ++i)
                    replay.entities.push_back(
                        {context->request.changes[i].target, versions[i]});
                rollback_replay(context, std::move(replay));
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::apply_next_change(
    const std::shared_ptr<save_context>& context) {
    if (expired(context)) {
        rollback(context, result_code::unavailable,
                 "save deadline expired before applying changes");
        return;
    }
    if (context->change_index >= context->request.changes.size()) {
        finish_request_record(context);
        return;
    }
    const auto index = context->change_index;
    const auto& change = context->request.changes[index];
    auto command = builder_.update(*context->schemas[index], change);
    if (!command) {
        rollback(context, result_code::invalid_request,
                 std::move(command.error));
        return;
    }
    request(context->backend, remaining(context), sql_exec_atom_v,
            context->transaction, command.statement.text,
            command.statement.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok) {
                    fail_database(context, result, result_code::internal_error,
                             result.error);
                    return;
                }
                const auto& change =
                    context->request.changes[context->change_index];
                if (result.affected > 0) {
                    load_change_version(context);
                    return;
                }
                if (change.create_if_missing
                    && (!change.check_version || change.expected_version == 0)) {
                    insert_change(context);
                    return;
                }

                auto exists = builder_.current_version(
                    *context->schemas[context->change_index], change.target);
                if (!exists) {
                    rollback(context, result_code::invalid_request,
                             std::move(exists.error));
                    return;
                }
                request(context->backend, remaining(context), sql_query_atom_v,
                        context->transaction, exists.statement.text,
                        exists.statement.params)
                    .then(
                        [this, context](db::db_result& current) {
                            if (!current.ok) {
                                fail_database(context, current, result_code::internal_error,
                                         current.error);
                                return;
                            }
                            const auto& change = context->request.changes[
                                context->change_index];
                            rollback(context,
                                     current.rows.empty()
                                         ? result_code::not_found
                                         : result_code::conflict,
                                     current.rows.empty()
                                         ? "entity not found"
                                         : (change.check_version
                                                ? "entity version conflict"
                                                : "entity was not updated"));
                        },
                        [this, context](caf::error& error) {
                            fail_transport(context, error);
                        });
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::insert_change(
    const std::shared_ptr<save_context>& context) {
    const auto index = context->change_index;
    auto command = builder_.insert(*context->schemas[index],
                                   context->request.changes[index]);
    if (!command) {
        rollback(context, result_code::invalid_request,
                 std::move(command.error));
        return;
    }
    request(context->backend, remaining(context), sql_exec_atom_v,
            context->transaction, command.statement.text,
            command.statement.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok || result.affected == 0) {
                    fail_database(context, result, result_code::conflict,
                             result.error.empty()
                                 ? "entity insert did not create a row"
                                 : result.error);
                    return;
                }
                load_change_version(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::load_change_version(
    const std::shared_ptr<save_context>& context) {
    const auto index = context->change_index;
    auto command = builder_.current_version(
        *context->schemas[index], context->request.changes[index].target);
    if (!command) {
        rollback(context, result_code::invalid_request,
                 std::move(command.error));
        return;
    }
    request(context->backend, remaining(context), sql_query_atom_v,
            context->transaction, command.statement.text,
            command.statement.params)
        .then(
            [this, context](db::db_result& result) {
                uint64_t version = 0;
                if (!result.ok || result.rows.empty()
                    || result.rows.front().empty()
                    || !parse_uint64(result.rows.front().front(), version)) {
                    fail_database(context, result, result_code::internal_error,
                             result.error.empty()
                                 ? "cannot read committed entity version"
                                 : result.error);
                    return;
                }
                context->versions.push_back(version);
                ++context->change_index;
                apply_next_change(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::finish_request_record(
    const std::shared_ptr<save_context>& context) {
    auto command = settings_.dialect.finish_request_record(
        *context->store, context->request.request_id,
        encode_versions(context->versions));
    request(context->backend, remaining(context), sql_exec_atom_v,
            context->transaction, command.text, command.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok || result.affected != 1) {
                    fail_database(context, result, result_code::internal_error,
                             result.error.empty()
                                 ? "cannot persist idempotent request result"
                                 : result.error);
                    return;
                }
                commit(context);
            },
            [this, context](caf::error& error) {
                fail_transport(context, error);
            });
}

inline void entity_store_actor::commit(
    const std::shared_ptr<save_context>& context) {
    if (expired(context)) {
        rollback(context, result_code::unavailable,
                 "save deadline expired before COMMIT");
        return;
    }
    request(context->backend, remaining(context), tx_commit_atom_v,
            context->transaction)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok) {
                    fail_database(context, result, result_code::commit_unknown,
                             result.error);
                    return;
                }
                save_result committed;
                committed.code = result_code::ok;
                committed.request_id = context->request.request_id;
                committed.committed = true;
                for (size_t i = 0; i < context->versions.size(); ++i)
                    committed.entities.push_back(
                        {context->request.changes[i].target,
                         context->versions[i]});
                finish_save(context, std::move(committed));
            },
            [this, context](caf::error& error) {
                fail_transport(context, error, result_code::commit_unknown);
            });
}

inline void entity_store_actor::rollback(
    const std::shared_ptr<save_context>& context, result_code code,
    std::string error) {
    auto failed = save_error(context->request, code, std::move(error));
    if (!context->begin_sent) {
        retry_or_finish(context, std::move(failed));
        return;
    }
    request(context->backend, std::chrono::seconds(2), tx_rollback_atom_v,
            context->transaction_key)
        .then(
            [this, context, failed](db::db_result&) mutable {
                retry_or_finish(context, failed);
            },
            [this, context, failed](caf::error&) mutable {
                retry_or_finish(context, failed);
            });
}

inline void entity_store_actor::rollback_replay(
    const std::shared_ptr<save_context>& context, save_result result) {
    request(context->backend, std::chrono::seconds(2), tx_rollback_atom_v,
            context->transaction_key)
        .then(
            [this, context, result](db::db_result&) mutable {
                finish_save(context, result);
            },
            [this, context, result](caf::error&) mutable {
                finish_save(context, result);
            });
}

inline void entity_store_actor::finish_save(
    const std::shared_ptr<save_context>& context, save_result result) {
    if (context->completed)
        return;
    context->completed = true;
    context->promise.deliver(std::move(result));

    std::shared_ptr<save_context> next;
    auto it = save_queues_.find(context->partition_key);
    if (it != save_queues_.end()) {
        if (!it->second.empty() && it->second.front() == context)
            it->second.pop_front();
        if (it->second.empty())
            save_queues_.erase(it);
        else
            next = it->second.front();
    }
    operation_finished();
    if (next)
        start_save(next);
}

inline std::string entity_store_actor::request_signature(
    const save_request& request) {
    std::string result;
    auto token = [&result](std::string_view value) {
        result += std::to_string(value.size()) + ":";
        result.append(value);
    };
    auto append_value = [&token](const value& item) {
        token(std::to_string(static_cast<unsigned>(item.kind)));
        switch (item.kind) {
            case value_kind::null_value: token(""); break;
            case value_kind::boolean: token(item.boolean_value ? "1" : "0"); break;
            case value_kind::signed_integer:
                token(std::to_string(item.signed_value));
                break;
            case value_kind::unsigned_integer:
                token(std::to_string(item.unsigned_value));
                break;
            case value_kind::real: {
                std::ostringstream number;
                number.imbue(std::locale::classic());
                number.precision(std::numeric_limits<double>::max_digits10);
                number << item.real_value;
                token(number.str());
                break;
            }
            case value_kind::decimal:
            case value_kind::text:
            case value_kind::json: token(item.text_value); break;
            case value_kind::bytes:
                token(std::string_view{
                    reinterpret_cast<const char*>(item.bytes_value.data()),
                    item.bytes_value.size()});
                break;
        }
    };
    token(std::to_string(request.changes.size()));
    for (const auto& change : request.changes) {
        token(change.target.store);
        token(change.target.partition);
        token(change.target.entity);
        token(std::to_string(change.target.key.size()));
        for (const auto& key : change.target.key) {
            token(key.name);
            append_value(key.data);
        }
        token(change.create_if_missing ? "1" : "0");
        token(change.check_version ? "1" : "0");
        token(std::to_string(change.expected_version));
        token(std::to_string(change.fields.size()));
        for (const auto& field : change.fields) {
            token(std::to_string(static_cast<unsigned>(field.operation)));
            token(field.name);
            append_value(field.data);
        }
    }
    return result;
}

inline std::string entity_store_actor::encode_versions(
    const std::vector<uint64_t>& versions) {
    std::string result;
    for (size_t i = 0; i < versions.size(); ++i) {
        if (i != 0)
            result.push_back(',');
        result += std::to_string(versions[i]);
    }
    return result;
}

inline bool entity_store_actor::decode_versions(
    std::string_view text, size_t expected, std::vector<uint64_t>& output) {
    output.clear();
    size_t begin = 0;
    while (begin <= text.size() && output.size() < expected) {
        const auto end = text.find(',', begin);
        const auto part = text.substr(
            begin, end == std::string_view::npos ? text.size() - begin
                                                 : end - begin);
        uint64_t version = 0;
        if (part.empty() || !parse_uint64(part, version))
            return false;
        output.push_back(version);
        if (end == std::string_view::npos)
            return output.size() == expected;
        begin = end + 1;
    }
    return false;
}

} // namespace caf_plugin_system::entity_store::sql

#include "templates/sql_entity_store_recovery.hpp"
