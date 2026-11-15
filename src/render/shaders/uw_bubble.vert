#version 460
// Bubbles (abyssal-garden vsBub, MIT): camera-facing quads for render::Bubble instances (the effects' under-water-only
// particles and Lara's breath, FrameOptions::bubbles), swaying side to side as they rise: two sines per axis of the
// frame time and the bubble's seed, scaled by its radius [I amplitudes after abyssal-garden's wobble]. The wobble is
// visual only; the bubble's path itself comes from the CPU.
#include "shaders/uw_particles.glsl"
layout(location = 0) in vec4 aBubble; // xyz centre, w radius
layout(location = 1) in vec2 aInfo;   // x alpha, y seed 0..1
layout(location = 0) out vec2 vUv;
layout(location = 1) flat out vec4 vP; // xyz centre, w radius
layout(location = 2) flat out vec2 vA; // alpha, seed
void main() {
    float t = uwp.eye.w, s = aInfo.y, r = aBubble.w;
    vec3 p = aBubble.xyz;
    p.x += sin(t * 1.7 + s * 50.0) * r * 0.8 + sin(t * 0.43 + s * 9.0) * r * 1.6;
    p.y += cos(t * 1.3 + s * 33.0) * r * 0.8 + cos(t * 0.37 + s * 7.0) * r * 1.6;
    // never under ~2 pixels across (a ring needs them): smaller ones draw at that size, thinned by their area
    float minR = 1.5 * length(p - uwp.eye.xyz) * uwp.right.w, size = max(r, minR);
    vec2 q = uwCorner(OU_VERTEX_ID);
    vUv = q, vP = vec4(p, r), vA = vec2(aInfo.x * min(1.0, (r * r) / (size * size)), aInfo.y);
    gl_Position = uwp.viewProj * vec4(p + (uwp.right.xyz * q.x + uwp.up.xyz * q.y) * size, 1.0);
}
