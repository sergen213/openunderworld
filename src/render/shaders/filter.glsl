// Original screen filters (render::ScreenFilter, docs/formats/EFFECTS.md "Screen filters"): tru.exe's PCColorDOFEffect
// and PCFinishEffect on the scene. Included after underwater.glsl (block `w`, uDepth, ouWorld). w.filters[0] = (alphaA,
// blurA, dof, colourMatrix), [1] = (tintA rgb, finishMatrix), [2] = (amountC, blurC, dofScale, dofBias); w.filterLut =
// 10 rows x 256 RGBA8 texels, 4 per uvec4. uFilterBlur: the half-resolution scene with its mip chain.
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
vec3 ouScreenFilter(vec3 c, vec2 uv) {
    vec4 a = w.filters[0], t = w.filters[1], f = w.filters[2];
    if (a.x <= 0.0 && a.w < 0.5 && f.x <= 0.0) return c;
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
    return c;
}
