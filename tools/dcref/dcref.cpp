// qilin_dcref: measures what a Dreamcast emulator's SH-4 gives for the FPU
// instructions no clean source pins down (fmac, fsrra, fsca, fipr, ftrv), and
// compares it with candidate definitions written here from the instructions'
// descriptions. The emulator (Flycast's libretro core, fetched by fetch.sh)
// is run as a black box: a probe program, built with the end-to-end tests'
// assembler, is booted as an ELF; its results are read from guest RAM. No
// emulator code or table enters this repository: the lifter's definitions are
// our own, and this tool only says which of them agrees with the emulator.
//
//   qilin_dcref <flycast_libretro.so> [--cases N] [--examples K]
//
// Linux x86-64 only (dlopen, EGL's surfaceless platform with Mesa's software
// renderer: the core needs an OpenGL context to boot). A developer tool, not
// part of CI: the core isn't in the repository.
#include <dlfcn.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <numbers>
#include <print>
#include <random>
#include <string>
#include <vector>

#include <immintrin.h>

#include "guest.hpp"
#include "lift_programs.hpp"
#include "libretro.h"

namespace {

// ---------------------------------------------------------------- the libretro host

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint = int32_t;
void* g_egl = nullptr;
void* (*p_eglGetProcAddress)(const char*) = nullptr;
unsigned g_fbo = 0;
retro_hw_render_callback g_hw{};
std::string g_sys;
bool g_verbose = false;

template <typename F>
F egl(const char* n) {
    return reinterpret_cast<F>(dlsym(g_egl, n));
}
template <typename F>
F gl(const char* n) {
    return reinterpret_cast<F>(p_eglGetProcAddress(n));
}
uintptr_t get_fb() { return g_fbo; }
retro_proc_address_t get_proc(const char* n) { return reinterpret_cast<retro_proc_address_t>(p_eglGetProcAddress(n)); }

// An OpenGL context with no window (EGL_MESA_platform_surfaceless) and a
// 640x480 framebuffer for the core to draw into.
bool make_gl() {
    g_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_egl) return false;
    p_eglGetProcAddress = egl<void* (*)(const char*)>("eglGetProcAddress");
    const auto platform_display = gl<EGLDisplay (*)(unsigned, void*, const EGLint*)>("eglGetPlatformDisplayEXT");
    if (!platform_display) return false;
    EGLDisplay d = platform_display(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    EGLint major = 0, minor = 0;
    if (!egl<unsigned (*)(EGLDisplay, EGLint*, EGLint*)>("eglInitialize")(d, &major, &minor)) return false;
    egl<unsigned (*)(unsigned)>("eglBindAPI")(0x30A2 /* EGL_OPENGL_API */);
    const EGLint config_attr[] = {0x3040 /* RENDERABLE_TYPE */, 0x0008 /* OPENGL_BIT */, 0x3033 /* SURFACE_TYPE */, 0, 0x3038};
    EGLConfig config = nullptr;
    EGLint n = 0;
    if (!egl<unsigned (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>("eglChooseConfig")(d, config_attr, &config, 1, &n) || n < 1)
        return false;
    const bool core = g_hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE;
    const EGLint context_attr[] = {0x3098, core ? static_cast<EGLint>(g_hw.version_major) : 2, 0x30FB, core ? static_cast<EGLint>(g_hw.version_minor) : 1,
                                   0x30FD /* PROFILE_MASK */, core ? 1 : 2, 0x3038};
    EGLContext c = egl<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>("eglCreateContext")(d, config, nullptr, context_attr);
    if (!c || !egl<unsigned (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>("eglMakeCurrent")(d, nullptr, nullptr, c)) return false;
    unsigned rb[2];
    gl<void (*)(int, unsigned*)>("glGenFramebuffers")(1, &g_fbo);
    gl<void (*)(unsigned, unsigned)>("glBindFramebuffer")(0x8D40, g_fbo);
    gl<void (*)(int, unsigned*)>("glGenRenderbuffers")(2, rb);
    const auto bind = gl<void (*)(unsigned, unsigned)>("glBindRenderbuffer");
    const auto storage = gl<void (*)(unsigned, unsigned, int, int)>("glRenderbufferStorage");
    const auto attach = gl<void (*)(unsigned, unsigned, unsigned, unsigned)>("glFramebufferRenderbuffer");
    bind(0x8D41, rb[0]), storage(0x8D41, 0x8058 /* RGBA8 */, 640, 480), attach(0x8D40, 0x8CE0, 0x8D41, rb[0]);
    bind(0x8D41, rb[1]), storage(0x8D41, 0x88F0 /* DEPTH24_STENCIL8 */, 640, 480), attach(0x8D40, 0x821A, 0x8D41, rb[1]);
    return true;
}

void log_cb(retro_log_level level, const char* fmt, ...) {
    if (level < RETRO_LOG_WARN && !g_verbose) return;
    va_list a;
    va_start(a, fmt);
    std::fputs("core: ", stderr);
    std::vfprintf(stderr, fmt, a);
    va_end(a);
}
bool env(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: static_cast<retro_log_callback*>(data)->log = log_cb; return true;
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data) = g_sys.c_str(); return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto* v = static_cast<retro_variable*>(data);
        // The emulator's own BIOS replacement (no Dreamcast BIOS needed), rendering on this thread.
        if (!std::strcmp(v->key, "reicast_hle_bios")) return v->value = "enabled", true;
        if (!std::strcmp(v->key, "reicast_threaded_rendering")) return v->value = "disabled", true;
        return false;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
        auto* cb = static_cast<retro_hw_render_callback*>(data);
        if (cb->context_type != RETRO_HW_CONTEXT_OPENGL_CORE && cb->context_type != RETRO_HW_CONTEXT_OPENGL) return false;
        cb->get_current_framebuffer = get_fb;
        cb->get_proc_address = get_proc;
        g_hw = *cb;
        return true;
    }
    default: return false;
    }
}
// --game's pictures and presses, to compare the port's renderer with the
// emulator's on the same scene: QILIN_DCREF_SHOT=<folder>,<every> writes
// every so many frames as a PPM; QILIN_DCREF_PRESS=<button>@<frame>[,...]
// (or <button>@<first>-<last>/<every>) holds a button for ten frames from
// the given one, with the names QILIN_PRESS uses (a b x y start up down
// left right lt rt).
std::string g_shot_folder;
int g_shot_every = 0, g_frame = 0;
struct Press {
    unsigned id;  // libretro's joypad button
    int from;
};
std::vector<Press> g_presses;

void video(const void* data, unsigned w, unsigned h, size_t) {
    if (g_shot_folder.empty() || g_frame % g_shot_every != 0 || data != RETRO_HW_FRAME_BUFFER_VALID) return;
    std::vector<uint8_t> px(size_t{w} * h * 4);
    gl<void (*)(unsigned, unsigned)>("glBindFramebuffer")(0x8D40, g_fbo);
    gl<void (*)(int, int, int, int, unsigned, unsigned, void*)>("glReadPixels")(0, 0, static_cast<int>(w), static_cast<int>(h), 0x1908 /* RGBA */,
                                                                                0x1401 /* UNSIGNED_BYTE */, px.data());
    const std::string name = g_shot_folder + "/frame_" + std::to_string(g_frame) + ".ppm";
    std::FILE* f = std::fopen(name.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + name);
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (unsigned y = h; y-- > 0;)  // OpenGL's rows run upwards
        for (unsigned x = 0; x < w; ++x) std::fwrite(&px[(size_t{y} * w + x) * 4], 1, 3, f);
    std::fclose(f);
}
void audio(int16_t, int16_t) {}
size_t audio_batch(const int16_t*, size_t n) { return n; }
void poll() {}
bool held(unsigned id) {
    for (const Press& p : g_presses)
        if (p.id == id && g_frame >= p.from && g_frame < p.from + 10) return true;
    return false;
}
int16_t input(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (port != 0) return 0;
    if (device == RETRO_DEVICE_JOYPAD) return held(id) ? 1 : 0;
    // The triggers, as analogue buttons.
    if (device == RETRO_DEVICE_ANALOG && index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) return held(id) ? 0x7FFF : 0;
    return 0;
}
void parse_presses(const char* spec) {
    // The Dreamcast's buttons on libretro's pad as the core lays them out
    // (its A where libretro's B is, and so on round the face).
    static const std::map<std::string, unsigned> ids = {
        {"a", RETRO_DEVICE_ID_JOYPAD_B},       {"b", RETRO_DEVICE_ID_JOYPAD_A},         {"x", RETRO_DEVICE_ID_JOYPAD_Y},
        {"y", RETRO_DEVICE_ID_JOYPAD_X},       {"start", RETRO_DEVICE_ID_JOYPAD_START}, {"up", RETRO_DEVICE_ID_JOYPAD_UP},
        {"down", RETRO_DEVICE_ID_JOYPAD_DOWN}, {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},   {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {"lt", RETRO_DEVICE_ID_JOYPAD_L2},     {"rt", RETRO_DEVICE_ID_JOYPAD_R2}};
    const std::string all = spec;
    for (size_t at = 0; at <= all.size();) {
        size_t end = all.find(',', at);
        if (end == std::string::npos) end = all.size();
        const std::string one = all.substr(at, end - at);
        at = end + 1;
        if (one.empty()) continue;
        const size_t a = one.find('@');
        if (a == std::string::npos || !ids.contains(one.substr(0, a))) throw std::runtime_error("QILIN_DCREF_PRESS: " + one);
        const unsigned id = ids.at(one.substr(0, a));
        int first = 0, last = 0, every = 0;
        if (std::sscanf(one.c_str() + a + 1, "%d-%d/%d", &first, &last, &every) == 3 && every > 0) {
            for (int f = first; f <= last; f += every) g_presses.push_back({id, f});
        } else {
            g_presses.push_back({id, std::atoi(one.c_str() + a + 1)});
        }
    }
}

// Boots `elf` in the core and runs frames until the word at `done` holds
// `magic`; returns guest RAM (16 MB from 0x8C000000).
std::vector<uint8_t> run_core(const char* core_path, const std::filesystem::path& elf, uint32_t done, uint32_t magic) {
    void* h = dlopen(core_path, RTLD_NOW);
    if (!h) throw std::runtime_error(dlerror());
#define SYM(n) const auto n = reinterpret_cast<decltype(&::n)>(dlsym(h, #n))
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_init);
    SYM(retro_load_game);
    SYM(retro_run);
    SYM(retro_get_memory_data);
    SYM(retro_get_memory_size);
#undef SYM
    g_sys = (std::filesystem::temp_directory_path() / "qilin_dcref_sys").string();
    std::filesystem::create_directories(g_sys);
    retro_set_environment(env), retro_set_video_refresh(video), retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch), retro_set_input_poll(poll), retro_set_input_state(input);
    retro_init();
    const std::string path = std::filesystem::absolute(elf).string();
    retro_game_info game{path.c_str(), nullptr, 0, nullptr};
    if (!retro_load_game(&game)) throw std::runtime_error("the core didn't load the probe");
    if (!make_gl()) throw std::runtime_error("no OpenGL context (EGL surfaceless)");
    if (g_hw.context_reset) g_hw.context_reset();
    const auto* ram = static_cast<const uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    if (!ram || retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) != qilin::test::kBase - 0x8C010000u + (16u << 20))
        throw std::runtime_error("no 16 MB of system RAM from the core");
    for (int frame = 0; frame < 3000; ++frame) {
        retro_run();
        uint32_t v;
        std::memcpy(&v, ram + (done - 0x8C000000u), 4);
        if (v == magic) return std::vector<uint8_t>(ram, ram + (16u << 20));
    }
    throw std::runtime_error("the probe didn't finish in 3000 frames");
}

// The game itself, for comparing the port's RAM with the console's at the
// same point of a run: loads `disc` (the player's own dump) in the core with
// its BIOS replacement and writes guest RAM (16 MB from 0x8C000000) to
// `dump`.<frame> every `every` frames up to `frames` (and pictures and
// presses, above). Nothing of the game is kept by this tool: the dumps and
// pictures go where the user says, outside the repo.
int run_game(const char* core_path, const std::filesystem::path& disc, int frames, int every, const std::filesystem::path& dump) {
    void* h = dlopen(core_path, RTLD_NOW);
    if (!h) throw std::runtime_error(dlerror());
#define SYM(n) const auto n = reinterpret_cast<decltype(&::n)>(dlsym(h, #n))
    SYM(retro_set_environment);
    SYM(retro_set_video_refresh);
    SYM(retro_set_audio_sample);
    SYM(retro_set_audio_sample_batch);
    SYM(retro_set_input_poll);
    SYM(retro_set_input_state);
    SYM(retro_init);
    SYM(retro_load_game);
    SYM(retro_run);
    SYM(retro_get_memory_data);
    SYM(retro_get_memory_size);
#undef SYM
    g_sys = (std::filesystem::temp_directory_path() / "qilin_dcref_sys").string();
    std::filesystem::create_directories(g_sys);
    retro_set_environment(env), retro_set_video_refresh(video), retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch), retro_set_input_poll(poll), retro_set_input_state(input);
    retro_init();
    const std::string path = std::filesystem::absolute(disc).string();
    retro_game_info game{path.c_str(), nullptr, 0, nullptr};
    if (!retro_load_game(&game)) throw std::runtime_error("the core didn't load the disc");
    if (!make_gl()) throw std::runtime_error("no OpenGL context (EGL surfaceless)");
    if (g_hw.context_reset) g_hw.context_reset();
    const auto* ram = static_cast<const uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    if (!ram || retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM) < (16u << 20)) throw std::runtime_error("no 16 MB of system RAM from the core");
    if (const char* s = std::getenv("QILIN_DCREF_SHOT")) {
        const std::string v = s;
        const size_t comma = v.rfind(',');
        g_shot_folder = v.substr(0, comma);
        g_shot_every = comma == std::string::npos ? 60 : std::max(1, std::atoi(v.c_str() + comma + 1));
        std::filesystem::create_directories(g_shot_folder);
    }
    if (const char* s = std::getenv("QILIN_DCREF_PRESS")) parse_presses(s);
    for (int frame = 1; frame <= frames; ++frame) {
        g_frame = frame;
        retro_run();
        if (frame % every == 0 || frame == frames) {
            const auto name = dump.string() + "." + std::to_string(frame);
            std::FILE* f = std::fopen(name.c_str(), "wb");
            if (!f || std::fwrite(ram, 1, 16u << 20, f) != (16u << 20)) throw std::runtime_error("cannot write " + name);
            std::fclose(f);
        }
    }
    return 0;
}

// An ELF executable of one loadable segment at 0x8C010000, entered at its start.
std::vector<uint8_t> elf(const std::vector<uint8_t>& image) {
    const uint32_t base = qilin::test::kBase, code_off = 0x100;
    const char shstr[] = "\0.text\0.shstrtab";
    const uint32_t shstr_off = code_off + static_cast<uint32_t>(image.size()), sh_off = (shstr_off + sizeof shstr + 3) & ~3u;
    std::vector<uint8_t> out(sh_off + 3 * 40, 0);
    const auto put16 = [&](size_t at, uint32_t v) { out[at] = static_cast<uint8_t>(v), out[at + 1] = static_cast<uint8_t>(v >> 8); };
    const auto put32 = [&](size_t at, uint32_t v) { put16(at, v), put16(at + 2, v >> 16); };
    std::memcpy(out.data(), "\x7F" "ELF\x01\x01\x01", 7);
    put16(16, 2), put16(18, 42 /* SuperH */), put32(20, 1), put32(24, base), put32(28, 52), put32(32, sh_off);
    put16(40, 52), put16(42, 32), put16(44, 1), put16(46, 40), put16(48, 3), put16(50, 2);
    const uint32_t ph[8] = {1, code_off, base, base, static_cast<uint32_t>(image.size()), static_cast<uint32_t>(image.size()), 7, 4};
    for (int k = 0; k < 8; ++k) put32(52 + 4 * k, ph[k]);
    std::memcpy(out.data() + code_off, image.data(), image.size());
    std::memcpy(out.data() + shstr_off, shstr, sizeof shstr);
    const uint32_t text[10] = {1, 1, 6, base, code_off, static_cast<uint32_t>(image.size()), 0, 0, 4, 0};
    const uint32_t names[10] = {7, 3, 0, 0, shstr_off, sizeof shstr, 0, 0, 1, 0};
    for (int k = 0; k < 10; ++k) put32(sh_off + 40 + 4 * k, text[k]), put32(sh_off + 80 + 4 * k, names[k]);
    return out;
}

// ---------------------------------------------------------------- the probes

using namespace qilin::test;

struct Probe {
    const char* name;
    unsigned in_words, out_words;  // per case
    uint32_t cases = 0, in = 0, out = 0;
    std::vector<uint32_t> inputs;
    std::function<void(Asm&)> body;  // r4: inputs, r5: outputs, advanced by the body
};

uint32_t u32(std::mt19937& rng) { return static_cast<uint32_t>(rng()); }
float f(uint32_t v) { return std::bit_cast<float>(v); }
uint32_t b(float x) { return std::bit_cast<uint32_t>(x); }

// Inputs a game feeds these: ordinary magnitudes, with the edges mixed in.
uint32_t value(std::mt19937& rng) {
    static const uint32_t special[] = {0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0x00000001u, 0x807FFFFFu,
                                       0x00800000u, 0x7F7FFFFFu, 0x3F000000u, 0x40000000u, 0x34000000u};
    switch (rng() % 8) {
    case 0: return special[rng() % std::size(special)];
    case 1: return u32(rng);  // any bits
    default: {                // magnitudes from 2^-20 to 2^20
        const uint32_t e = 127 - 20 + static_cast<uint32_t>(rng() % 41);
        return (u32(rng) & 0x807FFFFFu) | e << 23;
    }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::println(stderr, "usage: qilin_dcref <flycast_libretro.so> [--cases N] [--examples K]");
        return 2;
    }
    if (argc >= 4 && !std::strcmp(argv[2], "--game")) {
        // qilin_dcref <core> --game <disc> <frames> <every> <dump prefix>
        if (argc != 7) {
            std::println(stderr, "usage: qilin_dcref <flycast_libretro.so> --game <disc> <frames> <every> <dump prefix>");
            return 2;
        }
        g_verbose = std::getenv("QILIN_DCREF_VERBOSE") != nullptr;
        return run_game(argv[1], argv[3], std::atoi(argv[4]), std::max(1, std::atoi(argv[5])), argv[6]);
    }
    uint32_t n = 50000;
    int examples = 4;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--cases")) n = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--examples")) examples = std::atoi(argv[i + 1]);
    }
    g_verbose = std::getenv("QILIN_DCREF_VERBOSE") != nullptr;
    std::mt19937 rng(20261121);

    std::vector<Probe> probes;
    // fmac fr0,fr1,fr2: fr2 = fr0 * fr1 + fr2
    probes.push_back({"fmac", 3, 1, n, 0, 0, {}, [](Asm& a) {
                          a.word(nm(0xF009, 0, 4)), a.word(nm(0xF009, 1, 4)), a.word(nm(0xF009, 2, 4));
                          a.word(nm(0xF00E, 2, 1)), a.word(nm(0xF00A, 5, 2)), a.word(imm(0x7000, 5, 4));
                      }});
    // fsrra fr3: 1 / sqrt(fr3)
    probes.push_back({"fsrra", 1, 1, n, 0, 0, {}, [](Asm& a) {
                          a.word(nm(0xF009, 3, 4)), a.word(n_(0xF07D, 3)), a.word(nm(0xF00A, 5, 3)), a.word(imm(0x7000, 5, 4));
                      }});
    // fsca fpul,dr2: fr2 = sin, fr3 = cos of 2 pi (fpul & 0xFFFF) / 65536; every angle
    probes.push_back({"fsca", 1, 2, 65536, 0, 0, {}, [](Asm& a) {
                          a.word(nm(0x6006, 0, 4)), a.word(n_(0x405A, 0)), a.word(n_(0xF0FD, 2));
                          a.word(nm(0xF00A, 5, 2)), a.word(imm(0x7000, 5, 4)), a.word(nm(0xF00A, 5, 3)), a.word(imm(0x7000, 5, 4));
                      }});
    // fipr fv4,fv0: fr3 = fv0 . fv4
    probes.push_back({"fipr", 8, 1, n, 0, 0, {}, [](Asm& a) {
                          for (unsigned k = 0; k < 8; ++k) a.word(nm(0xF009, k, 4));
                          a.word(0xF1ED), a.word(nm(0xF00A, 5, 3)), a.word(imm(0x7000, 5, 4));
                      }});
    // ftrv xmtrx,fv0: the back bank's matrix times fv0
    probes.push_back({"ftrv", 20, 4, n / 2, 0, 0, {}, [](Asm& a) {
                          a.word(0xFBFD);
                          for (unsigned k = 0; k < 16; ++k) a.word(nm(0xF009, k, 4));
                          a.word(0xFBFD);
                          for (unsigned k = 0; k < 4; ++k) a.word(nm(0xF009, k, 4));
                          a.word(0xF1FD);
                          for (unsigned k = 0; k < 4; ++k) a.word(nm(0xF00A, 5, k)), a.word(imm(0x7000, 5, 4));
                      }});

    // The ordinary arithmetic, as a check on the emulator's denormal mode and on the lifter's own definitions.
    const auto binary = [&](const char* name, uint16_t op) {
        probes.push_back({name, 2, 1, n / 2, 0, 0, {}, [op](Asm& a) {
                              a.word(nm(0xF009, 0, 4)), a.word(nm(0xF009, 1, 4)), a.word(nm(op, 0, 1));  // op fr1,fr0
                              a.word(nm(0xF00A, 5, 0)), a.word(imm(0x7000, 5, 4));
                          }});
    };
    binary("fadd", 0xF000), binary("fsub", 0xF001), binary("fmul", 0xF002), binary("fdiv", 0xF003);
    probes.push_back({"fsqrt", 1, 1, n / 2, 0, 0, {}, [](Asm& a) {
                          a.word(nm(0xF009, 0, 4)), a.word(n_(0xF06D, 0)), a.word(nm(0xF00A, 5, 0)), a.word(imm(0x7000, 5, 4));
                      }});

    // Memory: the probe's code at 0x8C010000, inputs from 0x8C100000, outputs from 0x8C700000.
    constexpr uint32_t kDone = 0x8C0FFFF0u, kMagic = 0x51D1F00Du;
    uint32_t in = 0x8C100000u, out = 0x8C700000u;
    for (Probe& p : probes) {
        p.in = in, p.out = out;
        for (uint32_t k = 0; k < p.cases * p.in_words; ++k) p.inputs.push_back(value(rng));
        if (!std::strcmp(p.name, "fsca"))
            for (uint32_t k = 0; k < p.cases; ++k) p.inputs[k] = k | (u32(rng) & 0xFFFF0000u);  // the high half is ignored
        in += p.cases * p.in_words * 4, out += p.cases * p.out_words * 4;
    }
    if (in > 0x8C700000u || out > 0x8CFF0000u) {
        std::println(stderr, "dcref: too many cases for guest RAM");
        return 2;
    }
    Asm a;
    a.movl(0, 0x00040000u), a.word(n_(0x406A, 0));  // lds r0,fpscr: the game's mode
    for (size_t k = 0; k < probes.size(); ++k) {
        Probe& p = probes[k];
        const std::string top = std::format("top{}", k), after = std::format("after{}", k);
        a.movl(4, p.in), a.movl(5, p.out), a.movl(6, p.cases);
        a.b12(0xA000, top), a.word(kNop);  // over the pool
        a.pool();
        a.label(top);
        p.body(a);
        a.word(n_(0x4010, 6));      // dt r6
        a.b8(0x8B00, top);          // bf top
        a.b12(0xA000, after), a.word(kNop);
        a.label(after);
    }
    a.movl(0, kDone), a.movl(1, kMagic), a.word(nm(0x2002, 0, 1));  // mov.l r1,@r0
    a.label("end"), a.b12(0xA000, "end"), a.word(kNop);
    a.pool();
    const std::vector<uint16_t> code = a.finish();
    std::vector<uint8_t> image(in - kBase, 0);
    std::memcpy(image.data(), code.data(), code.size() * 2);
    for (const Probe& p : probes) std::memcpy(image.data() + (p.in - kBase), p.inputs.data(), p.inputs.size() * 4);
    const std::filesystem::path elf_path = std::filesystem::temp_directory_path() / "qilin_dcref_probe.elf";
    {
        const std::vector<uint8_t> e = elf(image);
        std::FILE* file = std::fopen(elf_path.string().c_str(), "wb");
        if (!file || std::fwrite(e.data(), 1, e.size(), file) != e.size()) return std::println(stderr, "dcref: cannot write the probe"), 1;
        std::fclose(file);
    }
    std::vector<uint8_t> ram;
    try {
        ram = run_core(argv[1], elf_path, kDone, kMagic);
    } catch (const std::exception& e) {
        std::println(stderr, "dcref: {}", e.what());
        return 1;
    }
    const auto ram_word = [&](uint32_t addr) {
        uint32_t v;
        std::memcpy(&v, ram.data() + (addr - 0x8C000000u), 4);
        return v;
    };

    // The candidates, in the game's mode: denormals flushed, round to nearest.
    qilin::rt::host_fp_setup();
    const auto same = [](uint32_t x, uint32_t y) { return x == y || (std::isnan(f(x)) && std::isnan(f(y))); };
    const auto ulps = [](uint32_t x, uint32_t y) {
        const auto key = [](uint32_t v) { return (v & 0x80000000u) ? int64_t(0x80000000u) - int64_t(v & 0x7FFFFFFFu) : int64_t(0x80000000u) + v; };
        return std::abs(key(x) - key(y));
    };
    struct Candidate {
        const char* name;
        std::function<void(const uint32_t* in, uint32_t* out)> fn;
    };
    const auto pr = [](float x) { return static_cast<double>(x); };
    std::map<std::string, std::vector<Candidate>> candidates;
    candidates["fmac"] = {
        {"two roundings: (a*b)+c in single", [](const uint32_t* i, uint32_t* o) { const float p = f(i[0]) * f(i[1]); o[0] = b(p + f(i[2])); }},
        {"one rounding: fmaf(a,b,c)", [](const uint32_t* i, uint32_t* o) { o[0] = b(std::fmaf(f(i[0]), f(i[1]), f(i[2]))); }},
        {"in double, rounded once", [&](const uint32_t* i, uint32_t* o) { o[0] = b(static_cast<float>(pr(f(i[0])) * pr(f(i[1])) + pr(f(i[2])))); }},
    };
    candidates["fsrra"] = {
        {"1/sqrt in double, rounded once", [&](const uint32_t* i, uint32_t* o) { o[0] = b(static_cast<float>(1.0 / std::sqrt(pr(f(i[0]))))); }},
        {"1.0f/sqrtf: two roundings", [](const uint32_t* i, uint32_t* o) { o[0] = b(1.0f / std::sqrt(f(i[0]))); }},
    };
    candidates["fsca"] = {
        {"sin, cos in double of 2 pi k / 65536, rounded once",
         [](const uint32_t* i, uint32_t* o) {
             const double t = static_cast<double>(i[0] & 0xFFFFu) * (2.0 * std::numbers::pi / 65536.0);
             o[0] = b(static_cast<float>(std::sin(t))), o[1] = b(static_cast<float>(std::cos(t)));
         }},
        {"sinf, cosf of the angle in single",
         [](const uint32_t* i, uint32_t* o) {
             const float t = static_cast<float>(i[0] & 0xFFFFu) * (2.0f * std::numbers::pi_v<float> / 65536.0f);
             o[0] = b(std::sin(t)), o[1] = b(std::cos(t));
         }},
    };
    const auto dot = [&](const float* x, const float* y, int how) -> float {
        switch (how) {
        case 0: return ((x[0] * y[0] + x[1] * y[1]) + x[2] * y[2]) + x[3] * y[3];
        case 1: return static_cast<float>(pr(x[0]) * pr(y[0]) + pr(x[1]) * pr(y[1]) + pr(x[2]) * pr(y[2]) + pr(x[3]) * pr(y[3]));
        case 2: return (x[0] * y[0] + x[1] * y[1]) + (x[2] * y[2] + x[3] * y[3]);
        default: return std::fmaf(x[3], y[3], std::fmaf(x[2], y[2], std::fmaf(x[1], y[1], x[0] * y[0])));
        }
    };
    const char* dot_names[] = {"single, in order", "in double, rounded once", "single, pairwise", "fmaf chain"};
    for (int how = 0; how < 4; ++how) {
        candidates["fipr"].push_back({dot_names[how], [&, how](const uint32_t* i, uint32_t* o) {
                                          float x[4], y[4];
                                          for (int k = 0; k < 4; ++k) x[k] = f(i[k]), y[k] = f(i[4 + k]);
                                          o[0] = b(dot(x, y, how));
                                      }});
        // XMTRX: xf0, xf4, xf8, xf12 are row 0 (column-major in the bank).
        candidates["ftrv"].push_back({dot_names[how], [&, how](const uint32_t* i, uint32_t* o) {
                                          float v[4];
                                          for (int k = 0; k < 4; ++k) v[k] = f(i[16 + k]);
                                          for (int r = 0; r < 4; ++r) {
                                              float row[4];
                                              for (int c = 0; c < 4; ++c) row[c] = f(i[r + 4 * c]);
                                              o[r] = b(dot(row, v, how));
                                          }
                                      }});
    }

    const auto with_mode = [](bool flush, auto fn) {
        const unsigned saved = _mm_getcsr();
        _mm_setcsr(flush ? 0x9FC0u : 0x1F80u);  // FTZ and DAZ, or neither; all exceptions masked, round to nearest
        fn();
        _mm_setcsr(saved);
    };
    const auto ops = [&](const char* name, auto op) {
        for (const bool flush : {true, false})
            candidates[name].push_back({flush ? "the lifter's: IEEE, denormals flushed (DN = 1)" : "IEEE, denormals kept",
                                        [=](const uint32_t* i, uint32_t* o) { with_mode(flush, [&] { o[0] = b(op(f(i[0]), f(i[1]))); }); }});
    };
    ops("fadd", [](float x, float y) { return qilin::rt::flt(qilin::rt::bits(x)) + y; });
    ops("fsub", [](float x, float y) { return qilin::rt::flt(qilin::rt::bits(x)) - y; });
    ops("fmul", [](float x, float y) { return qilin::rt::flt(qilin::rt::bits(x)) * y; });
    ops("fdiv", [](float x, float y) { return qilin::rt::flt(qilin::rt::bits(x)) / y; });
    ops("fsqrt", [](float x, float) { return std::sqrt(x); });

    // The port's own definitions (recomp/runtime/guest.hpp), in the game's mode.
    namespace rt = qilin::rt;
    const auto port = [&](const char* name, std::function<void(const uint32_t*, uint32_t*)> fn) {
        candidates[name].insert(candidates[name].begin(), {"the port's (guest.hpp)", [=](const uint32_t* i, uint32_t* o) { with_mode(true, [&] { fn(i, o); }); }});
    };
    port("fmac", [](const uint32_t* i, uint32_t* o) { o[0] = b(rt::fmac(f(i[0]), f(i[1]), f(i[2]))); });
    port("fsrra", [](const uint32_t* i, uint32_t* o) { o[0] = b(rt::fsrra(f(i[0]))); });
    port("fsca", [](const uint32_t* i, uint32_t* o) {
        float s0, c0;
        rt::fsca(i[0], s0, c0);
        o[0] = b(s0), o[1] = b(c0);
    });
    port("fipr", [](const uint32_t* i, uint32_t* o) { o[0] = b(rt::fipr(f(i[0]), f(i[1]), f(i[2]), f(i[3]), f(i[4]), f(i[5]), f(i[6]), f(i[7]))); });
    port("ftrv", [](const uint32_t* i, uint32_t* o) {
        float xf[16];
        for (int k = 0; k < 16; ++k) xf[k] = f(i[k]);
        float v0 = f(i[16]), v1 = f(i[17]), v2 = f(i[18]), v3 = f(i[19]);
        rt::ftrv(xf, v0, v1, v2, v3);
        o[0] = b(v0), o[1] = b(v1), o[2] = b(v2), o[3] = b(v3);
    });

    for (const Probe& p : probes) {
        std::println("{}: {} cases", p.name, p.cases);
        for (const Candidate& c : candidates[p.name]) {
            uint32_t match = 0, ordinary = 0, ordinary_match = 0, no_denormal = 0, no_denormal_match = 0;
            int64_t worst = 0;
            int shown = 0;
            std::map<int64_t, uint32_t> hist;
            for (uint32_t k = 0; k < p.cases; ++k) {
                const uint32_t* i = p.inputs.data() + k * p.in_words;
                uint32_t want[4] = {}, got[4] = {};
                c.fn(i, want);
                bool all = true;
                for (unsigned w = 0; w < p.out_words; ++w) {
                    got[w] = ram_word(p.out + (k * p.out_words + w) * 4);
                    if (!same(got[w], want[w])) {
                        all = false;
                        const int64_t d = (std::isnan(f(got[w])) || std::isnan(f(want[w]))) ? -1 : ulps(got[w], want[w]);
                        ++hist[std::min<int64_t>(d, 1000)];
                        worst = std::max(worst, d);
                    }
                }
                match += all;
                // Ordinary inputs: every operand a normal number or zero (no
                // denormal, infinity or NaN, where emulators differ most).
                bool plain = true, denormal = false;
                for (unsigned q = 0; q < p.in_words; ++q) {
                    const uint32_t e = (i[q] >> 23) & 0xFF;
                    if (std::strcmp(p.name, "fsca") && (e == 0xFF || (e == 0 && (i[q] & 0x7FFFFFu)))) plain = false;
                    if (std::strcmp(p.name, "fsca") && e == 0 && (i[q] & 0x7FFFFFu)) denormal = true;
                }
                ordinary += plain, ordinary_match += plain && all;
                no_denormal += !denormal, no_denormal_match += !denormal && all;
                if (!all && shown < examples) {
                    ++shown;
                    std::string ins, g, w;
                    for (unsigned q = 0; q < std::min(p.in_words, 8u); ++q) ins += std::format(" {:08X}", i[q]);
                    for (unsigned q = 0; q < p.out_words; ++q) g += std::format(" {:08X}", got[q]), w += std::format(" {:08X}", want[q]);
                    std::println("      in{} -> emulator{}, candidate{}", ins, g, w);
                }
            }
            if (!std::strcmp(p.name, "fsca")) {  // the difference in absolute terms: what a game sees
                double most = 0;
                for (uint32_t k = 0; k < p.cases * p.out_words; ++k) {
                    uint32_t want[2];
                    c.fn(p.inputs.data() + k / 2, want);
                    most = std::max(most, std::abs(static_cast<double>(f(ram_word(p.out + 4 * k))) - static_cast<double>(f(want[k % 2]))));
                }
                std::println("      largest absolute difference: {:.3g} (2^{:.1f})", most, std::log2(most));
            }
            std::string h;
            for (const auto& [d, count] : hist) h += std::format(" {}:{}", d < 0 ? std::string("NaN") : std::to_string(d), count);
            std::println("  {:6} / {} agree ({} / {} with ordinary inputs, {} / {} without denormals)  {}{}", match, p.cases, ordinary_match, ordinary,
                         no_denormal_match, no_denormal, c.name,
                         match == p.cases ? "" : std::format("  (ulps off:{})", h));
        }
    }
    return 0;
}
