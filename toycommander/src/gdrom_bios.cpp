#include "gdrom_bios.hpp"

#include <cstdint>

#include "chain.hpp"
#include "gdrom.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

const qilin::gdrom::Disc* g_disc = nullptr;

// The commands (call 0's r4) gdd sends.
enum Command : uint32_t {
    kPioRead = 16,  // {frame address, sectors, buffer, 0}: 2048-byte sectors to the buffer
    kDmaRead = 17,  // the same, through the system bus's GD-ROM DMA, which ends with event 0xA40
    kInit = 24,     // the drive's initialisation: no parameters
    kVersion = 40,  // the drive's version string, to the address in the first parameter
    kPlay = 20,     // CD audio: {start, end, repeats, 0}, frame addresses
    kPlay2 = 21,    // CD audio: the same, in minutes:seconds:frames
    kPause = 22,    // CD audio: pause
    kRelease = 23,  // CD audio: go on from the pause (the engine's 0x8C049B4C, through gdFs's 0x8C0B250E: {0, 0})
    kSubcode = 34,  // {format, bytes, buffer, 0}: the sub-channel's data
    kToc = 19,      // {area (0: the single-density area, 1: the high-density one), buffer}: the area's table of contents
};

// A request: the drive does each command as it is sent, so every request is
// complete when gdd first asks after it. `bytes` is what it transferred.
struct Request {
    uint32_t id = 0;
    uint32_t bytes = 0;
};
Request g_last;

// CD audio: what the drive plays (the mixer will read the tracks from the
// disc reader); paused or stopped otherwise. The drive's state reads 3
// (playing) while it plays, 1 (paused) otherwise.
struct CdAudio {
    bool playing = false;
    uint32_t start = 0, end = 0, repeats = 0, at = 0;
} g_cd;
uint32_t g_next_id = 1;

// The GD-ROM DMA's end (the system bus's event 0xA40), raised by a DMA read
// and delivered at the next system call that can take it: on the console it
// comes a moment after the read is sent, once the caller has recorded what it
// is waiting for, never inside the call that sent it.
bool g_dma_end_pending = false;
void deliver_pending(rt::Context& c) {
    if (!g_dma_end_pending || ((c.sr >> 4) & 15u) >= 4) return;  // level 4 masked: later
    g_dma_end_pending = false;
    raise_bus_event(c, 0xA40u);
}

// Frame addresses count from the disc's start, 150 before the first data
// sector (the 2-second lead-in): the disc reader's sectors are the ISO's.
constexpr uint32_t kFrameOffset = 150;

uint32_t read_sectors(uint32_t frame, uint32_t count, uint32_t to) {
    if (!g_disc) rt::fatal("gdrom: no disc");
    if (frame < kFrameOffset || count == 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, frame};
    for (uint32_t k = 0; k < count; ++k) {
        const auto data = g_disc->read_sector(frame - kFrameOffset + k);
        if (!data) rt::fatal("gdrom: %s", data.error().c_str());
        const uint32_t at = to + k * 2048;
        for (uint32_t i = 0; i < 2048; i += 4)
            rt::st32(at + i, uint32_t((*data)[i]) | uint32_t((*data)[i + 1]) << 8 | uint32_t((*data)[i + 2]) << 16 | uint32_t((*data)[i + 3]) << 24);
    }
    return count * 2048;
}

// The table of contents of one area, as the drive gives it: 99 words for
// tracks 1-99 (control << 28 | 1 << 24 | the frame address of index 1; 4 is
// a data track's control, 0 an audio track's; 0xFFFFFFFF for a track not in
// the area), then the first and last tracks (control << 28 | 1 << 24 |
// number << 16) and the lead-out (the last track's control | the frame
// address after it).
void write_toc(uint32_t area, uint32_t to) {
    if (!g_disc || area > 1) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, area};
    uint32_t words[102];
    for (uint32_t& w : words) w = 0xFFFFFFFFu;
    const qilin::gdrom::Track* first = nullptr;
    const qilin::gdrom::Track* last = nullptr;
    const auto control = [](const qilin::gdrom::Track& t) { return (t.audio ? 0u : 4u) << 28 | 1u << 24; };
    for (const auto& t : g_disc->tracks()) {
        if (t.high_density != (area == 1) || t.number < 1 || t.number > 99) continue;
        words[t.number - 1] = control(t) | (t.start + kFrameOffset);
        if (!first) first = &t;
        last = &t;
    }
    if (!first) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, area};
    words[99] = control(*first) | first->number << 16;
    words[100] = control(*last) | last->number << 16;
    words[101] = control(*last) | (last->end() + kFrameOffset);
    for (uint32_t k = 0; k < 102; ++k) rt::st32(to + 4 * k, words[k]);
}

// 0x8C0C1520, call 0: sends command r4 with the parameter block at r5 and
// returns the request's id, never 0 (0 is gdd's "not accepted").
// The sub-channel's data in format 1, the Q channel's current position: 14
// bytes, as the drive's command set lays them out. Byte 1 is the audio
// status (0x11 playing, 0x12 paused, 0x15 nothing to report); bytes 2-3 the
// length, 14; byte 4 the track's control and address nibbles (as in the
// table of contents: 0x01 for an audio track), 5 its number, 6 the index;
// 7-9 the frame within the track and 11-13 the frame address on the disc,
// high byte first. The position is where the play was started: it moves
// once the mixer plays the track. Toy Racer's audio layer asks every frame
// (ToyRacer--QilinRecomp wrote this; nothing here has sent it yet). Other
// formats and lengths stop.
uint32_t write_subcode(uint32_t format, uint32_t bytes, uint32_t to) {
    if (!g_disc) rt::fatal("gdrom: no disc");
    if (format != 1 || bytes != 14) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, format << 16 | (bytes & 0xFFFFu)};
    uint8_t q[14] = {};
    q[1] = g_cd.playing ? 0x11 : g_cd.at != 0 ? 0x12 : 0x15;
    q[3] = 14;
    if (g_cd.at != 0) {
        const uint32_t sector = g_cd.at - kFrameOffset;
        for (const auto& t : g_disc->tracks()) {
            if (sector < t.start || sector >= t.end()) continue;
            const uint32_t in_track = sector - t.start;
            q[4] = t.audio ? 0x01 : 0x41, q[5] = static_cast<uint8_t>(t.number), q[6] = 1;
            q[7] = static_cast<uint8_t>(in_track >> 16), q[8] = static_cast<uint8_t>(in_track >> 8), q[9] = static_cast<uint8_t>(in_track);
        }
        q[11] = static_cast<uint8_t>(g_cd.at >> 16), q[12] = static_cast<uint8_t>(g_cd.at >> 8), q[13] = static_cast<uint8_t>(g_cd.at);
    }
    for (uint32_t k = 0; k < 14; ++k) rt::st8(to + k, q[k]);
    return 14;
}

void gd_send_command(rt::Context& c) {
    const uint32_t command = c.r[4];
    Request r{g_next_id++, 0};
    switch (command) {
    case kInit: break;
    case kPlay:
        g_cd = {true, rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8), rt::ld32(c.r[5])};
        break;
    case kToc: write_toc(rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4)); break;
    case kPlay2: throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, command};
    case kPause: g_cd.playing = false; break;
    case kRelease:
        // Only after a play, and with the parameter the game sends (0).
        if (g_cd.at == 0 || rt::ld32(c.r[5]) != 0) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, command};
        g_cd.playing = true;
        break;
    case kSubcode: r.bytes = write_subcode(rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8)); break;
    case kPioRead:
    case kDmaRead:
        r.bytes = read_sectors(rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8));
        if (command == kDmaRead) g_dma_end_pending = true;
        break;
    case kVersion: {
        // gdFs reads the digits at 12, 14 and 15 of "GDC Version x.yz" into
        // its handle's word 11 as x << 16 | y << 8 | z. Above 0x10001 its CD
        // audio calls (play by track 0x8C0B21A4, by frame 0x8C0B22B0, go on
        // 0x8C0B250E) take a second path: command 36, the drive's state,
        // after a play, and a new play rather than command 23 after a pause.
        // 1.00 keeps them on the first, through the commands this file
        // answers. Both are drives the library was written for.
        static constexpr char kText[] = "GDC Version 1.00";
        const uint32_t to = rt::ld32(c.r[5]);
        for (uint32_t k = 0; k + 1 < sizeof kText; ++k) rt::st8(to + k, static_cast<uint8_t>(kText[k]));
        break;
    }
    default: throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1520u, command};
    }
    if (g_next_id == 0) g_next_id = 1;
    g_last = r;
    c.r[0] = r.id;
}

// 0x8C0C1534, call 1: the state of request r4, with four words at r5: the
// error (sense key), its detail, the bytes transferred, and a fourth gdd
// copies with the bytes. Returns 2, complete: the drive did it when it was
// sent. Asking after a request that was never sent stops.
void gd_check_command(rt::Context& c) {
    deliver_pending(c);
    const uint32_t id = c.r[4], status = c.r[5];
    if (id == 0 || id != g_last.id) throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1534u, id};
    rt::st32(status, 0), rt::st32(status + 4, 0), rt::st32(status + 8, g_last.bytes), rt::st32(status + 12, 0);
    c.r[0] = 2;
}

// 0x8C0C1548, call 2: the boot ROM's turn to process commands, which gdd
// gives it while it waits. Commands are done as they are sent: nothing to do.
void gd_main_loop(rt::Context& c) { deliver_pending(c); }

// 0x8C0C155C, call 3: the drive's initialisation. gdd ignores the result.
// Nothing is queued after it.
void gd_init(rt::Context&) {}

// 0x8C0C1570, call 4: the drive's state into r4's two words: the drive
// (1, paused: a disc in, the drive idle) and the disc (0x80, a GD-ROM, which
// gdFs requires; 6, the lid open, is what it would wait on). Returns 0, the
// state read (gdd takes 1 as "busy, ask again").
void gd_check_drive(rt::Context& c) {
    rt::st32(c.r[4], g_cd.playing ? 3 : 1), rt::st32(c.r[4] + 4, 0x80);
    c.r[0] = 0;
}

// 0x8C0C1584, call 5: the GD-ROM DMA's end, which the system's handler for
// event 0xA40 reports to the boot ROM (gdd calls it from Shinobi's chain).
// The drive's transfers are done when sent: nothing to do.
void gd_dma_end(rt::Context& c) { c.r[0] = 0; }

constexpr rt::Provided kCalls[] = {
    {0x8C0C1520u, "gd_send_command", gd_send_command},
    {0x8C0C1534u, "gd_check_command", gd_check_command},
    {0x8C0C1548u, "gd_main_loop", gd_main_loop},
    {0x8C0C155Cu, "gd_init", gd_init},
    {0x8C0C1570u, "gd_check_drive", gd_check_drive},
    {0x8C0C1584u, "gd_dma_end", gd_dma_end},
};

}  // namespace

void insert_disc(const qilin::gdrom::Disc& disc) { g_disc = &disc; }

void provide_gdrom_bios() { rt::provide(kCalls); }

}  // namespace toycommander
