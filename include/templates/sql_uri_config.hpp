#pragma once

// MySQL/PostgreSQL 等命名 SQL 连接的轻量配置解析。
// 仅承载当前配置契约 user:pass@host:port/database；驱动专属参数仍由插件处理。

#include <caf/all.hpp>

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace caf_plugin_system::sql_backend {

struct ConnectionSpec {
    std::string name;
    std::string user;
    std::string pass;
    std::string host = "127.0.0.1";
    int port = 0;
    std::string dbname;
};

class ConnectionUriParser {
public:
    ConnectionUriParser(std::initializer_list<std::string_view> schemes,
                        std::string default_user, int default_port)
        : default_user_(std::move(default_user)),
          default_port_(default_port) {
        schemes_.reserve(schemes.size());
        for (auto scheme : schemes)
            schemes_.emplace_back(scheme);
    }

    std::vector<ConnectionSpec> parse(const caf::settings& values) const {
        std::vector<ConnectionSpec> result;
        for (const auto& [name, value] : values) {
            auto uri = caf::get_if<std::string>(&value);
            if (uri)
                result.push_back(parse_one(name, *uri));
        }
        if (result.empty())
            result.push_back(make_default());
        return result;
    }

private:
    ConnectionSpec make_default() const {
        ConnectionSpec spec;
        spec.name = "default";
        spec.user = default_user_;
        spec.port = default_port_;
        return spec;
    }

    ConnectionSpec parse_one(const std::string& name,
                             std::string uri) const {
        auto spec = make_default();
        spec.name = name.empty() ? "default" : name;
        remove_scheme(uri);
        parse_credentials(uri, spec);
        parse_endpoint(uri, spec);
        return spec;
    }

    void remove_scheme(std::string& uri) const {
        for (const auto& scheme : schemes_)
            if (uri.rfind(scheme, 0) == 0) {
                uri.erase(0, scheme.size());
                return;
            }
    }

    static void parse_credentials(std::string& uri, ConnectionSpec& spec) {
        const auto at = uri.find('@');
        if (at == std::string::npos)
            return;
        auto credentials = uri.substr(0, at);
        uri.erase(0, at + 1);
        const auto colon = credentials.find(':');
        if (colon == std::string::npos) {
            spec.user = std::move(credentials);
            return;
        }
        spec.user = credentials.substr(0, colon);
        spec.pass = credentials.substr(colon + 1);
    }

    static void parse_endpoint(const std::string& uri,
                               ConnectionSpec& spec) {
        const auto slash = uri.find('/');
        auto host_port =
            slash == std::string::npos ? uri : uri.substr(0, slash);
        const auto colon = host_port.find(':');
        if (colon == std::string::npos) {
            if (!host_port.empty())
                spec.host = std::move(host_port);
        } else {
            spec.host = host_port.substr(0, colon);
            spec.port = std::atoi(host_port.substr(colon + 1).c_str());
        }
        if (slash != std::string::npos)
            spec.dbname = uri.substr(slash + 1);
    }

    std::vector<std::string> schemes_;
    std::string default_user_;
    int default_port_;
};

} // namespace caf_plugin_system::sql_backend
