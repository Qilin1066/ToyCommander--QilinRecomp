// iPadOS and iOS (ios.hpp): QILIN_IOS builds only.
#include "qilin/host/ios.hpp"

#include <SDL3/SDL.h>
#include <sys/mman.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "qilin/host/display.hpp"
#include "qilin/host/paths.hpp"
#include "qilin/host/vulkan.hpp"

// Calls fn(arg) with the stack pointer at `top` (rounded down to 16 bytes),
// and puts the caller's back after it. The frame pointer chain runs on into
// the caller's frame, so backtraces cross over. fn must not throw.
extern "C" int qilin_call_on_stack(void* arg, int (*fn)(void*), void* top);
#if defined(__aarch64__)
asm(R"(
	.text
	.globl _qilin_call_on_stack
	.p2align 2
_qilin_call_on_stack:
	stp x29, x30, [sp, #-16]!
	mov x29, sp
	and x2, x2, #0xfffffffffffffff0
	mov sp, x2
	blr x1
	mov sp, x29
	ldp x29, x30, [sp], #16
	ret
)");
#elif defined(__x86_64__)
asm(R"(
	.text
	.globl _qilin_call_on_stack
	.p2align 4
_qilin_call_on_stack:
	pushq %rbp
	movq %rsp, %rbp
	andq $-16, %rdx
	movq %rdx, %rsp
	callq *%rsi
	movq %rbp, %rsp
	popq %rbp
	retq
)");
#else
#error "no stack switch for this processor (host/src/ios.cpp)"
#endif

namespace qilin::host::ios {

bool self_test() {
    static const bool on = [] {
        const char* v = std::getenv("QILIN_IOS_SELFTEST");
        return v && *v && *v != '0';
    }();
    return on;
}

std::filesystem::path documents() {
    // An app's HOME is its own container; Documents is made with it.
    const char* home = std::getenv("HOME");
    if (!home || !*home) die("no HOME: can't find the app's Documents folder");
    return std::filesystem::path(home) / "Documents";
}

void start() {
    // The home indicator hidden, and a swipe from any edge kept for the game
    // first (a second swipe goes to the system): SDL's "2". Landscape only.
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    if (!SDL_Init(SDL_INIT_VIDEO)) die("SDL can't start: %s", SDL_GetError());
    const std::filesystem::path data = user_folder();
    const char* log = std::getenv("QILIN_LOG");
    if (self_test() || (log && std::strcmp(log, "console") == 0)) return;
    // Nobody sees an app's stderr: it goes to the data folder, which the
    // Files app shows, for a report.
    const std::string file = (data / "log.txt").string();
    if (std::freopen(file.c_str(), "w", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
}

void tell(const char* title, const char* text) {
    std::fprintf(stderr, QILIN_APP ": %s: %s\n", title, text);
    if (self_test()) return;
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, title, text, nullptr))
        std::fprintf(stderr, QILIN_APP ": can't show the message: %s\n", SDL_GetError());
}

bool ask(const char* title, const char* text, const char* yes, const char* no) {
    std::fprintf(stderr, QILIN_APP ": %s: %s\n", title, text);
    if (self_test()) return false;
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no},
    };
    SDL_MessageBoxData box{};
    box.flags = SDL_MESSAGEBOX_INFORMATION;
    box.title = title;
    box.message = text;
    box.numbuttons = 2;
    box.buttons = buttons;
    int pressed = 0;
    if (!SDL_ShowMessageBox(&box, &pressed)) {
        std::fprintf(stderr, QILIN_APP ": can't show the message: %s\n", SDL_GetError());
        return false;
    }
    return pressed == 1;
}

void show_fatal(const char* message) {
    if (self_test()) return;  // the log has it
    static bool showing = false;  // a stop while showing one: the first is enough
    if (showing) return;
    showing = true;
    const std::string text = std::string(message) +
                             "\n\nThe log is in the Files app: On My iPad, " QILIN_APP_TITLE ", qilin, " QILIN_APP ", log.txt.";
    if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, QILIN_APP_TITLE " stopped", text.c_str(), nullptr))
        std::fprintf(stderr, QILIN_APP ": can't show the message: %s\n", SDL_GetError());
}

int run_on_big_stack(int (*body)(void*), void* arg, size_t bytes) {
    // A guard region below it: running off the end faults at once.
    constexpr size_t kGuard = 64 * 1024;
    bytes = (bytes + kGuard - 1) & ~(kGuard - 1);
    void* p = mmap(nullptr, bytes + kGuard, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) die("can't make the game's stack (%zu MB)", bytes >> 20);
    if (mprotect(p, kGuard, PROT_NONE) != 0) die("can't guard the game's stack");
    return qilin_call_on_stack(arg, body, static_cast<char*>(p) + kGuard + bytes);
}

int self_test_display() {
    Display::Options o;
    o.fullscreen = true;
    Display display(o);
    std::fprintf(stderr, "QILIN_IOS_SELFTEST: drawing with %s\n", display.device_name().c_str());
    static const uint8_t grey[4] = {48, 48, 48, 255};
    for (int frame = 0; frame < 60; ++frame) {
        if (!display.poll()) {
            std::fprintf(stderr, "QILIN_IOS_SELFTEST: the window was closed at frame %d\n", frame);
            break;
        }
        if (frame < 3 || frame % 20 == 0) std::fprintf(stderr, "QILIN_IOS_SELFTEST: frame %d\n", frame);
        display.present(1, 1, grey, 4.0f / 3.0f, 0);
    }
    std::fprintf(stderr, "QILIN_IOS_SELFTEST: passed\n");
    std::fflush(nullptr);
    std::_Exit(0);  // what a quit does: no teardown
}

}  // namespace qilin::host::ios
