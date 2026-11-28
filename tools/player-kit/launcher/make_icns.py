#!/usr/bin/env python3
# Writes the launcher's icon as a macOS .icns (the app's AppIcon.icns), drawn
# by make_icon.py (the same artwork as launcher.ico, which this leaves as it
# is): PNG images at 16 to 512 points, each at 1x and 2x.
#   python3 tools/player-kit/launcher/make_icns.py <out.icns>
import os
import struct
import sys

sys.dont_write_bytecode = True  # no __pycache__ beside the sources
sys.path.insert(0,os.path.dirname(os.path.abspath(__file__)))
import make_icon  # noqa: E402

# (type, pixels): Apple's PNG icon types.
TYPES = (
    (b"icp4", 16), (b"icp5", 32), (b"ic11", 32),    # 16 pt, 1x and 2x
    (b"ic12", 64),                                  # 32 pt, 2x
    (b"ic07", 128), (b"ic13", 256),                 # 128 pt
    (b"ic08", 256), (b"ic14", 512),                 # 256 pt
    (b"ic09", 512),                                 # 512 pt, 1x
)


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: make_icns.py <out.icns>")
    pngs = {}
    body = b""
    for kind, size in TYPES:
        if size not in pngs:
            pngs[size] = make_icon.png(size)
        body += kind + struct.pack(">I", 8 + len(pngs[size])) + pngs[size]
    out = b"icns" + struct.pack(">I", 8 + len(body)) + body
    with open(sys.argv[1], "wb") as f:
        f.write(out)
    print(f"{sys.argv[1]}: {len(out)} bytes")


if __name__ == "__main__":
    main()
