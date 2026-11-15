#version 460
// Bubbles, abyssal-garden fsBub (MIT) literally: a disc whose outline wobbles on big bubbles (ss(0.04, 0.22, size)),
// mostly transparent with a rim and two highlights. Body: the water's in-scatter looking down (lo) to looking up (hi,
// x 1.5) over 80 m from the bubble (agFogIn), x (0.5 + 1.2 rim); highlights: agSunE x (0.25 + 0.75 shadow) x 1.6. Then
// its own fog: T = e^(-SIG.g d) ss(1.2, 3, d), c T + fogIn(eye -> bubble) (1 - T), alpha x mix(0.35, 1, T).
// Premultiplied into the particle target (x the bubble's own alpha, render::Bubble::alpha).
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
    float t = agTime();
    float an = atan(vUv.y, vUv.x + 1e-4);
    float big = agSs(0.04, 0.22, vP.w);
    float r = length(vUv) / (0.9 + big * 0.05 * sin(an * 3.0 + t * 5.0 + vA.y * 40.0) + big * 0.02 * sin(an * 5.0 - t * 7.0));
    float aa = fwidth(r) * 1.5;
    float disc = 1.0 - agSs(1.0 - aa, 1.0, r);
    if (disc <= 0.0 || uwSurfaceAt(vP.xyz) < -1e29 || uwOccluded(vP.xyz)) discard;
    float rim = agSs(0.55, 0.98, r);
    float hl = agSs(0.34, 0.0, length(vUv - vec2(-0.36, 0.42))) + 0.5 * agSs(0.2, 0.0, length(vUv - vec2(0.4, -0.42)));
    vec3 wp = agPos(vP.xyz), sun = agSun();
    vec3 E = agSunE(wp.y, kAgZone) * (0.25 + 0.75 * ouSunShadowVisibility(vP.xyz));
    vec3 hi = agFogIn(wp, vec3(0.0, 1.0, 0.0), 80.0, kAgZone, sun) * 1.5;
    vec3 lo = agFogIn(wp, vec3(0.0, -1.0, 0.0), 80.0, kAgZone, sun);
    vec3 c = mix(lo, hi, agSs(-0.5, 0.8, vUv.y + rim * 0.4)) * (0.5 + 1.2 * rim) + E * hl * 1.6;
    float al = disc * agSat(0.03 + rim * rim * rim * 0.9 + hl);
    vec3 cam = agEye(), v = wp - cam;
    float d = length(v);
    float T = exp(-kAgSig.g * d) * agSs(1.2, 3.0, d);
    c = c * T + agFogIn(cam, v / max(d, 1e-6), d, kAgZone, sun) * (1.0 - T);
    al *= mix(0.35, 1.0, T) * vA.x;
    oColor = vec4(min(max(c * al, vec3(0.0)), vec3(20.0)), al);
}
