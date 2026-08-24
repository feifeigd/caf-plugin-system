@echo off
title CAF TUI Console (interactive)
cd /d "%~dp0.."

echo ============================================
echo  Copy latest exe + start bridge + TUI
echo ============================================
copy /y out\build\windows-x64\src\app\Debug\caf_plugin_app.exe run\ >nul
if errorlevel 1 goto FAIL

cd /d run
start "CAF Bridge" cmd /c "(ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe --caf-plugin-system.bridge-port=48060"

echo Wait 8s for bridge...
timeout /t 8 /nobreak >nul

echo Starting TUI console...
echo Commands: call svc payload / callhex svc hex / auto-reply on or off / clear / quit
set PYTHONIOENCODING=utf-8
"%~dp0.venv\Scripts\python.exe" "%~dp0cluster_tui.py" 127.0.0.1 48060

echo TUI exited. Bridge window auto-closes in ~3 min (or Ctrl+C).
pause
exit /b 0

:FAIL
echo COPY FAILED - build first (cmake --build out/build/windows-x64 --config Debug)
pause
exit /b 1
