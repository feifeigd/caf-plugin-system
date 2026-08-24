# 控制台关闭按钮（X）自动化验证方法

2026-08-23 会话沉淀：修复"点 X 被 conhost ~0ms 强杀"时开发的完整测试方法。
手动点 X 窗口一闪而过没法看输出，这套方法把 X-click 路径变成可重复的自动化验证。

## 1. 模拟真实点 X：SC_CLOSE

真实点 X = 系统菜单的 `SC_CLOSE`，即 `WM_SYSCOMMAND`(0x0112) + `SC_CLOSE`(0xF060)。
用 PostMessage 发给控制台窗口句柄（`$p.MainWindowHandle`）。CloseMainWindow()
发的是 WM_CLOSE，两者都会触发 CTRL_CLOSE_EVENT，但 SC_CLOSE 才是 X 的等价消息。

```powershell
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
'@
[W]::PostMessage($hwnd, 0x0112, 0xF060, 0)   # $hwnd = $p.MainWindowHandle
```

## 2. 启动方式（真实度排序）

- **wscript + VBS**（最接近双击）：交互会话、可设 CWD、真实 conhost 窗口。
  ```vbs
  Set sh = CreateObject("WScript.Shell")
  sh.CurrentDirectory = "G:\git\caf-plugin-system\out\build\windows-x64\run"
  sh.Run """G:\git\caf-plugin-system\out\build\windows-x64\run\caf_plugin_app.exe""", 1, False
  ```
- **Start-Process -WorkingDirectory ... -PassThru**：CWD 可控，且能直接拿退出码
  （`$p.WaitForExit(15000)` 后 `$p.ExitCode`）。注意：从 `Get-Process` 拿的
  Process 对象退出后 `ExitCode` 是**空**的——必须用 Start-Process 的 -PassThru。
- **explorer.exe 'path\exe'**：也是双击等价，但 **CWD 不可控**（实测文件写到
  别处，验证落盘/trace 时别用 explorer 启动）。
- 环境警告：WSL→PowerShell 拉起的控制台（ConPTY 链）在旧代码（handler 立即返回）
  下点 X 是 **0ms 强杀、无 5s 宽限**——比用户真实双击更严苛，但正好能暴露 bug；
  修复后（handler 阻塞 + ExitProcess）任何启动方式都能活到链完成。

## 3. 验证指标（修复后的通过标准）

1. 退出码 = **0**（ExitProcess(0)），不是 0xC000013A（STATUS_CONTROL_C_EXIT）。
2. `shutdown-trace.log` 完整链：`[signal] CTRL_CLOSE_EVENT...` → `shutdown_atom
   RECEIVED` → `Graceful shutdown initiated` → 每个插件 `resolved→drained→saved→
   checkpointed` → `All plugins saved` → `STOPPED` → `[signal] shutdown chain
   completed; ExitProcess(0)`。
3. `checkpoints/*.ckpt` mtime 更新 = 数据真落盘。
4. 进程无残留：`Get-Process caf_plugin_app` 为空。

## 4. 环境探针（对照实验）：这台机器/这种启动方式给不给 5s 宽限

裸控制台程序验证 conhost 行为——**与业务代码无关，先确认系统机制**：

```cpp
#include <windows.h>
#include <cstdio>
static BOOL WINAPI handler(DWORD sig) {
    FILE* f = fopen("handler.log", "a");
    if (f) { fprintf(f, "signal=%lu t=%lu BLOCKING 8s\n", (unsigned long)sig,
                     (unsigned long)GetTickCount()); fclose(f); }
    Sleep(8000);   // 阻塞：测 conhost 是否等待
    return TRUE;
}
int main() {
    SetConsoleCtrlHandler(handler, TRUE);
    for (int i = 0; i < 200; i++) { Sleep(50); /* 每 200ms 写 alive 行 */ }
    return 0;
}
```

判读（handler.log + 进程存活时长）：
- handler 立即返回 + SC_CLOSE → **0ms 被杀** = 该环境无宽限 → 异步关机链必死，
  只能"handler 阻塞等链完成 + ExitProcess(0)"。
- handler 阻塞 8s → **存活 ~4.6s** = 宽限存在（conhost 等 handler/进程约 5s）。

编译（WSL 里 cmd 内联引号会坏，写 .bat 再跑）：
```bat
@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
cd /d G:\git\caf-plugin-system\out\build\windows-x64\run
cl /nologo /EHsc /Fe:ctrl_test.exe ctrl_test.cpp
```

## 5. 构建命令（WSL 侧）

WSL PATH 里没有 cmake，用 Windows 版 CMake：
```
"/mnt/c/Program Files/CMake/bin/cmake.exe" --build out/build/windows-x64 --config Debug --target caf_plugin_app
```
改源码后必须 `touch` 改过的文件（drvfs mtime 秒级粒度，MSVC 会漏编译）；
构建完 cp 到 run/ 后 `strings -a exe | grep -c "<新标记>"` 验证新代码真进去了。

## 6. 本次修复要点（代码落点，供复盘）

- `framework_bootstrap.cpp`：`notify_shutdown_complete()` / `wait_for_shutdown_complete()`
  （mutex+cv）；CTRL_CLOSE/LOGOFF/SHUTDOWN 三个 case：置 console_closing →
  fd 重定向 NUL → trace_signal → anon_send(shutdown_atom) → **阻塞等链完成
  （4s 超时兜底）→ ExitProcess(0)**。CTRL_C/BREAK 不变（console 还活着）。
- `graceful_shutdown.cpp`：`finish_shutdown()` 末尾 `notify_shutdown_complete()`；
  shutdown_atom 重复触发保护——`shutting_down` 状态直接忽略
  （trace：`[state] already shutting down, ignore duplicate`）。
- ExitProcess 安全前提：CheckpointManager 是 tmp 文件 + flush + close + 原子
  rename，链完成时数据已在磁盘（ExitProcess 不跑 atexit、不刷 stdio 缓冲）。
