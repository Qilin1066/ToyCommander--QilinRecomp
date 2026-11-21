// The port's screen (video.hpp).
#include "video.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "guest.hpp"
#include "pvr.hpp"
#include "qilin/host/display.hpp"
#include "renderer.hpp"
#include "system.hpp"
#include "ta.hpp"

namespace toycommander::video {

namespace rt = qilin::rt;

namespace {

std::unique_ptr<qilin::host::Display> g_display;
std::unique_ptr<renderer::Renderer> g_renderer;
std::thread::id g_thread;
std::string g_shot_folder;
uint64_t g_shot_every = 60, g_pictures = 0;

// The registers the renderer reads (the console's names).
constexpr uint32_t kParamBase = 0x005F8020u, kFbXClip = 0x005F8068u, kFbYClip = 0x005F806Cu, kIspBackgndD = 0x005F8088u,
                   kIspBackgndT = 0x005F808Cu, kTextControl = 0x005F80E4u, kPalRamCtrl = 0x005F8108u, kPtAlphaRef = 0x005F811Cu;

float as_float(uint32_t w) {
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

// The background plane: ISP_BACKGND_T points at its instruction words and
// three vertices in the parameter memory (video memory's 32-bit view), which
// the game's library writes there itself. Drawn as the parallelogram they
// span, at ISP_BACKGND_D's depth.
void background(renderer::Source& src) {
    const uint32_t t = pvr_register(kIspBackgndT);
    const uint32_t at = (pvr_register(kParamBase) & 0x00F00000u) + ((t >> 3) & 0x1FFFFFu) * 4u;
    const uint32_t skip = (t >> 24) & 7u, vertex_words = 3u + skip, offset = t & 7u;
    if (t & (1u << 27)) throw rt::Stop{rt::Stop::Unsupported, 0, t};  // a background with two volumes
    if (at + (3u + (offset + 3u) * vertex_words) * 4u > (8u << 20)) throw rt::Stop{rt::Stop::Unsupported, 0, t};
    const auto word = [&](uint32_t k) {
        uint32_t w;
        std::memcpy(&w, vram() + at + k * 4u, 4);
        return w;
    };
    ta::Strip& s = src.background;
    s = {};
    s.list = ta::kOpaque;
    s.isp = word(0), s.tsp[0] = word(1), s.tcw[0] = word(2);
    s.textured = (s.isp >> 25) & 1u;
    const bool with_offset = (s.isp >> 24) & 1u, uv16 = (s.isp >> 22) & 1u;
    // The strip's PCW bits the renderer reads: texture, offset, Gouraud.
    s.pcw = (s.textured ? 0x08u : 0u) | (with_offset ? 0x04u : 0u) | ((s.isp >> 23) & 1u ? 0x02u : 0u);
    float depth = as_float(pvr_register(kIspBackgndD) & ~0xFu);
    ta::Vertex* v = src.background_vertices;
    for (uint32_t n = 0; n < 3; ++n) {
        uint32_t k = 3u + (offset + n) * vertex_words;
        ta::Vertex& o = v[n];
        o = {};
        o.x = as_float(word(k)), o.y = as_float(word(k + 1)), o.z = depth > 0.0f ? depth : as_float(word(k + 2));
        k += 3;
        if (s.textured) {
            if (uv16) {
                o.u[0] = as_float(word(k) & 0xFFFF0000u), o.v[0] = as_float(word(k) << 16);
                k += 1;
            } else {
                o.u[0] = as_float(word(k)), o.v[0] = as_float(word(k + 1));
                k += 2;
            }
        }
        const auto packed = [](uint32_t argb, float out[4]) {
            out[0] = float((argb >> 16) & 255u) / 255.0f, out[1] = float((argb >> 8) & 255u) / 255.0f;
            out[2] = float(argb & 255u) / 255.0f, out[3] = float(argb >> 24) / 255.0f;
        };
        packed(word(k), o.base[0]);
        if (with_offset) packed(word(k + 1), o.offset[0]);
        if (!std::isfinite(o.x) || !std::isfinite(o.y)) return;  // nothing set yet: no plane
    }
    // A strip A, B, C, D with D completing the parallelogram.
    v[3] = v[2];
    v[3].x = v[1].x + v[2].x - v[0].x, v[3].y = v[1].y + v[2].y - v[0].y;
    v[3].u[0] = v[1].u[0] + v[2].u[0] - v[0].u[0], v[3].v[0] = v[1].v[0] + v[2].v[0] - v[0].v[0];
    s.first = 0, s.count = 4;
}

void shot(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h) {
    const std::string path = g_shot_folder + "/frame_" + std::to_string(g_pictures) + ".ppm";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) rt::fatal("can't write %s", path.c_str());
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (size_t k = 0; k < rgba.size(); k += 4) std::fwrite(rgba.data() + k, 1, 3, f);
    std::fclose(f);
}

}  // namespace

void start() {
    qilin::host::Display::Options o;
    o.width = 1280, o.height = 960;
    const char* headless = std::getenv("QILIN_HEADLESS");
    o.headless = headless && *headless && *headless != '0';
    g_display = std::make_unique<qilin::host::Display>(o);
    g_thread = std::this_thread::get_id();
    uint32_t scale = 2;
    if (const char* s = std::getenv("QILIN_SCALE")) scale = static_cast<uint32_t>(std::atoi(s));
    if (scale < 1 || scale > 8) rt::fatal("QILIN_SCALE must be 1 to 8");
    g_renderer = std::make_unique<renderer::Renderer>(g_display->gpu(), scale);
    if (const char* s = std::getenv("QILIN_SHOT")) {
        g_shot_folder = s;
        if (const size_t comma = g_shot_folder.find(','); comma != std::string::npos) {
            g_shot_every = std::strtoull(g_shot_folder.c_str() + comma + 1, nullptr, 10);
            g_shot_folder.resize(comma);
            if (g_shot_every == 0) rt::fatal("QILIN_SHOT: every 0th picture?");
        }
    }
    std::fprintf(stderr, QILIN_APP ": drawing with %s\n", g_display->device_name().c_str());
}

void render() {
    if (!g_renderer) return;
    renderer::Source src;
    src.scene = &ta::scene();
    // The picture's size: the frame buffer clip's (inclusive) ends.
    const uint32_t x_end = (pvr_register(kFbXClip) >> 16) & 0x7FFu, y_end = (pvr_register(kFbYClip) >> 16) & 0x3FFu;
    if (x_end && y_end) src.width = x_end + 1, src.height = y_end + 1;
    src.palette_format = pvr_register(kPalRamCtrl) & 3u;
    src.stride = pvr_register(kTextControl) & 31u;
    src.punch_alpha_ref = pvr_register(kPtAlphaRef);
    src.palette = pvr_palette();
    src.read = vram64_read;
    src.generation = vram64_generation;
    background(src);
    const renderer::Picture p = g_renderer->draw(src);
    ++g_pictures;
    if (!g_shot_folder.empty() && g_pictures % g_shot_every == 0) shot(g_renderer->read_back(), p.width, p.height);
    g_display->present_image(p.image, p.view, p.width, p.height, 4.0f / 3.0f, src.height);
    poll();
}

void poll() {
    if (!g_display || std::this_thread::get_id() != g_thread) return;
    if (!g_display->poll()) {
        std::fflush(nullptr);
        std::_Exit(0);
    }
}

}  // namespace toycommander::video
