// The settings menu over the game, as the other Qilin recomps have it (from
// Punch-Out's, kept in step): F1 or Escape (a controller: Back and Start
// together) open and close it. It has Controls, Display and Game pages;
// while it's open the game is paused (video.hpp: the guest parks at a
// vertical blank and its clock stops), and closing it saves the settings. A
// hint says how to open it for the first seconds of a run.
//
// Controls are set by pressing them: Add on a control's row, then the key,
// the pad's button or a click. The picture's settings show at once
// (Look at the picture hides the menu until any key or button); the
// internal resolution and the graphics device wait for the next start.
//
// Dear ImGui (MIT) with its SDL3 backend for input and its Vulkan backend
// drawing over the picture (display.hpp's overlay). Everything here runs on
// the main thread. Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "qilin/host/display.hpp"
#include "save_history.hpp"
#include "settings.hpp"

namespace dreamcast {

class Menu {
public:
    // Without a window there is no menu (ok() is false); the game runs as before.
    Menu(qilin::host::Display& display, Settings& settings, std::filesystem::path folder);
    ~Menu();
    Menu(const Menu&) = delete;
    Menu& operator=(const Menu&) = delete;

    bool ok() const { return ok_; }
    // Open and showing: the main loop shows the paused picture under it
    // (present_again) and gives the game no input.
    bool is_open() const { return open_; }
    // Once per pass of the main loop, after the display's poll: opens once
    // the game has parked, builds the menu's (or the hint's) drawing.
    void tick();
    bool quit_requested() const { return quit_; }
    // Whether the controls changed since the last call (the main loop's
    // table of them is rebuilt).
    bool take_controls_changed();
    // "Reload texture packs" was pressed since the last call.
    bool take_reload_textures();
    // The game has parked (or is running again).
    void set_parked(bool parked) { parked_ = parked; }
    // Asked to open: the game should park (video.cpp), then tick() opens it.
    bool wants_pause() const { return opening_ || open_; }
    // The internal resolution the game runs at (a change waits for the next start).
    void set_running_scale(unsigned scale) { running_scale_ = scale; }
    // Lines under the frame-time graph (where the time goes), from the main loop.
    void set_frame_details(std::vector<std::string> lines) { frame_details_ = std::move(lines); }
    bool frame_times_shown() const;

private:
    bool handle(const void* sdl_event);
    void toggle();
    void close();
    void draw_menu();
    void controls_page();
    void display_page();
    void game_page();
    void draw_hint();
    void draw_frame_times();
    void bind(const Binding& b);

    qilin::host::Display& display_;
    Settings& s_;
    std::filesystem::path folder_;
    bool ok_ = false, opening_ = false, open_ = false, quit_ = false, close_asked_ = false;
    bool sdl_backend_ = false, vulkan_backend_ = false, parked_ = false;
    bool hint_ = false, drawn_ = false, peek_ = false, controls_changed_ = false, reload_textures_ = false;
    std::vector<std::string> frame_details_;
    uint64_t hint_until_ = 0;  // SDL ticks (ms)
    // Setting a control by pressing it.
    int capture_ = -1;          // the Control being set, or -1
    uint64_t capture_until_ = 0;
    uint32_t capture_held_ = 0;  // the pad's buttons held when it began (they don't count)
    bool wait_release_ = false;  // after a pad button was taken: gamepad navigation back once it's let go
    bool history_loaded_ = false;
    std::vector<SaveBackup> save_history_;
    std::string note_;           // said under the table (what moved where)
    unsigned running_scale_ = 1;
};

}  // namespace dreamcast
