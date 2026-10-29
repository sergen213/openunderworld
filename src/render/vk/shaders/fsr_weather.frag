#version 460
// FSR reactivity of Enhanced rain / snow (drawn in the post pass without motion): their coverage (atmosphere.glsl
// ouPrecipitation), 90 %. Drawn with post.vert over the mask pass; post's sets 1 (WaterBlock + opaque depth) and 2.
layout(location = 0) in vec2 vUv;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
#include "shaders/underwater.glsl"
#define OU_SHADOW_SET 2
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
void main() {
    oMotion = vec2(0.0);
    oReactive = 0.0;
    if (w.weather.x < 0.5 || w.fogColor.w > 0.5) return; // no weather, or underwater (no precipitation drawn)
    const float d = texture(uDepth, vUv).r;
    const bool sky = d >= 0.999999;
    const vec3 p = ouWorld(vUv, d, w.invViewProj);
    const vec3 dir = normalize(sky ? ouWorld(vUv, 1.0, w.invViewProj) - w.eye.xyz : p - w.eye.xyz);
    oReactive = min(ouPrecipitation(dir, sky ? 1e30 : length(p - w.eye.xyz), w.weather.x > 1.5, w.weather.y).a, 1.0) * 0.9;
}
