// Original screen filters (render::ScreenFilter, docs/formats/EFFECTS.md "Screen filters"): tru.exe's PCColorDOFEffect,
// PCFinishEffect, PCFeedbackEffect, PCNoiseEffect and the plain tints on the scene. Included after underwater.glsl
// (block `w`, uDepth, ouWorld). w.filters[0] = (alphaA, blurA, dof, colourMatrix), [1] = (tintA rgb, finishMatrix),
// [2] = (amountC, blurC, dofScale, dofBias), [3] = additive tint rgb, [4] = alpha tint (rgb, k), [5] = feedback (keep,
// amount, blur, zoom x), [6] = feedback (rgb, angle), [7] = (zoom y, noise alpha, noise cells, 0), [8] = noise (rgb,
// seed); w.filterLut = 10 rows x 256 RGBA8 texels, 4 per uvec4. uFilterBlur: the half-resolution scene with its mips.
#ifdef VULKAN
layout(set = 1, binding = 2) uniform sampler2D uFilterBlur;
#else
layout(binding = 7) uniform sampler2D uFilterBlur;
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
vec3 ouFilterBlur(vec2 uv, float r) {
    vec2 t = 0.5 * exp2(r) / vec2(textureSize(uFilterBlur, 0));
    return 0.25 * (textureLod(uFilterBlur, uv + vec2(-t.x, -t.y), r).rgb + textureLod(uFilterBlur, uv + vec2(t.x, -t.y), r).rgb +
                   textureLod(uFilterBlur, uv + vec2(-t.x, t.y), r).rgb + textureLod(uFilterBlur, uv + vec2(t.x, t.y), r).rgb);
}
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
    if (fb.y > 0.0 || fb.x < 1.0) { // 0x51A530: scene x keep, + the blurred frame zoomed / turned about the centre x colour
        // x amount (blend 5, additive). The exe feeds back the previous frame; this one's blur stands in [I].
        float s = sin(fc.w), co = cos(fc.w);
        vec2 d = (uv - 0.5) / vec2(max(fb.w, 0.01), max(w.filters[7].x, 0.01));
        vec2 q = 0.5 + vec2(co * d.x - s * d.y, s * d.x + co * d.y);
        c = c * fb.x + ouFilterBlur(clamp(q, 0.0, 1.0), fb.z) * fc.rgb * fb.y;
    }
    c += w.filters[3].rgb; // tints (0x52B860): additive (alpha 0x80) and alpha-blended
    c = mix(c, w.filters[4].rgb, w.filters[4].w);
    return ouScreenNoise(c, uv);
}
