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
};

// A request: the drive does each command as it is sent, so every request is
// complete when gdd first asks after it. `bytes` is what it transferred.
struct Request {
    uint32_t id = 0;
    uint32_t bytes = 0;
};
Request g_last;
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

// 0x8C0C1520, call 0: sends command r4 with the parameter block at r5 and
// returns the request's id, never 0 (0 is gdd's "not accepted").
void gd_send_command(rt::Context& c) {
    const uint32_t command = c.r[4];
    Request r{g_next_id++, 0};
    switch (command) {
    case kInit: break;
    case kPioRead:
    case kDmaRead:
        r.bytes = read_sectors(rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8));
        if (command == kDmaRead) g_dma_end_pending = true;
        break;
    case kVersion: {
        // gdFs reads the digits at 12, 14 and 15 of "GDC Version x.yz" into a
        // word nothing else reads.
        static constexpr char kText[] = "GDC Version 1.10";
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
    rt::st32(c.r[4], 1), rt::st32(c.r[4] + 4, 0x80);
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
