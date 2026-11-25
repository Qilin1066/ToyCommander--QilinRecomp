// The settings file (dreamcast/src/settings.cpp): what it writes reads
// back the same, edits are read, and mistakes are reported with their line.
// (From Punch-Out's.)
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>

#include "qilin/host/pad.hpp"
#include "settings.hpp"
#include "system.hpp"

// The runtime's messages, for the settings file's loader (no runtime here).
namespace qilin::rt {
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    char message[1024];
    std::vsnprintf(message, sizeof message, format, args);
    va_end(args);
    throw std::runtime_error(message);
}
void log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
}
}  // namespace qilin::rt

namespace {

namespace tc = dreamcast;

int g_failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

bool same(const tc::Settings& a, const tc::Settings& b) {
    if (a.scale != b.scale || a.preset != b.preset || a.fullscreen != b.fullscreen || a.monitor != b.monitor || a.scaling != b.scaling ||
        a.smooth != b.smooth || a.look != b.look || a.scanlines != b.scanlines || a.mask != b.mask || a.vsync != b.vsync || a.gpu != b.gpu ||
        a.performance_overlay != b.performance_overlay || a.match_refresh != b.match_refresh || a.variable_refresh != b.variable_refresh ||
        a.rumble != b.rumble || a.language != b.language || a.stereo != b.stereo || a.anisotropy != b.anisotropy || a.msaa != b.msaa ||
        a.texture_packs != b.texture_packs || a.dump_textures != b.dump_textures ||
        a.rumble_strength != b.rumble_strength || a.volume != b.volume || a.mute_unfocused != b.mute_unfocused)
        return false;
    for (unsigned c = 0; c < tc::kControlCount; ++c)
        if (a.controls[c] != b.controls[c]) return false;
    return true;
}

// The error a text gives, or "" if it reads.
std::string error_of(const std::string& text) {
    tc::Settings s;
    std::string e;
    return tc::parse_settings(text, s, e) ? "" : e;
}

}  // namespace

int main() {
    using tc::Binding;
    namespace host = qilin::host;
    const tc::Settings d = tc::default_settings();
    {
        tc::Settings s;
        std::string e;
        check(tc::parse_settings(tc::settings_text(d), s, e) && same(s, d), "the defaults read back as written");
        check(tc::parse_settings("", s, e) && same(s, d), "an empty file means the defaults");
    }
    {
        tc::Settings s;
        std::string e;
        const bool ok = tc::parse_settings(
            "[Display]\n"
            "internal_scale = 3   # sharper\n"
            "\n"
            "[controls]\n"
            "  STICK_LEFT = pad:Y , KEY:Right Shift,mouse\n"
            "start =\n",
            s, e);
        check(ok && s.scale == 3, "internal_scale is read, sections and names in any case");
        check(ok && s.controls[tc::kCtlStickLeft].size() == 3 && s.controls[tc::kCtlStickLeft][0].pad == host::kPadY &&
                  s.controls[tc::kCtlStickLeft][1].kind == Binding::kKey && s.controls[tc::kCtlStickLeft][1].key == "Right Shift" &&
                  s.controls[tc::kCtlStickLeft][2].kind == Binding::kMouse,
              "a control takes pads, keys with spaces in their names, and the mouse");
        check(ok && s.controls[tc::kCtlStart].empty(), "an empty list leaves a control unused");
        tc::Settings untouched = s;
        untouched.scale = d.scale;
        untouched.controls[tc::kCtlStickLeft] = d.controls[tc::kCtlStickLeft];
        untouched.controls[tc::kCtlStart] = d.controls[tc::kCtlStart];
        check(ok && same(untouched, d), "controls not mentioned keep their defaults");
    }
    check(error_of("[controls]\njump = pad:A\n").rfind("line 2: 'jump' isn't a control", 0) == 0, "an unknown control is reported with its line");
    check(error_of("[controls]\na = pad:Z\n").rfind("line 2: 'Z' isn't a pad button", 0) == 0, "an unknown pad button");
    check(error_of("[controls]\na = Z\n").rfind("line 2: 'Z' isn't an input", 0) == 0, "an input without its kind");
    check(error_of("[display]\ninternal_scale = 9\n").rfind("line 2: internal_scale is a whole number from 1 to 8", 0) == 0, "a scale out of range");
    {
        tc::Settings s;
        std::string e;
        check(tc::parse_settings("[display]\ninternal_scale = auto\nmatch_refresh = false\nvariable_refresh = true\n", s, e) && s.scale == 0 &&
                  !s.match_refresh && s.variable_refresh,
              "auto scale and the refresh switches");
        check(tc::parse_settings("[display]\npreset = desktop\ninternal_scale = 2\n", s, e) && s.scale == 2 && s.preset == "desktop" &&
                  s.anisotropy == 16 && s.msaa == 4,
              "a preset, then a key that overrides it");
        check(tc::parse_settings("[display]\npreset = desktop\nmsaa = 1\n", s, e) && s.msaa == 1, "msaa overrides the preset's");
        check(tc::parse_settings("[display]\npreset = low-end\n", s, e) && s.msaa == 1, "low-end has no multisampling");
        check(!tc::parse_settings("[display]\npreset = huge\n", s, e) && e.find("presets are") != std::string::npos, "an unknown preset");
    }
    check(error_of("[controls]\nstart = key:F1\n").find("kept for the program") != std::string::npos, "F1 can't be bound");
    check(error_of("[controls]\nstart = key:escape\n").find("kept for the program") != std::string::npos, "nor Escape");
    {
        tc::Settings s = d;
        s.preset = "desktop", s.fullscreen = true, s.monitor = "DELL U2720Q", s.scaling = host::Scaling::integer;
        s.smooth = false, s.look = host::Look::crt, s.scanlines = 80, s.mask = 0, s.scale = 5;
        s.vsync = false, s.gpu = "AMD Radeon RX 6600", s.rumble = false, s.language = 3, s.stereo = false, s.performance_overlay = true;
        s.anisotropy = 8, s.msaa = 8, s.texture_packs = false, s.dump_textures = true, s.rumble_strength = 40, s.volume = 65, s.mute_unfocused = true;
        s.controls[tc::kCtlL] = {{Binding::kPad, host::kPadLT, {}}, {Binding::kKey, 0, "Left Shift"}};
        tc::Settings back;
        std::string e;
        check(tc::parse_settings(tc::settings_text(s), back, e) && same(back, s), "every setting reads back as written");
    }
    check(error_of("[display]\nlook = vhs\n").find("look is plain or crt") != std::string::npos, "an unknown look");
    check(error_of("[display]\nscanlines = 101\n").find("from 0 to 100") != std::string::npos, "scanlines out of range");
    check(error_of("[console]\nlanguage = 5\n").find("language is 1 (English)") != std::string::npos, "a language not on the disc");
    check(error_of("[console]\nsound = surround\n").find("sound is mono or stereo") != std::string::npos, "a sound mode that isn't");
    check(error_of("[controls]\nrumble = maybe\n").find("rumble is true or false") != std::string::npos, "a boolean that isn't");
    check(error_of("[display]\nanisotropy = 3\n").find("anisotropy is 1, 2, 4, 8 or 16") != std::string::npos, "anisotropy that isn't a power of 2");
    check(error_of("[display]\nmsaa = 3\n").find("msaa is 1, 2, 4 or 8") != std::string::npos, "msaa that isn't a power of 2");
    check(error_of("[display]\nmsaa = 16\n").find("msaa is 1, 2, 4 or 8") != std::string::npos, "msaa beyond 8");
    check(error_of("[display]\nmsaa = 0\n").find("msaa is 1, 2, 4 or 8") != std::string::npos, "msaa of 0");
    check(error_of("[controls]\nvolume = 101\n").find("from 0 to 100") != std::string::npos, "a volume out of range");
    check(error_of("scale = 2\n").rfind("line 1: 'scale' comes before any [section]", 0) == 0, "a setting outside a section");
    check(error_of("[audio]\n").rfind("line 1: there's no section [audio]", 0) == 0, "an unknown section");
    check(error_of("[controls]\na = pad:A\na = pad:B\n").rfind("line 3: 'a' is set twice", 0) == 0, "a control set twice");
    check(error_of("[controls]\r\na = pad:A\r\n").empty(), "Windows line endings");
    check(error_of("\xEF\xBB\xBF[controls]\na = pad:A\n").empty(), "UTF-8 BOM from Windows editors");
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / std::filesystem::path(u8"dreamcast_settings_保_é");
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const tc::Settings s = tc::load_settings(dir);
        check(same(s, d) && std::filesystem::exists(dir / (QILIN_APP ".ini")), "a missing file is written with the defaults");
        tc::Settings changed = s;
        changed.look = host::Look::crt;
        check(tc::save_settings(dir, changed) && tc::load_settings(dir).look == host::Look::crt, "saving, then loading, keeps a change");
        for (unsigned i = 0; i < 12; ++i) {
            changed.volume = i;
            check(tc::save_settings(dir, changed) && tc::load_settings(dir).volume == i, "repeated settings replacement");
        }
        const auto file = tc::settings_file(dir);
        std::filesystem::remove(file);
        std::filesystem::create_directory(file);
        bool refused = false;
        try { (void)tc::load_settings(dir); } catch (const std::runtime_error&) { refused = true; }
        check(refused && std::filesystem::is_directory(file), "unreadable existing settings are not replaced with defaults");
        std::filesystem::remove_all(dir);
    }
    std::printf("%s\n", g_failures ? "FAILED" : "all passed");
    return g_failures ? 1 : 0;
}
