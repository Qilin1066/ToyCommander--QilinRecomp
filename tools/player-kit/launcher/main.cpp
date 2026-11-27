// Play <Title>.exe: the Windows download's launcher, one program for both
// games (kit\game.txt names the game). It finds the player's disc (chosen or
// dragged onto the window, remembered in the per-user data folder), builds
// the game from it the first time and after an update (build.hpp), starts
// it, and updates the download from GitHub's releases (update.hpp).
//
// Drawn with Dear ImGui over SDL_Renderer, on Vulkan or in software (the
// project's graphics are Vulkan only).
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "build.hpp"
#include "disc.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "kit.hpp"
#include "kit_text.hpp"
#include "process.hpp"
#include "update.hpp"

namespace fs = std::filesystem;
using namespace qilin::launcher;

namespace {

enum class Screen {
    Broken,      // the download itself is incomplete: only a message
    Starting,    // checking the remembered disc and the build
    NeedDisc,    // no usable disc yet
    Building,
    BuildFailed,
    Ready,
    Playing,     // the window is hidden while the game runs
    GameFailed,
};

enum class UpdateState { Idle, Checking, Known, Offered, Installing, Failed };

fs::path path_from_utf8(const char* utf8) { return fs::path(widen(utf8)); }

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
        if (!claim_folder(kit_)) {
            fail_start("This folder's launcher is already open.");
            return;
        }
        clean_after_update(kit_);
        screen_ = Screen::Starting;
        // The remembered disc, and whether the game is built from this kit.
        run_task([this] {
            auto identity = build_identity(kit_);
            if (!identity) {
                post([this, why = identity.error()] { fail_start(why); });
                return;
            }
            std::optional<fs::path> disc;
            if (const auto text = read_file(kit_.disc_file()))
                if (const auto line = trim(*text); !line.empty()) disc = fs::path(widen(line));
            DiscCheck check;
            if (disc) check = check_disc(kit_, *disc);
            post([this, built_from = *identity, disc, check] {
                identity_ = built_from;
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

    // A file dropped onto the window or chosen in the dialog (any thread).
    void offer_disc(fs::path path) {
        const std::lock_guard lock(mutex_);
        chosen_ = std::move(path);
    }

    bool busy() const { return busy_ || screen_ == Screen::Playing; }
    bool playing() const { return screen_ == Screen::Playing; }
    bool want_quit() const { return quit_; }

    // Closing the window: stop what's running (the build's tools with it).
    void stop() {
        cancel_ = true;
        stop_build_tools();
        if (worker_.joinable()) worker_.join();
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
        return !busy_ && (screen_ == Screen::NeedDisc || screen_ == Screen::Ready || screen_ == Screen::BuildFailed ||
                          screen_ == Screen::GameFailed);
    }

    void take_disc(const fs::path& disc) {
        const Screen before = screen_;
        notice_ = "Checking " + show(disc.filename()) + "...";
        run_task([this, disc, before] {
            const DiscCheck check = check_disc(kit_, disc);
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
        screen_ = Screen::Building;
        cancel_ = false;
        run_task([this, disc = *disc_, use_cache] {
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
        Command c;
        c.exe = kit_.game_exe();
        c.args = {show(*disc_)};
        c.cwd = kit_.top;
        c.out = kit_.top / "run.log";
        c.err = kit_.top / "run-errors.log";
        c.build_tool = false;
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
            quit_ = true;
            return;
        }
        SDL_ShowWindow(window_);
        SDL_RaiseWindow(window_);
        screen_ = Screen::GameFailed;
        failure_ = "The game stopped with " + describe_exit(code) + ".";
        errors_ = tail(kit_.top / "run-errors.log", 20);
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

    void open_build_log() {
        std::string url = "file:///" + show(kit_.build_log());
        for (char& c : url)
            if (c == '\\') c = '/';
        SDL_OpenURL(url.c_str());
    }

    static void dialog_done(void* self, const char* const* files, int) {
        if (files && files[0]) static_cast<App*>(self)->offer_disc(path_from_utf8(files[0]));
    }

    void choose_disc() {
        static const SDL_DialogFileFilter filters[] = {
            {"Dreamcast disc images (.cue, .gdi, .chd)", "cue;gdi;chd"},
        };
        std::string folder;
        if (disc_) folder = show(disc_->parent_path());
        SDL_ShowOpenFileDialog(dialog_done, this, window_, filters, 1, folder.empty() ? nullptr : folder.c_str(), false);
    }

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
    wrapped("This download builds the game from your own disc: " + kit_.wanted_disc() +
            ". Choose its .cue file (with the .bin tracks beside it), its .gdi or its .chd.");
    ImGui::Spacing();
    if (big_button("Choose your disc")) choose_disc();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(kQuiet, "or drag the file onto this window.");
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

    switch (screen_) {
        case Screen::Broken:
            coloured(kWarning, failure_);
            ImGui::Spacing();
            if (big_button("Quit")) quit_ = true;
            break;
        case Screen::Starting: wrapped("Checking the download and your disc..."); break;
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
                case Stage::CheckingDisc:
                    label = "Checking the disc";
                    fraction = 0.01f;
                    break;
                case Stage::Recompiling:
                    label = "Recompiling " + kit_.boot + " into C++";
                    fraction = 0.03f;
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
            wrapped("Building the game from your disc. This happens once (and after an update): a few minutes, up to 15 on a slower PC.");
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
            if (disc_) ImGui::TextColored(kQuiet, "Disc: %s", show(*disc_).c_str());
            ImGui::Spacing();
            ImGui::BeginDisabled(busy_);
            ImGui::PushFont(nullptr, style.FontSizeBase * 1.4f);
            if (big_button("  Play  ")) play();
            ImGui::PopFont();
            ImGui::Spacing();
            if (ImGui::Button("Choose another disc")) choose_disc();
            ImGui::SameLine();
            if (ImGui::Button("Build again from scratch")) {
                notice_.clear();
                start_build(false);
            }
            ImGui::EndDisabled();
            ImGui::TextColored(kQuiet, "Or drag another disc image onto this window.");
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
            if (big_button("Choose another disc")) choose_disc();
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
    // The Vulkan renderer needs a Vulkan window; without a Vulkan loader the
    // window can't be one, and the software renderer draws instead.
    SDL_Window* window = SDL_CreateWindow("QilinRecomp", width, height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");
        window = SDL_CreateWindow("QilinRecomp", width, height, SDL_WINDOW_RESIZABLE);
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
    // Segoe UI, Windows' own, reads better than the built-in pixel font.
    bool font = false;
    for (const char* name : {"SystemRoot", "windir"}) {  // SDL's lookup minds the case
        const char* windows = SDL_getenv(name);
        if (font || !windows) continue;
        const std::string segoe = std::string(windows) + "\\Fonts\\segoeui.ttf";
        std::error_code ec;
        if (fs::is_regular_file(path_from_utf8(segoe.c_str()), ec)) font = io.Fonts->AddFontFromFileTTF(segoe.c_str(), 19.0f) != nullptr;
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
