# VS Code IntelliSense / F12 跳转（2026-08-23）

现象：VS Code 里按 F12 不跳函数定义（悬停无类型提示、无补全）。

## 根因

- 项目没有 `c_cpp_properties.json`（C/C++ 扩展零配置）。
- 主构建目录 `out/build/windows-x64` 的生成器是 **Visual Studio 17 2022（vcxproj）**——**VS 生成器不支持 `CMAKE_EXPORT_COMPILE_COMMANDS`**，不可能从它导出 compile_commands.json。这是 F12 不工作的根本原因（MS C/C++ 扩展没有编译信息可用）。

## 解法：Ninja 专用 IntelliSense 目录（不用于构建）

仓库根已有 `setup_intellisense.bat`（改 CMakeLists 后重跑一次即可）：

```bat
call "...\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=...\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cmake -S . -B out\intellisense -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

要点：
- ninja.exe 在 VS 安装目录 `Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\`（无需单独装）。
- 根 CMakeLists **自动探测 vcpkg toolchain**（`CMakeLists.txt` 16/34 行，搜 candidate）→ 不用显式传 `CMAKE_TOOLCHAIN_FILE`。
- 首次 configure 会给新目录装 vcpkg 依赖（caf/spdlog/fmt/openssl…，实测 ~8 分钟）；之后增量秒级。
- 产物 = `out/intellisense/compile_commands.json`（本项目 21 个编译单元；每条命令含完整 MSVC + vcpkg_installed include 路径 + 各插件自己的 `PLUGIN_NAME` define）。

`.vscode/c_cpp_properties.json`（已提交）——compileCommands 指向 intellisense 目录，includePath/defines 全部由扩展从编译命令推导，无需手写：

```json
{
  "version": 4,
  "configurations": [
    {
      "name": "Win64 (MSVC via compile_commands)",
      "intelliSenseMode": "windows-msvc-x64",
      "compileCommands": "${workspaceFolder}/out/intellisense/compile_commands.json",
      "cppStandard": "c++20",
      "cStandard": "c17"
    }
  ]
}
```

## 生效步骤（VS Code 里）

1. `Ctrl+Shift+P` → **Developer: Reload Window**（让扩展重读配置）
2. 还不行 → **C/C++: Reset IntelliSense Database**
3. F12 / 悬停类型 / 补全即恢复（核心 + 插件都能跳）

## 排查

- 先确认 `out/intellisense/compile_commands.json` 存在（改 CMakeLists 后没重跑 bat = 旧的）。
- compile_commands 的 file 字段是 Windows 路径（`G:/...`），在 Windows VS Code 打开工程（G:\git\caf-plugin-system）最稳；WSL Remote 打开 /mnt/g 路径时扩展解析可能不一致。
