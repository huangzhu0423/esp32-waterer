#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""按 Windows 平台挑出 esp32:esp32 真正需要的包，经 gh-proxy 下载到本地。"""
import json, os, subprocess, sys, hashlib

ROOT = os.path.dirname(os.path.abspath(__file__))
IDX = os.path.join(ROOT, "esp32idx.json")
OUT = os.path.join(ROOT, "archives")
VER = "3.3.12"
PROXY = "https://gh-proxy.com/"

# 只装经典 ESP32 编译和上传真正用得到的，跳过 riscv / 其他芯片预编译库 / openocd
WANT = {"esp-x32", "esp32-libs", "mklittlefs", "xtensa-esp-elf-gdb", "esptool_py"}

os.makedirs(OUT, exist_ok=True)
d = json.load(open(IDX, encoding="utf-8"))
pkg = [x for x in d["packages"] if x["name"] == "esp32"][0]
tools = {t["name"]: t for t in pkg["tools"]}
plat = [x for x in pkg["platforms"] if x["version"] == VER][0]

items = [{"name": "platform", "file": plat["archiveFileName"],
          "url": plat["url"], "sha": plat["checksum"], "ver": VER,
          "size": int(plat["size"])}]

for dep in plat["toolsDependencies"]:
    if dep["name"] not in WANT:
        continue
    t = tools.get(dep["name"])
    if not t:
        print("!! 索引里没有", dep["name"])
        continue
    cand = None
    for s in t["systems"]:
        if s["host"] in ("x86_64-mingw32", "i686-mingw32", "any"):
            cand = s
            break
    if cand is None:
        print("!! 无 Windows 版本", dep["name"])
        continue
    items.append({"name": dep["name"], "file": cand["archiveFileName"],
                  "url": cand["url"], "sha": cand["checksum"],
                  "ver": dep["version"], "size": int(cand["size"])})

json.dump(items, open(os.path.join(ROOT, "need.json"), "w"), indent=1)

ok = True
for it in items:
    dst = os.path.join(OUT, it["file"])
    if os.path.exists(dst) and os.path.getsize(dst) == it["size"]:
        print("[跳过] %s 已完整" % it["file"])
        continue
    print("[下载] %-12s %6.1f MB  %s" % (it["name"], it["size"] / 1048576, it["file"]), flush=True)
    url = it["url"]
    if url.startswith("https://github.com/"):
        url = PROXY + url
    r = subprocess.call(["curl", "-sL", "-C", "-", "--max-time", "900",
                         "--retry", "3", "-o", dst, url])
    if r != 0 or not os.path.exists(dst):
        print("  !! 下载失败 curl=%d" % r)
        ok = False
        continue
    got = os.path.getsize(dst)
    if got != it["size"]:
        print("  !! 大小不符 %d / %d" % (got, it["size"]))
        ok = False
        continue
    if it["sha"].startswith("sha256:"):
        h = hashlib.sha256(open(dst, "rb").read()).hexdigest()
        if h != it["sha"][7:]:
            print("  !! 校验和不符")
            ok = False
            continue
    print("  OK", flush=True)

print("__DONE__" if ok else "__FAILED__")
