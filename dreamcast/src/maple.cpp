// The Maple bus (maple.hpp).
//
// The command table, as the hardware reads it: for each frame, a control
// word (bits 0-7 the frame's data length in words, 8-10 the pattern, 16-17
// the port, bit 31 the last frame), the reply's address, then the frame: a
// header word (bits 0-7 the command, 8-15 the recipient, 16-23 the sender,
// 24-31 the data length in words) and its data. A reply has the same header
// (its command a reply code) and its data. A port with nothing on it answers
// 0xFFFFFFFF.
//
// The device information a controller gives is the documented format
// (Sega's "Maple Bus" specification, cited by title): the functions it has,
// a word per function saying which of its controls exist, the region, the
// connector's direction, a name, a licence string and its power draw. The
// controller's word lists the standard pad's controls: A, B, X, Y, Start,
// the d-pad, both triggers and the stick; its name is the standard pad's.
#include "maple.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <vector>

#include "clock.hpp"
#include "guest.hpp"
#include "pvr.hpp"
#include "save_history.hpp"
#include "qilin/host/paths.hpp"
#include "system.hpp"
#include "video.hpp"

namespace dreamcast::maple {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kMdstar = 0x005F6C04u, kMdtsel = 0x005F6C10u, kMden = 0x005F6C14u, kMdst = 0x005F6C18u;
constexpr unsigned kDmaEnd = 12;  // the system bus's normal event

// Commands and replies.
enum : uint32_t {
    kDeviceInfo = 1, kAllInfo = 2, kReset = 3, kShutdown = 4, kInfoReply = 5, kOk = 7, kDataReply = 8, kGetCondition = 9,
    kSetCondition = 14,
    kUnsupported = 0xFE, kUnknownCommand = 0xFD,
};
constexpr uint32_t kController = 0x01000000u;  // the function, as the words carry it (big-endian bit 0)

bool g_trace = std::getenv("QILIN_TRACE_MAPLE") != nullptr;

// Guest RAM from the bus's physical addresses.
uint32_t ram(uint32_t physical) {
    const uint32_t a = (physical & 0x1FFFFFFFu) | 0x80000000u;
    if (a < 0x8C000000u || a >= 0x8D000000u) throw rt::Stop{rt::Stop::Unsupported, 0, physical};
    return a;
}

uint32_t be(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; }

// Writes a reply: the header, then `n` data words.
void reply(uint32_t to, uint32_t command, uint32_t recipient, uint32_t sender, const uint32_t* data, uint32_t n) {
    rt::st32(to, command | recipient << 8 | sender << 16 | n << 24);
    for (uint32_t k = 0; k < n; ++k) rt::st32(to + 4 + k * 4, data[k]);
}

void device_info(uint32_t to, uint32_t recipient, uint32_t sender) {
    uint8_t info[112] = {};
    const uint32_t words[4] = {kController, be(0x000F06FEu), 0, 0};  // the controls: A B X Y Start, d-pad; triggers, stick
    std::memcpy(info, words, 16);
    info[16] = 0xFF;  // every region
    info[17] = 0;     // the connector faces the cable's end
    // The product name: games tell devices apart by it (Toy Racer compares it
    // with "Dreamcast Controller" and "Racing Controller", the names its own
    // boot file holds, and takes no input from a pad it doesn't recognise).
    const char name[] = "Dreamcast Controller";
    std::memset(info + 18, ' ', 30 + 60);
    std::memcpy(info + 18, name, sizeof name - 1);
    const uint16_t standby = 430, max = 500;  // in 0.1 mA
    std::memcpy(info + 108, &standby, 2);
    std::memcpy(info + 110, &max, 2);
    uint32_t data[28];
    std::memcpy(data, info, sizeof info);
    reply(to, kInfoReply, recipient, sender, data, 28);
}

// ---- the memory card (Sega's Visual Memory): a sub-unit with three
// functions, storage, its screen and its clock. The storage is 256 blocks
// of 512 bytes, kept as a file in the data folder (memory_card_a1.bin, an
// image any Visual Memory tool reads). Sega's Maple bus and Visual Memory
// specifications, cited by title, have the commands' formats and the file
// system's.
constexpr uint32_t kStorage = 0x02000000u, kScreen = 0x04000000u, kClock = 0x08000000u;
enum : uint32_t { kMediaInfo = 10, kBlockRead = 11, kBlockWrite = 12, kWriteDone = 13 };
constexpr uint32_t kBlocks = 256, kBlockBytes = 512, kWriteParts = 4;

std::vector<uint8_t> g_card;
std::filesystem::path g_card_file;

// A save is many block writes: the card as it was before the first of them
// (the first after two seconds without one) is kept in the saves' history
// (save_history.hpp), so a save cut short can be undone from the menu.
std::chrono::steady_clock::time_point g_last_write{};

void save_card(uint32_t at, uint32_t bytes) {
    const auto now = std::chrono::steady_clock::now();
    if (g_last_write == std::chrono::steady_clock::time_point{} || now - g_last_write > std::chrono::seconds(2)) {
        std::string error;
        if (!backup_save(g_card_file.parent_path(), g_card_file.filename().string(), error))
            rt::fatal("cannot keep a copy of %s before saving: %s", g_card_file.string().c_str(), error.c_str());
    }
    g_last_write = now;
    std::FILE* f = std::fopen(g_card_file.string().c_str(), "r+b");
    if (!f) f = std::fopen(g_card_file.string().c_str(), "w+b");
    if (!f) rt::fatal("cannot write %s", g_card_file.string().c_str());
    if (std::fseek(f, 0, SEEK_END) == 0 && std::ftell(f) != long(g_card.size())) {
        std::fseek(f, 0, SEEK_SET);
        std::fwrite(g_card.data(), 1, g_card.size(), f);
    } else {
        std::fseek(f, long(at), SEEK_SET);
        std::fwrite(g_card.data() + at, 1, bytes, f);
    }
    if (std::fclose(f) != 0) rt::fatal("cannot write %s", g_card_file.string().c_str());
}

// The file system's layout, which the media information reports too: the
// root block last (255), the FAT before it (254, one block), the directory
// below that (253 down to 241), 200 blocks for files from block 0 (and 31
// more above them that a Visual Memory game can use).
constexpr uint16_t kRoot = 255, kFat = 254, kFatBlocks = 1, kDirectory = 253, kDirectoryBlocks = 13, kUserBlocks = 200, kExtraBlocks = 31;

uint8_t bcd(int v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }

// A newly formatted card: the root block (sixteen 0x55 bytes, the date it
// was formatted in BCD, then the layout as 16-bit fields from 0x40, as in
// the media information), the FAT (each block's next, 0xFFFC free, 0xFFFA
// the last of its chain: the directory's blocks chained downwards, the FAT
// and the root alone) and an empty directory.
void format_card() {
    auto put16 = [](uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v), p[1] = static_cast<uint8_t>(v >> 8); };
    uint8_t* root = g_card.data() + kRoot * kBlockBytes;
    std::memset(root, 0x55, 16);
    const std::time_t now = std::time(nullptr);
    std::tm t{};
#if defined(_WIN32)
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    const int year = t.tm_year + 1900;
    const uint8_t when[8] = {bcd(year / 100), bcd(year % 100), bcd(t.tm_mon + 1), bcd(t.tm_mday), bcd(t.tm_hour), bcd(t.tm_min), bcd(t.tm_sec),
                             static_cast<uint8_t>((t.tm_wday + 6) % 7)};
    std::memcpy(root + 0x30, when, sizeof when);
    const uint16_t layout[12] = {kBlocks - 1, 0, kRoot, kFat, kFatBlocks, kDirectory, kDirectoryBlocks, 0, kUserBlocks, kExtraBlocks, 0, 0};
    for (unsigned k = 0; k < 12; ++k) put16(root + 0x40 + 2 * k, layout[k]);
    uint8_t* fat = g_card.data() + kFat * kBlockBytes;
    for (unsigned b = 0; b < kBlocks; ++b) put16(fat + 2 * b, 0xFFFCu);
    for (unsigned b = kDirectory; b > kDirectory - kDirectoryBlocks + 1; --b) put16(fat + 2 * b, static_cast<uint16_t>(b - 1));
    put16(fat + 2 * (kDirectory - kDirectoryBlocks + 1), 0xFFFAu);
    put16(fat + 2 * kFat, 0xFFFAu);
    put16(fat + 2 * kRoot, 0xFFFAu);
}

void load_card() {
    if (!g_card.empty()) return;
    g_card_file = qilin::host::user_folder() / "memory_card_a1.bin";
    g_card.assign(kBlocks * kBlockBytes, 0);
    if (std::FILE* f = std::fopen(g_card_file.string().c_str(), "rb")) {
        const size_t got = std::fread(g_card.data(), 1, g_card.size(), f);
        std::fclose(f);
        if (got != g_card.size()) rt::fatal("%s is not a %u-byte memory card", g_card_file.string().c_str(), kBlocks * kBlockBytes);
    } else {
        format_card();
        save_card(0, kBlocks * kBlockBytes);
    }
}

void memory_card(uint32_t header, uint32_t data_at, uint32_t to) {
    const uint32_t command = header & 0xFFu, recipient = (header >> 8) & 0xFFu, sender = (header >> 16) & 0xFFu, words = header >> 24;
    const uint32_t me = recipient;
    load_card();
    switch (command) {
    case kDeviceInfo: {
        uint8_t info[112] = {};
        // The functions, and a word for each from the highest: the clock's,
        // the screen's (48 x 32, one bit a pixel), the storage's (one
        // partition, 512-byte blocks, written in 4 parts, read in 1).
        const uint32_t ids[4] = {kStorage | kScreen | kClock, be(0x403F7E7Eu), be(0x00100500u), be(0x000F4100u)};
        std::memcpy(info, ids, 16);
        info[16] = 0xFF, info[17] = 0;
        const char name[] = "Visual Memory";
        std::memset(info + 18, ' ', 30 + 60);
        std::memcpy(info + 18, name, sizeof name - 1);
        const uint16_t standby = 124, max = 130;
        std::memcpy(info + 108, &standby, 2), std::memcpy(info + 110, &max, 2);
        uint32_t data[28];
        std::memcpy(data, info, sizeof info);
        reply(to, kInfoReply, sender, me, data, 28);
        return;
    }
    case kReset:
    case kShutdown: reply(to, kOk, sender, me, nullptr, 0); return;
    default: break;
    }
    const uint32_t function = words >= 1 ? rt::ld32(data_at) : 0;
    if (function != kStorage) throw rt::Stop{rt::Stop::Unsupported, 0, header};
    // The location: [0] the partition, [1] the part of the block, [2] the block, high byte first.
    const uint32_t location = words >= 2 ? rt::ld32(data_at + 4) : 0;
    const uint32_t partition = location & 0xFFu, part = (location >> 8) & 0xFFu, block = (location >> 24) | ((location >> 8) & 0xFF00u);
    if (partition != 0) throw rt::Stop{rt::Stop::Unsupported, 0, location};
    switch (command) {
    case kMediaInfo: {
        // In 16-bit fields: the last block, the partition, the system area
        // (the root block), the FAT's block and its size, the directory's
        // first block and its size, the icon, the save area's first block
        // and its size, the area reserved for a game.
        const uint16_t f[12] = {kBlocks - 1, 0, kRoot, kFat, kFatBlocks, kDirectory, kDirectoryBlocks, 0, kUserBlocks, kExtraBlocks, 0, 0};
        uint32_t data[7] = {kStorage};
        std::memcpy(data + 1, f, sizeof f);
        reply(to, kDataReply, sender, me, data, 7);
        return;
    }
    case kBlockRead: {
        if (block >= kBlocks || part != 0 || words != 2) throw rt::Stop{rt::Stop::Unsupported, 0, location};
        uint32_t data[2 + kBlockBytes / 4] = {kStorage, location};
        std::memcpy(data + 2, g_card.data() + block * kBlockBytes, kBlockBytes);
        reply(to, kDataReply, sender, me, data, 2 + kBlockBytes / 4);
        return;
    }
    case kBlockWrite: {
        constexpr uint32_t part_bytes = kBlockBytes / kWriteParts;
        if (block >= kBlocks || part >= kWriteParts || words != 2 + part_bytes / 4) throw rt::Stop{rt::Stop::Unsupported, 0, location};
        const uint32_t at = block * kBlockBytes + part * part_bytes;
        for (uint32_t k = 0; k < part_bytes; k += 4) {
            const uint32_t w = rt::ld32(data_at + 8 + k);
            std::memcpy(g_card.data() + at + k, &w, 4);
        }
        save_card(at, part_bytes);
        reply(to, kOk, sender, me, nullptr, 0);
        return;
    }
    case kWriteDone:
        if (block >= kBlocks || part != kWriteParts) throw rt::Stop{rt::Stop::Unsupported, 0, location};
        reply(to, kOk, sender, me, nullptr, 0);
        return;
    default: throw rt::Stop{rt::Stop::Unsupported, 0, header};
    }
}

// ---- the Jump Pack: a sub-unit in every controller's second slot (0x02)
// with one function, vibration. Its device information's word for it, read
// by Shinobi's vibration library (0x8C0C17C4-0x8C0C1C0C) as bytes, is the
// number of vibration sources and how many run at once: one each.
//
// What the game sends, read from that library and traced in a mission: a
// set condition (14) with one word per source, its bytes the source (bits
// 7-4 of the first, here 1), then the power (bits 6-4 one way, 2-0 the
// other, 0-7; bits 7 and 3 the library's flags), the frequency and the
// ramp. The engine (0x8C066E84) sends its vibration again every frame for
// as long as it should last and never sends a stop, so a pack here stops
// by itself when the commands stop: 100 ms after the last.
constexpr uint32_t kVibration = 0x00010000u;
constexpr uint64_t kVibrationHold = 100'000'000u;  // ns

struct Pack {
    std::atomic<uint8_t> power{0};  // 0-7
    std::atomic<uint64_t> until{0};  // guest ns
};
Pack g_packs[4];

void jump_pack(uint32_t port, uint32_t header, uint32_t data_at, uint32_t to) {
    const uint32_t command = header & 0xFFu, recipient = (header >> 8) & 0xFFu, sender = (header >> 16) & 0xFFu, words = header >> 24;
    const uint32_t me = recipient;
    switch (command) {
    case kDeviceInfo: {
        uint8_t info[112] = {};
        const uint32_t ids[4] = {kVibration, 0x00000101u, 0, 0};  // one source, one at a time
        std::memcpy(info, ids, 16);
        info[16] = 0xFF, info[17] = 0;
        const char name[] = "Puru Puru Pack";
        std::memset(info + 18, ' ', 30 + 60);
        std::memcpy(info + 18, name, sizeof name - 1);
        const uint16_t standby = 200, max = 1600;
        std::memcpy(info + 108, &standby, 2), std::memcpy(info + 110, &max, 2);
        uint32_t data[28];
        std::memcpy(data, info, sizeof info);
        reply(to, kInfoReply, sender, me, data, 28);
        return;
    }
    case kReset:
    case kShutdown:
        g_packs[port].power.store(0);
        reply(to, kOk, sender, me, nullptr, 0);
        return;
    case kSetCondition: {
        if (words != 2 || rt::ld32(data_at) != kVibration) throw rt::Stop{rt::Stop::Unsupported, 0, header};
        const uint32_t w = rt::ld32(data_at + 4);
        const uint32_t source = (w >> 4) & 0xFu, power = std::max((w >> 12) & 7u, (w >> 8) & 7u);
        if (source != 1) throw rt::Stop{rt::Stop::Unsupported, 0, w};
        if (g_trace) std::fprintf(stderr, "maple: port %u vibration %08X: power %u\n", port, w, power);
        g_packs[port].power.store(static_cast<uint8_t>(power));
        g_packs[port].until.store(guest_ns() + kVibrationHold);
        reply(to, kOk, sender, me, nullptr, 0);
        return;
    }
    default: throw rt::Stop{rt::Stop::Unsupported, 0, header};
    }
}

void frame(uint32_t port, uint32_t header, uint32_t data_at, uint32_t to) {
    const uint32_t command = header & 0xFFu, recipient = (header >> 8) & 0xFFu, sender = (header >> 16) & 0xFFu;
    const Controls in = video::controls(port);
    if (g_trace)
        std::fprintf(stderr, "maple: port %u command %u to %02X from %02X, %u words, reply at %08X, buttons %04X\n", port, command, recipient, sender,
                     header >> 24, to, in.buttons);
    // The main unit (0x20 | the port's bits) on a port with a pad, a memory
    // card in its first slot (sub-unit 0x01) on port A and a Jump Pack in
    // every pad's second (0x02).
    const bool card = port == 0 && in.connected;
    if (in.connected && card && (recipient & 0x3Fu) == 0x01u) {
        memory_card(header, data_at, to);
        return;
    }
    if (in.connected && (recipient & 0x3Fu) == 0x02u) {  // a Jump Pack in the second slot
        jump_pack(port, header, data_at, to);
        return;
    }
    if (!in.connected || (recipient & 0x3Fu) != 0x20u) {
        rt::st32(to, 0xFFFFFFFFu);
        return;
    }
    const uint32_t me = recipient | (card ? 0x01u : 0u) | 0x02u;  // the reply's sender: the main unit, with its sub-units' bits
    switch (command) {
    case kDeviceInfo: device_info(to, sender, me); return;
    case kGetCondition: {
        if (rt::ld32(data_at) != kController) {
            reply(to, kUnsupported, sender, me, nullptr, 0);
            return;
        }
        // Buttons are active low.
        const uint32_t data[3] = {kController, uint32_t(uint16_t(~in.buttons)) | uint32_t(in.rt) << 16 | uint32_t(in.lt) << 24,
                                  uint32_t(in.x) | uint32_t(in.y) << 8 | 128u << 16 | 128u << 24};
        reply(to, kDataReply, sender, me, data, 3);
        return;
    }
    case kReset:
    case kShutdown:
    case kSetCondition: reply(to, kOk, sender, me, nullptr, 0); return;  // a controller has nothing to set
    default: throw rt::Stop{rt::Stop::Unsupported, 0, header};
    }
}

void run() {
    if (!(pvr_register(kMden) & 1u)) return;
    uint32_t at = ram(pvr_register(kMdstar));
    for (int n = 0; n < 64; ++n) {
        const uint32_t control = rt::ld32(at);
        const uint32_t length = control & 0xFFu, pattern = (control >> 8) & 7u, port = (control >> 16) & 3u;
        if (pattern == 0) {
            const uint32_t to = ram(rt::ld32(at + 4)), header = rt::ld32(at + 8);
            if ((header >> 24) != length) throw rt::Stop{rt::Stop::Unsupported, 0, control};
            frame(port, header, at + 12, to);
            at += 12 + length * 4;
        } else if (pattern == 7) {  // nothing sent: a frame that only skips
            at += 4;
        } else {
            throw rt::Stop{rt::Stop::Unsupported, 0, control};
        }
        if (control >> 31) {
            set_pvr_register(kMdst, 0);
            raise_event(kDmaEnd);
            return;
        }
    }
    throw rt::Stop{rt::Stop::Unsupported, 0, pvr_register(kMdstar)};  // a table without its last frame
}

}  // namespace

void start() { run(); }

float vibration(unsigned port) {
    const Pack& p = g_packs[port & 3u];
    const uint8_t power = p.power.load();
    return power && guest_ns() < p.until.load() ? float(power) / 7.0f : 0.0f;
}

void vblank() {
    if (pvr_register(kMdtsel) & 1u) run();
}

}  // namespace dreamcast::maple
