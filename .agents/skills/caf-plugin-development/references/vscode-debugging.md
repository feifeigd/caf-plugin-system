# VS Code F5 断点调试（caf_plugin_app）

Windows 侧 VS Code + C/C++ 扩展 + `cppvsdbg`（VS 调试后端，直接吃 MSVC PDB）。
配置在 `G:\git\caf-plugin-system\.vscode\{launch,tasks}.json`（4 个配置：master / worker-a / worker-b / standalone）。

## launch.json 要点

- `"type": "cppvsdbg"` —— VS 调试后端，吃 MSVC PDB，断点/变量/调用栈齐全
- `"program"` 指向 **MSVC 真实产物** `out/build/windows-x64/src/app/Debug/caf_plugin_app.exe`（不是 run/ 拷贝 → 免手动同步 exe）
- `"cwd"` **必须是** `out/build/windows-x64/run/` —— 插件扫描 `./plugins`、配置文件、checkpoints 全相对它；cwd 错 = 插件加载失败/秒退零输出
- `"args": ["--config-file=master.conf"]` 集群调试；standalone 用默认 caf-application.conf
- `"preLaunchTask": "cmake-build-debug"`（tasks.json 构建任务，F5 一条龙）

## tasks.json 要点

- **`"type": "process"`**（直接 CreateProcess）—— `"type": "shell"` 在 PowerShell 下会把 command 里的引号当字面量 → preLaunchTask 报 exit 1
- `"command"` 裸路径不带引号：`C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe`
- `"args": ["--build", "out/build/windows-x64", "--config", "Debug"]`，`"problemMatcher": ["$msCompile"]`

## stdin 交互调试（关键坑）

- **cppvsdbg 的 Debug Console 不支持往被调试程序喂 stdin** —— 敲 `help` 没反应（控制台线程 `getline(stdin)` 拿不到输入，程序 stdin 在调试器下是空的）
- 修法：`"console": "externalTerminal"` —— 程序在**独立 Windows 控制台窗口**跑，stdin/stdout 完整，可直接交互输 ops 命令（help/list/reload/reload-all/...）
- `integratedTerminal` 对 cppvsdbg 的 stdin 支持不稳定，别用

## 坑速查

- **PDB 锁（C1090 vc143.pdb 写入失败）**：VS Code 调试会话/构建占着 PDB 时，WSL 侧同时构建会报错——停掉调试会话重试即可（瞬时锁，非永久问题）
- 集群调试：master 一个 F5 会话，worker 另开会话/终端，多会话并行
- 断点建议：`plugin_manager.cpp` 的 `reload_atom` handler、`ops_actor.cpp` 的 `handle_remote_reload` —— 跟热更全流程（quiesce → save_state → LoadLibrary 新路径 → 台账 → resume → send_exit）
