#version 450
// The PowerVR2's texture and shading processor for one polygon: the texture,
// combined with the base colour as the TSP word's shading instruction says,
// plus the offset (specular) colour.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_base;
layout(location = 2) in vec4 v_offset;

layout(set = 0, binding = 0) uniform sampler2D tex;

// The frame's buffer (renderer.cpp's frame_data): the translucent modifier
// volumes' count, the shadow's factor, the depth scale and the picture's
// pixels per scene pixel; the table fog's colour (FOG_COL_RAM), density
// (FOG_DENSITY as a number) and table (FOG_TABLE's 128 entries), vertex
// fog's colour (FOG_COL_VERT) and the colour clamp's bounds (FOG_CLAMP_MAX,
// FOG_CLAMP_MIN); then each
// translucent modifier volume's bounds on screen (scene pixels) and its
// triangles; the triangles, three corners of x, y (scene pixels) and z (1/w),
// the first corner's fourth word the triangle's user tile clip.
struct Volume {
    vec4 box;     // x0, y0, x1, y1
    uvec4 range;  // first triangle, count
};
layout(std430, set = 1, binding = 0) readonly buffer Volumes {
    uint count;
    uint unused;
    float shade;
    float depth_scale;
    float pixel_scale;
    float unused2[3];
    vec4 fog_colour;
    vec4 fog_vertex_colour;
    vec4 clamp_max;
    vec4 clamp_min;
    float fog_density;
    float unused3[3];
    uint fog_table[128];
    Volume volume[64];
    vec4 corner[];
} volumes;

// Inside a volume: an odd number of its faces in front of the pixel at its
// own depth (z = 1/w, larger is nearer), as the hardware tests each pixel of
// a translucent polygon against the translucent modifier volumes.
bool in_volume(vec2 p, float depth) {
    for (uint v = 0u; v < volumes.count; ++v) {
        const vec4 b = volumes.volume[v].box;
        if (p.x < b.x || p.y < b.y || p.x > b.z || p.y > b.w) continue;
        bool inside = false;
        const uint first = volumes.volume[v].range.x, n = volumes.volume[v].range.y;
        for (uint t = first; t < first + n; ++t) {
            // The triangle's user tile clip (renderer.cpp): it counts only inside (2) or outside (3) it.
            const uint clip = floatBitsToUint(volumes.corner[3u * t].w);
            if ((clip >> 20) >= 2u) {
                const vec2 lo = vec2(float(clip >> 14 & 63u), float(clip >> 10 & 15u)) * 32.0;
                const vec2 hi = vec2(float((clip >> 4 & 63u) + 1u), float((clip & 15u) + 1u)) * 32.0;
                const bool in_clip = p.x >= lo.x && p.y >= lo.y && p.x < hi.x && p.y < hi.y;
                if (in_clip != ((clip >> 20) == 2u)) continue;
            }
            const vec3 a = volumes.corner[3u * t].xyz, b1 = volumes.corner[3u * t + 1u].xyz, c = volumes.corner[3u * t + 2u].xyz;
            const float area = (b1.x - a.x) * (c.y - a.y) - (b1.y - a.y) * (c.x - a.x);
            if (area == 0.0) continue;
            const float w0 = ((b1.x - p.x) * (c.y - p.y) - (b1.y - p.y) * (c.x - p.x)) / area;
            const float w1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
            const float w2 = 1.0 - w0 - w1;
            if (w0 < 0.0 || w1 < 0.0 || w2 < 0.0) continue;
            const float z = (w0 * a.z + w1 * b1.z + w2 * c.z) * volumes.depth_scale;
            if (z > depth) inside = !inside;
        }
        if (inside) return true;
    }
    return false;
}

// Table fog's value for a pixel at depth z (1/w), 0 to 1: the coefficient,
// FOG_DENSITY times 1/w, picks an entry by its exponent and the top 4 bits of
// its mantissa (16 entries an octave from 1, clamped to the table), and the
// value goes from the entry's high byte to its low byte by the mantissa's
// next bits. Our own definition, from what the emulator's pictures show
// (tools/dcref, --fog; renderer.cpp's table_fog is the same in C++).
float table_fog(float z) {
    const float coef = volumes.fog_density * z;
    if (!(coef >= 1.0)) return float(volumes.fog_table[0] >> 8 & 255u) / 255.0;
    int index = 127;
    float frac = 1.0;
    if (coef < 256.0) {
        int e;
        const float steps = (frexp(coef, e) * 2.0 - 1.0) * 16.0;
        index = 16 * (e - 1) + int(steps), frac = fract(steps);
    }
    const uint t = volumes.fog_table[index];
    return mix(float(t >> 8 & 255u), float(t & 255u), frac) / 255.0;
}

#ifdef SORTED
// The translucent list in auto-sort mode: each fragment, its colour finished
// but not blended, goes onto its pixel's list (head: the pixel's newest
// node; a node: the colour, the depth, the blend factors and the draw's
// place in the order sent, the next node), and sort.comp blends each
// pixel's list from the farthest. The opaque scene's depth is tested first,
// so a fragment behind it is never added.
layout(early_fragment_tests) in;
layout(std430, set = 1, binding = 1) coherent buffer Heads { uint head[]; } heads;
layout(std430, set = 1, binding = 2) coherent buffer Nodes {
    uint count;
    uint capacity;
    uint width;
    uint unused;
    uvec4 node[];
} nodes;
#endif

layout(push_constant) uniform Push {
    vec4 screen;
    uint flags;       // bit 0 textured, 1 offset colour, 2 use alpha, 3 ignore the texture's alpha, 4-5 the shading instruction,
                      // 6 darken inside a translucent modifier volume, 7 table fog, 8 and 9 drawn only outside or inside
                      // the translucent modifier volumes, 10-11 the user tile clip (2 inside, 3 outside), 12 vertex fog,
                      // 13 the fog table's second mode, 14 the colour clamp
    float alpha_ref;  // punch-through: texels below it are dropped
    uint sort_word;   // SORTED: the source and destination blend factors (bits 31-29, 28-26), the draw's place (25-0)
    float unused;
    vec4 clip;        // the user tile clip: x0, y0, x1, y1 in scene pixels (x1, y1 excluded)
} push;

layout(location = 0) out vec4 colour;

void main() {
    const vec2 p = gl_FragCoord.xy / volumes.pixel_scale;  // in scene pixels
    const uint clip = (push.flags >> 10) & 3u;
    if (clip >= 2u) {
        const bool inside = p.x >= push.clip.x && p.y >= push.clip.y && p.x < push.clip.z && p.y < push.clip.w;
        if (inside != (clip == 2u)) discard;
    }
    // A two-volume polygon's volumes, each on its own side of the translucent modifier volumes.
    if ((push.flags & 0x300u) != 0u && in_volume(p, gl_FragCoord.z) != ((push.flags & 0x200u) != 0u)) discard;
    vec4 base = v_base;
    if ((push.flags & 4u) == 0u) base.a = 1.0;
    vec4 c = base;
    if ((push.flags & 1u) != 0u) {
        vec4 t = texture(tex, v_uv);
        if ((push.flags & 8u) != 0u) t.a = 1.0;
        switch ((push.flags >> 4) & 3u) {
        case 0u: c = t; break;                                              // decal
        case 1u: c = vec4(base.rgb * t.rgb, t.a); break;                    // modulate
        case 2u: c = vec4(mix(base.rgb, t.rgb, t.a), base.a); break;        // decal alpha
        default: c = base * t; break;                                       // modulate alpha
        }
        if ((push.flags & 2u) != 0u) c.rgb += v_offset.rgb;
    }
    c = clamp(c, 0.0, 1.0);
    if ((push.flags & 64u) != 0u && in_volume(p, gl_FragCoord.z)) c.rgb *= volumes.shade;
    // The colour clamp (the TSP word's bit 21), before the fog.
    if ((push.flags & 0x4000u) != 0u) c = clamp(c, volumes.clamp_min, volumes.clamp_max);
    // Fog. gl_FragCoord.w is 1 / the clip w, which pvr.vert makes the
    // vertex's z: the pixel's 1/w. Vertex fog takes its value from the offset
    // colour's alpha; the table's second mode gives the fog colour, with the
    // table's value as its alpha.
    if ((push.flags & 128u) != 0u) c.rgb = mix(c.rgb, volumes.fog_colour.rgb, table_fog(gl_FragCoord.w));
    else if ((push.flags & 0x1000u) != 0u) c.rgb = mix(c.rgb, volumes.fog_vertex_colour.rgb, clamp(v_offset.a, 0.0, 1.0));
    else if ((push.flags & 0x2000u) != 0u) c = vec4(volumes.fog_colour.rgb, table_fog(gl_FragCoord.w));
#ifdef PUNCH_THROUGH
    if (c.a < push.alpha_ref) discard;
    c.a = 1.0;
#endif
#ifdef SORTED
    const uint n = atomicAdd(nodes.count, 1u);
    if (n < nodes.capacity) {  // over: dropped, and the renderer makes the list longer for the next frame
        const uint pixel = uint(gl_FragCoord.y) * nodes.width + uint(gl_FragCoord.x);
        const uint next = atomicExchange(heads.head[pixel], n);
        nodes.node[n] = uvec4(packUnorm4x8(c), floatBitsToUint(gl_FragCoord.z), push.sort_word, next);
    }
    colour = vec4(0.0);  // not written (the pipeline's mask)
#else
    colour = c;
#endif
}
