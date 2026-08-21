# CAF Plugin System

基于 C++ Actor Framework (CAF 1.1) 的生产级插件系统 + 分布式集群。

插件系统（`src/core/plugin/`）与集群（`src/core/cluster/`）正交组合，
同一进程可按需装配，支持三种进程形态。

## 特性

- **Actor 模型并发**：无锁消息传递，天然多线程安全
- **IoC 依赖注入**：服务注册与发现（service_registry）
- **循环依赖检测**：拓扑排序 + DFS 环检测
- **热更新（Hot Reload）**：Proxy 模式零停机切换，provides 只增不减
- **优雅关机**：Drain → Save → Exit 三级状态机，插件状态逐个落盘
- **状态持久化**：CheckpointManager 自动存盘与恢复（CRC32 + 原子 rename）
- **公共生命周期协议**：`PluginLifecycleHooks` + `plugin_lifecycle()` 组合 API
- **分布式集群**：master 扁平注册表 + region/worker 多级子树
- **容错自愈**：lease 过期清理 + 断线重连 + master 恢复自动重新注册
- **零外部依赖**：仅依赖 CAF（vcpkg manifest 自动安装）

## 架构

```
caf-plugin-system/
├── src/
│   ├── app/
│   │   └── main.cpp              # caf_main：插件框架 ⊗ 集群节点 正交装配
│   └── core/                     # caf_plugin_core 单库（STATIC）
│       ├── framework_bootstrap.* # 插件框架引导（framework_config + bootstrap + wait）
│       ├── service_registry.*    # IoC 服务注册表
│       ├── checkpoint_manager.*  # 状态存盘管理
│       ├── graceful_shutdown.*   # 优雅关机协调器（shutdown_mgr）
│       ├── dependency_graph.*    # 依赖图与环检测
│       └── plugin/               # 插件子系统
│           ├── plugin_interface.hpp   # 插件最小接口（含跨平台导出宏）
│           ├── plugin_manager.*       # 插件管理器（加载/卸载/热更新）
│           ├── plugin_loader.*        # 插件扫描 + 依赖解析 + 拓扑排序
│           ├── plugin_lifecycle.*     # 公共生命周期协议（Hooks + 组合 API）
│           └── dynamic_library.*      # 跨平台动态库加载
│       └── cluster/              # 集群子系统
│           ├── bootstrap.*       # 节点引导（node_settings + bootstrap_node + wait）
│           ├── master.*          # master 扁平注册表（注册/心跳/lease/拓扑/路由）
│           ├── client.*          # 节点客户端状态机（自愈注册 + 心跳）
│           └── membership_registry.* # 成员表（manifest + monitor + lease 双通道）
├── include/common/               # 内核协议（message_tags / message_meta / cluster_types / envelope）
├── plugins/                      # 示例插件（business / logger / platform）
├── examples/cluster/             # 集群样例配置 + 一键拉起脚本
└── docs/
    ├── plugin-guide.md           # 插件开发指南（消息体系/号段/权限/生命周期）
    └── sequence-diagram.html     # 时序图
```

## 三种进程形态

| 形态 | 配置 | 说明 |
|---|---|---|
| 纯插件 | 只配 `entry-plugins` | 单进程插件宿主 |
| 纯节点 | 只配 `node-kind` | 集群 master / region / worker |
| 节点 + 插件（混合） | 两者都配 | 节点上跑业务插件，如 region 挂服务 |

## 快速开始

### 构建

```bash
# vcpkg manifest 模式自动安装 CAF 1.1
cmake --preset windows-x64
cmake --build --preset windows-x64-debug
```

### 运行（插件模式）

`caf-application.conf` 是 **CAF 默认配置文件名**，exe 无参数自动加载：

```conf
# caf-application.conf
caf-plugin-system {
  entry-plugins = ["BusinessPlugin"]
}
```

```bash
./caf_plugin_app.exe        # 无需 --config-file
```

### 运行（集群）

```bash
# master
./caf_plugin_app.exe --config-file=master.conf
# worker（自动注册 + 心跳 + 自愈重连）
./caf_plugin_app.exe --config-file=worker-a.conf
```

完整拓扑（master + 2 workers、region 子树）见 [`examples/cluster/`](examples/cluster/README.md)，
含一键拉起脚本 `run-cluster.sh`。

## 配置参考（CAF 1.1 规范）

配置文件用**花括号嵌套块**（JSON-like），注释用 `#`：

```conf
caf-plugin-system {
  # 插件：顶层插件名单，依赖自动拓扑排序
  entry-plugins = ["BusinessPlugin","LoggerPlugin"]
  # 关闭顺序（可选），留空用依赖反序
  # shutdown-order = ["BusinessPlugin"]
  # 插件扫描目录（默认 ./plugins）
  # plugins-dir = "./plugins"
  # 启动后自动优雅关机（冒烟测试后门）
  # test-auto-shutdown = true

  # 集群节点
  node-kind = "worker"        # master / region / worker（空 = 纯插件）
  node-name = "worker-a"
  node-host = "127.0.0.1"
  node-port = 0               # 0 = 自动分配（master 通常固定）
  master-host = "127.0.0.1"
  master-port = 47000
  lease-seconds = 30          # 租约；0 = 永不过期
  parent = "master"           # 父节点名（region/worker 层级标记）
}
```

要点（实机踩坑结论）：
- **字符串必须带引号**，数组用 `[...]`；`[section]` ini 语法会解析失败
- 自定义组选项用 `--caf-plugin-system.xxx` 前缀（CLI 同理）
- 日志文件分离：`--caf.logger.file.path=xxx.log`

## 集群容错（实测验证）

- **lease 清理**：心跳停止（网络分区/进程僵死）→ lease 过期 →
  `expired node 'xxx'`（maintenance 1s 步长兜底，监控通道失效场景）
- **自愈**：master 挂 → worker 秒级 `master lost, reconnecting` → 1s 重试 →
  master 恢复自动 `register: OK`，零人工干预
- **优雅退出**：Ctrl+C → 完整优雅关机（插件逐个 save_state → registry 停 →
  节点 actor 停止 → 哨兵自动退）→ 进程自然退出，不丢状态
- **down 检测**：进程退出/被强杀 → 监控通道秒级感知（`went down: remote_link_unreachable`）

## 插件开发

见 [`docs/plugin-guide.md`](docs/plugin-guide.md)（消息注册硬要求 / 号段与权限 /
三套消息机制 / 热更新 / 生命周期骨架）。

## 设计要点

### 公共生命周期协议

插件最常见的 5 个生命周期 handler（init/drain/save/restore/shutdown）骨架
固化在框架侧，插件只需注册回调：

```cpp
PluginLifecycleHooks hooks;
hooks.on_init     = [](caf::actor mgr, const std::string& cfg) { /* 初始化 */ };
hooks.on_save     = [] { return std::vector<std::byte>{ /* 序列化 */ }; };
hooks.on_shutdown = [] { /* 清理 */ };

caf::message_handler business{ /* 私有业务 handler */ };
return caf::behavior{business.or_else(plugin_lifecycle(self, hooks))};
```

- 私有在前：业务消息一次命中（高频路径最优）
- or_else 语义：插件可写同名生命周期 handler 覆盖框架默认
- drain 回执 / shutdown quit 由框架统一处理，写错即卡死/泄漏的样板不再存在

### 热更新 / 卸载退役

- reload：校验 provides 只增不减 → quiesce → save_state 屏障 → spawn 新实例 →
  旧服务 hot_reload / 新服务注册 + ACL → resume → 立即 send_exit 旧实例
- 旧 DLL 句柄池常驻（meta 池 + plugin 池），退役统一 `retired_` + down_msg 回收

### 优雅关机流程

```
Ctrl+C / shutdown_atom
  │
  ▼
[Drain]  停止接收新消息，处理完存量请求
  │
  ▼
[Save]   按拓扑逆序逐插件序列化状态 → CheckpointManager 落盘
  │
  ▼
[Exit]   停 registry/plugin_mgr/checkpoint_mgr → 停节点 actor → 哨兵自动退
```

## License

MIT
