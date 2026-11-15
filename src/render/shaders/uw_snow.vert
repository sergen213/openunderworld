#version 460
// Marine snow (abyssal-garden vsSnow, MIT): uwp.params.x instanced specks in a box of edge uwp.up.w that wraps around
// the eye (fract), so the field is endless and costs nothing per frame on the CPU. Each speck's seed is a hash of its
// instance index; it drifts with a slow current plus a wobble, all a function of the frame time (no state, no gameplay
// effect). Sizes are 3.5..16.5 units (abyssal-garden's 1.2..5.7 cm x 290 units per metre), never under ~one pixel
// (the fade keeps the light of far specks constant instead of their size).
#include "shaders/uw_particles.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vP;    // xyz centre, w fade x twinkle
layout(location = 2) flat out float vTint; // 0..1 hue variation
void main() {
    uint id = uint(OU_INSTANCE_ID) * 4u;
    vec4 a = vec4(uwHash(id), uwHash(id + 1u), uwHash(id + 2u), uwHash(id + 3u));
    float t = uwp.eye.w, B = uwp.up.w;
    // current 64 / 38 / 14 units/s (abyssal-garden's 0.22 / 0.13 / 0.05 m/s, Y-up -> Z-up), 145-unit wobble [I scale]
    vec3 p = a.xyz * B + vec3(64.0, 38.0, 14.0) * t +
             145.0 * vec3(sin(t * 0.3 + a.x * 60.0), cos(t * 0.27 + a.z * 70.0), sin(t * 0.23 + a.y * 50.0));
    p = uwp.eye.xyz + (fract((p - uwp.eye.xyz) / B + 0.5) - 0.5) * B;
    float d = length(p - uwp.eye.xyz);
    float sz = 3.5 + 13.0 * a.w * a.w * a.w;
    float minSz = 0.9 * d * uwp.right.w; // half a quad of ~1.8 pixels
    // fades: the box edge (no popping where it wraps), the lens (60..250 units [I]), sub-pixel specks by their size
    float fade = uwSs(B * 0.5, B * 0.32, d) * uwSs(60.0, 250.0, d) * min(1.0, sz / max(minSz, 1e-3));
    float twinkle = 0.6 + 0.4 * sin(t * (1.0 + a.w * 2.0) + a.x * 100.0);
    vec2 q = uwCorner(OU_VERTEX_ID);
    vUv = q, vP = vec4(p, fade * twinkle), vTint = a.w;
    gl_Position = fade > 0.002 ? uwp.viewProj * vec4(p + (uwp.right.xyz * q.x + uwp.up.xyz * q.y) * max(sz, minSz), 1.0)
                               : vec4(2.0, 2.0, 2.0, 1.0); // culled: every corner outside the clip volume
}
