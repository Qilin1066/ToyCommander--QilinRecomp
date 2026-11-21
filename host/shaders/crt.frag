#version 450
// A CRT television, written for this project after the APB recomp's: each of
// the console's raster lines is a beam whose height grows with its
// brightness (a Gaussian, so the average brightness is kept), with darker
// gaps between lines, and an aperture grille's red, green and blue stripes
// down the screen in window pixels. Light adds up in linear terms (a tube's
// 2.4 gamma).
//
// The frame may be drawn at several times the console's resolution: along a
// line it's sampled as it is (the detail is kept); across lines, each beam
// takes the frame's colour at its line's centre. The gaps fade out where a
// raster line gets fewer than about two window pixels, where they'd only
// make moire.
#extension GL_GOOGLE_include_directive : require
#include "present.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

// Raster line k's light at uv.x, linear.
vec3 line_light(float k) {
    const float y = (clamp(k, 0.0, push.lines.y - 1.0) + 0.5) / push.lines.y;
    return pow(textureLod(frame, vec2(uv.x, y), 0.0).rgb, vec3(2.4));
}

// The share of a line's light at distance d (in lines) from its centre:
// brighter lines spread wider, and every line gives the same total.
vec3 beam(vec3 light, float d) {
    const vec3 sigma = mix(vec3(0.19), vec3(0.40), sqrt(light));
    return light * exp(-0.5 * (d / sigma) * (d / sigma)) / (sigma * 2.5066283);
}

void main() {
    const float across = uv.y * push.lines.y;  // in raster lines
    const float lines_per_pixel = fwidth(across);
    const float v = across - 0.5;
    const float k = floor(v);
    const float t = v - k;  // 0: on line k's centre; 1: on line k+1's
    const vec3 a = line_light(k), b = line_light(k + 1.0);
    const vec3 beams = beam(a, t) + beam(b, 1.0 - t);
    const vec3 flat_ = t < 0.5 ? a : b;  // the same lines with no gaps
    const float fade = clamp(1.0 / max(lines_per_pixel, 1e-5) - 1.25, 0.0, 1.0);
    vec3 light = mix(flat_, beams, push.scanlines * fade);

    // The aperture grille: stripes down the screen, one window pixel each.
    const int stripe = int(mod(gl_FragCoord.x, 3.0));
    const float low = 1.0 - push.mask;
    vec3 grille = vec3(low);
    grille[stripe] = 1.0;
    light *= grille * (3.0 / (1.0 + 2.0 * low));

    colour = vec4(pow(clamp(light, 0.0, 1.0), vec3(1.0 / 2.4)), 1.0);
}
