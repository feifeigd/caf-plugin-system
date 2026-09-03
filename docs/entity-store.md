# 通用实体仓储抽象

## 目标

游戏、订单、支付等业务只依赖逻辑实体，不依赖 MySQL、PostgreSQL、SQLite 或 MongoDB 的
查询语言。统一协议在 `entity_store_contract.hpp`，服务名建议使用
`entity_store`。

```text
Game / Order / Payment actor
          |
          | entity_load_atom / entity_save_atom
          v
    EntityStore service
      | schema、分片、版本、幂等、事务
      v
SQL/Mongo adapter -> mysql_service / pg_service / sqlite_service / mongo_service
```

这层划分有两个目的：

- 业务代码不拼 SQL，也不能把客户端传来的实体名或字段名直接当数据库标识符。
- 事务边界由一条保存请求决定，不把连接池事务句柄交给业务 actor 长期持有。

## 协议

### 读取

`entity_load_atom + load_request -> load_result`

- `entity_ref` 由 `store + partition + entity + key` 定位记录；
- `field_mask.all_fields=true` 读取 schema 允许返回的全部字段；
- 否则只读取 `field_mask.names`，实现真正的字段投影；
- 返回 `version`，后续保存可用它做乐观锁。

### 保存

`entity_save_atom + save_request -> save_result`

- `changes` 是一个事务中的实体 patch 列表；
- 每个 `entity_patch.fields` 只描述变化字段，未出现的字段保持原值；
- `set` 写值，`erase` 删除/置空，`increment` 在数据库端原子累加；
- 所有 change 必须路由到同一 `store + partition`，跨分片事务直接返回
  `invalid_request`；
- 任一 patch 失败、记录不存在或版本冲突，整批回滚；
- 只有数据库 COMMIT 成功后，`committed` 才为 true。

示例：

```cpp
using namespace caf_plugin_system::entity_store;

save_request request{
    .request_id = "payment-callback-20260903-42",
    .changes = {
        entity_patch{
            .target = {
                .store = "commerce",
                .partition = "order-20260903-42",
                .entity = "order",
                .key = {{"order_id", value::text("20260903-42")}},
            },
            .fields = {
                {patch_op::set, "status", value::text("paid")},
                {patch_op::increment, "paid_amount", value::decimal("99.50")},
            },
            .check_version = true,
            .expected_version = 7,
        },
        entity_patch{
            .target = {
                .store = "commerce",
                .partition = "order-20260903-42",
                .entity = "payment",
                .key = {
                    {"payment_id", value::text("pay-9001")},
                    {"order_id", value::text("20260903-42")},
                },
            },
            .fields = {
                {patch_op::set, "status", value::text("succeeded")},
            },
            .create_if_missing = true,
        },
    },
};

self->request(entity_store_proxy, timeout, entity_save_atom_v, request);
```

这个请求要么同时把订单标记为已支付并记录支付成功，要么两处都不修改。

## SQL 适配规则

每个逻辑实体必须在服务端注册 schema：

```text
entity order
  table: orders
  shard key: order_id
  key: order_id -> order_id
  version: version
  fields:
    status      -> status      : text
    paid_amount -> paid_amount : decimal
```

字段名只用于查 schema；SQL 标识符来自可信配置，字段值全部走参数绑定。例如
上面的 order patch 可生成：

```sql
UPDATE orders
SET status = ?, paid_amount = paid_amount + ?, version = version + 1
WHERE order_id = ? AND version = ?;
```

`affected=0` 且启用版本检查时返回 `conflict`；未启用版本检查时应区分
`not_found`。主键、分片键和只读字段禁止出现在 patch 中。

## 事务执行

EntityStore 对一次 save 执行固定状态机：

1. 校验请求、schema、字段类型、权限、大小上限和同分片约束。
2. 开启数据库事务并取得仅在适配器内部使用的 `tx_handle`。
3. 在同一事务内写入 `request_id` 去重记录。
4. 按请求顺序执行所有 patch；任何失败立即 ROLLBACK。
5. 保存每个实体的新版本和最终结果。
6. COMMIT；成功后返回 `committed=true`。

底层 SQL 插件现支持：

- 普通读写：`(sql, params)` 或 `(conn, sql, params)`；
- 事务内读写：`(tx_handle, sql, params)`；
- `tx_begin_atom` / `tx_commit_atom` / `tx_rollback_atom`。

事务占用的连接不会接收普通 round-robin 请求，防止其他业务语句误入事务。

## SQL 插件公共实现

三个 SQL 后端复用两层公共组件：

- `sql_connection_pool.hpp`：`ConnectionPool` 管理普通请求选路和事务句柄，
  `ConnectionSlot` 封装专属 FIFO、worker 生命周期与事务槽；
- `sql_service_handlers.hpp`：`SqlServiceDispatcher` 封装请求构造、普通路由、
  事务路由以及 CAF handler 的生命周期；
- `sql_uri_config.hpp`：`ConnectionUriParser` 封装 MySQL/PostgreSQL 命名连接
  URI 的解析策略。

SQLite、MySQL 和 PostgreSQL 插件只保留连接建立、参数绑定、SQL 执行和结果集
转换等驱动相关代码。新增 SQL 后端时应复用这两层，而不是复制事务状态机。

## 写入时序

- 一条 `save_request` 内的 patch 固定在同一事务连接上，并按 `changes`
  顺序进入该连接的 FIFO，因此请求内部有序且原子；
- 使用同一 `tx_handle` 的 SQL 固定进入同一个 `ConnectionSlot`，按
  SQL 服务实际接收顺序执行；
- 不带事务句柄的独立 SQL 会由连接池并行执行，不能保证“先发送就先提交”；
- EntityStore 实现应以 `store + partition` 作为串行键：同分区一次只执行
  一个 save，不同分区并行。多进程或多节点部署还必须使用
  `expected_version` 乐观锁作为最终仲裁，不能只依赖进程内队列。

不建议按整张表串行化。订单、支付等业务通常以订单号作为 partition，同一订单
严格有序，不同订单仍可并行。
## 并发、超时与重试

- 推荐所有可覆盖写都带 `check_version=true`，防止最后写入者静默覆盖。
- `increment` 必须生成数据库原子表达式，不能先读后加。
- `request_id` 必须全局唯一。重复请求返回首次保存的结果，不重复执行 patch。
- 如果 COMMIT 阶段断线，返回 `commit_unknown`；调用方只能用相同
  `request_id` 重试，不能换新 ID。
- 事务必须有总超时；超时或 actor 退出时适配器主动 ROLLBACK 并归还连接。
- 自动重试只适用于事务开始前的连接错误，或使用相同幂等键确认结果；不要盲目
  重放一个可能已经提交的事务。

## 后端边界

- MySQL / PostgreSQL / SQLite：完整支持字段投影、patch、乐观锁和单分片事务。
- MongoDB：字段投影与 patch 可直接映射；多文档事务需要 replica set/session
  支持，实现前不能声称 save 批次原子。
- Redis：适合作缓存或通过 Lua 实现单 key 原子更新；不要把 MULTI/EXEC 的
  命令流伪装成跨实体强事务。

协议中的 `validate` 只做后端无关的结构校验。服务实现还必须做 schema
白名单、类型、权限、请求大小、事务超时和幂等去重。
