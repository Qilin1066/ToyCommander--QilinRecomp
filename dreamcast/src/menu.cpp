// The settings menu (menu.hpp), from Punch-Out's, kept in step. Runtime code: no std::format.
#include "menu.hpp"

#include <cmath>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "system.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "?"
#endif

namespace dreamcast {
namespace host = qilin::host;

namespace {

constexpr uint64_t kHintMs = 8000;     // how long the hint shows at the start
constexpr uint64_t kCaptureMs = 6000;  // how long a control waits to be pressed
constexpr const char* kHint = "F1 or Esc (controller: Back + Start): settings";
constexpr const char* kLanguages[] = {"", "English", "German", "French", "Spanish"};

// ImGui's Vulkan backend loads its functions through volk's loader.
VkInstance g_instance = VK_NULL_HANDLE;
PFN_vkVoidFunction load(const char* name, void*) { return vkGetInstanceProcAddr(g_instance, name); }
void check(VkResult r) {
    if (r < 0) qilin::rt::log("settings menu: Vulkan error %d", int(r));
}

uint32_t lowest_bit(uint32_t v) { return v & (~v + 1); }

}  // namespace

Menu::Menu(host::Display& display, Settings& settings, std::filesystem::path folder)
    : display_(display), s_(settings), folder_(std::move(folder)) {
    auto* window = static_cast<SDL_Window*>(display.sdl_window());
    if (!window) return;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // the settings file is the only file
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    // 1.25x, as the other recomps have it, times the display's own scale.
    float dpi = SDL_GetWindowDisplayScale(window);
    if (!(dpi > 0.0f)) dpi = 1.0f;
    ImGui::GetStyle().ScaleAllSizes(1.25f * dpi);
    ImGui::GetStyle().FontScaleMain = 1.25f;
    ImGui::GetStyle().FontScaleDpi = dpi;

    sdl_backend_ = ImGui_ImplSDL3_InitForVulkan(window);
    // Every controller works the menu, not only the first one SDL lists
    // (which can be a virtual pad, or a wheel's base).
    if (sdl_backend_) ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoAll);
    host::Gpu& gpu = display.gpu();
    g_instance = gpu.instance;
    if (!sdl_backend_ || !ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, load)) {
        qilin::rt::log("the settings menu can't start (its SDL or Vulkan side)");
        return;
    }
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = gpu.instance;
    ii.PhysicalDevice = gpu.physical;
    ii.Device = gpu.device;
    ii.QueueFamily = gpu.family;
    ii.Queue = gpu.queue;
    ii.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLER_POOL_SIZE;
    ii.MinImageCount = std::max(2u, display.swap_image_count());
    ii.ImageCount = ii.MinImageCount;
    ii.UseDynamicRendering = true;
    static VkFormat format;  // the backend keeps a pointer to it
    format = display.swap_format();
    VkPipelineRenderingCreateInfoKHR& pr = ii.PipelineInfoMain.PipelineRenderingCreateInfo;
    pr = {};
    pr.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
    pr.colorAttachmentCount = 1, pr.pColorAttachmentFormats = &format;
    ii.CheckVkResultFn = check;
    vulkan_backend_ = ImGui_ImplVulkan_Init(&ii);
    if (!vulkan_backend_) {
        qilin::rt::log("the settings menu can't start (its Vulkan side)");
        return;
    }
    display.set_overlay([this](VkCommandBuffer cb) {
        if (drawn_) ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cb);
    });
    display.set_event_hook([this](const void* e) { return handle(e); });
    ok_ = true;
    hint_ = true;
    hint_until_ = SDL_GetTicks() + kHintMs;
    qilin::rt::log("%s", kHint);
}

Menu::~Menu() {
    display_.set_overlay(nullptr);
    display_.set_event_hook(nullptr);
    if (vulkan_backend_) {
        vkDeviceWaitIdle(display_.gpu().device);
        ImGui_ImplVulkan_Shutdown();
    }
    if (sdl_backend_) ImGui_ImplSDL3_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
}

bool Menu::take_controls_changed() {
    const bool was = controls_changed_;
    controls_changed_ = false;
    return was;
}

bool Menu::handle(const void* event) {
    const SDL_Event& e = *static_cast<const SDL_Event*>(event);
    if (!ok_) return false;
    // Another window has the keyboard: what it types isn't the menu's.
    if (!display_.focused() && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                                e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_WHEEL ||
                                e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP ||
                                e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION || e.type == SDL_EVENT_TEXT_INPUT))
        return false;
    // Looking at the picture: any key or button brings the menu back.
    if (peek_ && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN)) {
        peek_ = false;
        return true;
    }
    // Setting a control: the key pressed (pad buttons and clicks are read in tick).
    if (capture_ >= 0 && e.type == SDL_EVENT_KEY_DOWN) {
        if (!e.key.repeat) {
            const char* name = SDL_GetScancodeName(e.key.scancode);
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                capture_ = -1, wait_release_ = true, note_.clear();
            } else if (name && *name && !key_reserved(name)) {
                bind({Binding::kKey, 0, name});
            }
        }
        return true;
    }
    const bool shown = open_ || opening_;
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!e.key.repeat && (e.key.key == SDLK_F1 || e.key.key == SDLK_ESCAPE)) {
                toggle();
                return true;
            }
            if (!e.key.repeat && e.key.key == SDLK_F3) {  // the frame-time readout, kept in the settings
                s_.performance_overlay = !s_.performance_overlay;
                if (!save_settings(folder_, s_))
                    qilin::rt::log("the settings can't be saved to %s", settings_file(folder_).string().c_str());
                return true;
            }
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN: {
            if (capture_ >= 0) break;
            // Back and Start together; with the menu open, Start alone closes it too.
            SDL_Gamepad* pad = SDL_GetGamepadFromID(e.gbutton.which);
            const bool back = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK);
            const bool start = pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START);
            if ((back && start) || (open_ && e.gbutton.button == SDL_GAMEPAD_BUTTON_START)) {
                toggle();
                return true;
            }
            break;
        }
        default: break;
    }
    if (open_) ImGui_ImplSDL3_ProcessEvent(&e);
    if (!shown) return false;
    switch (e.type) {  // the game doesn't see input while the menu is up
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
        case SDL_EVENT_TEXT_INPUT:
        case SDL_EVENT_MOUSE_MOTION:
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
        case SDL_EVENT_MOUSE_WHEEL:
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: return true;
        default: return false;
    }
}

void Menu::toggle() {
    if (open_ || opening_) {
        close();
        return;
    }
    opening_ = true;  // the game parks at its next vertical blank (video.cpp); tick() opens then
}

void Menu::close() {
    if (open_) {
        s_.fullscreen = display_.fullscreen();
        if (!save_settings(folder_, s_))
            qilin::rt::log("the settings can't be saved to %s", settings_file(folder_).string().c_str());
    }
    open_ = opening_ = peek_ = false;
    capture_ = -1;
    drawn_ = false;
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad) & ~ImGuiConfigFlags_NoMouse;
    wait_release_ = false;
}

void Menu::bind(const Binding& b) {
    note_.clear();
    for (unsigned c = 0; c < kControlCount; ++c) {
        auto& list = s_.controls[c];
        const auto it = std::find(list.begin(), list.end(), b);
        if (it == list.end()) continue;
        if (int(c) != capture_) note_ = binding_label(b) + " moved here from " + control_label(Control(c)) + ".";
        list.erase(it);
    }
    s_.controls[capture_].push_back(b);
    controls_changed_ = true;
    capture_ = -1;
    wait_release_ = true;
}

void Menu::tick() {
    if (!ok_) return;
    if (opening_ && parked_) {
        opening_ = false, open_ = true, hint_ = false;
        ImGui::GetIO().ClearInputKeys();
    }
    ImGuiIO& io = ImGui::GetIO();
    if (open_ && capture_ >= 0) {
        // A pad button or a click pressed since it began sets the control.
        const uint32_t held = display_.buttons();
        const uint32_t fresh = held & ~capture_held_;
        capture_held_ &= held;  // let go, a button counts again
        if (fresh) {
            const uint32_t bit = lowest_bit(fresh);
            bind(bit == host::kPadClick ? Binding{Binding::kMouse, 0, {}} : Binding{Binding::kPad, bit, {}});
        } else if (SDL_GetTicks() >= capture_until_) {
            capture_ = -1, wait_release_ = true;
            note_ = "Nothing was pressed: unchanged.";
        }
    }
    // The mouse and the pad's navigation come back once the press that set a control is let go.
    if (wait_release_ && display_.buttons() == 0) {
        io.ConfigFlags = (io.ConfigFlags | ImGuiConfigFlags_NavEnableGamepad) & ~ImGuiConfigFlags_NoMouse;
        wait_release_ = false;
    }
    if (open_) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (!peek_) draw_menu();
        ImGui::Render();
        drawn_ = true;
        display_.set_picture(picture_of(s_));  // shown at once, under the menu
        if (close_asked_ || quit_) {
            close_asked_ = false;
            if (quit_) {
                s_.fullscreen = display_.fullscreen();
                if (!save_settings(folder_, s_))
                    qilin::rt::log("the settings can't be saved to %s", settings_file(folder_).string().c_str());
            } else {
                close();
            }
        }
        return;
    }
    if (hint_ && SDL_GetTicks() >= hint_until_) hint_ = false;
    if (hint_ || s_.performance_overlay) {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        if (hint_) draw_hint();
        if (s_.performance_overlay) draw_frame_times();
        ImGui::Render();
        drawn_ = true;
        return;
    }
    drawn_ = false;
}

bool Menu::frame_times_shown() const { return s_.performance_overlay; }

// F3: how evenly frames reach the screen, top left. The lows are the frame
// rates of the slowest 1% and 0.1% of frames over the last 20 s.
void Menu::draw_frame_times() {
    const host::FrameTimes& t = display_.frame_times();
    const host::FrameTimes::Summary s = t.summary();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 8.0f, vp->WorkPos.y + 8.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##frame times", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    if (s.frames == 0) {
        ImGui::TextUnformatted("frame times: waiting for frames");
    } else {
        ImGui::Text("%.1f fps  %.2f ms", s.fps, s.average_ms);
        ImGui::Text("1%% low %.1f  0.1%% low %.1f", s.low1_fps, s.low01_fps);
        ImGui::Text("worst %.1f ms", s.worst_ms);
        const std::vector<float> recent = t.recent(240);
        ImGui::PlotLines("##ft", recent.data(), int(recent.size()), 0, nullptr, 0.0f, 50.0f, ImVec2(240.0f, 48.0f));
    }
    for (const std::string& line : frame_details_) ImGui::TextUnformatted(line.c_str());
    ImGui::End();
}

void Menu::draw_hint() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float pad = 12.0f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + pad, vp->WorkPos.y + vp->WorkSize.y - pad), ImGuiCond_Always, ImVec2(0, 1));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##hint", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(kHint);
    ImGui::End();
}

void Menu::draw_menu() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("Settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextUnformatted(QILIN_APP_TITLE " - settings (the game is paused)");
    // Close and Quit, top right on every page (Quit was at the bottom of the Game page, easy to miss).
    const ImGuiStyle& st = ImGui::GetStyle();
    const float buttons = ImGui::CalcTextSize("Close (Esc)").x + ImGui::CalcTextSize("Quit the game").x + 4 * st.FramePadding.x + st.ItemSpacing.x;
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - buttons + ImGui::GetCursorPosX());
    if (ImGui::Button("Close (Esc)")) close_asked_ = true;  // tick() closes it, once the frame is built
    ImGui::SameLine();
    if (ImGui::Button("Quit the game")) quit_ = true;
    ImGui::Separator();
    if (ImGui::BeginTabBar("pages")) {
        if (ImGui::BeginTabItem("Controls")) {
            controls_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            display_page();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Game")) {
            game_page();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void Menu::controls_page() {
    ImGui::SeparatorText("Controllers");
    if (display_.controllers_connected() == 0) ImGui::TextDisabled("None connected: the keyboard plays the Dreamcast controller on port A.");
    else ImGui::TextDisabled("Each controller takes the next port when it first has a button pressed.");
    for (unsigned port = 0; port < 4; ++port) {
        const std::string name = display_.player_controller(port);
        if (name.empty()) ImGui::BulletText("Port %c: %s", 'A' + port, port == 0 ? "the keyboard (and the first controller to press a button)" : "nobody yet");
        else ImGui::BulletText("Port %c: %s%s", 'A' + port, name.c_str(), port == 0 ? ", and the keyboard" : "");
    }
    if (ImGui::Button("Choose the ports again")) display_.reset_player_order();
    ImGui::SameLine();
    ImGui::TextDisabled("(then press a button on each controller, in port order)");
    ImGui::Checkbox("Rumble", &s_.rumble);
    int strength = int(s_.rumble_strength);
    ImGui::BeginDisabled(!s_.rumble);
    if (ImGui::SliderInt("Rumble strength", &strength, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.rumble_strength = unsigned(strength);
    ImGui::EndDisabled();

    ImGui::SeparatorText("The Dreamcast controller (a pad's left stick and triggers work its own in full)");
    ImGuiIO& io = ImGui::GetIO();
    if (ImGui::BeginTable("bindings", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Button", ImGuiTableColumnFlags_WidthStretch, 0.28f);
        ImGui::TableSetupColumn("Pressed by (click one to remove it)", ImGuiTableColumnFlags_WidthStretch, 0.57f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 0.15f);
        ImGui::TableHeadersRow();
        for (unsigned c = 0; c < kControlCount; ++c) {
            ImGui::PushID(int(c));
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(control_label(Control(c)));
            ImGui::TableNextColumn();
            if (capture_ == int(c)) {
                const uint64_t left = capture_until_ > SDL_GetTicks() ? (capture_until_ - SDL_GetTicks() + 999) / 1000 : 0;
                ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Press a key, a pad button or click... (Esc: cancel, %u s)",
                                   unsigned(left));
            } else {
                auto& list = s_.controls[c];
                if (list.empty()) ImGui::TextDisabled("unused");
                for (size_t i = 0; i < list.size(); ++i) {
                    ImGui::PushID(int(i));
                    if (i) ImGui::SameLine();
                    if (ImGui::SmallButton(binding_label(list[i]).c_str())) {
                        list.erase(list.begin() + std::ptrdiff_t(i));
                        controls_changed_ = true;
                        note_.clear();
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capture_ >= 0);
            if (ImGui::SmallButton("Add")) {
                capture_ = int(c);
                capture_until_ = SDL_GetTicks() + kCaptureMs;
                capture_held_ = display_.buttons();
                note_.clear();
                // The mouse and the pad's navigation are the control's to take until it's set.
                io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableGamepad) | ImGuiConfigFlags_NoMouse;
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (!note_.empty()) ImGui::TextUnformatted(note_.c_str());
    if (ImGui::Button("Back to the defaults")) {
        const Settings d = default_settings();
        for (unsigned c = 0; c < kControlCount; ++c) s_.controls[c] = d.controls[c];
        controls_changed_ = true;
        note_.clear();
    }
    ImGui::TextDisabled("An input on one control moves there from another.");
    ImGui::TextDisabled("F1 or Esc (controller: Back + Start): this menu. F3: frame times. F11 or Alt+Enter: full screen.");
}

void Menu::display_page() {
    ImGui::SeparatorText("Window");
    bool full = display_.fullscreen();
    if (ImGui::Checkbox("Full screen (F11 or Alt+Enter)", &full)) {
        display_.set_fullscreen(full);
        s_.fullscreen = full;
    }
    {
        const std::string current = s_.monitor.empty() ? "The main display" : s_.monitor;
        if (ImGui::BeginCombo("Display", current.c_str())) {
            if (ImGui::Selectable("The main display", s_.monitor.empty())) {
                s_.monitor.clear();
                display_.move_to("");
            }
            const std::vector<std::string> names = display_.monitors();
            for (size_t i = 0; i < names.size(); ++i) {
                ImGui::PushID(int(i));
                if (ImGui::Selectable(names[i].c_str(), names[i] == s_.monitor)) {
                    s_.monitor = names[i];
                    display_.move_to(names[i]);
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
    }

    ImGui::SeparatorText("Picture");
    int mode = int(s_.scaling);
    ImGui::RadioButton("Whole multiples (sharpest)", &mode, int(host::Scaling::integer));
    ImGui::SameLine();
    ImGui::RadioButton("Fit", &mode, int(host::Scaling::fit));
    ImGui::SameLine();
    ImGui::RadioButton("Fill the window", &mode, int(host::Scaling::stretch));
    s_.scaling = host::Scaling(mode);
    int look = int(s_.look);
    ImGui::RadioButton("Plain", &look, int(host::Look::plain));
    ImGui::SameLine();
    ImGui::RadioButton("CRT television", &look, int(host::Look::crt));
    s_.look = host::Look(look);
    if (s_.look == host::Look::crt) {
        int scan = int(s_.scanlines), mask = int(s_.mask);
        if (ImGui::SliderInt("Scanlines", &scan, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.scanlines = unsigned(scan);
        if (ImGui::SliderInt("Phosphor stripes", &mask, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.mask = unsigned(mask);
        ImGui::TextDisabled("The raster is the console's own lines: it shows best at Whole multiples, or a large window.");
    } else {
        ImGui::Checkbox("Smooth", &s_.smooth);
        ImGui::SameLine();
    }
    ImGui::Checkbox("Vsync", &s_.vsync);
    ImGui::Checkbox("Frame times (F3)", &s_.performance_overlay);
    ImGui::SeparatorText("Refresh rate");
    const bool was_match = s_.match_refresh, was_vrr = s_.variable_refresh;
    ImGui::Checkbox("Fullscreen at a multiple of 60 Hz (120 Hz on a 144 Hz monitor)", &s_.match_refresh);
    ImGui::Checkbox("Variable refresh (G-Sync, FreeSync: on in the driver too)", &s_.variable_refresh);
    if ((was_match != s_.match_refresh || was_vrr != s_.variable_refresh) && display_.fullscreen()) {
        display_.set_picture(picture_of(s_));
        display_.set_fullscreen(true);  // the mode for the new choice
    }
    if (!display_.mode_note().empty()) ImGui::TextWrapped("%s", display_.mode_note().c_str());
    if (ImGui::Button("Look at the picture")) peek_ = true;
    ImGui::SameLine();
    ImGui::TextDisabled("(any key or button comes back)");

    ImGui::SeparatorText("Enhancements");
    static constexpr const char* kPresets[] = {"original", "low-end", "desktop"};
    if (ImGui::BeginCombo("Preset", s_.preset.c_str())) {
        for (const char* p : kPresets)
            if (ImGui::Selectable(p, s_.preset == p)) apply_preset(s_, p);
        ImGui::EndCombo();
    }
    int scale = int(s_.scale);
    char label[96];
    if (scale == 0) std::snprintf(label, sizeof label, "Auto (the screen's height: %ux now)", running_scale_);
    else std::snprintf(label, sizeof label, "%dx (%dx%d)", scale, 640 * scale, 480 * scale);
    if (ImGui::SliderInt("Internal resolution", &scale, 0, 8, label, ImGuiSliderFlags_AlwaysClamp)) s_.scale = unsigned(scale);
    ImGui::TextDisabled("The GPU's work grows with the square: 8x is 64 times the pixels of 1x, 4x 16 times.");
    if (s_.scale != 0 && s_.scale != running_scale_)
        ImGui::TextDisabled("The game runs at %ux now: %ux from the next start.", running_scale_, s_.scale);
    const float limit = display_.gpu().max_anisotropy;
    ImGui::BeginDisabled(limit <= 1.0f);
    static constexpr unsigned kAniso[] = {1, 2, 4, 8, 16};
    std::snprintf(label, sizeof label, s_.anisotropy == 1 ? "%ux (the console's)" : "%ux", s_.anisotropy);
    if (ImGui::BeginCombo("Anisotropic filtering", label)) {
        for (unsigned a : kAniso) {
            if (float(a) > limit && a > 1) continue;
            std::snprintf(label, sizeof label, a == 1 ? "%ux (the console's)" : "%ux", a);
            if (ImGui::Selectable(label, s_.anisotropy == a)) s_.anisotropy = a;
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    if (limit <= 1.0f) ImGui::TextDisabled("This GPU has no anisotropic filtering.");
    else ImGui::TextDisabled("For the textures the game filters; its unfiltered ones stay as they are.");

    ImGui::SeparatorText("Graphics device");
    const std::string chosen = s_.gpu.empty() ? "The best one" : s_.gpu;
    if (ImGui::BeginCombo("Device", chosen.c_str())) {
        if (ImGui::Selectable("The best one", s_.gpu.empty())) s_.gpu.clear();
        for (const std::string& name : display_.gpu().usable)
            if (ImGui::Selectable(name.c_str(), name == s_.gpu)) s_.gpu = name;
        ImGui::EndCombo();
    }
    ImGui::TextDisabled("In use: %s. A change takes effect the next time the game starts.", display_.device_name().c_str());
}

void Menu::game_page() {
    ImGui::SeparatorText("Save recovery");
    ImGui::TextWrapped("The five latest previous copies of the memory card and of the console's flash memory are kept, each from "
                       "before a save. Restore restarts the game; the present save is kept as a copy first.");
    if (ImGui::TreeNode("Previous saves")) {
        if (!history_loaded_ || ImGui::Button("Refresh previous saves")) save_history_ = save_backups(folder_), history_loaded_ = true;
        if (save_history_.empty()) ImGui::TextDisabled("No previous saves yet.");
        for (size_t i = 0; i < save_history_.size(); ++i) {
            ImGui::PushID(int(i));
            ImGui::TextWrapped("%s", save_history_[i].label.c_str());
            ImGui::SameLine();
            if (ImGui::Button("Restore and restart")) {
                s_.fullscreen = display_.fullscreen();
                if (!save_settings(folder_, s_)) note_ = "The settings couldn't be saved: the restore wasn't asked for.";
                else if (request_save_restore(folder_, save_history_[i].file, note_)) restart_program();
            }
            ImGui::PopID();
        }
        ImGui::TreePop();
    }
    if (!note_.empty()) ImGui::TextWrapped("%s", note_.c_str());

    ImGui::SeparatorText("The Dreamcast's own settings (read the next time the game starts)");
    const int lang = std::clamp(int(s_.language), 1, 4);  // the disc's languages
    if (ImGui::BeginCombo("Language", kLanguages[lang])) {
        for (int i = 1; i <= 4; ++i)
            if (ImGui::Selectable(kLanguages[i], i == lang)) s_.language = unsigned(i);
        ImGui::EndCombo();
    }
    static constexpr const char* kSound[] = {"Mono", "Stereo"};
    int sound = s_.stereo ? 1 : 0;
    if (ImGui::Combo("Sound", &sound, kSound, 2)) s_.stereo = sound == 1;

    ImGui::SeparatorText("Sound");
    int volume = int(s_.volume);
    if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp)) s_.volume = unsigned(volume);
    ImGui::Checkbox("Mute when another window has the keyboard", &s_.mute_unfocused);

    ImGui::SeparatorText("About");
    ImGui::TextWrapped(QILIN_APP_TITLE " QilinRecomp %s: a static recompilation, the game's own code compiled for this computer.",
                       QILIN_VERSION);
    ImGui::TextWrapped("Graphics: Vulkan. Window, sound and input: SDL3 (zlib). Menu: Dear ImGui (MIT).");
    ImGui::TextWrapped("Settings file: %s", settings_file(folder_).string().c_str());
    ImGui::TextWrapped("Saves and the memory card: %s", folder_.string().c_str());
}

}  // namespace dreamcast
