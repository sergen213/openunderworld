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
#endif
