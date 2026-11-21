#version 450
// The frame as it is, scaled by the sampler (bilinear when smooth, else nearest).
#extension GL_GOOGLE_include_directive : require
#include "present.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

void main() { colour = vec4(texture(frame, uv).rgb, 1.0); }
