// Underwater post: tru.exe's full-screen water pass and light rays + Enhanced caustics and sun shafts on the submerged scene.
// Included by the post shaders after their own declarations; returns the input colour unchanged
// when the camera is not submerged, so Original output stays byte-identical.
#ifdef VULKAN
layout(set = 1, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; // original screen filters (shaders/filter.glsl)
    vec4 waterPass[3]; // tru.exe's full-screen under-water pass (ouWaterPass); [2].y light-ray weight (ouRays)
    uvec4 rays[157];   // tru.exe's under-water light rays, 50 x 50 bytes (ouRays)
    mat4 seaWaves;     // tru.exe's sea waves at the camera (caustics: shaders/waves.glsl)
    vec4 optics[3];    // Enhanced water colour (render::waterOptics; water.frag ouWaterLook): all 0 = the unit's own
    vec4 ocean[2];     // the FFT ocean (caustics: shaders/waves.glsl ouOcean*)
} w;
#ifdef VULKAN
layout(set = 1, binding = 1) uniform sampler2D uDepth;
layout(set = 1, binding = 3) uniform sampler2D uRipples; // the interactive wave field (caustics)
layout(set = 1, binding = 5) uniform sampler2DArray uOcean; // the FFT ocean (caustics; shaders/ocean.comp)
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 9) uniform sampler2D uRipples;
layout(binding = 12) uniform sampler2DArray uOcean;
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#endif
vec3 ouWorld(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clip = vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    vec4 p = invViewProj * clip;
    return p.xyz / p.w;
}
#include "shaders/caustics.glsl"
// Caustics on the opaque scene point p (shaders/caustics.glsl), camera under water only: from above, this pass sees the
// water surface's composite (water.frag puts them on the refracted bed there). Only inside a water volume below its
// surface; ouSunShadowVisibility is in atmosphere.glsl (after render/sun.glsl in both renderers).
vec3 ouCaustics(vec3 c, vec2 uv) {
    float d = texture(uDepth, uv).r;
    vec3 p = ouWorld(uv, d, w.invViewProj);
    // derivatives before any branch: the surface normal and the cell footprint
    vec3 n = cross(dFdx(p), dFdy(p));
    float footprint = length(fwidth(p.xy)); // world units per pixel
    if (w.fogColor.w < 0.5 || d >= 0.999999) return c;
    float surf = -1e30;
    for (int i = 0; i < int(w.detail.w); ++i) {
        vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi))) { surf = hi.z; break; }
    }
    return c * ouCausticLight(p, n, surf - p.z, footprint);
}
// tru.exe's under-water pass (0x6D8C20 drawing the unit's water material, PS lib 410 slot 0; RENDERING.md "Under
// water"), both water qualities: the colour runs from the deep colour (looking down) to the surface colour (looking
// up), its alpha from the eye's value to 1 at the material's distance (view depth). The PS's noise texture wobble
// (+-0.05 of the gradient, 0.004 of the colour) and the scripted murk (c134.y) are left out.
// Enhanced water colour preset (w.optics[0].w, only set with Enhanced water): the colour takes the preset's in-scatter
// hue at the unit colour's luminance (as water.frag ouWaterLook), the alpha its per-channel extinction from the eye's
// value (red fogs first), so the view under water matches the surface seen from above.
vec3 ouPresetHue(vec3 gammaCol) { // gamma colour -> the preset hue at its luminance, gamma
    float lum = dot(pow(max(gammaCol, vec3(0.0)), vec3(2.2)), vec3(0.2126, 0.7152, 0.0722));
    return pow(max(w.optics[1].rgb * lum, vec3(0.0)), vec3(1.0 / 2.2));
}
// Enhanced under-water fog (ours, camera submerged: w.fogColor.w; RENDERING.md "Under water"): the shared medium
// (shaders/uw_medium.glsl) per pixel. Per-channel extinction sigma along the view ray; the in-scatter of light that itself
// falls off as e^(-kd depth) below the surface, integrated in closed form along the ray (brighter looking up, darker and
// bluer looking down and deeper down), with the forward sun glow; the opaque scene's light loses red and brightness
// with its depth under the surface and takes the hemispherical water-coloured ambient. Sets gUwT / gUwL (the pixel's
// transmittance and in-scatter) for the grade's glow (filter.glsl).
#include "shaders/uw_medium.glsl"
vec3 gUwT = vec3(1.0), gUwL = vec3(0.0);
vec3 ouUwFog(vec3 c, vec2 uv) {
    float depth = texture(uDepth, uv).r;
    vec3 q = ouWorld(uv, depth, w.invViewProj);
    vec3 n = cross(dFdx(q), dFdy(q)); // before any branch
    bool sky = depth >= 0.999999;
    if (sky) q = ouWorld(uv, 1.0, w.invViewProj);
    vec3 rd = q - w.eye.xyz;
    float len = length(rd);
    rd /= max(len, 1e-3);
    if (sky) len = 1e7;
    float surf = ouUwSurfaceZ();
    // What lies above the water is seen through the surface (water.frag's Snell's window and total internal reflection,
    // drawn over the sky too): fog only up to it, and keep that colour; only rays that miss every surface (looking down
    // into nothing) are pure in-scatter.
    bool through = rd.z > 1e-4 && w.eye.z < surf;
    if (through) len = min(len, (surf - w.eye.z) / rd.z);
    float d = min(len, 1e7) / kM;
    vec3 l = ouUwSunDir();
    OuUwMedium m = ouUwEyeMedium();
    vec3 lin = pow(max(c, vec3(0.0)), vec3(2.2));
    vec3 lit = lin;
    if (!sky) { // the scene's light under its own water surface
        float ps = -1e30;
        for (int i = 0; i < int(w.detail.w); ++i) {
            vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
            if (all(greaterThanEqual(q, lo)) && all(lessThan(q, hi))) { ps = hi.z; break; }
        }
        n = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);
        if (dot(n, w.eye.xyz - q) < 0.0) n = -n;
        if (ps > q.z) lit *= ouUwSurfaceLight(m, n, (ps - q.z) / kM, l);
    }
    gUwT = exp(-m.sigma * d);
    gUwL = ouUwFogIn(m, rd, d, ouUwEyeDepth(), l);
    vec3 fogged = (sky && !through ? vec3(0.0) : lit * gUwT) + gUwL;
    gUwT = mix(vec3(1.0), gUwT, w.waterPass[2].x), gUwL *= w.waterPass[2].x;
    return pow(max(mix(lin, fogged, w.waterPass[2].x), vec3(0.0)), vec3(1.0 / 2.2));
}
vec3 ouWaterPass(vec3 c, vec2 uv) {
    if (w.waterPass[2].x <= 0.0) return c;
    if (w.fogColor.w > 0.5) return ouUwFog(c, uv); // Enhanced, submerged
    vec3 far = ouWorld(uv, 1.0, w.invViewProj);
    float up = clamp(0.5 + 0.5 * normalize(far - w.eye.xyz).z, 0.0, 1.0);
    vec3 col = mix(w.waterPass[1].rgb, w.waterPass[0].rgb, up);
    vec3 q = ouWorld(uv, texture(uDepth, uv).r, w.invViewProj);
    float viewZ = (w.viewProj * vec4(q, 1.0)).w;
    if (w.optics[0].w > 0.5) {
        vec3 a = 1.0 - (1.0 - w.waterPass[0].w) * exp(-w.optics[0].rgb * min(viewZ, 1e5));
        return mix(c, ouPresetHue(col), clamp(a * w.waterPass[2].x, 0.0, 1.0));
    }
    float a = mix(w.waterPass[0].w, 1.0, clamp(viewZ / w.waterPass[1].w, 0.0, 1.0)) * w.waterPass[2].x;
    return mix(c, col, clamp(a, 0.0, 1.0));
}
// tru.exe's under-water light rays (render::UnderwaterRays, both water qualities): the 50 x 50 additive image of the
// record's light columns, which the +0x6C material (PS lib 405) adds over the screen, bilinear, x the weight.
float ouRayTexel(ivec2 p) {
    p = clamp(p, ivec2(0), ivec2(49));
    uint i = uint(p.y * 50 + p.x);
    return float((w.rays[i >> 4][(i >> 2) & 3u] >> (8u * (i & 3u))) & 255u) / 255.0;
}
vec3 ouRays(vec3 c, vec2 uv) {
    if (w.waterPass[2].y <= 0.0) return c;
    vec2 t = vec2(uv.x, 0.5 - 0.5 * OU_NDC_Y(uv)) * 50.0 - 0.5; // texels, rows down
    ivec2 i = ivec2(floor(t));
    vec2 f = t - vec2(i);
    float r = mix(mix(ouRayTexel(i), ouRayTexel(i + ivec2(1, 0)), f.x), mix(ouRayTexel(i + ivec2(0, 1)), ouRayTexel(i + ivec2(1, 1)), f.x), f.y);
    return c + r * w.waterPass[2].y;
}
// Enhanced (ours, RENDERING.md "Under water"): volumetric sun shafts, the pipeline of Noniv's abyssal-garden (MIT;
// docs/RESEARCH.md; its shaders.js W_RAYS, W_RBLUR and W_POST's upsample) on TRU's water. Three passes before the post
// pass, only while ouShaftsOn():
// 1. ouShaftPatternTexel: the focusing of the drawn waves (caustics.glsl ouCausticFocus, as the caustics on the bed)
//    baked once per frame into a kShaftTile^2 tile of the surface around the camera's sun entry point (kShaftTexel
//    units a texel, snapped to texels so it does not swim), times slow wave groups (ouShaftGroups): the light that
//    enters each spot of the surface.
// 2. ouShaftMarch (half resolution): the view ray from the eye to the scene, the nearest surface or kShaftRange (and
//    at most 5 green extinction lengths) in `steps` even steps jittered per pixel by interleaved gradient noise; each
//    step traces back along the refracted sun to its surface entry point, takes the tile there, the sun shadow map
//    (one tap: the occluders' shadow volumes) and the extinction e^(-sigma (sun path under water + view distance)) per
//    channel; HG phase toward the sun. Out: rgb + linear view depth of the scene there.
// 3. ouShaftBlur: depth-aware 3x3 blur at half resolution (hides the jitter).
// The post pass adds them (ouShaftsComposite: depth-aware bilinear upsample, then soft-clamped under the water colour).
#ifdef VULKAN
layout(set = 1, binding = 6) uniform sampler2D uShaftsRaw;    // pass 2's output (the blur's input)
layout(set = 1, binding = 7) uniform sampler2D uShafts;       // pass 3's output (the post composite)
layout(set = 1, binding = 8) uniform sampler2D uShaftPattern; // pass 1's tile (the march)
#else
layout(binding = 13) uniform sampler2D uShaftsRaw;
layout(binding = 14) uniform sampler2D uShafts;
layout(binding = 15) uniform sampler2D uShaftPattern;
#endif
float ouPhase(float cosT, float g);
float ouSunTapVisibility(vec3 p);
const float kShaftTexel = 48.0;       // [I] tile texel (world units): streaks ~0.5..2 m across
const int kShaftTile = 512;           // tile texels per side (render::kUnderwaterShaftTile): 24576 units
const float kShaftRange = 12000.0;    // [I] longest march (abyssal-garden's 120 m)
const float kShaftFocusDepth = 2000.0; // [I] depth whose focal length the tile's focusing takes
const float kShaftBase = 0.12;        // [I] in-scatter of the unfocused light (the soft glow; abyssal-garden 0.012)
const float kShaftStreak = 1.6;       // [I] in-scatter added by the focused light (the streaks)
const float kShaftScatter = 1.0;      // [I] scattering coefficient / green extinction
const float kShaftCap = 0.5;          // [I] soft clamp under this x the water colour: never a saturated blob
// Shafts this frame: Enhanced water (fogColor.w), the camera below the nearest surface, a sun above the horizon.
// Mirrors render::underwaterShaftsActive (Water.cpp), which records the passes.
bool ouShaftsOn() {
    return w.fogColor.w > 0.5 && w.sunColor.w > 0.0 && w.sunDir.z >= 0.05 && w.detail.w > 0.5 && w.eye.z < w.volumes[1].z;
}
vec3 ouShaftSun() { return ouUwSunDir(); } // toward the sun, under water (ouShaftsOn: there is one)
// The tile's world-space corner (both the pattern pass and the march compute it the same way from the block).
vec2 ouShaftTileOrigin(vec3 l) {
    vec2 c = w.eye.xy + l.xy * ((w.volumes[1].z - w.eye.z) / l.z);
    return floor(c / kShaftTexel) * kShaftTexel - 0.5 * float(kShaftTile) * kShaftTexel;
}
// Wave groups: the swell focuses in patches ~10 m across (3000 units) that drift and come and go: streaks 0..1.
float ouShaftGroups(vec2 e, float t) {
    float m = sin(dot(e, vec2(0.0021, 0.0009)) + 0.23 * t) * sin(dot(e, vec2(-0.0008, 0.0019)) - 0.17 * t);
    m += 0.5 * sin(dot(e, vec2(0.0013, -0.0027)) + 0.31 * t);
    return smoothstep(-0.6, 0.9, m);
}
// Pass 1, tile texel `px`: the focused light entering the surface there, 0..1.
float ouShaftPatternTexel(ivec2 px) {
    vec2 e = ouShaftTileOrigin(ouShaftSun()) + (vec2(px) + 0.5) * kShaftTexel;
    float g = ouShaftGroups(e, w.eye.w);
    if (g <= 0.0) return 0.0;
    float f = ouCausticFocus(e, kShaftFocusDepth, kShaftTexel, 1).g;
    return smoothstep(1.0, 1.9, f) * g;
}
float ouIgn(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); } // Jimenez 2014
// Linear view depth of the opaque scene at full-resolution pixel `fp` (the bilateral weights' key), at most 1e6.
float ouViewDepth(ivec2 fp) {
    vec2 uv = (vec2(fp) + 0.5) / vec2(textureSize(uDepth, 0));
    vec3 q = ouWorld(uv, texelFetch(uDepth, fp, 0).r, w.invViewProj);
    return min((w.viewProj * vec4(q, 1.0)).w, 1e6);
}
// Pass 2, half-resolution pixel `hp` (its full-resolution pixel 2 hp): in-scattered sun light (rgb) + view depth.
vec4 ouShaftMarch(ivec2 hp, int steps) {
    ivec2 fp = min(hp * 2, textureSize(uDepth, 0) - 1);
    vec2 uv = (vec2(fp) + 0.5) / vec2(textureSize(uDepth, 0));
    vec3 q = ouWorld(uv, texelFetch(uDepth, fp, 0).r, w.invViewProj);
    float viewZ = min((w.viewProj * vec4(q, 1.0)).w, 1e6);
    vec3 dir = q - w.eye.xyz;
    float len = length(dir);
    dir /= max(len, 1e-3);
    float surf = w.volumes[1].z;
    if (dir.z > 1e-4) len = min(len, (surf - w.eye.z) / dir.z);
    OuUwMedium m = ouUwEyeMedium(); // the fog's medium (shaders/uw_medium.glsl): red goes first, blue-green shafts
    len = max(min(len, min(kShaftRange, 5.0 * kM / max(m.sigma.g, 1e-7))), 0.0);
    vec3 l = ouShaftSun();
    vec2 org = ouShaftTileOrigin(l);
    float span = kShaftTexel * float(kShaftTile);
    float dt = len / float(max(steps, 1)), j = ouIgn(vec2(hp) + 0.5);
    vec3 acc = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float t = (float(i) + j) * dt;
        vec3 p = w.eye.xyz + dir * t;
        float sunPath = max(surf - p.z, 0.0) / l.z;
        vec2 tc = (p.xy + l.xy * sunPath - org) / span;
        vec2 edge = min(tc, 1.0 - tc);
        float pat = textureLod(uShaftPattern, tc, 0.0).r * smoothstep(0.0, 0.06, min(edge.x, edge.y));
        // the sun's light left at p's depth (ouUwSunLight, as the fog lights the scene) x the view path's extinction
        acc += ouUwSunLight(m, max(surf - p.z, 0.0) / kM, l) * exp(-m.sigma * (t / kM)) *
               (ouSunTapVisibility(p) * (kShaftBase + kShaftStreak * pat));
    }
    float ph = mix(1.0, 12.566371 * ouPhase(dot(dir, l), 0.75), 0.8); // x 4 pi: isotropic = 1
    return vec4(w.sunColor.rgb * w.sunColor.w * acc * (dt / kM * kShaftScatter * m.sigma.g * ph), viewZ);
}
// Pass 3: depth-aware 3x3 blur of pass 2 (abyssal-garden W_RBLUR's weights 1 / (0.03 + relative depth difference)).
vec4 ouShaftBlur(ivec2 p) {
    ivec2 mx = textureSize(uShaftsRaw, 0) - 1;
    vec4 c = texelFetch(uShaftsRaw, p, 0);
    vec3 acc = vec3(0.0);
    float ws = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) {
            vec4 r = texelFetch(uShaftsRaw, clamp(p + ivec2(x, y), ivec2(0), mx), 0);
            float k = 1.0 / (0.03 + abs(r.a - c.a) / max(c.a, 1.0));
            acc += r.rgb * k, ws += k;
        }
    return vec4(acc / ws, c.a);
}
// Post pass: the blurred shafts upsampled to this pixel (bilinear weights x depth similarity, W_POST), soft-clamped
// cap (1 - e^(-s / cap)) under kShaftCap x the water colour, added.
vec3 ouShaftsComposite(vec3 c) {
    if (!ouShaftsOn()) return c;
    ivec2 px = ivec2(gl_FragCoord.xy);
    float zf = ouViewDepth(min(px, textureSize(uDepth, 0) - 1));
    vec2 h = (gl_FragCoord.xy - 0.5) * 0.5; // half-resolution texel k holds full pixel 2k
    vec2 b = floor(h), f = h - b;
    ivec2 bi = ivec2(b), mx = textureSize(uShafts, 0) - 1;
    vec3 acc = vec3(0.0);
    float ws = 0.0;
    for (int k = 0; k < 4; ++k) {
        ivec2 o = ivec2(k & 1, k >> 1);
        vec4 r = texelFetch(uShafts, clamp(bi + o, ivec2(0), mx), 0);
        float bw = (o.x == 1 ? f.x : 1.0 - f.x) * (o.y == 1 ? f.y : 1.0 - f.y);
        float wk = (bw + 0.02) / (0.02 + abs(r.a - zf) / max(zf, 1.0));
        acc += r.rgb * wk, ws += wk;
    }
    vec3 cap = kShaftCap * pow(ouUwEyeMedium().scatter, vec3(1.0 / 2.2)); // the fog's in-scatter colour, gamma
    return c + cap * (1.0 - exp(-max(acc / ws, vec3(0.0)) / cap));
}
vec3 ouUnderwater(vec3 c, vec2 uv) {
    // caustics light the scene, the water pass fogs it, then the shafts' and tru.exe's rays add light (0x6D8C20 order)
    return ouRays(ouShaftsComposite(ouWaterPass(ouCaustics(c, uv), uv)), uv);
}
