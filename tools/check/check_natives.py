#!/usr/bin/env python3
"""Checks that every replaced function lifted code can reach has a native.

`qilin lift` writes a stub for each replaced function (stubs.cpp) and the
lifted functions (lifted_*.cpp). A replaced function that lifted code calls,
or whose address it holds, is reachable; the game's program must register a
native for it, or the game stops there as unregistered: the game's own
natives are `{0x...u, "name", fn}` in the tables of <game>/src/*.cpp, and the
shared Shinobi replacements' (shinobi/src) are the `.natives = {...}` block of
<game>/src/build.hpp, one `.field = 0x...u` each (0 for one the game doesn't
register). This lists the ones without:

    build/qilin lift <the game's .cue> --out gen
    python3 tools/check/check_natives.py gen toycommander/src

Exit status 1 if any reachable replaced function has no native. Reads only
the generated code (never committed) and the game's own sources.
"""
import argparse
import glob
import os
import re
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("gen", help="the folder qilin lift wrote")
    parser.add_argument("src", help="the game's sources (toycommander/src or toyracer/src)")
    args = parser.parse_args()

    with open(os.path.join(args.gen, "stubs.cpp")) as f:
        replaced = set(re.findall(r"fn_([0-9A-F]{8})", f.read()))
    reached = set()
    for path in glob.glob(os.path.join(args.gen, "lifted_*.cpp")):
        with open(path) as f:
            text = f.read()
        reached |= set(re.findall(r"fn_([0-9A-F]{8})\(c\)", text))
        reached |= set(re.findall(r"0x([0-9A-F]{8})u", text))
    natives = set()
    for path in glob.glob(os.path.join(args.src, "*.cpp")):
        with open(path) as f:
            natives |= {a.upper() for a in re.findall(r"\{\s*0x([0-9A-Fa-f]{8})u\s*,\s*\"", f.read())}
    build = os.path.join(args.src, "build.hpp")
    if os.path.exists(build):
        with open(build) as f:
            block = re.search(r"\.natives\s*=\s*\{(.*?)\}", f.read(), re.S)
        if not block:
            print(f"{build}: no .natives block", file=sys.stderr)
            return 2
        natives |= {a.upper() for a in re.findall(r"\.\w+\s*=\s*0x([0-9A-Fa-f]{8})u", block.group(1))}

    reachable = sorted(replaced & reached)
    missing = [a for a in reachable if a not in natives]
    print(f"{len(replaced)} replaced, {len(reachable)} reachable from lifted code, {len(missing)} without a native")
    for a in missing:
        print(f"  0x{a}")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
