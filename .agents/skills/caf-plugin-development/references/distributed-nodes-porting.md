# distributed-nodes 架构分析 + 移植到 caf-plugin-system 的方案与实施

来源：`G:\git\warehouse-backend\distributed-nodes`（caf::io + middleman 的树形多进程示例，2026-08 分析）。
CAF vcpkg 含 `caf_io.lib` + `caf/io/*` 头文件，middleman 前提满足。

## 架构精髓（"master 扁平 + 任意多子树"）

```
master（扁平注册表：所有节点注册，树关系是数据不是连接）
├── region-a（region 服务：child_membership 管子树）
│   ├── compute-a1（向 master 注册 + attach region-a + 心跳）
│   └── storage-a1
└── region-b（任意多子树，每 region 一个进程/actor）
```

1. **master 扁平** = 树是 *数据* 不是 *连接*：所有节点向 master 注册 `node_manifest{kind, node_name, host, port, parent, exported_actors}`（`master_register_atom`）；`parent` 只是 manifest 里的字符串字段。查拓扑 `master_topology_atom` / 查子树 `master_children_atom(parent)` / 解析 actor `master_resolve_atom(node, actor) → actor_route`。
2. **健康检测双通道**：monitor/down_msg（`monitored_node_registry::erase_by_monitor`）+ lease 心跳过期清理（`prune_expired`，master/region 各自 delayed_send maintenance_tick）。核心数据结构 `monitored_node_registry`（slot = manifest + monitor_actor + expires_at；upsert/touch/erase/for_each_manifest）——master_state 和 child_membership 复用它，是移植的核心可复用件。
3. **子树 attach**：子节点注册 master 后，若 parent 非空，再 `region_attach_atom` 到父 region 的 router（remote_lookup parent 的 `region.router`）；父 region 维护 `child_membership`（同一成员表）。detach/unregister 对称。
4. **寻址**：`cluster` 类（main-thread 同步客户端，勿跨 actor 共享）——master 连接（`remote_lookup(master.control)`）+ route_cache + remote_actor_manager 缓存 + `with_retry`（注册/attach 重试）。
5. **关机级联**：`shutdown_signal`（lifetime/Ctrl+C/外部请求）→ `node_shutdown_atom(shutdown_request{initiator, source_node, reason, source})` 沿树传播；父请求用 response_promise + `add_completion_waiter` **等子树真关完再回复**；`shutdown-parent-on-exit` / `shutdown-children-on-exit` 开关 + 防回弹（父→子时子不回发）。
6. **节点入口模式**：每 exe 一个 `*_config : node_config` + `run_*()`（spawn node_control + 业务 actor + `sys.registry().put(name, actor)` + lifecycle）+ `CAF_MAIN(id_block::distributed_nodes, io::middleman)`。node_control 处理 `node_describe_atom` / `node_shutdown_atom`。

## 实际落点（2026-08 已实施，双进程闭环验证通过）——与方案的差异

**用户拍板：优先当前项目注册机制（插件要动态热更新），只参考集群机制。** 因此：

| 方案（移植前） | 实际实施 |
|---|---|
| 节点协议类型用 type id block（静态链接 OK） | **进 `message_tags.def` 显式 ID（235-253）**，沿用手写 meta——零机制冲突，ID 空间不分裂（ID 冲突问题直接消失） |
| master 做成**服务插件** | **内核 actor**（`spawn_cluster_master` + `sys.registry().put("cluster.master", ...)`）——注册表是节点级基础设施，与插件服务正交；后续仍可包成插件 |
| cluster 同步客户端 | **`cluster_client` actor**（connect→register→心跳→断线自动重连自愈），bootstrap spawn 后不管 |

落地文件：
- `include/common/cluster_types.hpp` — 协议类型（node_kind master/region/worker、node_manifest、node_registration、register_reply、topology_snapshot、child_snapshot、actor_route、shutdown_request/shutdown_source）+ inspect；**X_REG 展开处类型要全限定 `caf_plugin_system::node_kind`**（app_meta 命名空间内）
- `src/core/cluster_membership.hpp` — 成员表（slot=manifest+monitor+lease；upsert/touch/erase/erase_by_monitor/prune_expired）
- `src/core/cluster_master.*` — master 注册表 actor（注册/心跳续租/down 清理/拓扑/子树/路由查询；master 自身跳过 lease 过期）
- `src/core/cluster_client.*` — 节点客户端 actor（状态机 connecting→registered→reconnecting）
- `framework_config` 节点选项：`--caf-plugin-system.node-kind/name/host/port/master-host/master-port/lease-seconds/parent`；**纯节点进程允许空 entry-plugins**（bootstrap 用 `if (!cfg.entry_plugins.empty())` 包裹插件引导段）

**双进程验证（全部通过）**：
```
master(47000) ←注册/心跳→ worker-a
[ClusterMaster] registered node 'worker-a' (worker) parent=master   ✓
[ClusterMaster] heartbeat refreshed 'worker-a' ×N                    ✓ lease 续租（lease=6s，心跳=lease/3）
[ClusterMaster] node 'worker-a' went down: remote_link_unreachable   ✓ 强杀感知（monitor 跨进程 + BASP 断开检测）
```
命令行验证：master `--caf-plugin-system.node-kind=master --caf-plugin-system.node-name=master --caf-plugin-system.node-port=47000 --caf-plugin-system.lease-seconds=6`；worker 同款 + `--caf-plugin-system.master-port=47000 --caf-plugin-system.parent=master`。

## 踩坑（实施期实锤）

1. **middleman 模块两步加载**：`caf::io::middleman::init_global_meta_objects()` **然后** `load<caf::io::middleman>()`，都在 actor_system 构造前（framework_config 构造函数）。只 load 不 init → `[FATAL] I/O module loaded without calling caf::io::middleman::init_global_meta_objects() before`（这正是 `CAF_MAIN(id_block, io::middleman)` 模块参数机制自动做的两步）。
2. **`caf_io.dll` 运行时依赖**：exe 链接 CAF::io 后，部署目录缺 caf_io.dll → 进程秒退零输出（loader 错误）。vcpkg debug/bin/caf_io.dll 拷到 run/。
3. **`remote_lookup` 返回 `strong_actor_ptr`**（不是 expected）——空检查用 `if (!remote)`，转 actor 用 `caf::actor_cast<caf::actor>(std::move(remote))`。`connect` 返回 `expected<node_id>` ✓。
4. **CLI 选项全名带组前缀**：`--caf-plugin-system.node-kind=master`；`--node-kind=master` → `not_an_option`（--help 输出里就是全名，别被省略迷惑）。
5. **头文件非 inline 函数多 TU include → LNK4006**（to_string/from_string/from_integer 必须标 inline）。
6. **PowerShell 杀进程陷阱**：`Get-Process caf_plugin_app | Select -First 1 | Stop-Process` **无序**可能杀掉 master（两个进程都匹配）——用 `Sort-Object StartTime -Descending | Select -First 1` 杀最新的（worker）。
7. **down 检测"不工作"先查杀进程是否真的杀了**：`Get-Process` 验证残留 + 看进程数。

## 未做（后续候选）

region 服务插件（子树 attach）、拓扑查询 CLI、节点间 plugin_envelope 跨进程调用、关机级联（node_shutdown_atom 双向传播）。
