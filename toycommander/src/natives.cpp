// Native replacements for guest functions (rt::provide). Each one does what
// its guest code does, as analysis of that code shows (docs/code-analysis.md),
// with what reaches hardware done the host's way. The natives both games
// share are in shinobi/src/library.cpp; these are Toy Commander's own.
#include "natives.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "game.hpp"
#include "system.hpp"
#include "video.hpp"

namespace toycommander {

using namespace dreamcast;

namespace rt = qilin::rt;

namespace {

// 0x8C0B487A, Shinobi's exit: on the console, interrupts masked, its
// libraries shut down and a jump to the boot ROM's start (its menu). The
// port's equivalent is to quit.
void shinobi_exit(rt::Context&) {
    rt::log("the game exited to the console's menu (Shinobi's exit, 0x8C0B487A)");
    std::fflush(nullptr);
    std::_Exit(0);
}

// 0x8C0BADE0, Shinobi's boot of the next program: with a program named
// (0x8C0BCFE0 puts the name at 0x8C0080F0 and sets 0x8C0080FC), the boot
// ROM loads and runs it; without, it returns to its menu. The port runs one
// program, this one: a name stops, naming it; no name quits.
void shinobi_boot_next(rt::Context& c) {
    if (rt::ld8(0x8C0080FCu) == 1) throw rt::Stop{rt::Stop::Unsupported, 0x8C0BADE0u, rt::ld32(0x8C0080F0u)};
    shinobi_exit(c);
}

// 0x8C0C1078, 0x8C0C108C, 0x8C0C10A0: the boot ROM's font (its address, and
// locking and unlocking it). The font is the boot ROM's own, and the port has
// no boot ROM: stop if the game ever draws with it.
void rom_font(rt::Context&) { throw rt::Stop{rt::Stop::Unsupported, 0x8C0C1078u, 0}; }

// 0x8C0B27E4: sets up a three-word record at r4 as {0, r5, 0} and returns
// r4 (the middleware gives it a 0x400-byte block it just allocated). r6 ends 0.
void shinobi_record_init_8C0B27E4(rt::Context& c) {
    rt::st32(c.r[4], 0), rt::st32(c.r[4] + 4, c.r[5]), rt::st32(c.r[4] + 8, 0);
    c.r[6] = 0, c.r[0] = c.r[4];
}

// 0x8C04C700: plays a movie, r4 its file's name, to its end or until the
// player skips it; returns in r0 whether it was skipped (0x8C0DF1F4, which
// its input callback sets). A native only so the port knows a movie is on
// screen: the movies are 4:3 pictures, shown at 4:3 even in the game's 16:9
// mode (game::movie_playing). Otherwise it does what the guest code does,
// call for call, in the same frame (0x254 bytes below the saved PR): the
// busy count up (0x8C04F482), the skip flag cleared, the player's record at
// sp + 4 set up with its callbacks (0x8C06D268), the name copied into it
// (0x8C067E90, its arguments in r0 and r1), its buffer size and mode
// (0x100000, 6) stored, the movie played (0x8C06D3E2), the busy count down
// (0x8C04F4D8).
std::atomic<int> g_movies{0};
void play_movie(rt::Context& c) {
    constexpr uint32_t kFrame = 0x254, kSkipped = 0x8C0DF1F4u;
    const uint32_t name = c.r[4], pr = c.pr;
    c.r[15] -= 4;
    rt::st32(c.r[15], pr);
    c.r[15] -= kFrame;
    const uint32_t sp = c.r[15];
    rt::st32(sp, name);
    rt::call_guest(c, 0x8C04F482u);
    rt::st32(kSkipped, 0);
    c.r[4] = sp + 4, c.r[5] = 0x8C04C6ECu, c.r[6] = 0x8C06DC30u;
    rt::call_guest(c, 0x8C06D268u);
    c.r[0] = sp + 0xC0, c.r[1] = rt::ld32(sp);
    rt::call_guest(c, 0x8C067E90u);
    rt::st32(sp + 0x1E0, 0x00100000u), rt::st32(sp + 0x218, 6);
    c.r[4] = sp + 4;
    ++g_movies;
    rt::call_guest(c, 0x8C06D3E2u);
    --g_movies;
    rt::call_guest(c, 0x8C04F4D8u);
    c.r[0] = rt::ld32(kSkipped);
    c.r[15] = sp + kFrame;
    c.pr = rt::ld32(c.r[15]);
    c.r[15] += 4;
}

// The screen mode switch (0x8C018F42): the cameras widened for the screen,
// not for the game's option (game.cpp's widen_to).
void screen_mode_switch(rt::Context&) { dreamcast::game::widen_to(dreamcast::video::screen_widening()); }

const rt::Provided kNatives[] = {
    {0x8C04C700u, "play_movie", play_movie},
    {0x8C018F42u, "screen_mode_switch", screen_mode_switch},
    {0x8C0B487Au, "shinobi_exit", shinobi_exit},
    {0x8C0BADE0u, "shinobi_boot_next", shinobi_boot_next},
    {0x8C0C1078u, "rom_font_address", rom_font},
    {0x8C0C108Cu, "rom_font_lock", rom_font},
    {0x8C0C10A0u, "rom_font_unlock", rom_font},
    {0x8C0B27E4u, "shinobi_record_init_8C0B27E4", shinobi_record_init_8C0B27E4},
};

}  // namespace

void provide_natives() { rt::provide(kNatives); }
bool movie_playing() { return g_movies.load(std::memory_order_relaxed) > 0; }

}  // namespace toycommander
