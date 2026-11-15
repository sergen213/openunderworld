#version 460
// Enhanced under-water bloom (docs/RENDERING.md "Under water"): abyssal-garden W_BLIT (Noniv, MIT) literally, the chain
// main.js builds with NB = 6 levels of W >> (i + 1): mode 0 fsDown0 (the linear composite -> level 0: four bilinear taps
// one input texel off the 2x position, each clamped to 24 and Karis-weighted 1 / (1 + max channel)), mode 1 fsDown (the
// same four taps averaged, level i - 1 -> i), mode 2 fsUp (level i -> i - 1 at 0.75 texel offsets, added by the blend).
// Fullscreen triangle (VK post.vert / GL kPostVs); inputs sampled linear, clamped to the edge (the demo's sClamp).
#ifdef VULKAN
layout(set = 0, binding = 0) uniform sampler2D uIn;
layout(push_constant) uniform Push { vec4 v; } pc; // x mode (uw_grade.frag reads yzw)
#define OU_MODE int(pc.v.x)
#else
layout(binding = 0) uniform sampler2D uIn;
layout(location = 0) uniform vec4 uV;
#define OU_MODE int(uV.x)
#endif
layout(location = 0) out vec4 oColor;
vec4 tap(vec2 uv) { return textureLod(uIn, uv, 0.0); }
vec4 kar(vec3 c) { float w = 1.0 / (1.0 + max(c.r, max(c.g, c.b))); return vec4(c * w, w); }
void main() {
    vec2 dim = vec2(textureSize(uIn, 0));
    if (OU_MODE == 2) {
        vec2 uv = gl_FragCoord.xy * 0.5 / dim, e = 0.75 / dim;
        vec4 s = tap(uv + e * vec2(-1.0, -1.0)) + tap(uv + e * vec2(1.0, -1.0)) + tap(uv + e * vec2(-1.0, 1.0)) + tap(uv + e * vec2(1.0, 1.0));
        oColor = vec4(s.rgb * 0.25, 1.0); // alpha 1: the additive blend scales by it
        return;
    }
    vec2 uv = gl_FragCoord.xy * 2.0 / dim, e = 1.0 / dim;
    if (OU_MODE == 0) {
        vec4 m = kar(min(tap(uv + e * vec2(-1.0, -1.0)).rgb, vec3(24.0))) + kar(min(tap(uv + e * vec2(1.0, -1.0)).rgb, vec3(24.0))) +
                 kar(min(tap(uv + e * vec2(-1.0, 1.0)).rgb, vec3(24.0))) + kar(min(tap(uv + e * vec2(1.0, 1.0)).rgb, vec3(24.0)));
        oColor = vec4(m.rgb / m.a, 1.0);
    } else {
        vec4 s = tap(uv + e * vec2(-1.0, -1.0)) + tap(uv + e * vec2(1.0, -1.0)) + tap(uv + e * vec2(-1.0, 1.0)) + tap(uv + e * vec2(1.0, 1.0));
        oColor = vec4(s.rgb * 0.25, 1.0);
    }
}
