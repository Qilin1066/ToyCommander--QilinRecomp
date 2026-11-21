// The port's screen: the window (or, headless, an image), the renderer, and
// what joins them to the PowerVR2's registers. STARTRENDER draws the scene
// the tile accelerator has taken into its frame buffer; the vertical blank
// shows the frame buffer the display reads.
#pragma once

#include "maple.hpp"

namespace dreamcast::video {

// Reads the settings (<game>.ini in the data folder), opens the window
// and creates the renderer and the settings menu. QILIN_HEADLESS=1: no window
// and no menu (for machines without a display, and tests). QILIN_SCALE=<n>:
// host pixels per game pixel (the settings' internal_scale otherwise). QILIN_SHOT=<folder>[,<every>]: writes every n-th
// picture (default 60) there as frame_<number>.ppm.
void start();

// STARTRENDER: draws the scene into its frame buffer.
void render();

// The vertical blank: shows the frame buffer the display reads, if anything
// was drawn since the last one shown; polls the window; runs the settings
// menu (F1 or Esc), where the game parks while it's open.
void vblank();

// The controls on a Maple port (0-3): port A is the host's first pad and the
// keyboard, as the settings bind them (settings.hpp: by default the pad's
// buttons by name, the arrows the stick, I J K L the d-pad, Z A, X B, A X,
// S Y, Enter Start, Q and W the triggers); ports B to D are the host's other
// pads, as they are connected (their buttons as the settings bind port A's
// pad buttons). As of the last poll; nothing while the settings menu is open.
maple::Controls controls(unsigned port);

// The window's events, from the thread that opened it (a call from any other
// does nothing). Ends the program when the player closes the window.
void poll();

}  // namespace dreamcast::video
