#version 460
// Marine snow (abyssal-garden fsSnow, MIT): a soft additive dot lit by the sun's light left at its depth x the sun
// shadow plus the water's ambient in-scatter, hue-varied per speck, then fogged by its own distance (uw_light.glsl).
// Only inside a water volume, fading out near its surface (abyssal-garden's 0.3..1.5 m x 290). Premultiplied output
// with alpha 0: adds to the particle target (blend ONE, ONE_MINUS_SRC_ALPHA), which the post pass composites.
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vP;
layout(location = 2) flat in float vTint;
layout(location = 0) out vec4 oColor;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
#include "shaders/uw_particles.glsl"
#include "shaders/uw_light.glsl"
void main() {
    float m = uwSs(1.0, 0.2, length(vUv));
    vec3 p = vP.xyz;
    float surf = uwSurfaceAt(p);
    if (m <= 0.0 || surf < -1e29 || uwOccluded(p)) discard;
    float under = uwSs(90.0, 435.0, surf - p.z);
    // ambient: the water's own glow looking up (the brighter end of ouWaterPass's gradient), half desaturated toward
    // its grey (detritus scatters white); sun x 1.6, abyssal-garden's sun 1.7 x 0.8 against its ambient x 0.3 [I weights]
    vec3 glow = uwFogColour(vec3(0.0, 0.0, 1.0));
    glow = mix(glow, vec3(dot(glow, vec3(0.2126, 0.7152, 0.0722))), 0.5);
    vec3 c = (uwSun(p, surf) * 1.6 + glow * 1.1) * mix(vec3(1.0), vec3(0.7, 1.0, 0.9), vTint);
    c *= m * m * vP.w * under;
    oColor = vec4(c * (1.0 - uwFogAlpha(p)), 0.0);
}
