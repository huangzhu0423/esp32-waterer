#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""多线程分段下载（单连接被限速到 36KB/s，分段并发能到 700KB/s 以上）。"""
import json, os, subprocess, sys, threading, hashlib

ROOT = os.path.dirname(os.path.abspath(__file__))
ARCH = os.environ.get("ESP32_ARCH", os.path.join(ROOT, "archives"))
PROXY = "https://gh-proxy.com/"
PARTS = 8


def worker(url, start, end, path, slot):
    expect = end - start + 1
    for _ in range(6):
        try:
            if os.path.exists(path):
                os.remove(path)
            with open(path, "wb") as f:
                subprocess.call(["curl", "-sL", "--max-time", "900",
                                 "-r", "%d-%d" % (start, end), "-o", "-", url],
                                stdout=f)
            if os.path.getsize(path) == expect:
                return True
        except Exception:
            pass
    return False


def download(url, dst, size):
    if os.path.exists(dst) and os.path.getsize(dst) == size:
        return True
    chunk = size // PARTS + 1
    threads, parts = [], []
    for i in range(PARTS):
        s = i * chunk
        e = min(s + chunk - 1, size - 1)
        if s > size - 1:
            break
        p = dst + ".p%d" % i
        parts.append((p, e - s + 1))
        t = threading.Thread(target=worker, args=(url, s, e, p, i))
        t.start()
        threads.append(t)
    for t in threads:
        t.join()
    for p, exp in parts:
        if not os.path.exists(p) or os.path.getsize(p) != exp:
            print("   !! 分段失败", p)
            return False
    with open(dst, "wb") as out:
        for p, _ in parts:
            with open(p, "rb") as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    out.write(b)
            os.remove(p)
    return os.path.getsize(dst) == size


def main():
    items = json.load(open(os.path.join(ROOT, "need.json"), encoding="utf-8"))
    os.makedirs(ARCH, exist_ok=True)
    ok = True
    for it in items:
        dst = os.path.join(ARCH, it["file"])
        url = it["url"]
        if url.startswith("https://github.com/"):
            url = PROXY + url
        print("[开始] %-12s %6.1f MB" % (it["name"], it["size"] / 1048576), flush=True)
        if not download(url, dst, it["size"]):
            print("  !! 失败", it["file"], flush=True)
            ok = False
            continue
        if it["sha"].startswith("sha256:"):
            h = hashlib.sha256(open(dst, "rb").read()).hexdigest()
            if h != it["sha"][7:]:
                print("  !! 校验和不符", flush=True)
                ok = False
                continue
        print("  OK", it["name"], flush=True)
    print("__DONE__" if ok else "__FAILED__", flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
