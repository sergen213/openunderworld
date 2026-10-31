// Enhanced caustics, shared by the underwater post pass (underwater.glsl: camera under water) and the water surface
// (water.frag: the refracted bed seen from above). Embedded for GL by render/Water.cpp (expands the #include lines),
// #included by glslc for Vulkan. Needs the block `w` (sunDir, sunColor, eye, ripple*, fogRange.z, seaWaves), the
// includer's `uRipples` and `float ouSunShadowVisibility(vec3 p)` (may be defined after this file).
// docs/RENDERING.md "Water". [I: tru.exe draws none; its under-water drawable is the sun glow and the full-screen
// pass]. Written for this project, after the physics Evan Wallace's WebGL Water uses (MIT; light intensity = old area
// / new area of the refracted beam, docs/RESEARCH.md "Caustics"): the drawn surface itself (shaders/waves.glsl: the
// Godot layers, afl_ext's sea and tru.exe's sea waves, the interactive wave field) refracts the sun, and the
// brightness is 1 / |det J| of that refraction map, so the lines are the surface's own fold curves and move with its
// waves, Lara's ripples and splashes. ouCausticFocus is the cheap variant for the sun shafts.
#include "shaders/waves.glsl"
// Caustic weights of the drawn surface's terms: the short waves (Godot layers, wave field) make the dense network of a
// shallow reef, the long swell (afl's sea, tru.exe's waves) focuses far too strongly at full weight (a few long folds
// metres apart): its effective focal length is shortened. Tuned by look.
const float kCausticChop = 3.0, kCausticSwell = 0.3;
// Caustic slope at p: water.frag's normal terms without its distance fades (the Godot layers, halved on seas; afl_ext's
// sea, seaIters iterations, difference step >= blur; tru.exe's sea waves), reweighted. blur: world units the slope is
// smoothed over (the layers' octaves finer than it fade). The wave field is ouCausticRipples.
vec2 ouCausticSlope(vec2 p, float t, float blur, int seaIters) {
    bool sea = any(notEqual(w.seaWaves[3], vec4(0.0)));
    vec2 s = (kCausticChop * (sea ? 0.25 : 0.5)) * (ouLayerA(p, t, blur).yz + ouLayerB(p, t, blur).yz);
    if (sea) {
        float e = max(0.01 * kSeaUnit, blur), h = ouSea(p, t, seaIters);
        vec2 g = vec2(ouSea(p + vec2(e, 0.0), t, seaIters) - h, ouSea(p + vec2(0.0, e), t, seaIters) - h) * (kSeaNormalAmp / e);
        s += kCausticSwell * (g + ouTruWaveSlope(p, w.seaWaves));
    }
    return s;
}
// Its Hessian (columns d slope / dx, d slope / dy, symmetrised) at p by differences `blur` apart; g0 the slope at p.
mat2 ouCausticHessian(vec2 p, vec2 g0, float t, float blur, int seaIters) {
    vec2 gx = (ouCausticSlope(p + vec2(blur, 0.0), t, blur, seaIters) - g0) / blur;
    vec2 gy = (ouCausticSlope(p + vec2(0.0, blur), t, blur, seaIters) - g0) / blur;
    float xy = 0.5 * (gx.y + gy.x);
    return mat2(gx.x, xy, xy, gy.y);
}
// The wave field's part (water.frag's ripple normal gain x kCausticChop): xy slope, z curvature taken isotropic (half
// its Laplacian; its own taps 1.5 cells apart, so the rest keeps fine differences).
vec3 ouCausticRipples(vec2 p) {
    vec4 v = ouWaves(p);
    return (kCausticChop * w.fogRange.z * 1.2) * vec3(v.xy, 0.5 * v.w);
}
// Relative intensity 1 / |det(I + c H)| (1 = flat water), softened by e, then soft-clamped I (1 + 1/3) / (1 + I/3)
// (1 stays 1, crossing folds saturate toward 4 instead of blowing out). c = refraction displacement per unit slope.
float ouFocus(mat2 H, float c, float e) {
    float det = (1.0 + c * H[0][0]) * (1.0 + c * H[1][1]) - c * c * H[0][1] * H[0][1];
    float i = sqrt((1.0 + e * e) / (det * det + e * e));
    return i * (4.0 / 3.0) / (1.0 + i / 3.0);
}
// Light-ray displacement on the bed per unit surface slope at depth h: path length x (1 - 1 / 1.333) (a tilted
// surface bends the refracted ray toward its crest: crests focus); capped at ~13 m of path where the folds would
// cross many times (one fold branch would then darken on average) and the sun-disk blur takes over.
float ouFocusLength(float h) { return 0.25 * min(h, 4000.0); }
// Cheap focus for the sun shafts (underwater.glsl): entry = world xy where the refracted sun ray entered the surface,
// h = depth under the surface there, blur = smoothing (world units), seaIters = afl iterations. Single colour, no
// inverse-map step. 1 = flat water, at most 4.
float ouCausticFocus(vec2 entry, float h, float blur, int seaIters) {
    float t = w.eye.w;
    blur = max(blur, 4.0);
    vec3 r = ouCausticRipples(entry);
    mat2 H = ouCausticHessian(entry, ouCausticSlope(entry, t, blur, seaIters), t, blur, seaIters) + mat2(r.z);
    return ouFocus(H, ouFocusLength(h), 0.15);
}
float ouSunShadowVisibility(vec3 p);
// Caustic light factor for the opaque point p lying h units under its water surface, n its (unnormalised) surface
// normal, footprint world units per pixel there. The light reaching p entered the surface near E (the flat-water
// refracted sun ray); it left from s = E - c slope(E) (one fixed-point step of the inverse refraction map, c =
// ouFocusLength), J = I + c Hessian(s), per colour with red focusing 2.5% deeper than blue (water dispersion: soft
// fringes). Blur: the sun disk (0.5 deg: ~0.005 x the path) and >= 1.5 pixels; it fades the fine wave octaves and
// widens the lines (no shimmer); depth also softens the folds (e) and lowers the contrast to 60%
// (l1_seafloor lies 95 m down: a soft shimmer kept for the look; physically caustics fade within ~10-20 m). Fades with
// the normal facing the refracted sun, in the sun shadow map, and with the footprint past ~60 units per pixel.
// Multiply the lit colour by it. The single fold branch loses the light of overlapping folds, so gaps keep half their
// deficit (about -20%; lines up to +200% at full strength).
// sunColor.w = strength (0: no sun).
vec3 ouCausticLight(vec3 p, vec3 n, float h, float footprint) {
    if (w.sunColor.w <= 0.0 || h <= 0.0 || w.sunDir.z < 0.05 || dot(n, n) < 1e-12) return vec3(1.0);
    vec3 l = -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / 1.333); // toward the sun, under water
    n = normalize(n);
    if (dot(n, w.eye.xyz - p) < 0.0) n = -n;
    float path = h / l.z, deep = 1.0 - exp(-h / 8000.0);
    float amount = w.sunColor.w * mix(1.0, 0.6, deep) * smoothstep(0.15, 0.7, dot(n, l)) * (1.0 - smoothstep(25.0, 60.0, footprint));
    if (amount <= 0.002) return vec3(1.0);
    amount *= ouSunShadowVisibility(p);
    float t = w.eye.w, c = ouFocusLength(path), e = 0.025 + 0.5 * deep; // deeper: wider sun-disk blur, folds wash out
    float blur = max(max(0.005 * path, 1.5 * footprint), 4.0);
    const int kSeaIters = 6;
    vec2 E = p.xy + l.xy * path; // where the flat-water refracted sun ray through p entered the surface
    vec2 s = E - c * (ouCausticSlope(E, t, blur, kSeaIters) + ouCausticRipples(E).xy);
    mat2 H = ouCausticHessian(s, ouCausticSlope(s, t, blur, kSeaIters), t, blur, kSeaIters) + mat2(ouCausticRipples(s).z);
    vec3 k = vec3(ouFocus(H, 0.975 * c, e), ouFocus(H, c, e), ouFocus(H, 1.025 * c, e));
    k = max(k, 0.5 + 0.5 * k);
    vec3 tint = w.sunColor.rgb / max(max(w.sunColor.r, w.sunColor.g), max(w.sunColor.b, 1e-3)); // the sun's hue
    return 1.0 + (0.7 * amount) * tint * (k - 1.0);
}
