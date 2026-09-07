#pragma once
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <hiredis/hiredis.h>
#include "redis_entity_codec.hpp"
#include "templates/redis_entity_store_config.hpp"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

namespace caf_plugin_system::entity_store::redis {

struct ConnectionOptions {
    std::string host = "127.0.0.1";
    int port = 6379;
    int database = 0;
};

// Worker-thread owned. Reconnects before a subsequent request, never replays
// a command whose reply was lost. Entity and raw commands use separate sessions.
class NativeConnection final {
public:
    using clock = std::chrono::steady_clock;
    using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
    explicit NativeConnection(ConnectionOptions options) : options_(std::move(options)) {}

    Reply command(const std::vector<std::string>& arguments, clock::time_point deadline, bool writes = false) {
        if (arguments.empty() || arguments.size() > size_t(std::numeric_limits<int>::max()))
            invalid("invalid Redis command size");
        ensure_connected(deadline);
        auto timeout = timeout_for(deadline);
        if (redisSetTimeout(connection_.get(), timeout) != REDIS_OK) {
            connection_.reset(); throw EntityError{result_code::unavailable, "Redis command timeout setup failed"};
        }
        std::vector<const char*> argv;
        std::vector<size_t> sizes;
        for (const auto& argument : arguments) { argv.push_back(argument.data()); sizes.push_back(argument.size()); }
        Reply reply{static_cast<redisReply*>(redisCommandArgv(connection_.get(),
            static_cast<int>(argv.size()), argv.data(), sizes.data())), freeReplyObject};
        if (!reply) {
            std::string error = connection_->err ? connection_->errstr : "Redis reply missing";
            connection_.reset();
            throw EntityError{writes ? result_code::commit_unknown : result_code::unavailable,
                std::move(error)};
        }
        return reply;
    }

private:
    static timeval timeout_for(clock::time_point deadline) {
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (millis <= 0) throw EntityError{result_code::unavailable, "Redis request deadline expired"};
        millis = std::min<int64_t>(millis, 2000);
        return {static_cast<long>(millis / 1000), static_cast<long>((millis % 1000) * 1000)};
    }
    void ensure_connected(clock::time_point deadline) {
        if (connection_ && !connection_->err) return;
        auto timeout = timeout_for(deadline);
        connection_.reset(redisConnectWithTimeout(options_.host.c_str(), options_.port, timeout));
        if (!connection_ || connection_->err) {
            std::string error = connection_ ? connection_->errstr : "Redis allocation failed";
            connection_.reset(); throw EntityError{result_code::unavailable, std::move(error)};
        }
        if (redisSetTimeout(connection_.get(), timeout_for(deadline)) != REDIS_OK) {
            connection_.reset(); throw EntityError{result_code::unavailable, "Redis timeout setup failed"};
        }
        // SELECT must be acknowledged, including db=0 after reconnect.
        Reply selected{static_cast<redisReply*>(redisCommand(connection_.get(), "SELECT %d", options_.database)), freeReplyObject};
        if (!selected || selected->type != REDIS_REPLY_STATUS
            || std::string_view{selected->str, selected->len} != "OK") {
            connection_.reset(); throw EntityError{result_code::unavailable, "Redis SELECT failed"};
        }
    }
    ConnectionOptions options_;
    std::unique_ptr<redisContext, decltype(&redisFree)> connection_{nullptr, redisFree};
};

class EntityStoreEngine final {
public:
    using clock = NativeConnection::clock;
    EntityStoreEngine(NativeConnection& connection, std::string name,
                      const service_config& settings, std::stop_token stop)
        : connection_(connection), name_(std::move(name)), settings_(settings), stop_(stop) {}

    load_result load(const load_request& request, clock::time_point deadline) {
        load_result result; result.target = request.target;
        try {
            check_config();
            if (auto error = validate(request); !error.empty()) invalid(error);
            const auto& store = find_store(request.target.store, false);
            const auto& mapped = find_entity(store, request.target.entity);
            const auto field = EntityCodec::object_field(mapped, request.target);
            std::vector<const schema::field_schema*> selected;
            if (request.fields.all_fields) {
                for (const auto& column : mapped.fields) selected.push_back(&column);
            } else for (const auto& name : request.fields.names) {
                auto column = mapped.find_field(name);
                if (!column) invalid("unknown projected field: " + name);
                selected.push_back(column);
            }
            for (uint32_t attempt = 1; ; ++attempt) {
                try {
                    check_budget(deadline);
                    auto reply = connection_.command({"HGET", EntityCodec::hash_key(store), field}, deadline);
                    check_reply(*reply);
                    if (reply->type == REDIS_REPLY_NIL) { result.code = result_code::not_found; return result; }
                    auto document = EntityCodec::decode(string_reply(*reply));
                    validate_document(document, mapped);
                    for (auto column : selected) {
                        auto found = document.fields.find(column->column);
                        result.fields.push_back({column->name, found == document.fields.end() ? value::null() : found->second});
                    }
                    result.version = document.version; result.code = result_code::ok; return result;
                } catch (const EntityError& error) {
                    if (error.code() == result_code::unavailable && attempt < settings_.load_retry_attempts
                        && pause(deadline, attempt)) continue;
                    throw;
                }
            }
        } catch (const EntityError& error) { result.code = error.code(); result.error = error.what(); }
        catch (const std::exception& error) { result.code = result_code::internal_error; result.error = error.what(); }
        result.fields.clear(); result.version = 0; return result;
    }

    save_result save(const save_request& request, clock::time_point deadline) {
        bool committing = false;
        try {
            check_config();
            if (auto error = validate(request); !error.empty()) invalid(error);
            if (request.request_id.size() > 255 || request.changes.size() > 1024) invalid("save request is too large");
            const auto& store = find_store(request.changes.front().target.store, true);
            std::vector<std::string> fields;
            for (const auto& patch : request.changes) {
                const auto& mapped = find_entity(store, patch.target.entity);
                validate_patch(mapped, patch);
                fields.push_back(EntityCodec::object_field(mapped, patch.target));
            }
            const auto signature = EntityCodec::signature(request);
            const auto hash = EntityCodec::hash_key(store);
            const auto ledger = EntityCodec::ledger_field(request.request_id);
            for (uint32_t attempt = 1; ; ++attempt) {
                try {
                    check_budget(deadline);
                    // Duplicated entities in a batch are applied sequentially in
                    // memory, then each physical object is written only once.
                    std::map<std::string, std::optional<std::string>> snapshots;
                    for (const auto& field : fields) snapshots.emplace(field, std::nullopt);
                    std::vector<std::string> reads{"HMGET", hash, ledger};
                    for (const auto& [field, unused] : snapshots) reads.push_back(field);
                    auto before = connection_.command(reads, deadline);
                    check_reply(*before);
                    if (before->type != REDIS_REPLY_ARRAY || before->elements != snapshots.size() + 1)
                        corrupt("unexpected Redis snapshot reply");
                    if (before->element[0]->type != REDIS_REPLY_NIL)
                        return EntityCodec::replay(request, signature, string_reply(*before->element[0]));
                    size_t index = 1, total_bytes = 0;
                    std::map<std::string, Document> documents;
                    for (auto& [field, bytes] : snapshots) {
                        auto* item = before->element[index++];
                        if (item->type != REDIS_REPLY_NIL) {
                            bytes = string_reply(*item); total_bytes += bytes->size();
                            if (total_bytes > Writer::max_bytes) invalid("batch snapshot exceeds 8 MiB");
                            documents.emplace(field, EntityCodec::decode(*bytes));
                        }
                    }
                    save_result result; result.request_id = request.request_id;
                    for (size_t i = 0; i < request.changes.size(); ++i) {
                        const auto& patch = request.changes[i];
                        auto& document = documents[fields[i]];
                        apply(document, find_entity(store, patch.target.entity), patch);
                        result.entities.push_back({patch.target, document.version});
                    }
                    const auto committed_record = EntityCodec::ledger(signature, result);
                    std::vector<std::string> command{"EVAL", commit_script(), "1", hash, ledger,
                        committed_record, std::to_string(snapshots.size())};
                    total_bytes = committed_record.size();
                    for (const auto& [field, bytes] : snapshots) {
                        auto updated = EntityCodec::encode(documents.at(field));
                        total_bytes += field.size() + updated.size() + (bytes ? bytes->size() : 0);
                        if (total_bytes > 3 * Writer::max_bytes) invalid("atomic batch exceeds 24 MiB");
                        command.push_back(field); command.push_back(bytes ? "1" : "0");
                        command.push_back(bytes.value_or("")); command.push_back(std::move(updated));
                    }
                    check_budget(deadline);
                    committing = true;
                    auto reply = connection_.command(command, deadline, true);
                    committing = false;
                    check_reply(*reply);
                    if (reply->type == REDIS_REPLY_INTEGER) {
                        result.code = result_code::ok; result.committed = true; return result;
                    }
                    if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 1
                        || reply->element[0]->type != REDIS_REPLY_INTEGER) corrupt("unexpected Redis commit reply");
                    if (reply->element[0]->integer == 2 && reply->elements == 2)
                        return EntityCodec::replay(request, signature, string_reply(*reply->element[1]));
                    if (reply->element[0]->integer != 0) corrupt("unknown Redis commit status");
                    if (attempt < settings_.save_retry_attempts && pause(deadline, attempt)) continue;
                    return failure(request, result_code::conflict, "concurrent update; retry with the same request_id");
                } catch (const EntityError& error) {
                    // NativeConnection classifies before-send failures separately.
                    // Never auto-replay a write after a missing response.
                    committing = false;
                    if (error.code() == result_code::unavailable && attempt < settings_.save_retry_attempts
                        && pause(deadline, attempt)) continue;
                    throw;
                }
            }
        } catch (const EntityError& error) { return failure(request, error.code(), error.what()); }
        catch (const std::exception& error) {
            return failure(request, committing ? result_code::commit_unknown : result_code::internal_error, error.what());
        }
    }

private:
    static const char* commit_script() {
        // All parsing/checks precede the ONLY write command. Redis scripts do
        // not roll back prior commands on runtime errors; single HSET publishes
        // every object and its idempotency record together. CAS compares bytes,
        // never numeric versions through Lua doubles. Max 1024 objects.
        return R"lua(
local previous = redis.call('HGET', KEYS[1], ARGV[1])
if previous then return {2, previous} end
local count = tonumber(ARGV[3])
if not count or count < 1 or count > 1024 or #ARGV ~= 3 + count * 4 then
  return redis.error_reply('invalid entity commit arguments')
end
local updates = {ARGV[1], ARGV[2]}
for i = 1, count do
  local offset = 4 + (i - 1) * 4
  local current = redis.call('HGET', KEYS[1], ARGV[offset])
  if ARGV[offset+1] == '0' then
    if current then return {0} end
  elseif not current or current ~= ARGV[offset+2] then return {0} end
  updates[#updates+1] = ARGV[offset]
  updates[#updates+1] = ARGV[offset+3]
end
return redis.call('HSET', KEYS[1], unpack(updates))
)lua";
    }
    static void check_reply(const redisReply& reply) {
        if (reply.type == REDIS_REPLY_ERROR)
            throw EntityError{result_code::unavailable, std::string{reply.str, reply.len}};
    }
    static std::string string_reply(const redisReply& reply) {
        if (reply.type != REDIS_REPLY_STRING) corrupt("unexpected Redis value type");
        return {reply.str, reply.len};
    }
    void check_config() const { if (!settings_.config_error.empty()) invalid(settings_.config_error); }
    void check_budget(clock::time_point deadline) const {
        if (stop_.stop_requested() || clock::now() >= deadline)
            throw EntityError{result_code::unavailable, "Redis operation stopped or deadline expired"};
    }
    bool pause(clock::time_point deadline, uint32_t attempt) const {
        auto delay = std::min(settings_.retry_backoff * (1u << std::min(attempt-1, 5u)), std::chrono::milliseconds{1000});
        if (stop_.stop_requested() || clock::now() + delay >= deadline) return false;
        std::mutex mutex; std::unique_lock lock{mutex}; std::condition_variable_any changed;
        changed.wait_for(lock, stop_, delay, [] { return false; });
        return !stop_.stop_requested() && clock::now() < deadline;
    }
    const schema::store_schema& find_store(const std::string& name, bool write) const {
        auto store = settings_.catalog.find_store(name);
        if (!store) invalid("unknown entity store: " + name);
        if ((write ? store->write_connection : store->read_connection) != name_)
            invalid("connection does not match entity store routing");
        return *store;
    }
    static const schema::entity_schema& find_entity(const schema::store_schema& store, const std::string& name) {
        auto found = store.entities.find(name);
        if (found == store.entities.end()) invalid("unknown entity: " + name);
        return found->second;
    }
    static void validate_patch(const schema::entity_schema& mapped, const entity_patch& patch) {
        if (patch.expected_version > uint64_t(std::numeric_limits<int64_t>::max())) invalid("version exceeds int64");
        for (const auto& field : patch.fields) {
            auto column = mapped.find_writable_field(field.name);
            if (!column) invalid("unknown or read-only field: " + field.name);
            if (field.operation == patch_op::set) EntityCodec::validate_value(field.data, *column);
            else if (field.operation == patch_op::erase) {
                if (!column->nullable) invalid("cannot erase required field: " + field.name);
            } else if (field.operation == patch_op::increment) {
                if (!detail::is_number(column->kind)) invalid("increment requires numeric schema field");
                Decimal delta{EntityCodec::numeric_text(field.data)};
                if (column->kind == value_kind::signed_integer || column->kind == value_kind::unsigned_integer)
                    (void)delta.integer();
            } else invalid("unknown patch operation");
        }
    }
    static void validate_document(const Document& document, const schema::entity_schema& mapped) {
        try {
            for (const auto& key : mapped.keys) {
                auto found = document.fields.find(key.column);
                if (found == document.fields.end()) corrupt("missing stored entity key");
                EntityCodec::validate_value(found->second, key, true);
            }
            for (const auto& column : mapped.fields) {
                auto found = document.fields.find(column.column);
                EntityCodec::validate_value(found == document.fields.end() ? value::null() : found->second, column);
            }
        } catch (const EntityError& error) { corrupt(error.what()); }
    }
    static void apply(Document& document, const schema::entity_schema& mapped, const entity_patch& patch) {
        const bool creating = document.version == 0;
        if (creating) {
            if (!patch.create_if_missing || (patch.check_version && patch.expected_version != 0))
                throw EntityError{result_code::not_found, "entity not found"};
            for (const auto& key : patch.target.key) document.fields.emplace(mapped.find_key(key.name)->column, key.data);
        } else {
            validate_document(document, mapped);
            if (patch.check_version && patch.expected_version != document.version)
                throw EntityError{result_code::conflict, "entity version conflict"};
        }
        if (document.version == uint64_t(std::numeric_limits<int64_t>::max())) invalid("entity version exhausted");
        for (const auto& field : patch.fields) {
            const auto* column = mapped.find_writable_field(field.name);
            if (field.operation == patch_op::erase) document.fields.erase(column->column);
            else if (field.operation == patch_op::set) document.fields[column->column] = field.data;
            else {
                auto found = document.fields.find(column->column);
                auto before = found == document.fields.end() ? EntityCodec::zero(column->kind) : found->second;
                document.fields[column->column] = EntityCodec::increment(before, field.data, column->kind);
            }
        }
        ++document.version;
        try { validate_document(document, mapped); }
        catch (const EntityError& error) { invalid(error.what()); }
    }
    static save_result failure(const save_request& request, result_code code, std::string error) {
        save_result result; result.request_id = request.request_id; result.code = code; result.error = std::move(error);
        return result;
    }
    NativeConnection& connection_;
    std::string name_;
    const service_config& settings_;
    std::stop_token stop_;
};

} // namespace caf_plugin_system::entity_store::redis

