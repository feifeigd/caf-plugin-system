# Actor 强引用环排查记录（2026-08-24 破案全过程）

caf-plugin-system 的 CRT dump 长期恒定 ~200 块/19.8KB，"字节与活动量零相关"被
判为常驻单例。用户凭代码阅读发现强引用环：**shutdown_mgr ↔ plugin_mgr**。
隔离验证后 dump 从 205 块/19,833B 暴跌到 **1 块/8B**——确认环就是"泄露"。

## 环的成员与赋值路径（改前）

| 持有方 | 成员 | 指向 | 赋值路径 |
|---|---|---|---|
| GracefulShutdown | `plugin_mgr_` (graceful_shutdown.hpp:36) | plugin_mgr | 构造参数（bootstrap_system_components spawn 时传入）|
| PluginManager | `shutdown_mgr_` (plugin_manager.hpp:41，已删) | shutdown_mgr | `(shutdown_atom, caf::actor mgr)` handler（死代码）或 `request_shutdown_atom` handler 的 `registry().get("shutdown_mgr")` |
| OpsActor | `shutdown_mgr_` (ops_actor.cpp:496) | shutdown_mgr | 构造参数（main.cpp spawn 时传入） |
| shutdown_mgr | `cluster_ctls_` vector | ops / master / client | main.cpp `register_cluster_atom_v` 注册 |
| shutdown_mgr | — | 全部组件 | 构造参数（plugin_mgr/registry/checkpoint/logging） |
| 静态 | `shutdown_manager_ref()` (framework_bootstrap.cpp:46) | shutdown_mgr | :335 `= out.shutdown_mgr`，永不清除（:463 清空被注释） |

环：shutdown_mgr →(plugin_mgr_)→ plugin_mgr →(shutdown_mgr_)→ shutdown_mgr
（以及 ops/master 两个同构环）。静态 `shutdown_manager_ref()` 单方面保活
shutdown_mgr——即使环断了，不清它 shutdown_mgr 的 control block 也不会释放。

## 关键语义（为什么这是泄露）

CAF 中 actor 收到 exit 终止 = 行为/actor 对象开始销毁，但 **control block 的
强引用数不到 0 就整个 actor 对象（含所有成员 `caf::actor`）永驻堆上**。
成员强引用随 actor 对象存活 → 环内任何一方都无法走到 refcount=0。
actor_system 析构 join 的是"活着的 actor"，不 join control block——所以
进程 EXIT 0、`registry().running()`=0 全部通过，dump 却报块。

## 修复（plugin_mgr 侧，2026-08-24 已提交）

plugin_manager.cpp：
1. 删死代码 handler `(shutdown_atom, caf::actor mgr)`（原 424-427 行）——
   bootstrap:350 的发送方 `self->send(out.plugin_mgr, shutdown_atom{}, out.shutdown_mgr)`
   早已被注释，handler 永远收不到消息
2. `request_shutdown_atom` 改为瞬时查找：`auto mgr = caf::actor_cast<caf::actor>(self->system().registry().get("shutdown_mgr")); if (mgr) self->send(mgr, shutdown_atom{});` ——不存成员
3. 删成员 `caf::actor shutdown_mgr_;`（plugin_manager.hpp:41）

注意：`request_shutdown_atom` 不是死代码！business 插件收到 `"shutdown"` 命令会发
（business_plugin.cpp:111），只是低频。所以不能整块删，只能改瞬时。

## 实测验证

- `--test-ctrl-c`（不依赖 ops 的后门；`--test-quit` 依赖 ops，ops 被注释时会
  `ops actor not found in registry` 挂死——这是正常的中间态不是回归）
- 改前基线：`--test-quit` 205 块/19,833B；冒烟全量 207 块/20,081B；裸跑 206 块/19,857B
- 改后（用户同时注释了 ops/节点 spawn 块做隔离）：**1 块/8B**（`{2348}` 8 bytes，
  指针形态 `10 89 A3 C1 FE 7F 00 00`，疑似 registry/静态残留）
- 优雅关机链完整：Graceful shutdown initiated → All plugins saved → STOPPED，EXIT 0

## 剩余 TODO（~~完整版清零还需三刀~~ —— 已解决，2026-08-24 后续分析推翻）

~~1. ops 侧：`ops_actor.cpp:496` 的 `caf::actor shutdown_mgr_;` 同款改瞬时/弱引用~~
~~2. `GracefulShutdown::finish_shutdown`：quit 前清 `plugin_mgr_/registry_/checkpoint_mgr_/cluster_ctls_`~~
~~3. `framework_bootstrap.cpp:463` 取消注释 `shutdown_manager_ref() = caf::actor{};`~~

**不需要了**：正常 return 路径下静态析构（[basic.start.term]）在 CRT dump 前执行，从根
（`shutdown_manager_ref()` 静态）递归解开整条引用链——实测不手动清任何东西 dump 也 0 块。
三刀只对 ExitProcess(0) 路径有意义（跳过静态析构），但该路径不触发 dump、无观测价值。
详见 SKILL.md "函数局部 static caf::actor 不需要手动清空" 条目。

## 跨进程（mesh 网状节点互相调用）——环的边界与句柄缓存纪律（2026-08-24）

**结论：mesh 调用本身不产生泄露；泄露只来自"跨进程持久句柄"被缓存。**

**为什么跨进程无环**：强引用环是**进程内**概念（control block 引用计数）。跨进程调用
走 BASP 连接 + 消息，不共享 control block——A 调 B、B 调 A 是双向消息流，各自进程内
没有互相持有的引用，天然无环。与 shutdown_mgr↔plugin_mgr 那种进程内环本质不同。

**泄露形态矩阵**：

| 模式 | 会不会泄露 |
|---|---|
| 调用中临时 connect / remote_lookup / request | 不会——异步消息，终态后释放 |
| 缓存 `actor_route`（纯数据：host/port/actor_name） | 不会——不保活 control block |
| **缓存 `caf::actor` 强引用**（remote actor 的本地代理 control block） | **会**——remote 死/断连，本地代理块被攥着不释放 |
| 重试状态机无终态（pending promise 永挂） | 会——promise 回调持有 sender 引用 |
| middleman 连接池 | 不会——CAF 系统级管理，析构关闭 |

**判据不同**：进程内环是**常量泄露**（对照实验测不出）；mesh 句柄泄露**随节点数/断连
次数线性增长**——杀节点重启风暴后 dump 涨 = 句柄泄露，恒定 = 干净（可验证，是好事）。

**当前架构安全性**（caf-plugin-system 已满足）：RemoteCaller 缓存路由数据（round-robin
游标 + healthy 标记），connect/lookup 临时用、用完即弃；cross_call_ex 有界重试有终态
（全败清缓存报错）。按此纪律 mesh 化不会泄露。

**四条纪律**：
1. 跨调用长期容器**只存 actor_route 数据**，不存 `caf::actor`
2. 若必须缓存 remote actor 句柄 → 必须 monitor + down_msg 清理
3. 重试状态机必须有终态（不留永久 pending promise）
4. 连接复用交给 middleman，别自建持久连接句柄池

**验证方法**：mesh 冒烟 = 3 节点互调 N 轮 → 关机 dump；再跑"杀节点重启 ×N"风暴 →
dump 随重启次数线性增长 = 句柄泄露，恒定 = 干净。

## 排查方法沉淀

- **引用图审计**：grep actor 类成员 `caf::actor`/`strong_actor_ptr`，按 spawn 参数
  连线，找互相指向
- **隔离实锤**：注释嫌疑 spawn 块 → dump 暴跌即证（比逐块分析快得多）
- **判读教训**：常量泄露（环）与活动量零相关 → 对照实验有盲区；"字节一致"
  只证明无增长，不证明无固定泄露
- 验证后门选择：`--test-quit` 依赖 ops（ops 注释/删除时不可用）；`--test-ctrl-c`
  直连 shutdown_mgr，任何组件裁剪下都能触发关机
