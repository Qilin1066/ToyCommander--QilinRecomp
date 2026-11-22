// A controller reaches the game: a virtual Xbox-layout gamepad (SDL's own
// virtual joystick), connected before the window opens as a player's usually
// is, and one connected later. Each press must show in Display::buttons().
// Needs a window: skips (exit 77) where there's no display or no Vulkan 1.3.
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "qilin/host/display.hpp"

namespace {
int g_failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_failures;
}

SDL_JoystickID attach_pad(const char* name) {
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.vendor_id = 0x045e;  // Microsoft
    desc.product_id = 0x0b12; // Xbox Series controller
    desc.name = name;
    return SDL_AttachVirtualJoystick(&desc);
}

// Poll until the buttons are `want` (events take a pass or two to arrive).
bool reaches(qilin::host::Display& d, uint32_t want) {
    for (int i = 0; i < 50; ++i) {
        d.poll();
        if ((d.buttons() & ~uint32_t(qilin::host::kPadClick)) == want) return true;
        SDL_Delay(5);
    }
    return false;
}
}  // namespace

int main() {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::printf("padtest: no display here (%s): skipped\n", SDL_GetError());
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;  // CI under Xvfb and Mesa: a skip is a failure
    }
    // The window is Vulkan: skip where there's no Vulkan loader (a macOS
    // runner without MoltenVK), as vktest does, rather than fail to open it.
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        std::printf("padtest: no Vulkan here (%s): skipped\n", SDL_GetError());
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;  // CI under Xvfb and Mesa: a skip is a failure
    }
    SDL_Vulkan_UnloadLibrary();
    const SDL_JoystickID first = attach_pad("Virtual Xbox controller");
    if (!first) {
        std::printf("padtest: SDL has no virtual joysticks here: skipped\n");
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;  // CI under Xvfb and Mesa: a skip is a failure
    }
    SDL_Joystick* j = SDL_OpenJoystick(first);
    qilin::host::Display::Options o;
    o.width = 320, o.height = 180;
    qilin::host::Display d(o);  // SDL_Init again: counted, harmless

    namespace h = qilin::host;
    check(reaches(d, 0), "connected before the window: nothing held");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, true);
    check(reaches(d, h::kPadA), "A (south) reaches the game: the pad was opened at start-up");
    check(d.pad_state(0).connected && (d.pad_state(0).buttons & h::kPadA) && !d.pad_state(1).connected,
          "the first controller to press a button is port A's");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, true);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true);
    check(reaches(d, h::kPadB | h::kPadRB), "B and RB together");
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_EAST, false);
    SDL_SetJoystickVirtualButton(j, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, -32767);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
    check(reaches(d, h::kPadLeft | h::kPadRT), "the left stick as the D-pad, the trigger as a button");
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, 0);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0);
    check(reaches(d, 0), "let go");

    // A second controller, pressed: port B's, port A's staying where it is.
    const SDL_JoystickID other = attach_pad("Other virtual controller");
    SDL_Joystick* jo = SDL_OpenJoystick(other);
    SDL_SetJoystickVirtualButton(jo, SDL_GAMEPAD_BUTTON_NORTH, true);
    check(reaches(d, h::kPadY), "a second controller's button");
    check((d.pad_state(1).buttons & h::kPadY) && !(d.pad_state(0).buttons & h::kPadY), "the second controller to press is port B's");
    SDL_SetJoystickVirtualButton(jo, SDL_GAMEPAD_BUTTON_NORTH, false);
    check(reaches(d, 0), "let go of the second");

    // Port A's unplugged, then another plugged in and pressed: it takes port A, port B's staying.
    SDL_CloseJoystick(j);
    SDL_DetachVirtualJoystick(first);
    d.poll();
    check(!d.pad_state(0).connected && d.pad_state(1).connected, "port A unplugged: port B's stays port B's");
    const SDL_JoystickID second = attach_pad("Second virtual controller");
    SDL_Joystick* j2 = SDL_OpenJoystick(second);
    SDL_SetJoystickVirtualButton(j2, SDL_GAMEPAD_BUTTON_START, true);
    check(reaches(d, h::kPadStart), "a controller connected later is used");
    check((d.pad_state(0).buttons & h::kPadStart) && !(d.pad_state(1).buttons & h::kPadStart), "it takes the unplugged port A");
    SDL_CloseJoystick(j2);
    SDL_CloseJoystick(jo);

    std::printf("padtest: %s\n", g_failures ? "FAILED" : "passed");
    return g_failures ? 1 : 0;
}
