#!/usr/bin/env python3
"""The natives' register footprints, from their source: recomp/src/native_footprints.inc.

A direct call from lifted code stores and reloads only the registers its
callee can read or change in the context (recomp/src/lifter.hpp). For a
replaced function the callee is a native, C++ code on rt::Context: this
reads each native's body, and those of the functions it passes the context
to, for every register it names (c.r[4], c.fr[0], c.mach ...). A native
whose registers it can't know for certain gets none written, which leaves
the lifter's default, every register: the context used any other way (a
register by a computed index, the whole context copied or assigned, its
address taken, passed to a function it can't find or through a pointer,
which is how guest code is run: rt::call, rt::call_guest). PR is in every
footprint: the return (rt::replaced) reads it.

The lifted code checks the rest at run time: a native's stub compares the
registers outside its footprint before and after (rt::replaced_within) and
stops if one changed.

    python3 tools/check/native_footprints.py          # write the table
    python3 tools/check/native_footprints.py --check  # CI: fail if it's out of date

Only the sources are read: no game files.
"""
import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, "recomp", "src", "native_footprints.inc")
# The games: their own sources, and the array the layout's record names (layout.cpp).
GAMES = [("toycommander", "kToyCommanderFootprints", "Toy Commander (USA)"), ("toyracer", "kToyRacerFootprints", "Toy Racer (Europe)")]
SHARED = ["shinobi/src", "dreamcast/src", "recomp/runtime"]

# RegSet's bits (recomp/src/lifter.hpp).
NAMED = {"t": 16, "pr": 17, "gbr": 18, "mach": 19, "macl": 20, "s": 21, "q": 22, "m": 23, "fpul": 24}
CONTEXT_ONLY = {"sr", "fpscr", "xf", "ret"}  # always in the context, never in lifted code's locals
ALL = (1 << 25) - 1 | (0xFFFF << 32)
PR = 1 << 17


def strip(text):
    """Comments, string and character literals blanked (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        ch = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif ch in "\"'":
            j = i + 1
            while j < n and text[j] != ch:
                j += 2 if text[j] == "\\" else 1
            out.append(ch + " " * (j - i - 1) + ch)
            i = j + 1
        else:
            out.append(ch)
            i += 1
    return "".join(out)


DEF = re.compile(r"\b([A-Za-z_]\w*)\s*\(([^()]*)\)\s*(?:const\s*)?(?:noexcept\s*)?\{")
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "sizeof", "decltype", "alignof"}


def definitions(paths):
    """name -> [(context parameter's position, its name, body)], every function taking an rt::Context&."""
    defs = {}
    for path in paths:
        with open(path, encoding="utf-8") as f:
            text = strip(f.read())
        for m in DEF.finditer(text):
            name, params = m.group(1), m.group(2)
            if name in KEYWORDS or "Context" not in params:
                continue
            parts = [p.strip() for p in params.split(",")]
            for k, p in enumerate(parts):
                pm = re.fullmatch(r"(?:const\s+)?(?:(?:qilin::)?rt::)?Context\s*&\s*(\w+)", p)
                if pm or re.fullmatch(r"(?:const\s+)?(?:(?:qilin::)?rt::)?Context\s*&", p):
                    depth, j = 1, m.end()
                    while depth and j < len(text):
                        depth += {"{": 1, "}": -1}.get(text[j], 0)
                        j += 1
                    defs.setdefault(name, []).append((k, pm.group(1) if pm else None, text[m.end():j - 1]))
                    break
    return defs


def uses(body, ctx):
    """The registers `ctx` is used for in `body`, and the calls it's passed to: (bits, [(callee, position)]),
    or (None, why) if it's used some other way."""
    bits, calls = 0, []
    for m in re.finditer(r"\b%s\b" % re.escape(ctx), body):
        after = body[m.end():]
        fm = re.match(r"\s*\.\s*(\w+)", after)
        if fm:
            field = fm.group(1)
            if field in ("r", "fr"):
                im = re.match(r"\s*\.\s*\w+\s*\[\s*(\d+)u?\s*\]", after)
                if not im or int(im.group(1)) > 15:
                    return None, f"{ctx}.{field} not by a constant index"
                bits |= 1 << (int(im.group(1)) + (32 if field == "fr" else 0))
            elif field in NAMED:
                bits |= 1 << NAMED[field]
            elif field not in CONTEXT_ONLY:
                return None, f"{ctx}.{field}"
            continue
        # Passed whole to a call: the argument itself, a plain function's.
        if not re.match(r"\s*[,)]", after):
            return None, "the whole context used"
        depth, j, position = 0, m.start() - 1, 0
        while j >= 0:
            ch = body[j]
            if ch in ")]}":
                depth += 1
            elif ch in "([{":
                if depth == 0:
                    break
                depth -= 1
            elif ch == "," and depth == 0:
                position += 1
            elif ch == ";" and depth == 0:
                return None, "the whole context used"
            j -= 1
        if j < 0 or body[j] != "(":
            return None, "the whole context used"
        before = body[:j].rstrip()
        cm = re.search(r"((?:\w+\s*::\s*)*)(\w+)$", before)
        if not cm or cm.group(2) in KEYWORDS:
            return None, "the whole context used"
        lead = before[:cm.start()].rstrip()
        if lead.endswith(".") or lead.endswith("->"):  # a member or a pointer called
            return None, f"passed to a function through a pointer ({cm.group(2)})"
        calls.append((cm.group(2), position))
    return bits, calls


def footprints(defs):
    """name -> bits (None: everything) for every function defined, by a fixed point over the calls; and
    name -> why, for those that are everything."""
    result = {name: 0 for name in defs}
    why = {}
    local = {}
    for name, ds in defs.items():
        total, calls = 0, []
        for _, ctx, body in ds:
            if ctx is None:
                continue
            b, c = uses(body, ctx)
            if b is None:
                total, why[name] = None, c
                break
            total |= b
            calls += c
        local[name] = (total, calls)
    changed = True
    while changed:
        changed = False
        for name, (b, calls) in local.items():
            if result[name] is None:
                continue
            if b is None:
                result[name], changed = None, True
                continue
            new = result[name] | b
            for callee, position in calls:
                fits = [d for d in defs.get(callee, []) if d[0] == position]
                if not fits or result.get(callee) is None:
                    new = None
                    why[name] = f"{callee}: {why[callee]}" if fits else f"passed to {callee}, not found"
                    break
                new |= result[callee]
            if new != result[name]:
                result[name], changed = new, True
    return result, why


def natives(game):
    """[(address, registered name, function)] the game provides."""
    src = os.path.join(ROOT, game, "src")
    with open(os.path.join(src, "build.hpp"), encoding="utf-8") as f:
        block = re.search(r"\.natives\s*=\s*\{(.*?)\}", f.read(), re.S)
    fields = {k: int(v, 16) for k, v in re.findall(r"\.(\w+)\s*=\s*0x([0-9A-Fa-f]+)u", block.group(1))}
    out = []
    for folder in (src, os.path.join(ROOT, "shinobi", "src")):
        for name in sorted(os.listdir(folder)):
            if not name.endswith(".cpp"):
                continue
            with open(os.path.join(folder, name), encoding="utf-8") as f:
                text = f.read()
            for a, label, fn in re.findall(r"\{\s*0x([0-9A-Fa-f]{8})u\s*,\s*\"(\w+)\"\s*,\s*(\w+)\s*\}", text):
                out.append((int(a, 16), label, fn))
            for field, label, fn in re.findall(r"\{\s*kBuild\.natives\.(\w+)\s*,\s*\"(\w+)\"\s*,\s*(\w+)\s*\}", text):
                if fields.get(field, 0):
                    out.append((fields[field], label, fn))
    return sorted(out)


def sources(game):
    paths = []
    for folder in [game + "/src"] + SHARED:
        for name in sorted(os.listdir(os.path.join(ROOT, folder))):
            if name.endswith((".cpp", ".hpp")):
                paths.append(os.path.join(ROOT, folder, name))
    return paths


def register_names(bits):
    names = [f"r{n}" for n in range(16) if bits >> n & 1]
    names += [k for k, b in NAMED.items() if bits >> b & 1]
    names += [f"fr{n}" for n in range(16) if bits >> (32 + n) & 1]
    return " ".join(names)


def table():
    lines = ["// Generated by tools/check/native_footprints.py from the natives' sources: don't edit.",
             "// Each native whose registers are known: its entry, the registers it can read or change in the",
             "// context (lifter.hpp's RegSet, PR included), and its name. The others aren't here: every register.", ""]
    for game, array, title in GAMES:
        fp, why = footprints(definitions(sources(game)))
        rows, others = [], []
        for address, label, fn in natives(game):
            bits = fp.get(fn)
            if bits is None:
                others.append(f"//   0x{address:08X}  {label}: {why.get(fn, 'no definition found') if fn in fp else 'no definition found'}")
                continue
            bits |= PR
            rows.append(f"    {{0x{address:08X}u, 0x{bits:X}ull, \"{label}\"}},  // {register_names(bits)}")
        lines.append(f"// {title}: {len(rows)} of {len(natives(game))} natives.")
        lines.append(f"constexpr NativeFootprint {array}[] = {{")
        lines += rows
        lines.append("};")
        lines.append("// Not listed (every register), and why:")
        lines += others
        lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true", help="fail if the table isn't what the sources give")
    args = ap.parse_args()
    want = table()
    if args.check:
        try:
            with open(OUT, encoding="utf-8") as f:
                have = f.read().replace("\r\n", "\n")
        except FileNotFoundError:
            have = ""
        if have != want:
            print(f"{os.path.relpath(OUT, ROOT)} is out of date: run python3 tools/check/native_footprints.py", file=sys.stderr)
            return 1
        print("native footprints up to date")
        return 0
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write(want)
    print(f"wrote {os.path.relpath(OUT, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
