// Underwater post: distance fog into the water's tint + caustics on the submerged scene.
// Included by the post shaders after their own declarations; returns the input colour unchanged
// when the camera is not submerged, so Original output stays byte-identical.
#ifdef VULKAN
layout(set = 1, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 absorb; vec4 shallow;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[3]; uvec4 filterLut[640]; // original screen filters (shaders/filter.glsl)
} w;
#ifdef VULKAN
layout(set = 1, binding = 1) uniform sampler2D uDepth;
#define OU_NDC_Y(uv) (1.0 - 2.0 * (uv).y)
#define OU_NDC_Z(d) (d)
#else
layout(binding = 6) uniform sampler2D uDepth;
#define OU_NDC_Y(uv) (2.0 * (uv).y - 1.0)
#define OU_NDC_Z(d) (2.0 * (d) - 1.0)
#endif
vec3 ouWorld(vec2 uv, float depth, mat4 invViewProj) {
    vec4 clip = vec4(2.0 * uv.x - 1.0, OU_NDC_Y(uv), OU_NDC_Z(depth), 1.0);
    vec4 p = invViewProj * clip;
    return p.xyz / p.w;
}
float ouCaustic(vec2 p, float t) {
    float a = sin(p.x * 0.045 + t * 0.9) + sin(p.y * 0.052 - t * 0.7) + sin((p.x + p.y) * 0.031 + t * 0.5);
    float b = sin(p.x * 0.037 - t * 0.6) + sin(p.y * 0.041 + t * 0.8) + sin((p.x - p.y) * 0.043 - t * 0.4);
    return pow(1.0 - clamp(abs(a + b) * 0.25, 0.0, 1.0), 6.0);
}
vec3 ouUnderwater(vec3 c, vec2 uv) {
    if (w.fogColor.w < 0.5) return c;
    float d = texture(uDepth, uv).r;
    vec3 p = ouWorld(uv, d, w.invViewProj);
    float dist = d >= 0.999999 ? w.fogRange.y : length(p - w.eye.xyz);
    float f = clamp(1.0 - exp(-dist * 0.0009), 0.0, 0.92);
    vec3 col = mix(c, w.absorb.rgb, f);
    if (p.z < w.ripple.w)
        col += w.sunColor.rgb * (ouCaustic(p.xy, w.eye.w) * w.sunColor.w * (1.0 - f));
    return col;
}
