// What the window's picture shaders share: the frame, and the look's numbers.
layout(set = 0, binding = 0) uniform sampler2D frame;  // filtered or not, as the settings say

layout(push_constant) uniform Push {
    vec2 lines;       // the console's picture in its own pixels (640 x 480 at 60 Hz): the CRT's raster
    float scanlines;  // CRT: how dark the gaps between raster lines are, 0..1
    float mask;       // CRT: the phosphor stripes' strength, 0..1
} push;
