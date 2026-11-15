#version 460
// Enhanced water's animated water texture (`uAgWater`, docs/RENDERING.md "Under water"): a literal port of
// abyssal-garden's W_WATER `fs` (Noniv, MIT; source/src/shaders.js, docs/RESEARCH.md 8.11): r = g = caustic (caus(): five
// warped sine iterations), ba = surface slope (the WK sum of 10 travelling waves), tiling (repeat sampler). Rendered into
// mip 0 of the kAgWaterSize^2 RGBA16F texture every frame Enhanced water is drawn (render/Water.hpp agWaterSource), the
// mips then box-filtered down (the demo's fsMip). Fullscreen triangle (VK post.vert / GL kPostVs).
// Time = the demo's tm.x = agTime() (WaterBlock eye.w, seconds): VK push constant / GL uniform location 0.
#ifdef VULKAN
layout(push_constant) uniform AgWaterPush { float time; } pc;
#define OU_AG_TIME pc.time
#else
layout(location = 0) uniform float uAgTime;
#define OU_AG_TIME uAgTime
#endif
layout(location = 0) out vec4 oWater;
const float kAgWaterSize = 512.0; // shaders.js WATER (quality.js high); = render::kAgWaterSize
float caus(vec2 uv, float t) {
    vec2 p = uv * 6.2831853;
    vec2 i = p;
    float c = 0.0;
    for (int n = 1; n <= 5; n++) {
        float tt = t * (1.0 - 3.5 / float(n));
        i = p + vec2(cos(tt - i.x) + sin(tt + i.y), sin(tt - i.y) + cos(tt + i.x));
        float s = sin(i.x + tt), k = cos(i.y + tt);
        c += abs(s * k) / (1.25 * sqrt(s * s + k * k) + 1e-4);
    }
    c = 1.17 - pow(c / 5.0, 1.4);
    return pow(abs(c), 8.0);
}
const vec3 WK[10] = vec3[](vec3(1.0, 2.0, 1.0), vec3(-2.0, 1.0, 0.9), vec3(3.0, 1.0, 0.6), vec3(1.0, -4.0, 0.5),
                           vec3(4.0, 3.0, 0.4), vec3(-3.0, 5.0, 0.32), vec3(6.0, -2.0, 0.28), vec3(5.0, 7.0, 0.2),
                           vec3(-8.0, 3.0, 0.18), vec3(2.0, 11.0, 0.14));
void main() {
    vec2 uv = gl_FragCoord.xy / kAgWaterSize;
    float t = OU_AG_TIME;
    float c = caus(uv, t * 0.45 + 23.0);
    vec2 wu = uv + 0.02 * vec2(sin(6.2831853 * (uv.y * 2.0 + 0.1 * t)), cos(6.2831853 * (uv.x * 3.0 - 0.07 * t)));
    vec2 s = vec2(0.0);
    for (int i = 0; i < 10; i++) {
        vec3 k = WK[i];
        s += k.xy * k.z *
             cos(dot(k.xy, wu) * 6.2831853 + t * (0.5 + 0.35 * sqrt(length(k.xy))) * ((i & 1) == 0 ? 1.0 : -1.0) + float(i) * 1.7);
    }
    oWater = vec4(c, c, s * 0.06);
}
