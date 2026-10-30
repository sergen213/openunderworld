#version 460
// Enhanced water surface: Wars and Warriors: Joan of Arc's port's stylised water (shaders/common/pbr_shade.glsl
// shade_water, same author, GPLv3) for TRU: saturated teal-to-deep-blue absorption over the bed seen through the
// surface (the opaque scene and its depth, refracted by the waves), a soft shoreline lace-foam band, two drifting
// normal layers, the interactive wave field (game::WaveField), Fresnel reflection and GGX sun glints. Joan works in
// linear light on its own units: the scene copy is gamma (D3D9-style), so colours go to linear and back, and every
// length is x kScale (TRU units per Joan unit, game::kWaterScale). Reflections are ours (screen-space; Joan reads a
// sky environment map). docs/RENDERING.md "Water".
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
} w;
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D uScene;
layout(set = 0, binding = 2) uniform sampler2D uDepth;
layout(set = 0, binding = 3) uniform sampler2D uRipples;
#else
layout(binding = 5) uniform sampler2D uScene;
layout(binding = 6) uniform sampler2D uDepth;
layout(binding = 7) uniform sampler2D uRipples;
#endif
#ifdef VULKAN
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#define OU_UV_Y(ndcy) (0.5 - 0.5 * (ndcy))
#else
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#define OU_UV_Y(ndcy) (0.5 + 0.5 * (ndcy))
#endif
const float kScale = 6.4; // game::kWaterScale
const float kPi = 3.14159265;
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;
layout(location = 0) out vec4 oColor;

vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
vec3 toGamma(vec3 c) { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
vec3 ouWorld(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clip = vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    vec4 p = invViewProj * clip;
    return p.xyz / p.w;
}
vec2 ouScreenUv(vec3 q, out bool inside) {
    vec4 clip = w.viewProj * vec4(q, 1.0);
    vec2 uv = vec2(clip.x / max(clip.w, 1e-4) * 0.5 + 0.5, OU_UV_Y(clip.y / max(clip.w, 1e-4)));
    inside = clip.w > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}
float ouEdgeFade(vec2 uv) { vec2 e = min(uv, 1.0 - uv); return clamp(min(e.x, e.y) * 10.0, 0.0, 1.0); }

// ---- reflections (ours): the opaque scene along the mirror ray, `env` where it leaves the screen; rgb + hit ----
vec4 ouReflectFast(vec3 p, vec3 r, vec3 env) {
    vec3 q = p + vec3(0.0, 0.0, 3.0);
    float stepLen = 90.0;
    for (int i = 0; i < 20; ++i) {
        q += r * stepLen;
        stepLen *= 1.18;
        bool inside;
        vec2 uv = ouScreenUv(q, inside);
        if (!inside) break;
        float sd = texture(uDepth, uv).r;
        if (sd >= 0.999999) continue;
        float dq = length(q - w.eye.xyz), ds = length(ouWorld(uv, sd, w.invViewProj) - w.eye.xyz);
        if (ds < dq && dq - ds < 500.0) return vec4(texture(uScene, uv).rgb, 1.0);
    }
    return vec4(env, 0.0);
}
// Water detail: thickness-tested march with a bisection refine; confidence fades at the screen border and with distance.
vec4 ouReflectDetail(vec3 p, vec3 r, vec3 env) {
    const int kSteps = 24;
    const float kMax = 9000.0;
    float prev = 0.0;
    for (int i = 1; i <= kSteps; ++i) {
        float s = float(i) / float(kSteps);
        float t = 8.0 + s * s * kMax;
        bool inside;
        vec2 uv = ouScreenUv(p + r * t, inside);
        if (!inside) break;
        float sd = texture(uDepth, uv).r;
        if (sd < 0.999999) {
            float diff = length(p + r * t - w.eye.xyz) - length(ouWorld(uv, sd, w.invViewProj) - w.eye.xyz);
            if (diff > 0.0 && diff < 40.0 + 0.1 * t) {
                float lo = prev, hi = t;
                for (int b = 0; b < 4; ++b) {
                    float mid = 0.5 * (lo + hi);
                    vec2 mu = ouScreenUv(p + r * mid, inside);
                    float md = texture(uDepth, mu).r;
                    float dm = length(p + r * mid - w.eye.xyz) - length(ouWorld(mu, md, w.invViewProj) - w.eye.xyz);
                    if (dm > 0.0) hi = mid; else lo = mid;
                }
                uv = ouScreenUv(p + r * hi, inside);
                float conf = ouEdgeFade(uv) * (1.0 - smoothstep(0.6, 1.0, hi / kMax));
                return vec4(mix(env, texture(uScene, uv).rgb, conf), conf);
            }
        }
        prev = t;
    }
    return vec4(env, 0.0);
}
// Water detail: screen-space sun occlusion of the glint (Joan: its shadow map), 8 steps toward the sun.
float ouSunVisibility(vec3 p) {
    for (int i = 1; i <= 8; ++i) {
        float s = float(i) / 8.0;
        float t = 20.0 + s * s * 2500.0;
        bool inside;
        vec3 q = p + w.sunDir.xyz * t;
        vec2 uv = ouScreenUv(q, inside);
        if (!inside) break;
        float sd = texture(uDepth, uv).r;
        if (sd >= 0.999999) continue;
        float diff = length(q - w.eye.xyz) - length(ouWorld(uv, sd, w.invViewProj) - w.eye.xyz);
        if (diff > 10.0 && diff < 80.0 + 0.2 * t) return 0.0;
    }
    return 1.0;
}

// ---- Joan's shade_water ----
// Smooth value noise in [0, 1] and its gradient, one lattice cell per unit of x.
float ouLattice(vec2 c) {
    uvec2 q = uvec2(ivec2(c));
    uint n = (q.x * 1597334677u) ^ (q.y * 3812015801u);
    n = (n ^ (n >> 15)) * 2246822519u;
    return float(n >> 8) * (1.0 / 16777216.0);
}
vec3 ouNoiseD(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
    float a = ouLattice(i), b = ouLattice(i + vec2(1, 0)), c = ouLattice(i + vec2(0, 1)), d = ouLattice(i + vec2(1, 1));
    float k = a - b - c + d;
    return vec3(a + (b - a) * u.x + (c - a) * u.y + k * u.x * u.y, du * vec2(b - a + k * u.y, c - a + k * u.x));
}
// The water normal map at uv (Joan: the original Lake1.bmp height map; TRU has none, so two octaves of value noise,
// 8 features per tile): xy the tangent slope (Joan's ta.xy / ta.z), z the height.
vec3 ouNormalLayer(vec2 uv) {
    vec3 a = ouNoiseD(uv * 8.0), b = ouNoiseD(uv * 19.0 + 7.3);
    return vec3((a.yz + 0.5 * b.yz) * 0.22, 0.65 * a.x + 0.35 * b.x);
}
const mat2 kWaterRot = mat2(0.8, 0.6, -0.6, 0.8);
// Interactive waves (game::WaveField): returns xy the height gradient (world xy, central differences 1.5 cells apart),
// z the height and w its Laplacian (negative on crests); 0 outside the field (faded over its outer 4 %) or without one.
vec4 ouWaves(vec2 p) {
    if (w.rippleInfo.y < 0.5) return vec4(0.0);
    float inv = 1.0 / (w.ripple.z * w.rippleInfo.x);
    vec2 uv = (p - w.ripple.xy) * inv;
    vec2 inside = min(uv, 1.0 - uv);
    float fade = smoothstep(0.0, 0.04, min(inside.x, inside.y));
    if (fade == 0.0) return vec4(0.0);
    // Taps 1.5 cells out: the bilinear fetch averages two cells, so the cell-sized checkerboard bodies stamp into the
    // field cancels instead of dotting the curvature (crest light) with speckles.
    float d = 1.5 * w.ripple.z, du = d * inv;
    float h = textureLod(uRipples, uv, 0.0).r;
    float x0 = textureLod(uRipples, uv - vec2(du, 0.0), 0.0).r, x1 = textureLod(uRipples, uv + vec2(du, 0.0), 0.0).r;
    float y0 = textureLod(uRipples, uv - vec2(0.0, du), 0.0).r, y1 = textureLod(uRipples, uv + vec2(0.0, du), 0.0).r;
    return fade * vec4(vec2(x1 - x0, y1 - y0) / (2.0 * d), h, (x0 + x1 + y0 + y1 - 4.0 * h) / (d * d));
}
float lattice_hash(vec2 c) { return fract(sin(dot(mod(c, 289.0), vec2(12.9898, 78.233))) * 43758.5453); }
float value_noise(vec2 x) {
    vec2 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(lattice_hash(i), lattice_hash(i + vec2(1.0, 0.0)), f.x),
               mix(lattice_hash(i + vec2(0.0, 1.0)), lattice_hash(i + 1.0), f.x), f.y);
}
float d_ggx(float nh, float a) {
    float a2 = a * a, d = nh * nh * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}
float v_smith(float nv, float nl, float a) { // height-correlated Smith visibility
    float a2 = a * a;
    return 0.5 / max(nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2), 1e-5);
}

void main() {
    vec3 p = vWorld;
    float t = w.eye.w;
    // Two drifting normal layers (broad swell + finer ripples, crossing directions), world-anchored.
    vec2 la = p.xy * (1.0 / (900.0 * kScale)) + t * vec2(0.016, 0.009);
    vec2 lb = kWaterRot * p.xy * (1.0 / (260.0 * kScale)) + t * vec2(-0.030, 0.041);
    vec3 na = ouNormalLayer(la), nb = ouNormalLayer(lb);
    vec2 s = 0.4 * na.xy + 0.35 * (transpose(kWaterRot) * nb.xy);
    // Wave slopes tilt the normal about 1:1, softly capped towards 0.3: steeper tilts swing the reflection across
    // shore / sky edges into hard-edged blotches. The normal of a height field is (-dh/dx, -dh/dy, 1).
    vec4 wave = ouWaves(p.xy);
    wave.xy *= w.fogRange.z * 1.2 / (1.0 + 4.0 * length(wave.xy));
    s -= wave.xy;
    vec3 n = normalize(vec3(s, 1.0));
    vec3 v = normalize(w.eye.xyz - p);
    float nv = max(dot(n, v), 1e-3), dist = distance(w.eye.xyz, p);
    bool detail = w.detail.x > 0.5;

    // Bed under the surface: vertical depth (shoreline) and, along the refracted view, the path through the water.
    vec2 uv = gl_FragCoord.xy * w.screen.zw;
    float sd0 = textureLod(uDepth, uv, 0.0).r;
    bool open = sd0 >= 0.999999; // nothing behind the surface in the depth buffer: open, deep water
    float depth = open ? 1e5 : max(p.z - ouWorld(uv, sd0, w.invViewProj).z, 0.0);
    // The waves refract more than the swell and already over a shallow bed.
    float far = 1.0 + dist / (1500.0 * kScale);
    vec2 ruv = uv + s * (0.025 * smoothstep(0.0, 80.0 * kScale, depth) / far) -
               wave.xy * (0.015 * smoothstep(0.0, 8.0 * kScale, depth) / far);
    float sd = textureLod(uDepth, ruv, 0.0).r;
    vec3 bed = ouWorld(ruv, sd, w.invViewProj);
    if (bed.z > p.z) { // the offset sample is above the water (shore, a foreground object): look straight through
        ruv = uv, sd = sd0;
        bed = ouWorld(uv, sd, w.invViewProj);
    }
    vec3 below = toLinear(textureLod(uScene, ruv, 0.0).rgb);
    float path = sd >= 0.999999 ? 1e5 : distance(p, bed), bedDepth = sd >= 0.999999 ? 1e5 : max(p.z - bed.z, 0.0);

    // Absorption (red first: teal shallows) + in-scattered colour from teal to deep blue, lit like the scene.
    vec3 light = toLinear(min(w.ambient.rgb + w.sunColor.rgb * max(w.sunDir.z, 0.0), vec3(1.0)));
    vec3 tr = exp(-vec3(0.0080, 0.0026, 0.0019) / kScale * path);
    vec3 deep = vec3(0.006, 0.05, 0.11);
    vec3 scatter = mix(vec3(0.05, 0.32, 0.30), deep, smoothstep(20.0 * kScale, 260.0 * kScale, bedDepth)) * light;
    vec3 refr = below * tr + scatter * (1.0 - tr);
    vec3 c = refr;

    // Reflection (the scene along the mirror ray, else a sky colour from the fog and the sun) with Schlick Fresnel; sun
    // glints on the ripples. Both fade in over the first units of depth so the waterline stays soft.
    float edge = smoothstep(0.0, 10.0 * kScale, depth);
    vec3 r = reflect(-v, n);
    r.z = abs(r.z);
    r = normalize(r);
    vec3 env = mix(w.fogColor.rgb, w.fogColor.rgb * 1.2 + w.sunColor.rgb * 0.08, clamp(r.z * 2.0, 0.0, 1.0)) +
               w.sunColor.rgb * (0.12 * pow(max(dot(r, w.sunDir.xyz), 0.0), 12.0));
    vec4 hit = detail ? ouReflectDetail(p, r, env) : ouReflectFast(p, r, env);
    vec3 sky = toLinear(hit.rgb);
    float fresnel = 0.02 + 0.98 * pow(1.0 - nv, 5.0);
    c = mix(c, sky, clamp(fresnel * w.fogRange.w, 0.0, 1.0) * edge);
    // Stylised wave light: crests catch the sky, troughs darken (readable from above, where Fresnel is ~0.02). Driven
    // by the curvature, not the height: a high-pass, so whole-pool sloshing and a wake's raised wedge stay a tilt and
    // the fronts read as lines. (Joan's -100 x Laplacian in its units: the Laplacian is 1/kScale in ours.)
    vec3 fog = toLinear(w.fogColor.rgb);
    float bend = -100.0 * kScale * wave.w, crest = sign(bend) * smoothstep(0.4, 3.0, abs(bend)) * edge;
    c = mix(c, crest > 0.0 ? mix(sky, fog, 0.5) : c * 0.4, 0.6 * abs(crest));
    vec3 l = w.sunDir.xyz, hv = normalize(l + v);
    float nl = max(dot(n, l), 0.0), a = 0.07;
    float sunVis = detail && nl > 0.0 ? ouSunVisibility(p + n * 2.0) : 1.0;
    float spec = d_ggx(max(dot(n, hv), 0.0), a) * v_smith(nv, nl, a) * nl *
                 (0.02 + 0.98 * pow(1.0 - max(dot(v, hv), 0.0), 5.0)) * sunVis * w.sunDir.w;
    c += spec * toLinear(min(w.sunColor.rgb, vec3(1.0))) * edge;
    if (detail) // nearby lamps glint on the waves too (torches over the pool)
        for (int i = 0; i < 4; ++i) {
            if (w.lamps[i].w <= 0.0) continue;
            vec3 L = w.lamps[i].xyz - p;
            float ld = length(L), reach = clamp(1.0 - ld / w.lamps[i].w, 0.0, 1.0);
            if (reach <= 0.0) continue;
            L /= ld;
            vec3 lh = normalize(L + v);
            float lnl = max(dot(n, L), 0.0);
            c += toLinear(min(w.lampColor[i].rgb, vec3(1.0))) * (d_ggx(max(dot(n, lh), 0.0), 0.12) * v_smith(nv, lnl, 0.12) *
                 lnl * (0.02 + 0.98 * pow(1.0 - max(dot(v, lh), 0.0), 5.0)) * reach * reach * edge);
        }

    // Shoreline foam: a band over the shallow bed, broken into drifting lace by fine noise.
    float band = 1.0 - smoothstep(0.0, 40.0 * kScale, depth), pattern = 0.5 * (na.z + nb.z);
    float lace = 0.6 * value_noise(p.xy / (12.0 * kScale) + t * vec2(0.25, -0.15)) +
                 0.4 * value_noise(p.xy / (5.0 * kScale) - t * vec2(0.2, 0.3));
    float foam = band * smoothstep(0.5, 0.75, lace);
    // Wave foam: splash craters or the sharpest crests, broken up by the height pattern.
    float churn = max(smoothstep(4.0 * kScale, 7.0 * kScale, abs(wave.z)), smoothstep(0.12 / kScale, 0.25 / kScale, -wave.w));
    foam = max(foam, churn * smoothstep(0.1, 0.7, pattern));
    c = mix(c, light * 0.85, foam * 0.75 * smoothstep(0.0, 4.0 * kScale, depth));
    if (w.fogColor.w > 0.5) c = mix(below, deep * light, 0.55); // the surface seen from below

    vec3 color = toGamma(c);
    float fogT = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
    color = mix(color, w.fogColor.rgb, fogT);
    int view = int(w.detail.y + 0.5);
    if (view == 1) color = n * 0.5 + 0.5;                                                           // wave normals
    else if (view == 2) color = mix(vec3(1, 0, 1), vec3(0, 0.2, 1), clamp(path / 3000.0, 0.0, 1.0)); // mask + depth
    else if (view == 3) color = mix(vec3(1, 0.15, 0), vec3(0.1, 1, 0.2), hit.a);                    // SSR hit vs env
    else if (view == 4) color = vec3(clamp(spec, 0.0, 1.0), clamp(spec, 0.0, 1.0) * sunVis, 1.0 - sunVis); // glint
    else if (view == 5) color = vec3(fresnel);                                                       // Fresnel term
    oColor = vec4(color, 1.0);
}
