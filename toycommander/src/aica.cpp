#include "aica.hpp"

#include <cmath>
#include <cstring>

#include "bus.hpp"
#include "clock.hpp"
#include "guest.hpp"

namespace toycommander {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kRamLo = 0x00800000u, kRamBytes = 2u << 20;
constexpr uint32_t kRegLo = 0x00700000u, kRegBytes = 0x10000u;
uint8_t g_ram[kRamBytes];
uint8_t g_regs[kRegBytes];

// ---- the port's stand-in for the sound driver (docs/runtime.md, "The audio
// manager"): what the ARM driver does that the audio manager can see. The
// audio manager queues 16-byte commands in 32 slots at 0xB200 and sets the
// command flag at 0xB400; the driver takes them in order, clearing each
// slot's first word, then the flag. A command is [0] its code, [1] 0xFF,
// [2] the port (voice); the rest depends on the code. Each port's read
// position, in samples, is a word at 0x14F0 + 4 * port, which
// acDigiGetCurrentReadPosition reads. The voices aren't mixed yet: their
// positions advance with guest time at their sample rates.
constexpr uint32_t kQueue = 0xB200u, kSlots = 32, kFlag = 0xB400u, kPositions = 0x14F0u;

struct Voice {
    bool open = false, playing = false;
    uint32_t address = 0, bytes = 0, type = 0, pitch = 0;  // as the open command gives them
    uint64_t started = 0;                                 // guest ns at play
};
Voice g_voice[64];
bool g_driver_started = false;
uint32_t g_next_slot = 0;

uint32_t rd32(uint32_t off) {
    uint32_t v;
    std::memcpy(&v, g_ram + off, 4);
    return v;
}
void wr32(uint32_t off, uint32_t v) { std::memcpy(g_ram + off, &v, 4); }

// The AICA's pitch word: octave (signed, 4 bits) << 11 | FNS (10 bits);
// the rate is 44100 * 2^octave * (1 + FNS / 1024).
double rate_of(uint32_t pitch) {
    int oct = static_cast<int>((pitch >> 11) & 15u);
    if (oct >= 8) oct -= 16;
    return 44100.0 * std::ldexp(1.0 + double(pitch & 0x3FFu) / 1024.0, oct);
}
uint32_t samples_of(const Voice& v) {
    switch (v.type) {
    case 0: return v.bytes / 2;  // 16-bit PCM
    case 1: return v.bytes;      // 8-bit PCM
    default: return v.bytes * 2; // ADPCM, 4 bits a sample
    }
}
uint32_t position(const Voice& v) {
    if (!v.playing) return 0;
    const uint32_t n = samples_of(v);
    const uint64_t done = static_cast<uint64_t>(double(guest_ns() - v.started) * rate_of(v.pitch) / 1e9);
    return n ? static_cast<uint32_t>(done % n) : 0;
}

[[noreturn]] void unknown_command(const uint8_t* cmd) {
    uint32_t w0;
    std::memcpy(&w0, cmd, 4);
    throw rt::Stop{rt::Stop::Unsupported, 0, w0};
}

void run_command(const uint8_t* cmd) {
    const uint8_t code = cmd[0];
    const unsigned port = cmd[2];
    const bool per_port = code == 0x90u || code == 0x91u || code == 0x93u || code == 0x94u || code == 0x96u || code == 0x97u ||
                          code == 0xACu || code == 0xADu;
    if (per_port && port >= 64) unknown_command(cmd);
    Voice& v = g_voice[port & 63u];
    const auto u32 = [&](unsigned at) {
        uint32_t x;
        std::memcpy(&x, cmd + at, 4);
        return x;
    };
    switch (code) {
    case 0x90:  // open: [4] address, [8] bytes, [12] type, [13] pitch (16 bits)
        v = Voice{true, false, u32(4), u32(8), cmd[12], static_cast<uint32_t>(cmd[13] | cmd[14] << 8), 0};
        break;
    case 0x91: v = Voice{}; break;  // close
    case 0x93:                       // play
        if (!v.open) unknown_command(cmd);
        v.playing = true, v.started = guest_ns();
        break;
    case 0x94: v.playing = false; break;  // stop
    case 0xAC:  // the sample rate (0x8C068C58): [4] the octave, [8] the FNS, a word each
        v.pitch = (u32(4) & 15u) << 11 | (u32(8) & 0x3FFu);
        break;
    case 0xAD:  // play with parameters (0x8C0686B0): [6] the pitch, high byte
                // first, with bit 10 a flag of the driver's (the AICA's FNS
                // is bits 0-9); [3]-[5], [8]-[15] the voice's other
                // parameters, for the mixer when there is one
        if (!v.open) unknown_command(cmd);
        v.pitch = static_cast<uint32_t>(cmd[6] << 8 | cmd[7]) & 0x7BFFu;
        v.playing = true, v.started = guest_ns();
        break;
    case 0x9C:  // play several: [2] the loop flag, [4] ports 32-63, [8] ports 0-31, a bit each
        for (unsigned k = 0; k < 64; ++k) {
            const bool chosen = k < 32 ? (u32(8) & (1u << k)) : (u32(4) & (1u << (k - 32)));
            if (!chosen) continue;
            if (!g_voice[k].open) unknown_command(cmd);
            g_voice[k].playing = true, g_voice[k].started = guest_ns();
        }
        break;
    case 0x9D:                            // stop several: [4] ports 32-63, [8] ports 0-31, a bit each
        for (unsigned k = 0; k < 32; ++k) {
            if (u32(8) & (1u << k)) g_voice[k].playing = false;
            if (u32(4) & (1u << k)) g_voice[32 + k].playing = false;
        }
        break;
    case 0x96:                            // volume
    case 0x97:                            // pan
    case 0x35:                            // CD audio's volume
    case 0x36:                            // CD audio's pan
        break;                            // kept by the mixer when there is one
    case 0x9E: break;                     // an interrupt to the ARM: nothing runs there
    case 0x3E: break;                     // CD audio's initialisation (amInit's last command)
    default: unknown_command(cmd);
    }
}

void take_commands() {
    for (uint32_t n = 0; n < kSlots; ++n) {
        const uint32_t at = kQueue + 16u * g_next_slot;
        if ((rd32(at) & 0xFFFFu) == 0) break;
        uint8_t cmd[16];
        std::memcpy(cmd, g_ram + at, 16);
        run_command(cmd);
        wr32(at, 0);
        g_next_slot = (g_next_slot + 1) % kSlots;
    }
    wr32(kFlag, 0);
}

uint32_t ram_read(uint32_t a, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off >= kPositions && off < kPositions + 256u && n == 4) return position(g_voice[(off - kPositions) / 4]);
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t v = 0;
    std::memcpy(&v, g_ram + off, n);
    return v;
}
void ram_write(uint32_t a, uint32_t v, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    std::memcpy(g_ram + off, &v, n);
    if (off == kFlag && v != 0 && g_driver_started) take_commands();
}
void ram_block(uint32_t a, const uint8_t* data, unsigned n) {
    const uint32_t off = a - kRamLo;
    if (off + n > kRamBytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    std::memcpy(g_ram + off, data, n);
}

// The registers are 32-bit words on the G2 bus, of which the chip uses the
// low 16 bits; accesses are 4 bytes wide.
uint32_t reg_read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t v;
    std::memcpy(&v, g_regs + (a - kRegLo), 4);
    return v;
}
// The ARM's reset (0x00702C00, bit 0: held in reset while set). Releasing
// it starts the driver; the port's stand-in does what the driver's start-up
// does that the audio manager can see: it writes where the sound RAM it
// doesn't use begins (0x30040) to 0xB408 and to 0xEC, which the audio
// manager's install waits on (docs/runtime.md, the audio manager).
constexpr uint32_t kArmReset = 0x00702C00u;
void start_driver() {
    g_driver_started = true;
    const uint32_t free_from = 0x30040u;
    std::memcpy(g_ram + 0xB408u, &free_from, 4);
    std::memcpy(g_ram + 0xECu, &free_from, 4);
}

void reg_write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    uint32_t old;
    std::memcpy(&old, g_regs + (a - kRegLo), 4);
    std::memcpy(g_regs + (a - kRegLo), &v, 4);
    if (a == kArmReset && (old & 1u) && !(v & 1u)) start_driver();
}

}  // namespace

void map_aica() {
    rt::map({kRamLo, kRamLo + kRamBytes, "sound RAM", ram_read, ram_write, ram_block});
    rt::map({kRegLo, kRegLo + kRegBytes, "sound chip registers", reg_read, reg_write});
}

uint8_t* sound_ram() { return g_ram; }

}  // namespace toycommander
