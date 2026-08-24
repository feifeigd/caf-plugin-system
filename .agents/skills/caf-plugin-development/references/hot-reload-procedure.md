# 热更新流程 Hot-Reload Procedure（caf-plugin-system）

完整流程文档，对接项目现有接口（quiesce/save_state/send_exit/hot_reload_atom/register_atom/resume_atom/retired_/plugin_lib_pool）。

## 总览时序

```
reload_atom(new_dll_path)
    │
    ▼
① 校验 manifest（失败→拒绝，旧服务零影响）
② quiesce 旧代理 → ack（断流：该来的都来了）
③ request save_state → 快照（FIFO 屏障：来了的都处理了）
④ LoadLibrary(新路径) → 新 HMODULE（路径必须变！）
⑤ spawn 新 actor → restore_state(快照)
⑥ 台账切换：旧服务 hot_reload / 新服务 register+ACL
⑦ resume 旧代理 → 流量切到新 actor
⑧ send_exit 旧 actor 立即冻结 → instance 移入 retired_
⑨ down_msg 到达 → destroy(instance)（永不 FreeLibrary）
```

## 1. 部署（落盘）

```
构建产物 → run/updates/<plugin>/<version>/<name>.dll   ← LoadLibrary 这个新路径
可选回滚通道：旧 DLL rename 成 <name>.dll.old（已加载 DLL 可 rename，不可覆盖）
```

- **绝不覆盖已加载的 DLL**：文件锁 + LoadLibrary 同路径缓存旧模块表项 → 同路径 = 旧代码
- `.old` 通道：`LoadLibrary(".../<name>.dll.old")` 是新路径 → 重新映射旧代码 → 免费回滚
- 建议带版本标记（构建哈希），加载后校验，防止 load 到错误版本

## 2. 校验（validate BEFORE quiesce）

| 检查项 | 规则 |
|---|---|
| provides | **只增不减**：删服务/改名 → 拒绝（旧代理永久静默 + 台账指向死 actor） |
| type_id | 无新增（元对象须在 actor_system 构造前注册，重复注册 abort） |
| 导出接口 | `register_meta_objects` / `create_plugin` 入口存在且签名匹配 |

校验失败 → 直接拒绝 reload，**旧服务完全不动**。唯一不需要回滚的失败路径。

## 3. 断流（quiesce）

- 对旧 provides 列表**每个服务代理**发 `quiesce_atom`，等 ack
- ack 语义：ack 前代理 delegate 的调用已**同步入队**旧 actor 邮箱；ack 后不再转发新调用
- 前提：registry 只暴露 proxy 不暴露 impl 句柄（架构约束，缺了屏障不成立）

## 4. 快照（save_state barrier）

- `request(旧actor, save_state_atom)` 等响应
- FIFO：响应到达 = 前面消息全部处理完 → **响应本身就是邮箱排空证明**（空数据也行）
- 快照数据（on_save 回调产出）序列化，交给新 actor

## 5. 加载新模块

- `LoadLibrary(new_path)` → 新 HMODULE / 新代码段 / 新 vtable
- LoadLibrary 失败往往静默（probe open FAILED 无错误信息）→ 先查 exe 目录缺不缺 fmt.dll/spdlog.dll 等运行时依赖
- 成功后 spawn 新 actor：`init_atom` → `restore_state_atom(snapshot)`
- restore 失败 → 走回滚矩阵

## 6. 台账切换

- 旧服务：`hot_reload_atom`（代理内部 target 切到新 actor，调用方手里的 proxy 句柄不变）
- 新服务：`register_atom`（自动建 proxy）+ 补发 `set_service_acl_atom`（若声明 acl_allow）
- 旧服务 ACL 原样保留

## 7. 流量切换

- 对旧代理列表发 `resume_atom` → 恢复转发到新 actor
- 此时新调用全部打新实例，热更新对调用方**无感**（只持有稳定 proxy）

## 8. 冻结旧 actor

- **立即** `send_exit(user_shutdown)`——**禁止 delayed_send**：延迟窗口里异步回调继续执行变更状态 → 快照 ≠ 终态
- 旧 instance 移入 `retired_`，从存活表移除
- **禁止立即 destroy**：send_exit 异步（exit 前排队消息仍处理）→ UAF
- 拒绝退役兜底：超时**告警** + 移出 retired_ 放弃跟踪（实例随 DLL 常驻，进程退出 OS 回收）——**禁止超时强制 destroy**

## 9. 失败回滚矩阵

| 失败点 | 处理 |
|---|---|
| 校验失败 | 拒绝，无事发生 |
| quiesce 超时 | 中止 + resume 旧代理，服务不中断 |
| save_state 超时 | 同上，中止回滚 |
| LoadLibrary 失败 | 中止，旧服务继续跑；查运行时依赖 |
| spawn/restore 失败 | 清理新 actor（send_exit），台账不动，resume 旧代理 |
| 切换后新 actor 崩溃 | down_msg 检测 → `.old` 回滚：LoadLibrary 旧路径 → spawn → restore 最近快照 |

## 10. 清理与句柄池

- `down_msg` 到达才 `destroy(instance)`（termination 信号 ≠ 资源释放，actor 对象 refcount 释放可能滞后）
- **DLL 永不 FreeLibrary**：vtable/lambda 在 DLL 代码段，CAF 异步释放引用，提前 unload → 0xC0000005；句柄池 `plugin_lib_pool()` 常驻到进程退出
- `.old` 回滚文件在下次成功更新后可清（进程退出后删，避免文件锁）

## 11. 红线清单

1. LoadLibrary 路径**必须变**（新版本 = 新路径）
2. provides 只增不减
3. type_id 不新增
4. send_exit 立即，不 delayed
5. 永不 FreeLibrary，永不超时强制 destroy
6. unregister 必须在 quiesce 之后
7. 旧 actor 只从 proxy 收调用（屏障前提）
