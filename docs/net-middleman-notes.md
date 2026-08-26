# CAF 网络模块选型：io::middleman vs net::middleman（2026-08-25 结论）

> 结论来源：CAF 1.1（vcpkg 安装）头文件 + caf_net.dll 导入表实测。
> 一句话：**io::middleman 是分布式 actor 通信协议层（BASP），net::middleman
> 是底层网络事件后端（multiplexer）——两者正交，不能互相取代。**

## 1. API 对比（头文件实锤）

`caf::actor_system` 有两个不同的访问器：

| 访问器 | 返回类型 | 模块 |
|---|---|---|
| `sys.middleman()` | `caf::io::middleman&` | I/O 模块（BASP） |
| `sys.network_manager()` | `caf::net::middleman&` | 网络模块 |

### caf::io::middleman（io/middleman.hpp）

分布式 actor 通信 API，全部依赖 BASP（Binary Actor System Protocol，
io/basp/ 目录：header / routing_table / instance / worker ...）：

- `open(port, ...)` — 开监听端口
- `connect(host, port)` — 连接远端节点，返回 `expected<node_id>`
- `remote_lookup(name, nid)` — 按名查远端 actor（返回 strong_actor_ptr）
- `publish(whom, port)` — 发布 actor 到端口
- `remote_actor(host, port)` — 连远端 actor

### caf::net::middleman（net/middleman.hpp）

全部 public API：

- `init_global_meta_objects()`
- `start()` / `stop()` / `init(cfg)`
- `mpx()` — 返回 multiplexer 引用

**没有 connect / remote_lookup / open / publish。** 它只是事件循环后端
（multiplexer 线程），给 net 层的 http / web_socket / lp / octet_stream /
ssl 协议栈提供底层 I/O 多路复用。没有"节点"概念，没有分布式 actor 寻址。

## 2. 本项目集群依赖 io::middleman（缺它集群跑不起来）

| 位置 | 调用 |
|---|---|
| cluster/bootstrap.cpp:74 | `sys.middleman().open(port, ...)` |
| cluster/client.cpp:29 | `system().middleman().connect(host, port)` |
| cluster/client.cpp:37 | `system().middleman().remote_lookup(name, nid)` |
| cluster/remote_caller.cpp | `middleman().connect / remote_lookup` |
| cluster/ops_actor.cpp | `middleman().connect / remote_lookup` |

全是 io::middleman 独有的 BASP API。net::middleman 一个都没有。

### 为什么 CAF_MAIN() 不写模块参数

middleman 是**运行时按需加载**（`cluster::init_node_io()`：
`init_global_meta_objects()` + `cfg.load<caf::io::middleman>()`，在
app_config 构造函数里、actor_system 构造前）。纯插件进程不配
node-kind 就不加载 middleman，省掉网络模块开销。`CAF_MAIN(caf::io::middleman)`
会强制所有进程加载，没必要。

## 3. net::middleman 的连接数上限（实测证据）

- **默认 64 并发连接**：`caf/defaults.hpp` `defaults::net::max_connections
  = 64`，注释明确"达到上限后 acceptor 停止接受新连接，直到有连接关闭"。
  可调大（dsl server_factory 的 `max_connections(n)`），但设计意图不是 C10K。
- **单线程事件循环**：net/middleman.hpp 里 `std::thread mpx_thread_` 一个
  线程跑整个 multiplexer。
- **平台后端**（caf_net.dll 导入表实测）：
  - Windows：**WSAPoll**（不是 IOCP，没有 GetQueuedCompletionStatus）
  - Linux：epoll（标准实现）
- 连接模型：每连接一个 `socket_manager` + 协议层对象，事件驱动非阻塞；
  actor_shell 模式下每连接一个 actor。
- 实际能力：调大 max_connections 后单线程 epoll/WSAPoll 撑几千连接没问题，
  吞吐受单核限制。CAF 官方定位是"协议栈运行平台"而非高并发网关。

## 4. 选型建议（本项目场景）

| 场景 | 选型 | 理由 |
|---|---|---|
| CAF 节点间通信（master/worker 集群） | **io::middleman（BASP）** | node_id、remote_lookup、跨节点消息路由全在 BASP |
| bridge sidecar（外部语言节点） | 现有自定义 TCP 行协议 | 连接数 = 外部节点数（个位数），无需换 |
| 中小规模 TCP 服务（<几千连接） | net::middleman + octet_stream/lp | 与 CAF actor 无缝集成，调大 max_connections |
| 高并发网关（上万连接 / 精细控制） | asio 或独立网关进程 | net::middleman 单线程事件循环是瓶颈，CAF 1.1 无多线程 multiplexer |

### 自研协议栈 vs asio 的代码量

用 net::middleman 做自定义二进制协议服务器，需要实现
`generic_upper_layer` 子类（接收字节流回调）——代码量与 asio 直接写
差不多，但好处是与 CAF actor 无缝集成（actor_shell 每连接一个 actor，
消息直接进 actor 邮箱）；坏处是绑死 CAF 的单线程事件循环。
asio 更通用，但要自己做 actor 系统 ↔ asio 回调的桥接。

## 5. 结论

- net::middleman 不能取代 io::middleman：集群代码强依赖
  connect / remote_lookup / open，缺了 BASP 集群就跑不起来。
- 想用 net 层做集群 = 从 TCP socket 自己实现一套 BASP 级别的协议
  （节点 ID、注册表同步、消息路由、序列化）——等于重新造 io::middleman。
- bridge sidecar 已部分走这条路（自定义行协议），但只用于外部语言节点，
  CAF 节点之间始终走 io::middleman 的 BASP。
