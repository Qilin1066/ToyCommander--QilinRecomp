// The window and what's shown in it: SDL3 for the window and its events,
// Vulkan to present. Headless, it draws into an image of its own instead, for
// tests and for machines without a display.
//
// The picture is drawn into the window through a look (plain, or a CRT
// television), at a size the settings choose, and anything drawn over it
// (the settings menu) after. F11 or Alt+Enter switch borderless full screen.
//
// Runtime code: no std::format (macOS 10.15).
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "qilin/host/pad.hpp"
#include "qilin/host/frame_times.hpp"
#include "qilin/host/picture.hpp"
#include "qilin/host/vulkan.hpp"

namespace qilin::host {

class Display {
public:
    struct Options {
        const char* title = QILIN_APP_TITLE;
        uint32_t width = 1280, height = 720;  // the window's, or the headless target's, size
        bool headless = false;
        bool fullscreen = false;              // borderless, at the desktop's mode
        std::string monitor;                  // the display's name; empty or missing: the main one
        Picture picture;
    };
    explicit Display(const Options& options);
    ~Display();
    Display(const Display&) = delete;
    Display& operator=(const Display&) = delete;

    // Handle the window's events. False once the player has closed it.
    bool poll();
    // The pad's buttons and the mouse's left button held now (PadButton bits),
    // as of the last poll.
    uint32_t buttons() const;
    // A key by its SDL name ("X", "Space", "Left Shift"): a code for key_down,
    // or -1 if there's no such key. Whether it's held, as of the last poll
    // (never, headless).
    int key_code(const char* name) const;
    bool key_down(int code) const;
    // The right stick, -1 to 1 each way (y up), dead zone removed.
    void right_stick(float& x, float& y) const;
    // The first pad as it is, for consoles with analogue controls: the
    // buttons (PadButton bits, the d-pad's alone: the stick isn't folded
    // into it), the sticks -1 to 1 each way (y down, as SDL gives them, no
    // dead zone) and the triggers 0 to 1. All zero with no pad.
    struct PadState {
        bool connected = false;
        uint32_t buttons = 0;
        float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
    };
    // A player's pad: port A's (0) or another player's (1-3). Every
    // controller SDL lists is opened; each takes a place when it first has a
    // button pressed (so a virtual pad or a wheel's base listed first doesn't
    // take port A from the player's real one): the first place whose
    // controller is unplugged, else the next. All zero for a place nobody
    // has. buttons() is every controller's together (the menu's).
    PadState pad_state() const;
    PadState pad_state(unsigned player) const;
    // The name of a place's controller (empty: nobody has it yet), how many
    // controllers are connected, and the places given up (each controller
    // joins again by pressing a button).
    std::string player_controller(unsigned player) const;
    unsigned controllers_connected() const;
    void reset_player_order();
    // Whether the window has the keyboard. While another window has it, the
    // buttons, keys and pads all read as nothing held, and no pad rumbles.
    bool focused() const;
    // If the mouse moved over the window since the last poll: where, over the
    // picture last presented, -1 to 1 each way (y up; beyond 1 off the picture).
    bool mouse_moved(float& x, float& y) const;
    // Rumble on the first pad, on or off.
    void rumble(bool on);
    // Rumble on a player's pad (0 the first), 0 (off) to 1 (full).
    void rumble(unsigned player, float strength);
    // Show a frame of RGBA8 pixels, at `aspect` (displayed width / height);
    // `lines`: the console picture's height in its own pixels (whole
    // multiples and the CRT's raster go by it). Black around it.
    void present(uint32_t width, uint32_t height, const uint8_t* rgba, float aspect, uint32_t lines);
    // Show an image drawn on this display's GPU (in TRANSFER_SRC_OPTIMAL
    // layout, with sampled usage and a colour view, its work submitted to the
    // same queue before this call), the same way. It's left as it was given.
    void present_image(VkImage image, VkImageView view, uint32_t width, uint32_t height, float aspect, uint32_t lines);
    // Show the last picture again (for a menu over a paused game). Not
    // counted in frame_times().
    void present_again();
    // The intervals between new pictures presented (present and
    // present_image), over the last 20 s (frame_times.hpp).
    const FrameTimes& frame_times() const;
    // Fullscreen's mode, when it isn't the desktop's (or why it couldn't be
    // even): for the log. Empty otherwise.
    const std::string& mode_note() const;
    // Since the last call: frames presented and the time spent waiting to
    // present them (for the previous one to reach the screen, and for a
    // swapchain image): with vsync, mostly the wait for the display.
    struct PresentStats {
        uint32_t presents = 0;
        double wait_ms = 0;  // in all
    };
    PresentStats take_present_stats();
    Gpu& gpu();
    // Headless: the last frame presented, as RGBA8 at the target's size.
    std::vector<uint8_t> read_back();
    const std::string& device_name() const;

    // The picture's settings, from the next present.
    void set_picture(const Picture& p);
    const Picture& picture() const;
    bool fullscreen() const;
    void set_fullscreen(bool on);
    // The displays connected, by name, and moving the window to one (empty: the main one).
    std::vector<std::string> monitors() const;
    void move_to(const std::string& monitor);

    // For a UI drawn over the picture: the SDL window (SDL_Window*), the
    // window images' format and count. Null and 0 when headless.
    void* sdl_window() const;
    VkFormat swap_format() const;
    uint32_t swap_image_count() const;
    // Called with every SDL event (an SDL_Event*) before the display's own
    // handling; true: taken, the display ignores it.
    void set_event_hook(std::function<bool(const void* sdl_event)> hook);
    // Called inside each present's drawing (dynamic rendering into the window
    // image, after the picture), to draw over it.
    void set_overlay(std::function<void(VkCommandBuffer)> overlay);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qilin::host
