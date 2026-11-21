// The texture decoder and the renderer, without the game: synthetic textures
// decoded and checked texel by texel, then a scene drawn headless and read
// back. The renderer's part skips (exit 77) where there's no Vulkan 1.3
// device, as on CI runners; the decoder's part always runs.
#include <cstdio>
#include <cstring>
#include <vector>

#include "qilin/host/display.hpp"
#include "renderer.hpp"
#include "ta.hpp"
#include "texture.hpp"

namespace tex = toycommander::texture;
namespace ta = toycommander::ta;

namespace {

int g_failures = 0;
void expect(bool ok, const char* what) {
    if (!ok) {
        std::printf("render_test: %s\n", what);
        ++g_failures;
    }
}

// A texture control word: format, twiddled or not, address in 8-byte units.
uint32_t tcw(uint32_t format, bool twiddled, uint32_t address = 0) { return format << 27 | (twiddled ? 0u : 1u << 26) | address >> 3; }

void decoder() {
    // Twiddling: (x, y) interleaves y's bits low, x's high; a 2:1 rectangle is two squares.
    expect(tex::twiddle(0, 0, 8, 8) == 0 && tex::twiddle(0, 1, 8, 8) == 1 && tex::twiddle(1, 0, 8, 8) == 2 && tex::twiddle(1, 1, 8, 8) == 3,
           "twiddle: the first 2x2 block");
    expect(tex::twiddle(2, 0, 8, 8) == 8 && tex::twiddle(7, 7, 8, 8) == 63, "twiddle: further out");
    expect(tex::twiddle(8, 0, 16, 8) == 64 && tex::twiddle(0, 8, 8, 16) == 64, "twiddle: rectangles");

    // RGB565, raster order, 8x8: texel k = k (so red/green/blue bits vary).
    {
        tex::Params p;
        p.tcw = tcw(tex::kRgb565, false);
        std::vector<uint8_t> d(8 * 8 * 2);
        for (uint32_t k = 0; k < 64; ++k) d[k * 2] = uint8_t(k == 9 ? 0x1F : 0), d[k * 2 + 1] = uint8_t(k == 9 ? 0xF8 : 0);
        expect(tex::bytes(p) == 128, "RGB565: 128 bytes");
        std::vector<uint32_t> out(64);
        tex::decode(p, d.data(), out.data());
        expect(out[9] == 0xFFFF00FFu, "RGB565: red and blue full, opaque");  // R=255, G=0, B=255, A=255 (bytes R,G,B,A)
        expect(out[0] == 0xFF000000u, "RGB565: black, opaque");
    }
    // ARGB1555, twiddled: texel (1,0) is stored third; alpha from bit 15.
    {
        tex::Params p;
        p.tcw = tcw(tex::kArgb1555, true);
        std::vector<uint8_t> d(128, 0);
        d[2 * 2] = 0xE0, d[2 * 2 + 1] = 0x83;  // index 2: A=1, R=0, G=31, B=0
        std::vector<uint32_t> out(64);
        tex::decode(p, d.data(), out.data());
        expect(out[1] == 0xFF00FF00u, "ARGB1555 twiddled: (1,0) green, opaque");
        expect(out[0] == 0u, "ARGB1555 twiddled: (0,0) clear");
    }
    // ARGB4444 mip-mapped 8x8: the largest level after 1+4+16 texels and 6 bytes of padding... (2 + 6 bytes: 1x1 sits at +6).
    {
        tex::Params p;
        p.tcw = tcw(tex::kArgb4444, true) | 1u << 31;
        expect(tex::bytes(p) == (1 + 4 + 16) * 2 + 6 + 128, "ARGB4444 mip-mapped: offset to the largest level");
    }
    // 8-bit palette, through ARGB8888 palette entries.
    {
        std::vector<uint32_t> pal(1024, 0);
        pal[256 + 5] = 0x80112233u;
        tex::Params p;
        p.tcw = tcw(tex::kPalette8, true) | 1u << 25;  // palette bank 1 (entries 256-511)
        p.palette = pal.data(), p.palette_format = 3;
        std::vector<uint8_t> d(64, 0);
        d[3] = 5;  // twiddled index 3: (1,1)
        std::vector<uint32_t> out(64);
        tex::decode(p, d.data(), out.data());
        expect(out[9] == 0x80332211u, "8-bit palette: entry 261 at (1,1)");
    }
    // Vector quantised RGB565: codebook entry 1's four texels, all white, fill the top-left 2x2.
    {
        tex::Params p;
        p.tcw = tcw(tex::kRgb565, true) | 1u << 30;
        std::vector<uint8_t> d(2048 + 16, 0);
        for (int k = 0; k < 4; ++k) d[(4 + k) * 2] = 0xFF, d[(4 + k) * 2 + 1] = 0xFF;
        d[2048] = 1;
        expect(tex::bytes(p) == 2048 + 16, "VQ: codebook and indices");
        std::vector<uint32_t> out(64);
        tex::decode(p, d.data(), out.data());
        expect(out[0] == 0xFFFFFFFFu && out[9] == 0xFFFFFFFFu && out[2] == 0xFF000000u, "VQ: one 2x2 block");
    }
}

bool vulkan_13_device() {
    if (volkInitialize() != VK_SUCCESS || volkGetInstanceVersion() < VK_API_VERSION_1_3) return false;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    VkInstance inst;
    if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) return false;
    volkLoadInstance(inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(inst, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(inst, &n, devs.data());
    bool ok = false;
    for (VkPhysicalDevice d : devs) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        ok |= p.apiVersion >= VK_API_VERSION_1_3;
    }
    vkDestroyInstance(inst, nullptr);
    return ok;
}

// Video memory for the scene: an 8x8 RGB565 raster texture, all blue, at 0.
uint8_t g_vram[128];
void read_vram(uint32_t offset, uint8_t* out, uint32_t n) { std::memcpy(out, g_vram + offset, n); }
uint64_t generation(uint32_t, uint32_t) { return 1; }

ta::Vertex vertex(float x, float y, float z, float r, float g, float b, float u = 0, float v = 0) {
    ta::Vertex o{};
    o.x = x, o.y = y, o.z = z, o.u[0] = u, o.v[0] = v;
    o.base[0][0] = r, o.base[0][1] = g, o.base[0][2] = b, o.base[0][3] = 1.0f;
    return o;
}

int renderer() {
    if (!vulkan_13_device()) {
        std::puts("render_test: no Vulkan 1.3 device here: the renderer's part skipped");
        return 77;
    }
    qilin::host::Display::Options o;
    o.headless = true, o.width = 64, o.height = 48;
    qilin::host::Display display(o);
    toycommander::renderer::Renderer r(display.gpu(), 1);
    for (int k = 0; k < 64; ++k) g_vram[k * 2] = 0x1F, g_vram[k * 2 + 1] = 0;  // RGB565 blue

    ta::Scene scene;
    // Opaque, Gouraud, depth "greater or equal": a far red quad over the whole 64x48 picture,
    // then a near green one over its left half, then a farther blue one over the right half
    // (hidden by the red), then a textured quad (blue texels, modulated by white) top right.
    const uint32_t gouraud = 0x02u, isp_ge = 6u << 29;
    auto quad = [&](float x0, float y0, float x1, float y1, float z, float r_, float g_, float b_, bool textured) {
        ta::Strip s{};
        s.list = ta::kOpaque;
        s.pcw = gouraud | (textured ? 0x08u : 0u);
        s.isp = isp_ge;
        s.textured = textured;
        s.tsp[0] = 1u << 6;  // modulate; 8x8
        s.tcw[0] = 1u << 27 | 1u << 26;  // RGB565, raster, at 0
        s.first = uint32_t(scene.vertices.size()), s.count = 4;
        scene.vertices.push_back(vertex(x0, y0, z, r_, g_, b_, 0, 0));
        scene.vertices.push_back(vertex(x1, y0, z, r_, g_, b_, 1, 0));
        scene.vertices.push_back(vertex(x0, y1, z, r_, g_, b_, 0, 1));
        scene.vertices.push_back(vertex(x1, y1, z, r_, g_, b_, 1, 1));
        scene.strips.push_back(s);
    };
    quad(0, 0, 64, 48, 0.1f, 1, 0, 0, false);
    quad(0, 0, 32, 48, 0.5f, 0, 1, 0, false);
    quad(32, 24, 64, 48, 0.05f, 0, 0, 1, false);
    quad(48, 0, 64, 16, 0.2f, 1, 1, 1, true);
    toycommander::renderer::Source src;
    src.scene = &scene, src.width = 64, src.height = 48;
    src.read = read_vram, src.generation = generation;
    r.draw(src);
    const std::vector<uint8_t> px = r.read_back();
    auto at = [&](int x, int y) { return &px[size_t(y * 64 + x) * 4]; };
    auto is = [&](int x, int y, uint8_t red, uint8_t green, uint8_t blue) {
        const uint8_t* p = at(x, y);
        return p[0] == red && p[1] == green && p[2] == blue;
    };
    expect(is(10, 10, 0, 255, 0), "renderer: the near quad covers the far one");
    expect(is(40, 40, 255, 0, 0), "renderer: the farther quad is hidden");
    expect(is(40, 10, 255, 0, 0), "renderer: the far quad shows where nothing is nearer");
    expect(is(56, 8, 0, 0, 255), "renderer: the texture, modulated by white");
    expect(r.stats().textures == 1, "renderer: one texture decoded");
    // Drawn again with nothing changed: no new upload.
    r.draw(src);
    expect(r.stats().uploads == 0, "renderer: a texture unchanged isn't uploaded again");
    return 0;
}

}  // namespace

int main() {
    decoder();
    const int r = renderer();
    std::printf("render_test: %s\n", g_failures ? "FAILED" : r == 77 ? "decoder passed (renderer skipped)" : "passed");
    if (g_failures) return 1;
    return r;
}
