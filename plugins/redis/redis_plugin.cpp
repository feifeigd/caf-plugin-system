// ------------------------------------------------------------------
// Redis 插件（数据库插件系列参考实现）
//
// 【阻塞 IO 模型】（数据库插件统一骨架，MySQL/PG/Mongo
// 共用 db_worker_pool.hpp，插件保留驱动 API）：
//   - 插件 actor（event-based）只收消息、入队，返回 response_promise
//   - 每个命名连接一个 worker；原始命令与实体操作各用独立 hiredis 连接。
//     执行完通过 response_promise 交付结果。
//   - worker 不额外持有插件 actor 句柄；任务中的 promise 持有回复所需引用。
//     工作线程必须由公共池 stop/join，不使用 detached 线程
//
// 【多连接】配置：走 CAF 默认配置系统（同一配置文件、字段区分），
//   caf-plugin-system.redis.uris 是字典，键为连接名，值为 URI。
//   uri = redis://host:port/db（db 可省略，默认 0）。
//   例（caf-application.conf）：
//     caf-plugin-system { redis { uris {
//       cache = "redis://127.0.0.1:6379/1"
//       main = "redis://127.0.0.1:6379/0"
//     } } }
//   同一进程可同时连多个实例 / 一个实例多个 db（每命名连接一个 worker）。
//   API：redis_cmd_atom + (conn, cmd, args)；无 conn 的三参版走默认连接
//   （默认 = 配置里第一条；配置缺失时 = 127.0.0.1:6379/db0）。
//   插件在 spawn 时经 sys.config() 读取（基类 get_or 自由函数，避开
//   跨 DLL RTTI 的静态库副本问题）。
//
// 原始命令接口不提供调用方之间的事务隔离：分散发送 MULTI/EXEC 时，
// 其他调用方的命令可能交错。统一对象接口另走 EntityStoreEngine 的原子提交。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "templates/db_worker_pool.hpp"

// hiredis 依赖 winsock2 的 timeval；必须先于 caf/all.hpp（其可能拉入
// windows.h）包含，否则 struct timeval 不完整 → 编译错
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <hiredis/hiredis.h>
#include "redis_entity_store.hpp"

#include <caf/all.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;
namespace entities = caf_plugin_system::entity_store;
namespace redis_entities = caf_plugin_system::entity_store::redis;

namespace {

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.redis.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define REDIS_FIELDS(X, XC)                                                   \
    X(caf::settings, uris, {})
PLUGIN_CONFIG(REDIS_FIELDS)
#undef REDIS_FIELDS

/// 单个命名连接的解析结果。
struct ConnSpec {
    std::string name;
    std::string host = "127.0.0.1";
    int port = 6379;
    int db = 0;
};

/// 从配置 dictionary 构造命名连接表（键 = 连接名，值 = uri 字符串）；
/// uri 尾部 /N 为 db 下标；非字符串条目跳过，数值按 atoi 转换，此处不严格校验 URI。
std::vector<ConnSpec> parse_uris(const caf::settings& uris) {
    std::vector<ConnSpec> out;
    for (const auto& [name, value] : uris) {
        auto uri_v = caf::get_if<std::string>(&value);
        if (!uri_v)
            continue;
        ConnSpec cs;
        cs.name = name;
        std::string uri = *uri_v;
        if (uri.rfind("redis://", 0) == 0)
            uri = uri.substr(8);
        auto slash = uri.find('/');
        std::string hostport = (slash == std::string::npos) ? uri : uri.substr(0, slash);
        auto colon = hostport.find(':');
        if (colon != std::string::npos) {
            cs.host = hostport.substr(0, colon);
            cs.port = std::atoi(hostport.substr(colon + 1).c_str());
        } else if (!hostport.empty()) {
            cs.host = hostport;
        }
        if (slash != std::string::npos)
            cs.db = std::atoi(uri.substr(slash + 1).c_str());
        if (cs.name.empty())
            cs.name = "default";
        out.push_back(std::move(cs));
    }
    if (out.empty()) {
        ConnSpec cs;
        cs.name = "default";
        out.push_back(std::move(cs));
    }
    return out;
}

/// 队列元素：命令 + 响应承诺。rp 是值类型，可跨线程传递；
/// worker 通过 promise 交付结果，不额外保存插件 actor 句柄。
struct Job {
    std::string cmd;
    std::vector<std::string> args;
    caf::typed_response_promise<db::db_result> rp;

    enum class Kind { Raw, Load, Save };
    Kind kind = Kind::Raw;
    entities::load_request load;
    entities::save_request save;
    caf::typed_response_promise<entities::load_result> load_rp;
    caf::typed_response_promise<entities::save_result> save_rp;
    std::chrono::steady_clock::time_point deadline;

    /// 仅尚未执行的任务由队列拒绝，不会宣称已执行的写请求失败。
    void fail(const std::string& err) {
        if (kind == Kind::Load) {
            entities::load_result result; result.target = load.target;
            result.code = entities::result_code::unavailable; result.error = err;
            load_rp.deliver(std::move(result));
        } else if (kind == Kind::Save) {
            entities::save_result result; result.request_id = save.request_id;
            result.code = entities::result_code::unavailable; result.error = err;
            save_rp.deliver(std::move(result));
        } else rp.deliver(caf::make_error(caf::sec::runtime_error, err));
    }
};

using WorkerState = caf_plugin_system::db_backend::WorkerState<Job>;
using ConnSlot    = caf_plugin_system::db_backend::WorkerSlot<Job>;
using WorkerPool  = caf_plugin_system::db_backend::WorkerPool<ConnSlot>;

/// 嵌套数组递归展平（逗号分隔，v1 简化表示）。
void flatten_reply(const redisReply* r, std::string& out) {
    if (r->type == REDIS_REPLY_ARRAY) {
        for (size_t i = 0; i < r->elements; ++i) {
            if (i > 0) out += ", ";
            flatten_reply(r->element[i], out);
        }
    } else if (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_STATUS) {
        out.append(r->str, r->len);
    } else if (r->type == REDIS_REPLY_INTEGER) {
        out += std::to_string(r->integer);
    }
}

db::db_result reply_to_result(const redisReply* r) {
    db::db_result res;
    switch (r->type) {
        case REDIS_REPLY_STRING:
        case REDIS_REPLY_STATUS:
            res.ok = true;
            res.rows.push_back({std::string(r->str, r->len)});
            break;
        case REDIS_REPLY_INTEGER:
            res.ok = true;
            res.rows.push_back({std::to_string(r->integer)});
            break;
        case REDIS_REPLY_ERROR:
            res.ok = false;
            res.error = std::string(r->str, r->len);
            break;
        case REDIS_REPLY_NIL:
            res.ok = true;
            break;
        case REDIS_REPLY_ARRAY: {
            res.ok = true;
            for (size_t i = 0; i < r->elements; ++i) {
                const redisReply* el = r->element[i];
                if (el->type == REDIS_REPLY_STRING || el->type == REDIS_REPLY_STATUS)
                    res.rows.push_back({std::string(el->str, el->len)});
                else if (el->type == REDIS_REPLY_INTEGER)
                    res.rows.push_back({std::to_string(el->integer)});
                else if (el->type == REDIS_REPLY_NIL)
                    res.rows.push_back({});
                else if (el->type == REDIS_REPLY_ARRAY) {
                    std::string flat;
                    flatten_reply(el, flat);
                    res.rows.push_back({std::move(flat)});
                } else {
                    res.rows.push_back({"(unknown)"});
                }
            }
            break;
        }
        default:
            res.ok = false;
            res.error = "unhandled redis reply type";
    }
    return res;
}

/// 每条命名连接一个串行 worker；原始命令和实体事务使用独立 native session。
void redis_worker_main(std::shared_ptr<WorkerState> state, const ConnSpec& spec,
                       std::shared_ptr<const redis_entities::service_config> settings) {
    redis_entities::ConnectionOptions options{spec.host, spec.port, spec.db};
    redis_entities::NativeConnection raw{options}, entity_connection{options};
    redis_entities::EntityStoreEngine engine{entity_connection, spec.name, *settings, state->stop_token()};
    while (auto job = state->next_job()) {
        if (job->kind == Job::Kind::Load) {
            job->load_rp.deliver(engine.load(job->load, job->deadline));
            continue;
        }
        if (job->kind == Job::Kind::Save) {
            job->save_rp.deliver(engine.save(job->save, job->deadline));
            continue;
        }
        auto started = std::chrono::steady_clock::now();
        db::db_result result;
        try {
            std::vector<std::string> arguments{job->cmd};
            arguments.insert(arguments.end(), job->args.begin(), job->args.end());
            auto reply = raw.command(arguments, started + std::chrono::seconds{2}, true);
            result = reply_to_result(reply.get());
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        result.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        job->rp.deliver(std::move(result));
    }
    LOG_INFO("Redis [{}] worker exited", spec.name);
}

} // namespace

class RedisPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        // 连接配置读取 caf-plugin-system.redis.uris，不依赖 config_service
        return {"RedisPlugin", "1.0.0",
                 {},
                {"redis_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // 声明式配置读取（PLUGIN_CONFIG）：字段、默认值、解析全部自动
        auto cfg = load_plugin_config(sys.config());
        caf::settings uris = cfg.uris;

        auto raw_settings = caf::get_or(sys.config(), "caf-plugin-system.entity_store", caf::settings{});
        auto entity_settings = std::make_shared<const redis_entities::service_config>(
            redis_entities::parse_service_config(raw_settings));
        return sys.spawn([logger, uris, entity_settings](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<ConnSpec>>(parse_uris(uris));
            auto pools = std::make_shared<WorkerPool>();
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto default_conn = std::make_shared<std::string>("default");

            // 只在首次调用时启动全部 worker（on_init 里调用）
            auto launch_workers = [=] {
                if (started->exchange(true))
                    return;
                // 默认连接 = 配置第一条（三参版 redis_cmd_atom 的落点）
                if (!specs->empty())
                    *default_conn = specs->front().name;
                for (const auto& s : *specs) {
                    auto slot = pools->add_slot(s.name);
                    slot->start_worker(redis_worker_main, s, entity_settings);
                    LOG_INFO_SELF(self, "worker launched: [{}] {}:{}/db{}",
                                  s.name, s.host, s.port, s.db);
                }
            };

            // 入队请求；conn 不存在 → 立即回错误（不排队）
            auto enqueue = [=](const std::string& conn, const std::string& cmd,
                               const std::vector<std::string>& args) {
                auto slot = pools->route_round_robin(conn);
                if (!slot) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown or stopped redis connection: " + conn;
                    self->make_response_promise<db::db_result>().deliver(std::move(r));
                    return;
                }
                auto rp = self->make_response_promise<db::db_result>();
                slot->enqueue(std::make_shared<Job>(Job{cmd, args, rp}));
            };

            // 自检：PING + SET 各一轮（默认连接），
            // 全链路（actor→队列→worker→deliver→回调）验证
            auto selfcheck = [=] {
                self->request(caf::actor{self}, std::chrono::seconds(5), redis_cmd_atom_v,
                              std::string("PING"), std::vector<std::string>{})
                    .then(
                        [=](const db::db_result& r) {
                            LOG_INFO_SELF(self, "selfcheck PING -> ok={} err={} rows={}",
                                          r.ok, r.error, r.rows.size());
                        },
                        [=](caf::error& e) {
                            LOG_ERROR_SELF(self, "selfcheck PING failed: {}", caf::to_string(e));
                        });
                self->request(caf::actor{self}, std::chrono::seconds(5), redis_cmd_atom_v,
                              std::string("SET"),
                              std::vector<std::string>{"hermes:db:ping", "1"})
                    .then(
                        [=](const db::db_result& r) {
                            LOG_INFO_SELF(self, "selfcheck SET -> ok={} err={}", r.ok, r.error);
                        },
                        [=](caf::error& e) {
                            LOG_ERROR_SELF(self, "selfcheck SET failed: {}", caf::to_string(e));
                        });
            };

            caf::message_handler business{
                [=](entity_load_atom, const std::string& conn, entities::load_request request,
                    uint64_t timeout_ms) -> caf::result<entities::load_result> {
                    auto promise = self->make_response_promise<entities::load_result>();
                    auto slot = pools->route_round_robin(conn);
                    if (!slot || slot->pending() >= entity_settings->max_pending_requests || timeout_ms == 0) {
                        entities::load_result result; result.target = request.target;
                        result.code = entities::result_code::unavailable;
                        result.error = "Redis entity connection unavailable, queue full or deadline expired";
                        promise.deliver(std::move(result)); return promise;
                    }
                    auto job = std::make_shared<Job>();
                    job->kind = Job::Kind::Load; job->load = std::move(request); job->load_rp = promise;
                    job->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{
                        std::min<uint64_t>(timeout_ms, entity_settings->request_timeout.count())};
                    slot->enqueue(std::move(job)); return promise;
                },
                [=](entity_save_atom, const std::string& conn, entities::save_request request,
                    uint64_t timeout_ms) -> caf::result<entities::save_result> {
                    auto promise = self->make_response_promise<entities::save_result>();
                    auto slot = pools->route_round_robin(conn);
                    if (!slot || slot->pending() >= entity_settings->max_pending_requests || timeout_ms == 0) {
                        entities::save_result result; result.request_id = request.request_id;
                        result.code = entities::result_code::unavailable;
                        result.error = "Redis entity connection unavailable, queue full or deadline expired";
                        promise.deliver(std::move(result)); return promise;
                    }
                    auto job = std::make_shared<Job>();
                    job->kind = Job::Kind::Save; job->save = std::move(request); job->save_rp = promise;
                    job->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{
                        std::min<uint64_t>(timeout_ms, entity_settings->request_timeout.count())};
                    slot->enqueue(std::move(job)); return promise;
                },
                // 四参版：指定命名连接。request → db_result；纯 send 无 rp，丢弃
                [=](redis_cmd_atom, const std::string& conn, const std::string& cmd,
                    const std::vector<std::string>& args) {
                    if (!self->current_message_id().is_request()) {
                        LOG_INFO_SELF(self, "redis_cmd send (no reply) ignored: {} @{}",
                                      cmd, conn);
                        return;
                    }
                    enqueue(conn, cmd, args);
                },
                // 三参版：默认连接（配置第一条）
                [=](redis_cmd_atom, const std::string& cmd,
                    const std::vector<std::string>& args) {
                    if (!self->current_message_id().is_request()) {
                        LOG_INFO_SELF(self, "redis_cmd send (no reply) ignored: {}", cmd);
                        return;
                    }
                    enqueue(*default_conn, cmd, args);
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "RedisPlugin initialized, conns={}", uris.size());
                    launch_workers();
                    selfcheck();
                },
                // 无业务状态：连接配置归 CAF 配置文件管，checkpoint 不写
                //（返回空 → plugin_manager 不调 restore_state_atom）
                .on_save = []() -> std::vector<std::byte> {
                    return {};
                },
                .on_shutdown = [=]() {
                    const auto joined = pools->stop_and_join();
                    LOG_INFO_SELF(self, "RedisPlugin shutdown, {} workers joined", joined);
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new RedisPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
