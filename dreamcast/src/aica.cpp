#include "aica.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "bus.hpp"
#include "clock.hpp"
#include "guest.hpp"
#include "pvr.hpp"
#include "sound.hpp"

namespace dreamcast {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kRamLo = 0x00800000u, kRamBytes = 2u << 20;
constexpr uint32_t kRegLo = 0x00700000u, kRegBytes = 0x10000u;
uint8_t g_ram[kRamBytes];
uint8_t g_regs[kRegBytes];

uint32_t rd32(uint32_t off) {
    uint32_t v;
    std::memcpy(&v, g_ram + off, 4);
    return v;
}
void wr32(uint32_t off, uint32_t v) { std::memcpy(g_ram + off, &v, 4); }

// ---- the port's stand-in for the sound driver (docs/runtime.md, "The audio
// manager"). AUDIO64.DRV's ARM code is never run; this does what its
// command handlers and its per-frame work do, read from the driver with a
// disassembler (nothing of it is copied): the same records in sound RAM, the
// same writes to the chip's channel registers, which the port's chip
// (sound.cpp) then plays.
//
// Two versions of the driver are known, and each game has its own: 1.80d.2
// (Toy Commander's) and 1.90h.2 (Toy Racer's). Their commands are the same
// but for a few (below, "1.90"), and 1.90 keeps its records lower in sound
// RAM. The stand-in knows which it stands for by the table of offsets the
// driver's image publishes at 0xE0-0xE8 (which the audio manager reads),
// and stops on any other.
//
// The driver's records (1.80's offsets; 1.90's in kDriver190):
//   0xB200  the command queue: 32 slots of 16 bytes; 0xB400 the flag that
//           says there are commands. A command is [0] its code, [2] the
//           port (a channel: port n plays on channel n); a slot is free
//           again when its bytes 0 and 1 are 0.
//   0xC800  a log of the commands taken: 16 bytes each, the command's first
//           12 then the word at 0xB410 (then cleared), in 2 KB; 0xB40C the
//           next entry's offset; the entry after the newest is all ones.
//   0x1AF0  a channel's registers as a close leaves them (in the driver's
//           image: full level, centred, no envelope); 0x1B38 + 0x48 n port
//           n's copy of its 18 channel registers, which the commands edit.
//   0x1468  a byte per port, 0xFF while it plays; 0x14F0 + 4 n port n's
//           position (the channel's CA, in samples); 0x15F0 + 4 n the one
//           before it.
//   0x14A8  64 bytes of messages to the SH-4 (0x9E, and position events);
//           0xE4 where the next goes. Each is followed by the sound chip's
//           interrupt.
//   0x16F0 + 4 n  port n's position event (0x92, 0xAD), pending while its bit
//           is set in 0x14E8 (ports 0-31) or 0x14EC (32-63). Each frame, a
//           pending port whose channel has passed the position since the
//           last frame posts its number as a message.
constexpr uint32_t kQueue = 0xB200u, kSlots = 32, kFlag = 0xB400u;
constexpr uint32_t kLog = 0xC800u, kLogIndex = 0xB40Cu, kLogExtra = 0xB410u, kLogMask = 0x7FFu;
constexpr uint32_t kShadowBytes = 0x48u, kRegisters = 18;
constexpr uint32_t kMessageNext = 0xE4u, kInterface = 0xE0u;

struct Driver {
    const char* version;
    uint32_t active, messages, positions;  // the interface table's last three words (0xE0, 0xE4, 0xE8)
    uint32_t templ, shadows, previous, events, pending;
    bool v190;  // the commands' differences: open closes first; level is the total level; the sends set the direct level; ...
};
constexpr Driver kDriver180{"1.80d.2", 0x1468u, 0x14A8u, 0x14F0u, 0x1AF0u, 0x1B38u, 0x15F0u, 0x16F0u, 0x14E8u, false};
constexpr Driver kDriver190{"1.90h.2", 0x0BACu, 0x0BECu, 0x0C34u, 0x1234u, 0x127Cu, 0x0D34u, 0x0E34u, 0x0C2Cu, true};
Driver g_driver{};  // the one installed, from its start

bool g_driver_started = false;
uint32_t g_next_slot = 0;
bool g_deferred_key_on = false;  // while several channels start together (0x9C)

// QILIN_TRACE_SOUND: each command the driver takes, as its 16 bytes.
const bool g_trace = std::getenv("QILIN_TRACE_SOUND") != nullptr;

uint32_t shadow(unsigned port, unsigned word) { return rd32(g_driver.shadows + kShadowBytes * port + 4 * word); }
void set_shadow(unsigned port, unsigned word, uint32_t v) { wr32(g_driver.shadows + kShadowBytes * port + 4 * word, v); }

// The chip's channel registers. Writing word 0 with KYONEX (bit 15) keys
// every channel on or off by its KYONB.
uint32_t hw(unsigned port, unsigned word) {
    uint32_t v;
    std::memcpy(&v, g_regs + 0x80u * port + 4 * word, 4);
    return v;
}
void set_hw(unsigned port, unsigned word, uint32_t v) {
    std::memcpy(g_regs + 0x80u * port + 4 * word, &v, 4);
    if (word == 0 && (v & 0x8000u)) sound::key_execute();
}

// A channel's registers from its copy (word 0 last, without KYONEX while
// several start together).
void load_channel(unsigned port) {
    for (unsigned w = 1; w < kRegisters; ++w) set_hw(port, w, shadow(port, w));
    uint32_t w0 = shadow(port, 0);
    if (g_deferred_key_on) w0 &= ~0x8000u;
    set_hw(port, 0, w0);
}

void mark_started(unsigned port) {
    wr32(g_driver.previous + 4 * port, 0xFFFFFFFFu);
    wr32(g_driver.positions + 4 * port, 0);
}

// Play (0x93, and the end of 0xAD): `loop` the loop flag, `points` the
// loop's start (low half) and end (high half) if not 0.
void play(unsigned port, bool loop, uint32_t points) {
    g_ram[g_driver.active + port] = 0xFF;
    if (points != 0) set_shadow(port, 3, points >> 16), set_shadow(port, 2, points & 0xFFFFu);
    const uint32_t w0 = (shadow(port, 0) & ~0x200u) | (loop ? 0x200u : 0u);
    set_shadow(port, 0, w0);
    set_hw(port, 0, w0 & ~0x4000u);  // key off first: playing again starts over
    load_channel(port);
    mark_started(port);
}

// Octave and FNS: `absolute`, or added to the copy's; to the copy too if `keep`.
void set_pitch(unsigned port, int32_t octave, uint32_t fns, bool absolute, bool keep) {
    const uint32_t base = absolute ? 0 : shadow(port, 6);
    fns = (fns & 0x3FFu) + (base & 0x3FFu);
    if (fns >= 0x400u) fns -= 0x400u, ++octave;
    const uint32_t v = ((static_cast<uint32_t>(octave) + (base >> 11)) & 15u) << 11 | (fns & 0x3FFu);
    if (keep) set_shadow(port, 6, v);
    set_hw(port, 6, v);
}

void set_level(unsigned port, uint32_t level) {
    const uint32_t v = (shadow(port, 9) & ~0xF00u) | (level & 15u) << 8;
    set_shadow(port, 9, v);
    set_hw(port, 9, v);
}
// The game's pan, 0-31 (16 the centre), as the chip's DIPAN: below 16 the
// other side's attenuation, 0 counting as 1.
void set_pan(unsigned port, uint32_t value) {
    uint32_t p = value & 0x1Fu;
    if (p < 0x10u) p = p == 0 ? 15u : (0u - p) & 15u;
    const uint32_t v = (shadow(port, 9) & 0xFFE0u) | p;
    set_shadow(port, 9, v);
    g_regs[0x80u * port + 0x24] = static_cast<uint8_t>(v);  // a byte write
}
void set_send(unsigned port, uint32_t level, uint32_t select) {
    const uint32_t v = (select & 15u) << 4 | (level & 15u);
    set_shadow(port, 8, v);
    set_hw(port, 8, v);
}
// 1.90's sends also set the direct level (DISDL, word 9's bits 11-8).
void set_direct_level(unsigned port, uint32_t level) {
    const uint32_t v = (shadow(port, 9) & ~0xF00u) | (level & 15u) << 8;
    set_shadow(port, 9, v);
    set_hw(port, 9, v);
}
// 1.90's level (0x96, 0xAD): the total level, an attenuation (TL, word 10's
// high byte), where 1.80's is the direct level.
void set_total_level(unsigned port, uint32_t level) {
    const uint32_t v = (shadow(port, 10) & 0xFFu) | (level & 0xFFu) << 8;
    set_shadow(port, 10, v);
    set_hw(port, 10, v);
}

// A message for the SH-4 (0x9E, a position event): a byte in the ring, then
// the sound chip's interrupt.
void post_message(uint8_t byte);

// A position event (0x92, 0xAD): `at` in samples, pending until passed.
void request_event(unsigned port, uint32_t at) {
    wr32(g_driver.events + 4 * port, at);
    const uint32_t mask = g_driver.pending + 4 * (port / 32);
    wr32(mask, rd32(mask) | 1u << (port % 32));
}
// 1.90's stops also drop a port's pending event.
void drop_event(unsigned port) {
    if (!g_driver.v190) return;
    const uint32_t mask = g_driver.pending + 4 * (port / 32);
    wr32(mask, rd32(mask) & ~(1u << (port % 32)));
}

// The CD inputs' level (0-127) and pan (0-127, 64 the centre) as EFSDL and EFPAN.
void set_cd_level(unsigned input, uint32_t value) { g_regs[0x2040u + 4 * input + 1] = static_cast<uint8_t>((value & 0x7Fu) >> 3); }
void set_cd_pan(unsigned input, uint32_t value) {
    int32_t p = static_cast<int32_t>(value >> 2) - 16;
    if (p < 0) p = (p ^ 0xFF) + 0x10;
    g_regs[0x2040u + 4 * input] = static_cast<uint8_t>(p & 0x1F);
}

// The sound chip's interrupt to the SH-4 (MCIPD's bit 5): the system bus's
// external event 1.
void interrupt_sh4() { raise_external_event(1); }

void post_message(uint8_t byte) {
    uint32_t at = rd32(kMessageNext);
    if (at < g_driver.messages || at >= g_driver.messages + 64) throw rt::Stop{rt::Stop::Unsupported, 0, at};
    g_ram[at] = byte;
    if (++at > g_driver.messages + 0x3F) at = g_driver.messages;
    wr32(kMessageNext, at);
    interrupt_sh4();
}

// CD audio's set-up (0x3E): the inputs panned apart; 1.80 at full level,
// 1.90 silent until a level (0x35).
void cd_set_up() {
    const uint32_t level = g_driver.v190 ? 0u : 0x1F00u, a = level | 0x0Fu, b = level | 0x1Fu;
    std::memcpy(g_regs + 0x2040u, &a, 4), std::memcpy(g_regs + 0x2044u, &b, 4);
}

// Each port of a 64-bit mask, [4] ports 32-63, [8] 0-31, from the highest down.
template <class F>
void each_port(const uint8_t* cmd, F f) {
    uint32_t high, low;
    std::memcpy(&high, cmd + 4, 4), std::memcpy(&low, cmd + 8, 4);
    for (int port = 63; port >= 0; --port)
        if ((port >= 32 ? high >> (port - 32) : low >> port) & 1u) f(static_cast<unsigned>(port));
}

[[noreturn]] void unknown_command(const uint8_t* cmd) {
    uint32_t w0;
    std::memcpy(&w0, cmd, 4);
    throw rt::Stop{rt::Stop::Unsupported, 0, w0};
}

void run_command(const uint8_t* cmd) {
    if (g_trace) {
        std::fprintf(stderr, "sound: %10.3f s", double(guest_ns()) / 1e9);
        for (unsigned k = 0; k < 16; ++k) std::fprintf(stderr, " %02X", cmd[k]);
        std::fprintf(stderr, "\n");
    }
    const uint8_t code = cmd[0];
    const unsigned port = cmd[2];
    const auto u32 = [&](unsigned at) {
        uint32_t x;
        std::memcpy(&x, cmd + at, 4);
        return x;
    };
    if (code >= 0x90u && code != 0x9Cu && code != 0x9Du && code != 0x9Eu && code != 0x9Fu && port >= 64) unknown_command(cmd);
    switch (code) {
    case 0x90: {  // open: [4] the address (the SH-4's), [8] bytes, [12] PCMS, [13] the pitch, high byte first
        if (g_driver.v190) {  // 1.90 closes the port first
            for (unsigned w = 0; w < kRegisters; ++w) set_shadow(port, w, rd32(g_driver.templ + 4 * w));
            wr32(g_driver.previous + 4 * port, 0xFFFFFFFFu);
        }
        const uint32_t address = u32(4), format = cmd[12];
        uint32_t samples = u32(8);
        if (format == 0) samples >>= 1;
        else if (format > 1) samples <<= 1;
        set_shadow(port, 0, shadow(port, 0) | ((address >> 16) & 0x7Fu) | format << 7);
        set_shadow(port, 1, address & 0xFFFFu);
        set_shadow(port, 2, 0);
        set_shadow(port, 3, samples - 1);
        set_shadow(port, 6, static_cast<uint32_t>(cmd[13]) << 8 | cmd[14]);
        break;
    }
    case 0x91:  // close: the registers as the driver's image has them
        for (unsigned w = 0; w < kRegisters; ++w) set_shadow(port, w, rd32(g_driver.templ + 4 * w));
        break;
    case 0x93: play(port, u32(8) & 1u, u32(12)); break;  // [8] loop, [12] the loop's points
    case 0x92: request_event(port, u32(4)); break;  // [4] a position, in samples
    case 0x94: set_hw(port, 0, shadow(port, 0) & ~0x4200u), drop_event(port); break;  // stop
    case 0x95:  // sends: [3] level, [4] select; 1.90: [5] the direct level
        set_send(port, cmd[3], cmd[4]);
        if (g_driver.v190) set_direct_level(port, cmd[5]);
        break;
    case 0x96: g_driver.v190 ? set_total_level(port, cmd[3]) : set_level(port, cmd[3]); break;
    case 0x97: set_pan(port, cmd[3]); break;
    case 0x98: set_pitch(port, static_cast<int32_t>(u32(8)), u32(4), false, false); break;  // relative, for now
    case 0xAC: set_pitch(port, static_cast<int32_t>(u32(8)), u32(4), true, true); break;    // [4] FNS, [8] octave
    case 0x9C:  // play several: [2] loop; key on together at the end
        g_deferred_key_on = true;
        {
            int last = -1;
            each_port(cmd, [&](unsigned p) {
                g_ram[g_driver.active + p] = 0xFF;
                set_shadow(p, 0, (shadow(p, 0) & ~0x200u) | (cmd[2] & 1u) << 9);
                load_channel(p);
                mark_started(p);
                last = static_cast<int>(p);
            });
            g_deferred_key_on = false;
            if (last >= 0) set_hw(static_cast<unsigned>(last), 0, shadow(static_cast<unsigned>(last), 0));
        }
        break;
    case 0x9D: each_port(cmd, [&](unsigned p) { set_hw(p, 0, shadow(p, 0) & ~0x4200u), drop_event(p); }); break;  // stop several
    case 0x9E: post_message(cmd[2]); break;  // a message for the SH-4: [2] (1.90 frees the slot first: nothing here can see that)
    case 0x9F: interrupt_sh4(); break;
    case 0xAD: {  // play with parameters: [3] level, [4] pan, [5] sends, [6] pitch, [8] an event, [12] the loop's points
        // 1.90: [10] the direct level, and only [8]'s low half is the event.
        g_driver.v190 ? set_total_level(port, cmd[3]) : set_level(port, cmd[3]);
        set_pan(port, cmd[4]);
        set_send(port, cmd[5] & 15u, cmd[5] >> 4);
        if (g_driver.v190) set_direct_level(port, cmd[10]);
        if (const uint32_t event = g_driver.v190 ? u32(8) & 0xFFFFu : u32(8); event != 0) request_event(port, event);
        play(port, u32(12) != 0, u32(12));
        const uint32_t pitch = static_cast<uint32_t>(cmd[6]) << 8 | cmd[7];
        if (pitch != 0) {
            int32_t octave = cmd[6] >> 3;
            if (octave & 8) octave |= ~7;
            set_pitch(port, octave, pitch, (pitch & 0x400u) != 0, false);
        }
        break;
    }
    case 0x35: set_cd_level(0, cmd[2]), set_cd_level(1, cmd[3]); break;  // CD audio's level, left and right
    case 0x36: set_cd_pan(0, cmd[2]), set_cd_pan(1, cmd[3]); break;      // and its pan
    case 0x3E: cd_set_up(); break;
    default: unknown_command(cmd);
    }
}

void log_command(const uint8_t* cmd) {
    const uint32_t index = rd32(kLogIndex) & kLogMask;
    std::memcpy(g_ram + kLog + index, cmd, 12);
    wr32(kLog + index + 12, rd32(kLogExtra));
    wr32(kLogExtra, 0);
    const uint32_t next = (index + 16) & kLogMask;
    wr32(kLogIndex, next);
    for (unsigned k = 0; k < 16; k += 4) wr32(kLog + next + k, 0xFFFFFFFFu);
}

void take_commands() {
    wr32(kFlag, 0);
    for (uint32_t n = 0; n < kSlots; ++n) {
        const uint32_t at = kQueue + 16u * g_next_slot;
        if (g_ram[at] == 0) break;
        uint8_t cmd[16];
        std::memcpy(cmd, g_ram + at, 16);
        log_command(cmd);
        run_command(cmd);
        g_ram[at] = 0, g_ram[at + 1] = 0;
        g_next_slot = (g_next_slot + 1) % kSlots;
    }
}

// Whether a port's channel passed its event's position between the last
// frame (`before`) and this one (`now`), allowing for a loop back. The two
// versions compare differently (signed, as the driver does).
bool passed(int32_t event, int32_t before, int32_t now) {
    if (g_driver.v190) {
        if (before == -1) return false;  // started since: the next frame
        return before >= now ? event < now || event > before : event > before && event <= now;
    }
    if (now >= event) return before <= event || now < before;
    return before < event && now < before;
}

// The driver's per-frame work: each playing port's position, and a port
// whose channel played to its end without a loop is keyed off and no longer
// playing; then the pending position events, ports 31 down to 0, then 63
// down to 32. The channels are rendered up to now once, and every port read
// at that moment (a read of the clock and a render a port was a cost on
// every read of the driver's records).
void driver_frame() {
    if (!g_driver_started) return;
    sound::sync();
    for (unsigned port = 0; port < 64; ++port) {
        if (g_ram[g_driver.active + port] != 0xFF) continue;
        const uint32_t at = sound::position(port);
        wr32(g_driver.previous + 4 * port, rd32(g_driver.positions + 4 * port));
        wr32(g_driver.positions + 4 * port, at);
        if (sound::finished(port) && !(hw(port, 0) & 0x200u)) {
            set_hw(port, 0, hw(port, 0) & 0x87FFu);
            g_ram[g_driver.active + port] = 0;
        }
    }
    for (unsigned group = 0; group < 2; ++group) {
        const uint32_t mask_at = g_driver.pending + 4 * group;
        uint32_t mask = rd32(mask_at);
        for (int bit = 31; bit >= 0; --bit) {
            if (!(mask >> bit & 1u)) continue;
            const unsigned port = 32 * group + static_cast<unsigned>(bit);
            const auto word = [&](uint32_t base) { return static_cast<int32_t>(rd32(base + 4 * port)); };
            if (!passed(word(g_driver.events), word(g_driver.previous), word(g_driver.positions))) continue;
            post_message(static_cast<uint8_t>(port));
            mask &= ~(1u << bit);
        }
        wr32(mask_at, mask);
    }
}

uint32_t ram_read(uint32_t a, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    auto held = sound::lock();
    if (g_driver_started && off >= g_driver.active && off < g_driver.previous + 256u) driver_frame();
    uint32_t v = 0;
    std::memcpy(&v, g_ram + off, n);
    return v;
}
void ram_write(uint32_t a, uint32_t v, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    auto held = sound::lock();
    std::memcpy(g_ram + off, &v, n);
    if (off == kFlag && v != 0 && g_driver_started) take_commands();
}
void ram_block(uint32_t a, const uint8_t* data, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    auto held = sound::lock();
    std::memcpy(g_ram + off, data, n);
}

// The registers are 32-bit words on the G2 bus, of which the chip uses the
// low 16 bits; accesses are 4 bytes wide.
uint32_t reg_read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    auto held = sound::lock();
    uint32_t v;
    std::memcpy(&v, g_regs + (a - kRegLo), 4);
    return v;
}

// The ARM's reset (0x00702C00, bit 0: held in reset while set). Releasing
// it starts the driver the audio manager has copied in; the stand-in knows
// which by its table of offsets, and does what the driver's start-up does
// that matters here: every port's copy of its registers from the template,
// the master volume at full and the CD inputs off (1.90 then does its CD
// set-up), and where the sound RAM it doesn't use begins (0x30040) at 0xB408
// and at 0xEC, which the audio manager's install waits on (docs/runtime.md,
// the audio manager).
constexpr uint32_t kArmReset = 0x00702C00u;
void start_driver() {
    const uint32_t table[3] = {rd32(kInterface), rd32(kInterface + 4), rd32(kInterface + 8)};
    if (table[0] == kDriver180.active && table[1] == kDriver180.messages && table[2] == kDriver180.positions) g_driver = kDriver180;
    else if (table[0] == kDriver190.active && table[1] == kDriver190.messages && table[2] == kDriver190.positions) g_driver = kDriver190;
    else throw rt::Stop{rt::Stop::Unsupported, 0, table[0]};  // a sound driver the stand-in doesn't know
    if (std::getenv("QILIN_TRACE_SOUND")) std::fprintf(stderr, "sound: the driver is version %s\n", g_driver.version);
    g_driver_started = true;
    for (unsigned port = 0; port < 64; ++port)
        for (unsigned w = 0; w < kRegisters; ++w) set_shadow(port, w, rd32(g_driver.templ + 4 * w));
    const uint32_t full = 0xFu, zero = 0;
    std::memcpy(g_regs + 0x2800u, &full, 4);
    std::memcpy(g_regs + 0x2040u, &zero, 4), std::memcpy(g_regs + 0x2044u, &zero, 4);
    if (g_driver.v190) cd_set_up();
    wr32(kMessageNext, g_driver.messages);
    const uint32_t free_from = 0x30040u;
    wr32(0xB408u, free_from);
    wr32(0xECu, free_from);
}

void reg_write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    auto held = sound::lock();
    const uint32_t off = a - kRegLo;
    uint32_t old;
    std::memcpy(&old, g_regs + off, 4);
    std::memcpy(g_regs + off, &v, 4);
    if (off < 0x2000u && (off & 0x7Fu) == 0 && (v & 0x8000u)) sound::key_execute();  // the SH-4 keying channels itself
    if (a == kArmReset && (old & 1u) && !(v & 1u)) start_driver();
}

}  // namespace

void map_aica(const qilin::gdrom::Disc& disc) {
    sound::attach(g_regs, g_ram, disc);
    sound::set_tick(driver_frame);
    rt::map({kRamLo, kRamLo + kRamBytes, "sound RAM", ram_read, ram_write, ram_block});
    rt::map({kRegLo, kRegLo + kRegBytes, "sound chip registers", reg_read, reg_write});
}

uint8_t* sound_ram() { return g_ram; }

}  // namespace dreamcast
