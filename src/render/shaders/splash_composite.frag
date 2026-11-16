#version 460
// Splash fluid, pass 3 of 3: shades the blurred splat target over the scene colour as liquid. Normals come from the
// reconstructed surface (edge-aware differences); the scene behind is refracted (the colour copy refreshed after the
// water surfaces, so a splash shows the water and shore behind it bent, not the bed under the surface) and absorbed
// through the thickness, Fresnel reflects the sky / sun as water.frag does, and spray (isolated particles: foam) scatters
// white. Output is premultiplied: blend ONE, ONE_MINUS_SRC_ALPHA (gamma-space scene, as water.frag); the refraction
// replaces what is behind, so a fully covered pixel is opaque. Adapted from talvinckb/OpenGL-Water-Simulation
// composite.frag (MIT, Copyright (c) 2026 Talvin Ackbaraly).
#include "shaders/fluid.glsl"
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
} w;
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D uSplash;
layout(set = 0, binding = 3) uniform sampler2D uScene; // the scene colour with the water surfaces in it
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_UV_DY(ndcDy) (-0.5 * (ndcDy))
#else
layout(binding = 10) uniform sampler2D uSplash;
layout(binding = 5) uniform sampler2D uScene;
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_UV_DY(ndcDy) (0.5 * (ndcDy))
#endif
layout(location = 0) out vec4 oColor;
const float kEmpty = -1e19;
// Clear water absorbs red first (Pope & Fry 1997: ~0.45 / 0.06 / 0.02 per m at 650 / 550 / 450 nm), per unit (290 / m).
const vec3 kAbsorb = vec3(0.45, 0.06, 0.02) / 290.0;
const float kIor = 1.0 / 1.33;
const float kRefract = 2.0; // screen offset of the refraction per unit of thickness along the normal, in pixels
const float kMirror = 150.0; // how far along the reflected ray the screen-space reflection looks, units (~0.5 m)

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toGamma(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
// World point `dist` along the eye ray through pixel q (the r channel holds -dist).
vec3 surface(ivec2 q, float dist) {
    vec2 uv = (vec2(q) + 0.5) * w.screen.zw;
    vec4 f = w.invViewProj * vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), 1.0, 1.0);
    return w.eye.xyz + normalize(f.xyz / f.w - w.eye.xyz) * dist;
}
// One-sided difference toward the neighbour on the same surface (the closer depth); `a` / `b` = before / after.
vec3 slope(ivec2 p, ivec2 step, vec3 pc, float d) {
    ivec2 hi = textureSize(uSplash, 0) - 1, pa = clamp(p - step, ivec2(0), hi), pb = clamp(p + step, ivec2(0), hi);
    float da = -texelFetch(uSplash, pa, 0).r, db = -texelFetch(uSplash, pb, 0).r;
    bool ha = pa != p && da < -kEmpty, hb = pb != p && db < -kEmpty;
    if (!ha && !hb) return vec3(0.0);
    if (ha && (!hb || abs(da - d) < abs(db - d))) return pc - surface(pa, da);
    return surface(pb, db) - pc;
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 s = texelFetch(uSplash, p, 0);
    if (s.r <= kEmpty || s.g <= 0.05) discard;
    // foam: the whiteness (fluid.glsl ouFluidFoam) relative to a lone spray sphere's thickness (1.0 kOuFluidRadius at its
    // centre, as splash.vert draws it): spray stays white, a sheet's 0.3 thins out over the thicker liquid behind it
    float dist = -s.r, thick = s.g, foam = clamp(s.b * kOuFluidRadius / max(s.g, 1e-3), 0.0, 1.0);
    vec3 pc = surface(p, dist), v = normalize(w.eye.xyz - pc);
    // Camera submerged (fogColor.w, Enhanced): the splash flies above the surface, seen only through Snell's window, with
    // the limits fx::System::spritesBelowZ and water.frag's restore of the scene above use: the view ray within 48.6 deg of
    // vertical and 60 m (17400 units), faded over 45..48.6 deg and 50..60 m. Outside it, it printed over the black total
    // internal reflection (water.frag) as a flat pale disc around Lara.
    float vis = 1.0;
    if (w.fogColor.w > 0.5 && pc.z > w.eye.z) {
        vec3 d = pc - w.eye.xyz;
        float len = length(d);
        vis = clamp((d.z / len - 0.6613) / (0.7071 - 0.6613), 0.0, 1.0) * clamp((17400.0 - len) / 2900.0, 0.0, 1.0);
        if (vis <= 0.0) discard;
    }
    vec3 dx = slope(p, ivec2(1, 0), pc, dist), dy = slope(p, ivec2(0, 1), pc, dist);
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 ? normalize(n) : v; // a lone pixel faces the camera
    if (dot(n, v) < 0.0) n = -n;

    // Refraction: the scene behind, displaced along the refracted ray's screen direction by the thickness crossed.
    vec3 t = refract(-v, n, kIor);
    vec4 c0 = w.viewProj * vec4(pc, 1.0), c1 = w.viewProj * vec4(pc + t, 1.0);
    vec2 sd = c1.xy / c1.w - c0.xy / c0.w;
    vec2 dir = dot(sd, sd) > 1e-12 ? normalize(vec2(sd.x, OU_UV_DY(sd.y) * 2.0)) : vec2(0.0);
    vec2 uv = gl_FragCoord.xy * w.screen.zw + dir * min(thick * kRefract, 60.0) * w.screen.zw * (1.0 - dot(n, v));
    vec3 behind = toLinear(texture(uScene, clamp(uv, vec2(0.0), vec2(1.0))).rgb) * exp(-kAbsorb * thick);

    float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 r = reflect(-v, n), rs = normalize(vec3(r.xy, abs(r.z)));
    vec3 sky = toLinear(w.fogColor.rgb * (0.6 + 0.6 * pow(1.0 - rs.z, 3.0)) +
                        w.sunColor.rgb * (0.12 * pow(max(dot(rs, w.sunDir.xyz), 0.0), 12.0)));
    // What the drop mirrors: the scene a short way along the reflected ray where that lands on screen (the walls, the
    // water and the shore around the splash), else the sky / fog term. A cave's fog is near black, which on its own
    // drew every grazing facet as a dark crack.
    vec4 cr = w.viewProj * vec4(pc + r * kMirror, 1.0);
    if (cr.w > 1.0) {
        vec2 ruv = vec2(0.5 + 0.5 * cr.x / cr.w, 0.5 + OU_UV_DY(cr.y / cr.w));
        vec2 edge = min(ruv, 1.0 - ruv);
        float on = smoothstep(0.0, 0.05, min(edge.x, edge.y));
        sky = mix(sky, toLinear(texture(uScene, clamp(ruv, vec2(0.0), vec2(1.0))).rgb), on);
    }
    float spec = pow(max(dot(n, normalize(w.sunDir.xyz + v)), 0.0), 200.0) * w.sunDir.w * 4.0;
    // Spray / aerated water scatters most light back: white lit by the scene (the old droplet disc's look).
    vec3 lit = min(w.ambient.rgb + w.sunColor.rgb * max(w.sunDir.z, 0.0), vec3(1.0));
    vec3 white = toLinear(vec3(0.85, 0.93, 0.98) * max(0.55 + 0.45 * max(w.sunDir.z, 0.0), dot(lit, vec3(1.0 / 3.0))));

    vec3 liquid = behind * (1.0 - fresnel) + sky * fresnel + spec * toLinear(min(w.sunColor.rgb, vec3(1.0)));
    // A drop's silhouette catches the light around it (total internal reflection and grazing Fresnel): a bright rim
    // around a clear middle is what reads as a water drop rather than a grey disc. Splash water carries some air: a
    // faint white body, more where it is spray.
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0);
    liquid += white * (0.45 * rim);
    vec3 lin = mix(liquid, white, 0.04 + 0.16 * foam);
    // Coverage from the thickness relative to one particle's (2 kOuFluidRadius): the blur's grown fringe (a tapered
    // copy of its neighbour's depth) fades out instead of drawing a hard square.
    float a = smoothstep(0.05, 0.4, thick / (2.0 * kOuFluidRadius)) * vis;
    if (a < 0.004) discard;
    vec3 c = toGamma(lin);
    float fogT = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
    oColor = vec4(mix(c, w.fogColor.rgb, fogT) * a, a);
}
