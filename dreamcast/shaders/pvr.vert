#version 450
// A PowerVR2 vertex: x and y in screen pixels, z = 1/w, as the game's library
// hands them to the tile accelerator. The clip-space w is 1/z, so textures
// and colours are perspective-correct as the hardware's are; depth is z
// itself, scaled into 0..1 (larger = nearer).
layout(location = 0) in vec3 position;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec4 base;
layout(location = 3) in vec4 offset;

layout(push_constant) uniform Push {
    vec4 screen;      // 2 / width, 2 / height, the depth scale (1 / the scene's largest z), the draw's widening
    uint flags;
    float alpha_ref;
    uint sort_word;
    float shift;      // the draw's x on screen: x times screen.w plus this
} push;

layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_base;
layout(location = 2) out vec4 v_offset;

void main() {
    float w = 1.0 / position.z;
    const vec2 at = vec2(position.x * push.screen.w + push.shift, position.y);
    gl_Position = vec4((at * push.screen.xy - 1.0) * w, position.z * push.screen.z * w, w);
    v_uv = uv;
    v_base = base;
    v_offset = offset;
}
