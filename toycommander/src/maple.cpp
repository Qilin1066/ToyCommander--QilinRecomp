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

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "guest.hpp"
#include "pvr.hpp"
#include "video.hpp"

namespace toycommander::maple {

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

void frame(uint32_t port, uint32_t header, uint32_t data_at, uint32_t to) {
    const uint32_t command = header & 0xFFu, recipient = (header >> 8) & 0xFFu, sender = (header >> 16) & 0xFFu;
    const Controls in = video::controls(port);
    if (g_trace)
        std::fprintf(stderr, "maple: port %u command %u to %02X from %02X, %u words, reply at %08X, buttons %04X\n", port, command, recipient, sender,
                     header >> 24, to, in.buttons);
    // Only the main unit (0x20 | the port's bits) is there, and only on a port with a pad.
    if (!in.connected || (recipient & 0x3Fu) != 0x20u) {
        rt::st32(to, 0xFFFFFFFFu);
        return;
    }
    const uint32_t me = recipient;  // the reply's sender: the main unit, no sub-units
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
        const uint32_t control = rt::ld32(at), to = ram(rt::ld32(at + 4));
        const uint32_t length = control & 0xFFu, pattern = (control >> 8) & 7u, port = (control >> 16) & 3u;
        if (pattern == 0) {
            const uint32_t header = rt::ld32(at + 8);
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

void vblank() {
    if (pvr_register(kMdtsel) & 1u) run();
}

}  // namespace toycommander::maple
