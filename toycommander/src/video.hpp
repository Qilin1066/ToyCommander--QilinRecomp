// The port's screen: the window (or, headless, an image), the renderer, and
// what joins them to the PowerVR2's registers. STARTRENDER draws the scene
// the tile accelerator has taken into its frame buffer; the vertical blank
// shows the frame buffer the display reads.
#pragma once

#include "maple.hpp"

namespace toycommander::video {

// Opens the window and creates the renderer. QILIN_HEADLESS=1: no window (for
// machines without a display, and tests). QILIN_SCALE=<n>: host pixels per
// game pixel (default 2). QILIN_SHOT=<folder>[,<every>]: writes every n-th
// picture (default 60) there as frame_<number>.ppm.
void start();

// STARTRENDER: draws the scene into its frame buffer.
void render();

// The vertical blank: shows the frame buffer the display reads, if anything
// was drawn since the last one shown; and polls the window.
void vblank();

// The controls on a Maple port (0-3): port A is the host's first pad and the
// keyboard (arrows the d-pad, Enter Start, Z A, X B, A X, S Y, Q and W the
// left and right triggers); the others are empty. As of the last poll.
maple::Controls controls(unsigned port);

// The window's events, from the thread that opened it (a call from any other
// does nothing). Ends the program when the player closes the window.
void poll();

}  // namespace toycommander::video
