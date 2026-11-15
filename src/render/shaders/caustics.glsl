// The drawn surface's focusing for the Enhanced under-water sun shafts (underwater.glsl ouShaftPatternTexel:
// ouCausticFocus). The caustics on the scene are abyssal-garden's texture caustics (uw_medium.glsl agCaus on uAgWater).
// Embedded for GL by render/Water.cpp (expands the #include lines), #included by glslc for Vulkan. Needs the block `w`
// (eye, ripple*, fogRange.z, seaWaves, ocean) and the includer's `uRipples` and `uOcean`. docs/RENDERING.md "Water".
// Written for this project, after the physics Evan Wallace's WebGL Water uses (MIT; light intensity = old area / new
// area of the refracted beam, docs/RESEARCH.md "Caustics"): the drawn surface itself (shaders/waves.glsl: the Godot
// layers, afl_ext's sea or the FFT ocean, tru.exe's sea waves, the interactive wave field) refracts the sun, and the
// brightness is 1 / |det J| of that refraction map.
#include "shaders/waves.glsl"
// Caustic weights of the drawn surface's terms: the short waves (Godot layers, wave field) make the dense network of a
// shallow reef, the long swell (afl's sea, tru.exe's waves) focuses far too strongly at full weight (a few long folds
// metres apart): its effective focal length is shortened. Tuned by look.
const float kCausticChop = 3.0, kCausticSwell = 0.3;
// The FFT ocean's cascades (swell, gravity, chop) [I]: the swell as afl's sea, the shorter bands toward the chop.
const vec3 kOceanCaustic = vec3(kCausticSwell, 1.0, 0.75);
bool ouCausticOcean() { return any(notEqual(w.seaWaves[3], vec4(0.0))) && ouOceanOn(); }
// Caustic slope at p without the FFT ocean: water.frag's normal terms without its distance fades (the Godot layers,
// halved on seas; afl_ext's sea, seaIters iterations, difference step >= blur; tru.exe's sea waves), reweighted; with
// the FFT ocean (fft) only tru.exe's waves (the ocean replaces the layers and afl's sea, as in water.frag). blur: world
// units the slope is smoothed over (the layers' octaves finer than it fade). The wave field is ouCausticRipples.
vec2 ouCausticSlopeBase(vec2 p, float t, float blur, int seaIters, bool fft) {
    bool sea = any(notEqual(w.seaWaves[3], vec4(0.0)));
    vec2 s = fft ? vec2(0.0) : (kCausticChop * (sea ? 0.25 : 0.5)) * (ouLayerA(p, t, blur).yz + ouLayerB(p, t, blur).yz);
    if (sea) {
        if (!fft) {
            float e = max(0.01 * kSeaUnit, blur), h = ouSea(p, t, seaIters);
            s += kCausticSwell * vec2(ouSea(p + vec2(e, 0.0), t, seaIters) - h, ouSea(p + vec2(0.0, e), t, seaIters) - h) * (kSeaNormalAmp / e);
        }
        s += kCausticSwell * ouTruWaveSlope(p, w.seaWaves);
    }
    return s;
}
// The full caustic slope at p: the base plus the FFT cascades' slopes (the drawn surface's, before water.frag's fades).
vec2 ouCausticSlope(vec2 p, float t, float blur, int seaIters) {
    bool fft = ouCausticOcean();
    vec2 s = ouCausticSlopeBase(p, t, blur, seaIters, fft);
    if (fft)
        for (int c = 0; c < 3; ++c) s += kOceanCaustic[c] * ouOceanSlope(c, p);
    return s;
}
// Its Hessian (columns d slope / dx, d slope / dy, symmetrised) at p: the base by differences `blur` apart (g0 the
// slope at p); each FFT cascade by differences of at least one of its texels (its slope is interpolated linearly, so a
// finer step would see a constant Hessian per texel: a grid of flat cells).
mat2 ouCausticHessian(vec2 p, vec2 g0, float t, float blur, int seaIters) {
    bool fft = ouCausticOcean();
    vec2 b0 = fft ? ouCausticSlopeBase(p, t, blur, seaIters, true) : g0;
    vec2 gx = (ouCausticSlopeBase(p + vec2(blur, 0.0), t, blur, seaIters, fft) - b0) / blur;
    vec2 gy = (ouCausticSlopeBase(p + vec2(0.0, blur), t, blur, seaIters, fft) - b0) / blur;
    float xy = 0.5 * (gx.y + gy.x);
    mat2 H = mat2(gx.x, xy, xy, gy.y);
    if (fft)
        for (int c = 0; c < 3; ++c) {
            float e = max(blur, ouOceanTexel(c));
            vec2 s0 = ouOceanSlope(c, p);
            vec2 hx = (ouOceanSlope(c, p + vec2(e, 0.0)) - s0) / e, hy = (ouOceanSlope(c, p + vec2(0.0, e)) - s0) / e;
            float hxy = 0.5 * (hx.y + hy.x);
            H += kOceanCaustic[c] * mat2(hx.x, hxy, hxy, hy.y);
        }
    return H;
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
// Water's dispersion, per colour: n_RGB = (1.331, 1.334, 1.339) (NeuralSpecOcean's whitepaper 4.3, "Analytical
// Chromatic Hessian Caustics": focal distance d_lambda = d (1 - 1 / n_lambda); MIT, docs/RESEARCH.md): red focuses
// 0.7% shorter and blue 1.1% longer than green, so each fold line splits into a thin red-to-blue fringe.
const vec3 kIorRgb = vec3(1.331, 1.334, 1.339);
vec3 ouFocusLengths(float h) { return (1.0 - 1.0 / kIorRgb) * min(h, 4000.0); }
vec3 ouFocusRgb(mat2 H, vec3 c, float e) { return vec3(ouFocus(H, c.r, e), ouFocus(H, c.g, e), ouFocus(H, c.b, e)); }
// Cheap focus for the sun shafts (underwater.glsl): entry = world xy where the refracted sun ray entered the surface,
// h = depth under the surface there, blur = smoothing (world units), seaIters = afl iterations. Per colour (one
// Hessian, the three focal distances), no inverse-map step. 1 = flat water, at most 4.
vec3 ouCausticFocus(vec2 entry, float h, float blur, int seaIters) {
    float t = w.eye.w;
    blur = max(blur, 4.0);
    vec3 r = ouCausticRipples(entry);
    mat2 H = ouCausticHessian(entry, ouCausticSlope(entry, t, blur, seaIters), t, blur, seaIters) + mat2(r.z);
    return ouFocusRgb(H, ouFocusLengths(h), 0.15);
}
