// Self-contained tests: SHA-256, the boot file loader, the SH-4 decoder,
// function discovery on synthetic programs and the GD-ROM reader on a
// synthetic disc. No game files required (runs in CI).
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "bootfile.hpp"
#include "cfg.hpp"
#include "decoder.hpp"
#include "fpmode.hpp"
#include "gdrom.hpp"
#include "hwaccess.hpp"
#include "layout.hpp"
#include "sha256.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

std::span<const uint8_t> bytes(std::string_view s) { return {reinterpret_cast<const uint8_t*>(s.data()), s.size()}; }

// ---------------------------------------------------------------- SHA-256

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(bytes("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::vector<uint8_t> million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------- boot file

void test_bootfile() {
    const std::vector<uint8_t> file{0x09, 0x00, 0x0B, 0x00, 0x78, 0x56, 0x34, 0x12};
    const auto boot = BootFile::parse(file);
    CHECK(boot.has_value());
    if (boot) {
        CHECK(boot->base() == 0x8C010000u && boot->entry() == 0x8C010000u);
        CHECK(boot->size() == 8 && boot->end() == 0x8C010008u);
        CHECK(boot->u16(0x8C010000u) == 0x0009 && boot->u16(0x8C010002u) == 0x000B);
        CHECK(boot->u32(0x8C010004u) == 0x12345678u && boot->u8(0x8C010007u) == 0x12);
        CHECK(boot->mapped(0x8C010007u) && !boot->mapped(0x8C010008u) && !boot->mapped(0x8C00FFFFu));
        CHECK(!boot->mapped(0x8C010006u, 4) && !boot->mapped(0xFFFFFFFFu, 2));
        CHECK(boot->sha256 == sha256_hex(file));
        CHECK(known_build(boot->sha256) == nullptr);
        bool threw = false;
        try {
            (void)boot->u32(0x8C010006u);
        } catch (const BootFileError&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(to_json(*boot).contains("\"build\": null") && to_json(*boot).contains("\"entry\": \"0x8C010000\""));
    }
    CHECK(!BootFile::parse({}).has_value());
    CHECK(!BootFile::parse(std::vector<uint8_t>(3)).has_value());
    CHECK(BootFile::parse(std::vector<uint8_t>(0xFF0000)).has_value());   // fills RAM exactly
    CHECK(!BootFile::parse(std::vector<uint8_t>(0xFF0002)).has_value());  // two bytes over
    CHECK(!BootFile::load("no-such-file.bin").has_value());
    const KnownBuild* usa = known_build("ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd");
    CHECK(usa && usa->product == "MK-57020" && usa->version == "V1.022");
}

// ---------------------------------------------------------------- decoder

struct Pinned {
    uint16_t word;
    uint32_t pc;
    std::string_view text;
};

// Text pinned to what Capstone 5.0.7 prints (tools/check/verify_decoder.py
// compares the whole encoding space); the last three, which Capstone doesn't
// decode, to the forms of Hitachi's SH-4 manual.
constexpr Pinned kPinned[] = {
    {0x0009, 0x8C010000, "nop"},
    {0x000B, 0x8C010000, "rts"},
    {0xE0FF, 0x8C010000, "mov #-1,r0"},
    {0x7F80, 0x8C010000, "add #-128,r15"},
    {0x88FF, 0x8C010000, "cmp/eq #-1,r0"},
    {0xC8FF, 0x8C010000, "tst #255,r0"},
    {0xCDFF, 0x8C010000, "and.b #255,@(r0,gbr)"},
    {0xC3FF, 0x8C010000, "trapa #255"},
    {0x9FFF, 0x8C010000, "mov.w 0x8c010202,r15"},
    {0x9FFF, 0x8C010002, "mov.w 0x8c010204,r15"},
    {0xDFFF, 0x8C010000, "mov.l 0x8c010400,r15"},
    {0xDFFF, 0x8C010002, "mov.l 0x8c010400,r15"},  // the PC's low two bits are dropped
    {0xC7FF, 0x8C010002, "mova 0x8c010400,r0"},
    {0x6123, 0x8C010000, "mov r2,r1"},
    {0x2F86, 0x8C010000, "mov.l r8,@-r15"},
    {0x68F6, 0x8C010000, "mov.l @r15+,r8"},
    {0x80FF, 0x8C010000, "mov.b r0,@(15,r15)"},
    {0x81FF, 0x8C010000, "mov.w r0,@(30,r15)"},
    {0x1FE3, 0x8C010000, "mov.l r14,@(12,r15)"},
    {0x51F3, 0x8C010000, "mov.l @(12,r15),r1"},
    {0x0126, 0x8C010000, "mov.l r2,@(r0,r1)"},
    {0x012E, 0x8C010000, "mov.l @(r0,r2),r1"},
    {0xC2FF, 0x8C010000, "mov.l r0,@(1020,gbr)"},
    {0xC5FF, 0x8C010000, "mov.w @(510,gbr),r0"},
    {0x012F, 0x8C010000, "mac.l @r2+,@r1+"},
    {0x89FF, 0x8C010000, "bt 0x8c010002"},
    {0x8F7F, 0x8C010000, "bf/s 0x8c010102"},
    {0xAFFF, 0x8C010000, "bra 0x8c010002"},
    {0xA7FF, 0x8C010000, "bra 0x8c011002"},
    {0xB800, 0x8C010000, "bsr 0x8c00f004"},
    {0x0323, 0x8C010000, "braf r3"},
    {0x430B, 0x8C010000, "jsr @r3"},
    {0x432B, 0x8C010000, "jmp @r3"},
    {0x40FE, 0x8C010000, "ldc r0,r7_bank"},
    {0x0FF2, 0x8C010000, "stc r7_bank,r15"},
    {0x4FF3, 0x8C010000, "stc.l r7_bank,@-r15"},
    {0x40F7, 0x8C010000, "ldc.l @r0+,r7_bank"},
    {0x03FA, 0x8C010000, "stc dbr,r3"},
    {0x4F22, 0x8C010000, "sts.l pr,@-r15"},
    {0x4F26, 0x8C010000, "lds.l @r15+,pr"},
    {0x436A, 0x8C010000, "lds r3,fpscr"},
    {0x035A, 0x8C010000, "sts fpul,r3"},
    {0x03C3, 0x8C010000, "movca.l r0,@r3"},
    {0x0383, 0x8C010000, "pref @r3"},
    {0xF12C, 0x8C010000, "fmov fr2,fr1"},
    {0xF126, 0x8C010000, "fmov @(r0,r2),fr1"},
    {0xF129, 0x8C010000, "fmov @r2+,fr1"},
    {0xF12B, 0x8C010000, "fmov fr2,@-r1"},
    {0xF12E, 0x8C010000, "fmac fr0,fr2,fr1"},
    {0xF31D, 0x8C010000, "flds fr3,fpul"},
    {0xF32D, 0x8C010000, "float fpul,fr3"},
    {0xF33D, 0x8C010000, "ftrc fr3,fpul"},
    {0xF37D, 0x8C010000, "fsrra fr3"},
    {0xF2FD, 0x8C010000, "fsca fpul,dr2"},
    {0xFDFD, 0x8C010000, "ftrv xmtrx,fv12"},
    {0xF3FD, 0x8C010000, "fschg"},
    {0xFBFD, 0x8C010000, "frchg"},
    {0xF6ED, 0x8C010000, "fipr fv8,fv4"},
    {0xF4BD, 0x8C010000, "fcnvds dr4,fpul"},
    {0xF4AD, 0x8C010000, "fcnvsd fpul,dr4"},
};

// Words that are not SH-4 instructions: undefined encodings, and the SH-2A,
// SH-4A and SH-DSP additions other disassemblers accept.
constexpr uint16_t kInvalid[] = {
    0x0000, 0x0001, 0xFFFF, 0xFFFD, 0xF7FD /* fpchg */, 0x00AB /* synco */, 0x0039 /* movrt */,
    0x004A /* stc tbr */, 0x005B /* resbank */, 0x0068 /* nott */, 0x006B /* rts/n */,
    0x4080 /* mulr */, 0x4081 /* clipu.b */, 0x408B /* mov.b r0,@rn+ */, 0x40A9 /* movua.l */,
    0x40F0 /* movmu.l */, 0x404B /* jsr/n */, 0x433A /* ldc rn,sgr */, 0x4036 /* ldc.l @rn+,sgr */,
    0x8200, 0x8300, 0x8600, 0x8700, 0x8A00, 0x8C00, 0x8E00,
    0xF1BD /* fcnvds with an odd register */, 0xF1AD,
    0xF00F, 0xF0CD, 0xF0DD, 0x0013, 0x0033, 0x0073, 0x4014, 0x4030,
};

void test_decoder() {
    using namespace sh4;
    for (const Pinned& p : kPinned) {
        const Insn i = decode(p.word);
        CHECK(i.valid());
        if (!i.valid()) continue;
        const std::string text = disasm(i, p.pc);
        if (text != p.text) {
            std::println(stderr, "{:04X}: got '{}', want '{}'", p.word, text, p.text);
            ++failures;
        }
    }
    for (uint16_t w : kInvalid) {
        if (decode(w).valid()) {
            std::println(stderr, "{:04X} must not decode, got '{}'", w, disasm(decode(w), 0x8C010000));
            ++failures;
        }
    }

    // Every form: its canonical word and the word with every field bit set
    // decode to it, and it is the only form that claims them.
    CHECK(specs().size() == static_cast<size_t>(Op::count_) - 1);
    for (const Spec& s : specs()) {
        CHECK(decode(s.match).op == s.op);
        CHECK(decode(static_cast<uint16_t>(s.match | ~s.mask)).op == s.op);
        CHECK(spec(s.op).match == s.match && spec(s.op).name == s.name);
        uint16_t fields = 0;
        for (Opnd o : s.operands) fields |= field_mask(o);
        CHECK(static_cast<uint16_t>(s.mask | fields) == 0xFFFF && (s.mask & fields) == 0);
        // A delayed branch is a branch; a call or return is one too.
        if (s.flags & (kDelayed | kCall | kReturn | kConditional)) CHECK(s.flags & kBranch);
        if (s.flags & (kFpPr | kFpSz | kFpscrWrite)) CHECK(s.flags & kFloat);
    }
    // The size of the SH-4's set in the encoding space, pinned: a change to
    // the table that adds or removes words shows here.
    unsigned valid = 0, delayed = 0;
    for (uint32_t w = 0; w < 0x10000; ++w) {
        const Insn i = decode(static_cast<uint16_t>(w));
        valid += i.valid();
        if (i.valid()) {
            delayed += (spec(i.op).flags & kDelayed) != 0;
            CHECK(!disasm(i, 0x8C010000).empty());
        }
    }
    CHECK(valid == 59001);
    CHECK(delayed == 8770);

    const Insn bra = decode(0xA800);  // the most negative 12-bit displacement
    CHECK(bra.branch_target(0x8C010000) == 0x8C00F004u);
    CHECK(decode(0x8B80).branch_target(0x8C010000) == 0x8C00FF04u);
    CHECK(decode(0xD001).pc_relative(0x8C010002) == 0x8C010008u);
    CHECK(decode(0x9001).pc_relative(0x8C010002) == 0x8C010008u);
    CHECK(decode(0x9001).pc_relative(0x8C010000) == 0x8C010006u);
    bool threw = false;
    try {
        (void)decode(0x0009).branch_target(0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        (void)disasm(decode(0xFFFF), 0);
    } catch (const std::logic_error&) {
        threw = true;
    }
    CHECK(threw);
}

// ---------------------------------------------------------------- function discovery

constexpr uint32_t kB = kBootFileBase;

// A boot file from 16-bit words (a 32-bit value is two: low half first).
BootFile program(std::initializer_list<uint16_t> words) {
    std::vector<uint8_t> file;
    for (uint16_t w : words) {
        file.push_back(static_cast<uint8_t>(w));
        file.push_back(static_cast<uint8_t>(w >> 8));
    }
    return *BootFile::parse(file);
}
constexpr uint16_t lo(uint32_t v) { return static_cast<uint16_t>(v); }
constexpr uint16_t hi(uint32_t v) { return static_cast<uint16_t>(v >> 16); }

bool has_rejected(const Cfg& cfg, uint32_t address, Origin origin) {
    return std::ranges::any_of(cfg.rejected, [&](const Rejected& r) { return r.address == address && r.origin == origin; });
}

void test_gpr_written() {
    const auto w = [](uint16_t word) { return gpr_written(sh4::decode(word)); };
    CHECK(w(0x68F6) == ((1u << 15) | (1u << 8)));  // mov.l @r15+,r8
    CHECK(w(0x2F86) == (1u << 15));                // mov.l r8,@-r15
    CHECK(w(0x3210) == 0);                         // cmp/eq r1,r2
    CHECK(w(0x321C) == (1u << 2));                 // add r1,r2
    CHECK(w(0x430B) == 0);                         // jsr @r3
    CHECK(w(0x8424) == 1);                         // mov.b @(4,r2),r0
    CHECK(w(0x4510) == (1u << 5));                 // dt r5
    CHECK(w(0xF129) == (1u << 2));                 // fmov @r2+,fr1
    CHECK(w(0x0302) == (1u << 3));                 // stc sr,r3
    CHECK(w(0xC801) == 0);                         // tst #1,r0
    CHECK(w(0xC901) == 1);                         // and #1,r0
    CHECK(w(0x012F) == ((1u << 1) | (1u << 2)));   // mac.l @r2+,@r1+
    CHECK(w(0x0429) == (1u << 4));                 // movt r4
    CHECK(w(0x2122) == 0);                         // mov.l r2,@r1
    CHECK(w(0x0323) == 0);                         // braf r3
    CHECK(w(0xC701) == 1);                         // mova
}

void test_cfg_calls() {
    // A call through a literal-pool address and a bsr.
    const BootFile boot = program({
        0xD103,  // 00  mov.l 0x10,r1
        0x410B,  // 02  jsr @r1
        0x0009,  // 04   nop
        0xB007,  // 06  bsr 0x18
        0x0009,  // 08   nop
        0x000B,  // 0A  rts
        0x0009,  // 0C   nop
        0x0009,  // 0E  (alignment)
        lo(kB + 0x14), hi(kB + 0x14),  // 10  .long f1
        0x000B,  // 14  f1: rts
        0x0009,  // 16   nop
        0xE005,  // 18  f2: mov #5,r0
        0x000B,  // 1A  rts
        0x0009,  // 1C   nop
    });
    const Cfg cfg = analyse(boot);
    CHECK(cfg.functions.size() == 3);
    CHECK(cfg.functions.contains(kB) && cfg.functions.contains(kB + 0x14) && cfg.functions.contains(kB + 0x18));
    if (cfg.functions.size() != 3) return;
    const Function& f = cfg.functions.at(kB);
    CHECK(f.origin == Origin::entry && f.insns.size() == 7 && f.end() == kB + 0x0E);
    CHECK(f.calls.size() == 2 && f.calls.at(kB + 2) == kB + 0x14 && f.calls.at(kB + 6) == kB + 0x18);
    CHECK(f.indirect_calls.empty() && f.problems.empty());
    CHECK(f.literals.size() == 1 && f.literals[0] == std::pair(kB + 0x10, 4u));
    CHECK(cfg.functions.at(kB + 0x14).origin == Origin::call);
    CHECK(cfg.at(kB + 0x0E) == Kind::padding && cfg.at(kB + 0x10) == Kind::literal && cfg.at(kB + 0x12) == Kind::literal);
    CHECK(cfg.at(kB + 0x0C) == Kind::code && cfg.at(kB + 0x18) == Kind::code);
    CHECK(cfg.owner[0x18 / 2] == kB + 0x18 && cfg.rejected.empty());
    uint32_t unknown = 1;
    CHECK(coverage(cfg, boot, {}, true, unknown).contains("code") && unknown == 0);
    CHECK(summary(cfg).contains("functions            3"));
    CHECK(to_json(cfg).contains("\"entry\":\"8C010018\",\"origin\":\"call\""));
    CHECK(suspects(cfg) == "0 suspects\n");
}

void test_cfg_switch() {
    // SHC's switch: a bound, a table of 16-bit offsets, braf.
    const BootFile boot = program({
        0xE103,  // 00  mov #3,r1
        0x3012,  // 02  cmp/hs r1,r0
        0x890B,  // 04  bt 0x1E
        0x4000,  // 06  shll r0
        0x6103,  // 08  mov r0,r1
        0xC702,  // 0A  mova 0x14,r0
        0x001D,  // 0C  mov.w @(r0,r1),r0
        0x0023,  // 0E  braf r0
        0x0009,  // 10   nop
        0x0000,  // 12  (alignment)
        0x0008, 0x000C, 0x0008,  // 14  .word 0x1A-0x12, 0x1E-0x12, 0x1A-0x12
        0x000B,  // 1A  rts
        0xE001,  // 1C   mov #1,r0
        0x000B,  // 1E  rts
        0xE002,  // 20   mov #2,r0
    });
    const Cfg cfg = analyse(boot);
    CHECK(cfg.functions.size() == 1 && cfg.rejected.empty());
    const Function& f = cfg.functions.at(kB);
    CHECK(f.switches.size() == 1 && f.indirect_jumps.empty() && f.problems.empty());
    if (f.switches.size() == 1) {
        const Switch& sw = f.switches[0];
        CHECK(sw.braf == kB + 0x0E && sw.table == kB + 0x14);
        CHECK(sw.targets == std::vector<uint32_t>({kB + 0x1A, kB + 0x1E, kB + 0x1A}));
    }
    CHECK(f.leaders == std::vector<uint32_t>({kB + 0x1A, kB + 0x1E}));
    CHECK(cfg.at(kB + 0x12) == Kind::padding && cfg.at(kB + 0x14) == Kind::jump_table && cfg.at(kB + 0x18) == Kind::jump_table);
    CHECK(cfg.at(kB + 0x20) == Kind::code);

    // Without the bound there is no table to trust: an indirect jump, reported.
    const BootFile loose = program({0xC701, 0x001D, 0x0023, 0x0009, 0x0008, 0x0008});
    const Cfg lc = analyse(loose);
    CHECK(lc.functions.at(kB).switches.empty() && lc.functions.at(kB).indirect_jumps == std::vector<uint32_t>({kB + 4}));
    CHECK(suspects(lc).contains("an indirect jump"));
}

void test_cfg_address_table() {
    // Hand-written code's table of code addresses, indexed by r0 (the C
    // runtime's unrolled copy at 0x8C081C2C enters itself this way).
    const BootFile boot = program({
        0xD303,  // 00  mov.l 0x10,r3   (the table's address)
        0x033E,  // 02  mov.l @(r0,r3),r3
        0x432B,  // 04  jmp @r3
        0x0009,  // 06   nop
        0x000B,  // 08  rts
        0xE001,  // 0A   mov #1,r0
        0x000B,  // 0C  rts
        0xE002,  // 0E   mov #2,r0
        0x0014, 0x8C01,  // 10  .long 0x8C010014
        0x0008, 0x8C01, 0x000C, 0x8C01,  // 14  .long 0x8C010008, 0x8C01000C
        0x0009, 0x000B,  // 1C  (not addresses: the table ends)
    });
    const Cfg cfg = analyse(boot);
    const Function& f = cfg.functions.at(kB);
    CHECK(f.switches.size() == 1 && f.indirect_jumps.empty());
    if (f.switches.size() == 1) {
        const Switch& sw = f.switches[0];
        CHECK(sw.braf == kB + 4 && sw.table == kB + 0x14 && sw.entry_size == 4);
        CHECK(sw.targets == std::vector<uint32_t>({kB + 0x08, kB + 0x0C}));
    }
    CHECK(cfg.at(kB + 0x08) == Kind::code && cfg.at(kB + 0x0E) == Kind::code);
    CHECK(cfg.at(kB + 0x14) == Kind::jump_table && cfg.at(kB + 0x1A) == Kind::jump_table);
}

void test_cfg_known() {
    // What a layout knows that references don't show: an entry nothing
    // refers to that falls into the next function, a stub that doesn't
    // decode, a data block, and the engine's swapped-nop filler.
    const BootFile boot = program({
        0x000B,  // 00  rts
        0x0009,  // 02   nop
        0x0900,  // 04  (filler: nop with its bytes swapped)
        0x0900,  // 06
        0xE001,  // 08  mov #1,r0       (an entry nothing refers to, falling into 0x0A)
        0xE002,  // 0A  mov #2,r0       (a function the image points at)
        0x000B,  // 0C  rts
        0x0009,  // 0E   nop
        0x003B,  // 10  a debugger's break
        0x000B,  // 12  rts
        0x0009,  // 14   nop
        0x1234,  // 16  data
        0x000A, 0x8C01,  // 18  .long 0x8C01000A
    });
    Known known;
    known.entries = {kB + 0x08};
    known.stubs = {{kB + 0x10, kB + 0x16}};
    known.data = {{kB + 0x16, kB + 0x18}};
    const Cfg cfg = analyse(boot, {}, known);
    CHECK(cfg.functions.contains(kB + 0x08) && cfg.functions.at(kB + 0x08).origin == Origin::layout);
    CHECK(cfg.functions.contains(kB + 0x10) && cfg.functions.at(kB + 0x10).insns.empty());
    CHECK(cfg.at(kB + 0x04) == Kind::padding && cfg.at(kB + 0x06) == Kind::padding);
    CHECK(cfg.at(kB + 0x10) == Kind::code && cfg.at(kB + 0x14) == Kind::code && cfg.at(kB + 0x16) == Kind::data);
}

void test_cfg_join() {
    // r1 holds one function on one path and another on the other: the call
    // through it is not resolved.
    const BootFile boot = program({
        0x8902,  // 00  bt 0x08
        0xD104,  // 02  mov.l 0x14,r1
        0xA001,  // 04  bra 0x0A
        0x0009,  // 06   nop
        0xD103,  // 08  mov.l 0x18,r1
        0x410B,  // 0A  jsr @r1
        0x0009,  // 0C   nop
        0x000B,  // 0E  rts
        0x0009,  // 10   nop
        0x0009,  // 12  (alignment)
        lo(kB + 0x20), hi(kB + 0x20),  // 14
        lo(kB + 0x24), hi(kB + 0x24),  // 18
        0x0009, 0x0009,  // 1C
        0x000B, 0x0009,  // 20  A: rts
        0x000B, 0x0009,  // 24  B: rts
    });
    const Cfg cfg = analyse(boot);
    const Function& f = cfg.functions.at(kB);
    CHECK(f.calls.empty() && f.indirect_calls == std::vector<uint32_t>({kB + 0x0A}));
    // Both are still found, as addresses in the literal pool.
    CHECK(cfg.functions.size() == 3);
    CHECK(cfg.functions.contains(kB + 0x20) && cfg.functions.at(kB + 0x20).origin == Origin::pointer);
    CHECK(cfg.functions.contains(kB + 0x24));

    // The same constant on both paths is resolved.
    const BootFile same = program({
        0x8902, 0xD104, 0xA001, 0x0009, 0xD103, 0x410B, 0x0009, 0x000B, 0x0009, 0x0009,
        lo(kB + 0x20), hi(kB + 0x20), lo(kB + 0x20), hi(kB + 0x20), 0x0009, 0x0009, 0x000B, 0x0009,
    });
    const Cfg sc = analyse(same);
    CHECK(sc.functions.at(kB).calls.size() == 1 && sc.functions.at(kB).calls.at(kB + 0x0A) == kB + 0x20);
    CHECK(sc.functions.at(kB + 0x20).origin == Origin::call);
}

void test_cfg_guesses() {
    // Data holding a function's address, the address of something that isn't
    // code, and a value that isn't an address.
    const BootFile boot = program({
        0x000B, 0x0009,                // 00  rts
        lo(kB + 0x10), hi(kB + 0x10),  // 04  .long f
        lo(kB + 0x18), hi(kB + 0x18),  // 08  .long (zeros)
        0x5678, 0x1234,                // 0C
        0xE001, 0x000B, 0x0009,        // 10  f: mov #1,r0 ; rts ; nop
        0x0009,                        // 16
        0x0000, 0x0000,                // 18
    });
    const Cfg cfg = analyse(boot);
    CHECK(cfg.functions.size() == 2 && cfg.functions.contains(kB + 0x10));
    CHECK(cfg.functions.at(kB + 0x10).origin == Origin::pointer);
    CHECK(has_rejected(cfg, kB + 0x18, Origin::pointer));
    CHECK(cfg.at(kB + 0x04) == Kind::unknown && cfg.at(kB + 0x0E) == Kind::unknown);
    CHECK(cfg.at(kB + 0x16) == Kind::padding && cfg.at(kB + 0x1A) == Kind::padding);
    uint32_t unknown = 0;
    (void)coverage(cfg, boot, {}, false, unknown);
    CHECK(unknown == 12);

    // Bytes a branch skips that happen to decode are not a function: they
    // would run on into the code after them.
    const BootFile island = program({
        0xA002, 0x0009,  // 00  bra 0x08
        0xE000, 0xE000,  // 04  (data: decodes as mov #0,r0)
        0x000B, 0x0009,  // 08  rts
    });
    const Cfg ic = analyse(island);
    CHECK(ic.functions.size() == 1 && ic.at(kB + 4) == Kind::unknown && has_rejected(ic, kB + 4, Origin::orphan));

    // An address the image holds of a short entry that falls into a shared
    // tail (code a function branches to) is a function; the same bytes with
    // nothing pointing at them stay unknown (above).
    const uint32_t entry = kB + 6;
    const BootFile tail = program({
        0xA002, 0xE401,  // 00  bra 0x08 ; mov #1,r4
        0x0009,          // 04  nop
        0xE407,          // 06  mov #7,r4 (falls into 08)
        0x000B, 0x0009,  // 08  rts
        static_cast<uint16_t>(entry), static_cast<uint16_t>(entry >> 16),  // 0C  a pointer to 06
    });
    const Cfg tc = analyse(tail);
    CHECK(tc.functions.contains(entry) && tc.functions.at(entry).origin == Origin::pointer);

    // Unreferenced code after a function is kept, marked as an orphan.
    const BootFile orphan = program({0x000B, 0x0009, 0xE001, 0x000B, 0x0009});
    const Cfg oc = analyse(orphan);
    CHECK(oc.functions.size() == 2 && oc.functions.at(kB + 4).origin == Origin::orphan);

    // A guess that calls something that isn't code is not kept either.
    const BootFile caller = program({
        0x000B, 0x0009,  // 00  rts
        0xB002, 0x0009,  // 04  bsr 0x0C
        0x000B, 0x0009,  // 08  rts
        0x0000, 0x0000,  // 0C
    });
    const Cfg cc = analyse(caller);
    CHECK(cc.functions.size() == 1 && has_rejected(cc, kB + 4, Origin::orphan));
}

void test_cfg_rejects() {
    // A call to something with a branch in a delay slot: not kept, and reported.
    const BootFile boot = program({
        0xB002, 0x0009,  // 00  bsr 0x08
        0x000B, 0x0009,  // 04  rts
        0x000B, 0xA000,  // 08  rts ; bra (in the slot)
    });
    const Cfg cfg = analyse(boot);
    CHECK(cfg.functions.size() == 1 && has_rejected(cfg, kB + 8, Origin::call));
    CHECK(cfg.rejected.size() == 1);
    CHECK(suspects(cfg).contains("8C010008 (call target, not kept)") && suspects(cfg).contains("1 suspects"));

    // An entry point that isn't code leaves nothing, and says why.
    const Cfg none = analyse(program({0x0000, 0x0000}));
    CHECK(none.functions.empty() && has_rejected(none, kB, Origin::entry));
    // Running off the end of the image.
    const Cfg off = analyse(program({0xE001, 0xE002}));
    CHECK(off.functions.empty() && off.rejected.size() == 1 && off.rejected[0].reason.contains("outside the image"));
    // A branch landing in a delay slot is kept and reported.
    const Cfg slot = analyse(program({
        0x8901,  // 00  bt 0x06
        0x0009,  // 02  nop
        0x000B,  // 04  rts
        0xE001,  // 06   mov #1,r0   (the slot, and a branch target)
        0x000B,  // 08  rts
        0x0009,  // 0A   nop
    }));
    CHECK(slot.functions.size() == 1 && slot.functions.at(kB).insns.size() == 6);
    CHECK(slot.functions.at(kB).problems.size() == 1 && suspects(slot).contains("lands in a delay slot"));
}

void test_cfg_v03() {
    // Constants through arithmetic: r1 = 0x8C010000 + 0x14 + 0x04, then a call.
    const BootFile folded = program({
        0xD103,  // 00  mov.l 0x10,r1
        0x7114,  // 02  add #20,r1
        0x7104,  // 04  add #4,r1
        0x410B,  // 06  jsr @r1
        0x0009,  // 08   nop
        0x000B,  // 0A  rts
        0x0009,  // 0C   nop
        0x0009,  // 0E
        lo(kB), hi(kB),  // 10
        0x000B, 0x0009,  // 14  rts ; nop
        0x000B, 0x0009,  // 18  f: rts ; nop
    });
    const Cfg fc = analyse(folded);
    CHECK(fc.functions.at(kB).calls.size() == 1 && fc.functions.at(kB).calls.at(kB + 6) == kB + 0x18);

    // A float reached through mova is a literal, not unknown bytes.
    const BootFile floats = program({
        0xC701,  // 00  mova 0x08,r0
        0xF108,  // 02  fmov @r0,fr1
        0x000B,  // 04  rts
        0x0009,  // 06   nop
        0x0000, 0x3F80,  // 08  1.0f
    });
    const Cfg flc = analyse(floats);
    CHECK(flc.at(kB + 8) == Kind::literal && flc.at(kB + 10) == Kind::literal);
    const auto& lit = flc.functions.at(kB).literals;
    CHECK(lit.size() == 1 && lit[0] == std::pair(kB + 8, 4u));

    // A bra to another function's prologue is a tail call, and that function
    // is found even though nothing calls it.
    const BootFile tail = program({
        0xA001,  // 00  bra 0x06
        0x0009,  // 02   nop
        0x0009,  // 04
        0x2FE6,  // 06  g: mov.l r14,@-r15
        0x000B,  // 08  rts
        0x6EF6,  // 0A   mov.l @r15+,r14
    });
    const Cfg tc = analyse(tail);
    CHECK(tc.functions.size() == 2 && tc.functions.contains(kB + 6));
    CHECK(tc.functions.at(kB).tail_jumps.size() == 1 && tc.functions.at(kB).tail_jumps.at(kB) == kB + 6);
    CHECK(tc.functions.at(kB).insns.size() == 2 && tc.functions.at(kB + 6).origin == Origin::tail);

    // A branch to an entry found otherwise (here: a call) is a tail call too,
    // once the second pass knows the entry.
    const BootFile shared = program({
        0xB003,  // 00  bsr 0x0A
        0x0009,  // 02   nop
        0xA001,  // 04  bra 0x0A
        0x0009,  // 06   nop
        0x0009,  // 08
        0xE001,  // 0A  h: mov #1,r0
        0x000B,  // 0C  rts
        0x0009,  // 0E   nop
    });
    const Cfg sc = analyse(shared);
    CHECK(sc.functions.size() == 2 && sc.functions.at(kB).tail_jumps.size() == 1);
    CHECK(sc.functions.at(kB).insns.size() == 4);

    // A layout's data region holds no functions, even where its words decode.
    const BootFile withdata = program({
        0xD001,  // 00  mov.l 0x08,r0  (a pointer into the data region)
        0x000B,  // 02  rts
        0x0009,  // 04   nop
        0x0009,  // 06
        lo(kB + 0x0C), hi(kB + 0x0C),  // 08
        0xE001, 0x000B, 0x0009, 0x0009,  // 0C  decodes, but it's data
    });
    const Region regions[] = {{kB, kB + 0x0C, "code", Plan::lift}, {kB + 0x0C, kB + 0x14, "data", Plan::data}};
    const Cfg dc = analyse(withdata, regions);
    CHECK(dc.functions.size() == 1 && dc.at(kB + 0x0C) == Kind::data && dc.at(kB + 0x12) == Kind::data);
    uint32_t unknown = 1;
    const std::string report = coverage(dc, withdata, regions, false, unknown);
    CHECK(unknown == 0 && report.contains("0 unknown bytes in lifted regions"));
    const Cfg undeclared = analyse(withdata);  // without the layout it is code
    CHECK(undeclared.functions.size() == 2);

    // The supported build has a layout covering its whole image, in order.
    const auto usa = layout("ee68a326da964fec5892b7226311f2293fd3a0bbf5bac496ec82991964d49bbd");
    CHECK(!usa.empty() && usa.front().begin == kBootFileBase && usa.back().end == 0x8C0C6B9Cu);
    for (size_t i = 1; i < usa.size(); ++i) CHECK(usa[i].begin == usa[i - 1].end && usa[i].begin < usa[i].end);
    CHECK(layout("0000").empty());
}

void test_hwaccess() {
    CHECK(hw_region(0x8C123456).empty());
    CHECK(hw_region(0xAC000000) == "RAM (uncached)" && hw_region(0x0C000000) == "RAM (another view)");
    CHECK(hw_region(0xA05F8000) == "PowerVR2 registers" && hw_region(0x005F8124) == "PowerVR2 registers");
    CHECK(hw_region(0xA05F6C04) == "Maple registers" && hw_region(0xA05F7018) == "GD-ROM registers");
    CHECK(hw_region(0xA0702800) == "sound chip registers" && hw_region(0xA0800000) == "sound memory");
    CHECK(hw_region(0xA5000000) == "video memory (32-bit view)" && hw_region(0xA4000000) == "video memory (64-bit view)");
    CHECK(hw_region(0x10000000) == "tile accelerator: polygons" && hw_region(0xB0000000) == "tile accelerator: polygons");
    CHECK(hw_region(0xE0000000) == "store queues" && hw_region(0xFF00001C) == "CPU: cache and exception control (CCN)");
    CHECK(hw_region(0xFFD80004) == "CPU: timers (TMU)" && hw_region(0xFFD00004) == "CPU: interrupt controller (INTC)");
    CHECK(hw_region(0xA0000000) == "boot ROM");

    // mov.l pool,r1 ; mov.l r0,@(4,r1) ; mov.l @r2,r3 (r2 unknown) ; pref @r1
    const BootFile boot = program({
        0xD103,  // 00  mov.l 0x10,r1
        0x1101,  // 02  mov.l r0,@(4,r1)
        0x6322,  // 04  mov.l @r2,r3
        0x0183,  // 06  pref @r1
        0x6112,  // 08  mov.l @r1,r1
        0x000B,  // 0A  rts
        0x0009,  // 0C   nop
        0x0009,  // 0E
        0x8000, 0xA05F,  // 10  0xA05F8000
    });
    const Cfg cfg = analyse(boot);
    const auto found = hardware_accesses(boot, cfg, cfg.functions.at(kB));
    CHECK(found.size() == 3);
    if (found.size() == 3) {
        CHECK(found[0].pc == kB + 2 && found[0].ea == 0xA05F8004u && found[0].size == 4 && found[0].kind == HwAccess::store);
        CHECK(found[1].pc == kB + 6 && found[1].kind == HwAccess::cache && found[1].size == 32);
        CHECK(found[2].pc == kB + 8 && found[2].kind == HwAccess::load);
    }
    CHECK(sh4::access_size(sh4::decode(0x2120)) == 1 && sh4::access_size(sh4::decode(0x2121)) == 2);
    CHECK(sh4::access_size(sh4::decode(0x0009)) == 0 && sh4::access_size(sh4::decode(0xF108)) == 4);
}

void test_cfg_pr() {
    // A call made by loading PR by hand, and a return through the saved PR:
    //   sts pr,r2 ; mov.l g,r0 ; mov.l back,r1 ; lds r1,pr ; jmp @r0 ; nop
    //   back: jmp @r2 ; nop
    const BootFile boot = program({
        0x022A,  // 00  sts pr,r2
        0xD003,  // 02  mov.l 0x10,r0
        0xD103,  // 04  mov.l 0x14,r1
        0x412A,  // 06  lds r1,pr
        0x402B,  // 08  jmp @r0
        0x0009,  // 0A   nop
        0x422B,  // 0C  back: jmp @r2
        0x0009,  // 0E   nop
        lo(kB + 0x18), hi(kB + 0x18),  // 10
        lo(kB + 0x0C), hi(kB + 0x0C),  // 14
        0x000B, 0x0009,  // 18  g: rts ; nop
    });
    const Cfg cfg = analyse(boot);
    const Function& f = cfg.functions.at(kB);
    CHECK(f.call_returns.size() == 1 && f.call_returns.at(kB + 8) == kB + 0x0C);
    CHECK(f.calls.size() == 1 && f.calls.at(kB + 8) == kB + 0x18);
    CHECK(f.jmp_returns == std::vector<uint32_t>({kB + 0x0C}));
    CHECK(f.indirect_jumps.empty() && f.tail_jumps.empty());
    CHECK(successors(boot, f, kB + 8) == std::vector<uint32_t>({kB + 0x0A}));
    CHECK(successors(boot, f, kB + 0x0A) == std::vector<uint32_t>({kB + 0x0C}));
    CHECK(successors(boot, f, kB + 0x0E).empty());

    // The entry PR pushed, a call, the push popped back, then a return
    // through a copy (the engine's matrix code: 0x8C061AA0):
    //   sts.l pr,@-r15 ; bsr g ; nop ; lds.l @r15+,pr ; sts pr,r2 ; jmp @r2 ; nop
    // With the slot overwritten before the pop (mov.l r3,@r15), it is not.
    const auto popped = [](uint16_t overwrite) {
        return program({
            0x4F22,     // 00  sts.l pr,@-r15
            0xB005,     // 02  bsr 0x10
            0x0009,     // 04   nop
            overwrite,  // 06
            0x4F26,     // 08  lds.l @r15+,pr
            0x022A,     // 0A  sts pr,r2
            0x422B,     // 0C  jmp @r2
            0x0009,     // 0E   nop
            0x000B, 0x0009,  // 10  g: rts ; nop
        });
    };
    {
        const BootFile b = popped(0x0009);
        const Cfg cb = analyse(b);
        const Function& h = cb.functions.at(kB);
        CHECK(h.jmp_returns == std::vector<uint32_t>({kB + 0x0C}));
        CHECK(h.indirect_jumps.empty());
    }
    {
        const BootFile b = popped(0x2F32);  // mov.l r3,@r15
        const Cfg cb = analyse(b);
        const Function& h = cb.functions.at(kB);
        CHECK(h.jmp_returns.empty());
        CHECK(h.indirect_jumps == std::vector<uint32_t>({kB + 0x0C}));
    }

    // A call by bra with PR loaded by hand (0x8C061AB4), back into a pop of
    // the pushed entry PR and a return through a copy:
    //   sts.l pr,@-r15 ; mov.l back,r0 ; lds r0,pr ; bra g ; nop
    //   back: lds.l @r15+,pr ; sts pr,r2 ; jmp @r2 ; nop
    {
        const BootFile b = program({
            0x4F22,     // 00  sts.l pr,@-r15
            0xD004,     // 02  mov.l 0x14,r0
            0x402A,     // 04  lds r0,pr
            0xA009,     // 06  bra 0x1C
            0x0009,     // 08   nop
            0x4F26,     // 0A  back: lds.l @r15+,pr
            0x022A,     // 0C  sts pr,r2
            0x422B,     // 0E  jmp @r2
            0x0009,     // 10   nop
            0x0009,     // 12
            lo(kB + 0x0A), hi(kB + 0x0A),  // 14
            0x0009, 0x0009,  // 18
            0x000B, 0x0009,  // 1C  g: rts ; nop
        });
        const Cfg cb = analyse(b);
        const Function& h = cb.functions.at(kB);
        CHECK(h.calls.size() == 1 && h.calls.at(kB + 6) == kB + 0x1C);
        CHECK(h.call_returns.size() == 1 && h.call_returns.at(kB + 6) == kB + 0x0A);
        CHECK(h.tail_jumps.empty() && h.indirect_jumps.empty());
        CHECK(h.jmp_returns == std::vector<uint32_t>({kB + 0x0E}));
        CHECK(successors(b, h, kB + 8) == std::vector<uint32_t>({kB + 0x0A}));
    }

    // A scratch register survives a call to a callee that doesn't write it.
    const auto with_leaf = [](uint16_t slot) {
        return program({
            0xD203,  // 00  mov.l 0x10,r2
            0xB007,  // 02  bsr 0x14
            0x0009,  // 04   nop
            0x420B,  // 06  jsr @r2
            0x0009,  // 08   nop
            0x000B,  // 0A  rts
            0x0009,  // 0C   nop
            0x0009,  // 0E
            lo(kB + 0x18), hi(kB + 0x18),  // 10
            0x000B, slot,                  // 14  leaf: rts ; (slot)
            0xE001, 0x000B, 0x0009,        // 18  g: mov #1,r0 ; rts ; nop
        });
    };
    const BootFile keeps = with_leaf(0x0009);  // nop
    const Cfg kc = analyse(keeps);
    CHECK(kc.functions.at(kB).calls.contains(kB + 6) && kc.functions.at(kB).calls.at(kB + 6) == kB + 0x18);
    const BootFile clobbers = with_leaf(0xE200);  // mov #0,r2
    const Cfg cc = analyse(clobbers);
    CHECK(!cc.functions.at(kB).calls.contains(kB + 6));
    CHECK(cc.functions.at(kB).indirect_calls == std::vector<uint32_t>({kB + 6}));
}

void test_cfg_long_functions() {
    // A function too long for bra branches inside itself through a register:
    //   mov.l label,r1 ; jmp @r1
    // The label is reached by the function's other paths too, so the jmp is a
    // branch, not a tail call, and the label's address in the literal pool
    // (a pointer candidate until the function is known) is not a function.
    const BootFile boot = program({
        0x000B, 0x0009,                // 00  rts ; nop
        lo(kB + 0x08), hi(kB + 0x08),  // 04  .long g
        0x4F22,                        // 08  g: sts.l pr,@-r15
        0x2448,                        // 0A  tst r4,r4
        0x8902,                        // 0C  bt 0x14
        0xD103,                        // 0E  mov.l 0x1C,r1
        0x412B,                        // 10  jmp @r1
        0x0009,                        // 12   nop
        0xE001,                        // 14  mov #1,r0
        0x4F26,                        // 16  label: lds.l @r15+,pr
        0x000B, 0x0009,                // 18  rts ; nop
        lo(kB + 0x16), hi(kB + 0x16),  // 1C  .long label
    });
    const Cfg cfg = analyse(boot);
    CHECK(cfg.functions.size() == 2 && cfg.functions.contains(kB + 8) && !cfg.functions.contains(kB + 0x16));
    if (cfg.functions.contains(kB + 8)) {
        const Function& g = cfg.functions.at(kB + 8);
        CHECK(g.tail_jumps.empty() && g.indirect_jumps.empty());
        CHECK(g.computed_branches.size() == 1 && g.computed_branches.contains(kB + 0x10) &&
              g.computed_branches.at(kB + 0x10) == kB + 0x16);
        CHECK(std::ranges::binary_search(g.leaders, kB + 0x16));
        CHECK(successors(boot, g, kB + 0x12) == std::vector<uint32_t>({kB + 0x16}));
    }
    CHECK(cfg.at(kB + 0x1C) == Kind::literal && cfg.at(kB + 0x16) == Kind::code);

    // The same code where nothing jumps to the label, only a data word holds
    // its address: the function still isn't allowed to run on into a
    // candidate, so the guess at the label is tried, and then thrown out
    // because the function with the prologue contains it.
    const BootFile coincidence = program({
        0x000B, 0x0009,                // 00  rts ; nop
        lo(kB + 0x0E), hi(kB + 0x0E),  // 04  a data word that happens to hold 0x0E
        0x4F22,                        // 08  g: sts.l pr,@-r15 (nothing refers to it)
        0xE001,                        // 0A  mov #1,r0
        0xE102,                        // 0C  mov #2,r1
        0x4F26,                        // 0E  lds.l @r15+,pr
        0x000B, 0x0009,                // 10  rts ; nop
    });
    const Cfg cc = analyse(coincidence);
    CHECK(cc.functions.size() == 2 && cc.functions.contains(kB + 8) && !cc.functions.contains(kB + 0x0E));
    CHECK(cc.functions.contains(kB + 8) && cc.functions.at(kB + 8).origin == Origin::orphan);
    CHECK(has_rejected(cc, kB + 0x0E, Origin::pointer));
    CHECK(cc.at(kB + 0x0E) == Kind::code && cc.owner[(0x0E) / 2] == kB + 8);

    // A guess with a prologue of its own, or one something calls, stays.
    const BootFile called = program({
        0xB006, 0x0009,                // 00  bsr 0x10 ; nop
        0x000B, 0x0009,                // 04  rts ; nop
        0x4F22,                        // 08  g: sts.l pr,@-r15
        0xE001,                        // 0A  mov #1,r0
        0xE102,                        // 0C  mov #2,r1
        0x0009,                        // 0E  nop
        0x4F26,                        // 10  h: lds.l @r15+,pr (called from the entry)
        0x000B, 0x0009,                // 12  rts ; nop
    });
    const Cfg kc = analyse(called);
    CHECK(kc.functions.contains(kB + 0x10) && kc.functions.at(kB + 0x10).origin == Origin::call);
}

void test_fpmode() {
    using namespace fpscr;
    const auto sz = [](const KnownBits& b) { return b.known(kSZ) ? int((b.value & kSZ) != 0) : -1; };
    const auto pr = [](const KnownBits& b) { return b.known(kPR) ? int((b.value & kPR) != 0) : -1; };

    // fschg around a 64-bit move; arithmetic after, in the default mode.
    {
        const BootFile boot = program({0xF3FD, 0xF21C, 0xF3FD, 0xF210, 0x000B, 0x0009});
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& m = modes.at(kB);
        CHECK(m.fpu == 4 && m.unproven == 0 && m.problems.empty());
        CHECK(sz(m.at.at(kB + 2)) == 1 && sz(m.at.at(kB + 6)) == 0 && pr(m.at.at(kB + 6)) == 0);
        CHECK(m.at.at(kB + 6).known(kRM | kDN) && (m.at.at(kB + 6).value & (kRM | kDN)) == kDN);
        CHECK(m.exit && m.exit->known(kTracked) && (m.exit->value & kTracked) == kDefault);
    }
    // Read-modify-write: set PR, add in double precision, clear PR.
    {
        const BootFile boot = program({
            0x016A,  // 00  sts fpscr,r1
            0xD205,  // 02  mov.l 0x18,r2
            0x212B,  // 04  or r2,r1
            0x416A,  // 06  lds r1,fpscr
            0xF210,  // 08  fadd fr1,fr2  (double)
            0x016A,  // 0A  sts fpscr,r1
            0xD203,  // 0C  mov.l 0x1C,r2
            0x2129,  // 0E  and r2,r1
            0x416A,  // 10  lds r1,fpscr
            0x000B,  // 12  rts
            0x0009,  // 14   nop
            0x0009,  // 16
            lo(kPR), hi(kPR),    // 18
            lo(~kPR), hi(~kPR),  // 1C
        });
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& m = modes.at(kB);
        CHECK(pr(m.at.at(kB + 8)) == 1 && m.unproven == 0 && m.problems.empty());
        CHECK(m.exit && pr(*m.exit) == 0);
    }
    // Saved and restored through the stack: the restore brings the mode back.
    {
        const BootFile boot = program({
            0x4F62,  // 00  sts.l fpscr,@-r15
            0xF3FD,  // 02  fschg
            0x4F66,  // 04  lds.l @r15+,fpscr
            0xF21C,  // 06  fmov fr1,fr2  (32-bit again)
            0x000B, 0x0009,
        });
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& m = modes.at(kB);
        CHECK(sz(m.at.at(kB + 6)) == 0 && m.problems.empty());
    }
    // A callee is analysed in the mode its caller calls it in.
    {
        const BootFile boot = program({
            0xF3FD,  // 00  fschg
            0xB004,  // 02  bsr 0x0E
            0x0009,  // 04   nop
            0xF3FD,  // 06  fschg
            0x000B,  // 08  rts
            0x0009,  // 0A   nop
            0x0009,  // 0C
            0xF21C,  // 0E  callee: fmov fr1,fr2
            0x000B,  // 10  rts
            0x0009,  // 12   nop
        });
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& callee = modes.at(kB + 0x0E);
        CHECK(callee.contexts == 1 && sz(callee.entry) == 1 && sz(callee.at.at(kB + 0x0E)) == 1);
        CHECK(callee.problems.empty() && modes.at(kB).problems.empty());
        CHECK(modes.at(kB).exit && sz(*modes.at(kB).exit) == 0);
    }
    // A function called only from replaced code (the game's main, called by
    // Shinobi's start-up) is analysed in the default mode, and so are its callees.
    {
        const BootFile boot = program({
            0xB005,  // 00  replaced code: bsr 0x0E
            0x0009,  // 02   nop
            0x000B,  // 04  rts
            0x0009,  // 06   nop
            0x0009, 0x0009, 0x0009,
            0xF21C,  // 0E  "main": fmov fr1,fr2
            0x000B,  // 10  rts
            0x0009,  // 12   nop
        });
        const Region regions[] = {{kB, kB + 0x0E, "replaced", Plan::replace}, {kB + 0x0E, kB + 0x14, "lifted", Plan::lift}};
        const auto modes = fp_modes(boot, analyse(boot, regions), regions);
        CHECK(modes.contains(kB + 0x0E) && !modes.contains(kB));
        const FpModes& m = modes.at(kB + 0x0E);
        CHECK(m.fpu == 1 && m.unproven == 0 && sz(m.at.at(kB + 0x0E)) == 0);
    }
    // Paths that join in different modes: reported, and what follows is unproven.
    {
        const BootFile boot = program({0x8901, 0xF3FD, 0x0009, 0xF21C, 0x000B, 0x0009});
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& m = modes.at(kB);
        CHECK(m.joins == 1 && m.unproven == 1 && sz(m.at.at(kB + 6)) == -1);
        CHECK(std::ranges::any_of(m.problems, [](const std::string& p) { return p.contains("paths join"); }));
    }
    // An indirect call in another mode: reported.
    {
        const BootFile boot = program({0xF3FD, 0x410B, 0x0009, 0xF3FD, 0x000B, 0x0009});
        const auto modes = fp_modes(boot, analyse(boot));
        CHECK(std::ranges::any_of(modes.at(kB).problems, [](const std::string& p) { return p.contains("indirect call"); }));
    }
    // A bank switch (frchg) doesn't need to be known: no problem, and the bank is tracked.
    {
        const BootFile boot = program({0xFBFD, 0xF210, 0xFBFD, 0x000B, 0x0009});
        const auto modes = fp_modes(boot, analyse(boot));
        const FpModes& m = modes.at(kB);
        CHECK(m.unproven == 0 && m.problems.empty());
        CHECK(m.at.at(kB + 2).known(kFR) && (m.at.at(kB + 2).value & kFR));
    }
    // Known bits: compose and meet.
    {
        const KnownBits entry_sz{0, 0, kSZ, 0}, flipped{0, 0, kSZ, kSZ};
        const KnownBits on{kSZ, kSZ, 0, 0}, off{kSZ, 0, 0, 0};
        CHECK(compose(entry_sz, on) == on && compose(flipped, on) == off);
        CHECK(meet(on, on) == on && meet(on, off).mask == 0);
        CHECK(meet(entry_sz, entry_sz) == entry_sz && meet(entry_sz, flipped).smask == 0);
    }
}

// ---------------------------------------------------------------- GD-ROM

// A synthetic GD-ROM dump in a scratch folder: tracks 1 (data) and 2 (audio)
// in the single-density area; track 3 (data: bootstrap, file system), track 4
// (audio, with a pregap) and track 5 (data, with a pregap: a file) in the
// high-density area.
struct FakeDisc {
    std::filesystem::path dir;
    std::vector<std::vector<uint8_t>> track;  // raw bytes of each track file

    static void put_sector(std::vector<uint8_t>& t, size_t index, std::span<const uint8_t> data, uint8_t mode = 1) {
        if (t.size() < (index + 1) * 2352) t.resize((index + 1) * 2352, 0);
        uint8_t* s = t.data() + index * 2352;
        s[0] = 0;
        std::fill(s + 1, s + 11, uint8_t{0xFF});
        s[11] = 0;
        s[15] = mode;
        std::copy(data.begin(), data.end(), s + 16);
    }
    static void le32(std::vector<uint8_t>& d, size_t o, uint32_t v) {
        for (int i = 0; i < 4; ++i) d[o + i] = static_cast<uint8_t>(v >> (8 * i));
    }
    static void record(std::vector<uint8_t>& dir, std::string_view name, uint32_t sector, uint32_t size, bool is_dir) {
        const size_t len = (33 + name.size() + 1) & ~size_t{1};
        const size_t o = dir.size();
        dir.resize(o + len, 0);
        dir[o] = static_cast<uint8_t>(len);
        le32(dir, o + 2, sector);
        le32(dir, o + 10, size);
        dir[o + 25] = is_dir ? 2 : 0;
        dir[o + 32] = static_cast<uint8_t>(name.size());
        std::copy(name.begin(), name.end(), dir.begin() + static_cast<std::ptrdiff_t>(o + 33));
    }

    // Track 5's file starts 60 sectors after track 3 (20) and track 4 (40).
    static constexpr uint32_t kTrack5File = 45000 + 20 + 40, kTrack5 = kTrack5File + 10;

    FakeDisc() : track(5) {
        dir = std::filesystem::temp_directory_path() / "qilin_tests_gdrom";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::vector<uint8_t> blank(2048, 0);
        put_sector(track[0], 9, blank);
        track[1].assign(2352 * 12, 0x55);
        // Track 3: the bootstrap, the volume descriptor, two directories, a file.
        std::vector<uint8_t> ip(2048, ' ');
        const auto text = [&](size_t at, std::string_view s) { std::copy(s.begin(), s.end(), ip.begin() + static_cast<std::ptrdiff_t>(at)); };
        text(0, "SEGA SEGAKATANA");
        text(16, "SEGA ENTERPRISES");
        text(32, "0000 GD-ROM1/1");
        text(48, "JUE");
        text(64, "T-00000");
        text(74, "V1.000");
        text(80, "19990101");
        text(96, "1ST_READ.BIN");
        text(112, "QILIN TESTS");
        text(128, "A SYNTHETIC DISC");
        put_sector(track[2], 0, ip);
        std::vector<uint8_t> pvd(2048, 0);
        pvd[0] = 1;
        std::memcpy(pvd.data() + 1, "CD001", 5);
        pvd[128] = 0x00, pvd[129] = 0x08;
        std::vector<uint8_t> root_record;
        record(root_record, std::string_view("\0", 1), 45017, 2048, true);
        std::copy(root_record.begin(), root_record.end(), pvd.begin() + 156);
        put_sector(track[2], 16, pvd);
        std::vector<uint8_t> root, sub;
        record(root, std::string_view("\0", 1), 45017, 2048, true);
        record(root, "\1", 45017, 2048, true);
        record(root, "1ST_READ.BIN;1", 45019, 3000, false);
        record(root, "DATA", 45018, 2048, true);
        root.resize(2048, 0);
        record(sub, std::string_view("\0", 1), 45018, 2048, true);
        record(sub, "\1", 45017, 2048, true);
        record(sub, "BIG.DAT;1", kTrack5, 5000, false);
        record(sub, "EMPTY.DAT;1", kTrack5, 0, false);
        sub.resize(2048, 0);
        put_sector(track[2], 17, root);
        put_sector(track[2], 18, sub);
        std::vector<uint8_t> a(4096), b(6144);
        for (size_t i = 0; i < a.size(); ++i) a[i] = static_cast<uint8_t>(i * 3 + 1);
        for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(i * 5 + 2);
        put_sector(track[2], 19, std::span(a).subspan(0, 2048));  // the last sector of track 3
        track[2].resize(2352 * 20, 0);
        // 1ST_READ.BIN's second sector would be in track 4: audio.
        track[3].assign(2352 * 40, 0x33);
        for (size_t i = 0; i < 3; ++i) put_sector(track[4], 10 + i, std::span(b).subspan(i * 2048, 2048));
        track[4].resize(2352 * 14, 0);
        write_all();
        write_cue(kCue);
    }
    ~FakeDisc() { std::filesystem::remove_all(dir); }

    static constexpr std::string_view kCue =
        "REM SINGLE-DENSITY AREA\n"
        "FILE \"t1.bin\" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n"
        "FILE \"t2.bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:02\n"
        "REM HIGH-DENSITY AREA\n"
        "FILE \"t3.bin\" BINARY\n  TRACK 03 MODE1/2352\n    INDEX 01 00:00:00\n"
        "FILE \"t4.bin\" BINARY\n  TRACK 04 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:00:05\n"
        "FILE \"t5.bin\" BINARY\r\n  TRACK 05 MODE1/2352\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:00:10\r\n";

    void write_all() const {
        for (size_t i = 0; i < track.size(); ++i) {
            std::ofstream f(dir / std::format("t{}.bin", i + 1), std::ios::binary);
            f.write(reinterpret_cast<const char*>(track[i].data()), static_cast<std::streamsize>(track[i].size()));
        }
    }
    std::filesystem::path write_cue(std::string_view text, std::string_view name = "disc.cue") const {
        std::ofstream f(dir / name, std::ios::binary);
        f << text;
        return dir / name;
    }
    std::filesystem::path cue() const { return dir / "disc.cue"; }
};

void test_gdrom() {
    using namespace gdrom;
    FakeDisc fake;
    const auto disc = Disc::open(fake.cue());
    CHECK(disc.has_value());
    if (!disc) {
        std::println(stderr, "{}", disc.error());
        return;
    }
    const std::vector<Track>& t = disc->tracks();
    CHECK(t.size() == 5);
    CHECK(!t[0].audio && !t[0].high_density && t[0].start == 0 && t[0].sectors == 10);
    CHECK(t[1].audio && t[1].file_start == 10 && t[1].start == 12 && t[1].sectors == 12);
    CHECK(t[2].high_density && t[2].start == 45000 && t[2].sectors == 20);
    CHECK(t[3].audio && t[3].file_start == 45020 && t[3].start == 45025);
    CHECK(!t[4].audio && t[4].file_start == FakeDisc::kTrack5File && t[4].start == FakeDisc::kTrack5 && t[4].end() == 45074);

    const auto boot = disc->bootstrap();
    CHECK(boot.has_value());
    if (boot) {
        CHECK(boot->hardware == "SEGA SEGAKATANA" && boot->boot_file == "1ST_READ.BIN");
        CHECK(boot->product == "T-00000" && boot->version == "V1.000" && boot->date == "19990101");
        CHECK(boot->areas == "JUE" && boot->title == "A SYNTHETIC DISC" && boot->company == "QILIN TESTS");
    }
    const auto files = disc->files();
    CHECK(files.has_value());
    if (files) {
        CHECK(files->size() == 3);
        if (files->size() == 3) {
            CHECK((*files)[0].path == "1ST_READ.BIN" && (*files)[0].sector == 45019 && (*files)[0].size == 3000);
            CHECK((*files)[1].path == "DATA/BIG.DAT" && (*files)[1].size == 5000);
            CHECK((*files)[2].path == "DATA/EMPTY.DAT" && (*files)[2].size == 0);
            const auto big = disc->read_file((*files)[1]);
            CHECK(big.has_value() && big->size() == 5000);
            if (big)
                for (size_t i = 0; i < big->size(); i += 499) CHECK((*big)[i] == static_cast<uint8_t>(i * 5 + 2));
            const auto empty = disc->read_file((*files)[2]);
            CHECK(empty.has_value() && empty->empty());
            // The boot file's second sector lies in the audio track: an error, not garbage.
            const auto first = disc->read_file((*files)[0]);
            CHECK(!first.has_value() && first.error().contains("audio track 4"));
        }
    }
    CHECK(disc->read_sector(45000).has_value());
    CHECK(!disc->read_sector(45030).has_value());   // audio
    CHECK(!disc->read_sector(45074).has_value());   // past the end
    CHECK(!disc->read_sector(44999).has_value());   // between the areas
    CHECK(!disc->read_sector(0).has_value());       // track 1's first sector was never written: no sync
    CHECK(disc->read_sector(9).has_value());
    CHECK(!disc->read_sector(45001).has_value());   // no sync pattern
    CHECK(disc->read(45016, 3 * 2048).has_value()); // several sectors in one read
    // CD audio: 588 left/right pairs a sector, little-endian.
    {
        const auto pcm = disc->read_audio(45025, 2);
        CHECK(pcm.has_value() && pcm->size() == 2 * 1176 && (*pcm)[0] == 0x3333 && pcm->back() == 0x3333);
        CHECK(!disc->read_audio(45000, 1).has_value());  // a data track
        CHECK(!disc->read_audio(45059, 2).has_value());  // runs off track 4 into track 5's data
        const auto single = disc->read_audio(10, 12);    // track 2, the single-density area's
        CHECK(single.has_value() && single->size() == 12 * 1176 && (*single)[5] == 0x5555);
    }

    // A sector of another mode stops the read.
    FakeDisc::put_sector(fake.track[2], 1, std::vector<uint8_t>(2048, 0), 2);
    fake.write_all();
    CHECK(!disc->read_sector(45001).has_value() && disc->read_sector(45001).error().contains("mode 2"));

    // Malformed cue sheets and track files.
    const auto bad = [&](std::string_view cue) { return !Disc::open(fake.write_cue(cue, "bad.cue")).has_value(); };
    std::string cue(FakeDisc::kCue);
    const auto replaced = [&](std::string_view from, std::string_view to) {
        std::string s = cue;
        const size_t at = s.find(from);
        if (at == std::string::npos) throw std::logic_error("test cue has no such text");
        return s.replace(at, from.size(), to);
    };
    CHECK(!Disc::open(fake.dir / "missing.cue").has_value());
    CHECK(bad(replaced("TRACK 03 MODE1/2352", "TRACK 03 MODE2/2352")));
    CHECK(bad(replaced("TRACK 03", "TRACK 04")));
    CHECK(bad(replaced("REM HIGH-DENSITY AREA\n", "")));
    CHECK(bad(replaced("t4.bin", "missing.bin")));
    CHECK(bad(replaced("    INDEX 01 00:00:05\n", "")));
    CHECK(bad(replaced("INDEX 01 00:00:05", "INDEX 01 00:00:75")));
    CHECK(bad(replaced("INDEX 01 00:00:05", "INDEX 01 00:01:00")));  // beyond the file
    CHECK(bad(replaced("INDEX 01 00:00:05", "INDEX 02 00:00:05")));
    CHECK(bad(replaced("FILE \"t4.bin\" BINARY", "FILE \"t4.bin\" WAVE")));
    CHECK(bad(replaced("FILE \"t4.bin\" BINARY\n", "")));  // two tracks in one file
    CHECK(bad(replaced("REM SINGLE", "CATALOG 0 REM SINGLE")));
    CHECK(bad("REM HIGH-DENSITY AREA\n"));
    // A track file that isn't whole sectors.
    fake.track[3].push_back(0);
    fake.write_all();
    CHECK(!Disc::open(fake.cue()).has_value());

    CHECK(!parse_bootstrap(std::vector<uint8_t>(100)).has_value());
    CHECK(!parse_bootstrap(std::vector<uint8_t>(2048, ' ')).has_value());
}

}  // namespace

int main() {
    test_sha256();
    test_bootfile();
    test_decoder();
    test_gpr_written();
    test_cfg_calls();
    test_cfg_switch();
    test_cfg_address_table();
    test_cfg_known();
    test_cfg_join();
    test_cfg_guesses();
    test_cfg_rejects();
    test_cfg_v03();
    test_hwaccess();
    test_cfg_pr();
    test_cfg_long_functions();
    test_fpmode();
    test_gdrom();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
