// Play <Title>: the downloads' launcher on Windows, Linux and macOS, one
// program for every game (kit/game.txt names the game and says what it
// needs: ADOPTING.md). It finds the player's data, a disc image or a folder
// (chosen or dragged onto the window, remembered in the per-user data
// folder), builds the game from it the first time and after an update
// (build.hpp), starts it, and updates the download from GitHub's releases
// (update.hpp).
//
// Drawn with Dear ImGui over SDL_Renderer, on Vulkan or in software (the
// project's graphics are Vulkan only).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "build.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "input.hpp"
#include "kit.hpp"
#include "kit_text.hpp"
#include "platform.hpp"
#include "process.hpp"
#include "shortcut.hpp"
#include "tidy.hpp"
#include "update.hpp"
#include "vulkan_check.hpp"
#if defined(__APPLE__)
#include "apple_tools.hpp"  // Apple's compiler, checked before a build; the app's MoltenVK for the game
#include "ios_install.hpp"  // "Play on iPad", where the app has an iOS kit
#endif

namespace fs = std::filesystem;
using namespace qilin::launcher;

namespace {

enum class Screen {
    Broken,      // the download itself is incomplete: only a message
    Starting,    // checking the remembered input and the build
    NeedDisc,    // no usable disc (or folder) yet
    Building,
    BuildFailed,
    Ready,
    Playing,     // the window is hidden while the game runs
    GameFailed,
};

enum class UpdateState { Idle, Checking, Known, Offered, Installing, Failed };

fs::path path_from_utf8(const char* utf8) { return from_utf8(utf8); }

class App {
public:
    explicit App(SDL_Window* window) : window_(window) {}

    void start() {
        auto loaded = load_kit(own_path());
        if (!loaded) {
            fail_start(loaded.error());
            return;
        }
        kit_ = std::move(*loaded);
        SDL_SetWindowTitle(window_, (kit_.title + " QilinRecomp").c_str());
        // The chooser's filter: game.txt's extensions (kept for the dialog's life).
        filter_name_ = capitalised(kit_.input_noun) + " (" + list_extensions(kit_.input_extensions) + ")";
        filter_pattern_.clear();
        for (const std::string& e : kit_.input_extensions) filter_pattern_ += (filter_pattern_.empty() ? "" : ";") + e;
        filter_ = {filter_name_.c_str(), filter_pattern_.c_str()};
        if (!claim_folder(kit_)) {
            fail_start("This folder's launcher is already open.");
            return;
        }
        clean_after_update(kit_);
        screen_ = Screen::Starting;
        // The remembered disc, and whether the game is built from this kit.
        run_task([this] {
            // What older downloads unpacked over this one left behind.
            (void)tidy_folder(kit_);
            const bool shortcut = has_desktop_shortcut(kit_);
            auto identity = build_identity(kit_);
            if (!identity) {
                post([this, why = identity.error()] { fail_start(why); });
                return;
            }
            std::optional<fs::path> disc;
            if (const auto text = read_file(kit_.disc_file()))
                if (const auto line = trim(*text); !line.empty()) disc = from_utf8(line);
            InputCheck check;
            if (disc) check = check_input(kit_, *disc);
            post([this, built_from = *identity, disc, check, shortcut] {
                identity_ = built_from;
                shortcut_ = shortcut;
                if (!disc) {
                    screen_ = Screen::NeedDisc;
                    return;
                }
                if (!check.ok) {
                    screen_ = Screen::NeedDisc;
                    notice_ = show(*disc) + ": " + check.message;
                    return;
                }
                disc_ = *disc;
                if (up_to_date(kit_, identity_)) {
                    screen_ = Screen::Ready;
                } else {
                    // A new folder, an update or another CPU: build again.
                    std::error_code ec;
                    notice_ = fs::exists(kit_.game_exe(), ec) ? "This version needs the game built again: a few minutes." : "";
                    start_build(true);
                }
            });
        });
    }

    // Called every frame on the main thread.
    void frame() {
        std::function<void()> next;
        {
            const std::lock_guard lock(mutex_);
            next = std::exchange(posted_, nullptr);
            if (chosen_) {
                const fs::path disc = *std::exchange(chosen_, std::nullopt);
                if (can_take_disc()) take_disc(disc);
            }
        }
        if (next) next();
        if (screen_ == Screen::Playing) watch_game();
        if (worker_.joinable() && !busy_) worker_.join();
    }

    void draw();

    // A file or folder dropped onto the window or chosen in the dialog (any thread).
    void offer_disc(fs::path path) {
        // A folder as some dialogs and drops give it, "C:\Games\X\": without the separator.
        if (!path.has_filename() && path.has_relative_path()) path = path.parent_path();
        const std::lock_guard lock(mutex_);
        chosen_ = std::move(path);
    }

    bool busy() const {
#if defined(__APPLE__)
        if (ios_.busy()) return true;
#endif
        return busy_ || screen_ == Screen::Playing;
    }
    bool playing() const { return screen_ == Screen::Playing; }
    bool want_quit() const { return quit_; }

    // Closing the window: stop what's running (the build's tools with it).
    void stop() {
        cancel_ = true;
        stop_build_tools();
        if (worker_.joinable()) worker_.join();
#if defined(__APPLE__)
        ios_.stop();
#endif
    }

private:
    SDL_Window* window_;
    Kit kit_;
    Screen screen_ = Screen::Starting;
    std::string identity_;
    std::optional<fs::path> disc_;
    std::string notice_;   // a line under the title: what the last disc check or step said
    std::string failure_;  // BuildFailed's or GameFailed's text
    std::string errors_;   // GameFailed: the end of run-errors.log
    bool quit_ = false;
    bool shortcut_ = false;        // a shortcut (or menu entry) to this launcher exists
    std::string shortcut_error_;
    std::atomic<bool> no_dialog_{false};  // the system has no file dialog: the path is typed or pasted
    char typed_path_[2048] = {};
    std::string filter_name_, filter_pattern_;  // the file dialog's filter (game.txt's input_extensions)
    SDL_DialogFileFilter filter_{};

    BuildProgress build_;
    std::optional<Process> game_;

    UpdateState update_ = UpdateState::Idle;
    std::string update_message_;
    std::optional<Release> release_;
    UpdateProgress download_;

    std::mutex mutex_;
    std::function<void()> posted_;          // the worker's result, run on the main thread
    std::optional<fs::path> chosen_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> cancel_{false};
    std::thread worker_;
#if defined(__APPLE__)
    AppleTools apple_tools_;
    IosInstall ios_;
#endif

    void fail_start(std::string why) {
        screen_ = Screen::Broken;
        failure_ = std::move(why);
    }

    // One task at a time on the worker thread; its result is posted back.
    void run_task(std::function<void()> task) {
        if (worker_.joinable()) worker_.join();
        busy_ = true;
        worker_ = std::thread([this, task = std::move(task)] {
            task();
            busy_ = false;
        });
    }
    void post(std::function<void()> f) {
        const std::lock_guard lock(mutex_);
        posted_ = std::move(f);
    }

    bool can_take_disc() const {
#if defined(__APPLE__)
        if (ios_.busy()) return false;
#endif
        return !busy_ && (screen_ == Screen::NeedDisc || screen_ == Screen::Ready || screen_ == Screen::BuildFailed ||
                          screen_ == Screen::GameFailed);
    }

    void take_disc(const fs::path& disc) {
        const Screen before = screen_;
        notice_ = "Checking " + show(disc.filename()) + "...";
        run_task([this, disc, before] {
            const InputCheck check = check_input(kit_, disc);
            post([this, disc, before, check] {
                if (!check.ok) {
                    notice_ = check.message;
                    if (before != Screen::Ready) screen_ = Screen::NeedDisc;
                    return;
                }
                if (const auto saved = write_file(kit_.disc_file(), show(disc)); !saved) {
                    notice_ = check.message + " (It can't be remembered: " + saved.error() + ".)";
                } else {
                    notice_ = check.message;
                }
                disc_ = disc;
                if (up_to_date(kit_, identity_)) screen_ = Screen::Ready;
                else start_build(true);
            });
        });
    }

    void start_build(bool use_cache) {
        if (!disc_) return;
#if defined(__APPLE__)
        if (!apple_tools_.ready(kit_, [this, use_cache] { start_build(use_cache); })) return;
#endif
        screen_ = Screen::Building;
        cancel_ = false;
        run_task([this, disc = *disc_, use_cache] {
            // No Vulkan 1.3 device: the game couldn't start, so don't build it.
            if (auto vulkan = check_vulkan(); !vulkan) {
                post([this, why = vulkan.error()] {
                    screen_ = Screen::BuildFailed;
                    failure_ = why;
                });
                return;
            }
            BuildOptions options;
            options.use_cache = use_cache;
            const auto built = build_game(kit_, disc, build_, cancel_, options);
            auto identity = build_identity(kit_);
            post([this, built, identity] {
                if (identity) identity_ = *identity;
                if (built) {
                    screen_ = Screen::Ready;
                    notice_ = "The game is built.";
                } else {
                    screen_ = Screen::BuildFailed;
                    failure_ = built.error();
                }
            });
        });
    }

    void play() {
        if (!disc_) return;
        auto command = game_command(kit_, *disc_);
        if (!command) {
            screen_ = Screen::GameFailed;
            failure_ = command.error();
            errors_.clear();
            return;
        }
        Command c = std::move(*command);
#if defined(__APPLE__)
        c.environment = apple_game_environment(kit_);
#endif
        auto started = Process::start(c);
        if (!started) {
            screen_ = Screen::GameFailed;
            failure_ = "The game didn't start: " + started.error() + ".";
            errors_.clear();
            return;
        }
        game_ = std::move(*started);
        screen_ = Screen::Playing;
        SDL_HideWindow(window_);
    }

    void watch_game() {
        if (!game_ || game_->running()) return;
        const unsigned long code = game_->wait();
        game_.reset();
        if (code == 0) {
            // The new build runs: the one kept in case it didn't can go.
            std::error_code ec;
            fs::remove(fs::path(kit_.game_exe()).concat(L".previous"), ec);
            quit_ = true;
            return;
        }
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
        screen_ = Screen::GameFailed;
        failure_ = "The game stopped with " + describe_exit(code) + ".";
        errors_ = tail(kit_.run_errors(), 20);
    }

    void check_updates() {
        update_ = UpdateState::Checking;
        update_message_ = "Asking GitHub...";
        run_task([this] {
            auto checked = check_for_update(kit_);
            post([this, checked] {
                if (!checked) {
                    update_ = UpdateState::Failed;
                    update_message_ = checked.error();
                    return;
                }
                update_message_ = checked->message;
                release_ = checked->newer;
                update_ = release_ ? UpdateState::Offered : UpdateState::Known;
            });
        });
    }

    void install() {
        if (!release_) return;
        update_ = UpdateState::Installing;
        download_.received = 0;
        download_.total = 0;
        download_.unpacking = false;
        cancel_ = false;
        run_task([this, release = *release_] {
            auto installed = install_update(kit_, release, download_, cancel_);
            post([this, installed] {
                if (!installed) {
                    update_ = UpdateState::Failed;
                    update_message_ = installed.error();
                    return;
                }
                // The new launcher sees the new version and builds the game again.
                if (start_detached(*installed, kit_.top)) {
                    quit_ = true;
                } else {
                    update_ = UpdateState::Known;
                    update_message_ = "Updated. Start " + show(installed->filename()) + " again to build the new version.";
                }
            });
        });
    }

    void open_build_log() { open_with_system(kit_.build_log()); }

    static void dialog_done(void* self, const char* const* files, int) {
        auto* app = static_cast<App*>(self);
        if (files && files[0]) app->offer_disc(path_from_utf8(files[0]));
        else if (!files) app->no_dialog_ = true;  // no file dialog here (some Linux desktops): a path box instead
    }

    void choose_disc() {
        std::string folder;
        if (disc_) folder = show(disc_->parent_path());
        const char* start = folder.empty() ? nullptr : folder.c_str();
        if (kit_.input == InputKind::Folder) SDL_ShowOpenFolderDialog(dialog_done, this, window_, start, false);
        else SDL_ShowOpenFileDialog(dialog_done, this, window_, kit_.input_extensions.empty() ? nullptr : &filter_,
                                    kit_.input_extensions.empty() ? 0 : 1, start, false);
    }
    // "Choose your disc", "Choose your game folder"
    std::string choose_label() const { return "Choose your " + kit_.input_noun; }
    std::string another_label() const { return "Choose another " + kit_.input_noun; }
    const char* thing() const { return kit_.input == InputKind::Folder ? "folder" : "file"; }

    void draw_disc_prompt();
    void draw_update();
};

bool big_button(const char* label, float width = 0) {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x * 2.5f, style.FramePadding.y * 2.0f));
    const bool pressed = ImGui::Button(label, ImVec2(width, 0));
    ImGui::PopStyleVar();
    return pressed;
}

void wrapped(const std::string& text) { ImGui::TextWrapped("%s", text.c_str()); }

void coloured(const ImVec4& colour, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    wrapped(text);
    ImGui::PopStyleColor();
}

const ImVec4 kWarning(1.0f, 0.75f, 0.35f, 1.0f);
const ImVec4 kGood(0.55f, 0.9f, 0.55f, 1.0f);
const ImVec4 kQuiet(0.7f, 0.7f, 0.7f, 1.0f);

void App::draw_disc_prompt() {
    wrapped("This download builds the game from your own " + kit_.input_noun + ": " + kit_.wanted() + ". " + kit_.input_hint);
    ImGui::Spacing();
    if (big_button(choose_label().c_str())) choose_disc();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kQuiet, "or drag the %s onto this window.", thing());
    if (no_dialog_) {
        // No file dialog on this desktop: the path, typed or pasted.
        ImGui::Spacing();
        wrapped(std::string("This desktop has no file chooser. Type or paste the full path of your ") + kit_.input_noun + ":");
        const std::string use = "Use this " + std::string(thing());
        ImGui::SetNextItemWidth(-ImGui::CalcTextSize(use.c_str()).x - ImGui::GetStyle().FramePadding.x * 4);
        const bool entered = ImGui::InputText("##path", typed_path_, sizeof typed_path_, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(use.c_str()) || entered) && typed_path_[0]) {
            std::string typed = std::string(trim(typed_path_));
            if (typed.size() > 1 && (typed.front() == '\'' || typed.front() == '"') && typed.back() == typed.front())
                typed = typed.substr(1, typed.size() - 2);  // pasted with quotes
            offer_disc(from_utf8(typed));
        }
    }
}

void App::draw_update() {
    ImGui::Separator();
    const bool idle = !busy_;
    switch (update_) {
        case UpdateState::Offered: {
            std::string label = "Download and install";
            if (release_ && release_->asset_size > 0) label += " (" + show_bytes(static_cast<std::uintmax_t>(release_->asset_size)) + ")";
            wrapped(update_message_);
            ImGui::BeginDisabled(!idle || screen_ == Screen::Building);
            if (ImGui::Button(label.c_str())) install();
            ImGui::SameLine();
            if (ImGui::Button("Not now")) update_ = UpdateState::Idle;
            ImGui::EndDisabled();
            return;
        }
        case UpdateState::Installing: {
            const std::uint64_t total = download_.total;
            const std::uint64_t got = download_.received;
            if (download_.unpacking) {
                ImGui::ProgressBar(-1.0f * static_cast<float>(ImGui::GetTime()), ImVec2(-1, 0), "Unpacking and installing...");
            } else {
                const std::string text = "Downloading " + show_bytes(got) + (total ? " of " + show_bytes(total) : std::string());
                ImGui::ProgressBar(total ? static_cast<float>(static_cast<double>(got) / static_cast<double>(total)) : 0.0f, ImVec2(-1, 0),
                                   text.c_str());
            }
            return;
        }
        default: break;
    }
    ImGui::BeginDisabled(!idle || screen_ == Screen::Building || screen_ == Screen::Starting);
    if (ImGui::Button("Check for updates")) check_updates();
    ImGui::EndDisabled();
    if (!update_message_.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (update_ == UpdateState::Failed) {
            ImGui::NewLine();
            coloured(kWarning, update_message_);
        } else {
            ImGui::TextColored(kQuiet, "%s", update_message_.c_str());
        }
    }
}

void App::draw() {
    const ImGuiViewport* view = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(view->WorkPos);
    ImGui::SetNextWindowSize(view->WorkSize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushFont(nullptr, style.FontSizeBase * 1.6f);
    ImGui::TextUnformatted(kit_.title.empty() ? "QilinRecomp" : kit_.title.c_str());
    ImGui::PopFont();
    if (!kit_.version.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kQuiet, "QilinRecomp %s", kit_.version.c_str());
    }
    ImGui::Separator();
    ImGui::Spacing();
#if defined(__APPLE__)
    if ((screen_ != Screen::Broken && apple_tools_.draw(kit_)) || (screen_ == Screen::Ready && ios_.draw(kit_))) {
        ImGui::End();
        return;
    }
#endif

    switch (screen_) {
        case Screen::Broken:
            coloured(kWarning, failure_);
            ImGui::Spacing();
            if (big_button("Quit")) quit_ = true;
            break;
        case Screen::Starting: wrapped("Checking the download and your " + kit_.input_noun + "..."); break;
        case Screen::NeedDisc:
            if (!notice_.empty()) {
                coloured(kWarning, notice_);
                ImGui::Spacing();
            }
            ImGui::BeginDisabled(busy_);
            draw_disc_prompt();
            ImGui::EndDisabled();
            break;
        case Screen::Building: {
            if (!notice_.empty()) wrapped(notice_);
            const Stage stage = build_.stage;
            const int done = build_.done;
            const int total = build_.total;
            std::string label;
            float fraction = 0;
            switch (stage) {
                case Stage::Checking:
                    label = "Checking " + kit_.the_input();
                    fraction = 0.01f;
                    break;
                case Stage::Recompiling:
                    label = kit_.boot.empty() ? std::string("Recompiling into C++") : "Recompiling " + kit_.boot + " into C++";
                    if (total > 1) label += ": step " + std::to_string(std::min(done + 1, total)) + " of " + std::to_string(total);
                    fraction = 0.01f + 0.06f * (total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f);
                    break;
                case Stage::Compiling:
                    label = "Compiling " + std::to_string(done) + " of " + std::to_string(total);
                    fraction = 0.08f + 0.87f * (total ? static_cast<float>(done) / static_cast<float>(total) : 0.0f);
                    break;
                case Stage::Linking:
                    label = "Linking";
                    fraction = 0.96f;
                    break;
                case Stage::Done:
                    label = "Done";
                    fraction = 1.0f;
                    break;
            }
            ImGui::Spacing();
            wrapped("Building the game from your " + kit_.input_noun + ". This happens once (and after an update): a few minutes, up to 15 on a slower PC.");
            ImGui::Spacing();
            ImGui::ProgressBar(fraction, ImVec2(-1, 0), label.c_str());
            ImGui::Spacing();
            ImGui::TextColored(kQuiet, "Your game files never leave your PC. Closing this window stops the build.");
            break;
        }
        case Screen::BuildFailed:
            coloured(kWarning, failure_);
            ImGui::Spacing();
            ImGui::BeginDisabled(busy_);
            if (disc_ && big_button("Try again")) start_build(true);
            ImGui::SameLine();
            if (big_button("Show build.log")) open_build_log();
            ImGui::Spacing();
            draw_disc_prompt();
            ImGui::EndDisabled();
            break;
        case Screen::Ready: {
            if (!notice_.empty()) coloured(kGood, notice_);
            if (disc_) ImGui::TextColored(kQuiet, "%s: %s", capitalised(kit_.input_noun).c_str(), show(*disc_).c_str());
            ImGui::Spacing();
            ImGui::BeginDisabled(busy_);
            ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
            if (big_button("  Play  ")) play();
            ImGui::PopFont();
            ImGui::Spacing();
            if (ImGui::Button(another_label().c_str())) choose_disc();
            ImGui::SameLine();
            if (ImGui::Button("Build again from scratch")) {
                notice_.clear();
                start_build(false);
            }
            if (!shortcut_ && shortcut_label()) {
                ImGui::SameLine();
                if (ImGui::Button(shortcut_label())) {
                    const auto made = make_desktop_shortcut(kit_);
                    shortcut_ = made.has_value();
                    shortcut_error_ = made ? std::string() : "The shortcut couldn't be made: " + made.error() + ".";
                }
            }
#if defined(__APPLE__)
            if (disc_ && IosInstall::available(kit_)) {
                ImGui::SameLine();
                if (ImGui::Button("Play on iPad")) ios_.open(kit_, *disc_);
            }
#endif
            ImGui::EndDisabled();
            if (!shortcut_error_.empty()) coloured(kWarning, shortcut_error_);
            ImGui::TextColored(kQuiet, "Or drag another %s onto this window.", thing());
            break;
        }
        case Screen::Playing: break;
        case Screen::GameFailed:
            coloured(kWarning, failure_);
            if (!errors_.empty()) {
                ImGui::TextColored(kQuiet, "The end of run-errors.log (send it with run.log when reporting a problem):");
                const float room = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 4;
                ImGui::BeginChild("errors", ImVec2(-1, room > 80 ? room : 80), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(errors_.c_str());
                ImGui::EndChild();
            }
            ImGui::Spacing();
            ImGui::BeginDisabled(busy_);
            if (big_button("Play again")) play();
            ImGui::SameLine();
            if (big_button(another_label().c_str())) choose_disc();
            ImGui::SameLine();
            if (big_button("Quit")) quit_ = true;
            ImGui::EndDisabled();
            break;
    }

    if (screen_ != Screen::Broken) {
        // The update line at the bottom.
        const float line = ImGui::GetFrameHeightWithSpacing() + style.ItemSpacing.y * 2;
        const float bottom = ImGui::GetWindowHeight() - style.WindowPadding.y - line * (update_ == UpdateState::Failed ? 2.5f : 1.0f);
        if (ImGui::GetCursorPosY() < bottom) ImGui::SetCursorPosY(bottom);
        draw_update();
    }
    ImGui::End();
}

}  // namespace

int main(int, char**) {
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "vulkan,software");  // Vulkan only for graphics; software where there's none
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "QilinRecomp", SDL_GetError(), nullptr);
        return 1;
    }
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (!(scale > 0.5f)) scale = 1.0f;
    const int width = static_cast<int>(780 * scale), height = static_cast<int>(500 * scale);
#if defined(__APPLE__)
    // Retina: the window's size in points, drawn at its pixels (Dear ImGui
    // takes the framebuffer's scale from SDL and renders its fonts at it).
    const SDL_WindowFlags density = SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    const SDL_WindowFlags density = 0;
#endif
    // The Vulkan renderer needs a Vulkan window; without a Vulkan loader the
    // window can't be one, and the software renderer draws instead.
    SDL_Window* window = SDL_CreateWindow("QilinRecomp", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | density);
    if (!window) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        window = SDL_CreateWindow("QilinRecomp", width, height, SDL_WINDOW_RESIZABLE | density);
    }
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "QilinRecomp", SDL_GetError(), window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowMinimumSize(window, static_cast<int>(560 * scale), static_cast<int>(400 * scale));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(20, 18);
    style.FramePadding = ImVec2(10, 6);
    style.ItemSpacing = ImVec2(10, 10);
    style.FrameRounding = 4;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    // The system's own interface font reads better than the built-in pixel
    // font: Segoe UI (Windows), the system font (macOS), DejaVu or Noto Sans
    // (Linux, where most desktops have one of them).
    std::vector<std::string> fonts;
#if defined(_WIN32)
    for (const char* name : {"SystemRoot", "windir"})  // SDL's lookup minds the case
        if (const char* windows = SDL_getenv(name)) fonts.push_back(std::string(windows) + "\\Fonts\\segoeui.ttf");
#elif defined(__APPLE__)
    fonts = {"/System/Library/Fonts/SFNS.ttf", "/System/Library/Fonts/Helvetica.ttc", "/Library/Fonts/Arial Unicode.ttf"};
#else
    fonts = {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
             "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
             "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"};
#endif
    bool font = false;
    for (const std::string& f : fonts) {
        std::error_code ec;
        if (!font && fs::is_regular_file(path_from_utf8(f.c_str()), ec)) font = io.Fonts->AddFontFromFileTTF(f.c_str(), 19.0f) != nullptr;
    }
    if (!font) {
        io.Fonts->AddFontDefault();
        style.FontScaleMain = 1.4f;
    }
    style.FontSizeBase = 19.0f;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    App app(window);
    app.start();
    bool running = true;
    while (running) {
        SDL_Event event;
        // Asleep until something happens, a few times a second while busy.
        if (SDL_WaitEventTimeout(&event, app.busy() ? 100 : 500)) {
            do {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT ||
                    (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)))
                    running = false;
                if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) app.offer_disc(path_from_utf8(event.drop.data));
            } while (SDL_PollEvent(&event));
        }
        app.frame();
        if (app.want_quit()) running = false;
        if (!running || app.playing()) continue;
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        app.draw();
        ImGui::Render();
        SDL_SetRenderDrawColor(renderer, 24, 26, 30, 255);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }
    app.stop();
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
