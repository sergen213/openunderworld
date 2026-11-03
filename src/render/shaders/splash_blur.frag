#version 460
// Splash droplets, pass 2 of 3, run twice (horizontal, then vertical): depth-aware (bilateral) blur of the splat target,
// which turns the overlapping spheres into one smooth liquid surface without bleeding across depth gaps. Thickness
// averages empty pixels in as 0, so edges taper; empty pixels near droplets take their depth and that tapered thickness,
// which grows the silhouette and bridges droplets a few pixels apart (the composite fades thin parts out). Adapted from
// talvinckb/OpenGL-Water-Simulation blur.frag (MIT, Copyright (c) 2026 Talvin Ackbaraly).
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
layout(location = 0) out vec2 oSplat;
const float kEmpty = -1e19;
const float kRange = 6.0; // depth scale (units) of the range weight exp(-(dd / kRange)^2)
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy), size = textureSize(uSplash, 0);
    vec2 c = texelFetch(uSplash, p, 0).rg;
    bool filled = c.r > kEmpty;
    // ponytail: fixed screen radius (8 px at 1080p), not scaled by distance; a far splash blurs over more world units.
    int radius = clamp(int(8.0 * w.screen.y / 1080.0 + 0.5), 2, 24);
    float inv2s2 = 2.0 / float(radius * radius); // sigma = radius / 2
    float depthSum = 0.0, depthW = 0.0, thickSum = 0.0, allW = 0.0;
    for (int i = -radius; i <= radius; ++i) {
        ivec2 q = clamp(p + ivec2(DIR * float(i)), ivec2(0), size - 1);
        vec2 s = texelFetch(uSplash, q, 0).rg;
        float wt = exp(-float(i * i) * inv2s2);
        if (s.r <= kEmpty) { allW += wt; continue; }
        if (filled) {
            float dd = (s.r - c.r) / kRange;
            wt *= exp(-dd * dd);
        }
        depthSum += s.r * wt;
        depthW += wt;
        thickSum += s.g * wt;
        allW += wt;
    }
    oSplat = depthW > 0.0 ? vec2(depthSum / depthW, thickSum / allW) : c;
}
