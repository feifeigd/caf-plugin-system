#pragma once

#include "mongo_entity_codec.hpp"
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/transaction.hpp>
#include <mongocxx/read_concern.hpp>
#include <mongocxx/read_preference.hpp>
#include <mongocxx/write_concern.hpp>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <unordered_set>

namespace caf_plugin_system::entity_store::mongo {

class OperationBudget {
public:
    using clock = std::chrono::steady_clock;
    OperationBudget(clock::time_point deadline, std::stop_token stop)
        : deadline_(deadline), stop_(stop) {}
    std::chrono::milliseconds remaining() const {
        auto result = std::chrono::duration_cast<std::chrono::milliseconds>(deadline_ - clock::now());
        if (stop_.stop_requested() || result.count() <= 0)
            throw EntityError{result_code::unavailable, "Mongo operation stopped or deadline expired"};
        return result;
    }
    bool pause(std::chrono::milliseconds delay) const {
        try {
            if (delay >= remaining()) return false;
            std::mutex mutex;
            std::unique_lock lock{mutex};
            std::condition_variable_any changed;
            changed.wait_for(lock, stop_, delay, [] { return false; });
            remaining();
            return true;
        } catch (const EntityError&) { return false; }
    }
private:
    clock::time_point deadline_;
    std::stop_token stop_;
};

class NativeTransaction {
public:
    NativeTransaction(mongocxx::client_session& session, const OperationBudget& budget)
        : session_(session) {
        mongocxx::options::transaction options;
        mongocxx::read_concern reads;
        reads.acknowledge_string("snapshot");
        mongocxx::write_concern writes;
        writes.majority(std::min(budget.remaining(), std::chrono::milliseconds{2000}));
        mongocxx::read_preference preference;
        preference.mode(mongocxx::read_preference::read_mode::k_primary);
        options.read_concern(reads).write_concern(writes).read_preference(preference)
            .max_commit_time_ms(std::min(budget.remaining(), std::chrono::milliseconds{2000}));
        session_.start_transaction(options);
        active_ = true;
    }
    ~NativeTransaction() {
        if (active_) {
            try { session_.abort_transaction(); } catch (...) {}
        }
    }
    NativeTransaction(const NativeTransaction&) = delete;
    NativeTransaction& operator=(const NativeTransaction&) = delete;
    // A commit whose outcome is unknown must never be followed by a new
    // transaction on this session or claimed to have been rolled back.
    void release() noexcept { active_ = false; }
private:
    mongocxx::client_session& session_;
    bool active_ = false;
};

// Every instance, client, session and cursor is used exclusively by one worker.
class EntityStoreEngine {
public:
    using clock = OperationBudget::clock;
    EntityStoreEngine(mongocxx::client& client, std::string database,
                      std::string connection, const service_config& settings,
                      std::stop_token stop)
        : client_(client), database_(std::move(database)), connection_(std::move(connection)),
          settings_(settings), stop_(stop) {}

    load_result load(const load_request& request, clock::time_point deadline) {
        OperationBudget budget{deadline, stop_};
        load_result result;
        result.target = request.target;
        try {
            check_config();
            if (auto error = validate(request); !error.empty()) EntityCodec::invalid(error);
            const auto& store = find_store(request.target.store, false);
            const auto& schema = find_entity(store, request.target.entity);
            auto filter = EntityCodec::key_filter(schema, request.target);
            std::vector<const sql::field_schema*> fields;
            if (request.fields.all_fields) {
                for (const auto& field : schema.fields) fields.push_back(&field);
            } else {
                for (const auto& name : request.fields.names) {
                    auto field = schema.find_field(name);
                    if (!field) EntityCodec::invalid("unknown projected field: " + name);
                    fields.push_back(field);
                }
            }
            bson::document projection;
            projection.append(bson::kvp(schema.version_column, 1));
            std::unordered_set<std::string> projected{schema.version_column};
            for (const auto& key : schema.keys) {
                if (projected.insert(key.column).second) projection.append(bson::kvp(key.column, 1));
            }
            for (auto field : fields) {
                if (projected.insert(field->column).second) projection.append(bson::kvp(field->column, 1));
            }
            for (uint32_t attempt = 1; ; ++attempt) {
                try {
                    mongocxx::options::find options;
                    options.max_time(budget.remaining()).projection(projection.view());
                    auto document = client_[database_][schema.table].find_one(filter.view(), options);
                    budget.remaining();
                    if (!document) { result.code = result_code::not_found; return result; }
                    validate_document_keys(document->view(), schema);
                    result.version = static_cast<uint64_t>(EntityCodec::version(document->view(), schema));
                    for (auto field : fields)
                        result.fields.push_back({field->name, EntityCodec::decode(document->view()[field->column], *field)});
                    result.code = result_code::ok;
                    return result;
                } catch (const mongocxx::operation_exception& error) {
                    if (retryable(error) && attempt < settings_.load_retry_attempts
                        && budget.pause(backoff(attempt))) continue;
                    throw;
                }
            }
        } catch (const EntityError& error) {
            result.code = error.code(); result.error = error.what();
        } catch (const bsoncxx::exception& error) {
            result.code = result_code::invalid_request; result.error = error.what();
        } catch (const mongocxx::operation_exception& error) {
            result.code = retryable(error) ? result_code::unavailable : result_code::internal_error;
            result.error = error.what();
        } catch (const std::exception& error) {
            result.code = result_code::unavailable; result.error = error.what();
        }
        result.fields.clear(); result.version = 0;
        return result;
    }

    save_result save(const save_request& request, clock::time_point deadline) {
        OperationBudget budget{deadline, stop_};
        try {
            check_config();
            if (auto error = validate(request); !error.empty()) EntityCodec::invalid(error);
            if (request.request_id.size() > 255 || request.changes.size() > 1024)
                EntityCodec::invalid("save request is too large");
            const auto& store = find_store(request.changes.front().target.store, true);
            for (const auto& change : request.changes) validate_patch(find_entity(store, change.target.entity), change);
            auto signature = EntityCodec::signature(request);
            for (uint32_t attempt = 1; ; ++attempt) {
                try {
                    budget.remaining();
                    ensure_indexes(store, budget);
                    auto session = client_.start_session();
                    NativeTransaction transaction{session, budget};
                    auto records = client_[database_][store.idempotency_table];
                    // Match the SQL contract: IDs are globally unique within
                    // the configured idempotency collection. Store/partition
                    // remain part of the signature, not the deduplication key.
                    auto filter = bson::make_document(bson::kvp("_id", request.request_id));
                    mongocxx::options::find options;
                    options.max_time(budget.remaining());
                    if (auto existing = records.find_one(session, filter.view(), options))
                        return replay(request, signature.view(), existing->view());
                    // Reserve the id before any business write; concurrent duplicate
                    // requests conflict here, then retry and read the committed result.
                    auto reserved = records.insert_one(session, bson::make_document(
                        bson::kvp("_id", request.request_id), bson::kvp("signature", signature.view())));
                    if (!reserved) EntityCodec::corrupt("unacknowledged idempotency reservation");
                    save_result result;
                    result.request_id = request.request_id;
                    bson::array versions;
                    for (const auto& change : request.changes) {
                        const auto version = apply(session, find_entity(store, change.target.entity), change, budget);
                        result.entities.push_back({change.target, static_cast<uint64_t>(version), change.operation});
                        versions.append(version);
                    }
                    mongocxx::options::find_one_and_update finish;
                    finish.max_time(budget.remaining());
                    auto updated = records.find_one_and_update(session, filter.view(),
                        bson::make_document(bson::kvp("$set", bson::make_document(
                            bson::kvp("versions", versions.extract())))), finish);
                    if (!updated) EntityCodec::corrupt("idempotency reservation disappeared");
                    bool uncertain = false;
                    for (uint32_t commit_attempt = 1; ; ++commit_attempt) {
                        try {
                            budget.remaining();
                            session.commit_transaction();
                            transaction.release();
                            result.code = result_code::ok;
                            result.committed = true;
                            return result;
                        } catch (const mongocxx::operation_exception& error) {
                            if (error.has_error_label("UnknownTransactionCommitResult")
                                || (!error.raw_server_error() && !error.has_error_label("TransientTransactionError"))) {
                                uncertain = true;
                                if (commit_attempt < settings_.save_retry_attempts
                                    && budget.pause(backoff(commit_attempt))) continue;
                                transaction.release();
                                return failure(request, result_code::commit_unknown, error.what());
                            }
                            if (uncertain) {
                                transaction.release();
                                return failure(request, result_code::commit_unknown, error.what());
                            }
                            throw;
                        } catch (const EntityError& error) {
                            if (uncertain) {
                                transaction.release();
                                return failure(request, result_code::commit_unknown, error.what());
                            }
                            throw;
                        } catch (const std::exception& error) {
                            // An unexpected client-side error after entering
                            // commit cannot prove the server did not commit.
                            transaction.release();
                            return failure(request, result_code::commit_unknown, error.what());
                        }
                    }
                } catch (const mongocxx::operation_exception& error) {
                    // Transaction destructor aborts before entering this handler.
                    if ((retryable(error) || duplicate_key(error))
                        && attempt < settings_.save_retry_attempts && budget.pause(backoff(attempt))) continue;
                    return failure(request, duplicate_key(error) ? result_code::conflict
                        : (retryable(error) ? result_code::unavailable : result_code::internal_error), error.what());
                }
            }
        } catch (const EntityError& error) { return failure(request, error.code(), error.what()); }
        catch (const bsoncxx::exception& error) { return failure(request, result_code::invalid_request, error.what()); }
        catch (const std::exception& error) { return failure(request, result_code::unavailable, error.what()); }
    }

private:
    void check_config() const {
        if (!settings_.config_error.empty()) EntityCodec::invalid(settings_.config_error);
        if (database_.empty()) EntityCodec::invalid("Mongo EntityStore connection URI requires a database");
    }
    const sql::store_schema& find_store(const std::string& name, bool write) const {
        auto store = settings_.catalog.find_store(name);
        if (!store) EntityCodec::invalid("unknown entity store: " + name);
        if ((write ? store->write_connection : store->read_connection) != connection_)
            EntityCodec::invalid("connection does not match entity store routing");
        return *store;
    }
    static const sql::entity_schema& find_entity(const sql::store_schema& store, const std::string& name) {
        auto found = store.entities.find(name);
        if (found == store.entities.end()) EntityCodec::invalid("unknown entity: " + name);
        return found->second;
    }
    std::chrono::milliseconds backoff(uint32_t attempt) const {
        return std::min(settings_.retry_backoff * (1u << std::min(attempt - 1, 5u)), std::chrono::milliseconds{1000});
    }
    static bool duplicate_key(const mongocxx::operation_exception& error) {
        return error.code().value() == 11000 || error.code().value() == 11001;
    }
    static bool retryable(const mongocxx::operation_exception& error) {
        if (error.has_error_label("TransientTransactionError")
            || error.has_error_label("RetryableWriteError") || !error.raw_server_error()) return true;
        switch (error.code().value()) {
            case 6: case 7: case 50: case 89: case 91: case 189: case 262:
            case 9001: case 10107: case 11600: case 11602: case 13435: case 13436: return true;
            default: return false;
        }
    }
    static save_result failure(const save_request& request, result_code code, std::string message) {
        save_result result;
        result.request_id = request.request_id; result.code = code; result.error = std::move(message);
        return result;
    }
    static void validate_patch(const sql::entity_schema& schema, const entity_patch& patch) {
        EntityCodec::key_filter(schema, patch.target);
        if (patch.expected_version > uint64_t(std::numeric_limits<int64_t>::max()))
            EntityCodec::invalid("expected version exceeds BSON int64");
        for (const auto& field : patch.fields) {
            auto mapped = schema.find_writable_field(field.name);
            if (!mapped) EntityCodec::invalid("unknown or read-only field: " + field.name);
            if (field.operation == patch_op::erase) {
                if (!mapped->nullable) EntityCodec::invalid("cannot erase required field: " + field.name);
            } else if (field.operation == patch_op::increment) {
                bson::document delta;
                EntityCodec::append_increment(delta, mapped->column, field.data, mapped->kind);
            } else if (field.operation == patch_op::set) {
                EntityCodec::validate_value(field.data, *mapped);
            } else EntityCodec::invalid("unknown patch operation");
        }
    }
    void ensure_indexes(const sql::store_schema& store, const OperationBudget& budget) {
        if (indexed_.contains(store.name)) return;
        auto database = client_[database_];
        // createIndexes outside the transaction also materializes collections.
        auto ensure = [&](const std::string& collection, bsoncxx::document::value keys,
                          const std::string& name, bool unique) {
            bson::document index;
            index.append(bson::kvp("key", keys.view()), bson::kvp("name", name));
            if (unique) index.append(bson::kvp("unique", true));
            bson::array indexes;
            indexes.append(index.extract());
            database.run_command(bson::make_document(bson::kvp("createIndexes", collection),
                bson::kvp("indexes", indexes.extract()), bson::kvp("maxTimeMS", int64_t(budget.remaining().count()))));
        };
        ensure(store.idempotency_table, bson::make_document(bson::kvp("_id", 1)), "_id_", false);
        for (const auto& [name, entity] : store.entities) {
            bson::document keys;
            bool only_id = entity.keys.size() == 1 && entity.keys.front().column == "_id";
            for (const auto& key : entity.keys) keys.append(bson::kvp(key.column, 1));
            ensure(entity.table, keys.extract(), only_id ? "_id_" : "__entity_store_key_" + name, !only_id);
        }
        indexed_.insert(store.name);
    }
    static void validate_document_keys(bsoncxx::document::view document, const sql::entity_schema& schema) {
        for (const auto& key : schema.keys) {
            if (!document[key.column] || document[key.column].type() == bsoncxx::type::k_null)
                EntityCodec::corrupt("missing/null entity key");
            EntityCodec::decode(document[key.column], key);
        }
    }
    static void validate_document(bsoncxx::document::view document, const sql::entity_schema& schema) {
        validate_document_keys(document, schema);
        for (const auto& field : schema.fields) EntityCodec::decode(document[field.column], field);
    }
    int64_t apply(mongocxx::client_session& session, const sql::entity_schema& schema,
                  const entity_patch& patch, const OperationBudget& budget) {
        auto collection = client_[database_][schema.table];
        auto filter = EntityCodec::key_filter(schema, patch.target);
        mongocxx::options::find find;
        find.max_time(budget.remaining());
        auto before = collection.find_one(session, filter.view(), find);
        if (!before) {
            if ((patch.operation != entity_operation::insert && !patch.create_if_missing)
                || (patch.check_version && patch.expected_version != 0))
                throw EntityError{result_code::not_found, "entity not found"};
            bson::document created;
            for (const auto& key : patch.target.key)
                EntityCodec::append(created, schema.find_key(key.name)->column, key.data);
            created.append(bson::kvp(schema.version_column, int64_t{1}));
            bson::document increments;
            for (const auto& field : patch.fields) {
                const auto* mapped = schema.find_writable_field(field.name);
                if (field.operation == patch_op::increment) {
                    EntityCodec::append(created, mapped->column, EntityCodec::zero(mapped->kind));
                    EntityCodec::append_increment(increments, mapped->column, field.data, mapped->kind);
                } else if (field.operation == patch_op::set)
                    EntityCodec::append(created, mapped->column, field.data);
            }
            auto document = created.extract();
            // No server defaults are assumed for omitted required fields.
            try { validate_document(document.view(), schema); }
            catch (const EntityError& error) { EntityCodec::invalid(error.what()); }
            budget.remaining();
            if (!collection.insert_one(session, document.view())) EntityCodec::corrupt("unacknowledged entity insert");
            if (!increments.view().empty()) {
                mongocxx::options::find_one_and_update options;
                options.max_time(budget.remaining()).return_document(mongocxx::options::return_document::k_after);
                auto after = collection.find_one_and_update(session, filter.view(),
                    bson::make_document(bson::kvp("$inc", increments.extract())), options);
                if (!after) EntityCodec::corrupt("new entity disappeared during increment");
                try { validate_document(after->view(), schema); }
                catch (const EntityError& error) { EntityCodec::invalid(error.what()); }
            }
            return 1;
        }
        if (patch.operation == entity_operation::insert)
            throw EntityError{result_code::conflict, "entity already exists"};
        validate_document(before->view(), schema);
        const auto version = EntityCodec::version(before->view(), schema);
        if (patch.check_version && patch.expected_version != uint64_t(version))
            throw EntityError{result_code::conflict, "entity version conflict"};
        if (patch.operation == entity_operation::delete_entity) {
            budget.remaining();
            auto removed = collection.delete_one(session, filter.view());
            if (!removed || removed->deleted_count() != 1)
                throw EntityError{result_code::conflict, "entity disappeared during delete"};
            return 0;
        }
        if (version == std::numeric_limits<int64_t>::max()) EntityCodec::invalid("entity version exhausted");
        bson::document set, increment, unset, update;
        increment.append(bson::kvp(schema.version_column, int64_t{1}));
        for (const auto& field : patch.fields) {
            const auto& column = schema.find_writable_field(field.name)->column;
            if (field.operation == patch_op::set) EntityCodec::append(set, column, field.data);
            else if (field.operation == patch_op::increment)
                EntityCodec::append_increment(increment, column, field.data, schema.find_writable_field(field.name)->kind);
            else unset.append(bson::kvp(column, ""));
        }
        if (!set.view().empty()) update.append(bson::kvp("$set", set.extract()));
        if (!unset.view().empty()) update.append(bson::kvp("$unset", unset.extract()));
        update.append(bson::kvp("$inc", increment.extract()));
        mongocxx::options::find_one_and_update options;
        options.max_time(budget.remaining()).return_document(mongocxx::options::return_document::k_after);
        auto after = collection.find_one_and_update(session, filter.view(), update.extract(), options);
        if (!after) throw EntityError{result_code::conflict, "entity disappeared during update"};
        // Reject Mongo's overflow promotions and invalid externally stored types
        // while still in the transaction, so no partially valid write commits.
        try { validate_document(after->view(), schema); }
        catch (const EntityError& error) { EntityCodec::invalid(error.what()); }
        return EntityCodec::version(after->view(), schema);
    }
    static save_result replay(const save_request& request, bsoncxx::document::view signature,
                              bsoncxx::document::view record) {
        auto stored = record["signature"];
        if (!stored || stored.type() != bsoncxx::type::k_document)
            EntityCodec::corrupt("invalid idempotency signature record");
        auto original = stored.get_document().value;
        if (original.length() != signature.length()
            || std::memcmp(original.data(), signature.data(), signature.length()) != 0)
            return failure(request, result_code::conflict, "request_id was used with different content");
        auto versions = record["versions"];
        if (!versions || versions.type() != bsoncxx::type::k_array)
            EntityCodec::corrupt("idempotency result is incomplete");
        save_result result;
        result.request_id = request.request_id;
        size_t i = 0;
        for (const auto& version : versions.get_array().value) {
            if (i >= request.changes.size() || version.type() != bsoncxx::type::k_int64
                || version.get_int64().value < 0
                || ((version.get_int64().value == 0)
                    != (request.changes[i].operation == entity_operation::delete_entity)))
                EntityCodec::corrupt("invalid idempotency result version");
            const auto& change = request.changes[i++];
            result.entities.push_back({change.target, uint64_t(version.get_int64().value), change.operation});
        }
        if (i != request.changes.size()) EntityCodec::corrupt("idempotency result size mismatch");
        result.code = result_code::ok; result.committed = true;
        return result;
    }

    mongocxx::client& client_;
    std::string database_;
    std::string connection_;
    const service_config& settings_;
    std::stop_token stop_;
    std::unordered_set<std::string> indexed_;
};
} // namespace caf_plugin_system::entity_store::mongo
