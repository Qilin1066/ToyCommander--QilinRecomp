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
//   qilin_dcref <flycast_libretro.so> --fog [density ... | random]
//
// --fog does the same for the PowerVR2's table fog: how a pixel's depth picks
// its value from the fog table, against renderer::table_fog's definition
// (below, "--fog"). And --game runs the player's own disc, for comparing RAM
// and pictures with the port's.
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
        // --fog renders into texture memory and reads the pixels back from video memory.
        if (!std::strcmp(v->key, "reicast_enable_rttb")) return v->value = "enabled", true;
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

// ---------------------------------------------------------------- --fog
// How the PowerVR2's table fog picks its fog value from a pixel's depth, as
// the emulator does it, measured as a black box: a probe program sets the
// chip up itself (registers, a region array, an opaque list through the
// store queues) and draws horizontal bands, each a black polygon at its own
// 1/w with white table fog, for each fog table and density asked for; the
// tool reads each picture back and prints the fog value (the white) of every
// band. No emulator code: only what its pictures show.
//
// The probe is a small interpreter over a command list in guest RAM, so each
// scene is data the tool builds:
//   {0}                 done
//   {1, address, value} store a word
//   {2, address, mask}  wait until (word & mask) != 0
//   {3, w0..w7}         32 bytes to the tile accelerator (store queue 0)
//   {4}                 a picture: count it, wait for the tool's answer
//   {5, from, to}       copy a word (the rendered pixels into main RAM)
using namespace qilin::test;
constexpr uint32_t kFogResults = 0x8C800000u, kFogCommands = 0x8C100000u, kFogCount = 0x8C0FFFE0u, kFogAck = 0x8C0FFFE4u, kFogDone = 0x8C0FFFF0u, kFogMagic = 0xF06F06F0u;

std::vector<uint16_t> fog_interpreter() {
    Asm a;
    a.movl(4, kFogCommands);
    a.label("loop");
    a.word(nm(0x6006, 0, 4));  // mov.l @r4+,r0
    a.word(imm(0x8800, 0, 0)), a.b8(0x8900, "done");  // cmp/eq #0,r0 ; bt done
    a.word(imm(0x8800, 0, 1)), a.b8(0x8900, "store");
    a.word(imm(0x8800, 0, 2)), a.b8(0x8900, "wait");
    a.word(imm(0x8800, 0, 3)), a.b8(0x8900, "burst");
    a.word(imm(0x8800, 0, 4)), a.b8(0x8900, "picture");
    a.word(imm(0x8800, 0, 5)), a.b8(0x8900, "copy");
    a.label("stuck"), a.b12(0xA000, "stuck"), a.word(kNop);
    a.label("store");
    a.word(nm(0x6006, 1, 4)), a.word(nm(0x6006, 2, 4)), a.word(nm(0x2002, 1, 2));  // r1 = address, r2 = value, mov.l r2,@r1
    a.b12(0xA000, "loop"), a.word(kNop);
    a.label("wait");
    a.word(nm(0x6006, 1, 4)), a.word(nm(0x6006, 2, 4));
    a.label("wait_again");
    a.word(nm(0x6002, 3, 1)), a.word(nm(0x2008, 3, 2));  // mov.l @r1,r3 ; tst r2,r3
    a.b8(0x8900, "wait_again");
    a.b12(0xA000, "loop"), a.word(kNop);
    a.label("burst");
    a.movl(1, 0xE0000000u);
    for (int k = 0; k < 8; ++k) a.word(nm(0x6006, 2, 4)), a.word(nm(0x2002, 1, 2)), a.word(imm(0x7000, 1, 4));
    a.movl(1, 0xE0000000u), a.word(n_(0x0083, 1));  // pref @r1: the queue to the tile accelerator
    a.b12(0xA000, "loop"), a.word(kNop);
    a.label("copy");
    a.word(nm(0x6006, 1, 4)), a.word(nm(0x6006, 2, 4)), a.word(nm(0x6002, 3, 1)), a.word(nm(0x2002, 2, 3));  // *r2 = *r1
    a.b12(0xA000, "loop"), a.word(kNop);
    a.label("picture");
    a.movl(1, kFogCount), a.word(nm(0x6002, 2, 1)), a.word(imm(0x7000, 2, 1)), a.word(nm(0x2002, 1, 2));  // ++count
    a.movl(1, kFogAck);
    a.label("ack");
    a.word(nm(0x6002, 3, 1)), a.word(nm(0x3000, 3, 2));  // mov.l @r1,r3 ; cmp/eq r2,r3
    a.b8(0x8B00, "ack");  // bf ack
    a.b12(0xA000, "loop"), a.word(kNop);
    a.label("done");
    a.movl(0, kFogDone), a.movl(1, kFogMagic), a.word(nm(0x2002, 0, 1));
    a.label("end"), a.b12(0xA000, "end"), a.word(kNop);
    a.pool();
    return a.finish();
}

struct FogScene {
    uint32_t density;        // FOG_DENSITY
    uint32_t table[128];     // FOG_TABLE's entries
    std::vector<float> inv_w;  // one band each, top down
    // For --modes: the TSP's fog mode (bits 23-22) and colour clamp (bit 21)
    // and the bands' colour (ARGB); with `offset_alpha`, the bands are
    // textured (white texels, modulated by the colour) with an offset colour
    // of black at that alpha, one a band. FOG_CLAMP_MAX and FOG_CLAMP_MIN.
    uint32_t mode = 0;
    bool clamp = false;
    uint32_t colour = 0xFF000000u;
    std::vector<uint32_t> offset_alpha = {};
    uint32_t clamp_max = 0xFFFFFFFFu, clamp_min = 0;
};

// The commands for one scene: the chip set up for a 640x480 picture (the
// video timing of an ordinary NTSC 640x480 interlaced mode), the bands sent
// as opaque polygons with table fog (or none, `fog_mode` 2), rendered, shown,
// and a picture taken.
void fog_scene(std::vector<uint32_t>& c, const FogScene& sc, uint32_t out) {
    const auto st = [&](uint32_t a, uint32_t v) { c.insert(c.end(), {1u, a, v}); };
    const auto wait = [&](uint32_t a, uint32_t m) { c.insert(c.end(), {2u, a, m}); };
    const auto burst = [&](std::initializer_list<uint32_t> w) { c.push_back(3u), c.insert(c.end(), w); };
    const auto fl = [](float x) { return std::bit_cast<uint32_t>(x); };
    constexpr uint32_t R = 0xA05F8000u, VRAM = 0xA5000000u, ISTNRM = 0xA05F6900u;
    constexpr uint32_t kOl = 0x12A580u, kRegion = 0x141D80u, kBackground = 0x141C80u, kFrame = 0x200000u;
    // Display: 16 bits a pixel, 640x480, the frame shown from kFrame.
    st(R + 0x044, 0x00800005u), st(R + 0x04C, 0xA0u), st(R + 0x05C, 0x0017753Fu), st(R + 0x0D0, 0x100u);
    st(R + 0x0C8, 0x03450000u), st(R + 0x0CC, 0x00150208u), st(R + 0x0D4, 0x007E0345u), st(R + 0x0D8, 0x020C0359u);
    st(R + 0x0DC, 0x00280208u), st(R + 0x0E0, 0x03F1933Fu), st(R + 0x0E8, 0x00160000u), st(R + 0x0EC, 0xA8u), st(R + 0x0F0, 0x00280028u), st(R + 0x0F4, 0x400u);
    // Rendering: into texture memory at kFrame, RGB565 (the emulator renders to texture memory only in 16 bits), the whole picture.
    st(R + 0x048, 0x1u), st(R + 0x04C, 640 * 2 / 8), st(R + 0x060, 0x01000000u | kFrame), st(R + 0x068, 0x027F0000u), st(R + 0x06C, 0x01DF0000u);
    st(R + 0x074, 0), st(R + 0x078, fl(0.001f)), st(R + 0x07C, 0x0027DF77u), st(R + 0x080, 7u), st(R + 0x098, 0x00800408u), st(R + 0x108, 0);
    // Fog: white from the table; the table and density asked for.
    st(R + 0x0B0, 0x00FFFFFFu), st(R + 0x0B4, 0x00FFFFFFu), st(R + 0x0B8, sc.density), st(R + 0x0BC, sc.clamp_max), st(R + 0x0C0, sc.clamp_min);
    // A texture for the textured bands: 8x8 white texels (RGB565, raster) at kTexture.
    constexpr uint32_t kTexture = 0x300000u;
    for (unsigned k = 0; k < 32; ++k) st(0xA4000000u + kTexture + 4 * k, 0xFFFFFFFFu);
    for (unsigned k = 0; k < 128; ++k) st(R + 0x200 + 4 * k, sc.table[k]);
    // The background: black and far, its plane in video memory before the region array.
    const uint32_t bg[] = {0xF0000000u, 0x20800000u, 0, fl(0), fl(0), fl(1e-9f), 0xFF000000u, fl(640), fl(0), fl(1e-9f), 0xFF000000u, fl(0), fl(480), fl(1e-9f), 0xFF000000u};
    for (unsigned k = 0; k < std::size(bg); ++k) st(VRAM + kBackground + 4 * k, bg[k]);
    st(R + 0x088, fl(1e-9f)), st(R + 0x08C, 0x01000000u | (kBackground / 4) << 3);
    // The region array: 20 x 15 tiles, each with an opaque list only.
    for (uint32_t t = 0; t < 300; ++t) {
        const uint32_t x = t % 20, y = t / 20, a = VRAM + kRegion + 24 * t;
        st(a, (t == 299 ? 0x80000000u : 0u) | y << 8 | x << 2), st(a + 4, kOl + 128 * t);
        for (unsigned l = 2; l < 6; ++l) st(a + 4 * l, 0x80000000u);
    }
    // The tile accelerator: parameters from 0, object lists from kOl, 32 words a tile's opaque list.
    st(R + 0x124, kOl), st(R + 0x128, 0), st(R + 0x12C, 0x000DFCE0u), st(R + 0x130, 0x000DFBE0u), st(R + 0x13C, 0x000E0013u);
    st(R + 0x140, 0x00100003u), st(R + 0x164, kOl), st(0xFF000038u, 0x10u), st(ISTNRM, 0xFFFFFFFFu), st(R + 0x144, 0x80000000u);
    // The bands.
    const unsigned n = static_cast<unsigned>(sc.inv_w.size()), h = 480 / n;
    for (unsigned k = 0; k < n; ++k) {
        const float y0 = float(k * h), y1 = float(k * h + h), z = sc.inv_w[k];
        const uint32_t tsp = 0x20000000u | sc.mode << 22 | (sc.clamp ? 1u << 21 : 0u);
        if (sc.offset_alpha.empty()) {  // vertex type 0: packed colour
            burst({0x80800000u, 0xE0000000u, tsp, 0, 0, 0, 0, 0});
            burst({0xE0000000u, fl(0), fl(y0), fl(z), 0, 0, sc.colour, 0});
            burst({0xE0000000u, fl(640), fl(y0), fl(z), 0, 0, sc.colour, 0});
            burst({0xE0000000u, fl(0), fl(y1), fl(z), 0, 0, sc.colour, 0});
            burst({0xF0000000u, fl(640), fl(y1), fl(z), 0, 0, sc.colour, 0});
        } else {  // vertex type 3: textured, packed colour, an offset colour
            const uint32_t off = sc.offset_alpha[k] << 24;
            burst({0x8080000Cu, 0xE0000000u, tsp | 1u << 6, 1u << 27 | 1u << 26 | kTexture >> 3, 0, 0, 0, 0});
            burst({0xE0000000u, fl(0), fl(y0), fl(z), fl(0), fl(0), sc.colour, off});
            burst({0xE0000000u, fl(640), fl(y0), fl(z), fl(1), fl(0), sc.colour, off});
            burst({0xE0000000u, fl(0), fl(y1), fl(z), fl(0), fl(1), sc.colour, off});
            burst({0xF0000000u, fl(640), fl(y1), fl(z), fl(1), fl(1), sc.colour, off});
        }
    }
    burst({0, 0, 0, 0, 0, 0, 0, 0});  // the list's end
    wait(ISTNRM, 0x80u);              // the opaque list's end
    st(R + 0x020, 0), st(R + 0x02C, kRegion), st(ISTNRM, 0xFFFFFFFFu), st(R + 0x014, 1);
    wait(ISTNRM, 0x4u);  // the render's end
    // Each band's middle two pixels into main RAM (a word: 2 pixels of 16 bits).
    for (unsigned k = 0; k < n; ++k) c.insert(c.end(), {5u, 0xA4000000u + kFrame + ((k * h + h / 2) * 640 + 320) * 2, out + 4 * k});
    c.push_back(4u);
}

// Runs the scenes; `pictures` gets each one's 60 band pixels (RGB565, two to a word, the left one first).
int run_fog(const char* core_path, const std::vector<FogScene>& scenes, std::vector<std::vector<uint8_t>>& pictures) {
    const std::vector<uint16_t> code = fog_interpreter();
    std::vector<uint32_t> cmds;
    for (size_t i = 0; i < scenes.size(); ++i) fog_scene(cmds, scenes[i], kFogResults + 256 * static_cast<uint32_t>(i));
    cmds.push_back(0u);
    std::vector<uint8_t> image(kFogCommands - kBase + cmds.size() * 4, 0);
    std::memcpy(image.data(), code.data(), code.size() * 2);
    std::memcpy(image.data() + (kFogCommands - kBase), cmds.data(), cmds.size() * 4);
    const std::filesystem::path elf_path = std::filesystem::temp_directory_path() / "qilin_dcref_fog.elf";
    {
        const std::vector<uint8_t> e = elf(image);
        std::FILE* file = std::fopen(elf_path.string().c_str(), "wb");
        if (!file || std::fwrite(e.data(), 1, e.size(), file) != e.size()) throw std::runtime_error("cannot write the probe");
        std::fclose(file);
    }
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
#undef SYM
    g_sys = (std::filesystem::temp_directory_path() / "qilin_dcref_sys").string();
    std::filesystem::create_directories(g_sys);
    retro_set_environment(env), retro_set_video_refresh(video), retro_set_audio_sample(audio);
    retro_set_audio_sample_batch(audio_batch), retro_set_input_poll(poll), retro_set_input_state(input);
    retro_init();
    const std::string path = std::filesystem::absolute(elf_path).string();
    retro_game_info game{path.c_str(), nullptr, 0, nullptr};
    if (!retro_load_game(&game)) throw std::runtime_error("the core didn't load the probe");
    if (!make_gl()) throw std::runtime_error("no OpenGL context (EGL surfaceless)");
    if (g_hw.context_reset) g_hw.context_reset();
    auto* ram = static_cast<uint8_t*>(retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const auto word = [&](uint32_t a) { uint32_t v; std::memcpy(&v, ram + (a - 0x8C000000u), 4); return v; };
    uint32_t taken = 0;
    for (int frame = 0; frame < 20000 && word(kFogDone) != kFogMagic; ++frame) {
        retro_run();
        if (word(kFogCount) > taken) {
            std::vector<uint8_t> px(240);
            std::memcpy(px.data(), ram + (kFogResults + 256 * taken - 0x8C000000u), px.size());
            pictures.push_back(px);
            ++taken;
            std::memcpy(ram + (kFogAck - 0x8C000000u), &taken, 4);
        }
    }
    if (word(kFogDone) != kFogMagic) throw std::runtime_error(std::format("the fog probe didn't finish ({} pictures)", taken));
    return 0;
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
    if (argc >= 3 && !std::strcmp(argv[2], "--fog")) {
        // qilin_dcref <core> --fog [density ...]: bands at 1/w = 2^(2 - k/4), k = 0..59, under three tables
        // (entry i = 2i in both bytes; 0xFF00 in every entry; 0x00FF in every entry), for each density.
        std::vector<uint32_t> densities;
        for (int i = 3; i < argc; ++i) densities.push_back(static_cast<uint32_t>(std::strtoul(argv[i], nullptr, 16)));
        if (densities.empty()) densities = {0x8000u};
        std::vector<FogScene> scenes;
        if (argc == 4 && !std::strcmp(argv[3], "random")) {  // random tables, densities and depths
            densities.clear();
            std::mt19937 rng(20261122);
            for (int i = 0; i < 40; ++i) {
                FogScene sc{static_cast<uint32_t>(rng() & 0xFFFFu), {}, {}};
                if (i % 2) sc.density = (sc.density & 0xFF00u) | static_cast<uint8_t>(int8_t(rng() % 25) - 12);  // exponents a game would use
                for (unsigned k = 0; k < 128; ++k) sc.table[k] = rng() & 0xFFFFu;
                for (int k = 0; k < 60; ++k) sc.inv_w.push_back(std::exp2(float(int(rng() % 4000)) / 100.0f - 25.0f));
                scenes.push_back(sc);
            }
        }
        for (uint32_t d : densities)
            for (int t = 0; t < 3; ++t) {
                FogScene sc{d, {}, {}};
                for (unsigned i = 0; i < 128; ++i) sc.table[i] = t == 0 ? (2 * i) << 8 | 2 * i : t == 1 ? 0xFF00u : 0x00FFu;
                for (int k = 0; k < 60; ++k) sc.inv_w.push_back(std::exp2(2.0f - float(k) / 4.0f));
                scenes.push_back(sc);
            }
        std::vector<std::vector<uint8_t>> pictures;
        try {
            run_fog(argv[1], scenes, pictures);
        } catch (const std::exception& e) {
            std::println(stderr, "dcref: {}", e.what());
            return 1;
        }
        // The candidate: the fog coefficient is FOG_DENSITY (mantissa bits 15-8 as 1.7 fixed point, times 2 to
        // the signed exponent in bits 7-0) times 1/w; its exponent e and mantissa m (1 <= m < 2) pick entry
        // 16 e + the top 4 bits of m's fraction, clamped to 0..127 (below 1: entry 0, at its high byte), and the
        // value goes from the entry's high byte to its low byte by m's next bits.
        const auto candidate = [](uint32_t density, const uint32_t* table, float inv_w) {
            const float d = float((density >> 8) & 0xFFu) / 128.0f * std::exp2(float(static_cast<int8_t>(density & 0xFFu)));
            const float coef = d * inv_w;
            if (!(coef >= 1.0f)) return float(table[0] >> 8);
            int e = 0;
            const float m = std::frexp(coef, &e) * 2.0f;  // [1, 2)
            const float steps = (m - 1.0f) * 16.0f;
            int index = 16 * (e - 1) + int(steps);
            float frac = steps - std::floor(steps);
            if (index > 127) index = 127, frac = 1.0f;
            const float hi = float(table[index] >> 8), lo = float(table[index] & 0xFFu);
            return hi * (1.0f - frac) + lo * frac;
        };
        int worst = 0, off = 0;
        for (size_t p = 0; p < pictures.size(); ++p) {
            const FogScene& sc = scenes[p];
            std::print("density {:04X} scene {}:", sc.density, p);
            for (int k = 0; k < 60; ++k) {
                const int got = (pictures[p][4 * k] | pictures[p][4 * k + 1] << 8) >> 5 & 63;  // 565's green
                const int want = int(std::lround(candidate(sc.density, sc.table, sc.inv_w[k]) * 63.0f / 255.0f));
                worst = std::max(worst, std::abs(got - want)), off += std::abs(got - want) > 1;
                if (std::abs(got - want) > 1) std::print(" {}[{}]", got, want);
                else std::print(" {}", got);
            }
            std::println("");
        }
        std::println("the candidate against the emulator: {} of {} bands more than 1/63 off, at worst {}/63", off, pictures.size() * 60, worst);
        return off == 0 ? 0 : 1;
    }
    if (argc == 3 && !std::strcmp(argv[2], "--modes")) {
        // qilin_dcref <core> --modes: what the renderer took from the hardware's description and hadn't yet
        // measured (docs/runtime.md, step 39): vertex fog, the fog table's second mode, the colour clamp and its
        // order against the fog. Each scene's prediction is the renderer's (pvr.frag), its colours 0-255.
        struct Case {
            const char* what;
            FogScene scene;
            std::function<std::array<float, 3>(int band)> want;
        };
        std::vector<Case> cases;
        const auto bands = [](float inv_w) { return std::vector<float>(60, inv_w); };
        {
            Case c{"vertex fog: black, towards FOG_COL_VERT (white) by the offset colour's alpha", {0x8000u, {}, bands(1.0f)}, {}};
            c.scene.mode = 1;
            for (int k = 0; k < 60; ++k) c.scene.offset_alpha.push_back(static_cast<uint32_t>(k * 255 / 59));
            c.want = [](int k) { const float a = float(k * 255 / 59); return std::array<float, 3>{a, a, a}; };
            cases.push_back(std::move(c));
        }
        {
            Case c{"the table's second mode: red becomes FOG_COL_RAM (white)", {0x8000u, {}, bands(0.5f)}, {}};
            c.scene.mode = 3, c.scene.colour = 0xFFFF0000u;
            for (uint32_t& e : c.scene.table) e = 0x8040u;
            c.want = [](int) { return std::array<float, 3>{255, 255, 255}; };
            cases.push_back(std::move(c));
        }
        {
            Case c{"the colour clamp, no fog: (0x20, 0xFF, 0x80) between (0x40, 0, 0) and (0xFF, 0x40, 0xFF)", {0x8000u, {}, bands(1.0f)}, {}};
            c.scene.mode = 2, c.scene.clamp = true, c.scene.colour = 0xFF20FF80u, c.scene.clamp_max = 0xFFFF40FFu, c.scene.clamp_min = 0x00400000u;
            c.want = [](int) { return std::array<float, 3>{64, 64, 128}; };
            cases.push_back(std::move(c));
        }
        {
            Case c{"the clamp before table fog: green held at 0x40, then half way to white", {0x8000u, {}, bands(0.5f)}, {}};
            c.scene.mode = 0, c.scene.clamp = true, c.scene.colour = 0xFF00FF00u, c.scene.clamp_max = 0xFFFF40FFu;
            for (uint32_t& e : c.scene.table) e = 0x8080u;
            c.want = [](int) {
                const float f = 128.0f / 255.0f;
                return std::array<float, 3>{255 * f, 64 + (255 - 64) * f, 255 * f};
            };
            cases.push_back(std::move(c));
        }
        std::vector<FogScene> scenes;
        for (const Case& c : cases) scenes.push_back(c.scene);
        std::vector<std::vector<uint8_t>> pictures;
        try {
            run_fog(argv[1], scenes, pictures);
        } catch (const std::exception& e) {
            std::println(stderr, "dcref: {}", e.what());
            return 1;
        }
        int failed = 0;
        for (size_t i = 0; i < cases.size(); ++i) {
            int off = 0;
            std::string first;
            for (int k = 0; k < 60; ++k) {
                const unsigned px = pictures[i][4 * k] | pictures[i][4 * k + 1] << 8;
                const int got[3] = {int(px >> 11 & 31), int(px >> 5 & 63), int(px & 31)};
                const auto w = cases[i].want(k);
                const int want[3] = {int(std::lround(w[0] * 31 / 255)), int(std::lround(w[1] * 63 / 255)), int(std::lround(w[2] * 31 / 255))};
                bool ok = true;
                for (int ch = 0; ch < 3; ++ch) ok = ok && std::abs(got[ch] - want[ch]) <= 1;
                if (!ok && first.empty())
                    first = std::format(" (band {}: {} {} {} in 565, wanted {} {} {})", k, got[0], got[1], got[2], want[0], want[1], want[2]);
                off += !ok;
            }
            std::println("{}: {} of 60 bands off{}", cases[i].what, off, first);
            failed += off != 0;
        }
        return failed == 0 ? 0 : 1;
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
