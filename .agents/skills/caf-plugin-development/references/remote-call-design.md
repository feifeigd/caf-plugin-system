# 跨节点调用设计（RemoteCaller + master 服务路由，2026-08 重构）

从 distributed-nodes 移植的集群机制只覆盖注册/心跳/lease/down 检测；跨节点**服务调用**
（RemoteCaller）是后续加的。本文记录一次完整设计重构：修掉两个扩展性问题 + 同名服务
多节点的语义缺口 + **blocking→event-based 架构纠正**，含完整实现与验证记录。

## 旧设计的问题（用户指出）

1. **cross_call_atom 只按 svc 找**：resolve 取拓扑快照里**第一个**导出该服务的节点——
   多个节点同名服务时：无法指定目标节点、无负载均衡（全打第一台）、无 failover
   （快照顺序稳定，失败重试很可能又命中同一台死节点）。
2. **resolve_service 每次拉全量拓扑**：`node_topology_atom` 返回所有节点的完整 manifest，
   客户端 O(N) 遍历过滤。上千节点时每次 cache miss 都是 O(N) 序列化 + 传输 + 遍历。
3. **blocking_actor 单线程全局串行**（重构中发现）：一 key 的长重试（cross_call_ex
   最多 attempts×1s）堵住整个 RemoteCaller 线程——其他节点/服务的调用全部排队。
   用户原话："一个节点可能堵死整个集群"。

根因：master 本来就是服务注册表，但查询逻辑放在客户端（拉全量 → 本地过滤）。
注册中心侧的过滤必须由注册中心做；调用侧串行粒度必须按 key，不能全局。

## 新设计 1：master 侧服务路由查询

### 协议增量（message_tags.def，续在 256 后）

```
X_TAG(service_resolve_atom, 257)  // 按服务名解析：master 返回导出该服务的全部节点路由
X_REG(service_route, (caf_plugin_system::service_route), 258, "service_route")
X_TAG(cross_call_ex_atom, 259)    // 跨节点调用（有界重试）
X_TAG(remote_retry_tick_atom, 260) // RemoteCaller 内部：per-key 重试定时器
```

cluster_types.hpp 新增：

```cpp
struct service_route {
  std::vector<actor_route> routes;   // 复用既有 actor_route（node_name/kind/host/port/actor_name/parent）
};
```

包一层 struct 而非裸 vector：沿用 child_snapshot 包 vector 的既有注册模式，
避免裸 `std::vector<T>` 跨进程类型注册的坑。

### master 新 handler（master.cpp）

```cpp
[this](service_resolve_atom, const std::string& svc) -> caf::result<service_route> {
  prune_expired();
  service_route route;
  nodes.for_each_manifest([&](const node_manifest& m) {
    if (std::find(m.exported_actors.begin(), m.exported_actors.end(), svc)
        == m.exported_actors.end())
      return;
    route.routes.push_back(
      actor_route{m.node_name, m.kind, m.host, m.port, svc, m.parent});
  });
  if (route.routes.empty())
    return caf::make_error(caf::sec::no_such_key);
  return route;
}
```

响应体 O(k)（k = 导出该服务的节点数），不再是 O(N)。master 侧内存扫描上千
manifest 是微秒级；真要 10k+ 节点再考虑反向索引 `svc → node_names`（upsert/erase
时同步维护），第一版不需要。

## 新设计 2：RemoteCaller = event-based actor + per-key 串行队列（重构终版）

**架构决策（用户拍板）**：blocking_actor 单线程全局串行不可接受，重构为
event_based_actor + `unordered_map<key, KeyState>`。key = svc（任意模式）或
svc@node（指定模式）。

- **同 key 严格串行**：KeyState 持 in_flight + deque<PendingCall> 队列，
  响应回调（finish）才放行队头 → 同一远端 actor 的消息 FIFO 有序
- **跨 key 完全并行**：一个节点的重试只阻塞它自己的队列
- **异步全链路**：resolve/connect/call 都是 `request(...).then(...)`；
  重试等待用 `delayed_send(this, 1s, remote_retry_tick_atom_v, key)`，
  期间 actor 照常处理其他 key 的消息
- **退出**：event-based 默认处理 exit；未 settle 的 promise 随终止自动交付
  sec::actor_died，调用方安全失败；delayed_send 定时器随 actor 死失效

### 状态机（每个 KeyState 独立）

```
enqueue(svc, node, env, attempts)
  ├─ in_flight ? queue.push_back(PendingCall{rp, env, attempts})
  └─ start：in_flight=true → pump

pump
  ├─ routes 空 → resolve（异步：service_resolve_atom / node_resolve_atom）
  └─ routes 非空 → try_route

try_route（round-robin 找第一个 healthy）
  ├─ 找到 → target 就绪 ? do_call : do_connect（异步 connect → 同步 lookup 一次 → do_call）
  └─ 全 unhealthy → routes.clear() → resolve（拿最新拓扑）

do_call 成功 → rp.deliver(value) → finish（放行同 key 队头）
do_call / do_connect / resolve 失败 → routes[idx].healthy=false → try_route
resolve 失败（含全候选失败后）→ retry_or_fail：
  ├─ --attempts_left > 0 → delayed_send(1s, remote_retry_tick_atom, key)
  └─ 0 → rp.deliver(err) → finish
```

核心代码骨架（remote_caller.cpp，CAF 1.1 实测可编译）：

```cpp
class RemoteCallerActor : public caf::event_based_actor {
  struct Route { actor_route info; caf::actor target; bool healthy = true; };
  struct PendingCall { response rp; plugin_envelope env; int attempts; };
  struct KeyState {
    std::string svc, node_name;
    std::vector<Route> routes; size_t cursor = 0;
    bool in_flight = false;
    response rp; plugin_envelope env; int attempts_left = 0;
    std::deque<PendingCall> queue;
  };
  // make_behavior：4 个 cross_call/cross_call_ex handler（arity 重载）+ remote_retry_tick handler
  // enqueue → keys_[key]（map 元素引用稳定：只增不删，回调可安全持引用）
};
```

## 语义决策（已拍板：方案 C）— cross_call_ex 有界重试

用户质疑："对方重启时调用直接按报错处理？" 关键在业务语义：

- **查询类**：失败报错合理，调用方重试或放弃即可
- **命令类**（写操作）：自动重试 = at-least-once，可能**重复执行**——RemoteCaller
  看到的是字节流信封，无法判断业务幂等

**方案 C 混合**：`cross_call(svc, env)` 单次立即报错（调用方按业务自定重试）；
`cross_call_ex(svc, node?, attempts, env)` 有界自动重试（重启窗口期调用不丢）。
幂等性由调用方选入口，RemoteCaller 不背锅。

四种形态（arity 重载共存）：cross_call(svc,env) / cross_call(svc,node,env) /
cross_call_ex(svc,attempts,env) / cross_call_ex(svc,node,attempts,env)。

**有序性约束（写进头注释）**：同 key 串行 FIFO（RemoteCaller per-key 队列保证）+
同 receiver（调用方用指定节点模式保证）。任意节点模式 round-robin 分散多副本，
节点间无顺序保证——严格有序必须指定节点模式。

## 验证记录（2026-08 实测）

### 基础 + failover（三节点）

配置：master.conf（node-kind=master + BusinessPlugin + test-cross-call +
allow-cross-node）+ worker-a/b.conf（worker + BusinessPlugin + allow-cross-node）。

```
[RemoteCaller] resolved 'business_service' -> 2 candidate(s)   // service_resolve 生效
[CrossCall] OK: cross-ok:cross                                  // 连续 10 次成功
```

failover 实测（调用循环进行中按命令行杀 worker-a）：
```
[RemoteCaller] call to 'worker-a' failed (request_timeout), failover
[RemoteCaller] all candidates unhealthy for 'business_service', re-resolving
[RemoteCaller] resolved 'business_service' -> 1 candidate(s)   // 切到 worker-b
[CrossCall] OK: cross-ok:cross                                  // 继续成功
```

### ex 重启窗口期验证（event-based 终版实测）

- 配置：master.conf `test-cross-call-ex = "business_service"`（15 次 × 1s）
- **先启 master、延迟 ~6s 启 worker** → 期望轨迹：
  `attempt failed for 'business_service' (no_such_key), retrying in 1s (14 left)`
  × 6 → `resolved 'business_service' -> 1 candidate(s)` → `[CrossCallEx] OK: cross-ok:cross`
- 测试后门：`--test-cross-call=<svc>`（循环 5+11 次）、`--test-cross-call-ex=<svc>`
  （有界重试 15×1s；framework_config 加 `test_cross_call_ex` 字段 + option）

### 坑 1：配置键写在块外被静默忽略

`cat >> master.conf` 追加 `test-cross-call = ...` 到 `caf-plugin-system { }` **闭括号
之后** → 该键不进任何 group，解析后为空，后门不触发且**无任何报错**（症状：CrossCall
测试不跑，排查半天）。必须写进 `caf-plugin-system { ... }` 块内。

### 坑 2：worker 端 ACL 拦截跨节点调用

business 插件 manifest 声明 `acl_allow={"LoggerPlugin"}` → worker 端 ServiceRegistry
默认 `allow_cross_node=false` → RemoteCaller 从 master 发来的调用被代理 ACL 拦下：
worker 日志 `[Proxy] ACL blocked a call to the service`，master 端表现为
`all candidates failed for 'business_service'`（failover 试完所有候选都失败）。
修复：worker 配置加 `allow-cross-node = true`。

### 坑 3：按命令行杀特定 worker（勿按启动时间挑进程）

所有节点进程都是同一个 exe（run/caf_plugin_app.exe），`Where-Object {$_.Path -like
'*worker*'}` 匹配不到。**按启动时间挑会误杀 master**（实测：Sort-Object StartTime |
Select -First 1 杀掉了最早启动的 master 自己，两个 worker 开始疯狂重连 master）。
正确做法是按命令行过滤：

```powershell
Get-CimInstance Win32_Process -Filter "Name='caf_plugin_app.exe'" |
  Where-Object { $_.CommandLine -like '*worker-a.conf*' } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force }
```

## CAF 1.1 事实（本次编译/运行实测，已入 SKILL.md pitfalls）

1. **`caf::result<T>` 没有 `operator bool`**（error C2451）：判别成功/失败用
   `caf::expected<T>`（try_once 内部返回 expected），handler 出口再包回
   `caf::result<T>{std::move(*res)}` 或 `{std::move(res.error())}`。
2. **`caf::result<T>` 可从 `typed_response_promise<T>` 构造**（`result_base(const
   response_promise&)`）：异步延迟响应姿势 = handler 里 `auto rp =
   make_response_promise<T>(); return rp;`（rp 随消息流传递），后续
   `rp.deliver(value)` / `rp.deliver(error)` 补响应。make_response_promise 是
   local_actor 成员。
3. **`remote_lookup()` 是阻塞的**（"Blocks the caller until nid responded"）：
   event-based actor 用 middleman actor 异步接口 connect：
   `request(mm.actor_handle(), timeout, caf::connect_atom_v, host, port)` →
   `result<node_id, strong_actor_ptr, set<string>>`。connect_atom 是 CAF core
   自带 atom（`caf::connect_atom_v`），无需自注册。lookup 只在首连时同步一次。
4. **异步回调捕获按 index 不按引用**：`.then` 里访问候选列表元素必须捕获索引
   （`ks.routes[idx]`），不能捕获 `auto& route`——回调执行时 vector 可能已
   clear/重填（全 unhealthy 清缓存重 resolve），引用悬垂。map 元素引用（`auto& ks
   = keys_[key]`）是安全的：只增不删。
5. **blocking_actor = 每实例一个专用 OS 线程**（detached）：receive 阻塞不占
   worker；等待循环必须监听 exit_msg（`receive(exit_handler, after(1s) >> ...)`）
   否则 shutdown join 卡死。**跨节点调用客户端别用 blocking**（全局串行堵全部），
   用 event-based per-key 队列。

## 改动面（本次实施）

- `include/common/cluster_types.hpp`：+service_route（含 inspect）
- `include/common/message_tags.def`：+257/258/259/260
- `src/core/cluster/master.cpp`：+service_resolve_atom handler
- `src/core/cluster/remote_caller.{hpp,cpp}`：blocking → event-based per-key 状态机，
  双形态 × 单次/重试四 handler，候选缓存 + failover + 有界重试
- `src/app/main.cpp` + `framework_bootstrap.{hpp,cpp}`：+test_cross_call_ex 后门
- 测试配置（run/ 目录，非入库）：master.conf / worker-a.conf / worker-b.conf 加
  allow-cross-node + test-cross-call(-ex)
