#!/usr/bin/env python3
# Writes the iPadOS and iOS app's icon (the iOS kit's
# xcode/Assets.xcassets/AppIcon.appiconset/AppIcon.png): the launcher's
# artwork (launcher/make_icon.py: the deep blue gradient and the light play
# triangle), as one opaque 1024-pixel square, which the system rounds itself.
# Antialiased by 3x3 supersampling.
#   python3 tools/player-kit/ios/make_ios_icon.py <out.png>
import os
import struct
import sys
import zlib

sys.dont_write_bytecode = True  # no __pycache__ beside the sources
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launcher"))
import make_icon  # noqa: E402

SIZE = 1024
N = 3


def render():
    rows = []
    for py in range(SIZE):
        t = py / (SIZE - 1)
        base = [round(make_icon.TOP[i] + (make_icon.BOTTOM[i] - make_icon.TOP[i]) * t) for i in range(3)]
        row = bytearray([0])  # PNG filter: none
        y_samples = [(py + (sy + 0.5) / N) / SIZE for sy in range(N)]
        for px in range(SIZE):
            x = (px + 0.5) / SIZE
            # Well away from the triangle (its box is 0.37-0.76 by 0.26-0.74): no sampling.
            if x < 0.36 or x > 0.77 or y_samples[0] < 0.25 or y_samples[-1] > 0.75:
                row += bytes(base)
                continue
            mark = 0
            for sx in range(N):
                xs = (px + (sx + 0.5) / N) / SIZE
                for ys in y_samples:
                    if make_icon.inside_triangle(xs, ys):
                        mark += 1
            f = mark / (N * N)
            row += bytes(round(base[i] + (make_icon.MARK[i] - base[i]) * f) for i in range(3))
        rows.append(bytes(row))
    return b"".join(rows)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_ios_icon.py <out.png>")

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", SIZE, SIZE, 8, 2, 0, 0, 0)  # 8-bit RGB: no alpha
    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(render(), 9)) + chunk(b"IEND", b"")
    with open(sys.argv[1], "wb") as f:
        f.write(data)
    print(f"{sys.argv[1]}: {len(data)} bytes")


if __name__ == "__main__":
    main()
