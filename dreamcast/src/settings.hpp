// The settings file: <game>.ini (QILIN_APP, e.g. toycommander.ini) in the game's data folder
// (host/paths.hpp), as the other Qilin recomps keep theirs: [display],
// [controls] and [console] sections, written with every key and its
// comments the first time the game runs. The settings menu (F1 or Esc in the
// game) changes them and saves them back; the file can be edited by hand
// too. A preset sets the enhancements' defaults and any key after it
// overrides one. A line that can't be read stops the program, naming the
// line, rather than being ignored. (From Punch-Out's, kept in step.)
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "qilin/host/picture.hpp"

namespace dreamcast {

// The Dreamcast controller's controls on port A. The pad's stick and
// triggers also work it in full (their own range); these are what digital
// inputs press: the stick's four ways (all the way) and the triggers (all
// the way in).
enum Control : unsigned {
    kCtlA, kCtlB, kCtlX, kCtlY, kCtlStart, kCtlUp, kCtlDown, kCtlLeft, kCtlRight,
    kCtlStickUp, kCtlStickDown, kCtlStickLeft, kCtlStickRight, kCtlL, kCtlR, kControlCount
};
const char* control_name(Control c);   // as the file names it: "stick_up"
const char* control_label(Control c);  // as the menu shows it: "Stick up"

// One input that presses a control.
struct Binding {
    enum Kind { kPad, kKey, kMouse } kind = kPad;
    uint32_t pad = 0;  // kPad: a qilin::host::PadButton bit
    std::string key;   // kKey: the key's name, as SDL names keys ("X", "Space", "Left")
    bool operator==(const Binding& o) const { return kind == o.kind && pad == o.pad && key == o.key; }
};
// As the file writes it ("pad:A", "key:Space", "mouse"), and as the menu
// shows it ("Pad A", "Space", "Mouse").
std::string binding_text(const Binding& b);
std::string binding_label(const Binding& b);
// The pad buttons that can be bound, by the file's names.
const char* pad_button_name(uint32_t bit);  // null if it isn't one
// Keys kept for the program: Escape and F1 (the menu), F3 (frame times), F11 (full screen).
bool key_reserved(const std::string& name);

struct Settings {
    // [display]
    std::string preset = "original";  // original, low-end, desktop
    bool fullscreen = false;          // borderless, at the desktop's mode (F11 or Alt+Enter switch it)
    std::string monitor;              // the display's name (empty: the main one)
    qilin::host::Scaling scaling = qilin::host::Scaling::fit;
    bool smooth = true;               // bilinear; off: nearest pixel
    qilin::host::Look look = qilin::host::Look::plain;
    unsigned scanlines = 60;          // CRT: the gaps between raster lines, 0-100 (percent dark)
    unsigned mask = 30;               // CRT: the phosphor stripes, 0-100
    unsigned scale = 1;               // internal_scale: 1-8x the console's 640x480; 0: auto (the screen's height)
    unsigned anisotropy = 1;          // filtered textures' filtering: 1 (the console's), 2, 4, 8 or 16
    bool vsync = true;                // at the display's refresh (no tearing); off: the lowest latency
    std::string gpu;                  // the graphics device's name (empty: the best one)
    bool performance_overlay = false; // frame rate, 1% and 0.1% lows and frame times in a corner (F3 switches it)
    bool match_refresh = true;        // fullscreen: a refresh that's a multiple of 60 Hz when the desktop's isn't
    bool variable_refresh = false;    // a G-Sync/FreeSync/VRR display: no mode switch, no locking to the refresh
    // [controls]
    bool rumble = true;  // the Jump Pack's rumble on the controller
    unsigned rumble_strength = 100;  // how strong, in percent
    unsigned volume = 100;           // the sound's, in percent
    bool mute_unfocused = false;     // silent while another window has the keyboard
    std::vector<Binding> controls[kControlCount];
    // [console]: what the game reads as the Dreamcast's own settings
    unsigned language = 1;  // 1 English, 2 German, 3 French, 4 Spanish (the disc's)
    bool stereo = true;     // false: mono
};

Settings default_settings();
// Sets a preset's enhancements (and its name); false for an unknown one.
bool apply_preset(Settings& s, const std::string& name);
// The picture as the display takes it.
qilin::host::Picture picture_of(const Settings& s);

// Read settings from text. On a problem: false, with `error` saying what
// and where ("line 12: ...").
bool parse_settings(const std::string& text, Settings& out, std::string& error);

// The file's text for the given settings, with comments explaining each part.
std::string settings_text(const Settings& s);

// <game>.ini in `folder`: read it, or write the defaults there if it's
// missing. A file that doesn't read stops the program.
Settings load_settings(const std::filesystem::path& folder);
// Write it back (through a temporary file, so a failure leaves the old one);
// false if it can't be written.
bool save_settings(const std::filesystem::path& folder, const Settings& s);
std::filesystem::path settings_file(const std::filesystem::path& folder);

}  // namespace dreamcast
