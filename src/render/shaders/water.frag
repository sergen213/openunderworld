#version 460
// Enhanced water surface. Base: "Realistic Water with Traced and Simple Reflection and Refraction V2" by
// smallcableboi (godotshaders.com, CC0; docs/RESEARCH.md), translated from Godot to TRU and improved:
// - Schlick Fresnel with the real cosine (the original fed the dot product to cos() and divided by it).
// - Two scrolling normal + height layers (the original's noise textures; here procedural value-noise fbm, anti-aliased
//   per octave and faded with distance so far water does not shimmer; the glint widens instead).
// - Absorption pow(absorption colour, metres) along the refracted path, plus in-scattering from teal (shallow) to deep
//   blue (the original goes black in deep water).
// - Traced refraction / SSR: quadratic steps + binary refinement (the original: 512 steps, a shadowed never-updated
//   `lastdepth1`, an uninitialised depth, no screen bound for refraction); a traced refraction that leaves the screen
//   or passes behind a foreground object falls back to the simple depth-scaled normal offset.
// - Edge foam from the water depth broken by an edge noise, a soft waterline, sun / lamp GGX glints.
// - From below: Snell's window (Fresnel from the air side) and total internal reflection outside it.
// Open seas (meshes with TRU's sea waves, vSea): afl_ext's "Very fast procedural ocean" waves (MIT, notice below)
// displace (water.vert) and bend the normals, with fewer iterations, a wider difference step and afl's normal
// smoothing with distance; its subsurface term (crests brighter by height) and sharp sun disc in the reflection.
// The look also takes cues from Shadertoy "Seascape" by TDM (CC BY-NC-SA: no code used).
// Plus ours: the interactive wave field (game::WaveField) bends the normals, foams and displaces (water.vert).
// The scene copy is gamma (D3D9-style): colours go to linear and back. docs/RENDERING.md "Water".
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; // (unused here; the post pass's screen filters)
    vec4 waterPass[3]; // the unit's water look: [0] surface rgb, [1] deep rgb + full-fog distance (ouWaterLook)
} w;
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D uScene;
layout(set = 0, binding = 2) uniform sampler2D uDepth;
layout(set = 0, binding = 3) uniform sampler2D uRipples;
#else
layout(binding = 5) uniform sampler2D uScene;
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 7) uniform sampler2D uRipples;
#endif
// Sun shadow map (render/sun.glsl; Vulkan: water layout set 1) for the caustics on the refracted bed.
#ifdef VULKAN
#define OU_SHADOW_SET 1
#endif
#include "sun.glsl"
#include "shaders/caustics.glsl"
float ouSunShadowVisibility(vec3 p) { return ouSunVisibility(p, vec3(0.0), true, false); }
#ifdef VULKAN
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#define OU_UV_Y(ndcy) (0.5 - 0.5 * (ndcy))
#else
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#define OU_UV_Y(ndcy) (0.5 + 0.5 * (ndcy))
#endif
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;
layout(location = 4) flat in float vSea; // 1 = an open sea (TRU's waves bound), 0 = pools / still water
layout(location = 0) out vec4 oColor;

// ---- noise (keep the layer constants in sync with water.vert) ----
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
// pixel) fade to their mean.
vec3 ouFbm(vec2 x, float px) {
    vec3 acc = vec3(0.0);
    float a = 0.5, f = 1.0, norm = 0.0;
    for (int i = 0; i < 4; ++i) {
        float k = a * (1.0 - smoothstep(0.25, 0.5, px * f));
        vec3 n = ouNoiseD(x * f + vec2(17.3, 5.1) * float(i));
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
// Godot edge1tex / edge2tex: two scrolling fine noises mixed, in [0, 1].
float ouEdgeNoise(vec2 p, float t) {
    return 0.5 * (ouNoiseD(p / (0.30 * kM) + t * vec2(0.30, 0.10)).x + ouNoiseD(p / (0.22 * kM) - t * vec2(0.10, 0.25)).x);
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

const float kPi = 3.14159265;
const float kIor = 1.33;                       // Godot IOR (air 1.0)
const float kF0 = 0.02;                        // ((1.33 - 1) / (1.33 + 1))^2
// Default water look where the unit has none (scene::Environment::waterPass[1].w = 0): l2_cliffclimb's (Coastal Thailand).
const vec4 kSurfaceDefault = vec4(0.447, 0.780, 0.792, 0.4), kDeepDefault = vec4(0.216, 0.439, 0.427, 100000.0);
const float kEdge = 0.15 * kM;                 // Godot edge_size (0.1 m)

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toGamma(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 ouWorld(vec2 uv, float depth) {
    vec4 p = w.invViewProj * vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    return p.xyz / p.w;
}
vec2 ouScreenUv(vec3 q, out bool inside) {
    vec4 clip = w.viewProj * vec4(q, 1.0);
    vec2 uv = vec2(clip.x / max(clip.w, 1e-4) * 0.5 + 0.5, OU_UV_Y(clip.y / max(clip.w, 1e-4)));
    inside = clip.w > 0.0 && all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)));
    return uv;
}
// Godot edge_fade: 0 at the screen border, 1 `size` inside it.
float ouEdgeFade(vec2 uv, float size) {
    vec2 e = clamp(min(uv, 1.0 - uv) / size, 0.0, 1.0);
    return e.x * e.y;
}
bool ouBehind(vec3 q, out vec2 uv, out bool inside) { // q lies behind the opaque scene at its pixel
    uv = ouScreenUv(q, inside);
    float sd = textureLod(uDepth, uv, 0.0).r;
    return sd < 0.999999 && distance(ouWorld(uv, sd), w.eye.xyz) < distance(q, w.eye.xyz);
}
// Godot's traced march for refraction and SSR: from p along d, quadratic steps out to maxT, then a binary refinement
// between the last two steps. Fails when the ray leaves the screen, reaches maxT, or the refined crossing lies more
// than `thick` behind the surface found (a foreground object in front of the ray, not the surface it reaches).
bool ouTrace(vec3 p, vec3 d, float maxT, float thick, out vec2 hitUv, out float hitT) {
    const int kSteps = 14;
    float prev = 0.0;
    bool inside;
    vec2 uv;
    for (int i = 1; i <= kSteps; ++i) {
        float s = float(i) / float(kSteps), t = s * s * maxT;
        bool behind = ouBehind(p + d * t, uv, inside);
        if (!inside) return false;
        if (behind) {
            float lo = prev, hi = t;
            for (int b = 0; b < 5; ++b) {
                float mid = 0.5 * (lo + hi);
                if (ouBehind(p + d * mid, uv, inside)) hi = mid; else lo = mid;
            }
            vec3 h = p + d * hi;
            hitUv = ouScreenUv(h, inside);
            float hd = textureLod(uDepth, hitUv, 0.0).r;
            if (distance(h, w.eye.xyz) - distance(ouWorld(hitUv, hd), w.eye.xyz) > thick + 0.05 * hi) return false;
            hitT = hi;
            return inside;
        }
        prev = t;
    }
    return false;
}
// Screen-space sun occlusion of the glint (water detail), 8 steps toward the sun.
float ouSunVisibility(vec3 p) {
    bool inside;
    for (int i = 1; i <= 8; ++i) {
        float s = float(i) / 8.0, t = 20.0 + s * s * 2500.0;
        vec3 q = p + w.sunDir.xyz * t;
        vec2 uv = ouScreenUv(q, inside);
        if (!inside) break;
        float sd = textureLod(uDepth, uv, 0.0).r;
        if (sd >= 0.999999) continue;
        float diff = distance(q, w.eye.xyz) - distance(ouWorld(uv, sd), w.eye.xyz);
        if (diff > 10.0 && diff < 80.0 + 0.2 * t) return 0.0;
    }
    return 1.0;
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
float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}
float v_smith(float nv, float nl, float a) {
    float a2 = a * a;
    return 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-5);
}
float schlick(float c) { return kF0 + (1.0 - kF0) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0); }
float ggxSpec(vec3 n, vec3 v, vec3 l, float a) {
    vec3 h = normalize(l + v);
    float nl = max(dot(n, l), 0.0), nv = max(dot(n, v), 1e-3);
    return d_ggx(max(dot(n, h), 0.0), a) * v_smith(nv, nl, a) * nl * schlick(dot(v, h));
}
// The unit's water look (WaterBlock waterPass = scene::Environment::waterPass, tru.exe's under-water pass material):
// in-scatter of its deep colour, extinction per unit from its surface colour and full-fog distance with
// red going first (1 - 0.85 x surface colour: Thailand's teal ~13 m to 1/e for red, ~23 m for green, so its reef ~10 m
// down shows as in the original; the North Sea ~5 m).
vec3 gSurf, gDeep, gSigma;
void ouWaterLook() {
    bool unit = w.waterPass[1].w > 0.0;
    vec4 sf = unit ? w.waterPass[0] : kSurfaceDefault, dp = unit ? w.waterPass[1] : kDeepDefault;
    gSurf = toLinear(sf.rgb), gDeep = toLinear(dp.rgb);
    gSigma = (1.0 - 0.85 * gSurf) * (30.0 / dp.w);
}
// Water transmittance and in-scatter over `path` units; `down`: how far the seen point lies under the surface, dimming
// the light that reached it (downwelling: submerged hulls darken with depth). The in-scatter colour is one colour, not
// keyed on the depth buffer: a shark or hull nearer than the bed would otherwise take a paler colour and show as a flat
// cutout; shallow beds read lighter by their own transmitted colour.
vec3 ouAbsorb(vec3 below, float path, float down, vec3 light) {
    vec3 tr = exp(-gSigma * min(path, 1e5));
    return below * tr * exp(-gSigma * min(down, 1e5)) + gDeep * light * (1.0 - tr);
}

void main() {
    vec3 p = vWorld;
    float t = w.eye.w, dist = distance(w.eye.xyz, p);
    ouWaterLook();
    float footprint = length(fwidth(p.xy)); // world units per pixel
    bool detail = w.detail.x > 0.5, sea = vSea > 0.5;

    // Normals: the Godot layers mixed 50/50 (samplermix), faded with distance; open seas add afl_ext's waves (36
    // iterations near, 12 far; difference step >= a pixel; smoothed toward flat with distance as afl does).
    vec3 la = ouLayerA(p.xy, t, footprint), lb = ouLayerB(p.xy, t, footprint);
    float nfade = 1.0 / (1.0 + dist / (60.0 * kM));
    vec2 s = 0.5 * (la.yz + lb.yz) * nfade * (sea ? 0.5 : 1.0);
    float seaH = 0.5;
    if (sea) {
        int iters = dist < 25.0 * kM ? 36 : dist < 80.0 * kM ? 24 : 12;
        float eps = max(0.01 * kSeaUnit, footprint);
        seaH = ouSea(p.xy, t, iters);
        vec2 g = vec2(ouSea(p.xy + vec2(eps, 0.0), t, iters) - seaH, ouSea(p.xy + vec2(0.0, eps), t, iters) - seaH) / eps;
        s += g * kSeaNormalAmp * (1.0 - 0.8 * min(1.0, sqrt(dist / kSeaUnit * 0.01) * 1.1));
    }
    vec4 wave = ouWaves(p.xy);
    s += wave.xy * (w.fogRange.z * 1.2 / (1.0 + 4.0 * length(wave.xy)));
    vec3 n = normalize(vec3(-s, 1.0)); // a height field's normal: (-dh/dx, -dh/dy, 1)
    bool below = w.eye.z < p.z;        // seen from under the surface
    vec3 nf = below ? -n : n;          // the normal facing the eye
    vec3 v = normalize(w.eye.xyz - p);
    float cosI = max(dot(nf, v), 1e-3);

    vec2 uv = gl_FragCoord.xy * w.screen.zw;
    float sd0 = textureLod(uDepth, uv, 0.0).r;
    bool open = sd0 >= 0.999999;
    vec3 bed0 = ouWorld(uv, sd0);
    float depth = open ? 1e5 : max(p.z - bed0.z, 0.0); // perpendicular water depth under this pixel
    // Foam, the reflection fade and the soft waterline belong at contact lines (a shore or a hull side rising through the
    // surface), not over a flat hull bottom or a swimmer lying a few units under it. The bed's slope from its screen
    // derivatives gives the horizontal distance to the contact (depth / slope); a jump in depth across a pixel is an
    // object's outline over a deeper bed, not a slope. Non-contacts count as a metre deeper.
    vec3 bx = dFdx(bed0), by = dFdy(bed0);
    vec3 bn = cross(bx, by);
    float run = depth * abs(bn.z) / max(length(bn.xy), 1e-3 * abs(bn.z) + 1e-6);
    bool jump = max(length(bx), length(by)) > 16.0 * footprint; // also a grazing wall: only matters off the waterline
    float cdepth = open ? depth : depth + kM * (jump ? smoothstep(0.02 * kM, 0.1 * kM, depth) : smoothstep(0.5 * kM, 1.5 * kM, run));
    vec3 light = toLinear(min(w.ambient.rgb + w.sunColor.rgb * max(w.sunDir.z, 0.0), vec3(1.0)));
    vec3 fogLin = toLinear(w.fogColor.rgb);
    // Sky for reflection misses: the fog colour, brightening toward the horizon as afl_ext's sky (x (1 + (1 - up)^3),
    // so wave faces tilted to the camera read darker than those mirroring the horizon), and a soft sun lobe.
    vec3 r = reflect(-v, nf);
    vec3 rs = normalize(vec3(r.xy, abs(r.z)));
    vec3 sky = toLinear(w.fogColor.rgb * (0.6 + 0.6 * pow(1.0 - rs.z, 3.0)) +
                        w.sunColor.rgb * (0.12 * pow(max(dot(rs, w.sunDir.xyz), 0.0), 12.0)));

    vec3 c;
    float fresnel, conf = 0.0, path = 0.0, spec = 0.0, sunVis = 1.0;
    bool traced = false;
    vec2 huv;
    float ht;
    if (!below) {
        // ---- refraction (Godot refraction_type 2 = Traced, falling back to 1 = Simple) ----
        vec3 rd = refract(-v, n, 1.0 / kIor);
        vec2 ruv = uv;
        if (detail && !open && ouTrace(p, rd, 30.0 * kM, 1.5 * kM, huv, ht) &&
            ouWorld(huv, textureLod(uDepth, huv, 0.0).r).z < p.z + 2.0) // else it found something above the water
            traced = true, ruv = huv, path = ht;
        if (!traced) {
            // Simple: the normal offset scaled by how far the bed lies behind the surface (Godot depth_diff / depth).
            float ds = open ? 1e5 : distance(bed0, w.eye.xyz);
            ruv = uv + s * (0.6 * clamp((ds - dist) / max(ds, 1.0), 0.0, 0.25));
            float sd = textureLod(uDepth, ruv, 0.0).r;
            vec3 bed = ouWorld(ruv, sd);
            if (sd < 0.999999 && bed.z > p.z) ruv = uv, sd = sd0, bed = bed0; // the offset reached the shore
            path = sd >= 0.999999 ? 1e5 : distance(p, bed);
        }
        float rsd = textureLod(uDepth, ruv, 0.0).r;
        vec3 rbed = ouWorld(ruv, rsd);
        float down = rsd >= 0.999999 ? 1e5 : max(p.z - rbed.z, 0.0);
        vec3 seen = toLinear(textureLod(uScene, ruv, 0.0).rgb);
        // Caustics (shaders/caustics.glsl) light the bed before the water absorbs it; the normal and footprint are the
        // unrefracted bed's (derivatives outside this branch).
        if (rsd < 0.999999) seen *= ouCausticLight(rbed, bn, down, length(abs(bx.xy) + abs(by.xy)));
        vec3 refr = ouAbsorb(seen, path, down, light);
        // Sea: afl's subsurface scattering, brighter where the wave stands higher (thin crest water lit through).
        if (sea) refr += gDeep * light * (1.6 * seaH * (0.4 + 0.6 * pow(1.0 - cosI, 2.0)));

        // ---- reflection: traced SSR, the sky where it misses or fades at the screen border ----
        fresnel = schlick(cosI);
        vec3 refl = sky;
        if (ouTrace(p + vec3(0.0, 0.0, 2.0), rs, 40.0 * kM, 2.0 * kM, huv, ht)) {
            conf = ouEdgeFade(huv, 0.08) * (1.0 - smoothstep(0.6, 1.0, ht / (40.0 * kM)));
            refl = mix(sky, toLinear(textureLod(uScene, huv, 0.0).rgb), conf);
        }
        float shore = smoothstep(0.0, 0.2 * kM, cdepth); // the reflection fades in off the waterline
        c = mix(refr, refl, clamp(fresnel * w.fogRange.w, 0.0, 1.0) * shore);

        // Sun glint (GGX; rougher far away, where the normals were faded) + on seas afl's sharp sun disc seen in the
        // reflection (pow 720 x 210, x Fresnel); nearby lamps (water detail).
        sunVis = detail && dot(n, w.sunDir.xyz) > 0.0 ? ouSunVisibility(p + n * 2.0) : 1.0;
        spec = ggxSpec(n, v, w.sunDir.xyz, mix(0.10, 0.04, nfade));
        if (sea) spec += pow(max(dot(rs, w.sunDir.xyz), 0.0), 720.0) * 210.0 * fresnel;
        spec *= sunVis * w.sunDir.w;
        c += spec * toLinear(min(w.sunColor.rgb, vec3(1.0))) * shore;
        if (detail)
            for (int i = 0; i < 4; ++i) {
                if (w.lamps[i].w <= 0.0) continue;
                vec3 L = w.lamps[i].xyz - p;
                float ld = length(L), reach = clamp(1.0 - ld / w.lamps[i].w, 0.0, 1.0);
                if (reach > 0.0)
                    c += toLinear(min(w.lampColor[i].rgb, vec3(1.0))) * (ggxSpec(n, v, L / ld, 0.12) * reach * reach * shore);
            }
        // Wave-field crests catch light and troughs darken (curvature: fronts read as lines, sloshing stays a tilt),
        // growing with the curvature: a splash's 45-unit crater (bend ~14) is a bright line, a stroke ring a tint.
        float bend = -640.0 * wave.w, crest = sign(bend) * smoothstep(2.0, 20.0, abs(bend)) * shore;
        c = mix(c, crest > 0.0 ? mix(sky, fogLin, 0.5) : c * 0.5, 0.45 * abs(crest));

        // Edge foam (Godot: foam where the edge texture exceeds depth / edge_size) + the wave field's churn.
        float e = ouEdgeNoise(p.xy, t);
        float foam = smoothstep(-0.05, 0.05, e * e - cdepth / kEdge); // squared: lace, not a solid band
        float churn = max(smoothstep(25.0, 45.0, abs(wave.z)), smoothstep(0.02, 0.04, -wave.w));
        foam = max(foam, churn * smoothstep(0.3, 0.7, e));
        c = mix(c, light * 0.9, foam * 0.65);
        // Soft waterline (Godot foam_or_fade = fade): the bed shows through the last few units.
        c = mix(toLinear(textureLod(uScene, uv, 0.0).rgb), c, smoothstep(0.0, 6.0, cdepth));
    } else {
        // ---- from below: Snell's window ----
        float sin2T = kIor * kIor * (1.0 - cosI * cosI);
        fresnel = sin2T >= 1.0 ? 1.0 : schlick(sqrt(1.0 - sin2T)); // Fresnel on the air side; TIR outside the window
        vec3 window = sky;
        if (fresnel < 1.0) {
            vec3 td = refract(-v, nf, kIor);
            if (ouTrace(p, td, 60.0 * kM, 3.0 * kM, huv, ht) && ouWorld(huv, textureLod(uDepth, huv, 0.0).r).z > p.z - 2.0) {
                traced = true;
                window = toLinear(textureLod(uScene, huv, 0.0).rgb);
            } else { // simple: the scene at the normal-offset pixel when it lies above the water, else the sky
                vec2 ruv = uv + s * 0.08;
                float sd = textureLod(uDepth, ruv, 0.0).r;
                if (sd < 0.999999 && ouWorld(ruv, sd).z > p.z) window = toLinear(textureLod(uScene, ruv, 0.0).rgb);
            }
        }
        // Total internal reflection: the water below by SSR, else the deep in-scatter colour.
        vec3 tir = gDeep * light;
        if (fresnel > 0.0 && ouTrace(p - vec3(0.0, 0.0, 2.0), r, 30.0 * kM, 2.0 * kM, huv, ht)) {
            conf = ouEdgeFade(huv, 0.08);
            tir = mix(tir, ouAbsorb(toLinear(textureLod(uScene, huv, 0.0).rgb), ht, 0.0, light), conf);
        }
        c = mix(window, tir, fresnel);
    }

    vec3 color = toGamma(c);
    if (!below) { // the scene's distance fog to the horizon (under water the post pass fogs, ouWaterPass)
        float fogT = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
        color = mix(color, w.fogColor.rgb, fogT);
    }
    int view = int(w.detail.y + 0.5);
    if (view == 1) color = n * 0.5 + 0.5;                                                                  // normals
    else if (view == 2) color = mix(traced ? vec3(0, 1, 0.3) : vec3(1, 0, 1), vec3(0, 0.2, 1), clamp(path / 3000.0, 0.0, 1.0)); // refraction traced / simple
    else if (view == 3) color = mix(vec3(1, 0.15, 0), vec3(0.1, 1, 0.2), conf);                            // SSR hit vs sky
    else if (view == 4) color = vec3(clamp(spec, 0.0, 1.0), clamp(spec, 0.0, 1.0) * sunVis, 1.0 - sunVis); // glint
    else if (view == 5) color = vec3(fresnel);                                                             // Fresnel
    oColor = vec4(color, 1.0);
}
