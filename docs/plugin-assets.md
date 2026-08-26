# 插件资源目录（asset_dir）机制

> 插件读取同目录资源（配置/数据/资源文件）的官方姿势。
> 实测通过：2026-08-26（feat/plugin-assets 分支）

## 机制

框架在插件加载时自动注入 **插件目录**（绝对路径，DLL 父目录，正常布局
`plugins/<name>/`），插件通过基类方法读取：

```cpp
// 插件代码（spawn 的 actor 内）：
auto p = asset_path("config.json");      // 绝对路径，Windows 分隔符自动处理
std::ifstream f(p);
```

- `set_asset_dir(dir)` / `asset_dir()` / `asset_path(rel)` 都是 `PluginEntry`
  基类方法（默认实现），**插件零配置零样板**——只有真要读资源的插件才写
  `asset_path(...)` 一行
- 不读资源的插件完全无感（不调用即可）

## 热更新语义（核心设计）

**资源目录与 DLL 位置解耦**：

| 阶段 | DLL 位置 | asset_dir | 资源 |
|---|---|---|---|
| 首次加载 | `plugins/<name>/xxx.dll` | `plugins/<name>/`（固化） | 正常读 |
| 热更新 | `stage/xxx_v2.dll`（新路径） | **继承旧实例值**，仍是 `plugins/<name>/` | **零复制，继续用原资源** |

- reload 时新 PluginEntry 继承 `LoadedPlugin::asset_dir`（旧值）
- 插件名热更时不可改（manifest 检查）→ 目录语义天然稳定
- **DLL 更新 ≠ 资源更新**：代码换位置，资源不搬（实测验证）
- 资源真要更新：直接更新 `plugins/<name>/` 下文件，下次启动/热更生效

实现位置：`plugin_interface.hpp`（基类方法）+ `plugin_manager.cpp`
（load 注入 `absolute(path).parent_path()`；reload 继承 `it->second.asset_dir`）
+ `plugin_manager.hpp`（`LoadedPlugin::asset_dir` 字段）。

## 验证记录（2026-08-26）

场景：postgres 插件 init 读 `resource.json`（可选，不存在跳过）。

```
v1 加载:  PostgresPlugin asset G:\...\run_bridge\plugins\postgres\resource.json
          -> {"env": "dev", "region": "cn", ...}          ← 注入+读取 ✓
热更后:   Plugin hot-reloaded: PostgresPlugin -> stage/postgres_plugin_v2.dll
          Hot-reloaded: pg_service (v2)
          PostgresPlugin asset G:\...\run_bridge\plugins\postgres\resource.json
          -> {"env": "dev", ...}                          ← 继承，零复制 ✓
```

两进程泄露 0 块。

## 注意事项

- **部署**：资源文件与 DLL 同目录（`plugins/<name>/`），部署时一起拷贝
  （run/ 不入库，资源文件在源码侧 `plugins/<name>/` 维护）
- **C++20 捕获**：读资源的 lambda 需显式捕获 this（`[=, this]` 或外层
  lambda 加 `this`）——C++20 起 `[=]` 不再隐式捕获 this（C3493）
- **资源可选原则**：资源文件缺失时插件应跳过（演示代码用 `if (f)` 守卫）
