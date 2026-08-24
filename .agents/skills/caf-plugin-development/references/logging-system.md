# 日志体系重构（2026-08-23）：logging_service 系统组件 + 单头宏日志

用户拍板三条：**写日志只有一个地方**、**必须用宏**、**记录行号**；开关机流程单独日志文件（shutdown-trace.log 保留）。
结果：LoggerPlugin 删除、`framework_log.{hpp,cpp}` 删除、52 处 CAF_LOG 死代码全转宏、`logging_service` 升级为系统组件。

## 为什么之前有两个 logging_service.hpp（合并的动因）

- `caf_plugin_core` 是**静态库**，被 exe **和每个插件 DLL 各链接一份** → 静态库里的全局单例在不同模块里是**不同变量**（符号副本）。插件 DLL 永远够不到 exe 侧的单例（与 g_console_closing 同款问题）。
- 所以插件宏只能靠**依赖注入**拿日志 actor（`deps[0]`），宏必须带 `logger` 参数 → 出现"核心头 + 插件头"两个同名文件。

## 解法：每模块单例（inline static 反用符号副本）

```cpp
// include/services/logging_service.hpp —— 全项目唯一日志头
namespace caf_plugin_system {
inline caf::actor& current_logger() { static caf::actor logger; return logger; }
inline void set_logger(const caf::actor& a) { current_logger() = a; }
inline std::string& log_source() { static std::string s = "core"; return s; }
inline void set_log_source(const std::string& s) { log_source() = s; }
}
```

inline 函数里的 static 局部变量**按模块实例化**：exe 内所有 TU 合并一份、每个插件 DLL 各一份（spdlog 注册表同款机制）。
- exe 侧：`spawn_logging_service(sys)`（bootstrap_system_components **最先 spawn**，进程第一个 actor）内部自动 `set_logger(svc)`。
- 插件侧：spawn 时 `set_logger(deps[0]) + set_log_source(PLUGIN_NAME)`（business/platform 已改）。

## 宏设计

```cpp
// __VA_ARGS__ 无法在预处理器里拆分（格式串 vs 参数），用模板 helper 拆：
//   0 参 = 原样返回（运行时拼接串）；1+ 参 = 首参是格式串、其余是参数。
template <typename T, typename... Args>
inline std::string log_fmt(const T& first, Args&&... args) {
    if constexpr (sizeof...(Args) == 0)
        return std::string(first);
    else
        return fmt::format(fmt::runtime(first), std::forward<Args>(args)...);
}

#define LOG_IMPL(level, ...)                                                       \
    do {                                                                           \
        if (auto& svc = caf_plugin_system::current_logger()) {                     \
            try {                                                                  \
                caf::anon_send(svc, log_atom{}, caf_plugin_system::log_source(), level, \
                    caf_plugin_system::log_loc(__FILE__, __LINE__)                 \
                        + caf_plugin_system::log_fmt(__VA_ARGS__));                \
            } catch (...) { /* <log format error> 兜底 */ }                        \
        }                                                                          \
    } while (0)
#define LOG_INFO(...)  LOG_IMPL("INFO",  __VA_ARGS__)
// LOG_WARN / LOG_ERROR / LOG_DEBUG 同理；LOG_*_SELF(self, ...) 有全套 4 级
// LOG_FROM(self, level, msg) — self->send（关机链 STOPPED 用，同 sender FIFO）
```

- `fmt::runtime(first)` + 参数放**外面**（`fmt::format(fmt::runtime(fmt_str), args...)`）：让**运行时拼接串**（`"x" + var`）和**字面量格式串**（`"n={}", n`）都能过编译（见坑 1、坑 6）。
- `log_loc` 只取 basename：`find_last_of("/\\")` 后截断，避免全路径噪音。
- 日志 actor 的 log_atom handler：`current_sender()` 非空 → 记 `source#actor_id`（首次见到绑 addr→source 映射），空（anon_send）→ 只记 source。**关机链 LOG_FROM 的 sender = shutdown_mgr**，天然被追踪。

## 关机链最后退出

finish_shutdown 顺序：stop_cluster → send_exit(plugin_mgr/registry/checkpoint_mgr) → state=stopped → `LOG_FROM(self,"INFO","STOPPED")` → `send_exit(logging_service_)` → quit → notify。
- LOG_FROM 与 send_exit **同 sender（shutdown_mgr）** → CAF FIFO 保证 STOPPED 日志先被日志 actor 处理，再收到 exit_msg。
- 日志 actor 的 exit_msg handler：`file_sink->flush(); console_sink->flush(); self->quit(reason)`。
- GracefulShutdown 构造加 `caf::actor logging_service_` 参数（bootstrap 传入 out.logging_service）。

## 核心内置服务（插件依赖可引用系统组件）

- `resolve_dependencies`（plugin_loader.cpp）：初始化 `service_to_plugin` 时预置 `{"logging_service", "@core"}`；依赖循环里 `if (svc_it->second == "@core") continue;`（不要求插件提供、不压栈）。DependencyGraph 对未知服务**静默忽略**，无需改。
- ServiceRegistry `register_atom(name, actor, provider)` 自动 spawn proxy + **镜像到 `sys.registry()`**（service_registry.cpp ~166 行）→ fw 侧与插件 deps 都能解析。
- ACL：`acl_allow` 写核心服务名（如 `{"logging_service"}`）时，plugin_manager 在 plugins_ 找不到 → **回退 `blocking->request(registry_, resolve_atom{}, pname)`** 拿 proxy 地址作为信任方（business manifest 已改，原 `{"LoggerPlugin"}` 因插件删除会告警 + 空信任集）。

## 编译坑（本轮实测，全是真报错）

1. **fmt v12 C7595**：`fmt::format` 的 format string **必须是编译期常量**——运行时拼接串直接编译失败（`error C7595: 对即时函数的调用不是常量表达式`，还伴随 `读取超过生命周期的变量` 的误导性子错误，指向 this/变量）。修法：`fmt::format(fmt::runtime(expr), args...)`（见坑 6）或干脆不做格式化。
6. **fmt::runtime 只收格式串一个参数（C2660）**：`fmt::runtime("n={}", n)` → `error C2660: "fmt::v12::runtime": 函数不接受 2 个参数`——**参数必须放在外面**：`fmt::format(fmt::runtime("n={}"), n)`。坑 1 的"修法"只写 `fmt::runtime(expr)` 时容易顺手把整个 `__VA_ARGS__` 塞进 runtime（0 参调用没暴露，一旦调用带参数 `LOG_INFO("{}", x)` 就炸）。**宏里拆分 __VA_ARGS__ 在预处理器做不到** → 模板 helper `log_fmt`（`if constexpr (sizeof...(Args)==0)` 分支：0 参原样返回、1+ 参首参当格式串），宏统一走 `log_fmt(__VA_ARGS__)`。注意补全变体宏（LOG_DEBUG_SELF 曾漏定义 → `error C3861 找不到标识符`）。
2. **宏不能带命名空间限定**：`caf_plugin_system::LOG_INFO(...)` → `error C2589: "do": "::" 右边的非法标记`（宏展开后 `::` 后是 `do {`）。sed 全局替换 `fw_log_info(` → `LOG_INFO(` 会把限定调用 `caf_plugin_system::fw_log_info(` 也变成 `caf_plugin_system::LOG_INFO(` —— 先处理限定形态（`caf_plugin_system::fw_log_info(` → `LOG_INFO(`）再处理裸形态，改完 grep `caf_plugin_system::LOG_` 复查。
3. **spdlog.h 不含 pattern_formatter.h**：`spdlog::pattern_formatter` 未定义 → `make_unique` 报 `_Ty 模板参数无效，应为类型` + 连锁 `C3536 formatter 初始化之前无法使用`。必须显式 `#include <spdlog/pattern_formatter.h>`。两个 sink 构造是 `explicit`（`explicit basic_file_sink(const filename_t&)`）但 make_shared/make_unique 是**直接初始化**，允许 explicit，不是问题；真问题是类型未定义。
4. **sed 按行号插入会错位**：`sed -i '101a\...'` 插入 7 行后，后续 `sed -i '136i\...'` 的目标行号已漂移 → 插错位置产出语法错误的代码。教训：行号型 sed 插入后**重新 grep 行号**，或直接用 Python 行区间替换（`lines[97:146] = new_block.split('\n')`），替换后检查是否多出/少掉闭合括号。
5. **`fw_log("INFO", msg)` 两参数形态被 sed 成裸 `log("INFO", msg)`** → 撞 cmath 的 `::log`（`error C2661: "log": 没有重载函数接受 2 个参数`，候选是 `double log(_Ty)`）。grep 残留用 `log\(` 模式查。

## 插件迁移步骤（business/platform 已按此完成）

1. 头文件从 `services/logging_service.hpp` 获取宏（不再有 logger 参数）。
2. spawn 函数里 `caf::actor logger = deps.empty() ? caf::actor{} : deps[0];` 后加：
   `caf_plugin_system::set_logger(logger); caf_plugin_system::set_log_source(PLUGIN_NAME);`
3. 所有 `LOG_*(logger, ...)` → `LOG_*(...)`（或 `LOG_*_SELF(self, ...)` 若要 actor 追踪）。
4. 清掉不再使用的 `logger` lambda 捕获（`[logger, plugin_mgr, self]` → `[plugin_mgr, self]`）。

## 验证清单（重构后必测）

- `--test-ctrl-c` EXIT 0；点 X 全链 + checkpoints 更新（老方法见 references/console-close-testing.md）。
- `logs/app.log` 应有：启动期 `[Bootstrap]` 诊断（**首次真正可见**）+ `[business#<id>]` 插件行（actor 追踪生效）+ 行号前缀 `[file:line]`。
- 关机链 STOPPED 行存在（LOG_FROM FIFO 生效）；shutdown-trace.log 完整。
