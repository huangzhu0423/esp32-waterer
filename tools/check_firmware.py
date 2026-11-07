#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
固件静态检查门禁。

本机没有 C++ 编译器（没有 arduino-cli / PlatformIO / g++），
所以用这个脚本挡住"不需要编译器也能发现"的错误。

能查出来的：
  - 括号 / 引号配平错误（先剥离注释与字符串，避免误报）
  - #include "x.h" 指向的文件不存在
  - 头文件缺 #pragma once
  - 头文件里声明了但从来没实现的函数
  - 引脚表：ADC 不在 ADC1、泵脚在禁用引脚、两套引脚冲突、数量对不上
  - input-only 引脚被当输出用
  - web_ui.h 与 web/index.html 不同步（忘了重新生成）
  - web_ui.h 里数组长度与 WEB_UI_GZ_LEN 不一致
  - 配置默认值越界

查不出来的（必须靠真编译）：
  类型错误、链接错误、Arduino core 2.x 与 3.x 的 API 差异、内存与栈溢出。

用法： python tools/check_firmware.py
"""
import re
import sys
import gzip
import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FW = ROOT / "esp32_waterer"
WEB = ROOT / "web" / "index.html"

errors = []
warns = []


def err(msg):
    errors.append(msg)


def warn(msg):
    warns.append(msg)


# ---------------------------------------------------------------- 词法剥离
def strip_code(text: str) -> str:
    """去掉注释、字符串、字符字面量，保留换行以免行号错乱。"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out.append(" ")
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out.append("  ")
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
        elif c == '"' or c == "'":
            quote = c
            out.append(" ")
            i += 1
            while i < n:
                if text[i] == "\\":
                    out.append(" ")
                    i += 2
                    continue
                if text[i] == quote:
                    out.append(" ")
                    i += 1
                    break
                out.append("\n" if text[i] == "\n" else " ")
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def check_balance(path: Path):
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = strip_code(raw)
    pairs = {"(": ")", "{": "}", "[": "]"}
    closing = {v: k for k, v in pairs.items()}
    stack = []
    line = 1
    for ch in code:
        if ch == "\n":
            line += 1
        elif ch in pairs:
            stack.append((ch, line))
        elif ch in closing:
            if not stack:
                err("%s:%d 多出一个 '%s'" % (path.name, line, ch))
                return
            top, tl = stack.pop()
            if top != closing[ch]:
                err("%s:%d '%s'(%d 行打开) 被 '%s' 关闭" % (path.name, line, top, tl, ch))
                return
    if stack:
        top, tl = stack[-1]
        err("%s:%d 的 '%s' 没有关闭" % (path.name, tl, top))


# ---------------------------------------------------------------- 文件集合
def fw_files():
    return sorted([p for p in FW.iterdir() if p.suffix in (".h", ".cpp", ".ino")])


def main() -> int:
    if not FW.exists():
        print("[ERR] 找不到目录 %s" % FW)
        return 2

    files = fw_files()
    if not files:
        print("[ERR] 固件目录是空的")
        return 2

    texts = {}
    codes = {}
    for p in files:
        raw = p.read_text(encoding="utf-8", errors="replace")
        texts[p.name] = raw
        codes[p.name] = strip_code(raw)

    # ---- 1. 括号配平 ----
    for p in files:
        check_balance(p)

    # ---- 2. include 存在性 ----
    for p in files:
        for m in re.finditer(r'#\s*include\s+"([^"]+)"', texts[p.name]):
            target = m.group(1)
            if not (FW / target).exists():
                err("%s 包含的 \"%s\" 不存在" % (p.name, target))

    # ---- 3. 头文件保护 ----
    for p in files:
        if p.suffix == ".h" and "#pragma once" not in texts[p.name]:
            err("%s 缺少 #pragma once" % p.name)

    # ---- 4. 声明了但没实现 ----
    proto_re = re.compile(
        r"^[ \t]*(?:[A-Za-z_][\w \t\*&:<>]*?)[ \t\*&]+([A-Za-z_]\w*)[ \t]*\([^;{}]*\)[ \t]*;[ \t]*$",
        re.M,
    )
    skip = {"if", "for", "while", "switch", "return", "sizeof", "static_assert"}
    body_text = "\n".join(codes[n] for n in codes if not n.endswith(".h"))
    for p in files:
        if p.suffix != ".h":
            continue
        for m in proto_re.finditer(codes[p.name]):
            fn = m.group(1)
            if fn in skip:
                continue
            if not re.search(r"\b" + re.escape(fn) + r"\s*\(", body_text):
                err("%s 声明了 %s() 但没有任何 .cpp/.ino 实现它" % (p.name, fn))

    # ---- 5. 引脚表 ----
    cfg = texts.get("config.h", "")
    ch_count = None
    m = re.search(r"#\s*define\s+CHANNEL_COUNT\s+(\d+)", cfg)
    if m:
        ch_count = int(m.group(1))
    else:
        err("config.h 里找不到 CHANNEL_COUNT")

    def pin_array(name):
        mm = re.search(r"PIN_%s\[CHANNEL_COUNT\]\s*=\s*\{([^}]*)\}" % name, cfg)
        if not mm:
            return None
        items = [x.strip() for x in mm.group(1).split(",") if x.strip()]
        return items

    adc_items = pin_array("ADC")
    pump_items = pin_array("PUMP")

    if adc_items is None or pump_items is None:
        err("config.h 里 PIN_ADC / PIN_PUMP 数组解析失败")
    else:
        if ch_count is not None:
            if len(adc_items) != ch_count:
                err("PIN_ADC 有 %d 个元素，CHANNEL_COUNT 是 %d" % (len(adc_items), ch_count))
            if len(pump_items) != ch_count:
                err("PIN_PUMP 有 %d 个元素，CHANNEL_COUNT 是 %d" % (len(pump_items), ch_count))

        def resolve(name):
            mm = re.search(r"#\s*define\s+%s\s+(\d+)" % re.escape(name), cfg)
            return int(mm.group(1)) if mm else None

        adc_pins, pump_pins = [], []
        for it in adc_items:
            v = resolve(it) if not it.isdigit() else int(it)
            if v is None:
                err("无法解析传感器引脚宏 %s" % it)
            else:
                adc_pins.append(v)
        for it in pump_items:
            v = resolve(it) if not it.isdigit() else int(it)
            if v is None:
                err("无法解析水泵引脚宏 %s" % it)
            else:
                pump_pins.append(v)

        adc2 = {0, 2, 4, 12, 13, 14, 15, 25, 26, 27}
        input_only = {34, 35, 36, 37, 38, 39}
        forbidden_out = set(range(6, 12)) | {0, 2, 12, 15, 1, 3, 4, 5, 14, 16, 17, 20, 24} | input_only

        for i, p in enumerate(adc_pins):
            if p in adc2:
                err("传感器 CH%d 用了 GPIO%d，它属于 ADC2，WiFi 开启时读不到值" % (i, p))
            if not (32 <= p <= 39):
                err("传感器 CH%d 的 GPIO%d 不在 ADC1 范围（32~39）" % (i, p))
        for i, p in enumerate(pump_pins):
            if p in forbidden_out:
                err("水泵 CH%d 用了 GPIO%d，这是禁用输出引脚" % (i, p))
        if len(set(adc_pins)) != len(adc_pins):
            err("传感器引脚有重复：%s" % adc_pins)
        if len(set(pump_pins)) != len(pump_pins):
            err("水泵引脚有重复：%s" % pump_pins)
        if set(adc_pins) & set(pump_pins):
            err("传感器与水泵引脚冲突：%s" % sorted(set(adc_pins) & set(pump_pins)))

        # input-only 被当输出
        for fname, code in codes.items():
            for m2 in re.finditer(r"pinMode\s*\(\s*(\d+)\s*,\s*OUTPUT\s*\)", code):
                if int(m2.group(1)) in input_only:
                    err("%s 对 input-only 引脚 GPIO%s 设置了 OUTPUT（无效且危险）" % (fname, m2.group(1)))

        # 只有 pump.cpp 能写水泵引脚的电平
        for fname, code in codes.items():
            if fname == "pump.cpp" or fname.endswith(".h"):
                continue
            if re.search(r"digitalWrite\s*\(\s*PIN_PUMP", code):
                err("%s 直接操作了 PIN_PUMP，水泵电平只允许 pump.cpp 修改" % fname)

    # ---- 6. 默认值范围 ----
    def cfg_int(name, default=None):
        mm = re.search(r"#\s*define\s+%s\s+(-?\d+)" % name, cfg)
        return int(mm.group(1)) if mm else default

    hard_max = cfg_int("HARD_MAX_SEC")
    def_max = cfg_int("DEF_MAX_SEC")
    def_thr = cfg_int("DEF_THR_PCT")
    if hard_max is not None and def_max is not None and def_max > hard_max:
        err("DEF_MAX_SEC(%d) 超过 HARD_MAX_SEC(%d)" % (def_max, hard_max))
    if def_thr is not None and not (5 <= def_thr <= 90):
        err("DEF_THR_PCT(%d) 不在 5~90 之间" % def_thr)

    # ---- 7. web_ui.h 同步性 ----
    ui = FW / "web_ui.h"
    if not ui.exists():
        err("web_ui.h 不存在，先运行 python tools/build_web.py")
    elif WEB.exists():
        digest = hashlib.sha256(WEB.read_bytes()).hexdigest()
        ui_text = ui.read_text(encoding="utf-8", errors="replace")
        if digest not in ui_text:
            err("web_ui.h 与 web/index.html 不同步（忘了重新生成？）")
        mm = re.search(r"#\s*define\s+WEB_UI_GZ_LEN\s+(\d+)", ui_text)
        if mm:
            declared = int(mm.group(1))
            body = ui_text.split("WEB_UI_GZ_LEN] PROGMEM = {", 1)
            if len(body) == 2:
                arr = body[1].split("};", 1)[0]
                actual = len(re.findall(r"0x[0-9a-fA-F]{2}", arr))
                if actual != declared:
                    err("web_ui.h 声明 %d 字节，实际数组 %d 字节" % (declared, actual))
                else:
                    # 顺带确认 gzip 头正确
                    data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", arr))
                    if data[:2] != b"\x1f\x8b":
                        err("web_ui.h 的数据不是 gzip 流（魔数不对）")
                    else:
                        try:
                            gzip.decompress(data)
                        except Exception as e:
                            err("web_ui.h 的 gzip 解压失败：%s" % e)

    # ---------------------------------------------------------------- 输出
    print("检查文件：%d 个" % len(files))
    for w in warns:
        print("  [WARN] %s" % w)
    if errors:
        print("")
        for e in errors:
            print("  [ERR ] %s" % e)
        print("\n共 %d 个错误。" % len(errors))
        return 1

    print("全部通过。注意：静态检查通过不等于能编译，最终仍要跑一次 arduino-cli compile。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
