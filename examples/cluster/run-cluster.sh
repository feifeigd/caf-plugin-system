#!/bin/bash
# 一键拉起带插件的集群：master + worker-a + worker-b
# 用法：./run-cluster.sh   （在 run/ 目录下执行）
set -u
cd "$(dirname "$0")"
mkdir -p logs

# 0) 清残留进程
powershell.exe -NoProfile -Command 'Get-Process caf_plugin_app -ErrorAction SilentlyContinue | Stop-Process -Force' 2>/dev/null
rm -f logs/*.out

echo "==> 启动 master（节点 + BusinessPlugin）..."
./caf_plugin_app.exe --config-file=master.conf > logs/master.out 2>&1 &
sleep 2

echo "==> 启动 worker-a（节点 + Business/Logger 插件）..."
./caf_plugin_app.exe --config-file=worker-a.conf > logs/worker-a.out 2>&1 &
echo "==> 启动 worker-b（节点 + BusinessPlugin）..."
./caf_plugin_app.exe --config-file=worker-b.conf > logs/worker-b.out 2>&1 &
sleep 8

echo ""
echo "================ 集群状态 ================"
echo "--- master: 插件初始化 + 节点注册 ---"
grep -aE "BusinessPlugin initialized|registered node|went down" logs/master.out | head -8
echo ""
echo "--- worker-a: 插件初始化 + 注册 ---"
grep -aE "LoggerPlugin initialized|BusinessPlugin initialized|READY|register: OK" logs/worker-a.out | head -8
echo ""
echo "--- worker-b: 插件初始化 + 注册 ---"
grep -aE "BusinessPlugin initialized|READY|register: OK" logs/worker-b.out | head -6

echo ""
echo "==> 停止集群（杀全部 caf_plugin_app 进程）..."
powershell.exe -NoProfile -Command 'Get-Process caf_plugin_app -ErrorAction SilentlyContinue | Stop-Process -Force' 2>/dev/null
echo "集群演示完毕。"
