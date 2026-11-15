#version 460
// Bubbles (abyssal-garden fsBub, MIT): a thin-walled sphere seen as a disc whose outline wobbles on big bubbles, mostly
// transparent with an opaque rim and two highlights (the sun's reflection up-left, a weaker one down-right). The body
// mirrors the water around it: the colour looking down at its foot to the colour looking up at its crown (the fog's
// in-scatter those ways from its depth, uw_light.glsl uwWaterToward), brighter at the rim; the highlights take the sunlight left at its depth x the sun
// shadow. Fogged by its own distance like the scene behind it; premultiplied into the particle target.
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vP;
layout(location = 2) flat in vec2 vA;
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
    float t = uwp.eye.w, seed = vA.y;
    float an = atan(vUv.y, vUv.x + 1e-4);
    float big = uwSs(8.0, 40.0, vP.w); // radius in units: wobbling outline on big ones (abyssal-garden 0.04..0.22 m [I])
    float r = length(vUv) / (0.9 + big * 0.05 * sin(an * 3.0 + t * 5.0 + seed * 40.0) + big * 0.02 * sin(an * 5.0 - t * 7.0));
    float aa = fwidth(r) * 1.5;
    float disc = 1.0 - uwSs(1.0 - aa, 1.0, r);
    vec3 p = vP.xyz;
    float surf = uwSurfaceAt(p);
    if (disc <= 0.0 || surf < -1e29 || uwOccluded(p)) discard;
    float rim = uwSs(0.55, 0.98, r);
    float hl = uwSs(0.34, 0.0, length(vUv - vec2(-0.36, 0.42))) + 0.5 * uwSs(0.2, 0.0, length(vUv - vec2(0.4, -0.42)));
    OuUwMedium md = ouUwEyeMedium();
    float h = max(surf - p.z, 0.0) / kM;
    vec3 hi = uwWaterToward(md, vec3(0.0, 0.0, 1.0), h) * 1.5, lo = uwWaterToward(md, vec3(0.0, 0.0, -1.0), h);
    vec3 c = mix(lo, hi, uwSs(-0.5, 0.8, vUv.y + rim * 0.4)) * (0.5 + 1.2 * rim) + uwSun(md, p, surf) * hl * 1.6;
    float al = disc * clamp(0.03 + rim * rim * rim * 0.9 + hl, 0.0, 1.0) * vA.x;
    // own fog (the medium between it and the eye), and gone right at the lens (abyssal-garden 1.2..3 m; 30..90 units [I])
    vec3 T, L;
    ouUwPointFog(md, p, T, L);
    float d = length(p - w.eye.xyz), ts = dot(T, vec3(1.0 / 3.0)) * uwSs(30.0, 90.0, d);
    c = c * T + L;
    al *= mix(0.35, 1.0, ts) * uwSs(30.0, 90.0, d);
    oColor = vec4(max(c, vec3(0.0)) * al, al);
}
