# Windows 优雅关机实战记录（控制台点 X / 日志体系 / CAF 语义）

> 本文件沉淀 caf-plugin-system 在 Windows 控制台环境下的关机、日志、CAF 使用经验。
> 来源：2026-08-22 ~ 08-23 多轮实测（含裸程序对照实验），与 `~/.hermes/skills/software-development/caf-plugin-development` 同步。
> 核心结论都经过实测验证，标注了实验条件，避免"想当然"。

---

## 1. 核心机制：CTRL_CLOSE_EVENT 的真相（最重要）

### 1.1 错误认知（旧，已推翻）

旧结论认为：注册 handler 处理 `CTRL_CLOSE_EVENT` 后，点 X 有**约 5 秒窗口期**，
超时才会被系统强杀。依据是微软文档的"short period"。

### 1.2 真相（2026-08-23 对照实验）

**handler 立即返回 TRUE → conhost 在 ~0ms 内 TerminateProcess（退出码 0xC000013A）**。
异步关机链根本没时间跑完（trace 断在半路、checkpoints 不落盘）。

对照实验（裸 20 行控制台程序 + `SetConsoleCtrlHandler`）：

| handler 行为 | 实测结果 |
|---|---|
| 立即返回 TRUE | SC_CLOSE 后 **0ms 被杀**（handler 写的日志文件能证明 handler 跑过） |
| handler 内 Sleep(8000) | **存活 4.6s 才被杀**（handler 还在阻塞就被杀，无"handler returned"日志） |

结论：**~5 秒是 conhost 的硬上限，从信号投递开始计时；阻塞 handler 只能把窗口从
~0ms 延长到 ~5s，不能无限延长。** 旧观察到的"5 秒窗口耗尽"实为误诊——半行 trace
残片（如 `down_mgr`）本来就是"写入途中被杀"（ms 级强杀），不是 5s 超时。

### 1.3 正确姿势

```cpp
case CTRL_CLOSE_EVENT:
    // ① 置 console_closing 标志 + fd 重定向 NUL（防后续写销毁中的控制台阻塞）
    // ② 落盘 trace（[signal] 行，唯一可靠证据）
    // ③ anon_send(shutdown_mgr, shutdown_atom)
    // ④ 【关键】阻塞等待关机链完成（cv，4s 超时兜底 < 4.6s 实测杀点）
    // ⑤ ExitProcess(0) —— 此时 checkpoints 已落盘，退出码干净
    ExitProcess(0);  // 绝不 return TRUE
```

- `ExitProcess` 不跑 atexit、不刷 stdio 缓冲，**但关机链的落盘都在它之前完成**
  （CheckpointManager 是 tmp + flush + close + 原子 rename，数据在链完成时已上磁盘）。
- CTRL_LOGOFF / CTRL_SHUTDOWN 同理（系统注销/关机路径，窗口期受
  `WaitToKillAppTimeout` 注册表控制，默认 5s，同样不能无限等）。
- Ctrl+C / Ctrl+Break 保持原样（控制台还活着，进程可自然退出，无需 ExitProcess）。

### 1.4 时间上限与"十分钟关机"问题

- **点 X：~5s 硬上限**，conhost 行为，无注册表/API 可延长。
- **系统注销/关机：** `WaitToKillAppTimeout`（默认 5000ms，注册表可调，全系统共享）。
- **GUI 窗口关闭（WM_CLOSE）：无限**，自己消息循环处理，无 conhost 参与。
  唯一能支撑任意时长保存的路径（代价：不再是纯控制台程序）。
- **工程推荐**：定期 checkpoint（后台自动落盘）+ 关机只存增量，让保存永远 < 5s。
  10 分钟的全量保存是设计问题，用增量思路解决，而不是指望 Windows 给更长窗口。

---

## 2. 关机链防卡堆栈（点 X 场景，从浅到深）

控制台销毁时**任何写控制台句柄的操作都会永久阻塞**（spdlog stdout sink / cout /
printf 全中招），必须逐层防御：

| 层 | 机制 | 作用 |
|---|---|---|
| 1 | handler 先 `anon_send(shutdown_atom)` 再打印 | 打印只是诊断，绝不能阻塞关机信号 |
| 2 | 全局 `console_closing()` 标志 + `shutdown_out()` | 关机链输出统一走：**先写文件（永远可靠），console_closing 才 cout** |
| 3 | **fd 重定向 NUL**（`_open("NUL")` + `_dup2(nul,1/2)`） | **进程级全局兜底**：任何线程/任何 DLL 的 fwrite/cout/printf 落 NUL 立即返回。`freopen` 对 spdlog 无效（sink 持有构造时的 FILE*），`_dup2` 走 fd 层才有效 |
| 4 | trace 写盘用 Windows API（`CreateFile FILE_APPEND_DATA` + `FILE_SHARE_READ\|WRITE`）+ 全局 mutex | MSVC ofstream 默认 `FILE_SHARE_READ`，并发打开同一文件会失败/阻塞——曾拖死关机链 |
| 5 | 双触发保护 | `shutdown_atom` 在 `shutting_down` 状态直接忽略，防止二次触发走 "Not ready, forcing exit" 打断保存 |

**静态库 + 插件 DLL 符号副本坑**：插件链接静态库 → `console_closing()` 在 DLL 里是
独立副本（永远 false）——插件侧检查必然失效，**只能靠层 3 的 fd 重定向兜底**。
同类问题：`fw_log` 的 `g_logger` 注入在 exe 侧，插件 DLL 感知不到。

### 落盘证据矩阵（控制台销毁场景唯一可信判断）

- `shutdown-trace.log`：`[signal] CTRL_CLOSE_EVENT` 行 + 全链 trace（带 `[HH:MM:SS]` 时间戳）
- `checkpoints/*.ckpt` mtime 更新 = **数据保存实锤**（强杀则两者都无）
- 排查：只有 `[signal]` 行 = 链没走；什么都没有 = handler 没被调用（查注册/旧 exe）

---

## 3. 日志体系（2026-08-23 重构：logging_service 系统组件 + 唯一头文件）

### 3.0 架构演进（LoggerPlugin → 核心组件）

**LoggerPlugin 已删除。** 日志服务是 `bootstrap_system_components` **最先 spawn** 的系统组件
（进程第一个 actor），`finish_shutdown` **最后 send_exit**（exit_msg 里 flush 再 quit），
spdlog console + `logs/app.log` 归它独占。插件 manifest 的 `logging_service` 依赖由
**核心内置服务**满足（`resolve_dependencies` 对 `{"logging_service", "@core"}` 跳过插件
加载；`plugin_manager` 从 ServiceRegistry 解析到核心 actor）。

### 3.1 唯一日志头：`include/services/logging_service.hpp`

**两个 `logging_service.hpp` 已合并为一个**（曾因 DLL 隔离存在两个：核心单例在静态库
`caf_plugin_core` 里，被 exe 和每个插件 DLL 各链一份 → 插件够不到 exe 的单例，只能靠
依赖注入）。合并方案是**每模块单例**（inline 函数 static 局部变量按模块实例化，spdlog
注册表同款机制）：

```cpp
inline caf::actor& current_logger() { static caf::actor l; return l; }  // exe/各DLL 各一份
inline void set_logger(const caf::actor& a) { current_logger() = a; }
```

- exe 侧：`spawn_logging_service` 内部自动 `set_logger()`；插件侧：spawn 时
  `set_logger(deps[0])` + `set_log_source(PLUGIN_NAME)`（business/platform 已内置）。
- 宏：`LOG_INFO/LOG_WARN/LOG_ERROR(msg或fmt串, args...)` + `LOG_*_SELF(self, ...)`
  （self->send，日志服务记录发送方 actor）+ `LOG_FROM(self, level, msg)`（关机链用，
  同 sender FIFO 保证 STOPPED 日志先落再 exit）。
- **行号自动嵌入**：`[文件名:行号]` 前缀（`log_loc(__FILE__, __LINE__)`）。
- **变参拆分**：`__VA_ARGS__` 无法在预处理器拆分 → 模板 helper `log_fmt(first, args...)`：
  0 参原样返回（运行时拼接串），1+ 参 = 首参格式串 + 其余参数。`fmt::runtime` 绕过
  fmt v12 的 consteval 要求（C7595：format string 必须是编译期常量），类型不匹配在
  运行时抛异常 → try/catch 兜底为 `<log format error>`。
- **格式串坑（C7595）**：`fmt::format("x " + var)` 编译不过（fmt v12 consteval）；
  必须走 `log_fmt` 或 `fmt::format(fmt::runtime(...))`。
- 日志服务 actor 按 source 建 per-source spdlog logger 缓存（sink 共享）；
  `current_sender()` 非空时显示 `source#<actor_id>`（actor 级追踪）。

### 3.2 核心内置服务（非插件提供的服务依赖）

`plugin_loader::resolve_dependencies` 预置 `{"logging_service", "@core"}`：
插件依赖声明 `"logging_service"` 不要求插件提供者；加载时 `plugin_manager` 从
ServiceRegistry resolve 到核心 actor（bootstrap 里 `register_atom{}` 注册，注册后
ServiceRegistry 建 proxy 并镜像到 CAF `sys.registry()`）。ACL 的 `acl_allow` 也可引用
核心服务名（plugin_manager 查 plugins_ 未命中时回退到 ServiceRegistry resolve）。

### 3.3 CAF_LOG_* 死代码（已清零）

应用侧 52 处 `CAF_LOG_*` 全是死代码（编译时未定义 `CAF_LOG_LEVEL`，宏被编掉，
`strings -a exe` 验证）。2026-08-23 全部转换：27 处→fw_log（旧），plugin_manager 22 处
→LOG_*（重构时），fw_log 本体删除后核心/插件一律 LOG_*。

### 3.4 日志文件收敛（43 → 2）

| 文件 | 来源 | 状态 |
|---|---|---|
| `logs/app.log` | logging_service 的 spdlog（basic_file_sink，**truncate=true 每次启动重写**） | **主日志**（业务 + core 运行时 + 关机链，带行号与 actor 追踪） |
| `shutdown-trace.log` | Windows API 追加写 | **关机证据**（控制台销毁时唯一可靠，必须独立） |
| `logs/caf-framework.log` | CAF file logger | **已消失**：verbosity debug→info 后无内容可写（原 1.2MB 99% 是 caf.core 调度 DEBUG 噪音） |

### 3.5 VS Code F12 跳转（IntelliSense）

VS 生成器（vcxproj）不支持 `CMAKE_EXPORT_COMPILE_COMMANDS` → 需要独立的 Ninja
配置目录专供 IntelliSense：

- `setup_intellisense.bat`（仓库根）：vcvars64 + Ninja + `cmake -B out/intellisense -G Ninja
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON`，生成 `out/intellisense/compile_commands.json`。
- `.vscode/c_cpp_properties.json`：`compileCommands` 指向该文件，`intelliSenseMode:
  windows-msvc-x64`。改 CMakeLists 后重跑 bat + `C/C++: Reset IntelliSense Database`。

---

## 4. CAF 语义要点（scoped_actor / anon_send）

### 4.1 scoped_actor 析构不等 send 的回复

```cpp
~scoped_actor() {
    self_->cleanup(exit_reason::user_shutdown);  // 自杀
    self_->join();                                // 等"自己终止"，非等回复
}
```

- `send()` 是 fire-and-forget，析构不等回复；迟到回复（mailbox 已关）直接丢弃，不挂起。
- `request().receive()` 是 **receive 调用点**阻塞等回复，不是析构在等。
- **挂死进程的真凶**是 `actor_system` 析构等"所有 actor 终止"——长生命周期上下文
  （如控制台线程的 getline）里的 actor 必须有自己的退出路径。

### 4.2 使用场景判断

| 场景 | 用哪个 |
|---|---|
| bootstrap/main 同步引导（函数作用域内短暂存在，可能要 request/receive） | **scoped_actor**（身份可溯源、可升级同步确认） |
| 控制台线程 / 信号 handler / 任何长生命周期线程（fire-and-forget） | **anon_send**（scoped_actor 永不退出 → 挂死进程） |

---

## 5. 构建 / 部署 / 验证踩坑速查

- **WSL 里没有 cmake**：用 `"/mnt/c/Program Files/CMake/bin/cmake.exe" --build
  out/build/windows-x64 --config Debug --target caf_plugin_app`。
- **drvfs 时间戳秒级粒度 → MSVC 漏编译**：改文件后必须 `touch`（改多个文件时
  `touch` 两次间隔 1s 更保险），否则构建 EXIT 0 但产物是旧代码。
- **run/ 手工拷贝**：构建后必须 `cp` 同步；残留进程锁 exe → 先
  `Get-Process caf_plugin_app | Stop-Process -Force`；cp 后
  `strings -a run/caf_plugin_app.exe | grep -c "<新标记>"` 验证。
- **hpp 改动波及全量重编**：framework_bootstrap.hpp 的修改会触发 ~10 分钟全量重编
  （正常，cl.exe 持续烧 CPU 就不是卡住）。
- **编译错误常见**：windows.h 缺失（INVALID_HANDLE_VALUE/DWORD 未声明）、
  fw_log 未加命名空间前缀（C3861）、replace_all 造成双前缀
  （`caf_plugin_system::caf_plugin_system::`）。

### 5.1 点 X 的自动化验证方法（无头模拟真实双击）

1. **启动**：`wscript.exe` 跑 VBS（`WScript.Shell.Run` + `CurrentDirectory` 指定 run/），
   在交互会话创建真实控制台窗口（比 `Start-Process`/`cmd start` 更接近双击）。
2. **关窗**：`PostMessage(hwnd, WM_SYSCOMMAND 0x0112, SC_CLOSE 0xF060, 0)`——
   与真实点 X 完全相同的消息序列。
3. **判定**：进程存活时长 + `shutdown-trace.log` 全链 + `checkpoints/` mtime + 退出码
   （0 = ExitProcess 成功；0xC000013A = 被 conhost 强杀）。
4. **对照实验**：裸控制台程序（仅注册 handler）测机制本身，区分"系统行为"与"应用问题"。

### 5.2 快速验证后门（--test-*）

- `--test-ctrl-c`：直接发 shutdown_atom（等价 Ctrl+C 路径），EXIT 0 才算过。
- `--test-quit`：走 ops quit 路径，任何模式自然退出 EXIT 0。
- 注意：`caf-application.conf` 里的 `test-auto-shutdown` 默认注释掉（否则双击会
  自动跑完冒烟测试自己退出，无法测窗口 X）。
