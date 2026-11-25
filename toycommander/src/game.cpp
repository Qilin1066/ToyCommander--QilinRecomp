// What Toy Commander's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
#include "game.hpp"

#include "chain.hpp"
#include "natives.hpp"

namespace dreamcast::game {

uint32_t interrupt_handler(uint32_t offset) { return toycommander::interrupt_handler(offset); }
void raise_bus_event(qilin::rt::Context& c, uint32_t event) { shinobi::raise_bus_event(c, event); }
void set_console(unsigned language, bool stereo) { toycommander::set_console(language, stereo); }
void on_sound_mode(void (*save)(bool stereo)) { toycommander::on_sound_mode(save); }
// The factor the projection reads (1, or what the screen mode switch, a native, widened it by).
float widening() { return qilin::rt::flt(qilin::rt::ld32(0x8C0A4120u)); }

// What the game's screen mode switch (0x8C018F42) does when its option
// changes, to any widening rather than its 4/3: the view of each of the four
// camera records in use (0x8C0D5904, 0x26C bytes each, in use when its first
// word has any of bits 0, 1, 3; the scaler 0x8C03D6C2) scaled at +40 and +84,
// one more value always (0x8C0C766C) and another while 0x8C0A4110's bit 3 is
// set (0x8C0C7408), the factor the projection reads and that new cameras take
// (0x8C0A4120) and the state word (0x8C0A410C).
void widen_to(float target) {
    using namespace qilin::rt;
    constexpr uint32_t kState = 0x8C0A410Cu, kFlags = 0x8C0A4110u, kFactor = 0x8C0A4120u;
    constexpr uint32_t kCameras = 0x8C0D5904u, kCameraSize = 0x26Cu, kAlways = 0x8C0C766Cu, kSometimes = 0x8C0C7408u;
    const float applied = flt(ld32(kFactor));
    if (target == applied || !(applied > 0.0f)) return;
    const float ratio = target / applied;
    const auto scale = [ratio](uint32_t at) { st32(at, bits(flt(ld32(at)) * ratio)); };
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t camera = kCameras + k * kCameraSize;
        if (ld32(camera) & 11u) scale(camera + 40), scale(camera + 84);
    }
    scale(kAlways);
    if (ld32(kFlags) & 8u) scale(kSometimes);
    st32(kState, target > 1.0f ? 1u : 0u);
    st32(kFactor, bits(target));
}
bool movie_playing() { return toycommander::movie_playing(); }

}  // namespace dreamcast::game
