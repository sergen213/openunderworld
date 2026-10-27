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

void main() {
    vec3 v = normalize(w.eye.xyz - vWorld);
    vec2 grad = ouWaveGrad(vWorld.xy, w.eye.w);
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
    vec3 trans = exp(-vec3(1.4, 0.55, 0.35) * (w.absorb.w * travel));
    refr = refr * trans + w.absorb.rgb * (1.0 - trans);
    refr += w.sunColor.rgb * (ouCaustic(bottom.xy, w.eye.w) * w.sunColor.w * trans *
                              clamp(1.0 - travel / 3000.0, 0.0, 1.0));

    float fresnel = 0.02 + 0.98 * pow(1.0 - clamp(dot(n, v), 0.0, 1.0), 5.0);
    vec3 refl = ouReflect(vWorld, n, v);
    vec3 color = mix(refr, refl, clamp(fresnel * w.fogRange.w, 0.0, 1.0));
    vec3 h = normalize(v + w.sunDir.xyz);
    color += w.sunColor.rgb * (pow(max(dot(n, h), 0.0), 220.0) * 1.6 * w.sunDir.w);
    float foam = (1.0 - smoothstep(0.0, w.shallow.w, thickness)) *
                 (0.55 + 0.45 * ouHash(floor(vWorld.xy / 23.0) + floor(w.eye.w * 3.0)));
    foam += smoothstep(6.0, 22.0, crest) * 0.5;
    color = mix(color, vec3(0.88, 0.95, 0.97), clamp(foam, 0.0, 1.0) * 0.75);
    if (w.fogColor.w > 0.5) color = mix(refr, w.absorb.rgb, 0.55); // the surface seen from below
    float dist = length(w.eye.xyz - vWorld);
    float fog = clamp((dist - w.fogRange.x) / max(1.0, w.fogRange.y - w.fogRange.x), 0.0, 1.0);
    color = mix(color, w.fogColor.rgb, fog);
    oColor = vec4(color, 1.0);
}
