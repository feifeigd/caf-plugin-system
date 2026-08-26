#pragma once
// ------------------------------------------------------------------
// 声明式插件配置（PLUGIN_CONFIG 宏）
//
// 目标：插件声明字段列表（类型+名字+默认值），注册/解析/读取全自动，
// 不写任何 .add 注册代码和 get_or 解析代码。
//
// 用法（插件 cpp 内；PLUGIN_NAME 由 CMakeLists 提供）：
//
//   #define MONGO_FIELDS(X) \
//       X(std::string, uris, "default=mongodb://127.0.0.1:27017") \
//       X(int, pool_size, 2, PLUGIN_CONF_CLI)   // 4 参 = 显式标记
//   PLUGIN_CONFIG(MONGO_FIELDS)
//   #undef MONGO_FIELDS
//
//   // spawn 处：
//   auto cfg = load_plugin_config(sys.config());
//   use(cfg.uris, cfg.pool_size);
//
// 读取路径 = caf-plugin-system.<PLUGIN_NAME>.<字段名>（conf 嵌套块）：
//
//   caf-plugin-system {
//     mongo {
//       uris = "default=mongodb://127.0.0.1:27017"
//       pool_size = 2
//     }
//   }
//
// 说明：
//   - 配置段名 = PLUGIN_NAME（CMakeLists 的 target_compile_definitions
//     PLUGIN_NAME="mongo"），与日志短名三合一，插件配置自动隔离。
//   - conf 键名 = C++ 字段名（pool_size，下划线原样）。CAF 点分路径
//     大小写敏感，先保持一致。
//   - 字段类型由 caf::get_or 支持集决定（string/int/double/bool/...）。
//   - CAF 的 conf 解析不校验键名，未注册键照常读取——只走 conf 的
//     字段零注册即可用；CLI 注册由字段标记控制（见下）。
//
// 字段级 CLI 标记（第 4 参，可省略，默认 PLUGIN_CONF_ONLY）：
//   X(Type, Name, Def)                  —— 只读 conf，不注册 CLI
//                                         （敏感配置推荐：URI 密码不进
//                                          命令行/ps/任务管理器）
//   X(Type, Name, Def, PLUGIN_CONF_CLI) —— conf + CLI 双通道
//   按字段粒度控制，互不影响。
//
// CLI 注册的接线：带 PLUGIN_CONF_CLI 标记的字段会生成注册条目
// （PLUGIN_FIELD_REG），由 register_plugin_config() 统一执行——该函数
// 由内核 declare_config 钩子（plugin_manager 在 spawn 前回调插件）调用，
// Phase 4 接线；当前为空实现占位。
// ------------------------------------------------------------------

#include <caf/actor_system_config.hpp>
#include <caf/settings.hpp>
#include <string>

namespace caf_plugin_system {

// 字段注册标记（仅作为宏参数使用，不可作为值比较）
#define PLUGIN_CONF_ONLY PLUGIN_CONF_ONLY
#define PLUGIN_CONF_CLI PLUGIN_CONF_CLI

} // namespace caf_plugin_system

// -- 参数个数选择（区分 3 参/4 参字段声明） ----------------------------------

#define PLUGIN_ARG_N(_1, _2, _3, _4, _5, _6, _7, _8, _9, N, ...) N
#define PLUGIN_RSEQ_N() 9, 8, 7, 6, 5, 4, 3, 2, 1, 0
#define PLUGIN_NARG(...) PLUGIN_ARG_N(__VA_ARGS__, PLUGIN_RSEQ_N())

// -- X-macro 字段处理 ------------------------------------------------------

#define PLUGIN_FIELD_DECL(Type, Name, Def, ...) Type Name = Def;
#define PLUGIN_FIELD_LOAD(Type, Name, Def, ...)                                \
  c.Name = caf::get_or(cfg, "caf-plugin-system." PLUGIN_NAME "." #Name, c.Name);

// CLI 注册条目（按标记分支；空参数 = 默认 CONF_ONLY 不注册）
#define PLUGIN_FIELD_REG(Type, Name, Def, ...)                                 \
  PLUGIN_FIELD_REG_SEL(PLUGIN_NARG(__VA_ARGS__), __VA_ARGS__)(Type, Name, Def)
#define PLUGIN_FIELD_REG_SEL(N, ...) PLUGIN_FIELD_REG_BY_##N
#define PLUGIN_FIELD_REG_BY_0(Type, Name, Def) /* 默认：只 conf，不注册 CLI */
#define PLUGIN_FIELD_REG_BY_1(Type, Name, Def, ...)                            \
  PLUGIN_FIELD_REG_MODE(__VA_ARGS__)(Type, Name, Def)
#define PLUGIN_FIELD_REG_MODE(Mode) PLUGIN_FIELD_REG_##Mode
#define PLUGIN_FIELD_REG_PLUGIN_CONF_ONLY(Type, Name, Def) /* 只 conf */
#define PLUGIN_FIELD_REG_PLUGIN_CONF_CLI(Type, Name, Def)                      \
  /* TODO(phase4): 生成 cfg.add(..., #Name, ...) CLI 注册 */                    \
  static_cast<void>(cfg)

// -- 实现 ------------------------------------------------------------------

#define PLUGIN_CONFIG(Fields)                                                  \
  namespace {                                                                  \
  struct PluginConfig {                                                        \
    Fields(PLUGIN_FIELD_DECL)                                                  \
  };                                                                           \
  inline PluginConfig load_plugin_config(const caf::settings& cfg) {           \
    PluginConfig c;                                                            \
    Fields(PLUGIN_FIELD_LOAD)                                                  \
    return c;                                                                  \
  }                                                                            \
  inline void register_plugin_config(caf::actor_system_config& cfg) {          \
    /* Phase 4 由内核 declare_config 钩子调用；当前空实现占位。            */ \
    Fields(PLUGIN_FIELD_REG)                                                   \
  }                                                                            \
  } // namespace
