#version 460
// Enhanced water surface vertex shader (GL 4.5 / Vulkan 1.3, same mesh layout as the scene pass). water.frag has the
// technique; the height layers here must match its constants.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; vec4 waterPass[3]; // as water.frag (GL links blocks across stages)
} w;
#ifdef VULKAN
layout(set = 0, binding = 3) uniform sampler2D uRipples;
#else
layout(binding = 7) uniform sampler2D uRipples;
#endif
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
#ifdef VULKAN
layout(push_constant) uniform Push { mat4 model; mat4 waves; } pc;
#define uWaves pc.waves
#else
layout(location = 0) uniform mat4 uModel;
layout(location = 1) uniform mat4 uWaves; // render::waterWaves: columns c60-c63 (zero = flat)
#endif
layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) out vec4 vColor;
layout(location = 4) flat out float vSea;

// ---- copies of water.frag's height layers ----
const float kM = 290.0;
float ouLattice(vec2 c) {
    uvec2 q = uvec2(ivec2(c));
    uint n = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    return float(n >> 8) * (1.0 / 16777216.0);
}
float ouNoise(vec2 p) {
    vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
    float a = ouLattice(i), b = ouLattice(i + vec2(1, 0)), c = ouLattice(i + vec2(0, 1)), d = ouLattice(i + vec2(1, 1));
    return a + (b - a) * u.x + (c - a) * u.y + (a - b - c + d) * u.x * u.y;
}
float ouFbm(vec2 x) {
    float h = 0.0, a = 0.5, f = 1.0, norm = 0.0;
    for (int i = 0; i < 4; ++i) {
        h += a * ouNoise(x * f + vec2(17.3, 5.1) * float(i));
        norm += a;
        a *= 0.5, f *= 2.03;
    }
    return h / norm;
}
const mat2 kWaterRot = mat2(0.8, 0.6, -0.6, 0.8);
const float kLambdaA = 2.0 * kM, kLambdaB = 1.4 * kM;
// afl_ext's "Very fast procedural ocean" (https://www.shadertoy.com/view/MdXyzX, 2017; "afl_ext 2017-2024, MIT
// License"; xscreensaver prococean: Copyright (C) 2017 by afl_ext. Permission to use, copy, modify, distribute, and
// sell this software and its documentation for any purpose is hereby granted without fee, provided that the above
// copyright notice appear in all copies and that both that copyright notice and this permission notice appear in
// supporting documentation. No representations are made about the suitability of this software for any purpose.
// It is provided "as is" without express or implied warranty.) wavedx / getwaves, ported: position in afl units
// (kSeaUnit TRU units), time as an argument. Returns the wave height in about [0, 1].
const float kSeaUnit = 1.0 * kM;  // TRU units per afl unit: the longest waves ~6 m
const float kSeaAmp = 0.45 * kM;  // TRU height of afl's 0..1 (its WATER_DEPTH)
vec2 aflWavedx(vec2 position, vec2 direction, float frequency, float timeshift) {
    float x = dot(direction, position) * frequency + timeshift;
    float wave = exp(sin(x) - 1.0);
    float dx = wave * cos(x);
    return vec2(wave, -dx);
}
float ouSea(vec2 p, float t, int iterations) {
    vec2 position = p / kSeaUnit;
    float wavePhaseShift = length(position) * 0.1;
    float iter = 0.0, frequency = 1.0, timeMultiplier = 2.0, weight = 1.0, sumOfValues = 0.0, sumOfWeights = 0.0;
    for (int i = 0; i < iterations; i++) {
        vec2 d = vec2(sin(iter), cos(iter));
        vec2 res = aflWavedx(position, d, frequency, t * timeMultiplier + wavePhaseShift);
        position += d * res.y * weight * 0.38; // DRAG_MULT
        sumOfValues += res.x * weight;
        sumOfWeights += weight;
        weight = mix(weight, 0.0, 0.2);
        frequency *= 1.18;
        timeMultiplier *= 1.07;
        iter += 1232.399963;
    }
    return sumOfValues / sumOfWeights;
}

void main() {
#ifdef VULKAN
    vec4 world = pc.model * vec4(aPos, 1.0);
    vNormal = mat3(pc.model) * aNormal;
#else
    vec4 world = uModel * vec4(aPos, 1.0);
    vNormal = mat3(uModel) * aNormal;
#endif
    // A sea's waves, as its original vertex shader (tru.exe 0x69DFB0): lowered by sum c63 (1 - cos(x c60 + y c61 + c62)).
    // The gameplay surface follows the same waves (game::waveRegisters).
    float rest = world.z;
    world.z -= dot(uWaves[3], 1.0 - cos(world.x * uWaves[0] + world.y * uWaves[1] + uWaves[2]));
    float t = w.eye.w;
    vSea = any(notEqual(uWaves[3], vec4(0.0))) ? 1.0 : 0.0;
    // Godot's height layers (amplitude detail.z) and on open seas afl_ext's waves (12 iterations, its geometry count),
    // centred on that surface (afl's height averages ~0.47) but never above the rest plane, the original's own crest
    // bound, so the yacht deck and every shore stay above the drawn sea. Visible where the surface mesh has interior
    // vertices (TRU's are mostly coarse).
    float h = 0.5 * (ouFbm(world.xy / kLambdaA + t * vec2(0.10, 0.02)) + ouFbm(kWaterRot * world.xy / kLambdaB + t * vec2(-0.04, 0.12)));
    world.z += w.detail.z * (h - 0.5);
    if (vSea > 0.5) world.z += kSeaAmp * (ouSea(world.xy, t, 12) - 0.47);
    world.z = min(world.z, rest);
    // The interactive wave field's height (game::WaveField), faded over its outer 4 % as in water.frag.
    if (w.rippleInfo.y > 0.5) {
        vec2 uv = (world.xy - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
        vec2 in2 = min(uv, 1.0 - uv);
        world.z += smoothstep(0.0, 0.04, min(in2.x, in2.y)) * textureLod(uRipples, uv, 0.0).r;
    }
    vWorld = world.xyz;
    vUv = aUv; vColor = aColor;
    gl_Position = w.viewProj * world;
}
