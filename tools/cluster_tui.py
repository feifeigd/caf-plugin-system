#!/usr/bin/env python3
"""cluster_tui.py —— CAF 集群控制台 TUI（Textual）。

连接 bridge sidecar（本地 TCP 行协议），可视化控制 CAF 集群：
  - 连接面板：host/port/状态灯/统计
  - 彩色协议日志：CALL/RESULT/REQ 帧流
  - 底部命令栏：发任意 CALL（任意服务名 + 任意 payload，字节级透传）
  - 集群→外部 REQ 自动 echo（可关）

用法：
    python cluster_tui.py [host] [port]             # 交互模式
    python cluster_tui.py --selftest [host] [port]  # 无头自测（CI/验证用）

命令（交互模式底部输入）：
    connect <host> <port>     连接 bridge
    disconnect                断开
    call <svc> <payload...>   调集群服务（payload 原样透传）
    auto-reply on|off         REQ 自动回复开关（默认 on）
    services                  已调用服务统计
    clear                     清日志
    help                      帮助
    quit                      退出
"""

import queue
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

# ---------------------------------------------------------------- 协议层


class BridgeLink:
    """bridge 行协议连接。所有事件经 callback 队列回 UI 线程。"""

    def __init__(self, on_event):
        self.on_event = on_event  # callback(event_dict)
        self.s = None
        self.reader = None
        self.lock = threading.Lock()
        self.pending = {}  # cid -> {"svc": str, "t0": float}
        self.next_id = 1
        self.req_count = 0
        self.resp_count = 0
        self.err_count = 0
        self.auto_reply = True
        self._rthread = None
        self._timers = []

    # -- 状态 ------------------------------------------------------
    @property
    def connected(self):
        return self.s is not None

    # -- 连接 ------------------------------------------------------
    def connect(self, host, port):
        if self.connected:
            self.disconnect()
        try:
            s = socket.create_connection((host, port), timeout=15)
            s.settimeout(None)
        except OSError as e:
            self.on_event({"type": "err", "msg": f"connect {host}:{port} failed: {e}"})
            return False
        self.s = s
        self.reader = LineReader(s)
        self._rthread = threading.Thread(target=self._reader, daemon=True)
        self._rthread.start()
        self.on_event({"type": "sys", "msg": f"connected to bridge {host}:{port}"})
        return True

    def disconnect(self):
        s, self.s = self.s, None
        if s is not None:
            try:
                s.close()
            except OSError:
                pass
        self.on_event({"type": "sys", "msg": "disconnected"})

    # -- 发送 ------------------------------------------------------
    def call(self, svc, payload, timeout=15):
        """发 CALL，注册 pending（超时由 Timer 清理标记）。payload 可为 str/bytes。"""
        if not self.connected:
            self.on_event({"type": "err", "msg": "not connected (use: connect <host> <port>)"})
            return
        if isinstance(payload, str):
            payload = payload.encode("utf-8")
        with self.lock:
            cid = self.next_id
            self.next_id += 1
            self.pending[cid] = {"svc": svc, "t0": time.time()}
        frame = make_call_frame(cid, svc, payload)
        try:
            self.s.sendall(frame)
        except OSError as e:
            self.on_event({"type": "err", "msg": f"send failed: {e}"})
            return
        self.on_event({"type": "call", "cid": cid, "svc": svc, "payload": payload})
        t = threading.Timer(timeout, self._timeout_call, args=(cid,))
        t.daemon = True
        t.start()
        self._timers.append(t)

    def _timeout_call(self, cid):
        with self.lock:
            ent = self.pending.pop(cid, None)
        if ent is not None:
            self.on_event(
                {"type": "result", "cid": cid, "svc": ent["svc"],
                 "ok": False, "payload": b"<timeout>", "timeout": True}
            )

    def reply_req(self, rid, payload):
        """回复集群→外部的 REQ（默认 echo）。"""
        if not self.connected:
            return
        frame = make_result_frame(rid, True, payload)
        try:
            self.s.sendall(frame)
        except OSError:
            pass

    # -- 读线程 ----------------------------------------------------
    def _reader(self):
        try:
            while True:
                try:
                    fr = read_frame(self.reader)
                except BadFrame:
                    continue  # 坏帧跳过（协议异常）
                if fr is None:
                    self.on_event({"type": "sys", "msg": "connection closed by bridge"})
                    self.s = None
                    break
                kind, ident, third, payload = fr

                if kind == "REQ":
                    # bridge → 外部: REQ <rid> <len>\n<payload>\n
                    self.req_count += 1
                    self.on_event({"type": "req", "rid": ident, "payload": payload})
                    if self.auto_reply:
                        self.reply_req(ident, b"echo:" + payload)
                        self.on_event({"type": "req_replied", "rid": ident})
                elif kind == "RESULT":
                    ok = third == "OK"
                    cid = int(ident) if ident.isdigit() else None
                    with self.lock:
                        ent = self.pending.pop(cid, None) if cid is not None else None
                    if ent is not None:
                        self.resp_count += 1
                        if not ok:
                            self.err_count += 1
                        self.on_event(
                            {"type": "result", "cid": cid, "svc": ent["svc"],
                             "ok": ok, "payload": payload, "timeout": False}
                        )
        except OSError:
            # bridge 关闭/强杀 → recv 抛 OSError（RST）或本地 close 唤醒。
            # 必须通知 UI 断线（否则 TUI 一直显示 CONNECTED——实测）。
            # disconnect() 先置 self.s=None 再 close → 这里 s 已是 None
            # 跳过，避免与 "disconnected" 事件重复。
            if self.s is not None:
                self.s = None
                self.on_event({"type": "sys",
                               "msg": "connection lost (bridge closed)"})
        except Exception as e:  # noqa: BLE001 —— 读线程兜底，不静默死掉
            self.on_event({"type": "err", "msg": f"reader: {e}"})


# ---------------------------------------------------------------- 自测模式


def parse_payload(s: str) -> bytes:
    """命令栏 payload → 字节。单遍扫描转义：
    \\xHH → 十六进制字节；\\n \\r \\t \\\\ → 转义；其余字符 → utf-8。
    例: call svc \\x01\\xff\\x00 → b'\\x01\\xff\\x00'（cmd 窗口里输一个反斜杠）
    """
    out = bytearray()
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c == "\\" and i + 1 < n:
            nxt = s[i + 1]
            if nxt == "x" and i + 3 < n:
                h = s[i + 2 : i + 4]
                try:
                    out.append(int(h, 16))
                    i += 4
                    continue
                except ValueError:
                    pass  # 非法 hex → 当字面斜杠
            esc = {"n": "\n", "r": "\r", "t": "\t", "\\": "\\"}.get(nxt)
            if esc is not None:
                out.extend(esc.encode("utf-8"))
                i += 2
                continue
        out.extend(c.encode("utf-8"))
        i += 1
    return bytes(out)


def selftest(host, port):
    """无头验证协议链路：connect → CALL OK 路径 → CALL ERR 路径 → REQ echo。"""
    events = []
    link = BridgeLink(lambda ev: events.append(ev))
    results = {}

    def wait_until(pred, timeout=8):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if pred():
                return True
            time.sleep(0.05)
        return False

    ok = False
    for attempt in range(10):
        ok = link.connect(host, port)
        if ok:
            break
        time.sleep(1)
    print(f"[selftest] connect {host}:{port} -> {ok} (after retries)")
    if not ok:
        return 1

    # CALL external_echo（桥内 round-trip：CALL→REQ→本进程 echo→RESULT）
    link.call("external_echo", "tui-self-ping")
    got = wait_until(
        lambda: any(e["type"] == "result" and e["cid"] == 1 for e in events)
    )
    r1 = next((e for e in events if e["type"] == "result" and e["cid"] == 1), None)
    print(f"[selftest] call external_echo -> {r1 and (r1['ok'], repr(r1['payload']))}")
    assert got and r1 and r1["ok"] and r1["payload"] == b"echo:tui-self-ping", "CALL OK path FAILED"

    # CALL 不存在的服务 → 期望 ERR
    link.call("no_such_service_xyz", "boom")
    got2 = wait_until(
        lambda: any(e["type"] == "result" and e["cid"] == 2 for e in events)
    )
    r2 = next((e for e in events if e["type"] == "result" and e["cid"] == 2), None)
    print(f"[selftest] call no_such_service_xyz -> {r2 and (r2['ok'], repr(r2['payload']))}")
    assert got2 and r2 and not r2["ok"], "CALL ERR path FAILED"

    # CALL 二进制 payload → echo 原样返回（含 0x00/0xff 不可打印字节）
    link.call("external_echo", parse_payload(r"\x01\xff\x00"))
    got3 = wait_until(
        lambda: any(e["type"] == "result" and e["cid"] == 3 for e in events)
    )
    r3 = next((e for e in events if e["type"] == "result" and e["cid"] == 3), None)
    print(f"[selftest] call external_echo binary -> {r3 and (r3['ok'], repr(r3['payload']))}")
    assert got3 and r3 and r3["ok"] and r3["payload"] == b"echo:\x01\xff\x00", "binary CALL FAILED"

    # REQ 自动回复验证：CALL external_echo 的 REQ 已被 echo（计数 ≥2）
    print(f"[selftest] REQ handled: {link.req_count}, RESULT: {link.resp_count}")
    assert link.req_count >= 2, "REQ auto-reply FAILED"

    # 清理
    link.disconnect()
    print("[selftest] ALL PASS")
    return 0


# ---------------------------------------------------------------- TUI 界面

from textual.app import App, ComposeResult  # noqa: E402
from textual.containers import Horizontal, Vertical  # noqa: E402
from textual.widgets import Footer, Header, Input, RichLog, Static  # noqa: E402

CSS = """
Screen { background: #070b16; }
#left { width: 44; padding: 0 1; }
.panel {
    border: round #5b4b8a;
    background: #0c1120;
    color: #c9d4f0;
    padding: 0 1;
    margin: 1 0;
    height: auto;
}
.panel > .panel-title {
    color: #b48cff;
    text-style: bold underline;
}
#conn_panel .status-ok { color: #4ade80; text-style: bold; }
#conn_panel .status-err { color: #f87171; text-style: bold; }
#right { width: 1fr; padding: 1 1 0 0; }
#log {
    background: #0a0f1e;
    border: round #5b4b8a;
    height: 1fr;
    padding: 0 1;
}
#cmd {
    margin: 0 1 1 1;
    border: tall #6a5acd;
    background: #0c1120;
    color: #e0e8ff;
}
#cmd:focus { border: tall #9d7bff; }
.log-call { color: #67e8f9; }
.log-ok { color: #4ade80; }
.log-err { color: #f87171; }
.log-req { color: #fbbf24; }
.log-reqrep { color: #a3e635; }
.log-sys { color: #8892b0; }
.log-warn { color: #fca5a5; }
"""


class ClusterTUI(App):
    TITLE = "CAF Cluster Console"
    SUB_TITLE = "bridge sidecar · 行协议控制台"
    CSS = CSS
    BINDINGS = [("ctrl+c", "quit", "退出")]

    def __init__(self, host="127.0.0.1", port=48000):
        super().__init__()
        self.auto_connect = (host, port)
        self.events = queue.Queue()
        self.link = BridgeLink(self.events.put)
        self.services = {}  # svc -> count
        self.rlog = None
        self.conn_panel = None
        self.svc_panel = None

    # -- 界面 ------------------------------------------------------
    def compose(self) -> ComposeResult:
        yield Header(show_clock=True)
        with Horizontal():
            with Vertical(id="left"):
                yield Static("", id="conn_panel", classes="panel")
                yield Static("", id="svc_panel", classes="panel")
            with Vertical(id="right"):
                yield RichLog(id="log", highlight=True, wrap=True, markup=True)
        yield Input(
            placeholder=("call <svc> <payload> · callhex <svc> <hex> · "
                         "connect <host> <port> · auto-reply on|off · help · quit"),
            id="cmd",
        )
        yield Footer()

    def on_mount(self) -> None:
        self.rlog = self.query_one("#log", RichLog)
        self.conn_panel = self.query_one("#conn_panel", Static)
        self.svc_panel = self.query_one("#svc_panel", Static)
        self.set_interval(0.08, self._drain_events)
        if self.auto_connect:
            h, p = self.auto_connect
            self.link.connect(h, p)
        self.query_one("#cmd", Input).focus()

    # -- 事件泵 ----------------------------------------------------
    def _drain_events(self) -> None:
        while True:
            try:
                ev = self.events.get_nowait()
            except queue.Empty:
                break
            self._handle(ev)
        self._refresh_panels()

    def _handle(self, ev: dict) -> None:
        t = ev["type"]
        if t == "sys":
            self.rlog.write(f"[log-sys]● {ev['msg']}")
        elif t == "err":
            self.rlog.write(f"[log-warn]✖ {ev['msg']}")
        elif t == "call":
            self.services[ev["svc"]] = self.services.get(ev["svc"], 0) + 1
            shown = ev["payload"][:80]
            self.rlog.write(f"[log-call]➜ CALL {ev['cid']} {ev['svc']} :: {shown!r}")
        elif t == "result":
            rid = ev["cid"]
            shown = ev["payload"][:80]
            if ev.get("timeout"):
                self.rlog.write(f"[log-err]◉ CALL {rid} {ev['svc']} → TIMEOUT")
            elif ev["ok"]:
                self.rlog.write(f"[log-ok]✔ CALL {rid} {ev['svc']} → {shown!r}")
            else:
                self.rlog.write(f"[log-err]✘ CALL {rid} {ev['svc']} → {shown!r}")
        elif t == "req":
            shown = ev["payload"][:80]
            self.rlog.write(f"[log-req]⇐ REQ {ev['rid']} :: {shown!r}")
        elif t == "req_replied":
            self.rlog.write(f"[log-reqrep]⇒ REQ {ev['rid']} auto-replied (echo)")

    def _refresh_panels(self) -> None:
        lk = self.link
        st = "● CONNECTED" if lk.connected else "○ DISCONNECTED"
        cls = "status-ok" if lk.connected else "status-err"
        self.conn_panel.update(
            "[panel-title]CONNECTION[/]\n"
            f"[{cls}]{st}[/]  " + ("" if not lk.connected else
                                   f"({self.auto_connect[0]}:{self.auto_connect[1]})") +
            f"\nREQ 收 {lk.req_count} · RESULT 收 {lk.resp_count}"
            f" · ERR {lk.err_count}"
        )
        if self.services:
            lines = "".join(
                f"\n  {svc} ×{n}" for svc, n in sorted(self.services.items())
            )
        else:
            lines = "\n  (暂无，用 call <svc> <payload> 发第一个)"
        self.svc_panel.update(
            "[panel-title]SERVICES CALLED[/]" + lines +
            f"\n\nauto-reply: {'ON' if lk.auto_reply else 'OFF'}"
        )

    # -- 命令 ------------------------------------------------------
    def on_input_submitted(self, event: Input.Submitted) -> None:
        raw = event.value.strip()
        self.query_one("#cmd", Input).value = ""
        if not raw:
            return
        parts = raw.split(None, 2) if raw.startswith("call") else raw.split()
        cmd = parts[0].lower()
        if cmd == "connect" and len(parts) == 3:
            ok = self.link.connect(parts[1], int(parts[2]))
            if ok:
                self.auto_connect = (parts[1], int(parts[2]))
        elif cmd == "disconnect":
            self.link.disconnect()
        elif cmd == "call" and len(parts) >= 3:
            svc, payload = parts[1], parts[2]
            self.link.call(svc, parse_payload(payload))
        elif cmd == "call" and len(parts) == 2:
            self.link.call(parts[1], b"")
        elif cmd == "callhex" and len(parts) >= 3:
            try:
                payload = bytes.fromhex(parts[2].replace(" ", ""))
            except ValueError:
                self.rlog.write("[log-warn]✖ callhex: 非法 hex（示例: callhex svc 01ff00）")
                return
            self.link.call(parts[1], payload)
        elif cmd == "auto-reply" and len(parts) == 2:
            self.link.auto_reply = parts[1] == "on"
            self.rlog.write(f"[log-sys]● auto-reply {parts[1]}")
        elif cmd == "services":
            self._refresh_panels()
        elif cmd == "clear":
            self.rlog.clear()
        elif cmd in ("help", "?"):
            self._show_help()
        elif cmd == "quit":
            self.exit()
        else:
            self.rlog.write("[log-warn]✖ 用法: connect <host> <port> | "
                           "call <svc> <payload> | auto-reply on|off | help | quit")

    def _show_help(self) -> None:
        self.rlog.write(
            "[log-sys]● 命令:\n"
            "    connect <host> <port>     连接 bridge\n"
            "    disconnect                断开\n"
            "    call <svc> <payload>      调集群服务；\x5cxHH 输二进制（\x5cx01\x5cxff）\n"
            "    callhex <svc> <hex>       十六进制直发（01ff00，可带空格）\n"
            "    call system.nodes       集群拓扑（worker/master 列表）\n"
            "    call system.services    本节点服务清单\n"
            "    call system.ping        连通性自检（pong）\n"
            "    auto-reply on|off         REQ 自动回复（默认 on）\n"
            "    services                  已调用服务统计\n"
            "    clear                     清日志\n"
            "    quit                      退出"
        )

    def action_quit(self) -> None:
        self.link.disconnect()
        self.exit()


def main():
    args = sys.argv[1:]
    if args and args[0] == "--selftest":
        host = args[1] if len(args) > 1 else "127.0.0.1"
        port = int(args[2]) if len(args) > 2 else 48000
        sys.exit(selftest(host, port))
    host = args[0] if len(args) > 0 else "127.0.0.1"
    port = int(args[1]) if len(args) > 1 else 48000
    ClusterTUI(host, port).run()


if __name__ == "__main__":
    main()
