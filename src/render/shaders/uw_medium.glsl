// Enhanced under-water medium (ours, camera submerged; RENDERING.md "Under water"): the one fog / light model every
// under-water effect uses — the post pass's fog (underwater.glsl ouUwFog), the sun shafts (ouShaftMarch /
// ouShaftsComposite), the particles (uw_light.glsl) and the surface's total internal reflection seen from below
// (water.frag). Analytic medium of abyssal-garden (Noniv, MIT; docs/RESEARCH.md 8.11: shaders.js `fogIn`, `fog`, `sg`,
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
// Light on a submerged point h metres under the surface, unit normal n, relative to the light TRU's shading gave it
// (1 at the surface for sun-facing tops): sunE (the refracted sun's path h / l.z) 60 % + amb (e^(-1.2 kd h), sky
// hemisphere (0.24 + 0.76 up^2)) 40 % + side (water colour on walls, e^(-kd h) (1 - |n.z|)) [I weights].
vec3 ouUwSunLight(OuUwMedium m, float h, vec3 l) { return exp(-m.kd * (h / max(l.z, 0.2))); } // sunE alone
vec3 ouUwSurfaceLight(OuUwMedium m, vec3 n, float h, vec3 l) {
    float up = 0.5 + 0.5 * n.z;
    vec3 amb = m.ambient * exp(-1.2 * m.kd * h) * (0.24 + 0.76 * up * up);
    vec3 side = m.ambient * exp(-m.kd * h) * ((1.0 - abs(n.z)) * 0.2);
    return 0.6 * ouUwSunLight(m, h, l) + 0.4 * amb + side;
}
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
// g.sun.xyz: toward the sun, unrefracted (the demo never refracts it); the level's sun when it has one high enough,
// else the demo's own SUN (0.27, 1, -0.24) normalised.
vec3 agSun() {
    return w.sunColor.w > 0.0 && w.sunDir.z > 0.05 ? normalize(agDir(w.sunDir.xyz)) : normalize(vec3(0.27, 1.0, -0.24));
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
// fog (S_B) seen from the eye: skylight at most 150 m along the ray (the vol() openness term is 1 here).
vec3 agFog(vec3 c, vec3 wp, float z) {
    vec3 cam = agEye();
    vec3 v = wp - cam;
    float d = length(v);
    vec3 rd = v / max(d, 1e-6);
    return c * exp(-agSg(z) * d) * agSs(230.0, 185.0, d) + agFogIn(cam, rd, d, z, agSun());
}
#endif
