// The texture decoder and the renderer, without the game: synthetic textures
// decoded and checked texel by texel, then a scene drawn headless and read
// back. The renderer's part skips (exit 77) where there's no Vulkan 1.3
// device, as on CI runners; the decoder's part always runs.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <vector>

#include "qilin/host/display.hpp"
#include "renderer.hpp"
#include "ta.hpp"
#include "texture.hpp"

namespace tex = dreamcast::texture;
namespace ta = dreamcast::ta;

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
        expect(tex::levels(p) == 4, "ARGB4444 mip-mapped 8x8: four levels");
        // Each level's first texel marked: 1x1 at byte 6, 2x2 at 8, 4x4 at 16, 8x8 at 48.
        std::vector<uint8_t> d(tex::bytes(p), 0);
        const uint32_t at[4] = {48, 16, 8, 6};
        for (uint32_t l = 0; l < 4; ++l) d[at[l]] = uint8_t(0x0F), d[at[l] + 1] = uint8_t(0xF0 | l);  // A=15, R=l, B=15
        std::vector<uint32_t> out(64);
        for (uint32_t l = 0; l < 4; ++l) {
            tex::decode_level(p, d.data(), l, out.data());
            expect(out[0] == (0xFFu << 24 | 0xFFu << 16 | (l * 17u)), "ARGB4444 mip-mapped: each level from its own place");
        }
    }
    // 4-bit palette, mip-mapped 4x4: the 1x1 level is nibble 3 (byte 1's high nibble), 2x2 from byte 2.
    {
        std::vector<uint32_t> pal(1024, 0);
        pal[7] = 0xFF0000FFu, pal[9] = 0xFF00FF00u;
        tex::Params p;
        p.tcw = tcw(tex::kPalette4, true) | 1u << 31;
        p.width = p.height = 4;
        p.palette = pal.data(), p.palette_format = 3;
        std::vector<uint8_t> d(tex::bytes(p), 0);
        expect(d.size() == 2 + 2 + 8, "4-bit mip-mapped 4x4: 1x1 and 2x2 in four bytes, then 4x4");
        d[1] = 0x70, d[2] = 0x09;
        std::vector<uint32_t> out(16);
        tex::decode_level(p, d.data(), 2, out.data());
        expect(out[0] == 0xFFFF0000u, "4-bit mip-mapped: the 1x1 texel in a high nibble");
        tex::decode_level(p, d.data(), 1, out.data());
        expect(out[0] == 0xFF00FF00u, "4-bit mip-mapped: the 2x2 level from the next byte");
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
        return std::getenv("QILIN_REQUIRE_GRAPHICS") ? 1 : 77;  // CI under Xvfb and Mesa: a skip is a failure
    }
    qilin::host::Display::Options o;
    o.headless = true, o.width = 64, o.height = 48;
    qilin::host::Display display(o);
    dreamcast::renderer::Renderer r(display.gpu(), 1);
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
    dreamcast::renderer::Source src;
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
    // Anisotropic filtering on: the samplers and the sets naming them are made
    // again, and the picture is the same (the quad faces the screen).
    r.set_anisotropy(16);
    r.draw(src);
    const std::vector<uint8_t> again = r.read_back();
    expect(again == px, "renderer: the same picture with anisotropic filtering on");
    r.set_anisotropy(1);

    // Cheap shadows: an opaque red quad over everything and a translucent
    // green one over the left quarter, both taking shadows (PCW bit 7); one
    // volume, a box from x 8 to 24 enclosing both surfaces, sent to the
    // opaque and the translucent modifier lists; SHADOW_SCALE 0x180 (a half).
    ta::Scene shadow;
    auto shadow_quad = [&](ta::List list, float x0, float x1, float z, float r_, float g_, float b_) {
        ta::Strip s{};
        s.list = list;
        s.pcw = gouraud | 0x80u;
        s.isp = list == ta::kOpaque ? isp_ge : 7u << 29;
        s.tsp[0] = 1u << 29 | 2u << 22;  // one, zero: the colour as it is; no fog
        s.first = uint32_t(shadow.vertices.size()), s.count = 4;
        shadow.vertices.push_back(vertex(x0, 0, z, r_, g_, b_));
        shadow.vertices.push_back(vertex(x1, 0, z, r_, g_, b_));
        shadow.vertices.push_back(vertex(x0, 48, z, r_, g_, b_));
        shadow.vertices.push_back(vertex(x1, 48, z, r_, g_, b_));
        shadow.strips.push_back(s);
    };
    shadow_quad(ta::kOpaque, 0, 64, 0.1f, 1, 0, 0);
    shadow_quad(ta::kTranslucent, 0, 16, 0.5f, 0, 1, 0);
    // The box's faces seen from the front: one nearer than both surfaces (z
    // 0.9), one farther (z 0.05); its sides are edge-on.
    const float faces[4][9] = {{8, 8, 0.9f, 24, 8, 0.9f, 8, 40, 0.9f},   {24, 8, 0.9f, 24, 40, 0.9f, 8, 40, 0.9f},
                               {8, 8, 0.05f, 24, 8, 0.05f, 8, 40, 0.05f}, {24, 8, 0.05f, 24, 40, 0.05f, 8, 40, 0.05f}};
    for (const ta::List list : {ta::kOpaqueModifier, ta::kTranslucentModifier}) {
        const uint32_t first = uint32_t(shadow.modifier_triangles.size() / 9);
        for (const auto& f : faces) shadow.modifier_triangles.insert(shadow.modifier_triangles.end(), std::begin(f), std::end(f));
        shadow.modifiers.push_back({list, 0x80000000u | uint32_t(list) << 24, 0, first, 3});
        shadow.modifiers.push_back({list, 0x80000000u | uint32_t(list) << 24, 1u << 29, first + 3, 1});  // the last: "inside"
    }
    dreamcast::renderer::Source ssrc;
    ssrc.scene = &shadow, ssrc.width = 64, ssrc.height = 48, ssrc.frame_buffer = 0x200000;
    ssrc.read = read_vram, ssrc.generation = generation;
    ssrc.shadow_scale = 0x180;
    r.draw(ssrc);
    const std::vector<uint8_t> sp = r.read_back();
    auto near = [&](int x, int y, int red, int green, int blue) {
        const uint8_t* p = &sp[size_t(y * 64 + x) * 4];
        return std::abs(p[0] - red) <= 2 && std::abs(p[1] - green) <= 2 && std::abs(p[2] - blue) <= 2;
    };
    expect(near(40, 20, 255, 0, 0), "shadows: the opaque surface outside the volume");
    expect(near(20, 20, 128, 0, 0), "shadows: the opaque surface inside the volume, halved");
    expect(near(4, 20, 0, 255, 0), "shadows: the translucent surface outside the volume");
    expect(near(12, 20, 0, 128, 0), "shadows: the translucent surface inside the volume, halved");
    expect(near(12, 44, 0, 255, 0), "shadows: the translucent surface below the volume");

    // Table fog: black opaque quads side by side, 8 pixels wide, each at its
    // own depth, with white fog from an irregular table; each pixel should be
    // the table's value at that depth (renderer::table_fog, the definition
    // the emulator's pictures gave, tools/dcref --fog), to within rounding.
    ta::Scene fog;
    const float depths[8] = {0.3f, 1.0f, 1.19f, 1.5f, 2.7f, 9.3f, 40.0f, 300.0f};
    for (int k = 0; k < 8; ++k) {
        ta::Strip s{};
        s.list = ta::kOpaque;
        s.isp = 7u << 29;              // always
        s.tsp[0] = 1u << 29 | 0u << 22;  // one, zero; table fog
        s.first = uint32_t(fog.vertices.size()), s.count = 4;
        const float x0 = float(8 * k), x1 = x0 + 8, z = depths[k];
        fog.vertices.push_back(vertex(x0, 0, z, 0, 0, 0)), fog.vertices.push_back(vertex(x1, 0, z, 0, 0, 0));
        fog.vertices.push_back(vertex(x0, 48, z, 0, 0, 0)), fog.vertices.push_back(vertex(x1, 48, z, 0, 0, 0));
        fog.strips.push_back(s);
    }
    dreamcast::renderer::Source fsrc;
    fsrc.scene = &fog, fsrc.width = 64, fsrc.height = 48, fsrc.frame_buffer = 0x300000;
    fsrc.read = read_vram, fsrc.generation = generation;
    fsrc.fog_colour = 0x00FFFFFFu, fsrc.fog_density = 0x9F00u;  // 1.24
    for (uint32_t i = 0; i < 128; ++i) fsrc.fog_table[i] = ((i * 37u + 11u) & 0xFFu) << 8 | ((i * 91u + 5u) & 0xFFu);
    r.draw(fsrc);
    const std::vector<uint8_t> fp = r.read_back();
    const float density = dreamcast::renderer::fog_density(fsrc.fog_density);
    expect(std::fabs(density - 1.2421875f) < 1e-6f, "fog: FOG_DENSITY 0x9F00 is 159/128");
    bool all = true;
    for (int k = 0; k < 8; ++k) {
        const float want = dreamcast::renderer::table_fog(density, fsrc.fog_table, depths[k]);
        const uint8_t* p = &fp[size_t(24 * 64 + 8 * k + 4) * 4];
        if (std::fabs(float(p[1]) - want) > 1.5f) all = false, std::printf("render_test: fog at z %g: %u, wanted %.1f\n", double(depths[k]), p[1], double(want));
    }
    expect(all, "fog: each depth's pixel is the table's value there");

    // Each scene below: a 64x48 picture, its pixels read back and compared.
    uint32_t next_buffer = 0x400000;
    auto draw = [&](const ta::Scene& sc, auto&& set) {
        dreamcast::renderer::Source s;
        s.scene = &sc, s.width = 64, s.height = 48, s.frame_buffer = next_buffer, next_buffer += 0x100000;
        s.read = read_vram, s.generation = generation;
        set(s);
        r.draw(s);
        return r.read_back();
    };
    auto pixel_is = [](const std::vector<uint8_t>& pic, int x, int y, int red, int green, int blue) {
        const uint8_t* q = &pic[size_t(y * 64 + x) * 4];
        const bool ok = std::abs(q[0] - red) <= 2 && std::abs(q[1] - green) <= 2 && std::abs(q[2] - blue) <= 2;
        if (!ok) std::printf("render_test: (%d, %d) is %u %u %u, wanted %d %d %d\n", x, y, q[0], q[1], q[2], red, green, blue);
        return ok;
    };
    // A quad as a strip of four, one colour, in a list, with its words.
    auto add_quad = [](ta::Scene& sc, ta::List list, uint32_t pcw, uint32_t isp, uint32_t tsp, float x0, float y0, float x1, float y1,
                       float z, const float (&rgba)[4]) {
        ta::Strip s{};
        s.list = list, s.pcw = pcw, s.isp = isp, s.tsp[0] = tsp, s.textured = pcw & 0x08u;
        s.tcw[0] = 1u << 27 | 1u << 26;  // RGB565, raster, at 0 (the blue texture)
        s.first = uint32_t(sc.vertices.size()), s.count = 4;
        const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
        for (int k = 0; k < 4; ++k) {
            ta::Vertex v = vertex(xs[k], ys[k], z, rgba[0], rgba[1], rgba[2], k & 1 ? 1.0f : 0.0f, k & 2 ? 1.0f : 0.0f);
            v.base[0][3] = rgba[3];
            sc.vertices.push_back(v);
        }
        sc.strips.push_back(s);
        return sc.strips.size() - 1;
    };
    const uint32_t always = 7u << 29, one_zero = 1u << 29, no_fog = 2u << 22;

    // Vertex fog (fog mode 1): a black textured quad (blue texels modulated by
    // black) whose offset colour's alpha is a half; FOG_COL_VERT white.
    {
        ta::Scene sc;
        const size_t q = add_quad(sc, ta::kOpaque, 0x08u | 0x04u, always, one_zero | 1u << 22 | 1u << 6, 0, 0, 64, 48, 0.5f, {0, 0, 0, 1});
        for (uint32_t k = 0; k < 4; ++k) sc.vertices[sc.strips[q].first + k].offset[0][3] = 0.5f;
        const auto pic = draw(sc, [](dreamcast::renderer::Source& s) { s.fog_vertex_colour = 0x00FFFFFFu, s.fog_colour = 0x00FF0000u; });
        expect(pixel_is(pic, 20, 20, 128, 128, 128), "vertex fog: half way to FOG_COL_VERT by the offset colour's alpha");
    }
    // The fog table's second mode (fog mode 3): a red translucent quad,
    // blended by its source alpha over black, becomes FOG_COL_RAM (white)
    // with the table's value (entry 0's high byte, 0x80, at a coefficient
    // below 1) as its alpha.
    {
        ta::Scene sc;
        add_quad(sc, ta::kTranslucent, 0, always, 4u << 29 | 5u << 26 | 3u << 22 | 1u << 20, 0, 0, 64, 48, 0.5f, {1, 0, 0, 1});
        const auto pic = draw(sc, [](dreamcast::renderer::Source& s) {
            s.fog_colour = 0x00FFFFFFu, s.fog_density = 0x8000u;  // 1
            s.fog_table[0] = 0x8040u;
        });
        expect(pixel_is(pic, 20, 20, 128, 128, 128), "fog table's second mode: the fog colour, the table's value as alpha");
    }
    // The colour clamp (TSP bit 21): (1, 0.2, 0.9) between FOG_CLAMP_MIN
    // (green at least 0x40) and FOG_CLAMP_MAX (red and blue at most 0x80).
    {
        ta::Scene sc;
        add_quad(sc, ta::kOpaque, 0, always, one_zero | no_fog | 1u << 21, 0, 0, 64, 48, 0.5f, {1, 0.2f, 0.9f, 1});
        const auto pic = draw(sc, [](dreamcast::renderer::Source& s) { s.clamp_max = 0xFF80FF80u, s.clamp_min = 0x00004000u; });
        expect(pixel_is(pic, 20, 20, 128, 64, 128), "colour clamp: between FOG_CLAMP_MIN and FOG_CLAMP_MAX");
    }
    // The user tile clip: a far red quad over everything; a near green one
    // drawn only inside tile (1, 0); a blue one between them drawn only
    // outside tile (0, 1).
    {
        ta::Scene sc;
        const uint32_t ge = 6u << 29;
        add_quad(sc, ta::kOpaque, 0, ge, one_zero | no_fog, 0, 0, 64, 48, 0.1f, {1, 0, 0, 1});
        const size_t g_ = add_quad(sc, ta::kOpaque, 2u << 16, ge, one_zero | no_fog, 0, 0, 64, 48, 0.5f, {0, 1, 0, 1});
        const size_t b_ = add_quad(sc, ta::kOpaque, 3u << 16, ge, one_zero | no_fog, 0, 0, 64, 48, 0.3f, {0, 0, 1, 1});
        const uint8_t inside[4] = {1, 0, 1, 0}, outside[4] = {0, 1, 0, 1};
        std::memcpy(sc.strips[g_].clip, inside, 4), std::memcpy(sc.strips[b_].clip, outside, 4);
        const auto pic = draw(sc, [](dreamcast::renderer::Source&) {});
        expect(pixel_is(pic, 40, 10, 0, 255, 0), "user tile clip: drawn inside its tile");
        expect(pixel_is(pic, 10, 10, 0, 0, 255), "user tile clip: not drawn outside its tile; the other drawn outside its own");
        expect(pixel_is(pic, 10, 40, 255, 0, 0), "user tile clip: not drawn inside the tile it excludes");
        expect(pixel_is(pic, 40, 40, 0, 0, 255), "user tile clip: drawn outside the tile it excludes");
    }
    // The two-volume mode (SHADOW_SCALE's bit 8 clear): an opaque quad over
    // everything, red in its first volume and blue in its second, and a
    // translucent one over the left quarter, green and white; the volume is
    // the box of the shadows' test (x 8 to 24, y 8 to 40), in both modifier
    // lists. Inside it each polygon shows its second volume.
    {
        ta::Scene sc;
        const uint32_t two = 0x80u | 0x40u | gouraud;
        const size_t oq = add_quad(sc, ta::kOpaque, two, 6u << 29, one_zero | no_fog, 0, 0, 64, 48, 0.1f, {1, 0, 0, 1});
        const size_t t = add_quad(sc, ta::kTranslucent, two, always, one_zero | no_fog, 0, 0, 16, 48, 0.5f, {0, 1, 0, 1});
        const float second[2][4] = {{0, 0, 1, 1}, {1, 1, 1, 1}};
        for (int k = 0; k < 2; ++k) {
            ta::Strip& s = sc.strips[k == 0 ? oq : t];
            s.two_volumes = true, s.tsp[1] = s.tsp[0];
            for (uint32_t i = 0; i < 4; ++i) std::memcpy(sc.vertices[s.first + i].base[1], second[k], sizeof second[k]);
        }
        sc.modifier_triangles = shadow.modifier_triangles;
        sc.modifiers = shadow.modifiers;
        const auto pic = draw(sc, [](dreamcast::renderer::Source&) {});
        expect(pixel_is(pic, 40, 20, 255, 0, 0), "two volumes: the opaque polygon's first volume outside");
        expect(pixel_is(pic, 20, 20, 0, 0, 255), "two volumes: the opaque polygon's second volume inside");
        expect(pixel_is(pic, 4, 20, 0, 255, 0), "two volumes: the translucent polygon's first volume outside");
        expect(pixel_is(pic, 12, 20, 255, 255, 255), "two volumes: the translucent polygon's second volume inside");
        expect(pixel_is(pic, 12, 44, 0, 255, 0), "two volumes: the translucent polygon below the volume");
    }
    // The translucent list in auto-sort mode (ISP_FEED_CFG's bit 0 clear),
    // sorted for each pixel: a red quad, half transparent, at z 0.5 over the
    // whole picture, then a green one through it, its z from 0.25 at the left
    // to 0.75 at the right (behind the red on the left, in front on the
    // right), over black; an opaque blue strip on the right, nearer than both,
    // keeps them out. Blended from the farthest at each pixel: (127, 64, 0) on
    // the left, (64, 127, 0) on the right. A sort by triangle gets one side
    // wrong.
    {
        ta::Scene sc;
        const uint32_t ge = 6u << 29, no_write = 1u << 26, alpha_blend = 4u << 29 | 5u << 26 | 1u << 20 | no_fog;
        add_quad(sc, ta::kOpaque, 0, ge, one_zero | no_fog, 0, 0, 64, 48, 0.01f, {0, 0, 0, 1});
        add_quad(sc, ta::kOpaque, 0, ge, one_zero | no_fog, 56, 0, 64, 48, 0.9f, {0, 0, 1, 1});
        add_quad(sc, ta::kTranslucent, 0, ge | no_write, alpha_blend, 0, 0, 64, 48, 0.5f, {1, 0, 0, 0.5f});
        const size_t g_ = add_quad(sc, ta::kTranslucent, 0, ge | no_write, alpha_blend, 0, 0, 64, 48, 0.5f, {0, 1, 0, 0.5f});
        for (uint32_t k = 0; k < 4; ++k) {  // the strip's corners: left, right, left, right
            ta::Vertex& v = sc.vertices[sc.strips[g_].first + k];
            v.z = k % 2 == 0 ? 0.25f : 0.75f;
        }
        // Drawn twice: the first frame sizes the lists (one node a pixel at first, two wanted here).
        const uint32_t same = next_buffer;
        draw(sc, [](dreamcast::renderer::Source& s) { s.presort = false; });
        const auto pic = draw(sc, [same](dreamcast::renderer::Source& s) { s.presort = false, s.frame_buffer = same; });
        // Each point across the diagonal that splits the green quad into two triangles from its own
        // triangle's mean depth, which says the other way.
        expect(pixel_is(pic, 6, 44, 127, 64, 0), "auto-sort: on the left the green, farther, blended first");
        expect(pixel_is(pic, 44, 4, 64, 127, 0), "auto-sort: on the right the red, farther, blended first");
        expect(pixel_is(pic, 60, 24, 0, 0, 255), "auto-sort: nothing behind an opaque surface");
        const auto sent = draw(sc, [](dreamcast::renderer::Source& s) { s.presort = true; });
        expect(pixel_is(sent, 6, 44, 64, 127, 0) && pixel_is(sent, 44, 4, 64, 127, 0), "presort: in the order sent everywhere");
    }
    // More fragments a pixel than the lists hold (one a pixel at first):
    // five additive layers of a tenth of red. The renderer reads the count
    // back and makes the lists longer, so the same scene drawn again is
    // whole: half red.
    {
        ta::Scene sc;
        const uint32_t additive = 1u << 29 | 1u << 26 | 1u << 20 | no_fog;
        add_quad(sc, ta::kOpaque, 0, 6u << 29, one_zero | no_fog, 0, 0, 64, 48, 0.01f, {0, 0, 0, 1});
        for (int k = 0; k < 5; ++k) add_quad(sc, ta::kTranslucent, 0, 6u << 29 | 1u << 26, additive, 0, 0, 64, 48, 0.2f + 0.1f * float(k), {0.1f, 0, 0, 1});
        const uint32_t again_at = next_buffer;
        draw(sc, [](dreamcast::renderer::Source& s) { s.presort = false; });
        const auto regrown = draw(sc, [again_at](dreamcast::renderer::Source& s) { s.presort = false, s.frame_buffer = again_at; });
        expect(pixel_is(regrown, 30, 24, 128, 0, 0), "auto-sort: lists made longer when a frame wanted more");
    }
    // A modifier volume's user tile clip: the shadows' scene again, its
    // volumes drawn only inside tile (0, 0) (x and y below 32): the box's
    // part below y 32 darkens nothing, the opaque surface's nor the
    // translucent one's.
    {
        ta::Scene sc = shadow;
        for (ta::Modifier& m : sc.modifiers) {
            m.pcw |= 2u << 16;
            const uint8_t tile[4] = {0, 0, 0, 0};
            std::memcpy(m.clip, tile, 4);
        }
        const auto pic = draw(sc, [](dreamcast::renderer::Source& s) { s.shadow_scale = 0x180; });
        expect(pixel_is(pic, 20, 20, 128, 0, 0), "volume clip: the opaque surface inside the volume and the clip, halved");
        expect(pixel_is(pic, 20, 36, 255, 0, 0), "volume clip: the opaque surface inside the volume, outside the clip");
        expect(pixel_is(pic, 12, 20, 0, 128, 0), "volume clip: the translucent surface inside the volume and the clip, halved");
        expect(pixel_is(pic, 12, 36, 0, 255, 0), "volume clip: the translucent surface inside the volume, outside the clip");
    }
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
