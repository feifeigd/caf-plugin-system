@echo off
title CAF Cluster Test (master + bridge + TUI)
cd /d "%~dp0.."

echo ============================================
echo  [0/4] Copy latest exe to run\ and run_bridge\
echo ============================================
copy /y out\build\windows-x64\src\app\Debug\caf_plugin_app.exe run\ >nul
if errorlevel 1 goto FAIL
if not exist run_bridge mkdir run_bridge
for %%f in (caf_plugin_app.exe caf_core.dll caf_io.dll fmtd.dll spdlogd.dll caf-application.conf) do (
    copy /y run\%%f run_bridge\ >nul
    if errorlevel 1 goto FAIL
)
if not exist run_bridge\plugins xcopy /e /i /y run\plugins run_bridge\plugins >nul

echo ============================================
echo  [1/4] Start master (47096, test-bridge-call)
echo ============================================
cd /d run
start "CAF Master" cmd /c "(ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe --caf-plugin-system.node-kind=master --caf-plugin-system.node-name=master --caf-plugin-system.node-port=47096 --caf-plugin-system.lease-seconds=6 --caf-plugin-system.test-bridge-call=bridge-a"

echo ============================================
echo  [2/4] Start bridge worker (47097, bridge 48063)
echo ============================================
start "CAF Bridge" cmd /c "cd /d "%~dp0..\run_bridge" && (ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe --caf-plugin-system.node-kind=worker --caf-plugin-system.node-name=bridge-a --caf-plugin-system.node-port=47097 --caf-plugin-system.master-port=47096 --caf-plugin-system.parent=master --caf-plugin-system.lease-seconds=6 --caf-plugin-system.bridge-port=48063"

echo [3/4] Wait 12s for cluster...
timeout /t 12 /nobreak >nul

echo ============================================
echo  [4/4] Start TUI (keep ~30s for cross-node calls)
echo ============================================
set PYTHONIOENCODING=utf-8
"%~dp0.venv\Scripts\python.exe" "%~dp0cluster_tui.py" 127.0.0.1 48063

echo Check: TUI shows yellow REQ + auto-reply;
echo        master window shows [BridgeTest] external_echo@'bridge-a' -> echo:bridge-ping
echo Master/Bridge windows auto-exit in ~3 min.
pause
exit /b 0

:FAIL
echo COPY FAILED - build first (cmake --build out/build/windows-x64 --config Debug)
pause
exit /b 1
