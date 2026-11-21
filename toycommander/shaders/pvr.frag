#version 450
// The PowerVR2's texture and shading processor for one polygon: the texture,
// combined with the base colour as the TSP word's shading instruction says,
// plus the offset (specular) colour.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_base;
layout(location = 2) in vec4 v_offset;

layout(set = 0, binding = 0) uniform sampler2D tex;

// The translucent modifier volumes (renderer.cpp's collect_translucent_volumes):
// the count, the shadow's factor, the depth scale and the picture's pixels per
// scene pixel; each volume's bounds on screen (scene pixels) and its
// triangles; the triangles, three corners of x, y (scene pixels) and z (1/w).
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

layout(push_constant) uniform Push {
    vec4 screen;
    uint flags;       // bit 0 textured, 1 offset colour, 2 use alpha, 3 ignore the texture's alpha, 4-5 the shading instruction,
                      // 6 darken inside a translucent modifier volume
    float alpha_ref;  // punch-through: texels below it are dropped
} push;

layout(location = 0) out vec4 colour;

void main() {
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
    if ((push.flags & 64u) != 0u && in_volume(gl_FragCoord.xy / volumes.pixel_scale, gl_FragCoord.z)) c.rgb *= volumes.shade;
#ifdef PUNCH_THROUGH
    if (c.a < push.alpha_ref) discard;
    c.a = 1.0;
#endif
    colour = c;
}
