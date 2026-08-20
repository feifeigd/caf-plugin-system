# CAF Plugin System

基于 C++ Actor Framework (CAF) 的跨平台生产级插件系统。

## 特性

- **跨平台**：Windows (MSVC/MinGW)、Linux、macOS
- **Actor 模型并发**：无锁消息传递，天然多线程安全
- **IoC 依赖注入**：服务注册与发现
- **循环依赖检测**：拓扑排序 + DFS 环检测
- **热更新（Hot Reload）**：Proxy 模式零停机切换
- **优雅关机**：Drain → Save → Exit 三级状态机
- **状态持久化**：自动存盘与恢复
- **零外部依赖**：仅依赖 CAF，不引入 Boost

## 项目结构

```
caf-plugin-system/
├── src/core/
│   ├── plugin_interface.hpp    # 插件最小接口（含跨平台导出宏）
│   ├── dynamic_library.hpp/cpp # 跨平台动态库加载（dlopen / LoadLibrary）
│   ├── dependency_graph.hpp    # 依赖图与环检测
│   ├── service_registry.hpp/cpp # IoC 服务注册表
│   ├── checkpoint_manager.cpp  # 状态存盘管理
│   ├── graceful_shutdown.hpp/cpp # 优雅关机协调器
│   ├── plugin_manager.hpp/cpp  # 插件管理器
│   └── plugin_loader.hpp/cpp   # 插件扫描 + 依赖解析 + 拓扑排序
├── src/app/
│   └── main.cpp                # 主程序（CAF 配置 + 启动流程）
├── plugins/                    # 示例插件
│   ├── logger/
│   └── business/
└── tests/
    └── test_dependency_graph.cpp
```

## 构建要求

- CMake >= 3.20
- C++20 编译器 (GCC 11+, Clang 14+, MSVC 2022+)
- vcpkg（manifest 模式自动安装 CAF）

## 构建步骤

### 使用 CMake Preset（推荐）

```bash
# 列出可用 preset
cmake --list-presets

# Windows (Visual Studio 2022)
cmake --preset windows-x64
cmake --build --preset windows-x64-release

# Windows (Ninja)
cmake --preset windows-x64-ninja
cmake --build --preset windows-x64-release

# Linux Debug
cmake --preset linux-x64-debug
cmake --build --preset linux-x64-debug

# Linux Release
cmake --preset linux-x64-release
cmake --build --preset linux-x64-release
```

### VS Code

安装 **CMake Tools** 扩展后，状态栏会自动识别 `CMakePresets.json`：

1. 点击状态栏的 **Configure Preset** → 选择 `windows-x64`（或 `linux-x64-release`）
2. 点击 **Build** → 自动编译
3. 点击 **Run** → 启动 `caf_plugin_app`

### 手动构建

#### Linux / macOS

```bash
export VCPKG_ROOT=/path/to/vcpkg
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel
./build/caf_plugin_app
```

#### Windows (Visual Studio 2022)

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg"
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake"
cmake --build build --config Release
.\build\Release\caf_plugin_app.exe
```

## 配置插件入口

在 `app.ini` 中声明顶层插件，系统自动解析依赖并拓扑排序加载：

```ini
[caf-plugin-system]
entry-plugins=["BusinessPlugin"]
```

运行时：
```bash
./caf_plugin_app --config-file=app.ini
```

## 跨平台动态库加载

项目内置 `DynamicLibrary` 类，封装了平台差异：

| 平台 | API |
|------|-----|
| Linux / macOS | `dlopen` / `dlsym` / `dlclose` |
| Windows | `LoadLibraryW` / `GetProcAddress` / `FreeLibrary` |

插件导出宏 `PLUGIN_API` 自动适配：
- Windows: `__declspec(dllexport)`
- Linux/macOS: `__attribute__((visibility("default")))`

## 设计要点

### 最小插件接口

插件只需实现 `PluginEntry`，业务交互完全通过 CAF 消息完成。

### 优雅关机流程

```
SIGTERM / Ctrl+C
  │
  ▼
[Drain]  停止接收新消息，处理完存量请求
  │
  ▼
[Save]   序列化状态 → CheckpointManager 落盘
  │
  ▼
[Exit]   按拓扑逆序卸载插件，退出 Actor
```

### 热更新

通过 Service Proxy 实现句柄不变、实现切换。

## License

MIT
