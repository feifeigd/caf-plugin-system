# 跨节点 envelope 实测步骤（业务服务信封调用）

> 目的：验证 **master → RemoteCaller → 远端节点业务插件服务** 的信封往返
> （plugin_envelope 跨节点链路）。区别于 bridge external_echo 三跳测试
> （docs/cluster-e2e-test.md）——本测试直连**插件服务**（pg_service），
> 不经过 bridge/外部进程。
> 实测通过：2026-08-26（feat/cross-node-envelope 分支）

## 0. 前置条件

- 已构建 `caf_plugin_app` + `postgres_plugin`（VS 内置 CMake，Debug）
- db-pg 容器运行（`docker.exe start db-pg`）
- 端口 47096 / 47097 空闲
- run_bridge 具备：exe/DLL/conf + `lib/` 驱动目录（见 docs/cluster-e2e-test.md 踩坑 1）

## 1. 业务服务加信封入口（功能前提）

RemoteCaller 把 `plugin_envelope` **直接发给目标服务**（remote_caller.cpp
`do_call`：`request(route.target, ..., ks.env)`，响应期待 `std::string`）。
因此插件服务要支持跨节点调用，需实现信封 handler（子协议号插件自管）：

```cpp
// postgres_plugin.cpp business behavior 内：
[=](plugin_envelope env) -> caf::result<std::string> {
    switch (env.sub_proto) {
    case 1: {   // hello：跨节点链路自检
        std::string in(reinterpret_cast<const char*>(env.payload.data()),
                       env.payload.size());
        return std::string("pg:hello:") + in;
    }
    default:
        return caf::make_error(caf::sec::invalid_argument,
                               "pg_service: unknown sub_proto: "
                                   + std::to_string(env.sub_proto));
    }
},
```

## 2. 启动 worker（PostgresPlugin 注册 pg_service）

```bash
cd run_bridge
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=worker \
  --caf-plugin-system.node-name=bridge-a \
  --caf-plugin-system.node-port=47097 \
  --caf-plugin-system.master-port=47096 \
  --caf-plugin-system.parent=master \
  --caf-plugin-system.lease-seconds=6 \
  > worker_env_test.log 2>&1"
```

预期：`[Registry] Registered: pg_service (v1) exported` + selfcheck 全绿。

## 3. 启动 master（--test-cross-call-ex=pg_service）

```bash
cd run
cmd.exe /c "caf_plugin_app.exe \
  --caf-plugin-system.node-kind=master \
  --caf-plugin-system.node-name=master \
  --caf-plugin-system.node-port=47096 \
  --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.test-cross-call-ex=pg_service \
  > master_env_test.log 2>&1"
```

参数：`test-cross-call-ex=<服务名>`——任意节点模式（resolve_any），
信封 sub_proto=1(hello) + payload="cross"，有界重试 15 次 × 1s
（worker 注册晚也不丢，重启窗口期语义）。

## 4. 验证判据

| 日志 | 含义 |
|---|---|
| `[CrossCallEx] request with attempts=15` | 调用启动 |
| `service resolve 'pg_service' failed: no_such_key` + `retrying` | worker 未注册，有界重试（正常时序） |
| `resolved 'pg_service' -> 1 candidate(s)` | worker 注册后路由命中 |
| **`[CrossCallEx] OK: pg:hello:cross`** | **跨节点信封往返成功** |

泄露判据：两进程 `grep -ac "normal block"` 均为 0。

## 5. 清理

```bash
cmd.exe /c "taskkill /IM caf_plugin_app.exe /F"
```

## 实测记录（2026-08-26）

- `resolved 'pg_service' -> 1 candidate(s)` → `[CrossCallEx] OK: pg:hello:cross`
- 前 2 次尝试 no_such_key（worker 启动中）→ 第 3 次成功（有界重试验证）
- 泄露 0 块（master + worker）
