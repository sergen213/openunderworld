#version 460
// Splash fluid, pass 2 of 3, run twice (horizontal, then vertical): depth-aware (bilateral) blur of the splat target,
// which turns the overlapping spheres into one smooth liquid surface without bleeding across depth gaps. Thickness and
// foam average empty pixels in as 0, so edges taper; empty pixels near particles take their depth and that tapered
// thickness, which grows the silhouette and bridges particles a few pixels apart (the composite fades thin parts out).
// A pixel a particle covers (a = 1, splash_splat.frag; kept through both passes) averages thickness and foam over the
// covered taps only, so a lone drop keeps its own thickness. Adapted from talvinckb/OpenGL-Water-Simulation blur.frag
// (MIT, Copyright (c) 2026 Talvin Ackbaraly).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
} w;
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D uSplash;
layout(push_constant) uniform Blur { vec2 dir; } b; // (1, 0) or (0, 1)
#define DIR b.dir
#else
layout(binding = 10) uniform sampler2D uSplash;
layout(location = 0) uniform vec2 uDir;
#define DIR uDir
#endif
#include "shaders/fluid.glsl"
layout(location = 0) out vec4 oSplat;
const float kEmpty = -1e19;
const float kRange = 1.5 * kOuFluidRadius; // depth scale (units) of the range weight exp(-(dd / kRange)^2): one sheet
const float kWorldRadius = 2.0 * kOuFluidRadius; // the kernel's radius in the world: a few particles, at any distance
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(uSplash, 0);
    vec4 c = texelFetch(uSplash, p, 0);
    bool filled = c.r > kEmpty, covered = c.a > 0.5;
    // kWorldRadius on screen at this pixel's depth (empty pixels: 8 px at 1080p): the view-projection's second row is
    // the focal length f times the camera's up, so a unit at distance d spans f * height / 2 / d pixels.
    float f = length(vec3(w.viewProj[0][1], w.viewProj[1][1], w.viewProj[2][1]));
    float px = filled ? kWorldRadius * f * 0.5 * w.screen.y / max(-c.r, 1.0) : 8.0 * w.screen.y / 1080.0;
    int radius = clamp(int(px + 0.5), 2, 24);
    float inv2s2 = 2.0 / float(radius * radius); // sigma = radius / 2
    float depthSum = 0.0, depthW = 0.0, allW = 0.0;
    vec2 thickSum = vec2(0.0); // thickness, foam
    for (int i = -radius; i <= radius; ++i) {
        ivec2 q = clamp(p + ivec2(DIR * float(i)), ivec2(0), size - 1);
        vec4 s = texelFetch(uSplash, q, 0);
        float wt = exp(-float(i * i) * inv2s2);
        if (s.r <= kEmpty) { allW += wt; continue; }
        if (filled) {
            float dd = (s.r - c.r) / kRange;
            wt *= exp(-dd * dd);
        }
        depthSum += s.r * wt;
        depthW += wt;
        thickSum += s.gb * wt;
        allW += wt;
    }
    // A covered pixel keeps the average over the filled taps; any other the average with the empty taps as 0, so the
    // silhouette grows by a tapering (round, after both passes) fringe.
    oSplat = depthW > 0.0 ? vec4(depthSum / depthW, thickSum / (covered ? depthW : allW), c.a) : c;
}
