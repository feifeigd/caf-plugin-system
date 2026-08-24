# CRT 泄露检测深潜（Debug CRT 三层方法 + 时序坑）

2026-08-24 实战沉淀（用户质疑"打印一大堆=内存泄漏"后逐步打磨出的完整方案）。

## MSVC 退出时序（一切判读的前提）

```
main 返回
  → exit() → 静态对象析构（含 spdlog registry、DLL 静态对象）
  → atexit：_CRTDBG_LEAK_CHECK_DF 自动 dump
```
- **atexit 自动 dump = "静态析构后"的干净数字**（207 块/19.8KB 恒定值就是这时点）。
- **main 体内手动 _CrtDumpMemoryLeaks() = "静态析构前"**，会把静态初始化分配算进来
  （实测一个 104KB 大块，块号 {249}=main 之前分配）→ 看起来"泄露一大堆"，
  **不是泄露判据**。
- 静态对象析构先于 atexit dump（标准 [basic.start.term]），所以 spdlog logger
  （registry 静态持有）在 atexit dump 时可能已释放——"business" 40B 块是
  registry 之外的持有物（actor 数据、addr_names 等）。
- **退出路径决定 dump 内容（2026-08-24 实测）**：正常 `main return` → 静态析构
  执行（CAF 的 meta 注册表清理守卫析构 → `delete[]`）→ dump 干净；`ExitProcess(0)`
  → 跳过静态析构 → 清理守卫不析构 → meta 注册表 11648B 残留被 dump 报"leak"。
  **dump 里看到 11648B 大块 ≠ CAF 泄露，是 ExitProcess 跳过了 CAF 自带的清理**。
- **"无 dump 输出"的诊断特征（2026-08-24）**：运行输出只有 `[LeakCheck]` 行、
  没有 "Detected memory leaks" = **`_CrtSetDbgFlag` 没执行**（LEAK_CHECK_DF 未设置）
  ——最常见原因：caf_main 没跑（exec_main 被注释/`#ifdef _DEBUG` 块内被误放
  `return 0;`）。先查代码路径真的走到 CRT init 了吗，再怀疑泄露。

## 三层方法（由粗到精）

1. **全量 dump**：`_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF|_CRTDBG_LEAK_CHECK_DF)` +
   `_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE/_CRTDBG_FILE_STDERR)`。
   判读用对照：普通版 vs 冒烟全量版字节数一致 = 常驻非泄露（见 SKILL.md 泄露测试条目）。
2. **checkpoint diff（推荐判据）**：`_CrtMemState before,after,diff;`
   `_CrtMemCheckpoint(&before)`（exec_main 前）→ exec_main → `_CrtMemCheckpoint(&after)` →
   `_CrtMemDifference(&diff,&before,&after)` + `_CrtMemDumpStatistics(&diff)`。
   diff = 运行期净分配，排除启动期静态（104KB 块）。实测普通版与冒烟版
   逐字节一致（243 块/115392B + 85 CRT 块/20568B）→ 无运行期泄露。
3. ~~MAP_ALLOC 行号~~ **已证伪（2026-08-24 实测）**：本工具链（VS2022 14.44 +
   UCRT 22621）`_CRTDBG_MAP_ALLOC` **对 C++ `new` 不生效**——现代 crtdbg.h
   （在 Windows Kits ucrt：`/mnt/c/Program Files (x86)/Windows Kits/10/Include/<ver>/ucrt/crtdbg.h`，
   **不在 VC include 目录**）只映射 malloc/free（L287-330），全 include 树无
   `#define new`（微软移除了 new 宏机制；vcruntime_new_debug.h 只有需显式调用的
   placement 重载 `new (_NORMAL_BLOCK,__FILE__,__LINE__) T`）。**实证**：编译命令行
   `/D _CRTDBG_MAP_ALLOC` + `/FIcrtdbg.h` + `/MDd` 全正确（`touch + cmake --build
   --verbose | grep cl.exe` 抓完整命令行验证，/D /FI 直接内联、无 rsp），报告仍
   0 个 `allocation site` 行；故意 `new char[64]` 泄露也无行号。**判死依据**：
   `grep -rn "define new" <MSVC>/include/` 只命中 xkeycheck.h 的故意报错宏。
   分配点定位替代方案：块内容分析（Data 十六进制里的可打印串/指针形态——
   "business" 40B = spdlog logger name，大量 16B+指针 = 注册表节点）或 UMDH
   调用栈捕获（需符号）。

## _CRTDBG_MAP_ALLOC 排查记录（全配置对也不生效，路线已弃）

2026-08-24 实测：`add_compile_definitions($<$<CONFIG:Debug>:_CRTDBG_MAP_ALLOC>)`
+ `add_compile_options($<$<CONFIG:Debug>:/FIcrtdbg.h>)` 双管齐下（vcxproj 的
PreprocessorDefinitions / ForcedIncludeFiles 均有，`grep` 可验证 ≥2），全量重编后
报告仍 0 行号——**根因是工具链移除了 new 宏映射，不是配置问题**（见三层方法③）。
经验：
- VS 多配置生成器没有 CMAKE_BUILD_TYPE，`if(CMAKE_BUILD_TYPE STREQUAL "Debug")` 永远为假，
  必须用 CONFIG generator expression。
- 映射逻辑在 `<crtdbg.h>` 里 → 只有显式 include 的编译单元生效（main.cpp），core/plugins
  无行号 → 需 `/FI` 强制包含。
- 验证宏是否真生效的实证法：故意 `new char[64]` 泄露一个块，报告若带本行行号 = 映射活了；
  不带 = 工具链不支持（本例）。
- `/FIcrtdbg.h` 触发全量重编 300s+，试错成本高——先跑实证法再决定要不要全量。

## 自定义 main 替代 CAF_MAIN（Debug 下插 actor_system 析构后钩子）

CAF_MAIN() 宏把 main 包死；actor_system 是 exec_main 的栈对象，**exec_main 返回
= actor_system 已析构（join 全部 actor）**——这是唯一安全的自定义钩子点：

```cpp
int main(int argc, char** argv) {
    int rc = EXIT_SUCCESS;
    {
        auto host_init_guard = caf::detail::do_init_host_system(caf::type_list<>{}, caf::type_list<>{});
        caf::exec_main_init_meta_objects<>();
        caf::core::init_global_meta_objects();
        rc = caf::exec_main<>(caf_main, argc, argv);   // 返回后 actor_system 已析构
        // ... checkpoint diff / 手动 dump / unload DLL 池都放这里 ...
    }
    return rc;
}
```
Release 分支保留 `CAF_MAIN()`。exec_main 支持 int 返回值传播
（exec_main.hpp L92-97 `if constexpr (std::is_convertible_v<result_type, int>)`）。

## unload DLL 池（~~unload_all_meta_libs / unload_all_plugin_libs~~ —— 已删除，2026-08-24）

**该实验路线已作废**：两个 unload 函数（plugin_loader/plugin_manager 各一）已作为死代码删除
（CAF_MAIN 版 main.cpp 从不调用；调用点只剩 include 注释）。删除后实测 0 泄露——
**函数局部 static（池本身）存储不在堆上（.data/.bss），CRT dump 永不统计；池内部堆缓冲
（deque 桶）正常 return 时由 static 析构器释放**。即"静态区常驻 ≠ 泄露"。历史结论保留：
- **时机**：必须 exec_main 返回后（actor 全 join）。caf_main 里调用
  → 插件 actor vtable 在 DLL 代码段 → 0xC0000005。
- **unload 后必须 ExitProcess(0)**（~~"必须"已被 clear 方案推翻，2026-08-24~~）：跳过静态析构阶段，否则 CAF 全局 meta
  注册表析构调用已卸载 DLL 的函数指针 → 崩。**准确表述**：不 clear meta 时 unload 才必须
  ExitProcess；`clear_global_meta_objects()` 之后 unload + 正常 return 实测无 0xC0000005
  （见下方替代方案节），且保留静态析构 + dump 完整触发，是更干净的选择。
- **ExitProcess 前必须 flush**：重定向到文件时 stdout 全缓冲，ExitProcess
  不跑 CRT 清理 → spdlog 日志全丢（症状：进程"没跑"其实跑了）。
  ```cpp
  std::cout.flush(); fflush(stdout); ExitProcess(0);
  ```
- ~~卸载后 DLL detach 完成，自动 dump 输出干净数字（实测 216 块/20.3KB ≈ 基线）~~
  **这条记录自相矛盾，不可信（2026-08-24 复核）**：`_CRTDBG_LEAK_CHECK_DF` 的自动 dump
  是 atexit 注册的，ExitProcess 不跑 atexit → dump 不可能触发。当时的"216 块干净数字"
  大概率来自非纯 ExitProcess 路径（ExitProcess 前手动 dump），记录未写明。**ExitProcess
  语义边界**：不执行进程级 CRT 清理链（atexit/stdio flush/静态析构），但**会**通知已加载
  DLL 的 DLL_PROCESS_DETACH（DLL 侧 detach 处理会跑）——本项目动态 CRT（VS2022+UCRT 22621）
  实测 stdout 缓冲仍丢，以实测为准。

## running() 不是 0 的判读

`sys.registry().running()` 在 wait_for_shutdown 返回时拍的是"send_exit 异步
排空"的瞬时快照（7↔9 抖动，非累积）。验证归零：打印前
`std::this_thread::sleep_for(std::chrono::milliseconds(200))` → remaining=0。
硬证据：actor_system 析构阻塞 join 全部 actor，秒退 + EXIT=0 = 全死光。

## CAF meta 注册表 11648B 解剖（init_global_meta_objects 的"泄露"真相）

2026-08-24 源码级验证（用户问"init_global_meta_objects 为什么泄露一块 11648B"）。
**112 × 104B = 11648B 完美整除**——就是全局 meta 注册表数组本身：

- **meta_object 结构**（`caf/detail/meta_object.hpp`）= **104B**：`std::string_view
  type_name` 16 + `size_t padded_size` 8 + `size_t simple_size` 8 + **9 个函数指针**
  （destroy/default_construct/copy_construct/move_construct/save_binary/load_binary/
  save/load/stringify）72 = 104B。
- **分配路径**：`caf::core::init_global_meta_objects()`（`libcaf_core/caf/
  init_global_meta_objects.cpp`，就一行）→ `caf::init_global_meta_objects<id_block::core_module>()`
  → 栈上构造 112 个 meta_object → `set_global_meta_objects` → `resize_global_meta_objects`
  → `new meta_object[112]`（`libcaf_core/caf/detail/meta_object.cpp`）→ 全局裸指针
  `meta_objects` + `meta_objects_size`。112 个 type_id = CAF core + 项目注册的类型总数。
- **CAF 自带清理（关键）**：meta_object.cpp 里静态 `meta_objects_cleanup : ref_counted`
  守卫（`cleanup_helper` 全局对象），**析构时 `delete[] meta_objects`**——正常退出路径
  会释放这块。所以：
  - **正常 `return`** → cleanup_helper 静态析构 → delete[] → dump 无此块（实测：
    exec_main 注释 + 正常 return → 自动 dump 0 块）
  - **ExitProcess(0)** → 跳过静态析构 → 残留 → dump 报 11648B "leak"——**这是
    ExitProcess 防 DLL 崩溃的代价，不是 CAF 泄露**
- checkpoint diff 显示它只是因为快照时机（exec_main 后、静态析构前）。

**"注释 exec_main 隔离静态 vs 运行期分配"调试法（2026-08-24）**：把
`rc = caf::exec_main<>(...)` 注释掉（只留 host_init + meta 初始化）跑一遍——
如果自动 dump 干净（0 块）= 那些 200+ 块全是 **exec_main 运行期创建的进程级单例**
（actor_system/CAF 注册表/spdlog logger/插件），不是 main 之前的静态初始化；
对比完整跑的数字即可回答"这些块哪来的"。注意：注释 exec_main 后 caf_main 不跑，
`_CrtSetDbgFlag` 也在 caf_main 里 → 想触发 dump 必须把 CRT init 挪到自定义 main。

### clear_global_meta_objects() = ExitProcess(0) 的替代（2026-08-24 实测）

自定义 main 里 exec_main 返回后调 `caf::detail::clear_global_meta_objects()`：

- **提前释放 meta 注册表数组**：dump 里 11648B 块消失（实测最大块只剩 ~2KB），
  数组被 delete 且指针置空 → 正常 return 后静态清理守卫对 nullptr delete[] 安全。
- **替代 ExitProcess(0)**：clear 之后 `unload_all_meta_libs/unload_all_plugin_libs`
  + 正常 return 共存，两次对照 EXIT=0 无 0xC0000005（skill 里"unload 后必须
  ExitProcess"的旧结论在这条路径上被推翻）——比 ExitProcess 干净：保留静态析构
  阶段 + CRT 自动 dump 完整触发。
- 前提：exec_main 返回后（actor_system 已析构、meta 不再被查询）才安全。
- 效果：dump 基线从 207 块/19858B → 205 块/19833B（meta 注册表提前释放，
  不是"泄露变少"）。
- 当前 main.cpp 组合（用户版）：exec_main → clear_global_meta_objects → unload
  两池 → 正常 return（ExitProcess 注释掉）。实测可复现。

### 对照实验新基线（clear 版本，2026-08-24）

`--test-quit`（最小活动）205 块/19833B vs 冒烟全量 207 块/20081B：
+2 块/+248B（一个 224B + 一个 24B，v2 热更新加载的常驻记录），
几百次请求/ACL/热更新只产生固定 +2 = 与活动量无关的又一实证。
最大块 1999/1871/1743/1679/1615B（CAF 注册表节点），40B×11（spdlog logger name）。

## 常量泄露（强引用环）——活动量对照测不出来（2026-08-24 方法论修正，重要）

**旧结论（已推翻）**："普通版 vs 冒烟全量版字节数一致 = 常驻非泄露"。这只对了一半——
**活动量对照只能排除"随负载增长的泄露"；常量泄露（固定大小）与活动量零相关，对照必然测不出来**。

**实证**：205 块/19.8KB 的"基线"里大部分其实是 shutdown_mgr 强引用环
（GracefulShutdown.plugin_mgr_ ↔ PluginManager.shutdown_mgr_；cluster_ctls_ 注册的
ops/master 回存 shutdown_mgr；静态 shutdown_manager_ref() 永不清除），破环 + 隔离后
dump 暴跌到 **1 块/8B**——之前的"206 块/19857B = 进程生命周期静态分配，非真泄露"
是**误判**：数字不涨 ≠ 没泄露。

**常量泄露的检测法（按成本排序）**：
1. **源码级查引用**：进程退出时谁还强引用谁。actor 的 `caf::actor` 成员 = 强引用；
   环 = 双方 control block 互相保活，退出不释放。函数局部静态 `caf::actor`
   （如 shutdown_manager_ref()）同样保活。
2. **注释隔离法**：注释掉怀疑的 spawn/代码块 → dump 数字暴跌 = 就是它（实测
   ops 块 + 节点块注释 + plugin_mgr 回指删除 → 205 块→1 块）。
3. checkpoint diff 对常量泄露**同样无效**（它只看运行期增量）。

**修复模式：回指不存成员，用时 registry 瞬时查找、发送即弃**：
```cpp
auto mgr = caf::actor_cast<caf::actor>(system().registry().get("shutdown_mgr"));
if (mgr) caf::anon_send(mgr, shutdown_atom{});
```
（需要 `#include <caf/actor_registry.hpp>`，pimpl，all.hpp 不含。）plugin_mgr 的
request_shutdown_atom 与 ops 的 quit 已按此改造。完整清零还需：静态
`shutdown_manager_ref()` 在 wait_for_shutdown 里置空（bootstrap 里曾被注释）、
GracefulShutdown 的 plugin_mgr_/registry_/checkpoint_mgr_/logging_service_/
cluster_ctls_ 在 finish_shutdown quit 前清空、master 的 monitor_actor 改弱引用。

## 终章：8B vtable 实例泄露（down_msg/exit 竞态）→ 0 块/0B（2026-08-24）

**症状**：破环后 dump 剩 1 块/8B，Data = `10 89 86 C1 FE 7F 00 00`（0x7FFE... 高地址）。

**定位三步**：
1. **无插件对照**：`entry-plugins = []` 跑 → dump 全空（0 块）→ 块来自插件加载。
   注意：无插件时 stderr 无任何 dump 输出 = 连"Detected memory leaks"都没有，
   EXIT=0 且零堆块残留，这才是真正的"零泄露"形态。
2. **块内容**：8B 且内容是 DLL 代码段指针 = 纯 vtable 对象（无数据成员的类实例）。
   本例 = `create_plugin()` 的 `new BusinessPlugin()`（BusinessPlugin 无成员，
   只有 vtable → 恰好 8 字节）。
3. **机制（源码审计）**：正常关机时插件 actor quit 发的 down_msg 与 plugin_mgr
   收到的 exit_msg 来自**不同 sender，无顺序保证**——finish_shutdown 的 send_exit
   先到 → plugin_mgr quit → down_msg 被丢 → down_msg handler 里的
   `destroy(instance)` 永不执行 → 实例泄露。destroy 只在 retired_（热更新）和
   "Plugin crashed"（down_msg 匹配 plugins_）两个分支，正常关机路径没有兜底。

**修复**：monitor（plugin_mgr）加 exit_msg handler，quit 前遍历
plugins_/retired_ destroy 全部实例（down_msg 路径已 erase 的不会双删）：
```cpp
[=, this](caf::exit_msg& em) {
    for (auto& kv : plugins_) if (kv.second.destroy) kv.second.destroy(kv.second.instance);
    plugins_.clear();
    for (auto& rit : retired_) if (rit.destroy) rit.destroy(rit.instance);
    retired_.clear();
    self->quit(em.reason);   // 处理 exit_msg 后必须显式 quit，CAF 不会自动退
},
```

**同类教训**：monitor 的 down_msg handler 里做"资源销毁"在关机路径不可靠——
down_msg 与 monitor 自己的 exit 竞态。凡是被 monitor 的 actor 有配套资源（实例/
句柄），monitor 必须 exit_msg 兜底。**验证**：--test-ctrl-c 与冒烟全量（含热更新
v2）均 0 泄露，EXIT=0，无残留 actor。战果：205 块/19.8KB → 1 块/8B → **0 块/0B**。

## 集群模式泄露测试（2026-08-24 实测：master+worker 双进程 0 泄露）

**目标**：验证双进程集群（middleman/BASP/注册表/心跳/monitor down 感知）生命周期无泄露。判据同单进程：两边 CRT dump 均为 0（无 "Detected memory leaks"）+ EXIT 0。

**核心难点**：`--test-ctrl-c` / `--test-auto-shutdown` 都是写死 delayed 2s 关机——master 必须活着等 worker 注册，2s 窗口必错过（worker 启动要 ~3.3s：exe 加载 + 插件 + checkpoint restore）。

**正确姿势（实测通过）**：

```bash
# master：cmd 括号子进程延迟 EOF——app 立即启动，60s 后 ping 结束 echo x → EOF → watchdog 优雅关机
cmd.exe /c "(ping -n 61 127.0.0.1 >nul & echo x) | caf_plugin_app.exe \
  --caf-plugin-system.node-kind=master --caf-plugin-system.node-name=master \
  --caf-plugin-system.node-port=47000 --caf-plugin-system.lease-seconds=6"

# worker：独立 run_worker/ 目录（防 checkpoints 双写冲突），test-ctrl-c 2s 自关
cd run_worker && ./caf_plugin_app.exe --caf-plugin-system.node-kind=worker \
  --caf-plugin-system.node-name=worker-a --caf-plugin-system.node-port=47001 \
  --caf-plugin-system.master-port=47000 --caf-plugin-system.parent=master \
  --caf-plugin-system.lease-seconds=6 --caf-plugin-system.test-ctrl-c=true < /dev/null
```

**验证证据**：worker `[NodeClient:worker-a] register: OK` + master `[ClusterMaster] registered node 'worker-a'` → worker 退出 → master `node 'worker-a' went down`（跨进程 monitor 实时感知）= 真集群交互。两边 EXIT 0 + dump 0 = 通过。

**踩坑（都实测过）**：
1. **cmd `&` 是顺序执行**：`ping -n 61 >nul & echo x | app` 会让 **app 延迟 60s 才启动**（管道整体在 ping 之后）→ worker 先到必连不上。**必须括号**：`(ping & echo x) | app`（括号子进程组作为管道左侧，app 立即启动）。
2. **WSL interop 下 FIFO 的 EOF 传不到 exe**：`< /tmp/fifo` 时 bash 持有 pipe 写端，FIFO 写端全关 ≠ exe 的 stdin EOF → watchdog 永远不触发。**只能用 Windows 侧管道**（cmd 的 `|`）控制 EOF。
3. **两个进程共享 run/ 会并发写 checkpoints/ 与 updates/**：MSVC ofstream 并发写同一文件会失败/阻塞（shutdown-trace.log 同款坑）→ worker 必须独立目录（`cp -r run run_worker` + 删日志/checkpoints）。
4. **残留进程并发写同一日志文件 → stdout 行粘连**（`[14:04:36] Graceful...registr[14:05:08] Graceful...` 挤一行）：看着像"卡 32s"实为两个 master 进程（前一个延迟启动版 + 正式版）并发写 master_out.log。**启动新测试前先确认旧 master 进程已退**（`tasklist | findstr caf_plugin_app` 应为空），否则误判。判据：shutdown-trace.log 只有一条完整链 = 正式进程正常；粘连行归属用 trace 时间戳对不上 = 残留进程。
5. worker `--test-ctrl-c` 用 `< /dev/null` 没问题（test 后门触发关机，watchdog 挂起无妨）；`echo hello |` 会立即 EOF 触发 watchdog 抢跑（注册窗口没了）。

## 用户信任工作流（重要）

用户对"无泄露"结论持怀疑时会亲自验证。正确应对：给**可复现的命令** +
**眼见为实的证据**（对照数字/块内容），不要只给结论。判据必须能被用户自己
复跑（两遍对照、块内容分析——**MAP_ALLOC 行号已不可用**，见三层方法③）。
用户会在 VS Code 里并行改代码——每次 patch 前先读文件（外部修改警告频繁），
改完主动构建+同步 run/。**"怎么还是内存泄漏"的回应框架（2026-08-24）**：
报告一直长这样、结论没变——"Detected memory leaks!" 是 CRT 对进程退出时
**所有存活堆块**的总报告，必然包含进程生命周期常驻（注册表/logger）；
正确指标是 checkpoint diff（运行期净增量），与活动量零相关。**Debug 主体被
跳过诊断（并行编辑二次实例）**：`[App] build` 打印 + 零 spdlog 日志 +
checkpoint diff 只剩 3 块/104KB + dump 仅 2 块 = caf_main 在 `#ifdef _DEBUG`
块内被误放的 `return 0;` 提前返回（主体全跳过），先查这个再怀疑泄露。
