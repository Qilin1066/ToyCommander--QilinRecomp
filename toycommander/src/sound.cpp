// The sound chip's output (sound.hpp).
//
// The channel registers, as the chip has them (32-bit words, the low 16
// bits used; channel n's at n * 0x80): word 0 KYONEX (bit 15), KYONB (14),
// LPCTL (9: loop), PCMS (8-7: 16-bit PCM, 8-bit PCM, 4-bit ADPCM, long
// ADPCM) and the start address's bits 22-16 (6-0); word 1 its bits 15-0;
// word 2 LSA and word 3 LEA, the loop's start and end in samples; word 6
// OCT (14-11, signed) and FNS (9-0), the rate 44.1 kHz * 2^OCT *
// (1 + FNS / 1024); word 9 DISDL (11-8) and DIPAN (4-0); word 10 TL (15-8).
// Levels: DISDL 15 is 0 dB and each step below 3 dB less, 0 silent; TL
// 0.375 dB a step; DIPAN's low four bits attenuate one side 3 dB a step (15:
// silent), the right with bit 4 set, the left without. The master volume
// (0x2800, bits 3-0) and the external inputs' EFSDL and EFPAN (0x2040 and
// 0x2044, as DISDL and DIPAN) go the same way. Yamaha's documents for the
// chip and for its ADPCM (the YMZ280B's data sheet), cited by title.
#include "sound.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "clock.hpp"
#include "gdrom.hpp"
#include "qilin/host/audio.hpp"
#include "system.hpp"

namespace toycommander::sound {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kRamMask = (2u << 20) - 1;

std::mutex g_mutex;
const uint8_t* g_regs = nullptr;
const uint8_t* g_ram = nullptr;
const qilin::gdrom::Disc* g_disc = nullptr;

uint32_t reg(uint32_t off) {
    uint32_t v;
    std::memcpy(&v, g_regs + off, 4);
    return v & 0xFFFFu;
}

// 3 dB steps: 10^(-3 n / 20).
float attenuation(unsigned steps) { return static_cast<float>(std::pow(10.0, -3.0 * steps / 20.0)); }
float send_level(unsigned level) { return level == 0 ? 0.0f : attenuation(15 - level); }
// The two sides' gains for a pan value.
void pan(unsigned p, float& left, float& right) {
    const float a = (p & 15u) == 15u ? 0.0f : attenuation(p & 15u);
    left = right = 1.0f;
    (p & 0x10u ? right : left) = a;
}

// The ADPCM's step multipliers (in 256ths) by a nibble's magnitude, and its
// step's bounds.
constexpr int kStepScale[8] = {230, 230, 230, 230, 307, 409, 512, 614};
constexpr int kStepMin = 127, kStepMax = 24576;

struct Channel {
    bool on = false, ended = false;
    uint32_t start = 0;  // the start address, latched at key on
    unsigned format = 0;
    uint32_t index = 0;  // the sample `now` is (CA)
    double frac = 0;     // how far from `before` to `now`
    float before = 0, now = 0;
    // ADPCM: the decoder's state after `index`, and after the loop's start
    // the first time it was passed (a loop goes back to it).
    int signal = 0, step = kStepMin;
    bool saved = false;
    int saved_signal = 0, saved_step = kStepMin;
    float saved_sample = 0;
};
Channel g_ch[64];

int16_t pcm16(uint32_t a) { return static_cast<int16_t>(g_ram[a & kRamMask] | g_ram[(a + 1) & kRamMask] << 8); }

// Sample i of channel c, decoding forward for ADPCM (i is index + 1).
float decode_next(Channel& c, uint32_t i) {
    switch (c.format) {
    case 0: return pcm16(c.start + 2 * i);
    case 1: return static_cast<float>(static_cast<int8_t>(g_ram[(c.start + i) & kRamMask])) * 256.0f;
    case 2: {
        const uint8_t byte = g_ram[(c.start + i / 2) & kRamMask];
        const int n = (i & 1u) ? byte >> 4 : byte & 15;
        const int diff = ((2 * (n & 7) + 1) * c.step) >> 3;
        c.signal = std::clamp(c.signal + ((n & 8) ? -diff : diff), -32768, 32767);
        c.step = std::clamp((c.step * kStepScale[n & 7]) >> 8, kStepMin, kStepMax);
        return static_cast<float>(c.signal);
    }
    default: rt::fatal("sound: channel format %u (long ADPCM) isn't supported", c.format);
    }
}

void key_on(unsigned n) {
    Channel& c = g_ch[n];
    const uint32_t w0 = reg(n * 0x80u);
    c = Channel{};
    c.on = true;
    c.format = (w0 >> 7) & 3u;
    c.start = ((w0 & 0x7Fu) << 16 | reg(n * 0x80u + 4)) & kRamMask;
    c.now = c.before = decode_next(c, 0);
    if (reg(n * 0x80u + 8) == 0) c.saved = true, c.saved_signal = c.signal, c.saved_step = c.step, c.saved_sample = c.now;
}

// One step forward through the samples: the loop's end goes back to its
// start, or ends the channel.
void advance(Channel& c, uint32_t base) {
    const uint32_t lsa = reg(base + 8), lea = reg(base + 12);
    c.before = c.now;
    if (c.index >= lea) {
        if (!(reg(base) & 0x200u)) {
            c.ended = true;
            c.now = 0;
            return;
        }
        c.index = lsa;
        if (c.format == 2) {
            if (!c.saved) rt::fatal("sound: an ADPCM loop back to a start never played");
            c.signal = c.saved_signal, c.step = c.saved_step, c.now = c.saved_sample;
        } else {
            c.now = decode_next(c, lsa);
        }
        return;
    }
    ++c.index;
    c.now = decode_next(c, c.index);
    if (c.index == lsa && !c.saved) c.saved = true, c.saved_signal = c.signal, c.saved_step = c.step, c.saved_sample = c.now;
}

// CD audio: the drive's play.
struct Cd {
    bool playing = false;
    uint32_t start = 0, end = 0, repeats = 0, at = 0;
    uint32_t frame = 0;  // the sample frame within the sector at `at`
    std::vector<int16_t> buffer;  // sectors from `buffered`
    uint32_t buffered = 0;
} g_cd;
constexpr uint32_t kFrameOffset = 150, kFramesPerSector = 588, kReadAhead = 16;

void cd_sample(float& left, float& right) {
    if (!g_cd.playing) return;
    const uint32_t sector = g_cd.at - kFrameOffset;
    if (g_cd.buffer.empty() || sector < g_cd.buffered || sector >= g_cd.buffered + g_cd.buffer.size() / (2 * kFramesPerSector)) {
        const uint32_t count = std::min(kReadAhead, g_cd.end - g_cd.at);
        auto pcm = g_disc->read_audio(sector, count);
        if (!pcm) rt::fatal("sound: CD audio: %s", pcm.error().c_str());
        g_cd.buffer = std::move(*pcm), g_cd.buffered = sector;
    }
    const size_t k = (size_t{sector - g_cd.buffered} * kFramesPerSector + g_cd.frame) * 2;
    left = g_cd.buffer[k], right = g_cd.buffer[k + 1];
    if (++g_cd.frame < kFramesPerSector) return;
    g_cd.frame = 0;
    if (++g_cd.at < g_cd.end) return;
    // The end: again from the start for each repeat left (15: for ever), or stop there.
    if (g_cd.repeats == 15 || g_cd.repeats-- > 0) {
        g_cd.at = g_cd.start;
    } else {
        g_cd.playing = false;
    }
}

void (*g_tick)() = nullptr;
uint64_t g_rendered = 0;     // samples rendered since guest time 0
std::vector<int16_t> g_out;  // rendered, not yet taken by the mixer thread

void render(uint64_t samples) {
    struct Live {
        Channel* c;
        uint32_t base;
        double step;
        float left, right;
    };
    Live live[64];
    unsigned count = 0;
    for (unsigned n = 0; n < 64; ++n) {
        Channel& c = g_ch[n];
        if (!c.on || c.ended) continue;
        const uint32_t base = n * 0x80u, pitch = reg(base + 0x18), levels = reg(base + 0x24);
        int oct = static_cast<int>((pitch >> 11) & 15u);
        if (oct >= 8) oct -= 16;
        float l, r;
        pan(levels & 0x1Fu, l, r);
        const float g = send_level((levels >> 8) & 15u) *
                        static_cast<float>(std::pow(10.0, -0.375 * double((reg(base + 0x28) >> 8) & 0xFFu) / 20.0));
        live[count++] = {&c, base, std::ldexp(1.0 + double(pitch & 0x3FFu) / 1024.0, oct), l * g, r * g};
    }
    float cd_l[2], cd_r[2];
    for (unsigned k = 0; k < 2; ++k) {
        const uint32_t v = reg(0x2040u + 4 * k);
        pan(v & 0x1Fu, cd_l[k], cd_r[k]);
        cd_l[k] *= send_level((v >> 8) & 15u), cd_r[k] *= send_level((v >> 8) & 15u);
    }
    const float master = send_level(reg(0x2800) & 15u);
    const size_t at = g_out.size();
    g_out.resize(at + samples * 2);
    for (uint64_t s = 0; s < samples; ++s) {
        float left = 0, right = 0;
        for (unsigned k = 0; k < count; ++k) {
            Live& v = live[k];
            Channel& c = *v.c;
            if (c.ended) continue;
            const float x = c.before + (c.now - c.before) * static_cast<float>(c.frac);
            left += x * v.left, right += x * v.right;
            c.frac += v.step;
            while (c.frac >= 1.0 && !c.ended) c.frac -= 1.0, advance(c, v.base);
        }
        float in_l = 0, in_r = 0;
        cd_sample(in_l, in_r);
        left += in_l * cd_l[0] + in_r * cd_l[1], right += in_l * cd_r[0] + in_r * cd_r[1];
        g_out[at + 2 * s] = static_cast<int16_t>(std::clamp(left * master, -32768.0f, 32767.0f));
        g_out[at + 2 * s + 1] = static_cast<int16_t>(std::clamp(right * master, -32768.0f, 32767.0f));
    }
}

// The output: the host's device, a WAV file, or neither.
std::unique_ptr<qilin::host::AudioOut> g_device;
std::FILE* g_wav = nullptr;
uint32_t g_wav_bytes = 0;

void wav_header() {
    auto le32 = [](uint8_t* p, uint32_t v) { for (int k = 0; k < 4; ++k) p[k] = static_cast<uint8_t>(v >> (8 * k)); };
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0, 0, 0, 0, 0, 0, 0, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    le32(h + 4, 36 + g_wav_bytes), le32(h + 24, kRate), le32(h + 28, kRate * 4), le32(h + 40, g_wav_bytes);
    std::fseek(g_wav, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, g_wav);
    std::fseek(g_wav, 0, SEEK_END);
}

}  // namespace

std::unique_lock<std::mutex> lock() { return std::unique_lock<std::mutex>(g_mutex); }

void attach(const uint8_t* registers, const uint8_t* ram, const qilin::gdrom::Disc& disc) {
    g_regs = registers, g_ram = ram, g_disc = &disc;
}

void set_tick(void (*tick)()) { g_tick = tick; }

void sync() {
    if (!g_regs) return;
    const uint64_t target = guest_ns() * kRate / 1'000'000'000ull;
    if (target > g_rendered) render(target - g_rendered), g_rendered = target;
}

void key_execute() {
    sync();
    for (unsigned n = 0; n < 64; ++n) {
        const bool kyonb = reg(n * 0x80u) & 0x4000u;
        if (kyonb && !g_ch[n].on) key_on(n);
        else if (!kyonb && g_ch[n].on) g_ch[n] = Channel{};
    }
}

uint32_t position(unsigned channel) {
    sync();
    return g_ch[channel & 63u].index;
}

bool finished(unsigned channel) {
    sync();
    return g_ch[channel & 63u].on && g_ch[channel & 63u].ended;
}

void cd_play(unsigned first, unsigned last, uint32_t repeats) {
    sync();
    const qilin::gdrom::Track* from = nullptr;
    const qilin::gdrom::Track* to = nullptr;
    for (const auto& t : g_disc->tracks()) {
        if (t.number == first) from = &t;
        if (t.number == last) to = &t;
    }
    if (!from || !to || !from->audio || !to->audio || last < first || repeats > 15)
        rt::fatal("sound: CD play of tracks %u to %u, %u repeats", first, last, repeats);
    g_cd.playing = true, g_cd.repeats = repeats, g_cd.frame = 0;
    g_cd.start = from->start + kFrameOffset, g_cd.end = to->end() + kFrameOffset, g_cd.at = g_cd.start;
    g_cd.buffer.clear();
}
void cd_pause() {
    sync();
    g_cd.playing = false;
}
void cd_resume() {
    sync();
    if (g_cd.at == 0) rt::fatal("sound: CD audio resumed before any play");
    if (g_cd.at < g_cd.end) g_cd.playing = true;
}
CdState cd_state() {
    sync();
    return {g_cd.playing, g_cd.at};
}

void start() {
    const char* headless = std::getenv("QILIN_HEADLESS");
    if (!(headless && *headless && *headless != '0')) g_device = std::make_unique<qilin::host::AudioOut>();
    if (const char* path = std::getenv("QILIN_WAV")) {
        g_wav = std::fopen(path, "wb");
        if (!g_wav) rt::fatal("QILIN_WAV: cannot write %s", path);
        wav_header();
    }
    std::thread([] {
        std::vector<int16_t> block;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            {
                auto held = lock();
                sync();
                if (g_tick) g_tick();
                block.swap(g_out);
                g_out.clear();
            }
            if (block.empty()) continue;
            if (g_device) g_device->push(block.data(), block.size() / 2, kRate);
            if (g_wav) {
                std::fwrite(block.data(), 2, block.size(), g_wav);
                g_wav_bytes += static_cast<uint32_t>(block.size() * 2);
                wav_header();
                std::fflush(g_wav);
            }
        }
    }).detach();
}

}  // namespace toycommander::sound
