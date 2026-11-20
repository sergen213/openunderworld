// Enhanced under-water medium (ours, camera submerged; RENDERING.md "Under water"). The `ag` section at the end is the
// literal abyssal-garden port every under-water effect uses: the post pass's light, fog and sun shafts (underwater.glsl),
// the particles (uw_light.glsl, uw_snow / uw_bubble) and water.frag (the bed's relight, the surface seen from below).
// Needs the WaterBlock `w` and kM (shaders/waves.glsl) declared before it.
#ifndef OU_UW_MEDIUM_GLSL
#define OU_UW_MEDIUM_GLSL
// Surface z of the nearest water volume (the eye's own height without one).
float ouUwSurfaceZ() { return w.detail.w > 0.5 ? w.volumes[1].z : w.eye.z; }

// ---- 1:1 port of abyssal-garden's medium (Noniv, MIT; source/src/shaders.js S_G / S_B, docs/RESEARCH.md 8.11) ----
// Literal constants and formulas, in the demo's own space ("ag space"): metres, y up, the water surface at y = 0 (points
// under water have y < 0), x / z horizontal. agPos maps a world point (TRU units, z up) into it; agDir a direction.
// The demo's zone z (0 = its open blue water, 1 = its purple grotto) is 0 here (kAgZone). Its density volume (vol():
// .g = sky openness, .r = rock occlusion) does not exist here: openness 1, no volume AO. Every function keeps the
// demo's name with an `ag` prefix; the sampled animated water texture (W_WATER: r = the demo's caustic, ba = surface
// slope, tiled per metre as in the demo; g = our floor caustics, agCausLod) is passed in as `tW`.
const vec3 kAgSig = vec3(0.058, 0.0235, 0.0195);
const vec3 kAgKd = vec3(0.056, 0.0245, 0.0170);
const vec3 kAgWcol = vec3(0.16, 0.58, 0.90);
const vec3 kAgKdz = vec3(0.034, 0.023, 0.018);
const vec3 kAgPcol = vec3(0.58, 0.66, 0.98);
const vec3 kAgSunc = vec3(1.7, 1.6, 1.45);
const vec3 kAgKdf = vec3(0.043, 0.045, 0.042);
const float kAgZone = 0.0;
float agSat(float x) { return clamp(x, 0.0, 1.0); }
float agSs(float a, float b, float x) { float t = clamp((x - a) / (b - a), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
float agIgn(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
float agHs(float x) { return fract(sin(x * 127.1) * 43758.5453); }
vec3 agKd(float z) { return mix(kAgKd, kAgKdz, z); }
vec3 agSg(float z) { return mix(kAgSig, vec3(0.034, 0.026, 0.022), z); }
// [I] ours, not the demo's: the depth the light terms see (sun, ambient, side light, the fog's in-scatter, the shafts),
// adapted for depths below the demo's range. Its deepest floor lies ~56 m down; TRU's seas go deeper (the l1 bed is
// ~139 m under the surface), where the literal exp(kd y) leaves a play area nearly black (sun ~0.03 in green). Literal
// down to 20 m, then a soft knee (slope 1 at the knee) towards 56 m: 61 m -> 44.5 m, 139 m -> 54.7 m. Positions (agCuv,
// the surface plane, fog distance, snow) stay literal. Ag metres; unchanged above -20 m (and above the surface).
float agLightY(float y) { return y >= -20.0 ? y : -20.0 - 36.0 * (1.0 - exp((y + 20.0) / 36.0)); }
// World (TRU units, z up) -> ag space, the surface of the eye's water volume (ouUwSurfaceZ) at y = 0.
vec3 agPos(vec3 p) { return vec3(p.x, p.z - ouUwSurfaceZ(), p.y) / kM; }
vec3 agDir(vec3 d) { return vec3(d.x, d.z, d.y); }
vec3 agEye() { return agPos(w.eye.xyz); }
float agTime() { return w.eye.w; } // the demo's g.cam.w: seconds
// Ours: the medium's light is the demo's daylight; the time of day (render::Underwater::light, WaterBlock medium) scales
// the light the medium adds on its own: its sky light (agFogIn's in-scatter, the sky through Snell's window, the
// particles' ambient) by agSkyK and its sun (the shafts, the particles' highlights, the glint) by agSunK. agSunE /
// agAmb / agSide stay the demo's daylight: agRelight takes their ratio around TRU's own (already night-lit) colour.
vec3 agSkyK() { return w.medium.rgb; }
float agSunK() { return w.medium.w; }
// g.sun.xyz: toward the sun under water. The demo's sun is steep (y = 0.94) and unrefracted; a level sun can sit low
// (z 0.05..0.3 would shear agCuv's projection 3-20x the depth and black out agSunE within metres), so the level sun is
// refracted at the flat surface (n = 1.333: elevation >= ~41 deg, y >= ~0.66), as the light really travels under water.
// Without a level sun: the demo's own SUN (0.27, 1, -0.24) normalised.
vec3 agSun() {
    if (w.sunColor.w <= 0.0 || w.sunDir.z <= 0.0) return normalize(vec3(0.27, 1.0, -0.24));
    return agDir(-refract(-normalize(w.sunDir.xyz), vec3(0.0, 0.0, 1.0), 1.0 / 1.333));
}
// The same sun above the surface (agSun is its refraction): the air side, where W_SURF's refracted ray rr looks for the
// glint. Without a level sun the demo's SUN, as agSun (the demo's one g.sun is both).
vec3 agSunAir() {
    if (w.sunColor.w <= 0.0 || w.sunDir.z <= 0.0) return normalize(vec3(0.27, 1.0, -0.24));
    return agDir(normalize(w.sunDir.xyz));
}
// Where the shafts (agRays) look up the sun shadow map, rendered along the sun in air, for a world point q under the
// surface, so that their shadow volumes follow the light as it travels under water, along agSun like the shafts' own
// pattern and the caustics (the demo's one sun casts its shadow map along the direction its shafts take). The refracted
// ray from q meets the surface at ps; the map's verdict for the air line through ps (what blocks the sun above the water:
// a hull) is read at that line's point at q's depth. Occluders under water are then found along that air line, shifted
// from the refracted one by up to depth x (cot(air elevation) - cot(refracted)): fine for points in open water, not for
// lit faces (their own rock would shadow them), so agRelight keeps q. Unchanged above the surface or without a level sun.
vec3 agShadowPoint(vec3 q) {
    float s = ouUwSurfaceZ();
    if (w.sunColor.w <= 0.0 || w.sunDir.z <= 0.0 || q.z >= s) return q;
    vec3 a = normalize(w.sunDir.xyz), r = agDir(agSun()); // toward the sun: in air, under water (world)
    vec3 ps = q + r * ((s - q.z) / r.z);
    return ps - a * ((s - q.z) / a.z);
}
vec3 agFogIn(vec3 ro, vec3 rd, float d, float z, vec3 sun) {
    vec3 k = mix(kAgKd, kAgKdf, z);
    vec3 a = agSg(z) - k * rd.y;
    vec3 I = (1.0 - exp(-a * d)) / a;
    float mu = max(dot(rd, sun), 0.0);
    float m2 = mu * mu;
    return mix(kAgWcol, kAgPcol, z) * agSg(z) * exp(k * agLightY(ro.y)) * I * (1.22 + 0.3 * m2 * m2) * agSkyK();
}
vec3 agFin(vec3 c) { return min(max(c, vec3(0.0)), vec3(40.0)); }
// cuv (S_B): the caustic texture projected along the sun onto the point.
vec2 agCuv(vec3 p) { vec3 s = agSun(); return p.xz - s.xz * (p.y / s.y); }
// The floor caustics: ours, not the demo's. Its caus / caus2 (r shaped by pow 2.3, x5, clamped at 4) left ~57 % of a
// lit floor on a flat white plateau with dark blobs. tW's g channel holds a physically formed map instead
// (shaders/uw_caustics.vert: sunlight refracted by a wavy surface and forward-mapped 4 m down; light-conserving, mean
// 1: thin bright filaments over cells at ~0.3-0.5x), projected along the refracted sun onto p (agCuv) and tiled per
// kAgCausTile = 6.6 m (2x the demo's 3.3 m: no visible repeats). With the depth h = -p.y under the surface: the
// contrast grows from none at the surface to full at 3 m (the waves focus the light with depth), the pattern blurs by
// an extra level per doubling of h past 2 m (the sun's 0.5 deg disc spreads each filament by ~0.7 % of the depth: ~0.5
// texel per metre) and fades to the mean between 10 and 35 m; it is the mean past 35..100 m from the eye. Returned in
// the demo's units: x kAgCausMean, the old texture's near-surface mean that agSolidLight's sun / ambient balance (and
// the user's look) was tuned at, so the average light is unchanged. Three taps 6 mm / 4 mm apart: water's dispersion
// (red refracts least) fringes the filaments by about that much. Explicit levels, for points reconstructed from the
// depth buffer (their implicit derivatives spike at depth edges and leave blurred-mip outlines on silhouettes): fp =
// the pixel's footprint in metres (distance x pixel angle), level = log2(fp / texel (kAgCausTile / size)).
const float kAgCausMean = 3.3, kAgCausTile = 6.6; // = uw_caustics.vert kTile
vec3 agCausLod(sampler2D tW, vec3 p, float d, float fp) {
    vec2 q = agCuv(p) / kAgCausTile;
    float h = max(-p.y, 0.0);
    float l = log2(max(fp * float(textureSize(tW, 0).x) / kAgCausTile, 1e-6)) + log2(max(h * 0.5, 1.0));
    vec2 o = vec2(0.006, 0.004) / kAgCausTile;
    vec3 c = vec3(textureLod(tW, q + o, l).g, textureLod(tW, q, l).g, textureLod(tW, q - o, l).g);
    float k = agSs(0.0, 3.0, h) * (1.0 - agSs(10.0, 35.0, h)) * (1.0 - agSs(35.0, 100.0, d));
    return kAgCausMean * mix(vec3(1.0), c, k);
}
vec3 agSunE(float y, float z) { return kAgSunc * exp(agKd(z) * (agLightY(min(y, 0.0)) / agSun().y)) * (0.5 + 0.5 * agSat(-y / 30.0)); }
vec3 agAmb(vec3 p, vec3 n, float sky, float z) {
    float up = n.y * 0.5 + 0.5;
    return mix(vec3(0.22, 0.60, 0.80), vec3(0.34, 0.56, 0.82), z) * exp(agKd(z) * agLightY(min(p.y, 0.0)) * 1.2) * (0.3 + 0.7 * sky) *
               (0.24 + 0.76 * up * up) * 1.3 * (0.55 + 0.45 * agSat(-p.y / 30.0)) +
           vec3(0.003, 0.010, 0.016);
}
vec3 agSide(vec3 p, vec3 n, float open, float z) {
    return mix(kAgWcol, kAgPcol, z) * exp(agKd(z) * agLightY(min(p.y, 0.0))) * open * (1.0 - abs(n.y)) * 0.7;
}
// W_SOLID fs light on rock (mat <= 1: gloss 0.1) at ag point p: N the shading normal, n0 the geometric one, V toward the
// eye, ca = agCausLod, sh = the sun visibility (shadow9). diffuse = dl + am (the demo multiplies it by the albedo: c = alb
// x (dl + am)), spec = its specular E (0.25 + ca) pow(N.H, mix(10, 70, gloss)) gloss ndl 1.8 (added as is). No vol():
// sky openness 1, AO 1, no fluorescence (rock); the environment reflection term is left out. Faces turned from the sun
// get the caustics' mean (ours: the demo's 0.33 there, a tenth of its mean, darkened them beyond their N.L).
void agSolidLight(vec3 p, vec3 N, vec3 n0, vec3 V, vec3 ca, float sh, out vec3 diffuse, out vec3 spec) {
    const float z = kAgZone, gloss = 0.1;
    vec3 L = agSun();
    vec3 E = agSunE(p.y, z) * sh;
    float ndl = agSat(dot(N, L));
    vec3 am = agAmb(p, N, 1.0, z) + agSide(p, N, 1.0, z);
    vec3 dl = E * ndl * (0.10 + mix(vec3(kAgCausMean), ca, agSs(0.1, 0.45, dot(n0, L))) * 2.5);
    diffuse = dl + am;
    vec3 H = normalize(V + L);
    spec = E * (0.25 + ca) * pow(agSat(dot(N, H)), mix(10.0, 70.0, gloss)) * gloss * ndl * 1.8;
}
// A submerged opaque point TRU lit (lin: its colour in the demo's HDR, agUnpost below; water.frag from above: linear)
// relit as W_SOLID lights rock: lin x L(p) / L_ref + spec x the
// albedo estimate's luminance. L = agSolidLight's diffuse at ag point p (normal n, toward the eye V, caustic ca, sun
// visibility sh); L_ref = the same normal and visibility at the surface (y = 0) under the mean caustic kAgCausMean. So
// TRU's colour stands for an average caustic-lit point just under the surface, its own shading, sun shadows and local
// light stay (the demo's N.L and shadow9 cancel instead of applying twice, and the flat-shaded reconstructed normal
// leaves no facets), and the demo's depth falloff, caustic contrast (bright filaments and dim cells around the mean; the
// mean itself far away and deep), ambient and side light are imposed. HDR: not clamped (the W_POST exposure + ACES
// compress the peaks).
vec3 agRelight(vec3 lin, vec3 p, vec3 n, vec3 V, vec3 ca, float sh) {
    vec3 dl, sp, dr, sr;
    agSolidLight(p, n, n, V, ca, sh, dl, sp);
    agSolidLight(vec3(p.x, 0.0, p.z), n, n, V, vec3(kAgCausMean), sh, dr, sr);
    vec3 a = lin / dr;
    return a * dl + sp * dot(a, vec3(0.2126, 0.7152, 0.0722));
}
// TRU's frame is display-referred: its lit colours are what a player sees (D3D9 gamma, <= 1, its own lighting already
// squeezed into that range), while the demo's medium, sky and particles are scene-referred HDR that W_POST exposes and
// tone maps (uw_grade.frag). Fed in as is, TRU's colours went through exposure (0.37 near the surface) and ACES a second
// time: 2-4x darker (0.5 -> 0.25 displayed, 0.05 -> 0.012), sunk into the full-strength fog: the flat, hazy frame. So
// TRU's linear colour is mapped back through the grade's tone map at the reference point (just under the surface:
// exposure EXPO x 0.6, main.js ek = 0): the HDR value W_POST's exposure + ACES fit turn into that colour. agRelight then
// imposes the depth falloff and caustics around it, the fog adds over it, and the frame's own exposure + ACES bring a
// point at the reference back to TRU's colour (before the grade's S-curve and desaturation). White (clamped at 0.98)
// maps to ~12, the demo's own range (a sun-facing top lit by its caustic plateau: albedo x ~8).
const float kAgExpoRef = 0.62 * 0.6;
vec3 agUnpost(vec3 lin) {
    vec3 y = clamp(lin, 0.0, 0.98);
    vec3 a = 2.51 - 2.43 * y, b = 0.03 - 0.59 * y; // tmap(x) = y: a x^2 + b x - 0.14 y = 0
    return (sqrt(b * b + 0.56 * a * y) - b) / (2.0 * a) / kAgExpoRef;
}
// The surface seen from below (water.frag, W_SURF) is HDR (the window's sky 0.4..3.9, the glint ~13) but goes through the
// scene target, 8-bit with graphics.hdr off: written as is it clipped at 1, the bright gaps (1.2, 3, 3.9) turning white
// (1, 1, 1). So water.frag writes c / (1 + c) (invertible, ~c in the darks, so a pixel the post does not decode reads
// about right) and ouUwFog decodes it for rays leaving through the surface (precision: ~5 % steps at c = 4).
vec3 agSurfEncode(vec3 c) { return c / (1.0 + c); }
vec3 agSurfDecode(vec3 e) { return e / max(1.0 - e, vec3(1.0 / 41.0)); } // agFin's 40 at most
// fog (S_B) seen from the eye: skylight at most 150 m along the ray (the vol() openness term is 1 here).
vec3 agFog(vec3 c, vec3 wp, float z) {
    vec3 cam = agEye();
    vec3 v = wp - cam;
    float d = length(v);
    vec3 rd = v / max(d, 1e-6);
    return c * exp(-agSg(z) * d) * agSs(230.0, 185.0, d) + agFogIn(cam, rd, d, z, agSun());
}
#endif
