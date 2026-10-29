#version 460
// Same as GL kPostFs: optional ACES tonemap (HDR) and classic FXAA, then the atmosphere / weather and the underwater effect.
layout(location = 0) in vec2 vUv;
layout(push_constant) uniform Push { int fxaa; int hdr; float brightness; float fade; } pc; // fade: FrameOptions::screenFade
layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(location = 0) out vec4 oColor;
vec3 fetch(vec2 uv) {
    vec3 c = textureLod(uScene, uv, 0.0).rgb;
    if (pc.hdr != 0) c = clamp(c * (2.51 * c + 0.03) / (c * (2.43 * c + 0.59) + 0.14), 0.0, 1.0);
    return c;
}
float luma(vec3 c) { return dot(c, vec3(0.299, 0.587, 0.114)); }
vec3 fxaa(vec2 uv) {
    vec2 rcp = 1.0 / vec2(textureSize(uScene, 0));
    vec3 m = fetch(uv);
    float nw = luma(fetch(uv + vec2(-1.0, -1.0) * rcp)), ne = luma(fetch(uv + vec2(1.0, -1.0) * rcp));
    float sw = luma(fetch(uv + vec2(-1.0, 1.0) * rcp)), se = luma(fetch(uv + vec2(1.0, 1.0) * rcp)), lm = luma(m);
    float lo = min(lm, min(min(nw, ne), min(sw, se))), hi = max(lm, max(max(nw, ne), max(sw, se)));
    vec2 dir = vec2(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    float reduce = max((nw + ne + sw + se) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * rcp;
    vec3 a = 0.5 * (fetch(uv + dir * (1.0 / 3.0 - 0.5)) + fetch(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (fetch(uv - dir * 0.5) + fetch(uv + dir * 0.5));
    float lb = luma(b);
    return (lb < lo || lb > hi) ? a : b;
}
#include "shaders/underwater.glsl"
#define OU_SHADOW_SET 2
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
// Brightness: display gamma after tonemap / FXAA (1 = neutral), as the original game's brightness option.
void main() {
    vec3 c = pow(pc.fxaa != 0 ? fxaa(vUv) : fetch(vUv), vec3(1.0 / pc.brightness));
    oColor = vec4(ouUnderwater(ouAtmosphere(c, vUv), vUv) * (1.0 - pc.fade), 1.0);
}
