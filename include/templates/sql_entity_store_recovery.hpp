#pragma once

// Included after entity_store_actor's declaration. All continuations run on
// its mailbox: retries retain the partition queue head and original request ID.

namespace caf_plugin_system::entity_store::sql {

inline void entity_store_actor::start_load(
    const std::shared_ptr<load_context>& context) {
    ++context->attempt;
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        context->deadline - std::chrono::steady_clock::now());
    auto backend = resolve_backend(context->store->read_service);
    if (!backend || timeout.count() <= 0) {
        retry_load(context, "read service unavailable or deadline expired", true);
        return;
    }
    request(backend, timeout, sql_query_atom_v, context->store->read_connection,
            context->statement.text, context->statement.params)
        .then(
            [this, context](db::db_result& result) {
                if (!result.ok) {
                    retry_load(context, result.error,
                               db::is_connection_error(result.code));
                    return;
                }
                finish_load(context, result);
            },
            [this, context](caf::error& error) {
                retry_load(context, caf::to_string(error), true);
            });
}

inline void entity_store_actor::retry_load(
    const std::shared_ptr<load_context>& context, std::string error,
    bool retryable) {
    const auto delay = std::min(settings_.retry_backoff
                                * (1u << std::min(context->attempt - 1, 5u)),
                                std::chrono::milliseconds{1000});
    if (retryable && context->attempt < settings_.load_retry_attempts
        && std::chrono::steady_clock::now() + delay < context->deadline) {
        run_delayed_weak(delay, [this, context] { start_load(context); });
        return;
    }
    context->promise.deliver(load_error(context->request.target,
                                       result_code::unavailable, std::move(error)));
    operation_finished();
}

inline void entity_store_actor::initialize_schema() {
    if (!settings_.config_error.empty())
        return;
    discovery_targets_ = settings_.catalog.discovery_targets();
    if (discovery_targets_.empty())
        return;
    schema_loading_ = true;
    schema_provider_ = make_schema_provider(settings_.dialect.kind());
    schema_deadline_ = std::chrono::steady_clock::now()
                       + settings_.request_timeout;
    discover_next_schema();
}

inline void entity_store_actor::discover_next_schema() {
    if (discovery_index_ == discovery_targets_.size()) {
        finish_schema();
        return;
    }
    auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        schema_deadline_ - std::chrono::steady_clock::now());
    if (timeout.count() <= 0) {
        finish_schema("database schema discovery deadline exceeded");
        return;
    }
    const auto& target = discovery_targets_[discovery_index_];
    const auto* store = settings_.catalog.find_store(target.store);
    auto* entity = settings_.catalog.mutable_entity(target.store, target.entity);
    auto backend = resolve_backend(store->write_service);
    if (!backend) {
        run_delayed_weak(std::min(settings_.retry_backoff, timeout),
                         [this] { discover_next_schema(); });
        return;
    }
    auto query = schema_provider_->metadata_query(*entity);
    request(backend, timeout, sql_query_atom_v, store->write_connection,
            query.text, query.params)
        .then(
            [this](db::db_result& result) {
                if (!result.ok && db::is_connection_error(result.code)) {
                    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                        schema_deadline_ - std::chrono::steady_clock::now());
                    if (remaining.count() <= 0) {
                        finish_schema("database schema discovery deadline exceeded");
                        return;
                    }
                    run_delayed_weak(std::min(settings_.retry_backoff, remaining),
                                     [this] { discover_next_schema(); });
                    return;
                }
                const auto& target = discovery_targets_[discovery_index_];
                auto* entity = settings_.catalog.mutable_entity(
                    target.store, target.entity);
                std::string error;
                if (!schema_provider_->apply(*entity, result, error)) {
                    finish_schema("schema discovery failed (" + target.store
                                  + "/" + target.entity + "): " + error);
                    return;
                }
                ++discovery_index_;
                discover_next_schema();
            },
            [this](caf::error& error) {
                finish_schema("schema discovery failed: " + caf::to_string(error));
            });
}

inline void entity_store_actor::finish_schema(std::string error) {
    schema_loading_ = false;
    if (!error.empty())
        settings_.config_error = std::move(error);
    // No schema pointer has escaped while the catalog was being populated.
    // Publish the complete snapshot before admitting any deferred operation.
    auto waiters = std::move(schema_waiters_);
    schema_waiters_.clear();
    for (auto& waiter : waiters)
        waiter();
    notify_drained();
}

inline void entity_store_actor::fail_database(
    const std::shared_ptr<save_context>& context, const db::db_result& result,
    result_code fallback, std::string error) {
    context->retry_requested = db::is_connection_error(result.code);
    if (fallback == result_code::commit_unknown)
        context->commit_uncertain = true;
    rollback(context,
             context->retry_requested ? result_code::unavailable : fallback,
             error.empty() ? result.error : std::move(error));
}

inline void entity_store_actor::fail_transport(
    const std::shared_ptr<save_context>& context, const caf::error& error,
    result_code fallback) {
    // Only generated EntityStore operations reach here. Arbitrary SQL is never
    // retried by this layer; save always restarts its entire idempotent unit.
    context->retry_requested = true;
    if (fallback == result_code::commit_unknown)
        context->commit_uncertain = true;
    rollback(context, fallback, caf::to_string(error));
}

inline void entity_store_actor::retry_or_finish(
    const std::shared_ptr<save_context>& context, save_result result) {
    if (context->completed)
        return;
    if (context->commit_uncertain)
        result.code = result_code::commit_unknown;
    const auto multiplier = 1u << std::min(context->attempt - 1, 5u);
    const auto delay = std::min(settings_.retry_backoff * multiplier,
                                std::chrono::milliseconds{1000});
    if (!context->retry_requested
        || context->attempt >= settings_.save_retry_attempts
        || std::chrono::steady_clock::now() + delay >= context->deadline) {
        finish_save(context, std::move(result));
        return;
    }
    // Rollback/cancel of the previous BEGIN key has completed (or timed out).
    // A new key and backend transaction token prevent late replies from being
    // mistaken for the new physical transaction.
    run_delayed_weak(delay, [this, context] {
        context->backend = resolve_backend(context->store->write_service);
        if (!context->backend) {
            ++context->attempt;
            retry_or_finish(context, save_error(
                context->request, result_code::unavailable,
                "write service unavailable during retry"));
            return;
        }
        start_save(context);
    });
}

} // namespace caf_plugin_system::entity_store::sql
