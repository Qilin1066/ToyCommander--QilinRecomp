// Apple's compiler and the app's MoltenVK, for the macOS launcher
// (apple_tools.hpp). Built on macOS only.
#include "apple_tools.hpp"

#include <filesystem>
#include <optional>
#include <system_error>

#include "imgui.h"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

const ImVec4 kWarning(1.0f, 0.75f, 0.35f, 1.0f);
const ImVec4 kQuiet(0.7f, 0.7f, 0.7f, 1.0f);

// Apple's installer, while it's open (one at a time).
std::optional<Process> g_installer;

std::string first_line(std::string_view text) {
    text = trim(text);
    return std::string(trim(text.substr(0, text.find('\n'))));
}

bool button(const char* label) {
    // As the launcher's big buttons (main.cpp).
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 2.5f, style.FramePadding.y * 2.0f));
    const bool pressed = ImGui::Button(label);
    ImGui::PopStyleVar();
    return pressed;
}

}  // namespace

int apple_clang_major(std::string_view version_text) {
    constexpr std::string_view kApple = "Apple clang version ";
    const std::size_t at = version_text.find(kApple);
    if (at == std::string_view::npos) return 0;
    int major = 0;
    for (std::size_t i = at + kApple.size(); i < version_text.size() && version_text[i] >= '0' && version_text[i] <= '9'; ++i)
        major = major * 10 + (version_text[i] - '0');
    return major;
}

AppleToolsCheck check_apple_tools(const Kit& kit) {
    AppleToolsCheck out;
    std::error_code ec;
    const fs::path said = kit.user / "compiler-check.txt";

    // Where the developer tools are (the Command Line Tools or Xcode).
    Command select = tool("/usr/bin/xcode-select", {"-p"}, kit.user, said);
    select.build_tool = false;
    const auto selected = run(select);
    const std::string folder = first_line(read_file(said).value_or(""));
    if (!selected || *selected != 0 || folder.empty() || !fs::is_directory(from_utf8(folder), ec)) {
        fs::remove(said, ec);
        out.result = AppleToolsCheck::Result::Missing;
        out.message = "Building the game needs Apple's command line tools: Apple's compiler, free from Apple (its licence doesn't let "
                      "it come with this download). This Mac doesn't have them yet. Install them with Apple's installer below (a few "
                      "minutes), then click Check again.";
        return out;
    }

    // The compiler itself, and its version.
    Command version = tool(kit.compiler(), kit.compiler_args, kit.user, said);
    version.build_tool = false;
    version.args.push_back("--version");
    const auto code = run(version);
    const std::string text = read_file(said).value_or("");
    fs::remove(said, ec);
    if (!code || *code != 0) {
        // xcrun's "You have not agreed to the Xcode license agreements" (exit 69).
        if (text.find("license") != std::string::npos || (code && *code == 69)) {
            out.result = AppleToolsCheck::Result::Licence;
            out.message = "Apple's developer tools are installed, but their licence hasn't been agreed to yet. Open Terminal, run "
                          "sudo xcodebuild -license and agree (or open Xcode once and agree there), then click Check again.";
            return out;
        }
        const std::string why = first_line(text);
        out.result = AppleToolsCheck::Result::Broken;
        out.message = "Apple's compiler (" + show(kit.compiler()) + ") doesn't start (" +
                      (why.empty() ? (code ? describe_exit(*code) : code.error()) : why) +
                      "). Installing Apple's command line tools again usually mends that; then click Check again.";
        return out;
    }
    const int major = apple_clang_major(text);
    if (major != 0 && major < kOldestAppleClang) {
        out.result = AppleToolsCheck::Result::TooOld;
        out.message = "This Mac's command line tools have Apple clang " + std::to_string(major) +
                      ", and building the game needs version " + std::to_string(kOldestAppleClang) +
                      " or newer (from Xcode 16). Update macOS's software in System Settings > General > Software Update (newer "
                      "tools may need a newer macOS first), then click Check again.";
        return out;
    }
    out.result = AppleToolsCheck::Result::Ready;
    return out;
}

std::vector<std::pair<std::string, std::string>> apple_game_environment(const Kit& kit) {
    const fs::path frameworks = kit.top / "Contents" / "Frameworks";
    const fs::path moltenvk = frameworks / "libMoltenVK.dylib";
    std::error_code ec;
    if (kit.top.extension() != ".app" || !fs::is_regular_file(moltenvk, ec)) return {};
    // volk asks for libvulkan.dylib, libvulkan.1.dylib, then libMoltenVK.dylib
    // by name; with only the app's Frameworks to search it finds the app's
    // MoltenVK (and skips a Vulkan SDK's loader in /usr/local/lib), and SDL
    // (whose own search would otherwise find volk's vkGetInstanceProcAddr,
    // a variable, in the game) loads the same file.
    return {{"DYLD_FALLBACK_LIBRARY_PATH", show(frameworks)}, {"SDL_VULKAN_LIBRARY", show(moltenvk)}};
}

AppleTools::~AppleTools() {
    if (worker_.joinable()) worker_.join();
}

void AppleTools::start_check(const Kit& kit) {
    if (state_ == State::Checking) return;
    if (worker_.joinable()) worker_.join();
    state_ = State::Checking;
    worker_ = std::thread([this, kit] {
        AppleToolsCheck result = check_apple_tools(kit);
        {
            const std::lock_guard lock(mutex_);
            last_ = std::move(result);
        }
        state_ = State::Done;
    });
}

bool AppleTools::ready(const Kit& kit, std::function<void()> then) {
    if (state_ == State::Done) {
        const std::lock_guard lock(mutex_);
        if (last_.result == AppleToolsCheck::Result::Ready) return true;
    }
    then_ = std::move(then);
    start_check(kit);  // again: the player may have installed them since
    return false;
}

bool AppleTools::draw(const Kit& kit) {
    if (g_installer && !g_installer->running()) g_installer.reset();
    if (!then_) return false;
    const State state = state_;
    AppleToolsCheck last;
    if (state == State::Done) {
        const std::lock_guard lock(mutex_);
        last = last_;
    }
    if (state == State::Done && last.result == AppleToolsCheck::Result::Ready) {
        const auto next = std::exchange(then_, nullptr);
        next();  // the build that waited
        return false;
    }
    if (state != State::Done) {
        ImGui::TextWrapped("Looking for Apple's compiler...");
        return true;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
    ImGui::TextWrapped("%s", last.message.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const bool can_install = last.result == AppleToolsCheck::Result::Missing || last.result == AppleToolsCheck::Result::Broken;
    if (can_install) {
        ImGui::BeginDisabled(g_installer.has_value());
        if (button("Install Apple's command line tools")) {
            // Apple's own installer: a dialog of the system's, which downloads and installs them.
            Command install;
            install.exe = "/usr/bin/xcode-select";
            install.args = {"--install"};
            install.build_tool = false;
            auto started = Process::start(install);
            if (started) {
                g_installer = std::move(*started);
                install_error_.clear();
            } else {
                install_error_ = "Apple's installer didn't start: " + started.error() +
                                 ". Open Terminal and run xcode-select --install instead.";
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (button("Check again")) start_check(kit);
    ImGui::SameLine();
    if (button("Not now")) then_ = nullptr;
    if (can_install) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
        ImGui::TextWrapped("%s", g_installer ? "Apple's installer is starting: follow its window, then click Check again when it has finished."
                                             : "Apple's installer opens a window of its own: click Install there and agree to Apple's licence.");
        ImGui::PopStyleColor();
    }
    if (!install_error_.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextWrapped("%s", install_error_.c_str());
        ImGui::PopStyleColor();
    }
    return true;
}

}  // namespace qilin::launcher
