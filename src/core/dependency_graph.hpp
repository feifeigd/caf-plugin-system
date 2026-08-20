#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>
#include <algorithm>
#include <queue>

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

    void set_priority(const std::string& plugin_name, int priority) {
        priorities_[plugin_name] = priority;
    }

    /// 循环检测
    bool has_cycle_from(const std::string& plugin_name) const {
        std::unordered_set<std::string> visiting; // 当前路径栈
        std::unordered_set<std::string> visited;  // 已经确认没环的节点
        return dfs(plugin_name, visiting, visited);
    }

    /// Kahn 算法拓扑排序 + 按 priority 优先队列
    std::vector<std::string> topological_order() const {
        // 1. 计算入度
        std::unordered_map<std::string, int> in_degree;
        for (const auto& [node, deps] : graph_) {
            in_degree[node] = static_cast<int>(deps.size());
            for (const auto& dep : deps) {
                in_degree[dep]; // 确保 dep 也在 map 中
            }
        }

        // 2. 优先队列：priority 小的先出队，priority 相同按名字排序（保证确定性）
        auto cmp = [this](const std::string& a, const std::string& b) {
            auto it_a = priorities_.find(a);
            auto it_b = priorities_.find(b);
            int pa = (it_a != priorities_.end()) ? it_a->second : 0;
            int pb = (it_b != priorities_.end()) ? it_b->second : 0;
            if (pa != pb) return pa > pb;  // priority 小的先出队
            return a > b;                   // 名字排序保证确定性
        };
        std::priority_queue<std::string, std::vector<std::string>, decltype(cmp)> q(cmp);

        // 3. 入度为 0 的节点入队（没有依赖，可以先加载）
        for (const auto& [node, deg] : in_degree) {
            if (deg == 0) q.push(node);
        }

        // 4. Kahn 算法主体
        std::vector<std::string> result;
        while (!q.empty()) {
            auto u = q.top(); q.pop();
            result.push_back(u);

            // 找到所有依赖 u 的节点，入度 -1
            for (const auto& [node, deps] : graph_) {
                if (std::find(deps.begin(), deps.end(), u) != deps.end()) {
                    if (--in_degree[node] == 0) {
                        q.push(node);
                    }
                }
            }
        }

        // 5. 环检测：结果数量不等于节点数量说明有环
        if (result.size() != in_degree.size()) {
            return {};
        }

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
        if (visited.count(node)) return false;

        visiting.insert(node);
        auto it = graph_.find(node);
        if (it != graph_.end()) {
            for (const auto& dep : it->second) {
                if (dfs(dep, visiting, visited)) return true;
            }
        }
        visiting.erase(node);
        visited.insert(node);
        return false;
    }

    std::unordered_map<std::string, std::vector<std::string>> graph_;
    std::unordered_map<std::string, std::string> service_providers_;
    std::unordered_map<std::string, int> priorities_;
};
