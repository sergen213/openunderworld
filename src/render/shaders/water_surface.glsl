// The one water surface every Enhanced effect reads (contract C2; docs/RENDERING.md "Interactive waves"): tru.exe's
// sea waves + the open sea's FFT ocean (or afl_ext's waves without it) + the interactive wave field, as a height offset
// from the volume's rest surface (TRU units, z up) and its slope. Needs the includer's block `w` (ripple, rippleInfo,
// ocean, seaWaves, eye, detail) and samplers `uRipples` and `uOcean`. Embedded for GL by render/Water.cpp (expands the
// #include line), #included by glslc for Vulkan.
//
// uRipples (contract C1; render/WaterField.hpp, written each frame by shaders/water_field.comp): rgba16f, waterfield::kN
// texels per side over the window, texel (0, 0)'s corner at world w.ripple.xy, w.ripple.z world units per texel,
// w.ripple.w the rest surface z the field rides on, w.rippleInfo.x = kN, w.rippleInfo.y = 1 while the field runs.
//   r  height above the rest surface (TRU units; the waves plus the dips of floating / wading bodies)
//   g  dh/dx, b dh/dy (unitless)
//   a  whitewater 0..1: the foam the waves made (breaking crests, a collapsing cavity's surge and the cavity itself,
//      drops falling back, strokes), fading over ~1.5 s
// The field is a perturbation riding on the other waves: its heights add to theirs.
#ifndef OU_WATER_SURFACE_GLSL
#define OU_WATER_SURFACE_GLSL
#include "shaders/waves.glsl"

// The interactive field at world p: xy the height gradient, z the height, w its Laplacian (negative on crests); faded
// over the window's outer 4 % (the field's absorbing sponge lives there too).
vec4 ouFieldSample(vec2 p) {
    if (w.rippleInfo.y < 0.5) return vec4(0.0);
    float inv = 1.0 / (w.ripple.z * w.rippleInfo.x);
    vec2 uv = (p - w.ripple.xy) * inv;
    vec2 in2 = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(in2.x, in2.y));
    if (fade == 0.0) return vec4(0.0);
    float d = 1.5 * w.ripple.z, du = d * inv;
    vec4 c = textureLod(uRipples, uv, 0.0);
    float sx0 = textureLod(uRipples, uv - vec2(du, 0.0), 0.0).g, sx1 = textureLod(uRipples, uv + vec2(du, 0.0), 0.0).g;
    float sy0 = textureLod(uRipples, uv - vec2(0.0, du), 0.0).b, sy1 = textureLod(uRipples, uv + vec2(0.0, du), 0.0).b;
    return fade * vec4(c.gb, c.r, (sx1 - sx0 + sy1 - sy0) / (2.0 * d));
}
float ouFieldHeight(vec2 p) {
    if (w.rippleInfo.y < 0.5) return 0.0;
    vec2 uv = (p - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
    vec2 in2 = min(uv, 1.0 - uv);
    return smoothstep(0.0, 0.04, min(in2.x, in2.y)) * textureLod(uRipples, uv, 0.0).r;
}
// Its whitewater (0..1; same fade).
float ouFieldFoam(vec2 p) {
    if (w.rippleInfo.y < 0.5) return 0.0;
    vec2 uv = (p - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
    vec2 in2 = min(uv, 1.0 - uv);
    return smoothstep(0.0, 0.04, min(in2.x, in2.y)) * textureLod(uRipples, uv, 0.0).a;
}
// The field's caustics on a lit bed point under the surface at rest height surfaceZ: sunlight refracted by the field's
// waves focuses under crests and spreads under troughs. For the small slopes of the field the refracted rays land
// displaced by D (1 - 1/n) grad h (D = the depth under the surface), so the landed area changes by the Jacobian
// J = 1 + D (1 - 1/n) lap h and the light by 1 / |J| (past the focus the rays cross: still |J|), capped at 4 at the
// folds; the Laplacian is read where the refracted sun ray through `bed` crossed the surface. Faded to 1 from 10 to
// 35 m deep (the sun's disc blurs the pattern, as agCausLod's) and with `fade` (the pixel's footprint over the cells).
float ouFieldCaustic(vec3 bed, float surfaceZ, vec3 sunDir, float fade) {
    float d = surfaceZ - bed.z;
    if (w.rippleInfo.y < 0.5 || d <= 0.0 || sunDir.z <= 0.0) return 1.0;
    vec3 rd = refract(-sunDir, vec3(0.0, 0.0, 1.0), 1.0 / 1.333);
    vec2 q = bed.xy - rd.xy * (d / max(-rd.z, 0.2));
    float j = 1.0 + d * (1.0 - 1.0 / 1.333) * ouFieldSample(q).w;
    return mix(1.0, 1.0 / max(abs(j), 0.25), fade * (1.0 - smoothstep(10.0 * kM, 35.0 * kM, d)));
}
// The slick the field's waves leave at p (0..1): a splash's or a stroke's turbulence damps the short wind chop within
// seconds (the smooth patch behind a boat or a swimmer, a whale's "footprint"), so their rings and wakes ride on calmer
// water. Read as the field's RMS slope over ~half a metre (eight taps 2.5 and 4 cells out; 1 at RMS 0.04, a few cm
// high rings) or its whitewater, with its fade.
float ouFieldSlick(vec2 p) {
    if (w.rippleInfo.y < 0.5) return 0.0;
    float inv = 1.0 / (w.ripple.z * w.rippleInfo.x);
    vec2 uv = (p - w.ripple.xy) * inv;
    vec2 in2 = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(in2.x, in2.y));
    if (fade == 0.0) return 0.0;
    float a = 2.5 * w.ripple.z * inv, b = 2.83 * w.ripple.z * inv, e = 0.0;
    const vec2 dirs[8] = vec2[8](vec2(1, 0), vec2(-1, 0), vec2(0, 1), vec2(0, -1), vec2(1, 1), vec2(-1, 1), vec2(1, -1), vec2(-1, -1));
    for (int i = 0; i < 8; ++i) {
        vec2 g = textureLod(uRipples, uv + dirs[i] * (i < 4 ? a : b), 0.0).gb;
        e += dot(g, g);
    }
    return fade * clamp(max(sqrt(e / 8.0) / 0.04, 1.5 * textureLod(uRipples, uv, 0.0).a), 0.0, 1.0);
}

// The base surface of one water mesh at world p (water.vert's displacement): `waves` = its tru.exe sea waves (columns
// c60-c63, zero = flat), `sea` = an open sea (the FFT ocean, or without it afl_ext's waves over Godot's height layers,
// amplitude w.detail.z, which still water has alone), t = time (s). Centred on the TRU surface but never above the rest plane (the original's own crest bound:
// decks and shores stay above the drawn sea).
float ouBaseHeight(vec2 p, mat4 waves, bool sea, float t) {
    float h = ouTruWaveHeight(p, waves);
    if (sea && ouOceanOn()) return min(h + ouOceanHeight(p) - 0.5 * kM * w.ocean[0].w, 0.0);
    h += w.detail.z * (0.5 * (ouFbm(p / kLambdaA + t * vec2(0.10, 0.02), 0.0).x +
                              ouFbm(kWaterRot * p / kLambdaB + t * vec2(-0.04, 0.12), 0.0).x) - 0.5);
    if (sea) h += kSeaAmp * (ouSea(p, t, 12) - 0.47);
    return min(h, 0.0);
}
// Its slope (the crest bound's flat tops ignored).
vec2 ouBaseSlope(vec2 p, mat4 waves, bool sea, float t) {
    vec2 s = ouTruWaveSlope(p, waves);
    if (sea && ouOceanOn()) return s + ouOceanSlope(0, p) + ouOceanSlope(1, p);
    vec3 la = ouLayerA(p, t, 0.0), lb = ouLayerB(p, t, 0.0);
    s += w.detail.z / kNormalAmp * 0.5 * (la.yz + lb.yz);
    if (sea) {
        float e = 0.01 * kSeaUnit, h = ouSea(p, t, 12);
        s += kSeaAmp * vec2(ouSea(p + vec2(e, 0.0), t, 12) - h, ouSea(p + vec2(0.0, e), t, 12) - h) / e;
    }
    return s;
}

// The surface at world p under the camera's sea (w.seaWaves; zero = still water, which then gets Godot's layers):
// height offset from the rest surface and its slope, every wave source summed.
bool ouSurfaceSea() { return any(notEqual(w.seaWaves[3], vec4(0.0))); }
float ouSurfaceHeight(vec2 p) { return ouBaseHeight(p, w.seaWaves, ouSurfaceSea(), w.eye.w) + ouFieldHeight(p); }
vec2 ouSurfaceSlope(vec2 p) { return ouBaseSlope(p, w.seaWaves, ouSurfaceSea(), w.eye.w) + ouFieldSample(p).xy; }
#endif
