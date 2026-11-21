// Self-contained tests: SHA-256, the boot file loader, the SH-4 decoder and
// the GD-ROM reader on a synthetic disc. No game files required (runs in CI).
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "bootfile.hpp"
#include "decoder.hpp"
#include "gdrom.hpp"
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
    test_gdrom();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
