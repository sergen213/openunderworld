#version 460
// Enhanced under-water composite (docs/RENDERING.md "Under water"): abyssal-garden W_POST (Noniv, MIT) literally, once,
// on the linear HDR frame the post pass wrote with the camera submerged (fogged scene + caustics + particles; alpha = the
// linear view depth in metres, render/Water.hpp UwGrade): the half-resolution sun shafts (underwater.glsl agRaysBlur)
// added with W_POST's depth-aware upsample (4 bilinear taps, weights (bw + .02) / (.02 + |r.a - zf| / zf); only with
// v.x = 1: the shaft passes ran this frame), after the bloom chain read the frame as in the demo, then
// bloom c = mix(c, bl, 0.06) + 0.03 bl (uw_bloom.frag level 0), exposure g.mis.x
// (= 0.62 (0.6 + 0.4 sat((depth - 8) / 26)), main.js), vignette 1 - 0.55 |uv - 0.5|^2, tmap (the ACES fit), the
// luminance S-curve (55 % toward smoothstep), highlight desaturation (10 % above 0.55), 7 % overall + (0.008, 0.002, 0),
// gamma 1 / 2.2, a triangular interleaved-gradient-noise dither of +-1 / 255. Mixed over the plain display encoding of
// the frame by the pass strength (FrameOptions::underwater; 1 under water), then the screen fade.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform sampler2D uHdr;
layout(set = 1, binding = 0) uniform sampler2D uBloom;
layout(set = 2, binding = 7) uniform sampler2D uShafts; // the post-water set's blurred shafts (VkRenderer recordUnderwaterShafts)
layout(push_constant) uniform Push { vec4 v; } pc; // x shafts on, y exposure, z screen fade, w strength (uw_bloom.frag: x mode)
#define OU_V pc.v
#else
layout(binding = 0) uniform sampler2D uHdr;
layout(binding = 1) uniform sampler2D uBloom;
layout(binding = 14) uniform sampler2D uShafts; // the blurred shafts (GlRenderer: the shaft passes' unit)
layout(location = 0) uniform vec4 uV;
#define OU_V uV
#endif
layout(location = 0) out vec4 oColor;
float ign(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
vec3 tmap(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uHdr, 0));
    vec4 src = texelFetch(uHdr, ivec2(gl_FragCoord.xy), 0);
    vec3 hdr = src.rgb, c = hdr;
    if (OU_V.x > 0.5) { // depth-aware upsample of the half-res shafts
        float zf = max(src.a, 1e-6);
        vec2 h = gl_FragCoord.xy * 0.5 - 0.5, b = floor(h), f = h - b;
        ivec2 bi = ivec2(b), mx = textureSize(uShafts, 0) - 1;
        vec3 acc = vec3(0.0);
        float ws = 0.0;
        for (int k = 0; k < 4; ++k) {
            ivec2 o = ivec2(k & 1, k >> 1);
            vec4 r = texelFetch(uShafts, clamp(bi + o, ivec2(0), mx), 0);
            float bw = mix(1.0 - f.x, f.x, float(o.x)) * mix(1.0 - f.y, f.y, float(o.y));
            float wk = (bw + 0.02) / (0.02 + abs(r.a - zf) / zf);
            acc += r.rgb * wk, ws += wk;
        }
        c += acc / ws;
    }
    vec3 bl = textureLod(uBloom, uv, 0.0).rgb;
    c = mix(c, bl, 0.06) + bl * 0.03;
    c *= OU_V.y;
    vec2 q = uv - 0.5;
    c *= 1.0 - dot(q, q) * 0.55;
    c = tmap(c);
    float l0 = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float l = mix(l0, l0 * l0 * (3.0 - 2.0 * l0), 0.55);
    c *= l / max(l0, 1e-4);
    c = mix(c, vec3(l), smoothstep(0.55, 1.0, l) * 0.1);
    c = mix(vec3(l), c, 0.93) + vec3(0.008, 0.002, 0.0);
    c = pow(max(c, vec3(0.0)), vec3(1.0 / 2.2));
    c = mix(pow(clamp(hdr, 0.0, 1.0), vec3(1.0 / 2.2)), c, OU_V.w);
    float n = ign(gl_FragCoord.xy) + ign(gl_FragCoord.xy + 37.0) - 1.0;
    oColor = vec4((c + n / 255.0) * (1.0 - OU_V.z), 1.0);
}
