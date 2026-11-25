// What Toy Racer's port gives the shared Dreamcast code (dreamcast/src/game.hpp).
#include "game.hpp"

#include "chain.hpp"
#include "library.hpp"
#include "natives.hpp"

namespace dreamcast::game {

uint32_t interrupt_handler(uint32_t offset) { return shinobi::interrupt_handler(offset); }
void raise_bus_event(qilin::rt::Context& c, uint32_t event) { shinobi::raise_bus_event(c, event); }
void set_console(unsigned language, bool stereo) { shinobi::set_console(language, stereo); }
void on_sound_mode(void (*save)(bool stereo)) { shinobi::on_sound_mode(save); }
// The factor the projection reads (1, or what the screen mode switch, a native, widened it by).
float widening() { return qilin::rt::flt(qilin::rt::ld32(0x8C0ADDD0u)); }

// What the game's screen mode switch (0x8C01AC4E) does when its option
// changes, to any widening rather than its 4/3: the view of each of the four
// camera records in use (0x8C109124, 0x438 bytes each, in use when its first
// word's bit 0 is set; the scaler 0x8C02BAD0) scaled at +72 and +156, one
// more value while 0x8C0ADDC0's bit 2 is set (0x8C1045D0), the factor the
// projection reads and that new cameras take (0x8C0ADDD0) and the state word
// (0x8C0ADDBC).
void widen_to(float target) {
    using namespace qilin::rt;
    constexpr uint32_t kState = 0x8C0ADDBCu, kFlags = 0x8C0ADDC0u, kFactor = 0x8C0ADDD0u;
    constexpr uint32_t kCameras = 0x8C109124u, kCameraSize = 0x438u, kSometimes = 0x8C1045D0u;
    const float applied = flt(ld32(kFactor));
    if (target == applied || !(applied > 0.0f)) return;
    const float ratio = target / applied;
    const auto scale = [ratio](uint32_t at) { st32(at, bits(flt(ld32(at)) * ratio)); };
    for (uint32_t k = 0; k < 4; ++k) {
        const uint32_t camera = kCameras + k * kCameraSize;
        if (ld32(camera) & 1u) scale(camera + 72), scale(camera + 156);
    }
    if (ld32(kFlags) & 4u) scale(kSometimes);
    st32(kState, target > 1.0f ? 1u : 0u);
    st32(kFactor, bits(target));
}
// Toy Racer has no movies (its logos are pictures it draws).
bool movie_playing() { return false; }

}  // namespace dreamcast::game
