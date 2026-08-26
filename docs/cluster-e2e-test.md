# 集群跨节点调用 E2E 测试步骤（bridge external_echo 链路）

> 目的：验证 **集群 → bridge → 外部进程** 三跳跨节点调用链路
> （master cross_call_ex → RemoteCaller 解析 → worker 节点 external_echo
> → REQ 行协议 → Python 外部进程 echo → RESULT 回程 → master 收结果）
> 实测通过：2026-08-26（commit 052140d broker 版 bridge + a567440 合并后）

## 0. 前置条件

- 已构建 `caf_plugin_app`（VS 内置 CMake，Debug）：
  `"/mnt/c/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" --build out/build/windows-x64 --config Debug --target caf_plugin_app`
- 端口 47096 / 47097 / 48063 空闲（`cmd.exe /c "netstat -ano | findstr "47096 47097 48063""` 为空）
- 无 db 容器要求（worker 不加载 db 插件）

## 1. 准备 run_bridge 目录（worker 运行环境）

```bash
cd /mnt/g/git/caf-plugin-system
# exe + 运行库 + conf（conf 的 entry-plugins 必须 = []，见踩坑 1）
for f in caf_plugin_app.exe caf_core.dll caf_io.dll fmtd.dll spdlogd.dll; do
  cp -f run/$f run_bridge/
done
cp -f run/caf-application.conf run_bridge/
# 插件（可留空；本测试不需要 db 插件）
for p in redis mysql postgres mongo; do
  mkdir -p run_bridge/plugins/$p
  cp -f out/build/windows-x64/plugins/$p/Debug/${p}_plugin.dll run_bridge/plugins/$p/
done
# 关键：run_bridge/caf-application.conf 的 entry-plugins 改为空列表！
# （PostgresPlugin 会因缺 lib/ 驱动 probe 失败 → Failed to resolve → 启动即退出）
```

## 2. 启动 master（后台，带跨节点测试后门）

```bash
cd run
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=master \
  --caf-plugin-system.node-name=master \
  --caf-plugin-system.node-port=47096 \
  --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.test-bridge-call=bridge-a \
  > master_test.log 2>&1"
```

参数说明：
- `test-bridge-call=<节点名>`：master 引导后对指定节点发 cross_call（external_echo + payload "bridge-ping"），**重试 60 次 × 1s**（等 worker 注册）

## 3. 启动 bridge worker（后台）

```bash
cd run_bridge
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=worker \
  --caf-plugin-system.node-name=bridge-a \
  --caf-plugin-system.node-port=47097 \
  --caf-plugin-system.master-port=47096 \
  --caf-plugin-system.parent=master \
  --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.bridge-port=48063 \
  > worker_test.log 2>&1"
```

## 4. 等集群就绪（~15s）

预期日志：
- worker：`[Registry] Registered: external_echo (v1) exported`
  `[Bridge] bridge-a listening on port 48063 (external_echo registered)`
  `Node 'bridge-a' listening on port 47097`
  `[NodeClient:bridge-a] register: OK (node registered)`
- master：`[RemoteCaller] resolved 'external_echo' -> node 'bridge-a'`
  （出现 attempt N: request_timeout 是正常的——外部进程还没连，见踩坑 2）

## 5. 跑外部进程客户端（接住 REQ 并自动 echo）

```bash
cd tools
timeout 45 python3 -u bridge_client.py 172.19.128.1 48063
# 注意：WSL 里必须用宿主 IP（127.0.0.1 localhost 转发失效，见踩坑 3）
```

预期：
- 客户端：`[client] REQ N handled -> b'echo:bridge-ping'`（master 重试的 REQ 全到达）
- master 日志出现：
  `[BridgeTest] external_echo@'bridge-a' -> echo:bridge-ping` ← **三跳链路验证成功**

## 6. 清理

```bash
cmd.exe /c "taskkill /IM caf_plugin_app.exe /F"
```

## 7. 验证判据

| 层 | 证据 |
|---|---|
| 节点注册 | worker `register: OK` + master 路由表含 bridge-a |
| 跨节点解析 | master `[RemoteCaller] resolved 'external_echo' -> node 'bridge-a'` |
| bridge REQ 出站 | 客户端收到 `REQ N ... echo:bridge-ping` |
| 外部 echo 回程 | 客户端 auto-reply（`echo_handler`） |
| 跨节点回程 | master `[BridgeTest] external_echo@'bridge-a' -> echo:bridge-ping` |

## 踩坑记录（2026-08-26 实测）

1. **run_bridge 缺 lib/ 驱动目录**：conf 的 `entry-plugins=["PostgresPlugin"]` 时，
   postgres_plugin.dll 因缺 libpq 等依赖 **probe 静默失败** → `Unknown plugin: PostgresPlugin`
   → `Failed to resolve dependencies` → 进程启动即优雅退出。
   解决：本测试不需要 db 插件，`entry-plugins = []`。
2. **master 早期 attempt timeout 是正常时序**：外部进程未连接时，REQ 行无连接可写
   （bridge 单连接语义），impl pending 不 deliver → request 3s 超时 → 重试。
   外部进程连上后下一轮 attempt 即成功。
3. **WSL 连 Windows 监听端口**：`127.0.0.1` 不通（WSL2 localhost 转发失效），
   必须用宿主 IP `172.19.128.1`（`ip route | grep default` 可查）。
4. **cluster_tui.py --selftest 需要 textual**（`ModuleNotFoundError: No module named 'textual'`）——
   无头验证直接用 `bridge_client.py`（只依赖 bridge_proto.py，纯 stdlib）。
5. **exe 必须是新构建**：run/run_bridge 的 caf_plugin_app.exe 不会自动同步 out/build 产物，
   拷贝前检查时间戳（旧版日志行号 542 vs 新版 293 可判别）。
