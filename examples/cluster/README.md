# 带插件的集群样例

一个 master + 两个 worker，每个节点都挂业务插件（混合模式）。

## 节点拓扑

| 节点 | 角色 | 插件 |
|---|---|---|
| master | master (port 47000) | BusinessPlugin |
| worker-a | worker | BusinessPlugin + LoggerPlugin |
| worker-b | worker | BusinessPlugin |

## region 子树（多级拓扑）

| 节点 | 角色 | parent |
|---|---|---|
| region-a | region (中间层) | master |
| worker-r1 | worker (叶子) | region-a |

region/worker 都连 master:47000，parent 字段标记层级。实测注册输出：

```
[ClusterMaster] registered node 'region-a' (region) parent=master
[ClusterMaster] registered node 'worker-r1' (worker) parent=region-a
```

## 运行

配置和脚本拷贝到 run/（或任何有 exe + plugins/ 的目录）：

```bash
cp *.conf run-cluster.sh <run目录>/
cd <run目录> && ./run-cluster.sh
```

脚本自动：清残留进程 → 起 master → 起两个 worker → 打印注册闭环 → 清理。

## 手动启动（等价）

```bash
./caf_plugin_app.exe --config-file=master.conf &
./caf_plugin_app.exe --config-file=worker-a.conf &
./caf_plugin_app.exe --config-file=worker-b.conf &
```

## 容错验证结论（实测）

- **lease 清理**：worker-lease.conf（lease-seconds=3）挂起心跳停 →
  master `expired node 'worker-lease'`（maintenance 1s 步长兜底）
- **自愈**：杀 master → worker 秒级 `master lost, reconnecting` →
  1s 重试 → master 重启后自动 `register: OK`，零人工干预
- **优雅退出**：Ctrl+C → 完整优雅关机（插件 save_state）→ 自然退出

## 格式说明

全部使用 CAF 1.1 配置规范：
- 花括号嵌套块（JSON-like），注释用 `#`
- 字符串必须带引号，数组用 `[...]`
- 节点参数：node-kind / node-name / node-port / master-port / lease-seconds / parent
- 插件参数：entry-plugins（顶层插件，依赖自动拓扑排序）
