# caf-plugin-sdk

> **本目录是生成物**：由 `sdk/package.cmake`（或 `sdk/package.bat`）从最新构建树组装，
> 请勿手改——改了下次打包会被覆盖。模板与脚本在仓库 `sdk/` 目录。

caf-plugin-system 的**最小插件开发依赖包**。拿到本包即可在仓库之外独立开发、编译插件 DLL，无需整个框架源码。

## 包内容

```
caf-plugin-sdk/
├── include/                    # 插件开发所需全部头文件（保持框架内原始相对布局）
│   ├── plugin/
│   │   ├── plugin_interface.hpp    # 插件 ABI：PluginEntry / plugin_manifest / extern "C" 导出
│   │   └── plugin_lifecycle.hpp    # 公共生命周期骨架（drain 回执 / shutdown quit 框架统一）
│   ├── graceful_shutdown.hpp       # 生命周期协议依赖的内核类型
│   ├── common/                     # 内核公共协议（消息标签 / 信封 / 集群与存储契约）
│   └── services/logging_service.hpp# 唯一日志入口：LOG_INFO/LOG_WARN/LOG_ERROR 宏
├── lib/
│   ├── Debug/caf_plugin_core.dll + .lib    # 预编译核心库（Debug）
│   └── Release/caf_plugin_core.dll + .lib  # 预编译核心库（Release）
├── cmake/caf-plugin-sdk-config.cmake       # CMake 包：find_package 后得 caf::plugin_core
├── vcpkg.json                              # 最小三方依赖清单（caf 1.1.0 / spdlog 1.16.0 / fmt 12.1.0）
├── examples/hello_plugin/                  # 独立插件工程示例（含 CMakeLists）
└── docs/plugin-guide.md                    # 插件开发指南（manifest/信封/ACL/热更/号段约定）
```

## 硬约束（ABI 兼容）

1. **CAF 版本必须精确一致**：插件经 vcpkg 使用 caf **1.1.0**（`vcpkg.json` 已锁定）。CAF 元对象/type_id 是进程级注册表，版本不一致直接崩。
2. **Debug/Release 必须同配置匹配**：插件与主程序运行配置一致（CRT、CAF 构建类型、`caf_plugin_core.dll` 都按配置取用 `lib/<Config>/`）。Release 插件跑 Debug 主程序（或反之）是未支持组合。
3. **工具链**：Windows + VS2022（v143）+ x64 + C++20，`/utf-8` 编译选项（源码含中文注释/字符串）。
4. **`caf_plugin_core.dll` 与主程序同源**：本包随框架版本发布；框架升级后需同步更换 SDK（dll + include），插件需重新编译。
5. **运行时 DLL**：插件部署目录由主程序的 `run/<Config>/` 提供 `caf_core.dll` / `caf_io.dll` / `fmtd.dll|fmt.dll` / `spdlogd.dll|spdlog.dll`（vcpkg runtime），SDK 不含这些，开发机上由 vcpkg 提供。

## 快速开始（以 examples/hello_plugin 为例）

```bat
:: 1. 把 SDK 的 vcpkg.json 拷到示例目录（vcpkg manifest 模式，自动装依赖）
copy ..\..\vcpkg.json .

:: 2. 配置（指向 SDK 根目录；vcpkg 工具链按环境变量 VCPKG_ROOT 或常见路径自动探测）
cmake -B out -G "Visual Studio 17 2022" -A x64 -DCAF_PLUGIN_SDK_ROOT=..\..\

:: 3. 构建
cmake --build out --config Debug

:: 4. 部署：out\Debug\hello_plugin.dll → <主程序>\run\Debug\plugins\hello\
```

然后在主程序 `caf-application.conf` 的 `entry-plugins` 中加入 `"HelloPlugin"`，重启即加载。

在自己的工程里使用：把 `vcpkg.json` 拷到工程根，`find_package(caf-plugin-sdk CONFIG REQUIRED)` 后 `target_link_libraries(your_plugin PRIVATE caf::plugin_core)` 即可，参考示例 CMakeLists。

## 插件开发要点速览

- **一个 DLL = 一个 actor**：`create_plugin()` 返回的 `PluginEntry`，`spawn()` 产出唯一 actor；`manifest.provides[]` 的每个服务名都映射到这同一个 actor（registry 为每个服务包一个代理）。
- **生命周期**：`init → drain(回执) → save_state → restore_state → shutdown`；直接用 `plugin_lifecycle(self, hooks)` 组合进 behavior，drain 回执与 shutdown quit 由框架统一（详见 docs/plugin-guide.md）。
- **日志**：只 include `services/logging_service.hpp`，用 `LOG_INFO(...)` 宏；插件模块内 `set_log_source(PLUGIN_NAME)` 一次（`PLUGIN_NAME` 由 CMake `target_compile_definitions` 注入）。
- **私有消息号段**：200~999 内核占用；插件私有从 **1000 起**自选互不重叠段，并实现 `register_meta_objects()` 导出（actor_system 构造前由框架调用）。跨插件消息推荐走 `plugin_envelope` 公共信封（不占号段、无需自注册）。
- **热更新**：只加不减服务（provides add-only）；新 DLL 必须放**新路径**（同路径 LoadLibrary 命中缓存）。

完整协议与踩坑清单见 [docs/plugin-guide.md](docs/plugin-guide.md)。
