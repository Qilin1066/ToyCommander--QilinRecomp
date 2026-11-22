// The settings file (settings.hpp), from Punch-Out's, kept in step.
#include "settings.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <system_error>

#include "qilin/host/pad.hpp"
#include "system.hpp"

namespace dreamcast {
namespace host = qilin::host;

namespace {

constexpr const char* kControlNames[kControlCount] = {"a", "b", "x", "y", "start", "up", "down", "left", "right",
                                                      "stick_up", "stick_down", "stick_left", "stick_right", "l", "r"};
constexpr const char* kControlLabels[kControlCount] = {
    "A", "B", "X", "Y", "Start", "D-pad up", "D-pad down", "D-pad left", "D-pad right",
    "Stick up", "Stick down", "Stick left", "Stick right", "L trigger", "R trigger"};
struct PadName {
    const char* name;   // in the file
    const char* label;  // in the menu
    uint32_t bit;
};
constexpr PadName kPadNames[] = {
    {"Up", "Pad Up", host::kPadUp},       {"Down", "Pad Down", host::kPadDown},  {"Left", "Pad Left", host::kPadLeft},
    {"Right", "Pad Right", host::kPadRight}, {"A", "Pad A", host::kPadA},           {"B", "Pad B", host::kPadB},
    {"X", "Pad X", host::kPadX},          {"Y", "Pad Y", host::kPadY},           {"Start", "Pad Start", host::kPadStart},
    {"Back", "Pad Back", host::kPadBack}, {"Guide", "Pad Guide", host::kPadGuide},
    {"LB", "Pad LB", host::kPadLB},       {"RB", "Pad RB", host::kPadRB},        {"LT", "Pad LT", host::kPadLT},
    {"RT", "Pad RT", host::kPadRT},       {"L3", "Pad L3", host::kPadL3},        {"R3", "Pad R3", host::kPadR3},
};

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool parse_binding(const std::string& text, Binding& b, std::string& why) {
    const std::string t = trim(text);
    const std::string l = lower(t);
    if (l == "mouse") {
        b = {Binding::kMouse, 0, {}};
        return true;
    }
    const size_t colon = t.find(':');
    if (colon == std::string::npos) {
        why = "'" + t + "' isn't an input: write pad:<button>, key:<key> or mouse";
        return false;
    }
    const std::string kind = lower(trim(t.substr(0, colon))), name = trim(t.substr(colon + 1));
    if (kind == "pad") {
        for (const PadName& p : kPadNames)
            if (lower(name) == lower(p.name)) {
                b = {Binding::kPad, p.bit, {}};
                return true;
            }
        why = "'" + name + "' isn't a pad button (Up Down Left Right A B X Y Start Back Guide LB RB LT RT L3 R3)";
        return false;
    }
    if (kind == "key") {
        if (name.empty()) {
            why = "key: needs a key's name";
            return false;
        }
        if (key_reserved(name)) {
            why = "key:" + name + " is kept for the program (Escape and F1 open the settings menu, F3 the frame times, F11 is full screen)";
            return false;
        }
        b = {Binding::kKey, 0, name};
        return true;
    }
    why = "'" + kind + "' isn't a kind of input (pad, key, mouse)";
    return false;
}

Binding pad(uint32_t bit) { return {Binding::kPad, bit, {}}; }
Binding key(const char* name) { return {Binding::kKey, 0, name}; }

const char* scaling_name(host::Scaling s) {
    switch (s) {
        case host::Scaling::integer: return "integer";
        case host::Scaling::stretch: return "stretch";
        case host::Scaling::fit: break;
    }
    return "fit";
}

}  // namespace

const char* control_name(Control c) { return kControlNames[c]; }
const char* control_label(Control c) { return kControlLabels[c]; }

const char* pad_button_name(uint32_t bit) {
    for (const PadName& p : kPadNames)
        if (p.bit == bit) return p.name;
    return nullptr;
}

bool key_reserved(const std::string& name) {
    const std::string l = lower(trim(name));
    return l == "escape" || l == "f1" || l == "f3" || l == "f11";
}

std::string binding_text(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "mouse";
        case Binding::kKey: return "key:" + b.key;
        case Binding::kPad:
            if (const char* n = pad_button_name(b.pad)) return std::string("pad:") + n;
    }
    return "?";
}

std::string binding_label(const Binding& b) {
    switch (b.kind) {
        case Binding::kMouse: return "Mouse";
        case Binding::kKey: return b.key;
        case Binding::kPad:
            for (const PadName& p : kPadNames)
                if (p.bit == b.pad) return p.label;
    }
    return "?";
}

Settings default_settings() {
    Settings s;
    s.controls[kCtlA] = {pad(host::kPadA), key("Z")};
    s.controls[kCtlB] = {pad(host::kPadB), key("X")};
    s.controls[kCtlX] = {pad(host::kPadX), key("A")};
    s.controls[kCtlY] = {pad(host::kPadY), key("S")};
    s.controls[kCtlStart] = {pad(host::kPadStart), key("Return")};
    s.controls[kCtlUp] = {pad(host::kPadUp), key("I")};
    s.controls[kCtlDown] = {pad(host::kPadDown), key("K")};
    s.controls[kCtlLeft] = {pad(host::kPadLeft), key("J")};
    s.controls[kCtlRight] = {pad(host::kPadRight), key("L")};
    s.controls[kCtlStickUp] = {key("Up")};
    s.controls[kCtlStickDown] = {key("Down")};
    s.controls[kCtlStickLeft] = {key("Left")};
    s.controls[kCtlStickRight] = {key("Right")};
    s.controls[kCtlL] = {pad(host::kPadLB), key("Q")};
    s.controls[kCtlR] = {pad(host::kPadRB), key("W")};
    return s;
}

bool apply_preset(Settings& s, const std::string& name) {
    if (name == "original" || name == "low-end") {  // the console's picture: 1x, its filtering (a Pi 4 does exactly this)
        s.scale = 1, s.anisotropy = 1, s.vsync = true;
    } else if (name == "desktop") {  // a desktop GPU: the screen's height (auto), 16x anisotropic
        s.scale = 0, s.anisotropy = 16, s.vsync = true;
    } else {
        return false;
    }
    s.preset = name;
    return true;
}

host::Picture picture_of(const Settings& s) {
    host::Picture p;
    p.scaling = s.scaling;
    p.smooth = s.smooth;
    p.look = s.look;
    p.scanlines = float(s.scanlines) / 100.0f;
    p.mask = float(s.mask) / 100.0f;
    p.vsync = s.vsync;
    p.match_refresh = s.match_refresh;
    p.variable_refresh = s.variable_refresh;
    return p;
}

bool parse_settings(const std::string& text, Settings& out, std::string& error) {
    Settings s = default_settings();
    std::istringstream in(text);
    std::string line, section;
    bool seen[kControlCount] = {};
    for (int n = 1; std::getline(in, line); ++n) {
        const auto fail = [&](const std::string& why) {
            error = "line " + std::to_string(n) + ": " + why;
            return false;
        };
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        line = trim(line);
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') return fail("a section is written [name]");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (section != "display" && section != "controls" && section != "console")
                return fail("there's no section [" + section + "] (display, controls, console)");
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) return fail("expected name = value");
        const std::string name = lower(trim(line.substr(0, eq))), value = trim(line.substr(eq + 1));
        if (section.empty()) return fail("'" + name + "' comes before any [section]");
        const std::string v = lower(value);
        bool ok = true;
        const auto boolean = [&](bool& to) {
            if (v == "true") to = true;
            else if (v == "false") to = false;
            else ok = false;
            return ok ? true : fail(name + " is true or false");
        };
        const auto number = [&](unsigned& to, unsigned lo, unsigned hi) {
            unsigned x = 0;
            ok = !v.empty() && v.size() <= 4;
            for (char ch : v) ok = ok && std::isdigit(static_cast<unsigned char>(ch)), x = x * 10 + unsigned(ch - '0');
            if (!ok || x < lo || x > hi) return fail(name + " is a whole number from " + std::to_string(lo) + " to " + std::to_string(hi));
            to = x;
            return true;
        };
        if (section == "display") {
            if (name == "preset") {
                if (!apply_preset(s, v)) return fail("the presets are original, low-end and desktop");
            } else if (name == "fullscreen") {
                if (!boolean(s.fullscreen)) return false;
            } else if (name == "monitor") {
                s.monitor = value;
            } else if (name == "scaling") {
                if (v == "integer") s.scaling = host::Scaling::integer;
                else if (v == "fit") s.scaling = host::Scaling::fit;
                else if (v == "stretch") s.scaling = host::Scaling::stretch;
                else return fail("scaling is integer, fit or stretch");
            } else if (name == "smooth") {
                if (!boolean(s.smooth)) return false;
            } else if (name == "look") {
                if (v == "plain") s.look = host::Look::plain;
                else if (v == "crt") s.look = host::Look::crt;
                else return fail("look is plain or crt");
            } else if (name == "scanlines") {
                if (!number(s.scanlines, 0, 100)) return false;
            } else if (name == "mask") {
                if (!number(s.mask, 0, 100)) return false;
            } else if (name == "internal_scale") {
                if (value == "auto" || value == "0") s.scale = 0;  // auto
                else if (!number(s.scale, 1, 8)) return false;
            } else if (name == "anisotropy") {
                if (!number(s.anisotropy, 1, 16)) return false;
                if (s.anisotropy & (s.anisotropy - 1)) return fail("anisotropy is 1, 2, 4, 8 or 16");
            } else if (name == "vsync") {
                if (!boolean(s.vsync)) return false;
            } else if (name == "gpu") {
                s.gpu = value;
            } else if (name == "performance_overlay") {
                if (!boolean(s.performance_overlay)) return false;
            } else if (name == "match_refresh") {
                if (!boolean(s.match_refresh)) return false;
            } else if (name == "variable_refresh") {
                if (!boolean(s.variable_refresh)) return false;
            } else {
                return fail("[display] has no '" + name +
                            "' (preset fullscreen monitor scaling smooth look scanlines mask internal_scale anisotropy vsync gpu "
                            "performance_overlay match_refresh variable_refresh)");
            }
            continue;
        }
        if (section == "console") {
            if (name == "language") {
                if (!number(s.language, 1, 4)) return fail("language is 1 (English), 2 (German), 3 (French) or 4 (Spanish)");
            } else if (name == "sound") {
                if (v == "mono") s.stereo = false;
                else if (v == "stereo") s.stereo = true;
                else return fail("sound is mono or stereo");
            } else {
                return fail("[console] has no '" + name + "' (language sound)");
            }
            continue;
        }
        // [controls]
        if (name == "rumble") {
            if (!boolean(s.rumble)) return false;
            continue;
        }
        if (name == "rumble_strength") {
            if (!number(s.rumble_strength, 0, 100)) return false;
            continue;
        }
        if (name == "volume") {
            if (!number(s.volume, 0, 100)) return false;
            continue;
        }
        if (name == "mute_unfocused") {
            if (!boolean(s.mute_unfocused)) return false;
            continue;
        }
        unsigned c = 0;
        while (c < kControlCount && name != kControlNames[c]) ++c;
        if (c == kControlCount)
            return fail("'" + name + "' isn't a control (a b x y start up down left right stick_up stick_down stick_left stick_right l r), rumble, rumble_strength, volume or mute_unfocused");
        if (seen[c]) return fail("'" + name + "' is set twice");
        seen[c] = true;
        s.controls[c].clear();
        std::istringstream items(value);
        for (std::string item; std::getline(items, item, ',');) {
            if (trim(item).empty()) continue;
            Binding b;
            std::string why;
            if (!parse_binding(item, b, why)) return fail(why);
            s.controls[c].push_back(b);
        }
    }
    out = std::move(s);
    return true;
}

std::string settings_text(const Settings& s) {
    const auto b = [](bool v) { return v ? "true" : "false"; };
    std::string t =
        "# " QILIN_APP_TITLE " QilinRecomp settings. The settings menu (F1 or Esc in the game) changes these and saves\n"
        "# them here; any text editor works too. Delete the file to get the defaults back.\n"
        "# Each line is name = value; # starts a comment.\n"
        "\n"
        "[display]\n"
        "# A preset sets the enhancements below it: original (the console's picture), low-end (a Raspberry Pi\n"
        "# 4-class board), desktop (the screen's resolution). Keys after it override it.\n"
        "preset = " + s.preset + "\n"
        "# Borderless full screen (F11 or Alt+Enter switch it), and on which display (its name; empty: the main one).\n"
        "fullscreen = " + b(s.fullscreen) + "\n"
        "monitor = " + s.monitor + "\n"
        "# The picture's size: integer (whole multiples of the console's, the sharpest), fit (the largest that\n"
        "# keeps its shape) or stretch (the whole window). smooth: filtered; false: nearest pixel.\n"
        "scaling = " + scaling_name(s.scaling) + "\n"
        "smooth = " + b(s.smooth) + "\n"
        "# Its look: plain, or crt (a CRT television: raster lines and phosphor stripes). For crt, how dark the\n"
        "# gaps between lines are and how strong the stripes, 0 to 100.\n"
        "look = " + (s.look == host::Look::crt ? "crt" : "plain") + "\n"
        "scanlines = " + std::to_string(s.scanlines) + "\n"
        "mask = " + std::to_string(s.mask) + "\n"
        "# The game's resolution, as a multiple of the Dreamcast's 640x480: 1 to 8, or auto (the smallest that\n"
        "# fills the screen line for line). Higher is sharper and needs a faster GPU: the GPU's work grows with\n"
        "# the square (8x is 64 times 1x's pixels). QILIN_SCALE in the environment overrides it.\n"
        "internal_scale = " + (s.scale ? std::to_string(s.scale) : std::string("auto")) + "\n"
        "# Anisotropic filtering of the textures the game filters: 1 (the console's), 2, 4, 8 or 16. Sharper\n"
        "# textures at a grazing angle (a floor far off); the game's unfiltered textures stay as they are.\n"
        "anisotropy = " + std::to_string(s.anisotropy) + "\n"
        "# vsync: true, no tearing; false, the lowest latency.\n"
        "vsync = " + b(s.vsync) + "\n"
        "# The graphics device, by name (empty: the best one).\n"
        "gpu = " + s.gpu + "\n"
        "# Frame rate, 1% and 0.1% lows and recent frame times in a corner (F3 switches it).\n"
        "performance_overlay = " + b(s.performance_overlay) + "\n"
        "# Fullscreen on a display whose refresh isn't a multiple of 60 Hz (75, 144, 165 Hz): switch to the\n"
        "# highest refresh at the same resolution that is (120 Hz on a 144 Hz monitor), so frames are even.\n"
        "match_refresh = " + b(s.match_refresh) + "\n"
        "# A G-Sync, FreeSync or VRR display with it on in its driver: no switch and no locking to the\n"
        "# refresh; the display follows the game's 60 frames a second.\n"
        "variable_refresh = " + b(s.variable_refresh) + "\n"
        "\n"
        "[controls]\n"
        "# The Jump Pack's rumble, on the controller.\n"
        "rumble = " + b(s.rumble) + "\n"
        "# How strong it is, 0 to 100.\n"
        "rumble_strength = " + std::to_string(s.rumble_strength) + "\n"
        "# The sound's volume, 0 to 100, and whether it's silent while another window has the keyboard.\n"
        "volume = " + std::to_string(s.volume) + "\n"
        "mute_unfocused = " + b(s.mute_unfocused) + "\n"
        "# The Dreamcast controller on port A. A gamepad's left stick and triggers work its stick and triggers\n"
        "# in full; the inputs below press its buttons, its stick all the way or its triggers all the way in.\n"
        "# Each control takes a list of inputs, separated by commas; an empty list leaves it unused. The\n"
        "# settings menu's Controls page sets them by pressing them.\n"
        "#   pad:<button>  a gamepad's button by an Xbox pad's names: Up Down Left Right A B X Y Start Back Guide\n"
        "#                 LB RB LT RT L3 R3\n"
        "#   key:<name>    a key, as SDL names it: A-Z, 0-9, Up, Down, Left, Right, Space, Return, Backspace,\n"
        "#                 Tab, Left Shift, Right Ctrl, F2 ... (not Escape, F1, F3 or F11: they're the program's)\n"
        "#   mouse         the mouse's left button\n";
    for (unsigned c = 0; c < kControlCount; ++c) {
        t += kControlNames[c];
        t += " =";
        for (size_t i = 0; i < s.controls[c].size(); ++i) t += (i ? ", " : " ") + binding_text(s.controls[c][i]);
        t += "\n";
    }
    t += "\n"
         "[console]\n"
         "# What the game reads as the Dreamcast's own settings, from the next start.\n"
         "# language: 1 English, 2 German, 3 French, 4 Spanish.\n"
         "language = " + std::to_string(s.language) + "\n"
         "# sound: mono or stereo.\n"
         "sound = " + (s.stereo ? "stereo" : "mono") + "\n";
    return t;
}

std::filesystem::path settings_file(const std::filesystem::path& folder) { return folder / (QILIN_APP ".ini"); }

bool save_settings(const std::filesystem::path& folder, const Settings& s) {
    const std::filesystem::path file = settings_file(folder), tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << settings_text(s);
        if (!out.flush()) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, file, ec);
    return !ec;
}

Settings load_settings(const std::filesystem::path& folder) {
    const std::filesystem::path file = settings_file(folder);
    std::ifstream in(file, std::ios::binary);
    if (in) {
        std::stringstream buf;
        buf << in.rdbuf();
        Settings s;
        std::string error;
        if (!parse_settings(buf.str(), s, error))
            qilin::rt::fatal("%s, %s (fix it, or delete the file to get the defaults back)", file.string().c_str(), error.c_str());
        qilin::rt::log("settings: read %s", file.string().c_str());
        return s;
    }
    const Settings s = default_settings();
    if (!save_settings(folder, s)) qilin::rt::fatal("cannot write %s", file.string().c_str());
    qilin::rt::log("settings: wrote the defaults to %s", file.string().c_str());
    return s;
}

}  // namespace dreamcast
