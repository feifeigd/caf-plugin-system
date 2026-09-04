# 通用实体仓储抽象

## 目标

游戏、订单、支付等业务只依赖逻辑实体，不手写日常读写 SQL。统一协议在
`include/common/entity_store_contract.hpp`，`EntityStorePlugin` 已提供
`entity_store` 服务，支持 SQLite、MySQL、PostgreSQL 和 MongoDB。
Redis 目前仍只有底层操作接口，没有接入这个实体事务协议。

```text
Game / Order / Payment actor
          |
          | entity_load_atom / entity_save_atom
          v
    EntityStore service
      | schema、读写路由、时序、版本、幂等、事务
      v
SQL adapter   -> mysql_service / pg_service / sqlite_service
Mongo adapter -> mongo_service
```

这层划分有两个目的：

- 业务代码不拼 SQL，也不能把客户端传来的实体名或字段名直接当数据库标识符。
- 事务边界由一条保存请求决定，不把连接池事务句柄交给业务 actor 长期持有。

## 协议

### 读取

`entity_load_atom + load_request -> load_result`

- `store` 选择配置好的读写连接，`entity + key` 通过 schema 定位记录；
- `partition` 是保存串行键与批次约束，当前不自动选择物理分片；物理分片可配置成不同 store；
- `field_mask.all_fields=true` 读取 schema 允许返回的全部字段；
- 否则只读取 `field_mask.names`，实现真正的字段投影；
- 返回 `version`，后续保存可用它做乐观锁。

### 保存

`entity_save_atom + save_request -> save_result`

- `changes` 是一个事务中的实体 patch 列表；
- 每个 `entity_patch.fields` 只描述变化字段，未出现的字段保持原值；
- `set` 写值，`erase` 删除/置空，`increment` 在数据库端原子累加；
- 所有 change 必须使用同一 `store + partition`，不一致直接返回
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
                {patch_op::increment, "paid_cents", value::signed_integer(9950)},
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
                },
            },
            .fields = {
                {patch_op::set, "state", value::text("succeeded")},
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
  key: order_id -> order_id
  version: version
  fields:
    status      -> status      : text
    paid_cents  -> paid_cents  : signed_integer
```

Schema 可以手写，也可以从配置白名单中的数据库表自动加载。字段名只用于查 schema；
SQL 标识符来自可信配置或已校验元数据，字段值全部走参数绑定。例如
上面的 order patch 可生成：

```sql
UPDATE orders
SET status = ?, paid_cents = paid_cents + ?, version = version + 1
WHERE order_id = ? AND version = ?;
```

`affected=0` 时继续在同一事务内查询主键：不存在返回 `not_found`，存在但
版本不匹配返回 `conflict`。主键、只读字段和内部版本列不能由 patch 改写。
业务表必须有真实的主键/唯一约束和非空整数版本列，新记录版本从 1 开始。
`create_if_missing + check_version` 仅在 `expected_version=0` 时允许插入缺失记录。

## 事务执行

SQL EntityStore 对一次 save 执行固定状态机（MongoDB 执行路径见下方专节）：

1. 校验请求、schema、字段类型、幂等键长度、在途请求上限和同分区约束。
2. 开启数据库事务并取得仅在适配器内部使用的 `tx_handle`。
3. 在同一事务内写入 `request_id` 去重记录。
4. 按请求顺序执行所有 patch；任何失败立即 ROLLBACK。
5. 保存每个实体的新版本和最终结果。
6. COMMIT；成功后返回 `committed=true`。

底层 SQL 插件现支持：

- 普通读写：`(sql, params)` 或 `(conn, sql, params)`；
- 事务内读写：`(tx_handle, sql, params)`；
- `tx_begin_atom` / `tx_commit_atom` / `tx_rollback_atom`。

`tx_begin_atom(conn, request_key)` 可以带客户端关联键；即使 BEGIN 回执超时、
尚未取得 handle，也可用 `tx_rollback_atom(request_key)` 取消同一笔事务。
关联键在事务归还时删除。COMMIT 失败会先尝试 ROLLBACK 再归还槽位；
MySQL/PostgreSQL 清理失败时关闭物理连接，后续新请求可重连。SQLite 保留停止故障
worker 的策略。旧事务令牌永远不能在重连后的新会话继续执行。

事务占用的连接不会接收普通 round-robin 请求，防止其他业务语句误入事务。

## SQL 插件公共实现

三个 SQL 后端复用以下公共组件：

- `sql_connection_pool.hpp`：`ConnectionPool` 管理普通请求选路和事务句柄，
  `ConnectionSlot` 封装专属 FIFO、worker 生命周期与事务槽；
- `sql_service_handlers.hpp`：`SqlServiceDispatcher` 封装请求构造、普通路由、
  事务路由以及 CAF handler 的生命周期；
- `sql_uri_config.hpp`：`ConnectionUriParser` 封装 MySQL/PostgreSQL 命名连接
  URI 的解析策略。
- `sql_reconnecting_worker.hpp`：`SqlConnection` 驱动接口与 `ReconnectingSqlWorker`，
  统一 MySQL/PostgreSQL 的连接重试、会话失效和事务令牌隔离。

SQLite、MySQL 和 PostgreSQL 插件只保留连接建立、参数绑定、SQL 执行和结果集
转换等驱动相关代码。EntityStore 自身也按 OOP 拆分为：

- `entity_store_schema.hpp`：SQL/Mongo 共用的 `schema_catalog`，字段白名单和读写路由；
  `sql_entity_store_schema.hpp` 保留旧命名空间别名，兼容已有调用；
- `sql_entity_store_statements.hpp`：`sql_dialect`、`statement_builder`，参数化 SQL 生成和结果解码；
- `sql_entity_store_actor.hpp`：`entity_store_actor`，保存队列、事务状态机、幂等回放和排空。
- `sql_entity_store_schema_provider.hpp`：`schema_provider` 及三种数据库实现，生成
  参数化元数据查询、补全字段、校验业务配置；本身不做 I/O。
- `sql_entity_store_recovery.hpp`：actor 的异步表结构加载、纯读取和整笔保存重试。

## MongoDB 适配

使用 `dialect = "mongodb"`（也接受 `"mongo"`），同时加载 `MongoPlugin` 与
`EntityStorePlugin`；完整配置见 [MongoDB 示例](../examples/entity_store/mongodb.conf)。
业务层继续使用上面的 `load_request` / `save_request`，不用手写 SQL 或 Mongo 更新文档。

- 沿用公共 schema 配置：`table` 对应集合，`column` 对应 BSON 字段。
  `keys` / `fields` 是明确的字段白名单；只修改 patch 中的字段，保留未涉及的数据。
  Mongo 当前仅支持 `schema_source = "manual"`；集合没有固定表结构，不能安全地用
  抽样文档推断整套字段、必填约束和键，因此 `database` 模式会明确报错。
- `set` / `increment` / `erase` 分别映射到 `$set` / `$inc` / `$unset`。
  `value::null()` 写 BSON null，`erase` 删除字段；两者仅允许用于 nullable 字段，
  读取缺失的 nullable 字段返回 null。Mongo 缺失字段可由 `$inc` 创建，但显式 null 不能
  直接累加（会报错并回滚），这与 SQL 的 NULL 运算规则不同。新建时必须提供所有必填字段。
- 键字段、只读字段和版本字段禁止 patch 修改。版本必须是正 BSON int64，新建为 1。
  已有文档应先迁移到该约定。系统 `_id` 可以映射为业务键，不能映射成普通字段或版本；
  未显式映射的 `_id` 不会暴露给业务，当前协议没有 ObjectId 类型；JSON 数组不能作为键。
  默认读取只返回普通字段；键已在 `target` 中，需要时可通过字段投影显式请求。
- 首次保存会在事务外创建去重集合和业务键唯一索引。已有重复键、冲突索引或权限不足会
  令保存失败；账号需要相应建集合/建索引权限。不会重写已有文档或自动修复脏数据。
- 一次保存固定在一个 worker 的同一个 session 中，业务变更与去重结果使用真正的多文档
  事务，snapshot 读取、primary 路由、majority 提交；不能跨不同 store / 数据库连接。
  MongoDB 必须是副本集或支持事务的分片集群，普通 standalone 不支持该能力。
  参见 [MongoDB 官方事务说明](https://www.mongodb.com/docs/manual/core/transactions/)。
- 同一 `store + partition` 按接收顺序排队，并固定到同一写 worker；不同分区可并行。
  这不是跨进程全局排序，仍应使用版本检查。load 按读路由执行，读副本可在对应 URI
  配置 `readPreference=secondaryPreferred`；不保证副本上的写后即读。
- 事务暂态失败可在预算内重试整笔；`UnknownTransactionCommitResult` 只重试确认
  同一笔提交。无法确认时返回 `commit_unknown`，调用方必须保留原 `request_id` 和内容。
  同一去重集合内 `request_id` 必须全局唯一，限制为 255 字节；
  同 ID 换内容（包括改 partition）返回 `conflict`。去重记录没有自动过期策略。
- 共享 `request_timeout_ms`、重试次数/退避和排队上限。Mongo worker 的连接、选主与
  socket 等待固定限制为各 2 秒，覆盖 URI 中这些超时配置，并关闭驱动普通读写自动重放；
  EntityStore 自行控制重试。停止会取消排队/退避，当前阻塞调用等待驱动超时，再由所属
  worker 释放 session/client 并 join；总退出时间并非硬实时保证。

类型转换由独立 `EntityCodec` 负责：布尔、signed int64、double、字符串、二进制均为
BSON 原生类型；decimal 和 unsigned integer 使用 Decimal128（不经过 double），
JSON 对象/数组存为嵌套 BSON，不是 JSON 字符串。整数越界、非有限数值、类型不符会被
拒绝；JSON 标量不支持。Mongo 支持 `bytes`，SQL 的二进制限制不变。保存签名最大 8 MiB。

Mongo 专用调度、配置、BSON 转换和原生事务分别放在独立类/文件中；底层已有的
`mongo_op_atom` CRUD 接口仍可使用，但该接口的多次调用不自动组成 EntityStore 事务，
也不自动获得实体层的版本、白名单、排序和幂等保护。

## 写入时序

- 一条 `save_request` 内的 patch 固定在同一事务连接上，并按 `changes`
  顺序进入该连接的 FIFO，因此请求内部有序且原子；
- 使用同一 `tx_handle` 的 SQL 固定进入同一个 `ConnectionSlot`，按
  SQL 服务实际接收顺序执行；
- 不带事务句柄的独立 SQL 会由连接池并行执行，不能保证“先发送就先提交”；
- 当前 EntityStore 以 `store + partition` 作为串行键：同分区一次只执行
  一个 save，不同分区并行。多进程或多节点部署还必须使用
  `expected_version` 乐观锁作为最终仲裁，不能只依赖进程内队列。

不建议按整张表串行化。订单、支付等业务通常以订单号作为 partition，同一订单
在同一个 EntityStore 实例内按接收顺序执行，不同订单仍可并行。多个发送者并没有
天然的全局发送顺序；同一实体必须始终使用相同的 partition，跨进程仍需乐观锁。

## 读写分离

每个 store 可分别配置 `read_service` / `write_service` 与
`read_connection` / `write_connection`。load 只走读路由；save 的全部查询、写入、
幂等记录、COMMIT/ROLLBACK 都钉在写连接的同一事务中，不会中途访问副本。

这只是路由能力，不会自动建立数据库复制，也不消除副本延迟。需要强一致读取时，
把该 store 的读路由指向主库；当前 load_request 没有自动 read-your-writes 标记。

## SQL 并发、超时与重试

- 推荐所有可覆盖写都带 `check_version=true`，防止最后写入者静默覆盖。
- `increment` 必须生成数据库原子表达式，不能先读后加。
- `request_id` 必须在同一幂等表内全局唯一，SQL 实现限制为最多 255 字节。
  同 ID、同请求内容返回首次保存结果；同 ID 换内容返回 `conflict`。
- 如果 COMMIT 阶段断线，先用相同 `request_id` 自动重试整笔保存；若原事务已经提交，
  幂等记录返回原结果，不再执行 patch。重试预算耗尽仍无法确认时返回 `commit_unknown`，
  调用方只能用相同 ID 和原内容再次请求，不能换新 ID。
- `request_timeout_ms` 是 save 总预算，包含排队等待；到期后不再开始新的变更，
  已发送 BEGIN 的请求会按关联键尝试回滚。底层数据库驱动仍应配置合理的连接/语句超时。
- 正常热更和关机会先排空 EntityStore，再关闭 SQL 插件。强杀进程/actor 或后端失联
  不等于已确认回滚，仍需依赖数据库连接断开/服务端超时释放资源；不能把超时当作成功。
- `max_pending_requests` 默认 1024，限制 EntityStore 已接收的在途及排队请求数，
  达到上限立即返回 `unavailable`。停止的数据库 worker 队列也会立即拒绝新任务。
- EntityStore 默认最多尝试 3 次（含首次），`save_retry_attempts` / `load_retry_attempts`
  可分别配置为 1～10；1 表示禁用该层重试。`retry_backoff_ms` 默认 100ms，指数退避
  上限 1s，且不突破该操作的剩余预算。事务取消清理另有最多 2s 超时。
  同分区的 save 在原队首重试，后续请求不会越过它；SQL 错误、版本冲突不自动重试。
- load 重试只用于 EntityStore 生成的纯 SELECT，不承诺重试前后的数据库快照相同。
  底层 SQL 服务不重放任意 SQL（`sql_query_atom` 也可能被调用者用于有副作用的语句）。
- 幂等结果持久化在 `__entity_store_requests`（名称可配置）；没有自动清理策略。
  清理记录会失去对应请求的重放保护，应按业务重试窗口制定保留策略。

## 配置与表结构

### 从数据库加载 Schema

`schema_source` 默认 `"manual"`，完全兼容原先配置。切换为 `"database"` 后：

```conf
entity_store {
  dialect = "sqlite"
  schema_source = "database"
  stores {
    commerce {
      read_connection = "reader"
      write_connection = "writer"
      entities {
        order {
          table = "orders"
          version_column = "version"
        }
        payment {
          table = "payments"
        }
      }
    }
  }
}
```

启动时通过每个 store 的**写服务/写连接**读取白名单中的表，SQLite 使用
`pragma_table_xinfo`，MySQL/PostgreSQL 使用 `information_schema`；校验完成后缓存，
业务 CRUD 不再查询元数据。初始化期间请求在有界队列等待，失败时返回明确错误，
不会发布半成品 Schema。整体初始化受 `request_timeout_ms` 限制。

- `entities` 仍是明确的表白名单，不自动扫描和暴露整库。逻辑别名与版本列保留配置。
- 省略 `keys` 时从真实主键推断（包括复合主键）；显式配置时必须覆盖完整主键。
- 省略 `fields` 时使用该表全部可支持的普通/生成列；显式给出 `fields` 时，它就是
  字段白名单，仅补全这些字段，**不会再额外加入其他列**。字段可只写
  `column`/`kind`/`writable` 等需要覆盖的属性。
- 非空整数版本列必须已经存在。主键不可被 patch 修改，生成列只读，隐藏列不暴露。
  `nullable` 只能收紧，不能放宽数据库的 NOT NULL。插入时未提供的普通列仍由数据库
  应用默认值；EntityStore 不生成或复制默认表达式。
- SQLite BOOLEAN/DECIMAL/无声明类型等无法可靠推断的列要求显式 `kind`。
  MySQL `tinyint(1)` 默认仍是整数，不擅自认为是布尔。二进制列当前不支持，需从字段
  白名单排除；主键若是二进制则不能作为此适配器的实体键。
- 自动发现不创建/迁移业务表，也不为调用方自动生成主键。表迁移应先完成，再启动
  EntityStore；后续迁移通过排空并重启/热更 EntityStore 重新加载，不在进行中的事务里改 Schema。
  store 内也可用 `schema_source` 覆盖全局模式。

### MySQL / PostgreSQL 断线恢复

两个数据库插件均支持 `reconnect_attempts=3`、`reconnect_delay_ms=100`、
`connect_timeout_seconds=2`、`io_timeout_seconds=5`。新请求发现连接未建立/已失效时，
在专属 worker 中进行有限次连接尝试；退避可被关机打断。驱动检测到执行中断线后只报告
失败并关闭会话，**不在新连接上偷偷重放该条 SQL**，后续新请求会触发连接恢复。
MySQL 显式关闭原生自动重连，PostgreSQL 使用带截止时间的非阻塞连接/读写。
MySQL 已建立连接的阻塞读写支持停止中断：每个槽保留一个同一 socket 的独立句柄副本，
停止回调关闭该通道的读写，并在 Windows 上用 `CancelIoEx` 唤醒已挂起的 I/O；
原生连接/语句仍由所属 worker 清理。副本不是新数据库
会话；注销回调后才释放副本，避免关闭或重连期间误伤被复用的句柄。取消不返回部分结果，
写入/COMMIT 仍按 `outcome_unknown` 处理，不假定服务器已回滚。
客户端停止不等于服务端立即取消 SQL：例如 `SLEEP` 可能执行完后才发现连接断开。
建连仍使用驱动连接超时，主机名解析受操作系统行为影响；这不是任意 CPU 计算或
单行内存复制的硬实时退出保证，也没有把 `io_timeout_seconds` 改成整条查询的总期限。

底层 `db_result` 新增 `code`、`native_code`、`sql_state`，用于区分 `sql_error`、
`connection_unavailable`、`connection_lost`、`transaction_lost`、`outcome_unknown`。
断线错误不等于“数据库一定没执行”；特别是写入/COMMIT 回执丢失时，需要业务幂等保护。
更新了消息结构，跨进程部署时应同步升级通信双方，不能混用新旧协议二进制。

### 部署说明

可用示例见 [SQLite 配置](../examples/entity_store/sqlite.conf) 和
[SQLite 业务表迁移](../examples/entity_store/sqlite-schema.sql)。配置加载入口必须同时
包含 `EntityStorePlugin` 与选定的数据库插件；数据库类型由 `dialect` 决定。
EntityStore 优先级为 100，内置数据库插件为 0，默认先启动数据库、先停止 EntityStore。
若显式指定 `shutdown-order`，也应保持这个停机次序。
数据库插件单独热更前也应先排空上层 EntityStore；当前不支持让已开始的事务
跨数据库插件实例续接，旧实例的事务句柄不能在新实例中复用。

SQL 业务代码不需要 SQL；建表、索引和结构迁移仍属于数据库部署工作。SQL EntityStore 只自动
创建自己的幂等表，不自动迁移业务表。SQLite 金额示例使用整数分，避免浮点运算精度问题。
SQL schema 当前不接受 `bytes` 字段；NULL 与空字符串通过结果集的 NULL 位图区分。
PostgreSQL 文本不支持内嵌 NUL 字节；驱动会拒绝这类参数和 SQL，避免静默截断主键或字段。

## 验证范围

- 2026-09-04：最新 Debug 完整构建成功，全部 **19/19 CTest 通过**（190.75 秒），
  包含 SQLite 应用集成以及依次新建的 MySQL、PostgreSQL、MongoDB 容器测试。
  首轮全量回归曾因工作盘空间不足在复制运行库时失败；清理本轮副本并补上自动回收后，
  完整重跑通过。运行库副本可重新生成，源码、原始构建产物和诊断日志未删除。
- MongoDB 新增独立 schema/config、actor 调度和真实插件 Docker 测试。2026-09-04 的
  单节点副本集联调通过：字段投影、原生 BSON、部分更新/NULL/删除、整数边界、多集合
  提交/回滚、版本冲突、幂等与跨 partition 重用 ID 拒绝、同分区顺序和独立读写连接。
  服务器日志确认暂态事务错误及提交 writeConcernError 两个失败注入确实触发，重试未重复累加。
  程序自然退出，worker 完整回收，未发现 CRT 泄漏报告；临时容器及数据已清理。
- Mongo actor 强制退出用例在独立 actor_system/时钟自然销毁后检查弱引用，排除 CAF 1.1
  请求超时定时器的合法外部引用后检测自环；同分区队列、路由、限流、drain 和超时测试
  连续重复 5 次通过。上述结果不等于所有负载、故障和第三方驱动场景均绝无泄漏。
- 单元测试覆盖契约、SQL 方言、字段投影、patch、NULL/版本列校验、事务连接路由及关联键取消。
- `entity_store_plugin_e2e` 使用真实 SQLite、两组命名连接和每组两个 worker，覆盖
  建档、部分更新、幂等重放/冲突、同分区时序、乐观锁、多实体回滚、读写路由、NULL 和提前取消事务。
- 集成测试还检查自然退出、worker join 和 Windows CRT 泄漏报告。
- 2026-09-04 使用 Docker 中的 MySQL 8.0.46 / PostgreSQL 16.15 完成同一套真实端到端回归，
  每次仅一个新容器；覆盖 8 KiB 以上文本、中文、引号/反斜杠与长文本幂等重放。
- 后端边界回归覆盖 MySQL 参数数量不匹配后的连接复用，以及 PostgreSQL 失败事务 COMMIT
  必须报错且不落库、连接可复用、NUL 参数/SQL/实体键不能被静默截断。
- 新增 4 项独立测试：断线 worker、Schema 推断、EntityStore 幂等恢复、Schema 启动屏障。
  覆盖 COMMIT 已落库但回执丢失、整笔重试仍保持分区顺序、BEGIN 取消、重试次数/期限、
  元数据未就绪时禁止业务访问、字段白名单和强制退出后的对象释放。
- Docker 回归分别运行 manual/database 两种 Schema 模式，并实际终止数据库会话，验证
  重连、旧事务失效、迟到清理不影响新事务；SQLite 也运行两种模式。
  启用 Docker 后共注册 16 项 CTest。退出检查包含各 4 个 worker joined 和 CRT 泄漏报告；
  这些是当前场景的检测结果，不是对所有负载和第三方驱动生命周期的绝对无泄漏保证。
- 2026-09-04 修复 MySQL 在途 I/O 停止后，Debug 完整构建及统一 CTest 回归通过：16/16
  （136.79 秒），包含串行双数据库
  Docker 联调。MySQL 8.0.46 / PostgreSQL 16.15 均通过 manual/database 两轮，
  正常退出、worker 完整回收，未发现 CRT 泄漏报告，临时容器及连接配置已清理。
- 新增 socket 中断测试覆盖已阻塞读取、先停止后注册、注销与停止竞态和句柄关闭/复用隔离。
  该测试与连接池、断线 worker 测试各重复 5 次，15 次全部通过。
  真实 MySQL 中持续慢结果流、`SELECT SLEEP(8)`、`DO SLEEP(8)` 的停止及 join 分别为
  0、0、2 毫秒（毫秒精度）；旧慢结果流诊断约为 7315 毫秒。三例均确认会话最终关闭、
  另一连接不受影响；取消写入返回 `outcome_unknown`。这是当前环境实测，不是硬实时承诺。

## Docker 真实数据库回归（Windows）

构建后运行：

```powershell
cmake --build --preset windows-x64-debug --target caf_plugin_app entity_store_plugin mysql_plugin postgres_plugin test_mysql_cancellation --parallel 2
pwsh -NoProfile -File ./tests/run_entity_store_docker.ps1
```

脚本默认顺序为 MySQL → PostgreSQL，每次新建一个容器，完成并确认删除后才启动
下一个。它不复用或停止已有数据库容器。使用随机密码、仅绑定 127.0.0.1 的随机端口、
tmpfs 临时数据；结束后删除临时连接配置。需要 Docker Desktop Linux 容器模式、
PowerShell 7，以及本机已有 `mysql:8.0` 和 `postgres:16-alpine` 镜像。
可用 `-Backend mysql` / `-Backend postgres` 单独重跑，或用 `-MySqlImage` /
`-PostgresImage` 指定已下载的版本/摘要。脚本不会自动拉取或升级镜像。

每次运行保留独立日志目录：`out/build/windows-x64/tests/entity_store_docker_<后端>-Debug-<运行号>/`。
其中 `manual-stdout.log` / `manual-stderr.log` 和
`database-stdout.log` / `database-stderr.log` 分别保存两种 Schema 模式的应用输出，
`database.log` 保存数据库日志，
`docker-run.json` 记录镜像 ID、容器 ID 和运行时间；数据本身不保留。
每轮结束自动删除隔离运行目录中的 EXE/DLL/PDB 副本，保留日志与元数据；
原始构建产物不受影响，避免多次联调持续占用工作盘。

也可在 CMake 配置中启用 `CAF_ENABLE_DOCKER_TESTS=ON`，将串行的
`entity_store_docker` 加入 CTest；默认关闭，普通测试不依赖 Docker。

```powershell
cmake --preset windows-x64 -DCAF_ENABLE_DOCKER_TESTS=ON
ctest --test-dir out/build/windows-x64 -C Debug -R entity_store_docker --output-on-failure
```

每个容器内先跑 manual 模式建表，再启动新应用跑 database 模式（只配置表名，不写字段）。
四个单槽命名连接均指向这个临时数据库：reader/writer 验证路由，recovery 与
recovery_admin 用于主动终止测试会话。管理员凭据仅用于临时容器，随配置一起清理；
断线测试还验证旧事务、迟到回滚不会影响新事务，PostgreSQL COPY 拒绝后能恢复。
MySQL 容器内还运行 `test_mysql_cancellation`：持续慢结果流、等待首个结果和慢写入
均在运行期间停止，要求 worker 在 2 秒内回收、取消结果不误报成功，并检查 CRT 泄漏。
对应日志为 `cancellation-stdout.log` / `cancellation-stderr.log`，无需额外容器或生产测试后门。
不验证主从复制、副本延迟或跨主机故障切换。
强制杀死脚本进程或 Docker 引擎失联时，不能保证自动清理；可依据容器上的
`caf.test=entity-store` / `caf.test.run` 标签定位本轮测试容器，勿清理已有业务数据库。

### MongoDB Docker 回归

```powershell
cmake --build --preset windows-x64-debug --target test_mongo_entity_store --parallel 2
pwsh -NoProfile -File ./tests/run_mongo_entity_store_docker.ps1
```

需要本机已有 `mongo:7` 镜像。脚本每次新建一个单节点副本集，随机端口只绑定
127.0.0.1，使用与 SQL 脚本相同的互斥锁；不会复用或停止已有数据库容器。
测试执行后自然退出，收集日志，再删除本次容器；不验证跨节点复制延迟或真实选主故障切换。
启用 `CAF_ENABLE_DOCKER_TESTS` 后，CTest 同时注册 `mongo_entity_store_docker`。
每轮日志保存在 `out/build/windows-x64/tests/mongo_entity_store_docker-<配置>-<运行号>/`，
包含 `stdout.log`、`stderr.log`、数据库日志和镜像/容器 ID 元数据。强制终止脚本或
Docker 引擎失联可能阻断清理，应根据本轮 ID 与 `caf.test=entity-store-mongo` 标签确认后处理。
Mongo runner 从已有依赖目录加载第三方 DLL，并在结束时删除本轮运行库副本；
即使日志写入失败，也会尝试清理已确认归属的容器。

## 后端能力边界

- MySQL / PostgreSQL / SQLite：实体 SQL 适配层已实现字段投影、patch、乐观锁和单连接事务。
- MongoDB：已实现字段投影、patch、版本检查、持久化幂等和 session 多文档事务；
  需要副本集或支持事务的分片集群，字段 schema 目前手动配置。
- Redis：适合作缓存或通过 Lua 实现单 key 原子更新；不要把 MULTI/EXEC 的
  命令流伪装成跨实体强事务。

协议中的 `validate` 只做后端无关的结构校验，SQL 实现额外做 schema 白名单、类型、
可写字段、事务预算和幂等检查。租户隔离、业务授权与入口请求体大小限制仍由业务服务/网关负责。
