@echo off
rem ============================================================
rem  IntelliSense 专用配置目录（out/intellisense）
rem  作用：生成 compile_commands.json 供 VS Code F12 跳转使用。
rem  注意：此目录不用来构建/运行，真正的构建仍在 out/build/windows-x64。
rem  原因：Visual Studio 生成器不支持 CMAKE_EXPORT_COMPILE_COMMANDS，
rem        必须用 Ninja 生成器导出（详见 docs/windows-shutdown-experience.md）。
rem  用法：双击，或 cmd /c setup_intellisense.bat
rem ============================================================
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%
cd /d G:\git\caf-plugin-system

cmake -S . -B out\intellisense -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
if exist out\intellisense\compile_commands.json (
    echo.
    echo [OK] compile_commands.json generated - VS Code F12 should work now
) else (
    echo.
    echo [FAIL] compile_commands.json not found - check errors above
)
