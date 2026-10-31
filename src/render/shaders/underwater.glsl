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
} w;
#ifdef VULKAN
layout(set = 1, binding = 1) uniform sampler2D uDepth;
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
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
// Enhanced (ours, RENDERING.md "Under water"): volumetric sun shafts. 12 jittered steps along the view ray (to the
// scene, the surface or 6000 units) in-scatter the sun where the sun shadow map sees it, through the caustic network
// at the point where the refracted sun ray entered the surface (900-unit cells, so the shafts follow the sun and
// flicker with the waves), fading below the surface and with the water fog toward the eye; tinted by the unit's
// surface water colour. ouSunTapVisibility / ouPhase are in atmosphere.glsl.
float ouSunTapVisibility(vec3 p);
float ouPhase(float cosT, float g);
vec3 ouShafts(vec3 c, vec2 uv) {
    if (w.fogColor.w < 0.5 || w.sunColor.w <= 0.0 || w.sunDir.z < 0.05 || w.detail.w < 0.5) return c;
    vec3 q = ouWorld(uv, texture(uDepth, uv).r, w.invViewProj);
    vec3 dir = q - w.eye.xyz;
    float len = length(dir);
    dir /= max(len, 1e-3);
    float surf = w.volumes[1].z;
    if (w.eye.z >= surf) return c;
    if (dir.z > 1e-4) len = min(len, (surf - w.eye.z) / dir.z);
    len = min(len, 6000.0);
    vec3 l = -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / 1.333); // toward the sun, under water
    const int steps = 12;
    float dt = len / float(steps);
    float t = dt * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float fog = w.waterPass[1].w > 0.0 ? 3.0 / w.waterPass[1].w : 1.0 / max(w.fogRange.y, 1.0);
    float lit = 0.0;
    for (int i = 0; i < steps; ++i, t += dt) {
        vec3 p = w.eye.xyz + dir * t;
        float h = surf - p.z;
        vec2 s = (p.xy + l.xy * (h / l.z)) / 600.0;
        float k = ouCausticLayer(s, w.eye.w * 0.6, 0.35);
        lit += ouSunTapVisibility(p) * k * k * exp(-h / 4000.0 - t * fog);
    }
    vec3 tint = w.waterPass[1].w > 0.0 ? w.waterPass[0].rgb : vec3(0.3, 0.55, 0.65);
    float air = (1.0 - exp(-len / 2500.0)) / float(steps); // more water in front, more scattering
    return c + w.sunColor.rgb * tint * (lit * air * ouPhase(dot(dir, l), 0.6) * 12.566371 * w.sunColor.w * 2.5);
}
vec3 ouUnderwater(vec3 c, vec2 uv) {
    // caustics light the scene, the water pass fogs it, then the shafts' and tru.exe's rays add light (0x6D8C20 order)
    return ouRays(ouShafts(ouWaterPass(ouCaustics(c, uv), uv), uv), uv);
}
