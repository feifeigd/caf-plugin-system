// ------------------------------------------------------------------
// MongoDB 插件（mongo-cxx-driver，Phase 3）
//
// 与 Redis/MySQL/PostgreSQL 同骨架（阻塞 IO 模型）：
//   event-based actor 收消息入队 → 每命名连接一个 worker 线程
//   （独占 mongocxx::client）→ rp.deliver() 回调用方。
// 差异仅在驱动适配：
//   - 操作：JSON 参数化（BSON 天然 JSON 化，无注入面）
//   - 无事务状态机：多文档事务 v1 不支持（调用方自行承担）
//   - 结果：find/aggregate → 每文档一行 JSON；写操作 → affected/insert_id
//
// 消息（mongo_op_atom，db_contract 契约）：
//   (conn, collection, op, json) → db_result   四参版显式连接
//   (collection, op, json)       → db_result   三参版走默认连接
//   op ∈ find / find_one / insert_one / insert_many / update_one /
//        update_many / delete_one / delete_many / count / aggregate /
//        drop / create_index / distinct
//
// json 参数（统一一个 JSON 文档，缺省字段按空处理）：
//   find:         {"filter": {}, "limit": N, "sort": {}}
//   find_one:     {"filter": {}}              （等价 find limit=1）
//   insert_one:   {"doc": {...}}
//   insert_many:  {"docs": [{...}, ...]}
//   update_*:     {"filter": {}, "update": {"$set": {...}}}
//   delete_*:     {"filter": {}}
//   count:        {"filter": {}}
//   aggregate:    {"$match": {}, "$group": {}, ...}（字段 = 阶段，v1
//                 每阶段各一次；多阶段同 key 需要 C API，留 v2）
//   drop:         {}
//   create_index: {"keys": {"k": 1}, "name": ""}
//   distinct:     {"field": "k", "filter": {}}
//
// 配置（CAF 配置系统，同文件字段区分）：
//   caf-plugin-system {
//     mongo-uris = "main=mongodb://127.0.0.1:27017/appdb,cache=mongodb://127.0.0.1:27017/cachedb"
//     db-pool-size = 2
//   }
// uri = mongodb://[user:pass@]host[:port][/dbname][?opts]（各段可省略）
// ------------------------------------------------------------------

#include "plugin/plugin_interface.hpp"
#include "plugin/plugin_lifecycle.hpp"
#include "services/logging_service.hpp"
#include "common/db_contract.hpp"
#include "common/plugin_config.hpp"
#include "templates/job_queue.hpp"

// mongocxx 依赖 winsock2，必须最先包含（同 hiredis/libpq 坑）
#ifdef _WIN32
#include <winsock2.h>
#endif
#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/pipeline.hpp>
#include <mongocxx/uri.hpp>
#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/json.hpp>

#include <caf/all.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace db = caf_plugin_system::db;
namespace bbs = bsoncxx::builder::basic;

namespace {

// 声明式配置（PLUGIN_CONFIG 宏，见 common/plugin_config.hpp）：
// 字段 + 默认值，读取路径 caf-plugin-system.mongo.<字段名>（conf 嵌套块）。
// X = 只读 conf；XC = conf + CLI 双通道（Phase 4 接线后生效）。
#define MONGO_FIELDS(X, XC)                                                   \
    X(caf::settings, uris, {})                                                \
    X(int, pool_size, 2)
PLUGIN_CONFIG(MONGO_FIELDS)
#undef MONGO_FIELDS

/// 单个命名连接的解析结果（mongodb://[user:***@]host[:port][/dbname]）。
struct MongoSpec {
    std::string name;
    std::string uri = "mongodb://127.0.0.1:27017";
    std::string dbname;
};

/// 从配置 dictionary 构造命名连接表（键 = 连接名，值 = uri 字符串）。
std::vector<MongoSpec> parse_uris(const caf::settings& uris) {
    std::vector<MongoSpec> out;
    for (const auto& [name, value] : uris) {
        auto uri = caf::get_if<std::string>(&value);
        if (!uri)
            continue;
        MongoSpec cs;
        cs.name = name;
        cs.uri = uri->empty() ? "mongodb://127.0.0.1:27017" : *uri;
        // 提取 dbname：最后一个 '/' 后、'?' 前的段（uri 查询参数不属库名）
        auto q = cs.uri.find('?');
        std::string path = cs.uri.substr(0, q == std::string::npos ? cs.uri.size() : q);
        auto slash = path.rfind('/');
        if (slash != std::string::npos && slash + 1 < path.size())
            cs.dbname = path.substr(slash + 1);
        out.push_back(std::move(cs));
    }
    if (out.empty()) {
        MongoSpec cs;
        cs.name = "default";
        out.push_back(std::move(cs));
    }
    return out;
}

/// Mongo 操作类型（字符串 op 映射）。
enum class Op {
    Find, FindOne, InsertOne, InsertMany, UpdateOne, UpdateMany,
    DeleteOne, DeleteMany, Count, Aggregate, Drop, CreateIndex, Distinct
};

struct Job {
    Op op = Op::Find;
    std::string collection;
    std::string json;
    std::function<void(db::db_result&)> done;  // worker 执行完回调（deliver）

    /// 失败交付方式（JobQueue::fail_all 统一调用）。
    void fail(const std::string& err) {
        if (done) {
            db::db_result r;
            r.ok = false;
            r.error = err;
            done(r);
        }
    }
};

using JobQueue = caf_plugin_system::JobQueue<Job>;

/// 连接槽：一个 mongocxx::client + 专属队列 + worker 线程。
struct ConnSlot {
    std::shared_ptr<JobQueue> queue = std::make_shared<JobQueue>();
    std::shared_ptr<std::thread> thread;
};

/// 取 json 中的 document 字段（缺省返回 fallback 文档）。
bsoncxx::document::value get_doc(bsoncxx::document::view v, const char* key) {
    auto it = v[key];
    if (it && it.type() == bsoncxx::type::k_document)
        return bsoncxx::document::value{it.get_document().value};
    return bsoncxx::document::value{bsoncxx::from_json("{}")};
}

/// 执行一次 Mongo 操作 → db_result（异常全捕获，驱动错误进 error）。
db::db_result mongo_execute(mongocxx::client& client, const std::string& dbname,
                            const std::string& collection, Op op,
                            const std::string& json) {
    db::db_result r;
    try {
        auto db = client[dbname];
        auto coll = db[collection];
        bsoncxx::document::value params = bsoncxx::from_json(json.empty() ? "{}" : json);
        auto v = params.view();

        switch (op) {
            case Op::Find:
            case Op::FindOne: {
                mongocxx::options::find fo;
                if (op == Op::FindOne)
                    fo.limit(1);
                else if (v["limit"] && v["limit"].type() == bsoncxx::type::k_int32)
                    fo.limit(v["limit"].get_int32().value);
                auto sort_el = v["sort"];
                if (sort_el && sort_el.type() == bsoncxx::type::k_document)
                    fo.sort(bsoncxx::document::value{sort_el.get_document().value});
                auto cursor = coll.find(get_doc(v, "filter").view(), fo);
                r.columns = {"doc"};
                for (auto&& doc : cursor)
                    r.rows.push_back({bsoncxx::to_json(doc)});
                r.ok = true;
                break;
            }
            case Op::InsertOne: {
                auto res = coll.insert_one(get_doc(v, "doc").view());
                if (res) {
                    // 4.0: inserted_id() 返回 bson_value::view（非 optional）——type 分发转字符串
                    auto& idv = res->inserted_id();
                    switch (idv.type()) {
                        case bsoncxx::type::k_oid:
                            r.insert_id = idv.get_oid().value.to_string();
                            break;
                        case bsoncxx::type::k_string:
                            r.insert_id = std::string(idv.get_string().value);
                            break;
                        case bsoncxx::type::k_int32:
                            r.insert_id = std::to_string(idv.get_int32().value);
                            break;
                        case bsoncxx::type::k_int64:
                            r.insert_id = std::to_string(idv.get_int64().value);
                            break;
                        default:
                            r.insert_id = "(unsupported id type)";
                    }
                    r.affected = 1;
                }
                r.ok = true;
                break;
            }
            case Op::InsertMany: {
                auto docs = v["docs"];
                if (!docs || docs.type() != bsoncxx::type::k_array) {
                    r.error = "insert_many requires docs array";
                    break;
                }
                int64_t n = 0;
                for (auto&& el : docs.get_array().value) {
                    if (el.type() == bsoncxx::type::k_document) {
                        coll.insert_one(el.get_document().value);
                        ++n;
                    }
                }
                r.affected = n;
                r.ok = true;
                break;
            }
            case Op::UpdateOne:
            case Op::UpdateMany: {
                // ⚠️ 必须把 document::value 留在作用域内：直接 .view() 绑定到
                // auto 会让临时 value 在语句末尾析构 → 悬垂 view → 驱动从已释放
                // 内存（Debug 0xDD）构造 bson_t → 后续 bson_free 堆断言崩溃
                // （is_block_type_valid）。move 进 view_or_value 由驱动持有。
                auto filter = get_doc(v, "filter");
                auto update = get_doc(v, "update");
                auto res = (op == Op::UpdateOne)
                               ? coll.update_one(std::move(filter), std::move(update))
                               : coll.update_many(std::move(filter), std::move(update));
                if (res) {
                    r.affected = static_cast<int64_t>(res->modified_count());
                    r.ok = true;
                }
                break;
            }
            case Op::DeleteOne:
            case Op::DeleteMany: {
                // 同 Update：悬垂 view 会让 bulk delete 模型读到 0xDD 垃圾，
                // bson_destroy/bson_free 在无效块头上断言崩溃（delete_many 卡死
                // 与 is_block_type_valid 崩溃同根）。
                auto filter = get_doc(v, "filter");
                auto res = (op == Op::DeleteOne)
                               ? coll.delete_one(std::move(filter))
                               : coll.delete_many(std::move(filter));
                if (res) {
                    r.affected = static_cast<int64_t>(res->deleted_count());
                    r.ok = true;
                }
                break;
            }
            case Op::Count: {
                r.affected = static_cast<int64_t>(
                    coll.count_documents(get_doc(v, "filter").view()));
                r.ok = true;
                break;
            }
            case Op::Aggregate: {
                // v1：json 的字段即管道阶段（每阶段各一次），用 mongocxx::pipeline
                // 逐阶段 append_stage（新版 API 不接受裸 document）
                mongocxx::pipeline pipeline;
                for (auto&& el : v) {
                    if (el.type() == bsoncxx::type::k_document) {
                        bbs::document stage;
                        stage.append(bbs::kvp(el.key(), el.get_document().value));
                        pipeline.append_stage(stage.extract());
                    }
                }
                auto cursor = coll.aggregate(pipeline);
                r.columns = {"doc"};
                for (auto&& doc : cursor)
                    r.rows.push_back({bsoncxx::to_json(doc)});
                r.ok = true;
                break;
            }
            case Op::Drop: {
                coll.drop();
                r.ok = true;
                break;
            }
            case Op::CreateIndex: {
                mongocxx::options::index iopts;
                auto name = v["name"];
                if (name && name.type() == bsoncxx::type::k_string)
                    iopts.name(name.get_string().value.data());
                coll.create_index(get_doc(v, "keys").view(), iopts);
                r.affected = 1;
                r.ok = true;
                break;
            }
            case Op::Distinct: {
                auto field = v["field"];
                if (!field || field.type() != bsoncxx::type::k_string) {
                    r.error = "distinct requires field string";
                    break;
                }
                auto values = coll.distinct(field.get_string().value.data(),
                                            get_doc(v, "filter").view());
                r.columns = {"value"};
                for (auto& val : values) {
                    bbs::document d;
                    d.append(bbs::kvp("value", val));
                    r.rows.push_back({bsoncxx::to_json(d.extract())});
                }
                r.ok = true;
                break;
            }
        }
    } catch (const mongocxx::exception& e) {
        r.ok = false;
        r.error = e.what();
    } catch (const bsoncxx::exception& e) {
        r.ok = false;
        r.error = e.what();
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    }
    return r;
}

/// worker 主循环：独占一个 mongocxx::client，串行执行队列。
void mongo_worker_main(const MongoSpec& spec, std::shared_ptr<ConnSlot> slot) {
    try {
        // instance 已由 spawn() 的 instance::current() 创建（进程级单例），
        // worker 只建 client，不再创建 instance（重复创建会抛异常）
        // 4.0 的 options::client 无 server_selection_timeout_ms——用 uri 查询参数
        // 控制连接超时（默认 30s 太久，连不上会卡死 worker 启动）
        std::string uri = spec.uri;
        if (uri.find('?') == std::string::npos)
            uri += "?serverSelectionTimeoutMS=3000";
        else
            uri += "&serverSelectionTimeoutMS=3000";
        mongocxx::client client{mongocxx::uri{uri}};
        LOG_INFO("MONGO-DBG client constructed");
        // 连接验证（client 构造不连接，首次操作才连；失败抛异常 → fail_all）
        auto db = client[spec.dbname.empty() ? "admin" : spec.dbname];
        LOG_INFO("MONGO-DBG db acquired");
        db.run_command(bsoncxx::from_json("{\"ping\":1}").view());
        LOG_INFO("MONGO-DBG ping ok");
        LOG_INFO("Mongo [{}] connected: {}", spec.name, spec.uri);

        for (;;) {
            auto job = slot->queue->pop();
            if (!job)
                break;
            auto t0 = std::chrono::steady_clock::now();
            db::db_result r = mongo_execute(client, spec.dbname, job->collection,
                                            job->op, job->json);
            r.duration_ms = static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            if (job->done)
                job->done(r);
        }
    } catch (const mongocxx::exception& e) {
        LOG_ERROR("Mongo [{}] connect failed: {}", spec.name, e.what());
        slot->queue->fail_all("mongo connect failed: " + std::string(e.what()));
        return;
    }
    LOG_INFO("Mongo [{}] worker exited", spec.name);
}

/// 字符串 op → 枚举；未知返回 false。
bool parse_op(const std::string& s, Op& out) {
    if (s == "find") out = Op::Find;
    else if (s == "find_one") out = Op::FindOne;
    else if (s == "insert_one") out = Op::InsertOne;
    else if (s == "insert_many") out = Op::InsertMany;
    else if (s == "update_one") out = Op::UpdateOne;
    else if (s == "update_many") out = Op::UpdateMany;
    else if (s == "delete_one") out = Op::DeleteOne;
    else if (s == "delete_many") out = Op::DeleteMany;
    else if (s == "count") out = Op::Count;
    else if (s == "aggregate") out = Op::Aggregate;
    else if (s == "drop") out = Op::Drop;
    else if (s == "create_index") out = Op::CreateIndex;
    else if (s == "distinct") out = Op::Distinct;
    else return false;
    return true;
}

} // namespace

class MongoPlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        return {"MongoPlugin", "1.0.0",
                 {},
                {"mongo_service"}, 0, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>& deps,
                     const std::string&) override {
        caf::actor logger = caf_plugin_system::current_logger();
        caf_plugin_system::set_log_source(PLUGIN_NAME);

        // mongocxx 驱动单例：instance.hpp 要求 "exactly one instance must be
        // created"——current() 在无显式实例时隐式构造（首次调用线程安全）
        mongocxx::instance::current();

        // 声明式配置读取（PLUGIN_CONFIG）：字段、默认值、解析全部自动
        auto cfg = load_plugin_config(sys.config());
        caf::settings uris = cfg.uris;
        int pool_size = cfg.pool_size < 1 ? 1 : cfg.pool_size;

        return sys.spawn([logger, uris, pool_size](caf::event_based_actor* self) -> caf::behavior {
            auto specs = std::make_shared<std::vector<MongoSpec>>(parse_uris(uris));
            auto pools = std::make_shared<std::map<std::string, std::vector<std::shared_ptr<ConnSlot>>>>();
            auto rr = std::make_shared<std::atomic<size_t>>(0);
            auto started = std::make_shared<std::atomic<bool>>(false);
            auto default_conn = std::make_shared<std::string>("default");

            auto launch_workers = [=] {
                if (started->exchange(true))
                    return;
                if (!specs->empty())
                    *default_conn = specs->front().name;
                for (const auto& s : *specs) {
                    auto& slots = (*pools)[s.name];
                    slots.reserve(pool_size);
                    for (int i = 0; i < pool_size; ++i) {
                        auto slot = std::make_shared<ConnSlot>();
                        slot->thread = std::make_shared<std::thread>(mongo_worker_main, s, slot);
                        slots.push_back(slot);
                    }
                    LOG_INFO_SELF(self, "pool launched: [{}] {} size={}",
                                  s.name, s.uri, pool_size);
                }
            };

            // 入队到指定命名连接的池（round-robin）
            auto enqueue_rr = [=](const std::string& name, std::shared_ptr<Job> job) {
                auto it = pools->find(name);
                if (it == pools->end()) {
                    db::db_result r;
                    r.ok = false;
                    r.error = "unknown mongo connection: " + name;
                    if (job->done)
                        job->done(r);
                    return;
                }
                auto& slots = it->second;
                size_t idx = (rr->fetch_add(1)) % slots.size();
                slots[idx]->queue->push(std::move(job));
            };

            auto make_job = [](Op op, const std::string& coll, const std::string& json,
                               auto rp) {
                auto job = std::make_shared<Job>();
                job->op = op;
                job->collection = coll;
                job->json = json;
                job->done = [rp](db::db_result& r) mutable { rp.deliver(std::move(r)); };
                return job;
            };

            // 自检：默认连接上 insert → find → delete（串行链，顺序确定）
            auto selfcheck = [=] {
                auto sc = [self](const std::string& coll, const std::string& op,
                                 const std::string& json, std::function<void()> next) {
                    self->request(caf::actor{self}, std::chrono::seconds(5),
                                  mongo_op_atom_v, coll, op, json)
                        .then(
                            [self, coll, op, next](const db::db_result& r) {
                                LOG_INFO_SELF(self, "selfcheck {} {} -> ok={} err={} rows={}",
                                              coll, op, r.ok, r.error, r.rows.size());
                                if (next)
                                    next();
                            },
                            [self, coll, op, next](caf::error& e) {
                                LOG_ERROR_SELF(self, "selfcheck {} {} failed: {}", coll, op,
                                               caf::to_string(e));
                                if (next)
                                    next();
                            });
                };
                sc("hermes_selfcheck", "insert_one",
                   "{\"doc\": {\"k\": \"db-ping\", \"ts\": 1}}", [=] {
                    sc("hermes_selfcheck", "delete_many",
                       "{\"filter\": {\"k\": \"db-ping\"}}", [=] {
                        sc("hermes_selfcheck", "find",
                           "{\"filter\": {\"k\": \"db-ping\"}, \"limit\": 1}", nullptr);
                    });
                });
            };

            caf::message_handler business{
                [=](mongo_op_atom, const std::string& conn, const std::string& coll,
                    const std::string& op, const std::string& json) {
                    if (!self->current_message_id().is_request())
                        return;
                    Op opv;
                    if (!parse_op(op, opv)) {
                        auto rp = self->make_response_promise<db::db_result>();
                        db::db_result r;
                        r.ok = false;
                        r.error = "unknown mongo op: " + op;
                        rp.deliver(std::move(r));
                        return;
                    }
                    auto rp = self->make_response_promise<db::db_result>();
                    enqueue_rr(conn, make_job(opv, coll, json, rp));
                },
                [=](mongo_op_atom, const std::string& coll,
                    const std::string& op, const std::string& json) {
                    if (!self->current_message_id().is_request())
                        return;
                    Op opv;
                    if (!parse_op(op, opv)) {
                        auto rp = self->make_response_promise<db::db_result>();
                        db::db_result r;
                        r.ok = false;
                        r.error = "unknown mongo op: " + op;
                        rp.deliver(std::move(r));
                        return;
                    }
                    auto rp = self->make_response_promise<db::db_result>();
                    enqueue_rr(*default_conn, make_job(opv, coll, json, rp));
                },
            };

            return caf::behavior{business.or_else(plugin_lifecycle(self, PluginLifecycleHooks{
                .on_init = [=](caf::actor, const std::string&) {
                    LOG_INFO_SELF(self, "MongoPlugin initialized, conns={}", uris.size());
                    launch_workers();
                    selfcheck();
                },
                // 无业务状态：连接配置归 CAF 配置文件管
                .on_save = []() -> std::vector<std::byte> {
                    return {};
                },
                .on_shutdown = [=]() {
                    for (auto& [name, slots] : *pools) {
                        for (auto& s : slots)
                            s->queue->stop();
                    }
                    size_t joined = 0;
                    for (auto& [name, slots] : *pools) {
                        for (auto& s : slots) {
                            if (s->thread->joinable()) {
                                s->thread->join();
                                ++joined;
                            }
                        }
                    }
                    std::cout << "[MongoPlugin] shutdown hook: " << joined
                              << " workers joined" << std::endl;
                    LOG_INFO_SELF(self, "MongoPlugin shutdown, {} workers joined", joined);
                },
            }))};
        });
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new MongoPlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* p) {
    delete p;
}
