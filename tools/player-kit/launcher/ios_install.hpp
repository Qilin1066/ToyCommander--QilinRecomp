// macOS only: "Play on iPad" (docs/platforms.md, the iPad section). The Mac
// app may carry an iOS kit (Contents/Resources/ios-kit: the runtime built for
// iPadOS as a static archive, its headers, build-flags.txt and a small Xcode
// project). With it, the launcher:
//
//   1. checks for full Xcode (the iOS SDK), an Apple development team (from
//      the keychain's signing identities and Xcode's accounts) and a
//      connected, trusted iPad or iPhone with Developer Mode on (devicectl),
//      each missing piece with a plain message and what to do;
//   2. compiles the lifted C++ (work/gen, from the Mac build) for iOS
//      with `xcrun --sdk iphoneos clang++` and the kit's flags, with the Mac
//      build's worker pool and an object cache of its own (build-cache-ios),
//      into work/ios/obj, archived as work/ios/libqilin_lifted.a;
//   3. builds and signs the app with xcodebuild (the player's team, a bundle
//      identifier made once per player), installs it with devicectl and copies
//      the player's data (a disc image's files, or a folder) into the app's Documents;
//   4. remembers when and with which team, and, for a free team's 7-day
//      profile, shows until when it works, with "Reinstall on iPad" (sign and
//      install again, no compiling, no copying).
//
// Everything is logged to work/ios/build.log (the checks to work/ios/checks.log).
// What it reads from Apple's tools is parsed by ios_text.hpp (portable, tested).
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "build.hpp"
#include "ios_text.hpp"
#include "kit.hpp"

namespace qilin::launcher {

class IosInstall {
public:
    IosInstall() = default;
    IosInstall(const IosInstall&) = delete;
    IosInstall& operator=(const IosInstall&) = delete;
    ~IosInstall();

    // Whether this app has an iOS kit.
    static bool available(const Kit& kit);
    // Shows the panel for `disc` and starts the checks.
    void open(const Kit& kit, const std::filesystem::path& disc);
    // Draws the panel (Dear ImGui, in the current window) in place of the
    // launcher's screen while it's open; false if it isn't.
    bool draw(const Kit& kit);
    // A check, build or install is running.
    bool busy() const { return task_ != Task::None; }
    // The window is closing: stop what runs (the build tools are ended by
    // stop_build_tools()) and wait for it.
    void stop();

    // The parts of the result the panel shows.
    enum class XcodeState { Unknown, Ready, Missing, FirstLaunch, Licence, TooOld, Broken };
    struct Checked {
        bool done = false;
        XcodeState xcode = XcodeState::Unknown;
        std::string xcode_message;    // what's wrong, or empty
        std::string xcode_note;       // a quiet line (installed but not selected: how to select it)
        std::string developer_dir;    // DEVELOPER_DIR for every Apple tool, when Xcode isn't the selected one
        std::string sdk_path;         // the iOS SDK
        std::vector<ios::Team> teams;
        std::string team_message;
        std::vector<ios::Device> devices;
        std::string device_message;   // devicectl itself failed
    };

private:
    enum class Task { None, Checking, Devices, Building, Reinstalling };
    enum class Step { Idle, Compiling, Archiving, Signing, Installing, Copying, Done };

    bool open_ = false;
    std::filesystem::path disc_;
    std::atomic<Task> task_{Task::None};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
    std::atomic<std::int64_t> devices_checked_{0};  // steady clock, milliseconds: devices are looked for again every few seconds

    std::mutex mutex_;  // guards what follows
    Checked checked_;
    std::string team_;        // the chosen team's id
    std::string device_;      // the chosen device's identifier
    std::optional<ios::Install> install_;
    std::string result_;      // the last build's or install's outcome
    bool failed_ = false;
    bool show_log_ = false;   // the failure has details in build.log
    std::string copying_;     // the file being copied

    // Progress.
    std::atomic<Step> step_{Step::Idle};
    BuildProgress compile_;
    std::atomic<int> copy_index_{0}, copy_count_{0};
    std::atomic<std::uint64_t> copy_done_{0}, copy_total_{0};

    void run(const Kit& kit, Task task);
    void check(const Kit& kit, bool devices_only);
    void build(const Kit& kit, bool reinstall);
    void draw_checks(const Kit& kit, const Checked& c, const std::string& team, const std::string& device, bool idle);
    void draw_progress(const Kit& kit);
};

}  // namespace qilin::launcher
