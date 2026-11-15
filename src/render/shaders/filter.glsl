// Original screen filters (render::ScreenFilter, docs/formats/EFFECTS.md "Screen filters"): tru.exe's PCColorDOFEffect,
// PCFinishEffect, PCFeedbackEffect, PCNoiseEffect and the plain tints on the scene. Included after underwater.glsl
// (block `w`, uDepth, ouWorld). w.filters[0] = (alphaA, blurA, dof, colourMatrix), [1] = (tintA rgb, finishMatrix),
// [2] = (amountC, blurC, dofScale, dofBias), [3] = additive tint rgb, [4] = alpha tint (rgb, k), [5] = feedback (keep,
// amount, blur, zoom x), [6] = feedback (rgb, angle), [7] = (zoom y, noise alpha, noise cells, previous frame copied),
// [8] = noise (rgb, seed); w.filterLut = 10 rows x 256 RGBA8 texels, 4 per uvec4. uFilterBlur: the half-resolution scene
// with its mips; uFeedback: the previous frame's final image, half resolution with mips (the renderer's feedback copy).
#ifdef VULKAN
layout(set = 1, binding = 2) uniform sampler2D uFilterBlur;
layout(set = 1, binding = 4) uniform sampler2D uFeedback;
#else
layout(binding = 7) uniform sampler2D uFilterBlur;
layout(binding = 11) uniform sampler2D uFeedback;
#endif
vec3 ouLutTexel(int row, int x) {
    int i = row * 256 + x;
    uint v = w.filterLut[i >> 2][i & 3];
    return vec3(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu)) / 255.0;
}
vec3 ouLut(int row, float v) { // linear between the 256 entries
    float x = clamp(v, 0.0, 1.0) * 255.0;
    int i = min(int(x), 254);
    return mix(ouLutTexel(row, i), ouLutTexel(row, i + 1), x - float(i));
}
// Matrix m of curves: input channel c's row adds its RGB contributions.
vec3 ouFilterMatrix(int m, vec3 c) { return ouLut(3 * m, c.r) + ouLut(3 * m + 1, c.g) + ouLut(3 * m + 2, c.b); }
// tru.exe 0x519F20: down to level ceil(r), back up with 4-tap bilinear steps, the fraction blended = mip level r here.
vec3 ouMipBlur(sampler2D s, vec2 uv, float r) {
    vec2 t = 0.5 * exp2(r) / vec2(textureSize(s, 0));
    return 0.25 * (textureLod(s, uv + vec2(-t.x, -t.y), r).rgb + textureLod(s, uv + vec2(t.x, -t.y), r).rgb +
                   textureLod(s, uv + vec2(-t.x, t.y), r).rgb + textureLod(s, uv + vec2(t.x, t.y), r).rgb);
}
vec3 ouFilterBlur(vec2 uv, float r) { return ouMipBlur(uFilterBlur, uv, r); }
// 0x51B1D0: a 10 x 8 grid of quads over the screen, each with a random offset into the noise texture (cells / 64 of it),
// colour x alpha, re-randomised every frame. The noise texture is the device's [U]: a hash here.
float ouNoiseHash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }
vec3 ouScreenNoise(vec3 c, vec2 uv) {
    vec4 n = w.filters[8];
    float a = w.filters[7].y;
    if (a <= 0.0) return c;
    vec2 g = uv * vec2(10.0, 8.0), cell = floor(g);
    vec2 o = floor(vec2(ouNoiseHash(cell + n.w), ouNoiseHash(cell + n.w + 17.0)) * 4096.0);
    vec2 texel = floor(fract(g) * max(w.filters[7].z, 1.0) / 64.0 * 256.0) + o;
    return mix(c, n.rgb * ouNoiseHash(texel), a); // blend mode 4: alpha [I]
}
vec3 ouScreenFilter(vec3 c, vec2 uv) {
    vec4 a = w.filters[0], t = w.filters[1], f = w.filters[2], fb = w.filters[5], fc = w.filters[6];
    if (a.w > 0.5) c = ouFilterMatrix(0, c); // 0x51A0C0: the scene through matrix 1
    if (a.x > 0.0) { // 0x5177E0: the blurred scene through matrix 2, tinted, blended over (by the DOF curve)
        float k = a.x;
        if (a.z > 0.5) k *= ouLut(9, length(ouWorld(uv, texture(uDepth, uv).r, w.invViewProj) - w.eye.xyz) * f.z + f.w).r;
        c = mix(c, ouFilterMatrix(1, ouFilterBlur(uv, a.y)) * t.rgb, k);
    }
    if (f.x > 0.0) { // 0x51AEF0: the blurred scene (through the finish matrix) added
        vec3 b = ouFilterBlur(uv, f.y);
        c += (t.w > 0.5 ? ouFilterMatrix(2, b) : b) * f.x;
    }
    if ((fb.y > 0.0 || fb.x < 1.0) && w.filters[7].w > 0.5) { // 0x51A530: scene x keep, + the previous frame (copied
        // half size at the end of each frame the effect ran, 0x509630 -> 0x51A2E0) blurred, zoomed / turned about the
        // centre x colour x amount (blend 5, additive). Nothing on the first frame: no copy yet (+0x3D).
        float s = sin(fc.w), co = cos(fc.w);
        vec2 d = (uv - 0.5) / vec2(max(fb.w, 0.01), max(w.filters[7].x, 0.01));
        vec2 q = 0.5 + vec2(co * d.x - s * d.y, s * d.x + co * d.y);
        c = c * fb.x + ouMipBlur(uFeedback, clamp(q, 0.0, 1.0), min(fb.z, 4.0)) * fc.rgb * fb.y; // blur <= 4 (0x519F20)
    }
    c += w.filters[3].rgb; // tints (0x52B860): additive (alpha 0x80) and alpha-blended
    c = mix(c, w.filters[4].rgb, w.filters[4].w);
    return ouScreenNoise(c, uv);
}
// Enhanced under-water grade (ours, camera submerged; abyssal-garden's W_POST, Noniv, MIT: docs/RESEARCH.md; RENDERING.md
// "Under water"), last in the post pass. Glow: the screen-filter blur chain (uFilterBlur; the renderers build it while
// submerged too) at mips 3 and 5, each of the 4 bilinear taps Karis-weighted 1 / (1 + max channel) so a lone bright
// pixel cannot bloom into a square (abyssal-garden's fsDown0 `kar`, firefly-safe), then fogged with this pixel's medium
// (underwater.glsl gUwT / gUwL: the blurred scene is unfogged) and mixed as the demo, 6 % + 3 % added. Then the vignette
// 1 - 0.55 r^2, the luminance S-curve (55 % toward smoothstep), 10 % desaturation of the highlights and 7 % overall, and
// a triangular interleaved-gradient-noise dither of +-1 / 255 before 8-bit quantisation (no bands in the fog gradients).
// All x the pass strength. Original water never sets the submerged flag: c unchanged.
vec3 ouUwKaris(vec2 uv, float r) {
    vec2 t = 0.5 * exp2(r) / vec2(textureSize(uFilterBlur, 0));
    vec3 s = vec3(0.0);
    float ws = 0.0;
    for (int i = 0; i < 4; ++i) {
        vec2 o = vec2((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0);
        vec3 v = min(pow(max(textureLod(uFilterBlur, uv + t * o, r).rgb, vec3(0.0)), vec3(2.2)), vec3(24.0));
        float k = 1.0 / (1.0 + max(v.r, max(v.g, v.b)));
        s += v * k, ws += k;
    }
    return s / ws;
}
float ouUwIgn(vec2 p) { return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715)))); }
vec3 ouUnderwaterGrade(vec3 c, vec2 uv) {
    if (w.fogColor.w < 0.5 || w.waterPass[2].x <= 0.0) return c;
    vec3 lin = pow(max(c, vec3(0.0)), vec3(2.2));
    vec3 bl = 0.5 * (ouUwKaris(uv, 3.0) + ouUwKaris(uv, 5.0)) * gUwT + gUwL;
    vec3 g = mix(lin, bl, 0.06) + bl * 0.03;
    vec2 q = uv - 0.5;
    g *= 1.0 - dot(q, q) * 0.55;
    float l0 = dot(g, vec3(0.2126, 0.7152, 0.0722));
    float lc = clamp(l0, 0.0, 1.0), l = mix(lc, lc * lc * (3.0 - 2.0 * lc), 0.55);
    g *= l / max(l0, 1e-4);
    g = mix(g, vec3(l), smoothstep(0.55, 1.0, l) * 0.1);
    g = mix(vec3(l), g, 0.93);
    g = mix(c, pow(max(g, vec3(0.0)), vec3(1.0 / 2.2)), w.waterPass[2].x);
    return g + (ouUwIgn(gl_FragCoord.xy) + ouUwIgn(gl_FragCoord.xy + 37.0) - 1.0) / 255.0;
}
