#!/usr/bin/env python3
"""Compares qilin's SH-4 decoder with Capstone's, word by word.

SH-4 instructions are 16-bit words, so the whole encoding space is 65,536
words: this checks all of it, at two addresses (PC-relative operands depend
on the address and its alignment), and needs no game files.

    pip install capstone        (5.0 or later: the first with SuperH)
    python3 tools/check/verify_decoder.py --qilin build/qilin

Both must agree on which words are instructions and on their text, except
for the reviewed differences below. Exit status 1 on any other difference.
"""
import argparse
import struct
import subprocess
import sys

try:
    import capstone
except ImportError:
    sys.exit("verify_decoder: needs Capstone (pip install capstone)")

# Reviewed difference 1: Capstone's SH-4 mode also decodes instructions that
# later SuperH cores added (SH-2A, SH-4A) and that the Dreamcast's SH7091
# doesn't have. qilin treats those words as invalid, on purpose: they can only
# be data. They are recognised here by mnemonic, register or addressing mode.
NOT_SH4_MNEMONICS = {
    "movrt", "nott", "resbank", "rts/n", "rtv/n", "synco", "mulr", "clipu.b", "clipu.w",
    "clips.b", "clips.w", "divu", "divs", "movua.l", "movmu.l", "movml.l", "stbank", "ldbank",
    "jsr/n", "fpchg", "icbi", "prefi", "movco.l", "movli.l",
}


def not_sh4(mnemonic, operands):
    if mnemonic in NOT_SH4_MNEMONICS:
        return True
    if "tbr" in operands:  # the SH-2A's table base register
        return True
    if mnemonic in ("ldc", "ldc.l") and operands.endswith(",sgr"):  # SH-4A: the SH-4 only stores SGR
        return True
    # SH-2A: mov.b/w/l r0,@rN+ and @-rM,r0
    if mnemonic in ("mov.b", "mov.w", "mov.l") and (operands.startswith("@-") or operands.endswith("+")):
        return operands.startswith("@-") or operands.startswith("r0,@")
    return False


# Reviewed difference 2: SH-4 instructions Capstone 5 doesn't decode. qilin's
# text for them follows Hitachi's SH-4 manual and is pinned in the unit tests.
CAPSTONE_LACKS = {"fipr", "fcnvds", "fcnvsd"}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qilin", required=True, help="path to the qilin executable")
    args = ap.parse_args()
    if not hasattr(capstone, "CS_ARCH_SH"):
        sys.exit("verify_decoder: this Capstone has no SuperH support (needs 5.0 or later)")
    md = capstone.Cs(capstone.CS_ARCH_SH,
                     capstone.CS_MODE_SH4 | capstone.CS_MODE_SHFPU | capstone.CS_MODE_LITTLE_ENDIAN)
    failures = 0
    for pc in (0x8C010000, 0x8C010002):
        out = subprocess.run([args.qilin, "disasm", "--pc", f"{pc:x}"], check=True, capture_output=True, text=True).stdout
        lines = out.splitlines()
        if len(lines) != 0x10000:
            sys.exit(f"verify_decoder: qilin disasm printed {len(lines)} lines, not 65536")
        agree = both_invalid = extra = lacks = 0
        for word, line in enumerate(lines):
            addr, hexword, ours = line.split(" ", 2)
            if int(addr, 16) != pc or int(hexword, 16) != word:
                sys.exit(f"verify_decoder: unexpected line '{line}'")
            theirs = None
            for insn in md.disasm(struct.pack("<H", word), pc, 1):
                theirs = (insn.mnemonic, insn.op_str)
            if ours == "(invalid)":
                if theirs is None:
                    both_invalid += 1
                elif not_sh4(*theirs):
                    extra += 1
                else:
                    failures += 1
                    print(f"{pc:08x} {word:04x}: qilin invalid, Capstone '{' '.join(theirs).strip()}'")
                continue
            if theirs is None:
                if ours.split(" ")[0] in CAPSTONE_LACKS:
                    lacks += 1
                else:
                    failures += 1
                    print(f"{pc:08x} {word:04x}: qilin '{ours}', Capstone invalid")
                continue
            text = " ".join(theirs).strip()
            if text == ours and not not_sh4(*theirs):
                agree += 1
            else:
                failures += 1
                print(f"{pc:08x} {word:04x}: qilin '{ours}', Capstone '{text}'")
        print(f"pc {pc:08x}: {agree} words agree, {both_invalid} invalid in both, "
              f"{extra} not SH-4 (Capstone only), {lacks} SH-4 (qilin only: {', '.join(sorted(CAPSTONE_LACKS))})")
    if failures:
        sys.exit(f"verify_decoder: {failures} unexplained difference(s)")
    print("verify_decoder: no unexplained differences")


if __name__ == "__main__":
    main()
