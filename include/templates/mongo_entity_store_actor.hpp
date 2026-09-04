#pragma once

#include "common/message_tags.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "templates/mongo_entity_store_config.hpp"

#include <caf/actor_registry.hpp>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace caf_plugin_system::entity_store::mongo {

// Routes backend-neutral entity requests. The Mongo worker owns the entire
// native transaction, including retries: no session crosses actor messages.
class entity_store_actor final : public caf::event_based_actor {
public:
    entity_store_actor(caf::actor_config& config, service_config settings)
        : caf::event_based_actor(config), settings_(std::move(settings)) {}

    caf::behavior make_behavior() override {
        caf::message_handler business{
            [this](entity_load_atom, load_request value) { return load(std::move(value)); },
            [this](entity_save_atom, save_request value) { return save(std::move(value)); },
            [this](drain_atom, caf::actor coordinator) {
                draining_ = true;
                drain_waiters_.push_back(std::move(coordinator));
                notify_drained();
            },
        };
        PluginLifecycleHooks hooks;
        hooks.on_save = [] { return std::vector<std::byte>{}; };
        hooks.on_shutdown = [this] { draining_ = true; };
        return caf::behavior{business.or_else(plugin_lifecycle(this, hooks))};
    }

    void on_exit() override {
        draining_ = true;
        // Promises own their actor. Break member -> promise -> self even on
        // forced termination, not only after a successful drain handshake.
        for (auto& [id, operation] : loads_) {
            operation->completed = true;
            operation->promise.deliver(failure(operation->input, result_code::unavailable,
                                                "entity store exited before load completed"));
        }
        for (auto& [key, queue] : saves_)
            for (auto& operation : queue) {
                operation->completed = true;
                operation->promise.deliver(failure(operation->input,
                    operation->sent ? result_code::commit_unknown : result_code::unavailable,
                    "entity store exited; retry with the same request_id and payload"));
            }
        loads_.clear();
        saves_.clear();
        drain_waiters_.clear();
        pending_ = 0;
    }

private:
    using Clock = std::chrono::steady_clock;
    using QueueKey = std::pair<std::string, std::string>;
    template<class Request, class Result>
    struct operation {
        Request input;
        caf::typed_response_promise<Result> promise;
        Clock::time_point deadline;
        bool sent = false;
        bool completed = false;
    };
    using Load = operation<load_request, load_result>;
    using Save = operation<save_request, save_result>;

    static load_result failure(const load_request& input, result_code code, std::string error) {
        load_result result;
        result.target = input.target;
        result.code = code;
        result.error = std::move(error);
        return result;
    }
    static save_result failure(const save_request& input, result_code code, std::string error) {
        save_result result;
        result.request_id = input.request_id;
        result.code = code;
        result.error = std::move(error);
        return result;
    }
    std::string admission_error() const {
        if (draining_)
            return "entity store is draining";
        if (pending_ >= settings_.max_pending_requests)
            return "entity store pending request limit reached";
        return {};
    }
    caf::actor backend(const std::string& service) {
        return caf::actor_cast<caf::actor>(system().registry().get(service));
    }
    template<class Operation>
    static std::chrono::milliseconds remaining(const Operation& value) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(value.deadline - Clock::now());
    }
    bool known(const entity_ref& target) const {
        return settings_.catalog.find_entity(target.store, target.entity) != nullptr;
    }

    caf::result<load_result> load(load_request input) {
        auto promise = make_response_promise<load_result>();
        auto error = settings_.config_error;
        if (error.empty()) error = entity_store::validate(input);
        if (error.empty() && !known(input.target)) error = "unknown entity store or entity";
        if (!error.empty()) {
            promise.deliver(failure(input, result_code::invalid_request, std::move(error)));
            return promise;
        }
        if (auto denied = admission_error(); !denied.empty()) {
            promise.deliver(failure(input, result_code::unavailable, std::move(denied)));
            return promise;
        }
        const auto* store = settings_.catalog.find_store(input.target.store);
        auto target = backend(store->read_service);
        if (!target) {
            promise.deliver(failure(input, result_code::unavailable, "MongoDB read service is unavailable"));
            return promise;
        }
        auto current = std::make_shared<Load>();
        current->input = std::move(input);
        current->promise = promise;
        current->deadline = Clock::now() + settings_.request_timeout;
        const auto id = ++sequence_;
        loads_.emplace(id, current);
        ++pending_;
        const auto budget = remaining(*current);
        this->request(target, budget, entity_load_atom_v, store->read_connection,
                      current->input, static_cast<uint64_t>(std::max<int64_t>(budget.count(), 1)))
            .then([this, current, id](load_result result) {
                finish_load(id, current, std::move(result));
            }, [this, current, id](const caf::error& error) {
                finish_load(id, current, failure(current->input, result_code::unavailable,
                                                 caf::to_string(error)));
            });
        return promise;
    }

    void finish_load(uint64_t id, const std::shared_ptr<Load>& current, load_result result) {
        if (current->completed) return;
        current->completed = true;
        current->promise.deliver(std::move(result));
        loads_.erase(id);
        --pending_;
        notify_drained();
    }

    caf::result<save_result> save(save_request input) {
        auto promise = make_response_promise<save_result>();
        auto error = settings_.config_error;
        if (error.empty()) error = entity_store::validate(input);
        if (error.empty() && input.request_id.size() > 255) error = "request_id exceeds 255 bytes";
        if (error.empty())
            for (const auto& change : input.changes)
                if (!known(change.target)) {
                    error = "unknown entity store or entity";
                    break;
                }
        if (!error.empty()) {
            promise.deliver(failure(input, result_code::invalid_request, std::move(error)));
            return promise;
        }
        if (auto denied = admission_error(); !denied.empty()) {
            promise.deliver(failure(input, result_code::unavailable, std::move(denied)));
            return promise;
        }
        const auto& target = input.changes.front().target;
        QueueKey key{target.store, target.partition};
        auto current = std::make_shared<Save>();
        current->input = std::move(input);
        current->promise = promise;
        current->deadline = Clock::now() + settings_.request_timeout;
        auto& queue = saves_[key];
        queue.push_back(current);
        ++pending_;
        if (queue.size() == 1) pump(key);
        return promise;
    }

    void pump(const QueueKey& key) {
        // Iterative rejection avoids recursively unwinding a large queue of
        // expired requests when a backend has been unavailable for a while.
        for (;;) {
            auto found = saves_.find(key);
            if (found == saves_.end()) return;
            auto current = found->second.front();
            const auto budget = remaining(*current);
            const auto* store = settings_.catalog.find_store(key.first);
            auto target = backend(store->write_service);
            if (budget.count() <= 0 || !target) {
                finish_save(key, current, failure(current->input, result_code::unavailable,
                    budget.count() <= 0 ? "save deadline expired while queued"
                                        : "MongoDB write service is unavailable"), false);
                continue;
            }
            current->sent = true;
            this->request(target, budget, entity_save_atom_v, store->write_connection,
                          current->input, static_cast<uint64_t>(budget.count()))
                .then([this, key, current](save_result result) {
                    finish_save(key, current, std::move(result));
                }, [this, key, current](const caf::error& error) {
                    // A lost reply cannot establish whether COMMIT happened.
                    // Do not replay here or change the request's identity.
                    finish_save(key, current, failure(current->input, result_code::commit_unknown,
                        caf::to_string(error) + "; retry with the same request_id and payload"));
                });
            return;
        }
    }

    void finish_save(const QueueKey& key, const std::shared_ptr<Save>& current,
                     save_result result, bool advance = true) {
        if (current->completed) return;
        current->completed = true;
        current->promise.deliver(std::move(result));
        auto found = saves_.find(key);
        if (found != saves_.end()) {
            found->second.pop_front();
            if (found->second.empty()) saves_.erase(found);
        }
        --pending_;
        notify_drained();
        if (advance) pump(key);
    }
    void notify_drained() {
        if (!draining_ || pending_ != 0) return;
        for (const auto& coordinator : drain_waiters_)
            send(coordinator, drain_atom{}, address());
        drain_waiters_.clear();
    }

    service_config settings_;
    size_t pending_ = 0;
    uint64_t sequence_ = 0;
    bool draining_ = false;
    std::map<uint64_t, std::shared_ptr<Load>> loads_;
    std::map<QueueKey, std::deque<std::shared_ptr<Save>>> saves_;
    std::vector<caf::actor> drain_waiters_;
};

} // namespace caf_plugin_system::entity_store::mongo
