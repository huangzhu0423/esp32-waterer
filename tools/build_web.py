#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
把 web/index.html 压成 gzip 字节数组写进 esp32_waterer/web_ui.h。

为什么必须用字节数组：gzip 流里一定有 0x00，
任何 const char* 路径都会在第一个 0 处被截断。

用法：
    python tools/build_web.py            # 生成
    python tools/build_web.py --check    # 只校验现有 web_ui.h 是否与源文件一致
"""
import gzip
import hashlib
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "web" / "index.html"
OUT = ROOT / "esp32_waterer" / "web_ui.h"

BUDGET_BYTES = 12 * 1024      # gzip 后的软预算，超了给提示但不失败
HARD_LIMIT = 16 * 1024        # 超过这个值直接失败


def compress(src_bytes: bytes) -> bytes:
    return gzip.compress(src_bytes, 9, mtime=0)


def prepare(html: bytes) -> bytes:
    """只做 HTML 层面上绝对安全的压缩：标签之间的纯空白。
    不动 CSS / JS 内部，避免踩到语义。"""
    text = html.decode("utf-8")
    text = re.sub(r">\s+<", "><", text)
    return text.encode("utf-8")


def render_header(payload: bytes, raw_len: int, digest: str) -> str:
    lines = []
    lines.append("#pragma once")
    lines.append("/* 本文件由 tools/build_web.py 自动生成，不要手工修改。")
    lines.append("   源文件: web/index.html")
    lines.append("   原始长度: %d 字节" % raw_len)
    lines.append("   gzip 长度: %d 字节" % len(payload))
    lines.append("   源文件 sha256: %s" % digest)
    lines.append("*/")
    lines.append("#include <stdint.h>")
    lines.append("#include <pgmspace.h>")
    lines.append("")
    lines.append("#define WEB_UI_GZ_LEN %d" % len(payload))
    lines.append("")
    lines.append("const uint8_t WEB_UI_GZ[WEB_UI_GZ_LEN] PROGMEM = {")
    for i in range(0, len(payload), 16):
        chunk = payload[i:i + 16]
        lines.append("  " + ",".join("0x%02x" % b for b in chunk) + ",")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    if not SRC.exists():
        print("[ERR] 找不到 %s" % SRC)
        return 2

    raw = SRC.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    prepared = prepare(raw)
    payload = compress(prepared)

    header = render_header(payload, len(raw), digest)

    if "--check" in sys.argv:
        if not OUT.exists():
            print("[ERR] web_ui.h 不存在，请先运行 build_web.py")
            return 1
        cur = OUT.read_text(encoding="utf-8")
        if ("源文件 sha256: %s" % digest) in cur:
            print("[OK] web_ui.h 与 web/index.html 一致 (%d 字节 gzip)" % len(payload))
            return 0
        print("[ERR] web_ui.h 已过期，请重新运行 build_web.py")
        return 1

    OUT.write_text(header, encoding="utf-8")

    ratio = len(raw) / max(1, len(payload))
    print("源文件      : %6d B (%.1f KB)" % (len(raw), len(raw) / 1024))
    print("压缩后      : %6d B (%.1f KB)  压缩比 %.2f:1" % (len(payload), len(payload) / 1024, ratio))
    print("输出去向    : %s" % OUT.relative_to(ROOT))

    if len(payload) > HARD_LIMIT:
        print("[ERR] gzip 后超过硬上限 %d B，请拆分 CSS/JS 为独立端点" % HARD_LIMIT)
        return 1
    if len(payload) > BUDGET_BYTES:
        print("[WARN] 超过软预算 %d B，flash 占用略大但不影响运行" % BUDGET_BYTES)
    return 0


if __name__ == "__main__":
    sys.exit(main())
