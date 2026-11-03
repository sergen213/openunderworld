#version 460
// Splash droplets, pass 3 of 3: shades the blurred splat target over the scene colour as liquid. Normals come from the
// reconstructed surface (edge-aware differences), Fresnel reflects the sky / sun as water.frag does, and the thickness
// gives the transmittance. Output is premultiplied: blend ONE, ONE_MINUS_SRC_ALPHA (gamma-space scene, as water.frag).
// No screen refraction: it would read the pre-water copy and print the bed through the water surface behind a splash.
// Adapted from talvinckb/OpenGL-Water-Simulation composite.frag (MIT, Copyright (c) 2026 Talvin Ackbaraly).
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
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#else
layout(binding = 10) uniform sampler2D uSplash;
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#endif
layout(location = 0) out vec4 oColor;
const float kEmpty = -1e19;
const float kSigma = 0.15; // extinction per unit of thickness (aerated: a 16-unit-thick droplet lets ~9 % through)

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
    vec2 s = texelFetch(uSplash, p, 0).rg;
    if (s.r <= kEmpty || s.g <= 0.05) discard;
    float dist = -s.r;
    vec3 pc = surface(p, dist), v = normalize(w.eye.xyz - pc);
    vec3 dx = slope(p, ivec2(1, 0), pc, dist), dy = slope(p, ivec2(0, 1), pc, dist);
    vec3 n = cross(dx, dy);
    n = dot(n, n) > 1e-12 ? normalize(n) : v; // a lone pixel faces the camera
    if (dot(n, v) < 0.0) n = -n;

    vec3 light = toLinear(min(w.ambient.rgb + w.sunColor.rgb * max(w.sunDir.z, 0.0), vec3(1.0)));
    float fresnel = 0.02 + 0.98 * pow(1.0 - max(dot(n, v), 0.0), 5.0);
    vec3 r = reflect(-v, n), rs = normalize(vec3(r.xy, abs(r.z)));
    vec3 sky = toLinear(w.fogColor.rgb * (0.6 + 0.6 * pow(1.0 - rs.z, 3.0)) +
                        w.sunColor.rgb * (0.12 * pow(max(dot(rs, w.sunDir.xyz), 0.0), 12.0)));
    float spec = pow(max(dot(n, normalize(w.sunDir.xyz + v)), 0.0), 200.0) * w.sunDir.w * 4.0;
    // Aerated white water scatters most light back: the old disc's white (floor 0.55 without sun), lit by the scene.
    vec3 lit = min(w.ambient.rgb + w.sunColor.rgb * max(w.sunDir.z, 0.0), vec3(1.0));
    vec3 body = toLinear(vec3(0.82, 0.92, 0.97) * max(0.55 + 0.45 * max(w.sunDir.z, 0.0), dot(lit, vec3(1.0 / 3.0))));

    float t = exp(-s.g * kSigma), edge = smoothstep(0.05, 2.0, s.g);
    float a = (1.0 - t * (1.0 - fresnel)) * edge;
    if (a < 0.004) discard;
    vec3 lin = body * (1.0 - t) * (1.0 - fresnel) + sky * fresnel + spec * toLinear(min(w.sunColor.rgb, vec3(1.0)));
    vec3 c = toGamma(lin * edge / a); // straight colour of the covered part, then premultiplied below
    float fogT = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
    oColor = vec4(mix(c, w.fogColor.rgb, fogT) * a, a);
}
