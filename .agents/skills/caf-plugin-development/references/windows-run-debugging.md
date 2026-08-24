# Windows 运行与调试 caf-plugin-system（实测 2026-08）

## run/ 目录组装（全链路验证）

main.cpp 用相对路径 `./plugins`、`./updates/business_plugin_v2.dll`、`./checkpoints`、`./logs`。
插件扫描（`scan_all_plugins`）是两级结构 `root/<name>/*.dll`；MSVC 多配置把 DLL 输出到
`Debug/` 子目录——直接指向构建树会扫到 0 个插件（"No plugins found" → 提前 shutdown）。

```powershell
$b='G:\git\caf-plugin-system\out\build\windows-x64'; $r="$b\run"
New-Item -ItemType Directory -Force "$r\plugins\logger","$r\plugins\business","$r\plugins\platform","$r\updates" | Out-Null
Copy-Item "$b\src\app\Debug\caf_plugin_app.exe" "$r"
Copy-Item "$b\src\app\Debug\caf_core.dll" "$r"
# 运行时依赖：插件 DLL 的 LoadLibrary 需要，缺了 probe open 静默失败（无错误文本）
Copy-Item "$b\vcpkg_installed\vcpkg\pkgs\fmt_x64-windows\bin\fmt.dll" "$r"
Copy-Item "$b\vcpkg_installed\vcpkg\pkgs\spdlog_x64-windows\bin\spdlog.dll" "$r"
# Debug 构建可能需要 debug 版：pkgs\fmt_x64-windows\debug\bin\fmtd.dll、spdlogd.dll
Copy-Item "$b\plugins\logger\Debug\logger_plugin.dll" "$r\plugins\logger"
Copy-Item "$b\plugins\business\Debug\business_plugin.dll" "$r\plugins\business"
Copy-Item "$b\plugins\platform\Debug\platform_plugin.dll" "$r\plugins\platform"
Copy-Item "$b\plugins\business_v2\Debug\business_plugin_v2.dll" "$r\updates"
```

## CAF 1.1 配置文件格式（花括号嵌套块，JSON-like；2026-08 实测修正）

默认配置文件 `caf-application.conf`——exe **无参数直接跑**即自动加载（`strings -a caf_core.dll | grep caf-application` 实锤默认文件名），`--config-file=xxx` 可覆盖。规范格式是花括号嵌套块：

```conf
caf-plugin-system {
  entry-plugins = ["BusinessPlugin"]
  test-auto-shutdown = true
}
```

- **注释符是 `#`，不是 `;`**——`;` 报 `invalid syntax in line 1 column 1`（line 1 即 `;` 所在行；旧 app.ini 里的 `;` 注释是历史遗留，须改成 `#`）
- 花括号块等价于点号路径（`caf-plugin-system { entry-plugins }` ≡ `caf-plugin-system.entry-plugins`）；点号键可解析但非规范写法
- 实测矩阵：花括号块+`#`注释 ✓ / 花括号块+`;`注释 ✗ / 花括号块无注释 ✓ / 点号键 ✓
- `[section]` ini 语法 → 解析失败；字符串值必须带引号（`node-kind = "worker"`），数组 `["A","B"]`，数字/布尔不带引号
- CLI：`--caf-plugin-system.entry-plugins=...` 名字被识别（值格式错 = invalid_argument）；
  `--entry-plugins=...`（无 group 前缀）→ not_an_option。vector 值在 CLI 易踩，用配置文件最稳。
- **Windows exe 不认 WSL `/tmp` 路径**：`--caf.logger.file.path=/tmp/x.log` 会被解析成盘符根目录（如 G:\tmp\...），日志"消失"——日志路径一律用 run/ 下相对路径（master.log / worker.log）

## 编译错误被输出过滤器吞掉（stale binary 的第二来源）

- 症状：跑起来"卡住"、连新加的调试打印都不出现——但残留进程已杀、exe 也拷了。
- 原因：构建命令的输出被 `grep/Select-String -Pattern 'error'` 过滤后**看起来成功了**，实际编译失败
  （本次实锤：receive lambda 用了循环变量 `name` 但捕获列表是 `[]` → C3493 无法隐式捕获；
  过滤后的构建输出为空，Copy-Item 拷走的是旧 exe）。
- 修复顺序：① 杀残留进程 ② 构建时**看原始输出的最后几行**（`Select-Object -Last 4`，确认
  `vcxproj ->` 行出现且无 error 行）③ 拷贝后 `strings -a exe | grep -c "<新标记>"` 验证新代码
  真的在二进制里，再开始运行时调试。运行期零输出的"卡住"先怀疑 stale binary，再怀疑业务逻辑。

## 残留进程锁 exe（浪费过大量轮次的坑）

- 症状：改代码 → 构建 → 拷贝 → 运行，输出却是旧行为；`cp` 报 `Permission denied`。
- 原因：后台跑的 `caf_plugin_app.exe` 进程锁住 exe/DLL，Copy-Item 静默失败或失败，
  实际在跑旧二进制（可能 strings 里根本没有你新加的调试标记）。
- 修复：
  ```bash
  powershell.exe -NoProfile -Command "Get-Process caf_plugin_app -ErrorAction SilentlyContinue | Stop-Process -Force"
  # 拷贝后验证部署的二进制版本：
  strings -a run/caf_plugin_app.exe | grep -c "<调试标记>"
  ```

## C1041 PDB 锁（kill 后台构建后，2026-08-23 实测）

- 症状：kill 掉后台 MSVC 构建（`process kill` 杀掉 bash/cmd 管道进程）后，下一次构建报
  `error C1041: 无法打开程序数据库 ...\vc143.pdb；如果要将多个 CL.EXE 写入同一个 .PDB 文件，请使用 /FS`——
  两三个 .cpp 的 C1041 一起出现且与源码无关（grep 构建输出只见 error C1041，无真实语法错误）。
- 原因：kill 只杀了管道进程，**孤儿 cl.exe 继续跑**，占着 `vc143.pdb`（MSBuild 多 cl 并发写同一
  PDB，需 `/FS`；孤儿进程的 PDB 句柄不释放 → 新 cl 打不开）。
- 恢复顺序：
  1. `cmd.exe /c "tasklist | findstr cl.exe"`（或 `findstr /i "cl.exe MSBuild.exe mspdbsrv"`）确认孤儿；
     若 cl/MSBuild 已自然退出则锁多半已释放，直接重构建即可（本会话曾出现 cl/MSBuild 全退、只剩
     mspdbsrv.exe 还活着但锁已释放的情况——先试构建，别再报 C1041 才需要杀）。
  2. 仍锁着才杀：`taskkill /F /IM cl.exe /T` + `taskkill /F /IM MSBuild.exe /T` +
     `taskkill /F /IM mspdbsrv.exe /T`（mspdbsrv = PDB server，常驻持锁；cl/MSBuild 退干净后它可能还在）。
  3. 删残留 `vc143.pdb`（`find out/build/windows-x64 -name "vc143.pdb" -delete`），重构建。
- **taskkill 属强杀类命令，WSL 侧会被安全确认拦截（BLOCKED: user consent）**——不要反复重试，
  直接把命令给用户在 Windows 终端跑，或等用户同意后再执行。
- 预防：杀后台构建用 `process kill` 后顺手 `tasklist | findstr cl.exe` 检查一遍，确认没有孤儿再开新构建。

## 业务日志静默丢失

CAF_LOG_*（main 和插件的）在 CAF 1.1 下可能完全不出现在日志（组件/verbosity 配置问题），
framework log 只有 caf.core / caf.system 行。调试期用 `std::cout << ... << std::endl`
（必须 endl 强制 flush——强杀进程时未 flush 的缓冲直接丢）。

## 全链路验证运行

```powershell
Set-Location run
.\caf_plugin_app.exe --config-file=app.ini
```

期望顺序：3 插件加载 → `[System] State: READY` → `[ACL test] blocked as expected`
→ `reload result: 1` → `processed by v2: hello` → `Graceful shutdown initiated`
→ `All plugins saved`。程序正常等待 Ctrl+C；自动关机需 `test-auto-shutdown = true`。
