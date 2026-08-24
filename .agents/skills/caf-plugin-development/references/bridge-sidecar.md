# Bridge —— 外部语言节点（Python/Go）sidecar 适配器

> 2026-08-24 实施并全链路验证（master + bridge 双进程 0 泄露）。
> 背景：集群是纯 CAF（BASP）节点，外部语言进程无法直接跑 CAF actor。
> 方案 B：每个外部节点配一个 CAF bridge 进程（sidecar），外部进程经
> 本地 TCP 行协议与 bridge 通信，bridge 以正常 worker 身份注册进集群。

## 架构

```
外部进程(Python/Go) ──TCP 行协议──> bridge 进程(caf_plugin_app)
                                     │  ├─ 正常 worker 身份注册 master
                                     │  ├─ external_echo 服务（handler 在外部进程）
                                     │  └─ bridge actor（外部 CALL → resolve → 信封调用）
                                     │
master ──BASP──> bridge 的 external_echo（集群→外部）
```

- bridge 就是 `caf_plugin_app --caf-plugin-system.bridge-port=<port>`
- 服务注册：`external_echo`（impl actor 在 bridge 进程，转发外部）
- 调用协议：全部复用 `plugin_envelope`（sub_proto=1 私有协议），跨节点
  `cross_call(svc, node, env)` 直达 bridge 的 external_echo

## 行协议（\n 分隔行 + 长度前缀 payload，可含任意字节）

```
外部 → bridge:  CALL <id> <svc> <len>\n<payload>\n   调本地服务
bridge → 外部:  RESULT <id> OK|ERR <len>\n<payload>\n   CALL 响应
bridge → 外部:  REQ <rid> <len>\n<payload>\n           集群→外部请求
外部 → bridge:  RESULT <rid> OK|ERR <len>\n<payload>\n REQ 响应
```

- id/rid 独立命名空间：CALL 的 id 由外部定（bridge 原样回显）；
  REQ 的 rid 由 impl actor 自增（外部回 RESULT 时带 rid）
- 无 JSON 依赖（行协议自解析，~50 行）；长度前缀防 payload 含空格/\n
- Python 参考客户端：`tools/bridge_client.py`（Windows 侧跑：
  `C:\...\Python314\python.exe G:\git\...\tools\bridge_client.py 127.0.0.1 <port>`）
  Go 侧实现同协议（socket + 行解析 + threading/goroutine）

## 验证命令（复现，2026-08-24 run8 全绿）

```
# master（180s 延迟 EOF + 跨节点调 external_echo 后门）
cd run8 && cmd.exe /c "(ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe \
  --caf-plugin-system.node-kind=master --caf-plugin-system.node-name=master \
  --caf-plugin-system.node-port=47060 --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.test-bridge-call=bridge-a"

# bridge（180s 延迟 EOF + worker + bridge-port）
sleep 3 && cd run8b && cmd.exe /c "(ping -n 181 127.0.0.1 >nul & echo x) | caf_plugin_app.exe \
  --caf-plugin-system.node-kind=worker --caf-plugin-system.node-name=bridge-a \
  --caf-plugin-system.node-port=47061 --caf-plugin-system.master-port=47060 \
  --caf-plugin-system.parent=master --caf-plugin-system.lease-seconds=6 \
  --caf-plugin-system.bridge-port=48060"

# Python 客户端（Windows 侧，跑 25s：CALL business_service / external_echo + 等 REQ）
python.exe tools\bridge_client.py 127.0.0.1 48060
```

预期结果（run8 实测）：
- `[BridgeTest] external_echo@'bridge-a' -> echo:bridge-ping`（master 跨节点成功）
- Python：`external_echo -> (True, 'echo:self-ping')`（桥内 round-trip）
- Python：`business_service -> ERR: ACL: sender not trusted`（**链路通**，ACL 是
  business_service 的安全策略——外部节点调受保护服务需配置 ACL 白名单）
- 双方 EOF 关机 EXIT=0 + CRT dump 0 + 残留进程 0

## 坑（全部实测踩过）

1. **actor 写"主 wq" vs 写线程消费"连接 wq"** → respond/REQ 全进无人消费的
   队列 → 外部进程永远等不到响应（Python 15s 超时、master request_timeout）。
   必须经 `ActiveConn::push()` 转发到【当前连接】的 wq。
2. **`addr.sin_port = htons(port)` 遗漏** → sockaddr_in{} 零初始化 → bind 随机
   端口（日志打印 48000 但 netstat 无此端口）。设端口后必须自查 getsockname。
3. **detached 线程强杀不 unwind** → 栈上 shared_ptr 泄漏（CRT dump 报
   "REQ ..." 残留）。线程全部登记到 ActiveConn，bridge actor exit 时
   `shutdown_and_join()`：close 全部 fd + wq->close() → 阻塞点
   （accept/recv/pop）返回 → join → 0 泄露。
4. **bridge 必须在节点引导【之前】spawn** → bootstrap_node 的 exported_actors
   只查一次，注册晚了 manifest 上报不含 external_echo → master 路由
   no_such_key。注册后【同步确认】（同 sender FIFO：send register →
   request list_services 确认在台账）。
5. **Python 客户端 `CALL` 行必须带 svc**（`CALL <id> <svc> <len>`，RESULT 才是
   `<id> OK|ERR <len>`）——漏了 svc → bridge 解析 sp2=npos 断连。
6. **Python reader 线程必须在 call() 之前启动**（构造时启动）——RESULT 靠它
   消费并 set pending 事件，否则 call 必超时。
7. **测试后门的 RemoteCaller 必须 send_exit**（函数末尾）——不杀则
   actor_system 析构 join 常驻 actor → 关机链 STOPPED 后进程挂起。
8. **bridge 主 actor exit 时连 impl 一起 send_exit**——registry 关机只杀
   proxy 不杀 impl，impl 无人 send_exit → 析构 join 挂起。
9. **`caf::response_promise` 拷贝构造是 private**（friend typed_response_promise）
   → map 存不了，用 `caf::typed_response_promise<T>`（public 可拷贝）。
10. **WSL→Windows 连接**：WSL 的 127.0.0.1 是 WSL 自己的 loopback，连不到
    Windows 监听；宿主 IP（172.19.128.1）被 Windows 防火墙拦。
    测试用 Windows 侧进程（python.exe 由 cmd 启动）连 127.0.0.1。
11. **cmd 括号管道偶发延迟启动**：`(ping & echo x) | app` 在部分轮次
    app 延迟 ~160s（ping 结束后才启动）——用 180s+ 窗口兜底，别用 90s。

## 线程模型（v1 单连接语义）

- listener 线程：accept → 每连接 写线程 + 读线程；连接 wq 注册为"当前连接"
- 读线程：read_line → CALL 投 bridge actor / RESULT 投 impl actor
  （捕获 actor_addr 弱引用，发送前升级——线程强杀不保活 actor）
- 写线程：pop 连接 wq → send_all（actor 经 active->push 入队）
- bridge actor exit：send_exit(impl) → active->shutdown_and_join() → quit
