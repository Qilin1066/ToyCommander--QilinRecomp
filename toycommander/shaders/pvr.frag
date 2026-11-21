#version 450
// The PowerVR2's texture and shading processor for one polygon: the texture,
// combined with the base colour as the TSP word's shading instruction says,
// plus the offset (specular) colour.
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_base;
layout(location = 2) in vec4 v_offset;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform Push {
    vec4 screen;
    uint flags;       // bit 0 textured, 1 offset colour, 2 use alpha, 3 ignore the texture's alpha, 4-5 the shading instruction
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
#ifdef PUNCH_THROUGH
    if (c.a < push.alpha_ref) discard;
    c.a = 1.0;
#endif
    colour = c;
}
