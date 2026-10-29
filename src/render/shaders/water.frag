#version 460
// Enhanced water surface: procedural waves + game ripple field, screen-space refraction with
// Beer-Lambert absorption, shore/crest foam, screen-space reflections, sun glints, caustics.
// Written for OpenUnderworld (no third-party shader code). docs/RENDERING.md "Water".
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 absorb; vec4 shallow;
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
vec3 ouWorld(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clip = vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    vec4 p = invViewProj * clip;
    return p.xyz / p.w;
}
vec2 ouWaveGrad(vec2 p, float t) { // four directional waves, analytic slopes [I amplitudes]
    vec2 g = vec2(0);
    vec2 d0 = normalize(vec2(1.0, 0.3)), d1 = normalize(vec2(-0.4, 1.0));
    vec2 d2 = normalize(vec2(0.7, -0.6)), d3 = normalize(vec2(-1.0, -0.2));
    float f = dot(p, d0) * 0.020 + t * 1.1; g += d0 * (cos(f) * 0.020 * 0.55);
    f = dot(p, d1) * 0.031 - t * 0.8; g += d1 * (cos(f) * 0.031 * 0.35);
    f = dot(p, d2) * 0.067 + t * 1.7; g += d2 * (cos(f) * 0.067 * 0.22);
    f = dot(p, d3) * 0.113 - t * 2.3; g += d3 * (cos(f) * 0.113 * 0.12);
    return g;
}
float ouCaustic(vec2 p, float t) { // animated ridged filaments [I]
    float a = sin(p.x * 0.045 + t * 0.9) + sin(p.y * 0.052 - t * 0.7) + sin((p.x + p.y) * 0.031 + t * 0.5);
    float b = sin(p.x * 0.037 - t * 0.6) + sin(p.y * 0.041 + t * 0.8) + sin((p.x - p.y) * 0.043 - t * 0.4);
    return pow(1.0 - clamp(abs(a + b) * 0.25, 0.0, 1.0), 6.0);
}
float ouHash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
layout(location = 0) in vec3 vWorld;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUv;
layout(location = 3) in vec4 vColor;
layout(location = 0) out vec4 oColor;

vec2 ouScreenUv(vec3 q, out bool inside) {
    vec4 clip = w.viewProj * vec4(q, 1.0);
    vec2 uv = vec2(clip.x / max(clip.w, 1e-4) * 0.5 + 0.5, OU_UV_Y(clip.y / max(clip.w, 1e-4)));
    inside = clip.w > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)));
    return uv;
}

vec3 ouReflect(vec3 p, vec3 n, vec3 v) {
    vec3 r = reflect(-v, n);
    if (r.z < 0.02) return w.fogColor.rgb;
    vec3 q = p + n * 3.0;
    float stepLen = 90.0;
    for (int i = 0; i < 20; ++i) {
        q += r * stepLen;
        stepLen *= 1.18;
        vec4 clip = w.viewProj * vec4(q, 1.0);
        if (clip.w <= 0.0) break;
        vec2 uv = vec2(clip.x / clip.w * 0.5 + 0.5, OU_UV_Y(clip.y / clip.w));
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;
        float sd = texture(uDepth, uv).r;
        if (sd >= 0.999999) continue;
        vec3 sp = ouWorld(uv, sd, w.invViewProj);
        float dq = length(q - w.eye.xyz), ds = length(sp - w.eye.xyz);
        if (ds < dq && dq - ds < 500.0) return texture(uScene, uv).rgb;
    }
    return w.fogColor.rgb;
}

// ---- water detail (FrameOptions::waterDetail) ----
// Lattice value noise with its analytic gradient (smoothstep interpolation): (value, d/dx, d/dy).
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
// Swell (two broad layers drifting apart) and a flowing detail layer warped by the swell and counter-rotated
// against a second one, so the surface reads as a current, not a lattice. Slopes in world units; `crestNoise` 0..1
// drives foam and glitter. Amplitudes [I].
vec2 ouDetailGrad(vec2 p, float t, out float crestNoise) {
    const mat2 M = mat2(0.8, 0.6, -0.6, 0.8);
    vec3 s1 = ouNoiseD(p / 900.0 + vec2(t * 0.020, -t * 0.013));
    vec3 s2 = ouNoiseD(M * p / 650.0 - vec2(t * 0.017, t * 0.011) + 13.7);
    float ang = t * 0.03;
    mat2 R = mat2(cos(ang), sin(ang), -sin(ang), cos(ang));
    vec2 q = p / 140.0 + (s1.yz + s2.yz) * 1.4;
    vec3 d1 = ouNoiseD(q + vec2(t * 0.11, t * 0.07));
    vec3 d2 = ouNoiseD(R * q * 1.7 - vec2(t * 0.09, -t * 0.05) + 41.0);
    crestNoise = clamp(d1.x * 0.5 + d2.x * 0.3 + s1.x * 0.2, 0.0, 1.0);
    float filt = 1.0 / (1.0 + 16.0 * dot(fwidth(q), fwidth(q))); // fade the detail where it would alias
    vec2 swell = (s1.yz / 900.0 + transpose(M) * s2.yz / 650.0) * 38.0;
    vec2 flow = (d1.yz + transpose(R) * d2.yz * 1.7 * 0.6) / 140.0 * 5.0 * filt;
    return swell + flow;
}
float ouEdgeFade(vec2 uv) { vec2 e = min(uv, 1.0 - uv); return clamp(min(e.x, e.y) * 10.0, 0.0, 1.0); }
// Thickness-tested screen-space reflection: the ray only hits where it passes behind the depth buffer by less than a
// thickness growing with distance; a short bisection refines the hit; confidence fades at the screen border and with
// distance, and the rest falls back to an environment colour (fog + a sun-tinted sky lobe). rgb + confidence.
vec4 ouReflectDetail(vec3 p, vec3 n, vec3 v, vec3 env) {
    vec3 r = reflect(-v, n);
    if (r.z < 0.0) r.z = -r.z * 0.3; // grazing waves: keep reflecting the far shore, not the water itself
    r = normalize(r);
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
            float thick = 40.0 + 0.1 * t;
            if (diff > 0.0 && diff < thick) {
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
// Screen-space sun occlusion of the glint: march toward the sun through the opaque depth (8 steps).
float ouSunVisibility(vec3 p) {
    float vis = 1.0;
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
        if (diff > 10.0 && diff < 80.0 + 0.2 * t) vis *= 0.0;
    }
    return vis;
}

void main() {
    vec3 v = normalize(w.eye.xyz - vWorld);
    bool detail = w.detail.x > 0.5;
    vec2 grad = ouWaveGrad(vWorld.xy, w.eye.w);
    float crestNoise = 0.0;
    vec2 detailGrad = ouDetailGrad(vWorld.xy, w.eye.w, crestNoise); // outside the branch: it uses fwidth
    if (detail) grad += detailGrad;
    float crest = 0.0;
    if (w.rippleInfo.y > 0.5) {
        vec2 ruv = (vWorld.xy - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
        if (all(greaterThan(ruv, vec2(0.001))) && all(lessThan(ruv, vec2(0.999))) && abs(vWorld.z - w.ripple.w) < 200.0) {
            float texel = 1.0 / w.rippleInfo.x;
            float hL = texture(uRipples, ruv - vec2(texel, 0.0)).r, hR = texture(uRipples, ruv + vec2(texel, 0.0)).r;
            float hD = texture(uRipples, ruv - vec2(0.0, texel)).r, hU = texture(uRipples, ruv + vec2(0.0, texel)).r;
            grad += vec2(hR - hL, hU - hD) / (2.0 * w.ripple.z) * w.fogRange.z;
            crest = texture(uRipples, ruv).r;
        }
    }
    vec3 n = normalize(vec3(-grad, 1.0));
    if (dot(n, vNormal) < 0.0) n = -n; // keep the surface's own orientation

    // Refraction of the opaque scene, with foreground objects kept out of the distortion.
    vec2 uv = gl_FragCoord.xy * w.screen.zw;
    vec2 ruv2 = uv + n.xy * 0.035;
    float sd = texture(uDepth, ruv2).r;
    vec3 bottom = ouWorld(ruv2, sd, w.invViewProj);
    if (bottom.z > vWorld.z + 1.0) {
        ruv2 = uv;
        sd = texture(uDepth, uv).r;
        bottom = ouWorld(uv, sd, w.invViewProj);
    }
    vec3 refr = texture(uScene, ruv2).rgb;
    float thickness = max(0.0, vWorld.z - bottom.z);
    float travel = max(thickness, 0.5 * length(vWorld - bottom));
    if (detail && sd >= 0.999999) travel = 1e5; // no floor in the depth buffer: open, deep water, not sky colour
    vec3 trans = exp(-vec3(1.4, 0.55, 0.35) * (w.absorb.w * travel));
    refr = refr * trans + w.absorb.rgb * (1.0 - trans);
    refr += w.sunColor.rgb * (ouCaustic(bottom.xy, w.eye.w) * w.sunColor.w * trans *
                              clamp(1.0 - travel / 3000.0, 0.0, 1.0));

    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
    vec3 h = normalize(v + w.sunDir.xyz);
    float ndh = max(dot(n, h), 0.0);
    vec3 refl;
    float hitConf = 1.0, glint, sunVis = 1.0;
    if (detail) {
        vec3 r = reflect(-v, n);
        vec3 env = mix(w.fogColor.rgb, w.fogColor.rgb * 1.2 + w.sunColor.rgb * 0.08, clamp(r.z * 2.0, 0.0, 1.0)) +
                   w.sunColor.rgb * (0.12 * pow(max(dot(r, w.sunDir.xyz), 0.0), 12.0));
        vec4 hit = ouReflectDetail(vWorld, n, v, env);
        refl = hit.rgb;
        hitConf = hit.a;
        // Sun glint: a broad sheen plus a sharp sparkle whose strength varies with the wave noise, broken into a
        // glitter path, and hidden where the depth buffer blocks the sun.
        float glitter = 0.25 + 2.2 * pow(crestNoise, 3.0);
        float path = 0.15 + 1.1 * smoothstep(0.45, 0.85, crestNoise);
        sunVis = ndh > 0.9 ? ouSunVisibility(vWorld + n * 2.0) : 1.0;
        glint = (0.1 * pow(ndh, 60.0) + pow(ndh, 700.0) * glitter * 4.0 + pow(ndh, 160.0) * path) * sunVis *
                (0.5 + fresnel);
    } else {
        refl = ouReflect(vWorld, n, v);
        glint = pow(ndh, 220.0) * 1.6;
    }
    vec3 color = mix(refr, refl, clamp(fresnel * w.fogRange.w, 0.0, 1.0));
    color += w.sunColor.rgb * (glint * w.sunDir.w);
    if (detail) // nearby lamps reflect directly on the wave normal (torches over the pool)
        for (int i = 0; i < 4; ++i) {
            if (w.lamps[i].w <= 0.0) continue;
            vec3 L = w.lamps[i].xyz - vWorld;
            float dist = length(L);
            float reach = clamp(1.0 - dist / w.lamps[i].w, 0.0, 1.0);
            if (reach <= 0.0) continue;
            L /= dist;
            float spec = pow(max(dot(n, normalize(L + v)), 0.0), 90.0) * max(dot(n, L), 0.0);
            color += w.lampColor[i].rgb * (spec * (0.04 + 0.96 * pow(1.0 - max(dot(n, v), 0.0), 5.0)) * reach * reach * 3.0);
        }
    float foam = (1.0 - smoothstep(0.0, w.shallow.w, thickness)) *
                 (0.55 + 0.45 * ouHash(floor(vWorld.xy / 23.0) + floor(w.eye.w * 3.0)));
    foam += smoothstep(6.0, 22.0, crest) * 0.5;
    if (detail) foam += smoothstep(0.78, 0.95, crestNoise) * 0.35 * smoothstep(0.0, 1.0, length(detailGrad) * 8.0);
    color = mix(color, vec3(0.88, 0.95, 0.97), clamp(foam, 0.0, 1.0) * 0.75);
    if (w.fogColor.w > 0.5) color = mix(refr, w.absorb.rgb, 0.55); // the surface seen from below
    float dist = length(w.eye.xyz - vWorld);
    float fog = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
    color = mix(color, w.fogColor.rgb, fog);
    int view = int(w.detail.y + 0.5);
    if (view == 1) color = n * 0.5 + 0.5;                                                     // wave normals
    else if (view == 2) color = mix(vec3(1, 0, 1), vec3(0, 0.2, 1), clamp(travel / 3000.0, 0.0, 1.0)); // mask + depth
    else if (view == 3) color = mix(vec3(1, 0.15, 0), vec3(0.1, 1, 0.2), hitConf);           // SSR hit vs environment
    else if (view == 4) color = vec3(clamp(glint, 0.0, 1.0), clamp(glint, 0.0, 1.0) * sunVis, 1.0 - sunVis); // glint
    else if (view == 5) color = vec3(fresnel);                                                 // Fresnel term
    oColor = vec4(color, 1.0);
}
