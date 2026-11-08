// Optional global tone mapping of the post pass (FrameOptions::tonemap == Agx; docs/RENDERING.md "Tone mapping"), shared by
// GL (kPostFs, embedded) and Vulkan (post.frag). Ported from NeuralSpecOcean's CinemaPostPass.js toneMapOklabCinema by
// Sheng Choi (docs/RESEARCH.md 8.10) with its constants unchanged; it cites Bjorn Ottosson (2020), "A perceptual color
// space for image processing" (Oklab) and Troy Sobotka's AgX (inset / outset working space + sigmoid). The sigmoid is the
// rational filmic fit the demo uses. Our scene is display-referred (TRU's shaders write sRGB-encoded colour, > 1 only on the
// hdr RGBA16F target), so it is decoded to linear and pre-exposed before the curve and encoded again after it.
//
// The ported code: Copyright (c) 2026 bgbg108555 and NeuralSpecOcean Contributors. MIT License:
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit
// persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright notice and
// this permission notice shall be included in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED
// "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// Bjorn Ottosson (2020): linear sRGB <-> Oklab.
vec3 ouLinearSrgbToOklab(vec3 c) {
    float l = 0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b;
    float m = 0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b;
    float s = 0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b;
    float l_ = pow(max(l, 0.0), 1.0 / 3.0);
    float m_ = pow(max(m, 0.0), 1.0 / 3.0);
    float s_ = pow(max(s, 0.0), 1.0 / 3.0);
    return vec3(0.2104542553 * l_ + 0.7936177850 * m_ - 0.0040720468 * s_,
                1.9779984951 * l_ - 2.4285922050 * m_ + 0.4505937099 * s_,
                0.0259040371 * l_ + 0.7827717662 * m_ - 0.8086757660 * s_);
}
vec3 ouOklabToLinearSrgb(vec3 lab) {
    float l_ = lab.x + 0.3963377774 * lab.y + 0.2158037573 * lab.z;
    float m_ = lab.x - 0.1055613458 * lab.y - 0.0638541728 * lab.z;
    float s_ = lab.x - 0.0894841775 * lab.y - 1.2914855480 * lab.z;
    float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
    return max(vec3(0.0), vec3(4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s,
                               -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s,
                               -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s));
}
// The demo's filmic sigmoid (toe + highlight shoulder).
vec3 ouAgxCurve(vec3 x) {
    vec3 a = x * (x + 0.0245786) - 0.000090537;
    vec3 b = x * (0.983729 * x + 0.4329510) + 0.238081;
    return clamp(a / b, 0.0, 1.0);
}
// Linear scene in, linear display out (0..1). 1.04 = the demo's exposure.
vec3 ouOklabAgx(vec3 hdr) {
    vec3 x = max(hdr, vec3(0.0)) * 1.04;
    // AgX-style working-space inset (keeps channels off the hard clip before the curve) and the outset back. Same GLSL
    // constructor order as the demo (column-major), so the same matrices.
    const mat3 inset = mat3(0.8566, 0.0951, 0.0483,
                            0.1373, 0.7612, 0.1015,
                            0.1119, 0.0768, 0.8113);
    const mat3 outset = mat3(1.1969, -0.1322, -0.0647,
                             -0.1906, 1.3585, -0.1679,
                             -0.1476, -0.0987, 1.2463);
    vec3 mapped = clamp(outset * ouAgxCurve(inset * x), 0.0, 1.0);
    // Inset gamut compression in Oklab: the mapped chroma, but 78% along the original signal's hue (constant hue: no
    // hue skew of saturated highlights, e.g. ACES' cyan -> purple).
    vec3 labOrig = ouLinearSrgbToOklab(min(x, vec3(8.0)));
    vec3 labMap = ouLinearSrgbToOklab(mapped);
    float origChroma = length(labOrig.yz), mapChroma = length(labMap.yz);
    vec2 hueDir = origChroma > 1e-5 ? labOrig.yz / origChroma : vec2(0.0);
    // Highlight path to white (chroma falls off above peak 1.25), mid-tone vibrance, gentle Oklab lightness S-curve.
    float peak = max(x.r, max(x.g, x.b));
    float highlightDesat = 1.0 / (1.0 + 0.24 * max(0.0, peak - 1.25));
    float midtoneVibrance = 1.0 + 0.14 * (1.0 - smoothstep(0.08, 0.56, mapChroma)) * smoothstep(0.04, 0.30, labMap.x);
    vec2 finalAB = mix(labMap.yz, hueDir * mapChroma, 0.78) * highlightDesat * midtoneVibrance;
    float Lc = clamp(labMap.x, 0.0, 1.0);
    float Lcontrast = clamp(Lc + 0.022 * sin((Lc - 0.48) * 3.14159265) * smoothstep(0.02, 0.22, Lc) *
                                     (1.0 - smoothstep(0.84, 0.99, Lc)), 0.0, 1.0);
    return clamp(ouOklabToLinearSrgb(vec3(Lcontrast, finalAB)), 0.0, 1.0);
}
vec3 ouSrgbToLinear(vec3 c) {
    c = max(c, vec3(0.0));
    return mix(pow((c + 0.055) / 1.055, vec3(2.4)), c / 12.92, step(c, vec3(0.04045)));
}
vec3 ouLinearToSrgb(vec3 c) {
    return mix(1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, c * 12.92, step(c, vec3(0.0031308)));
}
// Post-pass entry: the stored scene colour (sRGB-encoded) -> tone-mapped, sRGB-encoded. 1.372: pre-exposure so linear
// mid-grey 0.18 stays 0.18 through the curve (solved numerically) [I]: the LDR scene keeps its mid-tones, white rolls off to
// ~0.89 and shadows deepen.
vec3 ouTonemapAgx(vec3 c) { return ouLinearToSrgb(ouOklabAgx(ouSrgbToLinear(c) * 1.372)); }
