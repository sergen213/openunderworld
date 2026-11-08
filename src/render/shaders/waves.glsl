// The Enhanced water surface's waves, one source for water.vert (displacement), water.frag (normals) and caustics.glsl
// (the bed lit through those same waves), so their shapes are identical. Embedded for GL by render/Water.cpp (expands
// the #include line), #included by glslc for Vulkan. ouWaves needs the includer's block `w` (ripple, rippleInfo,
// ocean) and `uRipples` (game::WaveField heights), the ouOcean* functions `uOcean` (the FFT ocean, shaders/ocean.comp).
// docs/RENDERING.md "Water".
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
// ---- FFT ocean (render/Ocean.hpp, shaders/ocean.comp; open seas when w.ocean[0].x = 1, replacing afl_ext's sea and
// the Godot layers there). uOcean layers per cascade c: 3c (Dx, Dy, h, Jxy), 3c + 1 (dh/dx, dh/dy, Jxx, Jyy),
// 3c + 2 (foam, J); metres, on tiles w.ocean[1][c] world units wide (swell 240 m, gravity 28 m, chop 11.3 m). Level
// w.ocean[0].y holds the frame's grid exactly (water.vert, the caustics); level 0 always holds it too (grid 64 doubled)
// and the levels below are its 2x2 means, which water.frag filters through.
bool ouOceanOn() { return w.ocean[0].x > 0.5; }
float ouOceanTexel(int c) { return w.ocean[1][c] / w.ocean[0].z; } // world units per texel
vec4 ouOceanFetch(int c, int layer, vec2 p) {
    return textureLod(uOcean, vec3(p / w.ocean[1][c], float(3 * c + layer)), w.ocean[0].y);
}
vec2 ouOceanSlope(int c, vec2 p) { return ouOceanFetch(c, 1, p).xy; } // dh/dx, dh/dy of cascade c (no fade)
// Swell + gravity height at p in world units (water.vert; the chop cascade only shades: NeuralSpecOcean's chop band
// has no height either).
float ouOceanHeight(vec2 p) { return (ouOceanFetch(0, 0, p).z + ouOceanFetch(1, 0, p).z) * kM; }
struct OuOcean {
    vec2 slope;          // dh/dx, dh/dy
    float jxx, jyy, jxy; // horizontal displacement gradients (choppiness applied)
    float h, foam, J;    // height (m), persisted whitecap foam 0..1, Jacobian of the summed displacement
};
const mat2 kOceanChopRot = mat2(0.857, 0.515, -0.515, 0.857); // 31 deg: the chop tile's second, rotated lookup
// The chop tile again at another scale, rotation and offset (NeuralSpecOcean Offshore.js's bands 3 and 4: its chop
// texture at 3.42 / 5.53 m and 1.04 / 1.68 m, rotated -23 / 49 / 31 / -61 deg, weights 0.50 x (0.62, 0.56) and
// 0.42 x (0.58, 0.58) against its 11.3 m band's 0.58): its slopes as they are (the k^-4 spectrum is self-similar in
// slope), filtered by the tile's own mips with the pixel's derivatives (dx, dy: world units per pixel).
vec2 ouOceanChopCopy(vec2 p, vec2 dx, vec2 dy, mat2 r, float tileM, vec2 offset) {
    float s = 1.0 / (tileM * kM);
    vec2 g = textureGrad(uOcean, vec3(r * p * s + offset, 7.0), r * dx * s, r * dy * s).xy; // layer 3 * 2 + 1
    return transpose(r) * g;
}
// The surface at p for shading: every cascade and the chop copies, filtered through uOcean's mips with the pixel's
// world-space derivatives (anisotropic, so grazing views keep the waves across the line of sight) instead of faded
// out, so far water neither shimmers nor turns smooth; plus the chop tile again rotated and 1.618x larger against
// visible tiling and, near the camera, NeuralSpecOcean's finer copies of it (above), so the near water has detail
// below the 11.3 m tile's texels (8.8 / 17.7 cm) instead of their bilinear blobs [I weights]. The swell and gravity
// foam (texels 1.9 / 0.22 m at grid 128) is read through a cubic B-spline (4 bilinear taps) where its texels are wider
// than the pixel, so the whitecap patches round off instead of showing the texel grid's straight bilinear edges.
vec4 ouOceanCubic(float layer, vec2 uv, vec2 gx, vec2 gy) {
    float n = w.ocean[0].z;
    vec2 t = uv * n - 0.5, i = floor(t), f = t - i;
    vec2 f2 = f * f, f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0, w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0, w3 = f3 / 6.0;
    vec2 g0 = w0 + w1, g1 = w2 + w3;
    vec2 h0 = (i - 0.5 + w1 / g0) / n, h1 = (i + 1.5 + w3 / g1) / n;
    return g0.y * (g0.x * textureGrad(uOcean, vec3(h0.x, h0.y, layer), gx, gy) + g1.x * textureGrad(uOcean, vec3(h1.x, h0.y, layer), gx, gy)) +
           g1.y * (g0.x * textureGrad(uOcean, vec3(h0.x, h1.y, layer), gx, gy) + g1.x * textureGrad(uOcean, vec3(h1.x, h1.y, layer), gx, gy));
}
OuOcean ouOceanSurface(vec2 p, vec2 dx, vec2 dy) {
    OuOcean o = OuOcean(vec2(0.0), 0.0, 0.0, 0.0, 0.0, 0.0, 1.0);
    for (int c = 0; c < 3; ++c) {
        float s = 1.0 / w.ocean[1][c];
        vec2 uv = p * s, gx = dx * s, gy = dy * s;
        vec4 a = textureGrad(uOcean, vec3(uv, float(3 * c)), gx, gy), b = textureGrad(uOcean, vec3(uv, float(3 * c + 1)), gx, gy);
        o.slope += b.xy, o.jxx += b.z, o.jyy += b.w, o.jxy += a.w;
        if (c < 2) o.h += a.z;
        float layer = float(3 * c + 2);
        bool magnified = c < 2 && min(length(gx), length(gy)) * w.ocean[0].z < 1.0; // texels wider than the pixel
        o.foam = max(o.foam, (magnified ? ouOceanCubic(layer, uv, gx, gy) : textureGrad(uOcean, vec3(uv, layer), gx, gy)).x);
    }
    o.slope += 0.6 * ouOceanChopCopy(p, dx, dy, kOceanChopRot, 1.618 * w.ocean[1][2] / kM, vec2(0.0));
    // NeuralSpecOcean's fine bands, weights relative to the 11.3 m tile here (1 = its 0.58), faded out from 4 to 16 of
    // their texels per pixel as its bands are (Offshore.js nyq3 / nyq4; beyond, their mips only repeat the coarser
    // tiles' scales), the fetches skipped there.
    float lx = length(dx), ly = length(dy);
    float px = max(min(lx, ly), max(lx, ly) / 8.0) / kM; // metres per filtered pixel (up to 8x anisotropic, render::ocean::kMaxAniso)
    float n = w.ocean[0].z;
    float k3 = 1.0 - smoothstep(4.0, 16.0, px * n / 5.53), k4 = 1.0 - smoothstep(4.0, 16.0, px * n / 1.68);
    if (k3 > 0.0) {
        o.slope += (0.534 * k3) * ouOceanChopCopy(p, dx, dy, mat2(0.9205, 0.3907, -0.3907, 0.9205), 3.42, vec2(0.73, 0.67));
        o.slope += (0.483 * k3) * ouOceanChopCopy(p, dx, dy, mat2(0.6561, -0.7547, 0.7547, 0.6561), 5.53, vec2(0.29, 0.81));
    }
    if (k4 > 0.0) {
        o.slope += (0.42 * k4) * ouOceanChopCopy(p, dx, dy, mat2(0.8572, -0.5150, 0.5150, 0.8572), 1.04, vec2(0.31, 0.89));
        o.slope += (0.42 * k4) * ouOceanChopCopy(p, dx, dy, mat2(0.4848, 0.8746, -0.8746, 0.4848), 1.68, vec2(0.64, 0.22));
    }
    o.J = (1.0 + o.jxx) * (1.0 + o.jyy) - o.jxy * o.jxy;
    return o;
}
// The slope water.frag builds its normal (-s, 1) from: the choppy surface x + D(x)'s normal (tangents (1 + Jxx, Jxy,
// dh/dx), (Jxy, 1 + Jyy, dh/dy)), the folded area (J -> 0) clamped.
vec2 ouOceanNormalSlope(OuOcean o) {
    vec3 n = vec3(o.jxy * o.slope.y - o.slope.x * (1.0 + o.jyy), o.slope.x * o.jxy - (1.0 + o.jxx) * o.slope.y, max(o.J, 0.15));
    return -n.xy / n.z;
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
