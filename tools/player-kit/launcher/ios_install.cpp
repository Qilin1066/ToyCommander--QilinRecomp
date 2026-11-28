// "Play on iPad" (ios_install.hpp). Built on macOS only.
#include "ios_install.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <random>
#include <system_error>

#include "imgui.h"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"

namespace qilin::launcher {

namespace fs = std::filesystem;

namespace {

const ImVec4 kWarning(1.0f, 0.75f, 0.35f, 1.0f);
const ImVec4 kGood(0.55f, 0.9f, 0.55f, 1.0f);
const ImVec4 kQuiet(0.7f, 0.7f, 0.7f, 1.0f);

constexpr const char* kSeeLog = " The details are in work/ios/build.log (Show build.log opens it).";
// Xcode's page in the Mac App Store.
constexpr const char* kXcodeStore = "macappstores://apps.apple.com/app/xcode/id497799835";

using Environment = std::vector<std::pair<std::string, std::string>>;

// ---- Where things are ----

fs::path ios_kit(const Kit& kit) { return kit.top / "Contents" / "Resources" / "ios-kit"; }
fs::path ios_work(const Kit& kit) { return kit.work / "ios"; }
fs::path ios_log(const Kit& kit) { return ios_work(kit) / "build.log"; }
fs::path lifted_archive(const Kit& kit) { return ios_work(kit) / "libqilin_lifted.a"; }
fs::path team_file(const Kit& kit) { return kit.user / "ios-team.txt"; }
fs::path device_file(const Kit& kit) { return kit.user / "ios-device.txt"; }
fs::path bundle_file(const Kit& kit) { return kit.user / "ios-bundle-id.txt"; }
fs::path install_file(const Kit& kit) { return kit.user / "ios-install.txt"; }

std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string first_line(std::string_view text) {
    text = trim(text);
    return std::string(trim(text.substr(0, text.find('\n'))));
}

std::string remembered(const fs::path& file) { return std::string(trim(read_file(file).value_or(""))); }

// "Saturday 5 December 2026 at 14:03", in the Mac's own time zone.
std::string show_time(std::int64_t when) {
    const auto t = static_cast<std::time_t>(when);
    std::tm local{};
    if (!localtime_r(&t, &local)) return "an unknown date";
    char day[32] = {}, month[32] = {}, clock[16] = {};
    std::strftime(day, sizeof day, "%A", &local);
    std::strftime(month, sizeof month, "%B", &local);
    std::strftime(clock, sizeof clock, "%H:%M", &local);
    return std::string(day) + " " + std::to_string(local.tm_mday) + " " + month + " " + std::to_string(local.tm_year + 1900) + " at " + clock;
}

// ---- Running Apple's tools ----

struct Ran {
    bool started = false;
    unsigned long code = 0;
    std::string text;  // what it printed (both streams)
    std::string why;   // why it didn't start
    bool ok() const { return started && code == 0; }
    std::string describe() const { return started ? describe_exit(code) : why; }
};

// Runs a tool in `cwd`, its output into `log` (and returned).
Ran run_tool(const fs::path& exe, std::vector<std::string> args, const fs::path& cwd, const Environment& env, BuildLog& log,
             bool build_tool = true) {
    static std::atomic<unsigned> n{0};
    std::error_code ec;
    fs::create_directories(cwd / "logs", ec);
    const fs::path out = cwd / "logs" / ("tool-" + std::to_string(n++) + ".txt");
    Command c = tool(exe, std::move(args), cwd, out);
    c.environment = env;
    c.build_tool = build_tool;
    Ran r;
    const auto code = launcher::run(c);
    r.started = code.has_value();
    if (code) r.code = *code;
    else r.why = code.error();
    r.text = read_file(out).value_or("");
    log.tool(c, out, r.describe());
    return r;
}

bool says(std::string_view text, std::string_view what) {
    // Case-blind: Apple's messages change their capitals between versions.
    const auto lower = [](char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); };
    return std::search(text.begin(), text.end(), what.begin(), what.end(), [&](char a, char b) { return lower(a) == lower(b); }) != text.end();
}

void open_url(const std::string& url) {
    Command c;
    c.exe = "/usr/bin/open";
    c.args = {url};
    c.build_tool = false;
    (void)Process::start(c);  // it hands the address over and ends
}

void open_xcode(const std::string& developer_dir) {
    Command c;
    c.exe = "/usr/bin/open";
    c.build_tool = false;
    // The Xcode found, or the one the system knows by its identifier.
    if (!developer_dir.empty()) c.args = {fs::path(developer_dir).parent_path().parent_path().string()};
    else c.args = {"-b", "com.apple.dt.Xcode"};
    (void)Process::start(c);
}

// The Xcode apps in /Applications (Xcode.app first).
std::vector<fs::path> xcode_apps() {
    std::vector<fs::path> out;
    std::error_code ec;
    for (fs::directory_iterator it("/Applications", ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.starts_with("Xcode") && name.ends_with(".app") && fs::is_directory(it->path() / "Contents" / "Developer", ec))
            out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
        const bool a_plain = a.filename() == "Xcode.app", b_plain = b.filename() == "Xcode.app";
        return a_plain != b_plain ? a_plain : a < b;
    });
    return out;
}

// ---- The checks ----

void check_xcode(IosInstall::Checked& c, const fs::path& cwd, BuildLog& log) {
    using S = IosInstall::XcodeState;
    Environment env;
    const auto sdk = [&] { return run_tool("/usr/bin/xcrun", {"--sdk", "iphoneos", "--show-sdk-path"}, cwd, env, log, false); };
    Ran found = sdk();
    std::error_code ec;
    const auto sdk_ok = [&](const Ran& r) { return r.ok() && fs::is_directory(fs::path(first_line(r.text)), ec); };
    if (!sdk_ok(found)) {
        // The Command Line Tools have no iOS SDK: Xcode may be there but not
        // the selected developer folder. Then every Apple tool here is run
        // with DEVELOPER_DIR, and the player is told how to select it.
        for (const fs::path& app : xcode_apps()) {
            const std::string dir = (app / "Contents" / "Developer").string();
            env = {{"DEVELOPER_DIR", dir}};
            found = sdk();
            if (sdk_ok(found) || says(found.text, "license")) {
                c.developer_dir = dir;
                c.xcode_note = app.filename().string() + " is installed but isn't the selected developer folder, so the launcher uses it "
                               "directly. To make it the one every tool uses, run this in Terminal: sudo xcode-select -s " + dir;
                break;
            }
            env.clear();
        }
    }
    if (says(found.text, "license") || (found.started && found.code == 69)) {
        c.xcode = S::Licence;
        c.xcode_message = "Xcode's licence hasn't been agreed to yet. Open Xcode once and agree to it (or run sudo xcodebuild -license in "
                          "Terminal), then click Check again.";
        return;
    }
    if (!sdk_ok(found)) {
        const bool any = !xcode_apps().empty();
        c.xcode = any ? S::Broken : S::Missing;
        c.xcode_message = any ? "Xcode is installed but has no iOS SDK. Open Xcode, and in Settings > Components install the iOS "
                                "platform, then click Check again."
                              : "Building for an iPad needs Xcode, Apple's free developer app (the command line tools aren't enough: "
                                "they have no iOS SDK). Install Xcode from the App Store (it's large: allow an hour), open it once, "
                                "then click Check again.";
        return;
    }
    c.sdk_path = first_line(found.text);
    // The licence and Xcode's first-launch setup (its components).
    const Ran licence = run_tool("/usr/bin/xcodebuild", {"-license", "check"}, cwd, env, log, false);
    if (!licence.ok()) {
        c.xcode = S::Licence;
        c.xcode_message = "Xcode's licence hasn't been agreed to yet. Open Xcode once and agree to it, then click Check again.";
        return;
    }
    const Ran first = run_tool("/usr/bin/xcodebuild", {"-checkFirstLaunchStatus"}, cwd, env, log, false);
    if (!first.ok()) {
        c.xcode = S::FirstLaunch;
        c.xcode_message = "Xcode hasn't finished setting itself up. Open Xcode once and let it install what it asks to, then click "
                          "Check again.";
        return;
    }
    // devicectl came with Xcode 15.
    const Ran devicectl = run_tool("/usr/bin/xcrun", {"--find", "devicectl"}, cwd, env, log, false);
    if (!devicectl.ok()) {
        c.xcode = S::TooOld;
        c.xcode_message = "This Xcode is too old to install on an iPad from here (it needs Xcode 15 or newer, for devicectl). Update "
                          "Xcode from the App Store, then click Check again.";
        return;
    }
    c.xcode = S::Ready;
}

void check_teams(IosInstall::Checked& c, const fs::path& cwd, const Environment& env, BuildLog& log) {
    std::vector<ios::Team> teams;
    // Xcode's accounts (its settings, exported as XML).
    const fs::path exported = cwd / "xcode-settings.plist";
    std::error_code ec;
    fs::remove(exported, ec);
    if (run_tool("/usr/bin/defaults", {"export", "com.apple.dt.Xcode", exported.string()}, cwd, env, log, false).ok() &&
        run_tool("/usr/bin/plutil", {"-convert", "xml1", exported.string()}, cwd, env, log, false).ok()) {
        for (ios::Team& t : ios::xcode_teams(read_file(exported).value_or(""))) ios::add_team(teams, std::move(t));
    }
    fs::remove(exported, ec);
    // The keychain's development identities, and their certificates' teams.
    const Ran listed = run_tool("/usr/bin/security", {"find-identity", "-v", "-p", "codesigning"}, cwd, env, log, false);
    const std::vector<ios::Identity> identities = ios::parse_identities(listed.text);
    std::vector<ios::Subject> subjects;
    std::vector<std::string> asked;
    for (const ios::Identity& id : identities) {
        if (std::find(asked.begin(), asked.end(), id.name) != asked.end()) continue;
        asked.push_back(id.name);
        const Ran pem = run_tool("/usr/bin/security", {"find-certificate", "-a", "-p", "-c", id.name}, cwd, env, log, false);
        for (ios::Subject& s : ios::pem_subjects(pem.text)) subjects.push_back(std::move(s));
    }
    for (ios::Team& t : ios::identity_teams(identities, subjects)) ios::add_team(teams, std::move(t));
    c.teams = std::move(teams);
    if (c.teams.empty())
        c.team_message = "Xcode has no Apple ID to sign the game with. Open Xcode, then Xcode > Settings > Accounts, click + and add your "
                         "Apple ID (a free one works), then click Check again.";
}

void check_devices(IosInstall::Checked& c, const fs::path& cwd, const Environment& env, BuildLog& log) {
    const fs::path json = cwd / "devices.json";
    std::error_code ec;
    fs::remove(json, ec);
    const Ran listed = run_tool("/usr/bin/xcrun", {"devicectl", "list", "devices", "--json-output", json.string()}, cwd, env, log, false);
    const auto text = read_file(json);
    fs::remove(json, ec);
    if (!listed.ok() || !text) {
        c.device_message = "Xcode's device tool (devicectl) didn't answer (" + listed.describe() + "). Open Xcode once, then click Check again.";
        return;
    }
    auto devices = ios::parse_devices(*text);
    if (!devices) {
        c.device_message = "Xcode's device tool gave an answer the launcher can't read (" + devices.error() + ").";
        return;
    }
    c.devices = std::move(*devices);
}

std::string device_advice(const ios::Device& d) {
    if (!d.reachable())
        return d.name + " isn't connected. Plug it into this Mac with a cable and unlock it. (Once it has been set up over a cable, being "
                        "on the same Wi-Fi network as this Mac is enough.)";
    if (!d.paired())
        return "Unlock " + d.name + " and tap Trust when it asks whether to trust this computer, then enter its passcode.";
    if (!d.developer_mode_on())
        return "Turn on Developer Mode on " + d.name + ": Settings > Privacy & Security > Developer Mode (near the bottom). It restarts; "
               "after it restarts, unlock it and choose Turn On. If Developer Mode isn't listed, open Xcode with " + d.name +
               " plugged in (Window > Devices and Simulators), then look again.";
    return {};
}

std::string describe_device(const ios::Device& d) {
    std::string s = d.name;
    std::string about = d.model;
    if (!d.os_version.empty()) about += std::string(about.empty() ? "" : ", ") + (d.device_type == "iPad" ? "iPadOS " : "iOS ") + d.os_version;
    if (d.transport == "localNetwork") about += std::string(about.empty() ? "" : ", ") + "over Wi-Fi";
    if (!about.empty()) s += " (" + about + ")";
    return s;
}

std::string describe_team(const ios::Team& t) {
    std::string s = t.name.empty() ? t.id : t.name + " (" + t.id + ")";
    if (t.known_free) s += t.free ? ", free: apps last 7 days" : ", paid membership";
    return s;
}

// ---- Plain messages for what xcodebuild and devicectl say ----

std::string signing_failure(const std::string& text, const std::string& team) {
    if (says(text, "maximum App ID limit") || says(text, "maximum number of apps"))
        return "Apple's limit for free Apple IDs has been reached (a few apps per device, and 10 new app identifiers a week). Delete "
               "another app you installed from Xcode from the iPad, or wait a few days, then try again.";
    if (says(text, "has no devices") || says(text, "register the device") || says(text, "device registration"))
        return "Xcode couldn't register the iPad with your team. Keep it plugged in and unlocked, then try again.";
    if (says(text, "No Account for Team") || says(text, "No signing certificate") || says(text, "requires a development team") ||
        says(text, "not signed in") || says(text, "session has expired"))
        return "Xcode couldn't sign with team " + team + ". Open Xcode > Settings > Accounts, check that your Apple ID is there and "
               "signed in (sign in again if it asks), then try again.";
    if (says(text, "license")) return "Xcode's licence hasn't been agreed to yet. Open Xcode once and agree to it, then try again.";
    return "Building the iPad app failed.";
}

std::string install_failure(const std::string& text, const std::string& device) {
    if (says(text, "Developer Mode"))
        return "Developer Mode is off on " + device + ": Settings > Privacy & Security > Developer Mode, then try again.";
    if (says(text, "locked")) return "Unlock " + device + " and keep it unlocked while the game installs, then try again.";
    if (says(text, "not paired") || says(text, "pairing") || says(text, "trust"))
        return "Unlock " + device + " and tap Trust when it asks whether to trust this computer, then try again.";
    if (says(text, "not enough space") || says(text, "insufficient") || says(text, "storage"))
        return device + " hasn't enough free space for the game and its data. Free some, then try again.";
    if (says(text, "unavailable") || says(text, "not connected") || says(text, "disconnected"))
        return device + " isn't connected any more. Plug it in, unlock it, then try again.";
    return "Installing on " + device + " failed.";
}

// The bundle identifier, made once per player and game.
std::string bundle_id(const Kit& kit) {
    const std::string have = remembered(bundle_file(kit));
    if (ios::valid_bundle_id(have)) return have;
    std::random_device entropy;
    const std::string made = ios::make_bundle_id(kit.game, static_cast<std::uint32_t>(entropy()));
    (void)write_file(bundle_file(kit), made + "\n");
    return made;
}

}  // namespace

// ---- The panel ----

IosInstall::~IosInstall() { stop(); }

bool IosInstall::available(const Kit& kit) {
    std::error_code ec;
    return kit.top.extension() == ".app" && fs::is_regular_file(ios_kit(kit) / "build-flags.txt", ec);
}

void IosInstall::stop() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

void IosInstall::open(const Kit& kit, const fs::path& disc) {
    open_ = true;
    disc_ = disc;
    {
        const std::lock_guard lock(mutex_);
        install_ = ios::parse_install(read_file(install_file(kit)).value_or(""));
        if (team_.empty()) team_ = remembered(team_file(kit));
        if (device_.empty()) device_ = remembered(device_file(kit));
        result_.clear();
        failed_ = false;
    }
    run(kit, Task::Checking);
}

void IosInstall::run(const Kit& kit, Task task) {
    if (task_ != Task::None) return;
    if (worker_.joinable()) worker_.join();
    cancel_ = false;
    task_ = task;
    worker_ = std::thread([this, kit, task] {
        if (task == Task::Checking || task == Task::Devices) check(kit, task == Task::Devices);
        else build(kit, task == Task::Reinstalling);
        if (task == Task::Checking || task == Task::Devices) devices_checked_ = steady_ms();
        task_ = Task::None;
    });
}

void IosInstall::check(const Kit& kit, bool devices_only) {
    std::error_code ec;
    const fs::path cwd = ios_work(kit);
    fs::create_directories(cwd, ec);
    BuildLog log(cwd / (devices_only ? "devices.log" : "checks.log"));
    Checked c;
    if (devices_only) {
        const std::lock_guard lock(mutex_);
        c = checked_;
        c.devices.clear();
        c.device_message.clear();
    } else {
        check_xcode(c, cwd, log);
    }
    const Environment env = c.developer_dir.empty() ? Environment{} : Environment{{"DEVELOPER_DIR", c.developer_dir}};
    if (c.xcode == XcodeState::Ready) {
        if (!devices_only) check_teams(c, cwd, env, log);
        check_devices(c, cwd, env, log);
    }
    c.done = true;
    const std::lock_guard lock(mutex_);
    checked_ = std::move(c);
    // The remembered choices, if they're still there; else the only team and the first ready device.
    if (std::none_of(checked_.teams.begin(), checked_.teams.end(), [&](const ios::Team& t) { return t.id == team_; }))
        team_ = checked_.teams.size() == 1 ? checked_.teams[0].id : std::string();
    if (std::none_of(checked_.devices.begin(), checked_.devices.end(), [&](const ios::Device& d) { return d.identifier == device_; }))
        device_ = !checked_.devices.empty() ? checked_.devices[0].identifier : std::string();
}

void IosInstall::build(const Kit& kit, bool reinstall) {
    std::error_code ec;
    const fs::path work = kit.work;
    const fs::path here = ios_work(kit);
    fs::create_directories(here / "logs", ec);
    BuildLog log(ios_log(kit));
    Checked c;
    std::string team, device_id;
    std::optional<ios::Install> before;
    {
        const std::lock_guard lock(mutex_);
        c = checked_;
        before = install_;
        team = team_;
        device_id = device_;
        result_.clear();
        failed_ = false;
        show_log_ = false;
    }
    const auto device_it = std::find_if(c.devices.begin(), c.devices.end(), [&](const ios::Device& d) { return d.identifier == device_id; });
    const ios::Device device = device_it != c.devices.end() ? *device_it : ios::Device{};
    const Environment env = c.developer_dir.empty() ? Environment{} : Environment{{"DEVELOPER_DIR", c.developer_dir}};
    const auto finish = [&](std::string message, bool failed, bool details) {
        log.line((failed ? "Stopped: " : "Done: ") + message);
        const std::lock_guard lock(mutex_);
        result_ = std::move(message);
        failed_ = failed;
        show_log_ = details;
        step_ = failed ? Step::Idle : Step::Done;
    };
    log.line(kit.title + " QilinRecomp " + kit.version + ": " + (reinstall ? "reinstalling on " : "building for ") + describe_device(device));
    log.line("Team: " + team);
    if (team.empty() || device.identifier.empty()) return finish("Choose a team and a device first.", true, false);

    // The iOS kit's parts (ios-kit/build-flags.txt: ios_text.hpp's KitLayout).
    const fs::path kit_dir = ios_kit(kit);
    const auto flag_text = read_file(kit_dir / "build-flags.txt");
    if (!flag_text) return finish("This download's iOS kit has no build-flags.txt. Download the game again.", true, false);
    const auto vars = parse_settings(*flag_text);
    const ios::KitLayout layout = ios::kit_layout(vars, kit.game);
    const fs::path kit_library = kit_dir / from_utf8(layout.kit_library);
    const fs::path project_source = kit_dir / from_utf8(layout.project);
    if (!fs::is_regular_file(kit_library, ec) || !fs::is_directory(project_source, ec))
        return finish("This download's iOS kit is incomplete (" + layout.kit_library + ", " + layout.project + "). Download the game again.",
                      true, false);
    const std::string bundle = bundle_id(kit);
    log.line("Bundle identifier: " + bundle);
    const fs::path archive = lifted_archive(kit);

    if (!reinstall || !fs::is_regular_file(archive, ec)) {
        // The lifted C++ of the Mac build (work/gen, subfolders too), compiled for iOS.
        bool have_sources = false;
        for (fs::recursive_directory_iterator it(work / "gen", ec), end; !ec && it != end && !have_sources; it.increment(ec))
            have_sources = it->path().extension() == ".cpp";
        if (!have_sources)
            return finish("The game's recompiled code isn't there yet: build the game for this Mac first (Back, then Build again from "
                          "scratch), then try again.",
                          true, false);
        step_ = Step::Compiling;
        compile_.stage = Stage::Compiling;
        compile_.done = 0;
        compile_.total = 0;
        compile_.cached = 0;
        auto it = vars.find("cxxflags");
        std::vector<std::string> cxxflags = it == vars.end() ? std::vector<std::string>{} : split_flags(it->second, kit_dir.string());
        // The kit's flags name the target; if they don't, the device's processor.
        if (std::none_of(cxxflags.begin(), cxxflags.end(), [](const std::string& f) { return f == "-arch" || f == "-target" || f.starts_with("--target="); }))
            cxxflags.insert(cxxflags.end(), {"-arch", "arm64"});
        cxxflags.push_back("-Igen");
        CompileJob job;
        job.cwd = work;
        job.obj = "ios/obj";
        job.logs = here / "logs";
        job.cache = kit.user / "build-cache-ios";
        job.compiler = "/usr/bin/xcrun";
        job.compiler_args = {"--sdk", "iphoneos", "clang++"};
        job.cxxflags = cxxflags;
        job.environment = env;
        job.target = "target\nios-arm64\nsdk\n" + c.sdk_path + "\n";
        job.include = kit_dir / "include";
        job.memory_mb = kit.compile_memory_mb;
        job.see_log = kSeeLog;
        const auto objects = compile_lifted(job, log, compile_, cancel_);
        if (!objects) return finish(objects.error(), true, objects.error() != "The build was stopped.");
        trim_cache(job.cache, 2ull << 30);
        log.line("Compiled for iOS: " + std::to_string(compile_.cached.load()) + " of " + std::to_string(compile_.total.load()) +
                 " files from the cache.");

        step_ = Step::Archiving;
        fs::remove(archive, ec);
        std::vector<std::string> args = {"libtool", "-static", "-no_warning_for_no_symbols", "-o", "ios/libqilin_lifted.a"};
        args.insert(args.end(), objects->begin(), objects->end());
        const Ran archived = run_tool("/usr/bin/xcrun", args, work, env, log);
        if (cancel_) return finish("The build was stopped.", true, false);
        if (!archived.ok() || !fs::is_regular_file(archive, ec))
            return finish("Making the game's library for iOS failed (" + archived.describe() + ")." + kSeeLog, true, true);
    }
    if (cancel_) return finish("The build was stopped.", true, false);

    // The Xcode project, copied out of the app (xcodebuild writes beside a
    // project, and the app is sealed), built and signed with the player's team.
    step_ = Step::Signing;
    const fs::path project_rel = from_utf8(layout.project);
    const fs::path copy_from = project_rel.has_parent_path() ? kit_dir / *project_rel.begin() : project_source;
    const fs::path copy_to = here / (project_rel.has_parent_path() ? *project_rel.begin() : project_rel.filename());
    fs::remove_all(copy_to, ec);
    fs::copy(copy_from, copy_to, fs::copy_options::recursive, ec);
    if (ec) return finish("Can't copy the Xcode project to " + show(here) + ": " + ec.message() + ".", true, false);
    const fs::path project = here / project_rel;
    const fs::path derived = here / "DerivedData";
    const fs::path products = derived / "Build" / "Products" / (layout.configuration + "-iphoneos");
    fs::remove_all(products, ec);
    std::vector<std::string> args = {"-project", project.string(), "-scheme", layout.scheme, "-configuration", layout.configuration,
                                     "-destination", device.udid.empty() ? std::string("generic/platform=iOS") : "platform=iOS,id=" + device.udid,
                                     "-derivedDataPath", derived.string(), "-allowProvisioningUpdates", "-allowProvisioningDeviceRegistration",
                                     "DEVELOPMENT_TEAM=" + team, "PRODUCT_BUNDLE_IDENTIFIER=" + bundle, "CODE_SIGN_STYLE=Automatic",
                                     layout.lifted_setting + "=" + archive.string(), layout.kit_setting + "=" + kit_library.string(),
                                     "QILIN_IOS_KIT=" + kit_dir.string(), "build"};
    const Ran built = run_tool("/usr/bin/xcodebuild", args, here, env, log);
    if (cancel_) return finish("The build was stopped.", true, false);
    fs::path app;
    for (fs::directory_iterator it(products, ec), end; !ec && it != end; it.increment(ec))
        if (it->path().extension() == ".app") app = it->path();
    if (!built.ok() || app.empty()) return finish(signing_failure(built.text, team) + kSeeLog, true, true);

    // The profile Xcode made: until when the app opens.
    ios::Install record;
    record.installed = unix_now();
    record.team = team;
    record.device = device.identifier;
    record.device_name = device.name;
    record.bundle_id = bundle;
    record.app = app.string();
    {
        const fs::path decoded = here / "profile.plist";
        fs::remove(decoded, ec);
        const Ran cms = run_tool("/usr/bin/security", {"cms", "-D", "-i", (app / "embedded.mobileprovision").string(), "-o", decoded.string()}, here,
                                 env, log);
        const auto profile = ios::parse_profile(read_file(decoded).value_or(""));
        if (cms.ok() && profile) {
            record.expires = profile->expires;
            record.short_lived = profile->short_lived();
            log.line("Profile: " + profile->name + ", team " + profile->team_id + ", until " + show_time(profile->expires));
        } else {
            log.line("The profile's expiry can't be read: " + (profile ? std::string("security cms failed") : profile.error()));
        }
    }

    // Installed (over an earlier one: its Documents, the disc and saves, stay).
    step_ = Step::Installing;
    const Ran installed = run_tool("/usr/bin/xcrun", {"devicectl", "device", "install", "app", "--device", device.identifier, app.string()}, here,
                                   env, log);
    if (cancel_) return finish("The build was stopped.", true, false);
    if (!installed.ok()) return finish(install_failure(installed.text, device.name) + kSeeLog, true, true);
    record.disc = before ? before->disc : std::string();

    // A reinstall on the same device keeps its Documents: no copying.
    if (!reinstall || !before || before->device != device.identifier || before->disc.empty()) {
        // The player's data into the app's Documents, unless already there: a
        // disc image's files (a .cue's or .gdi's tracks beside it), or a
        // folder's every file as Documents/<folder>/... (ADOPTING.md: the
        // iOS runtime finds it there).
        step_ = Step::Copying;
        std::expected<std::vector<std::string>, std::string> names;
        if (kit.input == InputKind::Folder) {
            const std::string top = show(disc_.filename());
            std::vector<std::string> found;
            for (fs::recursive_directory_iterator it(disc_, ec), end; !ec && it != end; it.increment(ec)) {
                if (!it->is_regular_file(ec)) continue;
                std::string rel = show(it->path().lexically_relative(disc_));
                std::replace(rel.begin(), rel.end(), '\\', '/');
                found.push_back(top + "/" + rel);
            }
            if (ec) return finish("Can't read the folder " + show(disc_) + ": " + ec.message() + ".", true, false);
            std::sort(found.begin(), found.end());
            names = std::move(found);
        } else {
            const auto image_text = read_file(disc_);
            const std::string ext = disc_.extension().string();
            const bool listed = says(ext, ".cue") || says(ext, ".gdi");
            if (listed && !image_text) return finish("Can't read " + show(disc_) + ".", true, false);
            names = ios::disc_files(disc_.filename().string(), listed ? *image_text : std::string());
        }
        if (!names) return finish(show(disc_.filename()) + " can't be copied: " + names.error() + ".", true, false);
        std::vector<std::pair<std::string, std::uint64_t>> files;
        std::uint64_t total = 0;
        for (const std::string& name : *names) {
            const fs::path local = disc_.parent_path() / from_utf8(name);
            const auto size = fs::file_size(local, ec);
            if (ec) return finish("The file " + show(local) + " is missing.", true, false);
            files.emplace_back(name, size);
            total += size;
        }
        // What's in Documents already (assumed: devicectl's `device info files`).
        const fs::path json = here / "documents.json";
        fs::remove(json, ec);
        const Ran there = run_tool("/usr/bin/xcrun", {"devicectl", "device", "info", "files", "--device", device.identifier, "--domain-type",
                                                      "appDataContainer", "--domain-identifier", bundle, "--subdirectory", "Documents",
                                                      "--json-output", json.string()},
                                   here, env, log);
        const std::vector<ios::RemoteFile> remote = there.ok() ? ios::parse_remote_files(read_file(json).value_or("")) : std::vector<ios::RemoteFile>{};
        fs::remove(json, ec);
        copy_count_ = static_cast<int>(files.size());
        copy_total_ = total;
        copy_done_ = 0;
        for (std::size_t i = 0; i < files.size(); ++i) {
            const auto& [name, size] = files[i];
            copy_index_ = static_cast<int>(i) + 1;
            {
                const std::lock_guard lock(mutex_);
                copying_ = name;
            }
            if (ios::already_there(remote, name, size)) {
                log.line("Already on the device: Documents/" + name);
            } else {
                const Ran copied = run_tool("/usr/bin/xcrun", {"devicectl", "device", "copy", "to", "--device", device.identifier, "--domain-type",
                                                               "appDataContainer", "--domain-identifier", bundle, "--source",
                                                               (disc_.parent_path() / from_utf8(name)).string(), "--destination", "Documents/" + name},
                                           here, env, log);
                if (cancel_) return finish("The copy was stopped.", true, false);
                if (!copied.ok()) {
                    std::string why = install_failure(copied.text, device.name);
                    if (why.starts_with("Installing")) why = "Copying " + kit.the_input() + " to " + device.name + " failed.";
                    return finish(why + kSeeLog, true, true);
                }
            }
            copy_done_ += size;
        }
        record.disc = disc_.string();
    }

    (void)write_file(install_file(kit), ios::format_install(record));
    {
        const std::lock_guard lock(mutex_);
        install_ = record;
    }
    finish("The game is installed on " + device.name + ".", false, false);
}

void IosInstall::draw_progress(const Kit& kit) {
    std::string label;
    float fraction = 0;
    switch (step_.load()) {
        case Step::Idle:
        case Step::Done: return;
        case Step::Compiling: {
            const int done = compile_.done, total = compile_.total;
            label = total ? "Compiling for iPad: " + std::to_string(done) + " of " + std::to_string(total) : std::string("Compiling for iPad");
            fraction = 0.02f + 0.6f * (total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f);
            break;
        }
        case Step::Archiving:
            label = "Making the game's library";
            fraction = 0.63f;
            break;
        case Step::Signing:
            label = "Building and signing the app with Xcode";
            fraction = 0.66f;
            break;
        case Step::Installing:
            label = "Installing on the device";
            fraction = 0.72f;
            break;
        case Step::Copying: {
            std::string name;
            {
                const std::lock_guard lock(mutex_);
                name = copying_;
            }
            const std::uint64_t done = copy_done_, total = copy_total_;
            label = "Copying " + kit.the_input() + ": " + name + " (" + std::to_string(copy_index_.load()) + " of " + std::to_string(copy_count_.load()) + ", " +
                    show_bytes(done) + " of " + show_bytes(total) + ")";
            fraction = 0.75f + 0.25f * (total ? static_cast<float>(static_cast<double>(done) / static_cast<double>(total)) : 0.0f);
            break;
        }
    }
    ImGui::ProgressBar(fraction, ImVec2(-1, 0), label.c_str());
    ImGui::TextColored(kQuiet, "Keep the device plugged in and unlocked. Closing this window stops it.");
}

void IosInstall::draw_checks(const Kit& kit, const Checked& c, const std::string& team, const std::string& device, bool idle) {
    const auto status = [](bool ok, const char* what) {
        ImGui::TextColored(ok ? kGood : kWarning, "%s %s", ok ? "[ready]" : "[to do]", what);
    };
    const auto warn = [](const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, kWarning);
        ImGui::TextWrapped("%s", text.c_str());
        ImGui::PopStyleColor();
    };

    // 1. Xcode.
    status(c.xcode == XcodeState::Ready, "Xcode");
    ImGui::Indent();
    if (c.xcode == XcodeState::Ready) {
        ImGui::TextColored(kQuiet, "The iOS SDK: %s", c.sdk_path.c_str());
    } else {
        warn(c.xcode_message);
        if (c.xcode == XcodeState::Missing && ImGui::Button("Install Xcode from the App Store")) open_url(kXcodeStore);
        if ((c.xcode == XcodeState::FirstLaunch || c.xcode == XcodeState::Licence || c.xcode == XcodeState::Broken) && ImGui::Button("Open Xcode"))
            open_xcode(c.developer_dir);
        if (c.xcode == XcodeState::TooOld && ImGui::Button("Update Xcode in the App Store")) open_url(kXcodeStore);
    }
    if (!c.xcode_note.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kQuiet);
        ImGui::TextWrapped("%s", c.xcode_note.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Unindent();
    if (c.xcode != XcodeState::Ready) return;

    // 2. The team.
    const bool team_ok = std::any_of(c.teams.begin(), c.teams.end(), [&](const ios::Team& t) { return t.id == team; });
    status(team_ok, "Your Apple ID's team");
    ImGui::Indent();
    if (c.teams.empty()) {
        warn(c.team_message);
        if (ImGui::Button("Open Xcode")) open_xcode(c.developer_dir);
    } else if (c.teams.size() == 1) {
        ImGui::TextWrapped("%s", describe_team(c.teams[0]).c_str());
    } else {
        if (!team_ok) ImGui::TextWrapped("Choose the team to sign the game with:");
        ImGui::BeginDisabled(!idle);
        for (const ios::Team& t : c.teams) {
            ImGui::PushID(t.id.c_str());
            if (ImGui::RadioButton(describe_team(t).c_str(), t.id == team)) {
                const std::lock_guard lock(mutex_);
                team_ = t.id;
                (void)write_file(team_file(kit), t.id + "\n");
            }
            ImGui::PopID();
        }
        ImGui::EndDisabled();
    }
    ImGui::Unindent();

    // 3. The device.
    const auto chosen = std::find_if(c.devices.begin(), c.devices.end(), [&](const ios::Device& d) { return d.identifier == device; });
    const bool device_ok = chosen != c.devices.end() && chosen->ready();
    status(device_ok, "Your iPad (or iPhone)");
    ImGui::Indent();
    if (!c.device_message.empty()) {
        warn(c.device_message);
    } else if (c.devices.empty()) {
        warn("No iPad or iPhone is connected. Plug it into this Mac with a cable, unlock it, and tap Trust if it asks whether to trust "
             "this computer.");
    } else {
        if (c.devices.size() > 1) {
            ImGui::BeginDisabled(!idle);
            for (const ios::Device& d : c.devices) {
                ImGui::PushID(d.identifier.c_str());
                if (ImGui::RadioButton(describe_device(d).c_str(), d.identifier == device)) {
                    const std::lock_guard lock(mutex_);
                    device_ = d.identifier;
                    (void)write_file(device_file(kit), d.identifier + "\n");
                }
                ImGui::PopID();
            }
            ImGui::EndDisabled();
        } else {
            ImGui::TextWrapped("%s", describe_device(c.devices[0]).c_str());
        }
        if (chosen != c.devices.end() && !chosen->ready()) warn(device_advice(*chosen));
    }
    ImGui::Unindent();
}

bool IosInstall::draw(const Kit& kit) {
    if (!open_) return false;
    const bool idle = task_ == Task::None;
    // While a device is missing, it's looked for again every few seconds.
    Checked c;
    std::string team, device, result;
    std::optional<ios::Install> install;
    bool failed = false, details = false;
    {
        const std::lock_guard lock(mutex_);
        c = checked_;
        team = team_;
        device = device_;
        install = install_;
        result = result_;
        failed = failed_;
        details = show_log_;
    }
    const auto chosen = std::find_if(c.devices.begin(), c.devices.end(), [&](const ios::Device& d) { return d.identifier == device; });
    const bool device_ready = chosen != c.devices.end() && chosen->ready();
    const bool team_ready = std::any_of(c.teams.begin(), c.teams.end(), [&](const ios::Team& t) { return t.id == team; });
    const bool all_ready = c.done && c.xcode == XcodeState::Ready && team_ready && device_ready;
    if (idle && c.done && c.xcode == XcodeState::Ready && !device_ready && steady_ms() - devices_checked_ > 4000) run(kit, Task::Devices);

    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(nullptr, style.FontSizeBase * 1.25f);
    ImGui::TextUnformatted("Play on iPad");
    ImGui::PopFont();
    ImGui::TextWrapped("%s", ("This Mac builds the game for your iPad or iPhone, signs it with your own Apple ID, and installs it over the "
                             "cable with a copy of your " + kit.input_noun + ". Your game files stay on your devices.").c_str());
    ImGui::Spacing();
    if (!c.done) {
        ImGui::TextWrapped("Checking Xcode, your Apple ID and your devices...");
    } else {
        draw_checks(kit, c, team, device, idle);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const bool working = task_ == Task::Building || task_ == Task::Reinstalling;
    if (working) {
        draw_progress(kit);
    } else if (!result.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, failed ? kWarning : kGood);
        ImGui::TextWrapped("%s", result.c_str());
        ImGui::PopStyleColor();
    }
    // Until when it works (a free team's 7-day profile).
    if (!working && install && install->expires > 0) {
        const bool gone = install->expires <= unix_now();
        const std::string where = install->device_name.empty() ? std::string("your iPad") : install->device_name;
        if (install->short_lived || gone) {
            const std::string text = gone ? "The game on " + where + " stopped opening on " + show_time(install->expires) +
                                                ": a free Apple ID's apps work for 7 days. Reinstall it (your saves stay)."
                                          : "Works on " + where + " until " + show_time(install->expires) +
                                                " (a free Apple ID's apps work for 7 days; then reinstall it: your saves stay).";
            ImGui::PushStyleColor(ImGuiCol_Text, gone ? kWarning : kQuiet);
            ImGui::TextWrapped("%s", text.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextColored(kQuiet, "Works on %s until %s.", where.c_str(), show_time(install->expires).c_str());
        }
    }
    if (!working && !failed && result.starts_with("The game is installed"))
        ImGui::TextColored(kQuiet, "The first time: on the device, Settings > General > VPN & Device Management, tap your Apple ID and Trust it.");
    ImGui::Spacing();

    ImGui::BeginDisabled(!idle);
    ImGui::BeginDisabled(!all_ready);
    if (ImGui::Button(install ? "Build again and install" : "Build and install")) run(kit, Task::Building);
    // Signed and installed again with what was built: no compiling, no copying.
    std::error_code ec;
    if (install && fs::is_regular_file(lifted_archive(kit), ec)) {
        ImGui::SameLine();
        if (ImGui::Button("Reinstall on iPad")) run(kit, Task::Reinstalling);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Check again")) run(kit, Task::Checking);
    if (details) {
        ImGui::SameLine();
        if (ImGui::Button("Show build.log")) open_with_system(ios_log(kit));
    }
    ImGui::SameLine();
    if (ImGui::Button("Back")) open_ = false;
    ImGui::EndDisabled();
    if (!all_ready && c.done && idle) ImGui::TextColored(kQuiet, "Each step above needs to be ready first.");
    return true;
}

}  // namespace qilin::launcher
