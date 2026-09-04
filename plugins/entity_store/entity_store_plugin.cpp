#include "common/plugin_config.hpp"
#include "plugin/plugin_interface.hpp"
#include "services/logging_service.hpp"
#include "templates/sql_entity_store_actor.hpp"

#include <caf/all.hpp>

#include <algorithm>
#include <chrono>
#include <string>

namespace entity_sql = caf_plugin_system::entity_store::sql;

namespace {

#define ENTITY_STORE_FIELDS(X, XC)                                           \
    X(std::string, dialect, "sqlite")                                       \
    X(std::string, backend_service, "")                                     \
    X(std::string, schema_source, "manual")                                  \
    X(int, request_timeout_ms, 10000)                                        \
    X(int, max_pending_requests, 1024)                                       \
    X(int, save_retry_attempts, 3)                                           \
    X(int, load_retry_attempts, 3)                                           \
    X(int, retry_backoff_ms, 100)                                            \
    X(caf::settings, stores, {})
PLUGIN_CONFIG(ENTITY_STORE_FIELDS)
#undef ENTITY_STORE_FIELDS

// 默认后端服务名（dialect -> service name）, 在数据库插件里面注册
std::string default_backend_service(entity_sql::dialect_kind dialect) {
    switch (dialect) {
        case entity_sql::dialect_kind::sqlite:   return "sqlite_service";
        case entity_sql::dialect_kind::mysql:    return "mysql_service";
        case entity_sql::dialect_kind::postgres: return "pg_service";
    }
    return {};
}

entity_sql::service_config make_service_config(const PluginConfig& config) {
    entity_sql::service_config result;
    auto dialect = entity_sql::parse_dialect(config.dialect);
    if (!dialect) {
        result.config_error = "unknown SQL dialect: " + config.dialect;
        return result;
    }
    result.dialect = entity_sql::sql_dialect{*dialect};
    result.request_timeout = std::chrono::milliseconds{std::max(config.request_timeout_ms, 100)};
    result.max_pending_requests = static_cast<size_t>(std::max(config.max_pending_requests, 1));
    result.save_retry_attempts = static_cast<uint32_t>(std::clamp(config.save_retry_attempts, 1, 10));
    result.load_retry_attempts = static_cast<uint32_t>(std::clamp(config.load_retry_attempts, 1, 10));
    result.retry_backoff = std::chrono::milliseconds{std::max(config.retry_backoff_ms, 1)};
    auto backend = config.backend_service.empty()
                       ? default_backend_service(*dialect)
                       : config.backend_service;
    std::string error;
    auto catalog = entity_sql::schema_catalog::parse(
        config.stores, backend, backend, error, config.schema_source);
    if (!catalog) {
        result.config_error = std::move(error);
        return result;
    }
    result.catalog = std::move(*catalog);
    return result;
}

} // namespace

class EntityStorePlugin : public PluginEntry {
public:
    plugin_manifest manifest() const override {
        // 后端服务由配置解析，运行时从 ServiceRegistry 获取稳定代理。
        // 因此 EntityStore 不静态绑定某一种数据库插件。
        // 独立 SQL 插件优先启动；反向关机时 EntityStore 会先排空请求，
        // 避免后端连接池先退出。后端类型仍由配置动态选择。
        return {"EntityStorePlugin", "1.0.0", {}, {"entity_store"}, 100, {}};
    }

    caf::actor spawn(caf::actor_system& sys,
                     const std::vector<caf::actor>&,
                     const std::string&) override {
        caf_plugin_system::set_log_source(PLUGIN_NAME);
        auto settings = make_service_config(load_plugin_config(sys.config()));
        if (!settings.config_error.empty())
            LOG_ERROR("EntityStore configuration error: {}",
                      settings.config_error);
        else
            LOG_INFO("EntityStore initialized with {} logical store(s)",
                     settings.catalog.size());
        return sys.spawn<entity_sql::entity_store_actor>(std::move(settings));
    }
};

extern "C" PLUGIN_API PluginEntry* create_plugin() {
    return new EntityStorePlugin();
}

extern "C" PLUGIN_API void destroy_plugin(PluginEntry* plugin) {
    delete plugin;
}
