# 远程热更新 E2E 测试步骤（reload-node 链路）

> 目的：验证 **master ops → reload-node → 目标节点 plugin_mgr** 远程热更链路：
> quiesce 屏障（代理静默 + 在途排空）→ 新路径加载新 DLL → registry 台账
> hot_reload（impl 引用 + 版本号）→ 代理 resume（切目标+冲刷）→ 旧实例退役。
> 实测通过：2026-08-26（PLUGIN_CONFIG 迁移后，commit 052140d/6f16c90 基线）

## 0. 前置条件

- 已构建 `caf_plugin_app` + `postgres_plugin`（VS 内置 CMake，Debug）
- db-pg 容器运行（PostgresPlugin selfcheck 需要）：
  `"/mnt/c/Program Files/Docker/Docker/resources/bin/docker.exe" start db-pg`
- 端口 47096 / 47097 空闲
- run_bridge 已具备：exe/DLL/conf + `lib/` 驱动目录（postgres 插件依赖 libpq 等，
  缺 lib/ 会 probe 静默失败 → 启动即退出，见 docs/cluster-e2e-test.md 踩坑 1）

## 1. 制造"新版本"插件（v2 识别特征）

热更新要求新代码在**新路径**（Windows 对已加载 DLL 有文件锁，且 LoadLibrary
同路径返回缓存旧模块）。测试用最小改动制造可观察差异：

```bash
# plugins/postgres/postgres_plugin.cpp 改 2 处：
#   ① manifest 版本 "1.0.0" -> "2.0.0"（registry 台账 Hot-reloaded 版本号判据）
#   ② init 日志 "PostgresPlugin initialized, conns={}" ->
#      "PostgresPlugin initialized (v2), conns={}"（肉眼判据）
"/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build out/build/windows-x64 --config Debug --target postgres_plugin

# 部署到 stage/ 目录（【必须】在 plugins/ 之外！扫描会 probe 目录内所有 DLL，
# 放 plugins/ 下会导致 worker 启动时直接加载 v2，破坏 v1→v2 测试语义）
mkdir -p run_bridge/stage
cp -f out/build/windows-x64/plugins/postgres/Debug/postgres_plugin.dll \
      run_bridge/stage/postgres_plugin_v2.dll
```

## 2. 启动 worker（加载 v1 旧 DLL）

```bash
cd run_bridge
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=worker \
  --caf-plugin-system.node-name=bridge-a \
  --caf-plugin-system.node-port=47097 \
  --caf-plugin-system.master-port=47096 \
  --caf-plugin-system.parent=master \
  --caf-plugin-system.lease-seconds=6 \
  > worker_reload_test.log 2>&1"
```

预期（v1 判据）：`Plugin loaded successfully: PostgresPlugin` +
`PostgresPlugin initialized, conns=1`（**无 v2 标记**）+ selfcheck 4 链全 ok。

## 3. 启动 master（带远程热更后门）

```bash
cd run
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=master \
  --caf-plugin-system.node-name=master \
  --caf-plugin-system.node-port=47096 \
  --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.test-remote-reload=bridge-a,PostgresPlugin,stage/postgres_plugin_v2.dll \
  > master_reload_test.log 2>&1"
```

参数说明：
- `test-remote-reload=<node>,<plugin>,<path>`：master 引导后 **12s 延迟**发
  `reload-node <node> <plugin> <path>` 到本机 ops（等 worker 注册命中）。
  逗号被替换为空格；路径相对目标节点 cwd（本例 worker cwd=run_bridge → `stage/...`）。
- 完整命令前缀（`reload `/`reload-node `/`reload-nodes `/`reload-all `）会原样透传。

## 4. 验证（热更触发后 ~15s）

worker 日志证据链（顺序即屏障时序）：

| 日志 | 含义 |
|---|---|
| `[Ops] remote reload 'PostgresPlugin' -> stage/postgres_plugin_v2.dll` | 命令到达目标节点 |
| `[Proxy] quiesced, buffering new calls` | **quiesce 屏障**：代理静默，新调用缓冲 |
| `Plugin hot-reloaded: PostgresPlugin -> stage/postgres_plugin_v2.dll` | 新 DLL 加载成功 |
| `Retired instance cleaned up (hot-reload/unload)` | 旧实例退役 |
| `[Registry] Hot-reloaded: pg_service (v2)` | **台账版本号更新** |
| `PostgresPlugin initialized (v2), conns=1` | **新 actor 跑新代码** |
| selfcheck 4 链 ok=true | 热更后服务可用（无中断） |

master 日志：`[Ops] remote reload 'bridge-a': OK - reloaded`

泄漏判据：两进程日志 `grep -ac "normal block"` 均为 0。

## 5. 清理

```bash
cmd.exe /c "taskkill /IM caf_plugin_app.exe /F"
# 测试痕迹清理：
git checkout -- plugins/postgres/postgres_plugin.cpp   # 回滚 v2 标记
# 重建 v1 保持 out/ 与源码一致
# stage/ 测试产物可删（不入库）
```

## 踩坑记录（2026-08-26 实测）

1. **新 DLL 不能放 plugins/ 目录**：扫描器 probe 子目录内**所有** .dll（目录名无关），
   v2 与 v1 同目录 → 启动直接加载 v2（日志出现 v2 标记），破坏 v1→v2 语义。
   新版本放 stage/ 等 plugins/ 之外的目录，热更 path 指向它。
2. **热更约束（实现内置）**：新代码必须新路径；不能删服务（provides 必须包含旧全集）；
   不能改插件名；不能注册新 type_id（号段启动时已锁，新协议走信封 sub_proto）。
3. **quiesce 失败自动回滚**：代理静默后等 ack 失败会 resume 回旧实现，不丢调用。
4. **热更期间调用不丢**：代理邮箱 FIFO——ack 前到达的调用都已委托给旧 actor，
   缓冲的新调用在 resume 时切到新 impl 并冲刷。
