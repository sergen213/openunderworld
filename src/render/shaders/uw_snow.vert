#version 460
// Marine snow, abyssal-garden vsSnow (MIT) literally: uwp.params.x (9000) specks, each with a = rnd()^4 (here a hash of
// its instance index), in a 44 m box wrapped around the eye (fract) in ag space, drifting with the current
// (0.22, 0.05, 0.13) m/s plus a 0.5 m wobble; size 0.012 + 0.045 a.w^3 m, never under d * 0.0011 (the demo's pixel at
// 1 m); faded at the box edge, at the lens (0.6..2.5 m), by sub-pixel size and toward the surface (-0.3..-1.5 m),
// twinkling. Stateless: a function of the frame time only (no gameplay effect).
#include "shaders/uw_particles.glsl"
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vP; // xyz centre (world), w fade x twinkle
layout(location = 2) flat out vec2 vA; // x a.w (tint), y distance to the eye (m)
void main() {
    uint id = uint(OU_INSTANCE_ID) * 4u;
    vec4 a = vec4(uwHash(id), uwHash(id + 1u), uwHash(id + 2u), uwHash(id + 3u));
    vec2 q = uwCorner(OU_VERTEX_ID);
    float t = uwp.eye.w;
    const float B = 44.0;
    vec3 cam = uwAg(uwp.eye.xyz);
    vec3 p = a.xyz * B + vec3(t * 0.22, t * 0.05, t * 0.13) +
             0.5 * vec3(sin(t * 0.3 + a.x * 60.0), sin(t * 0.23 + a.y * 50.0), cos(t * 0.27 + a.z * 70.0));
    p = cam + (fract((p - cam) / B + 0.5) - 0.5) * B;
    float d = length(p - cam);
    float sz = 0.012 + 0.045 * a.w * a.w * a.w;
    float fade = uwSs(B * 0.5, B * 0.32, d) * uwSs(0.6, 2.5, d) * min(1.0, sz / (d * 0.0011)) * uwSs(-0.3, -1.5, p.y);
    float tw = 0.6 + 0.4 * sin(t * (1.0 + a.w * 2.0) + a.x * 100.0);
    vec3 c = uwWorld(p);
    vUv = q, vP = vec4(c, fade * tw), vA = vec2(a.w, d);
    gl_Position = fade > 0.0 ? uwp.viewProj * vec4(c + (uwp.right.xyz * q.x + uwp.up.xyz * q.y) * (max(sz, d * 0.0011) * uwp.params.z), 1.0)
                             : vec4(2.0, 2.0, 2.0, 1.0); // culled: every corner outside the clip volume
}
