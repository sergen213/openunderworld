// The Enhanced water surface's waves, one source for water.vert (displacement), water.frag (normals) and caustics.glsl
// (the bed lit through those same waves), so their shapes are identical. Embedded for GL by render/Water.cpp (expands
// the #include line), #included by glslc for Vulkan. ouWaves needs the includer's block `w` (ripple, rippleInfo) and
// `uRipples` (game::WaveField heights). docs/RENDERING.md "Water".
#ifndef OU_WAVES_GLSL
#define OU_WAVES_GLSL
const float kM = 290.0; // TRU units per metre (Lara ~1.75 m = ~512 units)
float ouLattice(vec2 c) {
    uvec2 q = uvec2(ivec2(c));
    uint n = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    return float(n >> 8) * (1.0 / 16777216.0);
}
vec3 ouNoiseD(vec2 p) { // value noise in [0, 1] and its gradient
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    float a = ouLattice(i), b = ouLattice(i + vec2(1, 0)), c = ouLattice(i + vec2(0, 1)), d = ouLattice(i + vec2(1, 1));
    float k = a - b - c + d;
    return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y, du * vec2(b - a + k * u.y, c - a + k * u.x));
}
// 4-octave fbm: x height in [0, 1], yz gradient per lattice unit. Octaves finer than ~4 pixels (px: lattice units per
// pixel; 0 = all octaves, as water.vert displaces) fade to their mean; fully faded ones are not evaluated (same value;
// the coarse caustic lookups of the sun shafts get cheap).
vec3 ouFbm(vec2 x, float px) {
    vec3 acc = vec3(0.0);
    float a = 0.5, f = 1.0, norm = 0.0;
    for (int i = 0; i < 4; ++i) {
        float k = a * (1.0 - smoothstep(0.25, 0.5, px * f));
        vec3 n = k > 0.0 ? ouNoiseD(x * f + vec2(17.3, 5.1) * float(i)) : vec3(0.0);
        acc += vec3(k * n.x + (a - k) * 0.5, k * f * n.yz);
        norm += a;
        a *= 0.5, f *= 2.03;
    }
    return acc / norm;
}
const mat2 kWaterRot = mat2(0.8, 0.6, -0.6, 0.8);
const float kLambdaA = 2.0 * kM, kLambdaB = 1.4 * kM; // Godot samplerscale 0.1: ~5 noise features per 10 m tile
const float kNormalAmp = 0.12 * kM;                    // height the Godot normals come from (normalstrength)
// Godot layer 1 / 2 (normal1tex + height1tex at UV, normal2tex + height2tex at UV2; 0.2 m/s, here crossing at an
// angle): x height in [0, 1], yz the slope of kNormalAmp x that height in world xy.
vec3 ouLayerA(vec2 p, float t, float footprint) {
    vec3 n = ouFbm(p / kLambdaA + t * vec2(0.10, 0.02), footprint / kLambdaA);
    return vec3(n.x, n.yz * (kNormalAmp / kLambdaA));
}
vec3 ouLayerB(vec2 p, float t, float footprint) {
    vec3 n = ouFbm(kWaterRot * p / kLambdaB + t * vec2(-0.04, 0.12), footprint / kLambdaB);
    return vec3(n.x, transpose(kWaterRot) * n.yz * (kNormalAmp / kLambdaB));
}
// afl_ext's "Very fast procedural ocean" (https://www.shadertoy.com/view/MdXyzX, 2017; "afl_ext 2017-2024, MIT
// License"; xscreensaver prococean: Copyright (C) 2017 by afl_ext. Permission to use, copy, modify, distribute, and
// sell this software and its documentation for any purpose is hereby granted without fee, provided that the above
// copyright notice appear in all copies and that both that copyright notice and this permission notice appear in
// supporting documentation. No representations are made about the suitability of this software for any purpose.
// It is provided "as is" without express or implied warranty.) wavedx / getwaves, ported: position in afl units
// (kSeaUnit TRU units), time as an argument. Returns the wave height in about [0, 1].
const float kSeaUnit = 1.0 * kM;  // TRU units per afl unit: the longest waves ~6 m
const float kSeaAmp = 0.45 * kM;  // TRU height of afl's 0..1 as displaced (water.vert)
const float kSeaNormalAmp = kSeaUnit; // ... as shaded: afl's own proportions (WATER_DEPTH 1 afl unit)
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
// tru.exe's sea waves (its vertex shader 0x69DFB0, render::waterWaves columns c60-c63): the height
// -sum c63 (1 - cos(x c60 + y c61 + c62)) and here its gradient.
vec2 ouTruWaveSlope(vec2 p, mat4 c) {
    vec4 a = c[3] * sin(p.x * c[0] + p.y * c[1] + c[2]);
    return -vec2(dot(a, c[0]), dot(a, c[1]));
}
// Interactive waves (game::WaveField): xy the height gradient (taps 1.5 cells apart so the cell-sized checkerboard
// bodies stamp cancels), z the height, w its Laplacian (negative on crests); faded over the field's outer 4 %.
vec4 ouWaves(vec2 p) {
    if (w.rippleInfo.y < 0.5) return vec4(0.0);
    float inv = 1.0 / (w.ripple.z * w.rippleInfo.x);
    vec2 uv = (p - w.ripple.xy) * inv;
    vec2 in2 = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(in2.x, in2.y));
    if (fade == 0.0) return vec4(0.0);
    float d = 1.5 * w.ripple.z, du = d * inv;
    float h = textureLod(uRipples, uv, 0.0).r;
    float x0 = textureLod(uRipples, uv - vec2(du, 0.0), 0.0).r, x1 = textureLod(uRipples, uv + vec2(du, 0.0), 0.0).r;
    float y0 = textureLod(uRipples, uv - vec2(0.0, du), 0.0).r, y1 = textureLod(uRipples, uv + vec2(0.0, du), 0.0).r;
    return fade * vec4(vec2(x1 - x0, y1 - y0) / (2.0 * d), h, (x0 + x1 + y0 + y1 - 4.0 * h) / (d * d));
}
#endif
