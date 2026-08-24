#!/usr/bin/env python3
"""caf-plugin-system bridge 行协议客户端 —— 外部语言节点（Python 版）示例。

用法：
    python3 bridge_client.py [host] [port]

行为：
  1. 连接 bridge（本地 TCP 行协议）
  2. 读线程：处理 REQ（external_echo 的 handler 在这里，echo 实现）
             并匹配 CALL 的 RESULT
  3. 发 CALL business_service  （验证 外部→集群 本地服务）
  4. 发 CALL external_echo     （桥内 round-trip：CALL→REQ→本 handler→RESULT）
  5. 保持连接等待 master 的 --test-bridge-call 跨节点调用打过来（集群→外部）

行协议（\n 分隔；payload 长度前缀，可含任意字节）：
  外部 → bridge:  CALL <id> <svc> <len>\n<payload>\n
  bridge → 外部:  RESULT <id> OK|ERR <len>\n<payload>\n
  
  bridge → 外部:  REQ <rid> <len>\n<payload>\n
  外部 → bridge:  RESULT <rid> OK|ERR <len>\n<payload>\n
"""
import socket
import sys
import threading
import time

from bridge_proto import (  # noqa: E402
    BadFrame,
    LineReader,
    make_call_frame,
    make_result_frame,
    read_frame,
)


class BridgeClient:
    def __init__(self, host, port, handler=None):
        self.s = socket.create_connection((host, port), timeout=60)
        self.reader = LineReader(self.s)
        self.lock = threading.Lock()
        self.pending = {}  # id -> [event, result]
        self.next_id = 1
        self.handler = handler or (lambda p: "echo:" + p)
        self.req_count = 0
        self.resp_count = 0
        # reader 线程必须在任何 call() 之前启动：RESULT 靠它消费并 set
        # pending 事件（之前放 run() 里，call 期间无人读 socket → 必超时）
        threading.Thread(target=self._reader, daemon=True).start()

    def _send_call(self, cid, svc, payload):
        # 行协议：CALL <id> <svc> <len>\n<payload>\n（svc 必须带！
        # 之前写成 RESULT 的格式漏了 svc → bridge 解析 sp2=npos 断连）
        self.s.sendall(make_call_frame(cid, svc, payload))

    def _send_result(self, rid, ok, payload):
        # 行协议：RESULT <id> OK|ERR <len>\n<payload>\n
        self.s.sendall(make_result_frame(rid, ok, payload))

    def call(self, svc, payload, timeout=15):
        with self.lock:
            cid = self.next_id
            self.next_id += 1
            self.pending[cid] = [threading.Event(), None]
        self._send_call(cid, svc, payload.encode())
        ev, _ = self.pending[cid]
        if not ev.wait(timeout):
            with self.lock:
                self.pending.pop(cid, None)
            return (False, "timeout")
        with self.lock:
            res = self.pending.pop(cid)[1]
        return res

    def _reader(self):
        try:
            while True:
                try:
                    fr = read_frame(self.reader)
                except BadFrame:
                    continue  # 坏帧跳过（协议异常）
                if fr is None:
                    print("[client] connection closed by bridge")
                    break
                kind, ident, third, payload = fr
                if kind == "REQ": # bridge → 外部:  REQ <rid> <len>\n<payload>\n
                    # 集群 → 外部：external_echo 的 Python 实现在这
                    body = self.handler(payload.decode("utf-8", "replace"))
                    if isinstance(body, str):
                        ok, out = True, body.encode()
                    else:
                        ok, out = body
                    self._send_result(ident, ok, out)
                    self.req_count += 1
                    print(f"[client] REQ {ident} handled -> {out!r}")
                elif kind == "RESULT":
                    ok = third == "OK"
                    cid = int(ident) if ident.isdigit() else None
                    with self.lock:
                        ev = self.pending.get(cid) if cid is not None else None
                        if ev:
                            ev[1] = (ok, payload.decode("utf-8", "replace"))
                            ev[0].set() # 触发事件
                    self.resp_count += 1
                    print(f"[client] RESULT {ident} "
                          f"{'OK' if ok else 'ERR'}: "
                          f"{payload.decode('utf-8', 'replace')[:80]!r}")
        except OSError:
            pass  # 连接被关闭/中止（正常退出路径）

    def run(self, seconds):
        time.sleep(seconds)
        try:
            self.s.close()
        except OSError:
            pass
        print(f"[client] done: {self.req_count} REQ handled, "
              f"{self.resp_count} RESULT received")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 48000

    def echo_handler(payload):
        # external_echo 服务的外部实现：echo 前缀
        return "echo:" + payload

    c = BridgeClient(host, port, echo_handler)
    print(f"[client] connected to bridge at {host}:{port}")

    # 测 1：外部 → 集群（bridge 本地服务；bridge 进程加载了 business 插件）
    r = c.call("business_service", "hello-from-python")
    print(f"[client] business_service -> {r}")

    # 测 2：桥内 round-trip（external_echo 由本进程 handler 处理）
    r = c.call("external_echo", "self-ping")
    print(f"[client] external_echo -> {r}")

    # 测 3：保持连接，等 master 的 --test-bridge-call 跨节点调用
    # （集群 → bridge → 本进程 REQ → echo 回 RESULT → 调用方）
    print("[client] waiting for cross-node calls from master (25s)...")
    c.run(25)


if __name__ == "__main__":
    main()
