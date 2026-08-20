# 插件开发指南（CAF 1.1）

本文约定插件消息体系的机制、号段管理、权限分级和插件开发要点。
配套示例代码：`plugins/business/business_plugin.cpp`（私有类型与信封并存对照）。

## 1. 为什么消息类型需要注册（CAF 1.1 硬要求）

CAF 1.1 移除了旧的 `caf::atom` API，并改变了消息存储方式：消息是类型擦除的，
创建/拷贝/析构都通过**全局元对象表**（以 `type_id` 为下标的进程级数组，
位于 caf_core.dll，exe 与所有插件 DLL 共享）查找类型的函数指针。

推论：

- 消息中每个自定义类型都要有**编译期 `type_id`**（uint16）和**运行时元对象**；
- 只给 type_id 不注册元对象 → 消息析构时调用空指针 → 进程崩溃（exec at 0x0）；
- 元对象必须在**构造任何 actor_system 之前**注册，之后注册是未定义行为
  （CAF 头文件明示）；重复注册同一段会直接 abort。

## 2. 号段划分与权限分级

type_id 是全局唯一的 uint16 空间，所有消息共用，靠分段管理（CAF 支持空洞）：

```
0 ~ 199        CAF 保留（core/io/net 等内置模块）
200 ~ 999      内核/框架协议类型（include/common/message_tags.def，单一数据源）
1000 ~ 4999    插件私有·系统级段（受信组件间通信）
5000 ~ 65535   插件私有·用户级段（可要求更高权限，见 §6）
```

信封 `plugin_envelope.sub_proto` 遵循同样的分级约定：**最高位（0x8000）置 1
表示用户级**，0x0000~0x7FFF 为系统级。

冲突防线：模块内撞 ID → 编译期 `type_by_id` 重定义报错；
插件间撞段 → 启动时注册 abort（启动即炸，不会带病运行）。

## 3. 三套消息机制对比

| | ① 公共协议标签 | ② 插件私有类型 + 自注册 | ③ 公共信封 plugin_envelope |
|---|---|---|---|
| 定义位置 | `message_tags.def` | 插件自己 | 内核（ID 231，只此一个） |
| 占用 ID | 内核段一个/条 | 私有段若干（按级别选段） | 不额外占 |
| 类型匹配 | 精确（handler 按类型） | 精确 | 无（统一入口 + switch 子协议号） |
| 载荷 | 任意已注册类型 | 任意已注册类型 | 字节流，自己序列化 |
| 新增成本 | 改 def 一行，重编内核 | 插件自带，不动内核 | 零（子协议号插件自管） |
| 权限分级 | 内核管理 | 系统级段 / 用户级段 | sub_proto 高位标记 |
| 适用 | 内核/框架契约 | 插件内部结构化消息 | 松散耦合、结构简单的消息 |

②③ 的并存对照示例见 `business_plugin.cpp`（`biz_ping_atom` vs `biz_env_hello` 信封）。
信封的完整利弊注释见 `include/common/plugin_envelope.hpp` 头注释。

## 4. 机制一：内核公共协议（message_tags.def）

X-macro 单一数据源：`message_tags.hpp` 展开它生成标签结构体 + type_id 特化；
`message_meta.hpp` 展开同一份生成元对象注册表。两处物理上不可能不同步。

维护规则：

1. **只追加**，新条目放表尾，ID = 当前最大 ID + 1；禁止插入中间、禁止复用；
2. 顺序与 ID 的一致性由 `message_meta.hpp` 里的 `static_assert` 编译期强制；
3. 条目两种形状：
   - `X_TAG(名字, ID)` —— 定义空标签结构体并注册；
   - `X_REG(键名, (已有类型), ID, "显示名")` —— 只注册已有类型（枚举/STL 容器/信封）。

不要用 CAF 的 `CAF_ADD_TYPE_ID` 顺序块替代显式 ID：它按 `__COUNTER__` 分配，
ID 依赖各翻译单元的包含顺序，exe 与各插件 DLL 分别编译时可能不一致，
跨 DLL 消息会错位（编译期无报错，运行时崩）。

## 5. 机制二：插件私有类型（自注册，分两级）

插件定义自己的消息类型时，**先定级别，再选号段**：

```cpp
// 系统级：插件内部 actor 之间、插件与内核之间的受信通信（段 1000~4999）
CAF_MESSAGE_TAG(biz_ping_atom, 1000)

// 用户级：消息源头可追溯到最终用户/外部输入（段 5000~65535，见 §6 权限规则）
CAF_MESSAGE_TAG(biz_user_cmd_atom, 5000)

extern "C" PLUGIN_API void register_meta_objects() {
    static const caf::detail::meta_object xs[] = {
        caf::detail::make_meta_object<biz_ping_atom>("biz_ping_atom"),
        caf::detail::make_meta_object<biz_user_cmd_atom>("biz_user_cmd_atom"),
    };
    caf::detail::set_global_meta_objects(1000, caf::make_span(xs));   // 注意分段注册
}
```

多个不相邻段就分多次调用 `set_global_meta_objects`（CAF 支持空洞）。
框架在 `main()` 里、构造 actor_system 之前扫描 `plugins/` 目录并调用该导出
（`preregister_plugin_meta`）。注意：

- 带此导出的 DLL 会**常驻进程**（元对象函数指针指向 DLL 代码段，卸载即崩）；
- 没有私有消息类型的插件不用实现这个导出；
- 段冲突在启动时 abort，换一个不重叠的段即可。

## 6. 权限分级模型：系统级 vs 用户级

### 定义

- **系统级私有协议**：发送方全部是加载时经过依赖解析的受信组件
  （内核 actor、其他插件的内部 actor）。默认信任，不额外校验。
- **用户级私有协议**：消息的触发源可以到达最终用户或外部输入——
  例如 HTTP/命令行入口、用户命令字符串、配置文件、网络报文。
  可以单独要求更高的权限。

### 判定规则

1. 消息的触发源能到达用户/外部输入 ⇒ 必须划为**用户级**；
2. 有危险副作用（shutdown、unload、写配置、删数据）的操作 ⇒ 只允许出现在
   系统级路径，或经过校验的用户级路径；
3. 拿不准就按用户级处理（更严不会错）。

### 反面教材（本项目现状）

`business_plugin.cpp` 的 `(const std::string& cmd)` handler：任何能拿到
`business_service` 代理的发送方，发字符串 `"shutdown"` 就会触发
`request_shutdown_atom` 直达关机总管。这是典型的"用户级输入触发系统级
副作用"，按本模型应在执行前校验调用方权限。

### 当前实现状态

**代理 ACL 已实现**：插件在 `manifest().acl_allow` 里声明可信插件名单后，
它提供的所有服务进入受限策略——服务代理（registry 为每个服务生成的 proxy，
所有调用的必经入口）在转发前检查 `current_sender()` 是否属于白名单插件的
actor；不在则拦截：带 promise 的 request 收到 `invalid_request` 错误响应，
纯 send 被丢弃。示例：`business_plugin.cpp` 的 manifest 把
`business_service` 限为仅 LoggerPlugin 可调；main.cpp 在 `test-auto-shutdown`
模式下有一个 ACL 自测——以未受信身份请求 `"shutdown"`，日志应出现
`[ACL test] blocked as expected`。

限制与边界（实话）：

- 白名单粒度是**插件级**（按插件名解析成 actor 地址），暂不支持按消息类型
  或按服务分别配置；
- 匿名消息（`anon_send`，sender 地址为空）在受限策略下一律拦截；
- 注册到 ACL 生效之间有个开放窗口（两条消息 FIFO 相继处理，仅启动期存在）；
- ACL 只管**经过服务代理**的调用；内核 lifecycle 消息（drain/save/shutdown）
  由 GracefulShutdown 直达插件 actor，不受 ACL 影响（也不需要）。

后续可选增强（未实现）：**capability token**——内核在加载时向各插件签发
令牌，用户级 handler 校验消息中携带的令牌。适合代理覆盖不到的
actor 直连路径。

## 7. 机制三：公共信封（plugin_envelope）

```cpp
struct plugin_envelope {
    std::uint16_t sub_proto;            // 子协议号：插件自管，跨插件可重复；
                                        // 高位 0x8000 置 1 表示用户级（§6）
    std::vector<std::byte> payload;     // 自己序列化
};
```

发送方编码 payload → 发信封；接收方在统一 handler 里 `switch (env.sub_proto)`
分发并解码。零注册、零协调；代价是失去类型匹配和编译期接口检查。
用户级信封消息同样适用 §6 的校验责任。

## 8. 热更新（reload_atom，旁路加载）

不重启进程把某个插件换成新版本：

```cpp
self->request(plugin_mgr, caf::infinite, reload_atom{}, name, 新DLL路径);
```

`PluginManager` 的流程（`plugin_manager.cpp` reload handler，**先排空后快照**）：

1. 准备：从**新路径**旁路加载新 DLL（manifest.name 必须与原名一致）、解析依赖；
2. **quiesce 静默**：对该插件每个服务的代理发 `quiesce_atom` 并等 ack——
   代理邮箱 FIFO 保证 ack 之前到达的调用都已委托给旧 actor；此后代理
   进入静默态，新调用进**缓冲**不再转发（任一代理静默失败则把已静默的
   resume 回旧实现，整体回滚）；
3. **快照**：对旧 actor `request save_state`——邮箱到达序屏障：响应到达时
   旧 actor 已处理完全部在途工作，且不会再有新工作，**快照即终态**；
4. spawn 新 actor → `restore_state` → monitor → `init_atom`；
5. registry 台账（`hot_reload_atom`：impl 引用 + 版本号）→ 对每个代理发
   `resume_atom`：切到新实现并**冲刷缓冲**（按缓冲时记录的原始 sender
   复查 ACL；缓冲的 request 通过 `response_promise.delegate` 按原
   sender/mid 路由交付响应，调用方无感）；
6. 旧实例退役：邮箱已空，2s 后收 `shutdown_atom` 退出，PluginManager 经
   down_msg 销毁旧 C++ 对象；**旧 DLL 常驻句柄池不卸载**（actor vtable 在
   DLL 代码段，CAF 异步释放引用，FreeLibrary 过早会崩——句柄池与热更新
   不冲突，恰恰是热更新的前提）。

### 状态一致性（快照为什么不丢数据）

热更新的经典难题是：快照之后、旧实现完全停下之前，旧实现处理的增量
修改会丢（"以哪方为准都有问题"）。本实现用**消息顺序**消除该窗口，
不需要锁：

- quiesce 的 ack 是代理侧屏障：ack 之前的调用都已离开代理；
- save_state 的响应是旧 actor 侧屏障：响应之前其邮箱已排空；
- 两个屏障都靠 CAF 邮箱的到达序天然成立，快照点之后旧实现处于
  静默态，不存在"快照后被修改"的可能。

代价与边界（实话）：

- 静默期间服务调用被**缓冲**而非拒绝，冲刷后由新实现处理——表现为
  一次 reload 时长（毫秒级）的延迟毛刺；
- 冲刷的**纯 send** 在新实现看到的 sender 是代理/PM 而非原始调用方
  （request 不受影响，promise 保存原始路由）；业务 handler 若依赖
  sender 身份需注意；
- 绕过代理直连插件 actor 的调用不受 quiesce 约束（lifecycle 消息同理），
  调用方需自行避开 reload 窗口。

### 为什么必须新路径

- Windows 对已加载 DLL 持文件锁，原路径无法覆盖写入；
- `LoadLibrary` 对同路径直接返回缓存的旧模块句柄，拿不到新代码。

因此更新包放 `./updates/` 等新位置（版本化文件名更佳）。演示目标
`business_plugin_v2`：同一份源码加 `BIZ_HOT_V2` 编出 v2，main.cpp 的
`test-auto-shutdown` 链路有完整自测，日志可见
`[Registry] Hot-reloaded: business_service (v2)` 与
`BusinessPlugin initialized (2.1.0-hot)`。

### 热更新能改什么 / 不能改什么

| | 内容 | 说明 |
|---|---|---|
| ✅ | handler 业务逻辑 | v2 演示：string handler 返回 `processed by v2: ...` |
| ✅ | 内部状态结构 | 只要 save/restore 字节格式跨版本兼容（移交是直接内存拷贝） |
| ✅ | 对**已注册类型**新增 handler | 类型系统没动，行为随便改 |
| ✅ | 同名服务实现切换 | 代理 quiesce/resume 静默切换 + 缓冲冲刷，在途请求不丢；ACL 白名单记的是调用方，热切换后原样保留 |
| ❌ | **新增未注册的 type_id（协议号）** | 元对象必须在 actor_system 构造前注册；热更新路径调新 DLL 的 `register_meta_objects` 会因重复注册同一段直接 abort |
| ❌ | C ABI / PluginEntry 接口 | create/destroy/manifest/spawn 签名必须兼容 |
| ❌ | manifest.name | reload 按名匹配 |

**想加新协议号的两条路**：

1. **走信封 `sub_proto`（推荐）**：不需要新 type_id，热更新随便加——
   v2 演示了热添加 `sub_proto=2`（`biz_env_v2_ping`）；
2. **v1 启动时预留注册**：先把类型注册进元对象表，handler 后续版本再上线。

都不行就只能全量重启。

## 9. 插件最小骨架与生命周期

- 导出 `create_plugin()` / `destroy_plugin()`（必需）、`register_meta_objects()`（可选）；
- `PluginEntry::manifest()` 声明 name/version/dependencies/provides，
  框架据此做依赖解析与拓扑排序加载；可选 `acl_allow` 声明可信调用方
  插件名单（声明后本插件的服务进入代理 ACL 受限策略，见 §6）；
- `PluginEntry::spawn()` 创建插件 actor，需实现的生命周期 handler：
  `init_atom`（初始化）→ `drain_atom`（排空，回执 `(drain_atom, self->address())`）
  → `save_state_atom`（返回 `std::vector<std::byte>`）/ `restore_state_atom`
  → `shutdown_atom`（`self->quit()`）；
- 状态由内核的 CheckpointManager 落盘（CRC32 + 原子 rename），下次启动自动恢复。

## 10. CAF 1.1 行为变化备忘（踩过的坑）

- **意外消息会杀 actor**：默认 `print_and_drop` 产生 error 结果并触发 quit。
  actor 必须为每种可能收到的消息写 handler，长期驻留的宿主 actor
  （如 PluginManager）应设兜底 default_handler；
- **`send` 调用有返回值的 handler，返回值会作为普通消息回弹给 sender**。
  被 send（非 request）调用的 handler 应返回 void，失败本地记日志；
- `event_based_actor` 里没有 `request().receive()`（blocking-only），
  用 `.then()` 续接或 scoped_actor；
- 手写 main 必须显式调用 `caf::core::init_global_meta_objects()` +
  `app_meta::init()`（内核元对象）+ `preregister_plugin_meta()`（插件元对象）；
- 日志宏改名（`CAF_LOG_WARN` → `CAF_LOG_WARNING`），
  配置键为 `caf.logger.*`；actor_system 构造前 CAF logger 不存在，
  该阶段的诊断输出请用 std::cout；
- **消息类型必须与接收方 handler 精确匹配**：代理 drain 旧实现时曾误发
  `self->address()`（`actor_addr`），而插件 drain handler 匹配
  `(drain_atom, caf::actor)`——类型不符 → 意外消息 → 旧 actor 被杀而非
  正常排空。应发 `caf::actor_cast<caf::actor>(self)` 的句柄。
  （现热更新已改走 §8 的 quiesce/resume 静默切换，不再 drain 旧实现；
  drain 协议仍服务于 GracefulShutdown 的优雅关机路径）；
- **暂存 request 的正确姿势（代理缓冲的实现要点）**：default_handler 里
  `make_response_promise()` 取走承诺 + 返回 `delegated<message>`（承诺
  存着原 sender/mid，CAF 不会自动响应也不会断约）；冲刷时用
  `promise.delegate(target, msg)` 转发——对单个 `caf::message` 参数
  原样透传不二次包装，响应直达原调用方。纯 send 无承诺，普通转发即可；
- **FreeLibrary 过早会崩（0xC0000005）**：actor 的 vtable/lambda 和元对象的
  destroy/copy 函数指针都在 DLL 代码段，而 CAF 的引用释放是异步的——
  卸载 DLL 后，迟到的清理会跳到已卸载代码。因此 `meta_lib_pool`
  （注册过元对象的 DLL）与 `plugin_lib_pool`（已加载插件的 DLL）都常驻到
  进程退出，由 OS 回收；
- **两个池 ≠ 每个 DLL 加载两次**：同一插件 DLL 实际被 `DynamicLibrary::open`
  三次——`preregister_plugin_meta`（有 `register_meta_objects` 导出才留进
  meta 池，没有则立即释放）、`probe_plugin`（读 manifest，读完即放）、
  `load_atom`/`reload_atom`（留进 plugin 池）。但 `LoadLibrary`/`dlopen`
  按模块引用计数：同路径只映射一次、`DllMain` 只跑一次，后续 open 只是
  同一 HMODULE 计数 +1，没有双份内存。两个池语义正交（元对象函数指针 vs
  actor 代码）；热更新的 v2 DLL 在前两个阶段扫不到（只扫 ./plugins），
  仅 reload 时进 plugin 池一次；
- **actor 退出 ≠ 可以卸载 DLL**：down_msg 只表示 actor 终止运行；actor 实现
  对象是侵入式引用计数的，析构发生在最后一个 `caf::actor` 句柄释放时——
  registry 的 `entry.impl`、代理的 `current`、PM 的 `plugins_`/`retired_`
  全是强引用，CAF 不提供"最后一根引用已释放"的通知，FreeLibrary 后迟到的
  析构会在某个 worker 线程跳进已卸载代码（0xC0000005）。元对象表同样不可
  按插件注销：`set_global_meta_objects` 对已有段直接 abort、actor_system
  构造后调用是 UB；唯一的 `clear_global_meta_objects` 官方标注
  "intended for unit testing only!"（全表清空）。注册过私有类型的 DLL
  一旦注册，其函数指针永久留在 caf_core 的全局表里，任何滞留消息的析构
  都要经过它——**这类 DLL 永远不能卸载**；
- **回收粒度的结论**：到 C++ 实例级为止（down_msg → `destroy_plugin`，
  §8 的 retired 路径即"检测退出后回收"），DLL 映射常驻。若未来有卸载刚需
  （高频热更的长驻服务），两条路：只对无 meta 导出、仅用已注册类型的插件
  开放 unload，配 drain → down_msg → 全链路句柄清零 → quarantine 延迟卸载
  协议（漏一处句柄就是远期崩溃）；或进程隔离插件宿主（杀进程即卸载，
  OS 兜底）。
