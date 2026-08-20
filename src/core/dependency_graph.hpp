#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <algorithm>

class DependencyGraph {
public:
    void register_service(const std::string& service, const std::string& plugin) {
        service_providers_[service] = plugin;
    }

    void add_plugin(const std::string& plugin_name,
                    const std::vector<std::string>& service_deps) {
        std::vector<std::string> plugin_deps;
        for (const auto& svc : service_deps) {
            auto it = service_providers_.find(svc);
            if (it != service_providers_.end() && it->second != plugin_name) {
                plugin_deps.push_back(it->second);
            }
        }
        graph_[plugin_name] = plugin_deps;
    }

    /// 循环检测
    bool has_cycle_from(const std::string& plugin_name) const {
        std::unordered_set<std::string> visiting; // 当前路径栈
        std::unordered_set<std::string> visited;  // 已经确认没环的节点
        return dfs(plugin_name, visiting, visited);
    }

    std::vector<std::string> topological_order() const {
        std::vector<std::string> result;
        std::unordered_set<std::string> visited;
        std::unordered_set<std::string> temp_mark; // 当前深度优先搜索栈

        for (const auto& [node, _] : graph_) {
            if (!visited.count(node)) {
                visit(node, visited, temp_mark, result);
            }
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    std::vector<std::string> reverse_topological_order() const {
        auto order = topological_order();
        std::reverse(order.begin(), order.end());
        return order;
    }

private:
    bool dfs(const std::string& node,
             std::unordered_set<std::string>& visiting,
             std::unordered_set<std::string>& visited) const {
        if (visiting.count(node)) return true;
        if (visited.count(node)) return false; // 已经确认没环的节点

        visiting.insert(node);
        auto it = graph_.find(node);
        if (it != graph_.end()) {
            for (const auto& dep : it->second) {
                if (dfs(dep, visiting, visited)) return true;
            }
        }
        visiting.erase(node);
        visited.insert(node); // 已经确认没环的节点
        return false;
    }

    void visit(const std::string& node,
               std::unordered_set<std::string>& visited,
               std::unordered_set<std::string>& temp_mark,
               std::vector<std::string>& result) const {
        if (temp_mark.count(node)) {
            throw std::runtime_error("Cycle detected at: " + node);
        }
        if (visited.count(node)) return;

        temp_mark.insert(node);
        auto it = graph_.find(node);
        if (it != graph_.end()) {
            for (const auto& dep : it->second) {
                visit(dep, visited, temp_mark, result);
            }
        }
        temp_mark.erase(node);
        visited.insert(node);
        result.push_back(node);
    }

    std::unordered_map<std::string, std::vector<std::string>> graph_;
    std::unordered_map<std::string, std::string> service_providers_;
};
