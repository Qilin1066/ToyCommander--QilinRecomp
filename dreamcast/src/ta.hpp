// The PowerVR2's tile accelerator, as far as the port needs one: it takes
// the parameter stream Kamui 2 writes (by store queue or DMA, at 0x10000000)
// and turns it into a scene of polygons for the Vulkan renderer, instead of
// the console's object lists in video memory. The stream's format is the
// hardware's: 32-byte parameters (64 for some), each starting with a
// parameter control word (PCW): end of list, user tile clip, object list
// set, a polygon's or sprite's or modifier volume's global parameters, or a
// vertex, whose layout the last global parameters decide. Anything the
// decoder doesn't know stops.
#pragma once

#include <cstdint>
#include <vector>

namespace dreamcast::ta {

enum List : uint8_t { kOpaque = 0, kOpaqueModifier = 1, kTranslucent = 2, kTranslucentModifier = 3, kPunchThrough = 4 };

struct Vertex {
    float x, y, z;
    float u[2], v[2];             // per volume
    float base[2][4], offset[2][4];  // per volume, RGBA, 0-1 (floats as given)
};

// A strip of vertices (a sprite is a strip of four) with the state the
// global parameters set: the ISP/TSP instruction word, and per volume the
// TSP instruction word and the texture control word. `clip` is the user tile
// clip in force when the strip began (the last user tile clip parameter's
// rectangle, in tiles of 32 pixels: x min, y min, x max, y max, inclusive);
// the PCW's bits 17-16 say whether the strip is drawn only inside it (2) or
// only outside it (3).
struct Strip {
    List list;
    bool sprite, two_volumes, textured;
    uint32_t pcw, isp, tsp[2], tcw[2];
    uint32_t first, count;  // in Scene::vertices
    uint8_t clip[4];
};

// A modifier volume: triangles, with the ISP word that says how they modify,
// and the user tile clip in force (as a strip's).
struct Modifier {
    List list;
    uint32_t pcw, isp;
    uint32_t first, count;  // triangles, 9 floats each, in Scene::modifier_triangles
    uint8_t clip[4];
};

struct Scene {
    std::vector<Vertex> vertices;
    std::vector<Strip> strips;
    std::vector<float> modifier_triangles;
    std::vector<Modifier> modifiers;
    uint32_t lists_ended = 0;  // a bit per List
    void clear();
};

// Maps the parameter input on the bus.
void map();
// Parameters by DMA (channel 2 to 0x10000000-0x107FFFFF): n bytes, a
// multiple of 32.
void feed(const uint8_t* data, unsigned n);
// TA_LIST_INIT: a new scene begins, its lists built in the parameter buffer at `base`
// (TA_ISP_BASE). The scene built before it is kept for its own buffer.
inline constexpr uint32_t kNoBase = 0xFFFFFFFFu;
void list_init(uint32_t base);
// The scene built in the parameter buffer at `base` (PARAM_BASE, for a render): the one being built
// if that's its buffer, else the last finished there; the one being built if none was.
const Scene& scene(uint32_t base);

}  // namespace dreamcast::ta
