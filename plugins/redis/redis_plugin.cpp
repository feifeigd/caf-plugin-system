// ------------------------------------------------------------------
// Redis 插件（数据库插件系列参考实现）
//
// 【阻塞 IO 模型】（数据库插件统一骨架，Phase 2/3 的 MySQL/PG/Mongo
// 照抄本文件结构，只换驱动 API）：
//   - 插件 actor（event-based）只收消息、入队，返回 response_promise
//   - 每个【命名连接】一个 worker 线程 + 独占 hiredis ctx（连接间
//     天然隔离），执行完 rp.deliver() 直接回调用方
//   - worker 线程【绝不持有 caf::actor 强引用】，只捕获 rp 值
//     （detached 线程被进程强杀不 unwind → 强引用导致 dump 泄露）
//
// 【多连接】配置：走 CAF 默认配置系统（同一配置文件、字段区分），
//   framework_config 注册的字段 "redis-uris"（caf-plugin-system 组）：
//   格式 "name1=uri1,name2=uri2"（逗号分隔、等号配对；无 name 条目归入
//   "default"）。uri = redis://host:port/db（db 可省略，默认 0）。
//   例（caf-application.conf）：
//     caf-plugin-system { redis-uris = "cache=redis://127.0.0.1:6379/1,main=redis://127.0.0.1:6379/0" }
//   同一进程可同时连多个实例 / 一个实例多个 db（每命名连接一个 worker）。
//   API：redis_cmd_atom + (conn, cmd, args)；无 conn 的三参版走默认连接
//   （默认 = 配置里第一条；配置缺失时 = 127.0.0.1:6379/db0）。
//   插件在 spawn 时经 sys.config() 读取（基类 get_or 自由函数，避开
//   跨 DLL RTTI 的静态库副本问题）。
//
// 事务：Redis 的 MULTI/EXEC 就是命令流，execute("MULTI")...execute("EXEC")
// 天然支持（同连接串行 = 同事务），无需显式事务状态机（tx_* 留给 MySQL/PG）。
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "templates/job_queue.hpp"

// hiredis 依赖 winsock2 的 timeval；必须先于 caf/all.hpp（其可能拉入
// windows.h）包含，否则 struct timeval 不完整 → 编译错
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <hiredis/hiredis.h>

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
/// uri 尾部 /N 为 db 下标。解析失败条目跳过（记日志由调用方做）。
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
/// worker 只调 deliver，绝不持有 actor 句柄。
struct Job {
    std::string cmd;
    std::vector<std::string> args;
    caf::typed_response_promise<db::db_result> rp;

    /// 失败交付方式（JobQueue::fail_all 统一调用）。
    void fail(const std::string& err) {
        rp.deliver(caf::make_error(caf::sec::runtime_error, err));
    }
};

using JobQueue = caf_plugin_system::JobQueue<Job>;

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

/// worker 主循环：独占一条连接，串行执行队列。
void redis_worker_main(const ConnSpec& spec, std::shared_ptr<JobQueue> queue) {
    struct timeval tv { 2, 0 };  // 连接/命令 2s 超时，防关机链 join 卡死
    redisContext* ctx = redisConnectWithTimeout(spec.host.c_str(), spec.port, tv);
    if (ctx && ctx->err) {
        redisFree(ctx);
        ctx = nullptr;
    }
    if (!ctx) {
        LOG_ERROR("Redis [{}] connect failed: {}:{}", spec.name, spec.host, spec.port);
        queue->fail_all("redis connect failed: " + spec.host + ":" + std::to_string(spec.port));
        return;
    }
    redisSetTimeout(ctx, tv);
    if (spec.db > 0) {
        // hiredis 1.x 的 redisCommand 返回 void*（宏展开为 redisvCommand）
        redisReply* r = static_cast<redisReply*>(redisCommand(ctx, "SELECT %d", spec.db));
        if (r && r->type == REDIS_REPLY_ERROR) {
            LOG_ERROR("Redis [{}] SELECT {} failed: {}", spec.name, spec.db, r->str);
            freeReplyObject(r);
            redisFree(ctx);
            queue->fail_all("redis SELECT failed");
            return;
        }
        if (r)
            freeReplyObject(r);
    }
    LOG_INFO("Redis [{}] connected: {}:{}/db{}", spec.name, spec.host, spec.port, spec.db);

    for (;;) {
        auto job = queue->pop();
        if (!job)
            break;
        auto t0 = std::chrono::steady_clock::now();

        std::vector<const char*> argv;
        std::vector<size_t> argvlen;
        argv.reserve(job->args.size() + 1);
        argvlen.reserve(job->args.size() + 1);
        argv.push_back(job->cmd.c_str());
        argvlen.push_back(job->cmd.size());
        for (const auto& a : job->args) {
            argv.push_back(a.c_str());
            argvlen.push_back(a.size());
        }

        redisReply* reply = static_cast<redisReply*>(redisCommandArgv(
            ctx, static_cast<int>(argv.size()), argv.data(), argvlen.data()));
        db::db_result r;
        if (!reply) {
            r.ok = false;
            r.error = ctx->err ? ctx->errstr : "unknown redis error";
        } else {
            r = reply_to_result(reply);
            freeReplyObject(reply);
        }
        r.duration_ms = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count());
        job->rp.deliver(std::move(r));
    }

    redisFree(ctx);
    LOG_INFO("Redis [{}] worker exited", spec.name);
}

} // namespace

class RedisPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        // 连接配置走 CAF 配置系统（redis-uris 字段），不依赖 config_service
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

        return sys.spawn([logger, uris](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<ConnSpec>>(parse_uris(uris));
            auto queues = std::make_shared<std::map<std::string, std::shared_ptr<JobQueue>>>();
            auto threads = std::make_shared<std::map<std::string, std::shared_ptr<std::thread>>>();
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
                    auto q = std::make_shared<JobQueue>();
                    (*queues)[s.name] = q;
                    (*threads)[s.name] = std::make_shared<std::thread>(
                        redis_worker_main, s, q);
                    LOG_INFO_SELF(self, "worker launched: [{}] {}:{}/db{}",
                                  s.name, s.host, s.port, s.db);
                }
            };

            // 入队请求；conn 不存在 → 立即回错误（不排队）
            auto enqueue = [=](const std::string& conn, const std::string& cmd,
                               const std::vector<std::string>& args) {
                auto it = queues->find(conn);
                if (it == queues->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown redis connection: " + conn;
                    self->make_response_promise<db::db_result>().deliver(std::move(r));
                    return;
                }
                auto rp = self->make_response_promise<db::db_result>();
                it->second->push(std::make_shared<Job>(Job{cmd, args, rp}));
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
                    // 先停全部队列再 join：worker 清空残留（deliver error）后快速退出；
                    // hiredis 命令 2s 超时兜底，join 不会无限卡关机链。
                    // 注意：此日志用 std::cout 直出（不经 logging_service）——
                    // 关机尾部 shutdown_mgr 的 send_exit(logging_service) 与本
                    // actor 的日志消息不同 sender 无 FIFO，异步日志可能被丢弃。
                    for (auto& [name, q] : *queues)
                        q->stop();
                    for (auto& [name, t] : *threads) {
                        if (t->joinable())
                            t->join();
                    }
                    std::cout << "[RedisPlugin] shutdown hook: " << threads->size()
                              << " workers joined" << std::endl;
                    LOG_INFO_SELF(self, "RedisPlugin shutdown, {} workers joined",
                                  threads->size());
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
