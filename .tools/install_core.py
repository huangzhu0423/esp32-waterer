#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把手动下载的 esp32 包解压进指定数据目录（默认 D:/esp32toolchain/data）。"""
import json, os, sys, zipfile, tarfile, shutil

ROOT = os.path.dirname(os.path.abspath(__file__))
ARCH = os.environ.get("ESP32_ARCH", r"D:\esp32toolchain\archives")
DATA = os.environ.get("ARDUINO_DIRECTORIES_DATA", r"D:\esp32toolchain\data")
PKG = os.path.join(DATA, "packages", "esp32")


def strip_extract(path, dest):
    os.makedirs(dest, exist_ok=True)
    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            names = z.namelist()
            roots = {n.split("/")[0] for n in names}
            files = [n for n in names if not n.endswith("/")]
            single = len(roots) == 1 and all("/" in n for n in files)
            if single:
                root = roots.pop()
                for n in names:
                    if not n.startswith(root + "/"):
                        continue
                    rel = n[len(root) + 1:]
                    if not rel:
                        continue
                    target = os.path.join(dest, rel.replace("/", os.sep))
                    if n.endswith("/"):
                        os.makedirs(target, exist_ok=True)
                    else:
                        os.makedirs(os.path.dirname(target), exist_ok=True)
                        with z.open(n) as src, open(target, "wb") as dst:
                            shutil.copyfileobj(src, dst)
            else:
                z.extractall(dest)
    else:
        with tarfile.open(path) as t:
            t.extractall(dest)


def main():
    items = json.load(open(os.path.join(ROOT, "need.json"), encoding="utf-8"))
    for it in items:
        src = os.path.join(ARCH, it["file"])
        if not os.path.exists(src):
            print("!! 缺文件", it["file"])
            return 1
        if it["name"] == "platform":
            dest = os.path.join(PKG, "hardware", "esp32", it["ver"])
        else:
            dest = os.path.join(PKG, "tools", it["name"], it["ver"])
        print("[解压] %-12s -> %s" % (it["name"], dest), flush=True)
        strip_extract(src, dest)
    print("完成")
    return 0


if __name__ == "__main__":
    sys.exit(main())
