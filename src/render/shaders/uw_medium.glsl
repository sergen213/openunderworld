// Enhanced under-water medium (ours, camera submerged; RENDERING.md "Under water"). The `ag` section at the end is the
// literal abyssal-garden port used by the post pass's light and fog (underwater.glsl ouUwFog) and water.frag's bed; the
// older tuned OuUwMedium API above still serves the sun shafts (ouShaftMarch / ouShaftsComposite), the particles
// (uw_light.glsl) and the surface's total internal reflection seen from below (water.frag).
// OuUwMedium: the analytic medium of abyssal-garden (Noniv, MIT; docs/RESEARCH.md 8.11: shaders.js `fogIn`, `fog`, `sg`,
// `kd`, `sunE`, `amb`, `side`) on the unit's own colours. Linear light, metres (kM units).
// Needs the WaterBlock `w`, kM (shaders/waves.glsl) and kIorRgb (shaders/caustics.glsl) declared before it.
#ifndef OU_UW_MEDIUM_GLSL
#define OU_UW_MEDIUM_GLSL
struct OuUwMedium { vec3 sigma, kd, scatter, ambient; };
// sigma: TRU's pass reaches 80 % fog at half its full-fog distance (the mix from the eye alpha to 1); an exponential
// medium with the same 80 % there gives green ln 5 / (D / 2) [I] (l1: 45000 units = 155 m -> 0.021 / m, abyssal-garden's
// 0.0235). Other channels by the surface colour: (green / channel)^0.6 [I] (l1: red 2.3x, blue 0.7x green; abyssal-garden
// 2.5x / 0.83x), so red goes first. kd = sigma x abyssal-garden's KD / SIG (0.97, 1.04, 0.87), its spread between the
// channels kept at 60 % [I] (TRU's unit colours already carry the depth's blue: the full spread turned l1_seafloor navy).
// In-scatter colour = the surface colour (the water just under the surface, horizontal view), linear, x 1.25 [I] (the
// demo's open water reads far brighter than TRU's). A preset (w.optics[0].w): its extinction ratios and in-scatter hue
// at that colour's luminance (as water.frag ouWaterLook).
OuUwMedium ouUwMedium() {
    bool unit = w.waterPass[1].w > 0.0;
    vec3 s = pow(max(unit ? w.waterPass[0].rgb : vec3(0.447, 0.780, 0.792), vec3(1e-3)), vec3(2.2));
    float g = 3.2189 / (unit ? w.waterPass[1].w : 100000.0) * kM; // per metre
    vec3 ratio = clamp(pow(vec3(s.g) / s, vec3(0.6)), vec3(0.5), vec3(4.0));
    vec3 scatter = s;
    if (w.optics[0].w > 0.5) {
        ratio = clamp(w.optics[0].rgb / max(w.optics[0].g, 1e-9), vec3(0.25), vec3(6.0));
        scatter = w.optics[1].rgb * dot(s, vec3(0.2126, 0.7152, 0.0722));
    }
    OuUwMedium m;
    m.sigma = g * ratio;
    vec3 kd = m.sigma * vec3(0.97, 1.04, 0.87);
    m.kd = mix(vec3(dot(kd, vec3(0.2126, 0.7152, 0.0722))), kd, 0.6);
    m.scatter = scatter * 1.25;
    // abyssal-garden's ambient is the water colour (0.22, 0.60, 0.80) ~ its WCOL: here the in-scatter hue at luminance 1,
    // half-way to white [I] (TRU's own lighting already carries the unit's tint).
    m.ambient = mix(vec3(1.0), scatter / max(dot(scatter, vec3(0.2126, 0.7152, 0.0722)), 1e-4), 0.5);
    return m;
}
// Surface z of the nearest water volume (the eye's own height without one).
float ouUwSurfaceZ() { return w.detail.w > 0.5 ? w.volumes[1].z : w.eye.z; }
// The eye's depth under that surface, metres.
float ouUwEyeDepth() { return max(ouUwSurfaceZ() - w.eye.z, 0.0) / kM; }
// Toward the sun under water (refracted at the flat surface), straight up without a sun.
vec3 ouUwSunDir() {
    return w.sunColor.w > 0.0 && w.sunDir.z > 0.0 ? -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / kIorRgb.g)
                                                   : vec3(0.0, 0.0, 1.0);
}
// The medium as seen from this frame's eye: TRU's seas are deep (l1_seafloor: the surface 81 m over the bed);
// e^(-kd h) there leaves green at 17 %, red at 2 % (navy black). The light falls off over an effective depth H
// (1 - e^(-h / H)), H = 30 m [I] (abyssal-garden's deepest floor is ~56 m): kd scaled by Heff / h0 for the whole frame,
// so the light still grows rising from the eye and every effect sees the same water.
OuUwMedium ouUwEyeMedium() {
    OuUwMedium m = ouUwMedium();
    const float kUwDepthScale = 30.0;
    float h0 = ouUwEyeDepth();
    if (h0 > 1e-3) m.kd *= kUwDepthScale * (1.0 - exp(-h0 / kUwDepthScale)) / h0;
    return m;
}
// In-scatter along unit rd (z up) over d metres from a point h0 metres under the surface; l: toward the sun under water.
// fogIn: sigma C e^(-kd h0) (1 - e^(-a d)) / a, a = sigma - kd rd.z (the light grows as the ray rises), x the forward
// sun glow (1.22 + 0.3 mu^4) / 1.22.
vec3 ouUwFogIn(OuUwMedium m, vec3 rd, float d, float h0, vec3 l) {
    vec3 a = m.sigma - m.kd * rd.z;
    a += vec3(1e-6) * vec3(lessThan(abs(a), vec3(1e-6)));
    float mu = max(dot(rd, l), 0.0), m2 = mu * mu;
    return m.scatter * m.sigma * exp(-m.kd * h0) * ((1.0 - exp(-a * d)) / a) * (1.0 + 0.246 * m2 * m2);
}
vec3 ouUwSunLight(OuUwMedium m, float h, vec3 l) { return exp(-m.kd * (h / max(l.z, 0.2))); } // sunE alone (shafts)
// The water between the eye and world point p (per-point form of ouUwFog): transmittance T and in-scatter L (linear),
// both x the pass strength (w.waterPass[2].x), so p's own light becomes light x T + L.
void ouUwPointFog(OuUwMedium m, vec3 p, out vec3 T, out vec3 L) {
    vec3 rd = p - w.eye.xyz;
    float d = length(rd);
    rd /= max(d, 1e-3);
    d /= kM;
    T = mix(vec3(1.0), exp(-m.sigma * d), w.waterPass[2].x);
    L = ouUwFogIn(m, rd, d, ouUwEyeDepth(), ouUwSunDir()) * w.waterPass[2].x;
}

// ---- 1:1 port of abyssal-garden's medium (Noniv, MIT; source/src/shaders.js S_G / S_B, docs/RESEARCH.md 8.11) ----
// Literal constants and formulas, in the demo's own space ("ag space"): metres, y up, the water surface at y = 0 (points
// under water have y < 0), x / z horizontal. agPos maps a world point (TRU units, z up) into it; agDir a direction.
// The demo's zone z (0 = its open blue water, 1 = its purple grotto) is 0 here (kAgZone). Its density volume (vol():
// .g = sky openness, .r = rock occlusion) does not exist here: openness 1, no volume AO. Every function keeps the
// demo's name with an `ag` prefix; the sampled animated water texture (W_WATER: r = caustic, ba = surface slope,
// tiled per metre as in the demo) is passed in as `tW`.
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
// World (TRU units, z up) -> ag space, the surface of the eye's water volume (ouUwSurfaceZ) at y = 0.
vec3 agPos(vec3 p) { return vec3(p.x, p.z - ouUwSurfaceZ(), p.y) / kM; }
vec3 agDir(vec3 d) { return vec3(d.x, d.z, d.y); }
vec3 agEye() { return agPos(w.eye.xyz); }
float agTime() { return w.eye.w; } // the demo's g.cam.w: seconds
// g.sun.xyz: toward the sun under water. The demo's sun is steep (y = 0.94) and unrefracted; a level sun can sit low
// (z 0.05..0.3 would shear agCuv's projection 3-20x the depth and black out agSunE within metres), so the level sun is
// refracted at the flat surface (n = 1.333: elevation >= ~41 deg, y >= ~0.66), as the light really travels under water.
// Without a level sun: the demo's own SUN (0.27, 1, -0.24) normalised.
vec3 agSun() {
    if (w.sunColor.w <= 0.0 || w.sunDir.z <= 0.0) return normalize(vec3(0.27, 1.0, -0.24));
    return agDir(-refract(-normalize(w.sunDir.xyz), vec3(0.0, 0.0, 1.0), 1.0 / 1.333));
}
vec3 agFogIn(vec3 ro, vec3 rd, float d, float z, vec3 sun) {
    vec3 k = mix(kAgKd, kAgKdf, z);
    vec3 a = agSg(z) - k * rd.y;
    vec3 I = (1.0 - exp(-a * d)) / a;
    float mu = max(dot(rd, sun), 0.0);
    float m2 = mu * mu;
    return mix(kAgWcol, kAgPcol, z) * agSg(z) * exp(k * ro.y) * I * (1.22 + 0.3 * m2 * m2);
}
vec3 agFin(vec3 c) { return min(max(c, vec3(0.0)), vec3(40.0)); }
// cuv / caus / caus2 (S_B): the caustic texture projected along the sun onto the point.
vec2 agCuv(vec3 p) { vec3 s = agSun(); return p.xz - s.xz * (p.y / s.y); }
vec3 agCaus(sampler2D tW, vec3 p, float d) {
    vec2 q = agCuv(p);
    vec2 o = vec2(0.004, 0.0025);
    vec3 a = vec3(texture(tW, q / 3.3 + o).r, texture(tW, q / 3.3).r, texture(tW, q / 3.3 - o).r);
    float b = texture(tW, q.yx / 6.9 + 0.37).r;
    vec3 c = min(pow(a, vec3(2.3)) * 5.0 * (0.45 + b * 1.3), vec3(4.0));
    return mix(c, vec3(0.33), agSs(35.0, 100.0, d));
}
float agCaus2(sampler2D tW, vec3 p, float d) {
    vec2 q = agCuv(p);
    float c = min(pow(texture(tW, q / 4.1).r, 1.4) * 2.6 * (0.45 + texture(tW, q.yx / 6.9 + 0.37).r * 1.3), 2.6);
    return mix(c, 0.33, agSs(35.0, 100.0, d));
}
// agCaus with explicit levels, for points reconstructed from the depth buffer (the post pass): their implicit
// derivatives spike at depth edges and leave blurred-mip outlines on silhouettes. fp = the pixel's footprint in metres
// (distance x pixel angle); each tap's level = log2(fp / its texel: 3.3 m / size for the q / 3.3 taps, 6.9 m / size for
// the q.yx / 6.9 one).
vec3 agCausLod(sampler2D tW, vec3 p, float d, float fp) {
    vec2 q = agCuv(p);
    float n = float(textureSize(tW, 0).x);
    float la = log2(max(fp * n / 3.3, 1e-6)), lb = log2(max(fp * n / 6.9, 1e-6));
    vec2 o = vec2(0.004, 0.0025);
    vec3 a = vec3(textureLod(tW, q / 3.3 + o, la).r, textureLod(tW, q / 3.3, la).r, textureLod(tW, q / 3.3 - o, la).r);
    float b = textureLod(tW, q.yx / 6.9 + 0.37, lb).r;
    vec3 c = min(pow(a, vec3(2.3)) * 5.0 * (0.45 + b * 1.3), vec3(4.0));
    return mix(c, vec3(0.33), agSs(35.0, 100.0, d));
}
vec3 agSunE(float y, float z) { return kAgSunc * exp(agKd(z) * (min(y, 0.0) / agSun().y)) * (0.5 + 0.5 * agSat(-y / 30.0)); }
vec3 agAmb(vec3 p, vec3 n, float sky, float z) {
    float up = n.y * 0.5 + 0.5;
    return mix(vec3(0.22, 0.60, 0.80), vec3(0.34, 0.56, 0.82), z) * exp(agKd(z) * min(p.y, 0.0) * 1.2) * (0.3 + 0.7 * sky) *
               (0.24 + 0.76 * up * up) * 1.3 * (0.55 + 0.45 * agSat(-p.y / 30.0)) +
           vec3(0.003, 0.010, 0.016);
}
vec3 agSide(vec3 p, vec3 n, float open, float z) {
    return mix(kAgWcol, kAgPcol, z) * exp(agKd(z) * min(p.y, 0.0)) * open * (1.0 - abs(n.y)) * 0.7;
}
// W_SOLID fs light on rock (mat <= 1: gloss 0.1) at ag point p: N the shading normal, n0 the geometric one, V toward the
// eye, ca = agCaus, sh = the sun visibility (shadow9). diffuse = dl + am (the demo multiplies it by the albedo: c = alb x
// (dl + am)), spec = its specular E (0.25 + ca) pow(N.H, mix(10, 70, gloss)) gloss ndl 1.8 (added as is). No vol(): sky
// openness 1, AO 1, no fluorescence (rock); the environment reflection term is left out.
void agSolidLight(vec3 p, vec3 N, vec3 n0, vec3 V, vec3 ca, float sh, out vec3 diffuse, out vec3 spec) {
    const float z = kAgZone, gloss = 0.1;
    vec3 L = agSun();
    vec3 E = agSunE(p.y, z) * sh;
    float ndl = agSat(dot(N, L));
    vec3 am = agAmb(p, N, 1.0, z) + agSide(p, N, 1.0, z);
    vec3 dl = E * ndl * (0.10 + mix(vec3(0.33), ca, agSs(0.1, 0.45, dot(n0, L))) * 2.5);
    diffuse = dl + am;
    vec3 H = normalize(V + L);
    spec = E * (0.25 + ca) * pow(agSat(dot(N, H)), mix(10.0, 70.0, gloss)) * gloss * ndl * 1.8;
}
// The near-surface spatial mean of agCaus (before its 35..100 m fade): 3.2..3.35 over the texture's animation (512^2 W_WATER
// texels at times 3, 10, 40, 100 s, a and b taps drawn independently; ~65 % of a top sits at the 4.0 clamp) [O, offline].
const float kAgCausMean = 3.3;
// A submerged opaque point TRU lit (lin: linear colour) relit as W_SOLID lights rock: lin x L(p) / L_ref + spec x the
// albedo estimate's luminance. L = agSolidLight's diffuse at ag point p (normal n, toward the eye V, caustic ca, sun
// visibility sh); L_ref = the same normal and visibility at the surface (y = 0) under the mean caustic kAgCausMean. So
// TRU's colour stands for an average caustic-lit point just under the surface, its own shading, sun shadows and local
// light stay (the demo's N.L and shadow9 cancel instead of applying twice, and the flat-shaded reconstructed normal
// leaves no facets), and the demo's depth falloff, caustic contrast (lines and dark cells around the mean; 0.33 far
// away ~0.1x), ambient and side light are imposed. HDR: not clamped (the W_POST exposure + ACES compress the peaks).
vec3 agRelight(vec3 lin, vec3 p, vec3 n, vec3 V, vec3 ca, float sh) {
    vec3 dl, sp, dr, sr;
    agSolidLight(p, n, n, V, ca, sh, dl, sp);
    agSolidLight(vec3(p.x, 0.0, p.z), n, n, V, vec3(kAgCausMean), sh, dr, sr);
    vec3 a = lin / dr;
    return a * dl + sp * dot(a, vec3(0.2126, 0.7152, 0.0722));
}
// fog (S_B) seen from the eye: skylight at most 150 m along the ray (the vol() openness term is 1 here).
vec3 agFog(vec3 c, vec3 wp, float z) {
    vec3 cam = agEye();
    vec3 v = wp - cam;
    float d = length(v);
    vec3 rd = v / max(d, 1e-6);
    return c * exp(-agSg(z) * d) * agSs(230.0, 185.0, d) + agFogIn(cam, rd, d, z, agSun());
}
#endif
