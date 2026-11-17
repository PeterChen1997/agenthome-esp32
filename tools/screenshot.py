#!/usr/bin/env python3
"""从 Agent Home 屏上抓当前画面存成 PNG(固件 AHS 命令)。

用法:
  python3 screenshot.py out.png           # 抓当前页
  python3 screenshot.py out.png --page    # 先翻一页再抓
"""
import argparse
import os
import sys
import time

from PIL import Image

from push_usage import find_port, open_port, read_until

W, H = 320, 240


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port")
    ap.add_argument("--page", action="store_true", help="先发 AHP 翻一页")
    args = ap.parse_args()
    port = args.port or find_port()
    if port is None:
        print("没找到 /dev/cu.usbmodem*", file=sys.stderr)
        return 1
    fd = open_port(port)
    try:
        for _ in range(10):  # 同 push_lines:先握手,板子就绪后再发命令
            os.write(fd, b"AH?\n")
            if b"diag" in read_until(fd, b"diag", 1, 0.3):
                break
        else:
            print("板子没应答", file=sys.stderr)
            return 1
        if args.page:
            os.write(fd, b"AHP\n")
            read_until(fd, b"rendered", 1, 2)
        read_until(fd, b"\n", 1000, 0.3)  # 清掉残余回显
        os.write(fd, b"AHS\n")
        buf = read_until(fd, b"\nend", 1, 10)
    finally:
        os.close(fd)
    head = buf.find(b"shot 320 240\n")
    raw = buf[head + 13:head + 13 + W * H * 2] if head >= 0 else b""
    if len(raw) < W * H * 2:
        print(f"截屏数据不完整:收到 {len(raw)}/{W * H * 2} 字节", file=sys.stderr)
        return 1
    img = Image.new("RGB", (W, H))
    px = []
    for i in range(0, len(raw), 2):
        v = raw[i] | raw[i + 1] << 8
        px.append(((v >> 8) & 0xF8, (v >> 3) & 0xFC, (v << 3) & 0xF8))
    img.putdata(px)
    img.save(args.out)
    print(f"已保存 {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
