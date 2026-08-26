#!/usr/bin/env python3
"""Render PlantUML .puml via plantuml.com public server (no Java needed).

Usage:
    python3 render_puml.py <file.puml> [outdir]

Each @startuml block in the file renders to <outdir>/<name>.png.
outdir defaults to the source file's directory.
Uses only the Python standard library. Network required (plantuml.com).
"""
import os
import re
import sys
import urllib.request
import zlib


def puml_encode(text: str) -> str:
    """PlantUML URL 编码：raw deflate + 标准 6-bit 编码。

    坑（2026-08-25 实测三连）：
    1. 必须是 3 字节 → 4 字符（每 24 位切 4×6 位）；误用 2 字节 → 2
       字符的实现会丢位，plantuml.com 报 "does not look like DEFLATE"。
    2. 尾部残缺组【必须补零、仍然发满 4 字符】——只发 2 字符会截断
       deflate 流（inflate 报 incomplete/truncated stream -5）。
       deflate 自终止，解码端多余字节被忽略，补零无害。
    3. ~1 前缀是 Huffman 编码才需要的，DEFLATE 用无前缀/~0。
    """
    comp = zlib.compress(text.encode("utf-8"), 9)[2:-4]  # 去 zlib 头/校验
    abc = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz-_"
    out = []
    for i in range(0, len(comp), 3):
        b1 = comp[i]
        b2 = comp[i + 1] if i + 1 < len(comp) else 0
        b3 = comp[i + 2] if i + 2 < len(comp) else 0
        out.append(abc[b1 >> 2])
        out.append(abc[((b1 & 0x03) << 4) | (b2 >> 4)])
        out.append(abc[((b2 & 0x0F) << 2) | (b3 >> 6)])
        out.append(abc[b3 & 0x3F])
    return "".join(out)


def render(puml_path: str, outdir: str) -> int:
    src = open(puml_path, encoding="utf-8").read()
    blocks = re.findall(r"@startuml (\S+)\n(.*?)@enduml", src, re.S)
    if not blocks:
        print(f"ERR: 没找到 @startuml 块: {puml_path}")
        return 1
    os.makedirs(outdir, exist_ok=True)
    ok = 0
    for name, body in blocks:
        url = ("https://www.plantuml.com/plantuml/png/"
               + puml_encode("@startuml\n" + body + "@enduml"))
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=30) as r:
                data = r.read()
        except Exception as e:
            print(f"FAIL {name}: {e}")
            continue
        if data[:8] == b"\x89PNG\r\n\x1a\n":
            out = os.path.join(outdir, name + ".png")
            with open(out, "wb") as f:
                f.write(data)
            print(f"OK  {out}  ({len(data)} bytes)")
            ok += 1
        else:
            print(f"ERR {name}: 非 PNG 响应: {data[:80]!r}")
    print(f"{ok}/{len(blocks)} rendered")
    return 0 if ok == len(blocks) else 1


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    src = sys.argv[1]
    outdir = sys.argv[2] if len(sys.argv) > 2 else (os.path.dirname(src) or ".")
    sys.exit(render(src, outdir))
