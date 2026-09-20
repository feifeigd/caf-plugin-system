@echo off
rem 打包 caf-plugin-sdk：从最新构建树组装插件开发 SDK
rem 用法：sdk\package.bat   （先确保已构建 caf_plugin_core）
setlocal
set ROOT=%~dp0..
cmake -P "%ROOT%\sdk\package.cmake"
if errorlevel 1 exit /b 1
echo.
echo SDK updated at %ROOT%\caf-plugin-sdk
