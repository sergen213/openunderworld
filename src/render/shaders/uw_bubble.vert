#version 460
// Bubbles, abyssal-garden vsBub (MIT): camera-facing quads for render::Bubble instances (the effects' under-water-only
// bubbles and Lara's breath, FrameOptions::bubbles). The CPU drives each bubble's path and size (the demo's rise
// a.xyz + h and a.w (0.55 + 0.7 life) ss(0, 0.03, life), fx/Breath.cpp); here the demo's sway in ag space: wob =
// b.w (0.3 + 0.035 h) on two sines per horizontal axis of the time, seed and height risen h, plus the slow 2 b.w drift,
// and the shrink over the last 1.2 m under the surface ss(0, -1.2, y).
#include "shaders/uw_particles.glsl"
layout(location = 0) in vec4 aBubble; // xyz centre (world), w radius (world units)
layout(location = 1) in vec4 aInfo;   // x alpha, y seed 0..1 (b.x), z height risen (m, h), w wobble (m, b.w)
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vP; // xyz centre (world), w size (m)
layout(location = 2) flat out vec2 vA; // alpha, seed
void main() {
    float t = uwp.eye.w, s = aInfo.y, h = aInfo.z, bw = aInfo.w;
    vec3 p = uwAg(aBubble.xyz);
    float wob = bw * (0.3 + h * 0.035);
    p.x += sin(t * 1.7 + s * 50.0 + h * 0.6) * wob + sin(h * 0.13 + s * 9.0) * bw * 2.0;
    p.z += cos(t * 1.3 + s * 33.0 + h * 0.5) * wob + cos(h * 0.11 + s * 7.0) * bw * 2.0;
    float sz = aBubble.w / uwp.params.z * uwSs(0.0, -1.2, p.y);
    vec2 q = uwCorner(OU_VERTEX_ID);
    vec3 c = uwWorld(p);
    vUv = q, vP = vec4(c, sz), vA = aInfo.xy;
    gl_Position = uwp.viewProj * vec4(c + (uwp.right.xyz * q.x + uwp.up.xyz * q.y) * (sz * uwp.params.z), 1.0);
}
