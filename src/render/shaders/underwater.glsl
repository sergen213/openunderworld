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
} w;
#ifdef VULKAN
layout(set = 1, binding = 1) uniform sampler2D uDepth;
layout(set = 1, binding = 3) uniform sampler2D uRipples; // the interactive wave field (caustics)
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 9) uniform sampler2D uRipples;
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
vec3 ouWaterPass(vec3 c, vec2 uv) {
    if (w.waterPass[2].x <= 0.0) return c;
    vec3 far = ouWorld(uv, 1.0, w.invViewProj);
    float up = clamp(0.5 + 0.5 * normalize(far - w.eye.xyz).z, 0.0, 1.0);
    vec3 col = mix(w.waterPass[1].rgb, w.waterPass[0].rgb, up);
    vec3 q = ouWorld(uv, texture(uDepth, uv).r, w.invViewProj);
    // Enhanced (submerged flag): what lies above the water is seen through the surface (water.frag's Snell's
    // window), so the water fogs the path only up to the nearest volume's surface.
    float surf = w.volumes[1].z;
    if (w.fogColor.w > 0.5 && w.detail.w > 0.5 && w.eye.z < surf && q.z > surf)
        q = w.eye.xyz + (q - w.eye.xyz) * ((surf - w.eye.z) / (q.z - w.eye.z));
    float viewZ = (w.viewProj * vec4(q, 1.0)).w;
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
// Enhanced (ours, RENDERING.md "Under water"): sun shafts as the volumetric continuation of the caustics. Each step
// along the view ray traces back along the refracted sun to where it entered the surface and takes the focusing of
// the same waves there (caustics.glsl ouCausticFocus, as the bed's caustics: the streaks meet the bright lines on the
// floor and move with the waves), blurred by the sun disk (0.005 x depth, as the bed) and 2.5% of the step (one afl iteration): many thin parallel streaks under the
// focusing crests (smoothstep(1.15, 2.2, focus)), coming and going in slow wave groups (ouShaftGroups). In-scatter:
// HG 0.7 (strongest toward the sun), 3x3-PCF sun shadow (soft occluder volumes, not texel blocks), attenuated per
// channel by the unit's extinction (water.frag ouWaterLook: red goes first) over the sun's path below the surface and
// the view path (blue-green, fading with depth), then soft-clamped under 0.3 x the water colour so looking into the
// sun never saturates. 4 steps over up to 4000 units packed toward the eye (t ~ u^2), steps between groups or in
// shadow skipping the focus (~1.5 ms at 2560 x 1440); the four pixels of each 2x2 quad march at interleaved quarter offsets and share their mean (ouQuadMean): 16 depth slices
// without bands and no screen-space dither left (a static per-pixel jitter was the black checker FSR kept).
float ouPhase(float cosT, float g);
// Wave groups: the swell focuses in patches ~10 m across (3000 units) that drift and come and go: streaks 0..1.
float ouShaftGroups(vec2 e, float t) {
    float m = sin(dot(e, vec2(0.0021, 0.0009)) + 0.23 * t) * sin(dot(e, vec2(-0.0008, 0.0019)) - 0.17 * t);
    m += 0.5 * sin(dot(e, vec2(0.0013, -0.0027)) + 0.31 * t);
    return smoothstep(-0.45, 0.9, m);
}
// Mean over the 2x2 pixel quad via fine derivatives (uniform control flow only; helper lanes take part).
vec3 ouQuadMean(vec3 v, ivec2 px) {
    vec3 dx = dFdxFine(v);
    vec3 row = v + (px.x == 0 ? dx : -dx) * 0.5;
    vec3 dy = dFdyFine(row);
    return row + (px.y == 0 ? dy : -dy) * 0.5;
}
vec3 ouShafts(vec3 c, vec2 uv) {
    if (w.fogColor.w < 0.5 || w.sunColor.w <= 0.0 || w.sunDir.z < 0.05 || w.detail.w < 0.5) return c;
    vec3 q = ouWorld(uv, texture(uDepth, uv).r, w.invViewProj);
    vec3 dir = q - w.eye.xyz;
    float len = length(dir);
    dir /= max(len, 1e-3);
    float surf = w.volumes[1].z;
    if (w.eye.z >= surf) return c;
    if (dir.z > 1e-4) len = min(len, (surf - w.eye.z) / dir.z);
    len = min(len, 4000.0);
    vec3 l = -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / 1.333); // toward the sun, under water
    bool unit = w.waterPass[1].w > 0.0; // ouWaterLook's extinction per unit length, linear colour
    vec3 sf = unit ? w.waterPass[0].rgb : vec3(0.447, 0.780, 0.792);
    vec3 sigma = (1.0 - 0.85 * pow(sf, vec3(2.2))) * (30.0 / (unit ? w.waterPass[1].w : 100000.0));
    const int steps = 4;
    ivec2 px = ivec2(gl_FragCoord.xy) & 1;
    float o = (float(px.x + 2 * px.y) + 0.5) * 0.25; // this pixel's quarter of each step (2x2 interleave)
    vec3 lit = vec3(0.0);
    for (int i = 0; i < steps; ++i) {
        float u = (float(i) + o) / float(steps), t = len * u * u, dt = len * 2.0 * u / float(steps);
        vec3 p = w.eye.xyz + dir * t;
        float h = max(surf - p.z, 1.0), sunPath = h / l.z;
        vec2 e = p.xy + l.xy * sunPath; // surface entry point
        float g = ouShaftGroups(e, w.eye.w), v = g > 0.0 ? ouSunShadowVisibility(p) : 0.0;
        if (v <= 0.01) continue; // between wave groups or in shadow: skip the focus (the costly part)
        float b = smoothstep(1.15, 2.2, ouCausticFocus(e, h, 0.005 * h + 0.025 * dt, 1)) * g;
        lit += exp(-sigma * (sunPath + t)) * (b * v * dt);
    }
    lit = ouQuadMean(lit, px); // the quad's four interleaved marches: 64 steps' worth, no dither left
    vec3 s = w.sunColor.rgb * w.sunColor.w * lit * (4.0e-3 * 12.566371 * ouPhase(dot(dir, l), 0.7));
    vec3 cap = 0.3 * sf; // soft clamp under a third of the water's own colour: streaks, never a saturated blob
    return c + cap * (1.0 - exp(-s / cap));
}
vec3 ouUnderwater(vec3 c, vec2 uv) {
    // caustics light the scene, the water pass fogs it, then the shafts' and tru.exe's rays add light (0x6D8C20 order)
    return ouRays(ouShafts(ouWaterPass(ouCaustics(c, uv), uv), uv), uv);
}
