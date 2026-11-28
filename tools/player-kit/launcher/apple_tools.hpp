// macOS only: what the launcher needs from Apple's side of the Mac. The
// game is compiled with Apple's clang from the Command Line Tools (Apple's
// SDK can't be redistributed, so the download carries no compiler): before
// a build the launcher checks they're installed (`xcode-select -p`) and new
// enough for the project's C++23 (`clang++ --version`: Apple clang 16, from
// Xcode 16, or newer), and offers Apple's own installer
// (`xcode-select --install`) where they're missing. And the game is started
// so that it finds the app's MoltenVK (Contents/Frameworks).
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "kit.hpp"

namespace qilin::launcher {

// The oldest Apple clang that compiles the project (CLAUDE.md: Xcode 16).
constexpr int kOldestAppleClang = 16;

// "Apple clang version 16.0.0 (clang-1600.0.26.3)" -> 16; 0 if the text
// doesn't say it's Apple's clang (another clang is taken as it is).
int apple_clang_major(std::string_view version_text);

struct AppleToolsCheck {
    enum class Result { Ready, Missing, Licence, TooOld, Broken };
    Result result = Result::Broken;
    std::string message;  // plain words for the player (empty when Ready)
};

// Runs `xcode-select -p`, then the kit's compiler with --version.
AppleToolsCheck check_apple_tools(const Kit& kit);

// What the game is started with so that its Vulkan loader (volk) and SDL
// both load the app's own MoltenVK, the same library: SDL from the path
// given, volk by name from the only folder searched. Empty outside an app.
std::vector<std::pair<std::string, std::string>> apple_game_environment(const Kit& kit);

// The launcher's gate in front of a build: the check runs on its own thread,
// and while a build waits for it (or for the player to install the tools)
// draw() shows why, in place of the launcher's screen.
class AppleTools {
public:
    AppleTools() = default;
    AppleTools(const AppleTools&) = delete;
    AppleTools& operator=(const AppleTools&) = delete;
    ~AppleTools();

    // Whether the compiler is known to be ready. If not, `then` is kept and
    // run by draw() (on the main thread) as soon as it is.
    bool ready(const Kit& kit, std::function<void()> then);
    // Draws what's missing and its buttons (Dear ImGui, in the current
    // window) while a build waits; true if it drew, false if nothing waits.
    bool draw(const Kit& kit);

private:
    enum class State { Unknown, Checking, Done };
    std::atomic<State> state_{State::Unknown};
    std::mutex mutex_;
    AppleToolsCheck last_;              // the check's result (State::Done)
    std::function<void()> then_;        // the build waiting for the compiler
    std::thread worker_;
    std::string install_error_;

    void start_check(const Kit& kit);
};

}  // namespace qilin::launcher
