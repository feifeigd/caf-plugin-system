@echo off
title CAF Bridge Selftest (one-click)
cd /d "%~dp0.."

echo ============================================
echo  [0/3] Copy latest exe to run\
echo ============================================
copy /y out\build\windows-x64\src\app\Debug\caf_plugin_app.exe run\ >nul
if errorlevel 1 goto FAIL

echo ============================================
echo  [1/3] Start standalone bridge (48060, 180s)
echo ============================================
cd /d run
start "CAF Bridge" cmd /c "(ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe --caf-plugin-system.bridge-port=48060"

echo [2/3] Wait 8s for bridge...
timeout /t 8 /nobreak >nul

echo ============================================
echo  [3/3] Run TUI selftest
echo ============================================
set PYTHONIOENCODING=utf-8
"%~dp0.venv\Scripts\python.exe" "%~dp0cluster_tui.py" --selftest 127.0.0.1 48060

echo Expect: ALL PASS (connect / CALL OK / CALL ERR / REQ auto-reply)
echo Bridge window auto-exits in ~3 min (EXIT=0, 0 leak).
pause
exit /b 0

:FAIL
echo COPY FAILED - exe may be RUNNING (quit TUI / close bridge first) or build missing
pause
exit /b 1
