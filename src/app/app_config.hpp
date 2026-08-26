#pragma once
// ------------------------------------------------------------------
// 进程配置 = 插件框架（caf_plugin_core） + 集群节点（caf_plugin_cluster）
// 两个模块正交，可按需组合：
//   - 纯插件进程：默认加载 caf-application.conf（CAF 默认文件名，无需参数）
//   - 纯节点进程：--caf-plugin-system.node-kind=master|region|worker
//   - 节点 + 插件：两者都配（region 上跑服务插件）
// 从 main.cpp 分离：供 main 与 app_backdoors 共用。
// ------------------------------------------------------------------

#include "framework_bootstrap.hpp"
#include "cluster/bootstrap.hpp"

namespace caf_plugin_system {

struct app_config : framework_config {
    cluster::node_settings node_cfg;
    app_config() : framework_config() {
        // middleman 元对象注册 + 加载（必须在 actor_system 构造前）
        cluster::init_node_io(*this);
        cluster::add_node_options(*this, node_cfg);
    }
};

} // namespace caf_plugin_system
