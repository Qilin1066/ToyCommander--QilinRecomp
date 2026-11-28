#!/usr/bin/env python3
# Draws the launcher's icon (launcher.ico, committed beside this script):
# a rounded square in deep blue with a light play triangle, at 16, 24, 32,
# 48, 64 and 256 pixels, each antialiased by 4x4 supersampling. Original
# artwork, made only by this script: run it again after changing it.
#   python3 tools/player-kit/launcher/make_icon.py
# With --png <file>, it writes only that: the 256-pixel image as a PNG (the
# Linux download's kit/icon.png, for its applications-menu entry), and
# leaves launcher.ico as it is.
import math
import os
import sys
import struct
import zlib

SIZES = (16, 24, 32, 48, 64, 256)
TOP = (0x2F, 0x6F, 0xE0)      # the square's colour at its top...
BOTTOM = (0x16, 0x2E, 0x7A)   # ...and at its bottom
MARK = (0xF4, 0xF7, 0xFF)     # the triangle


def inside_square(x, y, radius):
    # A square from 0.03 to 0.97 with rounded corners.
    lo, hi = 0.03, 0.97
    cx = min(max(x, lo + radius), hi - radius)
    cy = min(max(y, lo + radius), hi - radius)
    return lo <= x <= hi and lo <= y <= hi and (x - cx) ** 2 + (y - cy) ** 2 <= radius ** 2


def inside_triangle(x, y):
    # Pointing right, its centre of mass a little right of the middle.
    ax, ay, bx, by, cx, cy = 0.37, 0.26, 0.37, 0.74, 0.76, 0.50
    def side(px, py, qx, qy):
        return (qx - px) * (y - py) - (qy - py) * (x - px)
    d1, d2, d3 = side(ax, ay, bx, by), side(bx, by, cx, cy), side(cx, cy, ax, ay)
    return (d1 <= 0 and d2 <= 0 and d3 <= 0) or (d1 >= 0 and d2 >= 0 and d3 >= 0)


def render(size):
    n = 4
    radius = 0.22
    rows = []
    for py in range(size):
        row = bytearray([0])  # PNG filter: none
        for px in range(size):
            cover = mark = 0
            for sy in range(n):
                for sx in range(n):
                    x = (px + (sx + 0.5) / n) / size
                    y = (py + (sy + 0.5) / n) / size
                    if inside_square(x, y, radius):
                        cover += 1
                        if inside_triangle(x, y):
                            mark += 1
            t = py / max(size - 1, 1)
            base = [round(TOP[i] + (BOTTOM[i] - TOP[i]) * t) for i in range(3)]
            if cover:
                f = mark / cover
                colour = [round(base[i] + (MARK[i] - base[i]) * f) for i in range(3)]
            else:
                colour = [0, 0, 0]
            row += bytes(colour) + bytes([round(255 * cover / (n * n))])
        rows.append(bytes(row))
    return b"".join(rows)


def png(size):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(render(size), 9)) + chunk(b"IEND", b"")


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--png":
        data = png(256)
        with open(sys.argv[2], "wb") as f:
            f.write(data)
        print(f"{sys.argv[2]}: {len(data)} bytes")
        return
    if len(sys.argv) != 1:
        sys.exit("usage: make_icon.py [--png <file>]")
    images = [png(s) for s in SIZES]
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(images)
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "launcher.ico")
    with open(path, "wb") as f:
        f.write(out)
    print(f"{path}: {len(out)} bytes")


if __name__ == "__main__":
    main()
