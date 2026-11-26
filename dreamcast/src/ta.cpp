#include "ta.hpp"

#include <array>
#include <cstring>
#include <utility>

#include "bus.hpp"
#include "guest.hpp"
#include "pvr.hpp"

namespace dreamcast::ta {

namespace rt = qilin::rt;

void Scene::clear() {
    vertices.clear(), strips.clear(), modifier_triangles.clear(), modifiers.clear();
    lists_ended = 0;
}

namespace {

// The PCW's fields.
constexpr uint32_t para_type(uint32_t pcw) { return pcw >> 29; }
constexpr bool end_of_strip(uint32_t pcw) { return pcw & (1u << 28); }
constexpr uint32_t list_type(uint32_t pcw) { return (pcw >> 24) & 7u; }
enum Para : uint32_t { kEndOfList = 0, kUserTileClip = 1, kObjectListSet = 2, kPolygon = 4, kSprite = 5, kVertex = 7 };

// The event each list's end raises (the system bus's normal status).
constexpr unsigned kListEndEvent[5] = {7, 8, 9, 10, 21};

[[noreturn]] void unknown(uint32_t pcw) { throw rt::Stop{rt::Stop::Unsupported, 0, pcw}; }

struct Decoder {
    Scene scene;
    bool in_list = false;
    List list = kOpaque;
    // The last global parameters.
    bool have_global = false;
    bool modifier = false, sprite = false, two_volumes = false, textured = false, uv16 = false;
    uint32_t col_type = 0, pcw = 0, isp = 0, tsp[2] = {}, tcw[2] = {};
    float face_base[2][4] = {}, face_offset[2][4] = {};
    float sprite_base[4] = {}, sprite_offset[4] = {};
    unsigned vertex_size = 32;  // bytes of the current vertex type
    bool open_strip = false;
    // A 64-byte parameter's first half.
    uint8_t pending[64];
    unsigned pending_bytes = 0, want = 0;
    // The modifier volume being gathered.
    bool open_modifier = false;
    // The last user tile clip (tiles: x min, y min, x max, y max).
    uint8_t clip[4] = {0, 0, 0x3F, 0x0F};

    void take(const uint8_t* block);  // 32 bytes
    void parameter(const uint32_t* w);
    void global(const uint32_t* w);
    void vertex(const uint32_t* w);
    void end_list();
};
Decoder g;

float f(uint32_t w) {
    float x;
    std::memcpy(&x, &w, 4);
    return x;
}
void packed(uint32_t argb, float out[4]) {
    out[0] = float((argb >> 16) & 0xFFu) / 255.0f;
    out[1] = float((argb >> 8) & 0xFFu) / 255.0f;
    out[2] = float(argb & 0xFFu) / 255.0f;
    out[3] = float(argb >> 24) / 255.0f;
}
void floats(const uint32_t* argb, float out[4]) { out[0] = f(argb[1]), out[1] = f(argb[2]), out[2] = f(argb[3]), out[3] = f(argb[0]); }
void scaled(const float face[4], float i, float out[4]) { out[0] = face[0] * i, out[1] = face[1] * i, out[2] = face[2] * i, out[3] = face[3]; }
// A 16-bit texture coordinate is a float's upper half.
float half(uint32_t bits16) { return f(bits16 << 16); }

// The size of the parameter a PCW starts, given the current global state.
unsigned size_of(const Decoder& d, uint32_t pcw) {
    switch (para_type(pcw)) {
    case kEndOfList:
    case kUserTileClip:
    case kObjectListSet: return 32;
    case kPolygon: {
        const uint32_t lt = list_type(pcw);
        if (lt == kOpaqueModifier || lt == kTranslucentModifier) return 32;
        const bool vol = pcw & 0x40u, tex = pcw & 0x08u, off = pcw & 0x04u;
        const uint32_t ct = (pcw >> 4) & 3u;
        if (vol) return ct == 2 ? 64 : 32;
        return ct == 2 && tex && off ? 64 : 32;
    }
    case kSprite: return 32;
    case kVertex: return d.vertex_size;
    default: unknown(pcw);
    }
}

void Decoder::take(const uint8_t* block) {
    if (want) {
        std::memcpy(pending + 32, block, 32);
        want = 0;
        uint32_t w[16];
        std::memcpy(w, pending, 64);
        parameter(w);
        return;
    }
    uint32_t first;
    std::memcpy(&first, block, 4);
    if (size_of(*this, first) == 64) {
        std::memcpy(pending, block, 32);
        want = 32;
        return;
    }
    // A 32-byte parameter, in a 64-byte buffer as the long ones are: the
    // decoder reads a parameter's second half only when size_of said it has
    // one, but the buffer never lets a disagreement read past it (GCC 14 sees
    // the reads and can't see that).
    uint32_t w[16] = {};
    std::memcpy(w, block, 32);
    parameter(w);
}

void Decoder::parameter(const uint32_t* w) {
    const uint32_t word = w[0];
    switch (para_type(word)) {
    case kEndOfList: end_list(); return;
    case kUserTileClip:  // words 4-7: x min, y min, x max, y max (6 bits for x, 4 for y)
        clip[0] = uint8_t(w[4] & 0x3Fu), clip[1] = uint8_t(w[5] & 0x0Fu), clip[2] = uint8_t(w[6] & 0x3Fu), clip[3] = uint8_t(w[7] & 0x0Fu);
        return;
    case kPolygon:
    case kSprite: global(w); return;
    case kVertex: vertex(w); return;
    default: unknown(word);
    }
}

void Decoder::global(const uint32_t* w) {
    const uint32_t p = w[0];
    if (!in_list) {
        list = static_cast<List>(list_type(p));
        if (list > kPunchThrough) unknown(p);
        in_list = true;
    }
    open_strip = false;
    have_global = true;
    pcw = p, isp = w[1];
    modifier = list == kOpaqueModifier || list == kTranslucentModifier;
    if (modifier) {
        if (para_type(p) != kPolygon) unknown(p);
        vertex_size = 64;
        open_modifier = false;
        return;
    }
    sprite = para_type(p) == kSprite;
    two_volumes = (p & 0x40u) && !sprite;
    textured = p & 0x08u;
    uv16 = p & 0x01u;
    col_type = (p >> 4) & 3u;
    tsp[0] = w[2], tcw[0] = w[3];
    if (sprite) {
        packed(w[4], sprite_base), packed(w[5], sprite_offset);
        vertex_size = 64;
        return;
    }
    if (two_volumes) {
        tsp[1] = w[4], tcw[1] = w[5];
        if (col_type == 2) floats(w + 8, face_base[0]), floats(w + 12, face_base[1]);
        if (col_type == 1) unknown(p);  // floating colour with two volumes: not a valid combination
        vertex_size = textured ? 64 : 32;
        return;
    }
    if (col_type == 2) {
        if (textured && (p & 0x04u)) floats(w + 8, face_base[0]), floats(w + 12, face_offset[0]);
        else floats(w + 4, face_base[0]);
    }
    vertex_size = textured && col_type == 1 ? 64 : 32;
}

void Decoder::vertex(const uint32_t* w) {
    if (!have_global || !in_list) unknown(w[0]);
    if (modifier) {
        // Type 17: a triangle A, B, C.
        if (!open_modifier) {
            scene.modifiers.push_back({list, pcw, isp, static_cast<uint32_t>(scene.modifier_triangles.size() / 9), 0, {clip[0], clip[1], clip[2], clip[3]}});
            open_modifier = true;
        }
        for (int k = 1; k <= 9; ++k) scene.modifier_triangles.push_back(f(w[k]));
        ++scene.modifiers.back().count;
        if (end_of_strip(w[0])) open_modifier = false;
        return;
    }
    if (!open_strip) {
        scene.strips.push_back({list, sprite, two_volumes, textured, pcw, isp, {tsp[0], tsp[1]}, {tcw[0], tcw[1]},
                                static_cast<uint32_t>(scene.vertices.size()), 0, {clip[0], clip[1], clip[2], clip[3]}});
        open_strip = true;
    }
    Strip& s = scene.strips.back();
    if (sprite) {
        // Types 15 and 16: corners A, B, C, D (D without z: on ABC's plane),
        // as a strip A, B, D, C.
        Vertex v[4]{};
        const float ax = f(w[1]), ay = f(w[2]), az = f(w[3]), bx = f(w[4]), by = f(w[5]), bz = f(w[6]);
        const float cx = f(w[7]), cy = f(w[8]), cz = f(w[9]), dx = f(w[10]), dy = f(w[11]);
        const float e1x = bx - ax, e1y = by - ay, e1z = bz - az, e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
        const float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
        const float dz = nz != 0.0f ? az - (nx * (dx - ax) + ny * (dy - ay)) / nz : az;
        const float xs[4] = {ax, bx, cx, dx}, ys[4] = {ay, by, cy, dy}, zs[4] = {az, bz, cz, dz};
        float us[4] = {}, vs[4] = {};
        if (textured) {
            us[0] = half(w[13] >> 16), vs[0] = half(w[13] & 0xFFFFu);
            us[1] = half(w[14] >> 16), vs[1] = half(w[14] & 0xFFFFu);
            us[2] = half(w[15] >> 16), vs[2] = half(w[15] & 0xFFFFu);
            us[3] = us[0] + us[2] - us[1], vs[3] = vs[0] + vs[2] - vs[1];
        }
        static constexpr int kOrder[4] = {0, 1, 3, 2};
        for (int k = 0; k < 4; ++k) {
            const int c = kOrder[k];
            Vertex& o = v[k];
            o.x = xs[c], o.y = ys[c], o.z = zs[c], o.u[0] = us[c], o.v[0] = vs[c];
            std::memcpy(o.base[0], sprite_base, sizeof sprite_base), std::memcpy(o.offset[0], sprite_offset, sizeof sprite_offset);
            scene.vertices.push_back(o);
        }
        s.count += 4;
        open_strip = false;  // every sprite is a strip of its own
        return;
    }
    Vertex o{};
    o.x = f(w[1]), o.y = f(w[2]), o.z = f(w[3]);
    const auto colour = [&](int vol, uint32_t word_base, uint32_t word_offset, bool with_offset) {
        if (col_type == 0) {
            packed(word_base, o.base[vol]);
            if (with_offset) packed(word_offset, o.offset[vol]);
        } else {  // intensity (mode 1 sets the face colour, mode 2 keeps the last)
            scaled(face_base[vol], f(word_base), o.base[vol]);
            if (with_offset) scaled(face_offset[vol], f(word_offset), o.offset[vol]);
        }
    };
    if (!two_volumes) {
        if (!textured) {
            if (col_type == 1) floats(w + 4, o.base[0]);  // type 1
            else if (col_type == 0) packed(w[6], o.base[0]);  // type 0
            else scaled(face_base[0], f(w[6]), o.base[0]);  // type 2
        } else if (col_type == 1) {  // types 5 and 6
            if (uv16) o.u[0] = half(w[4] >> 16), o.v[0] = half(w[4] & 0xFFFFu);
            else o.u[0] = f(w[4]), o.v[0] = f(w[5]);
            floats(w + 8, o.base[0]), floats(w + 12, o.offset[0]);
        } else {  // types 3, 4, 7, 8
            if (uv16) o.u[0] = half(w[4] >> 16), o.v[0] = half(w[4] & 0xFFFFu);
            else o.u[0] = f(w[4]), o.v[0] = f(w[5]);
            colour(0, w[6], w[7], pcw & 0x04u);
        }
    } else if (!textured) {  // types 9 and 10
        colour(0, w[4], 0, false), colour(1, w[5], 0, false);
    } else {  // types 11-14
        for (int vol = 0; vol < 2; ++vol) {
            const uint32_t* q = w + 4 + vol * 4;
            if (uv16) o.u[vol] = half(q[0] >> 16), o.v[vol] = half(q[0] & 0xFFFFu);
            else o.u[vol] = f(q[0]), o.v[vol] = f(q[1]);
            colour(vol, q[2], q[3], pcw & 0x04u);
        }
    }
    scene.vertices.push_back(o);
    ++s.count;
    if (end_of_strip(w[0])) open_strip = false;
}

void Decoder::end_list() {
    if (!in_list) return;  // an empty list's end: nothing was opened
    if (want) unknown(0);
    scene.lists_ended |= 1u << list;
    raise_event(kListEndEvent[list]);
    in_list = false, have_global = false, open_strip = false, open_modifier = false;
}

// The input: 32-byte bursts (store queues, DMA); single words gather into one.
uint8_t g_word_block[32];
unsigned g_word_bytes = 0;

void block_write(uint32_t a, const uint8_t* data, unsigned n) {
    if (a >= 0x10800000u || n % 32 || g_word_bytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    for (unsigned k = 0; k < n; k += 32) g.take(data + k);
}
void word_write(uint32_t a, uint32_t v, unsigned n) {
    if (a >= 0x10800000u || n != 4 || (a & 31u) != g_word_bytes) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    std::memcpy(g_word_block + g_word_bytes, &v, 4);
    if ((g_word_bytes += 4) == 32) g_word_bytes = 0, g.take(g_word_block);
}

}  // namespace

void map() { rt::map({0x10000000u, 0x11000000u, "tile accelerator input", nullptr, word_write, block_write}); }

// Scenes finished in other parameter buffers, by the buffer's address (TA_ISP_BASE when their lists
// began). The game builds the next scene's lists in a second buffer while the last renders, and
// starts that render (STARTRENDER, PARAM_BASE naming the buffer) when the hardware's done with the
// one before: by then the next scene may be begun. g.scene is the one being built, in g_building.
struct Built {
    uint32_t base = kNoBase;
    Scene scene;
};
std::array<Built, 2> g_built;
uint32_t g_building = kNoBase;
size_t g_oldest = 0;  // the slot to reuse when neither holds the base

void list_init(uint32_t base) {
    if (g_building != kNoBase) {  // the scene built so far is kept for its buffer
        // Its own buffer's slot, else a free one, else the one kept longest.
        size_t slot = g_built.size();
        for (size_t i = 0; i < g_built.size(); ++i)
            if (g_built[i].base == g_building) slot = i;
        for (size_t i = 0; i < g_built.size() && slot == g_built.size(); ++i)
            if (g_built[i].base == kNoBase) slot = i;
        if (slot == g_built.size()) slot = g_oldest;
        std::swap(g_built[slot].scene, g.scene);  // the slot's old scene's storage is reused
        g_built[slot].base = g_building;
        if (slot == g_oldest) g_oldest = (g_oldest + 1) % g_built.size();
    }
    for (Built& b : g_built)
        if (b.base == base) b.base = kNoBase;  // overwritten by the scene beginning
    g_building = base;
    g.scene.clear();
    g.in_list = false, g.have_global = false, g.open_strip = false, g.want = 0, g.open_modifier = false;
}

void feed(const uint8_t* data, unsigned n) { block_write(0x10000000u, data, n); }

const Scene& scene(uint32_t base) {
    if (base != g_building)
        for (const Built& b : g_built)
            if (b.base == base) return b.scene;
    return g.scene;
}

}  // namespace dreamcast::ta
