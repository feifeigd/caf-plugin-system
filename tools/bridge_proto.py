#!/usr/bin/env python3
"""bridge 行协议公共层 —— bridge_client.py / cluster_tui.py 共用。

行协议（\\n 分隔；payload 长度前缀，可含任意字节）：
  外部 → bridge:  CALL <id> <svc> <len>\\n<payload>\\n
  bridge → 外部:  RESULT <id> OK|ERR <len>\\n<payload>\\n
  bridge → 外部:  REQ <rid> <len>\\n<payload>\\n
  外部 → bridge:  RESULT <rid> OK|ERR <len>\\n<payload>\\n

用法：
  from bridge_proto import LineReader, read_frame, BadFrame,
                           make_call_frame, make_result_frame
"""
import socket


class LineReader:
    """带缓冲的行读取器：read_line 读 \\n 结尾行，read_exact 读 N 字节。"""

    def __init__(self, s):
        self.s = s
        self.buf = b""

    def read_line(self):
        while True:
            i = self.buf.find(b"\n")
            if i >= 0:
                line = self.buf[: i + 1]
                self.buf = self.buf[i + 1 :]
                return line
            d = self.s.recv(4096)
            if not d:
                return None
            self.buf += d

    def read_exact(self, n):
        while len(self.buf) < n:
            d = self.s.recv(4096)
            if not d:
                return None
            self.buf += d
        out = self.buf[:n]
        self.buf = self.buf[n:]
        return out


class BadFrame(Exception):
    """头行格式非法（字段不足/长度非数字）——调用方选择跳过继续读。"""


def read_frame(reader):
    """读一帧。

    Returns:
        (kind, ident, third, payload) —— kind/ident/third 为 str，
        payload 为 bytes（已消费行终止符 \\n）。
        None —— EOF（对端关闭/半包）。
    Raises:
        BadFrame —— 头行非法（协议异常，通常直接退出或 continue）。
    """
    head = reader.read_line()
    if head is None:
        return None
    head = head.decode("utf-8", "replace").rstrip("\r\n")
    parts = head.split(" ")
    if len(parts) < 3:
        raise BadFrame(repr(head))
    try:
        ln = int(parts[-1])
    except ValueError:
        raise BadFrame(repr(head))
    payload = reader.read_exact(ln)
    if payload is None:
        return None
    reader.read_exact(1)  # 行终止符 \\n
    return parts[0], parts[1], parts[2], payload


def make_call_frame(cid, svc, payload):
    """外部 → bridge: CALL <id> <svc> <len>\\n<payload>\\n。payload 可为 str/bytes。"""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return f"CALL {cid} {svc} {len(payload)}\n".encode("utf-8") + payload + b"\n"


def make_result_frame(rid, ok, payload):
    """bridge → 外部 或 外部 → bridge: RESULT <id> OK|ERR <len>\\n<payload>\\n。"""
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    return f"RESULT {rid} {'OK' if ok else 'ERR'} {len(payload)}\n".encode("utf-8") + payload + b"\n"
