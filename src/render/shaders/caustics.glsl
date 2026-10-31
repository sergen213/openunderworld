// Enhanced caustics, shared by the underwater post pass (underwater.glsl: camera under water) and the water surface
// (water.frag: the refracted bed seen from above). Embedded for GL by render/Water.cpp (expands the #include line),
// #included by glslc for Vulkan. Needs the block `w` (sunDir, sunColor, eye) and the includer's
// `float ouSunShadowVisibility(vec3 p)` (may be defined after this file). docs/RENDERING.md "Water".
// [I: tru.exe draws none; its under-water drawable is the sun glow and the full-screen pass]. Written for this
// project: an animated Voronoi border network (F2 - F1; bright where light focuses, brighter at the nodes), two
// domain-warped layers multiplied by their mean squared, evaluated where the refracted sun ray through a point entered
// the surface, so the pattern is anchored in the world and slides across slopes with the sun.
vec2 ouCausticHash(vec2 i) { return fract(sin(vec2(dot(i, vec2(127.1, 311.7)), dot(i, vec2(269.5, 183.3)))) * 43758.5453); }
float ouCausticLayer(vec2 x, float t, float width) {
    vec2 i = floor(x), f = x - i;
    float f1 = 8.0, f2 = 8.0;
    for (int y = -1; y <= 1; ++y)
        for (int k = -1; k <= 1; ++k) {
            vec2 g = vec2(k, y), h = ouCausticHash(i + g);
            float d = length(g + 0.5 + 0.38 * sin(t * (0.6 + 0.5 * h.yx) + 6.2831853 * h) - f);
            if (d < f1) f2 = f1, f1 = d; else f2 = min(f2, d);
        }
    return 1.0 - smoothstep(0.0, width, f2 - f1);
}
float ouCaustic(vec2 x, float t, float width) { // x in cells
    x += 0.35 * vec2(sin(x.y * 1.7 + t * 0.7) + 0.5 * sin(x.x * 2.9 - t * 0.9), sin(x.x * 1.5 - t * 0.6) + 0.5 * sin(x.y * 3.1 + t * 0.8));
    float a = ouCausticLayer(x + t * vec2(0.03, 0.02), t, width);
    float b = ouCausticLayer(mat2(0.8, 0.6, -0.6, 0.8) * x * 1.37 + 5.2, t * 1.3, width * 1.2);
    return (0.5 * a + 0.5 * b) * (0.5 * a + 0.5 * b);
}
float ouSunShadowVisibility(vec3 p);
// Caustic light factor for the opaque point p lying h units under its water surface, n its (unnormalised) surface
// normal, footprint world units per pixel there. Fades with depth toward half strength (and defocuses: wider cells,
// softer lines, a slight red/blue split; physically caustics vanish within ~10 m, but TRU's sea floors lie 100+ m
// down), with the normal facing the refracted sun, in the sun shadow map, and where cells shrink under ~2 pixels (no
// shimmer). Multiply the lit colour by it: about mean-preserving (lines up to +170%, gaps -30% at full strength).
// sunColor.w = strength (0: no sun).
vec3 ouCausticLight(vec3 p, vec3 n, float h, float footprint) {
    if (w.sunColor.w <= 0.0 || h <= 0.0 || w.sunDir.z < 0.05 || dot(n, n) < 1e-12) return vec3(1.0);
    vec3 l = -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / 1.333); // toward the sun, under water
    n = normalize(n);
    if (dot(n, w.eye.xyz - p) < 0.0) n = -n;
    float defocus = 1.0 - exp(-h / 3000.0), cell = 180.0 * (1.0 + defocus); // cell size, world units
    vec2 s = (p.xy + l.xy * (h / l.z)) / cell; // where that ray entered the surface, in cells
    float cells = footprint / cell;
    float width = mix(0.22, 0.45, defocus) + 1.5 * cells;
    float amount = w.sunColor.w * (0.5 + 0.5 * exp(-h / 3000.0)) * smoothstep(0.15, 0.7, dot(n, l)) * (1.0 - smoothstep(0.2, 0.6, cells));
    if (amount <= 0.002) return vec3(1.0);
    amount *= ouSunShadowVisibility(p);
    vec2 split = normalize(l.xy + 1e-4) * (0.03 * defocus);
    float t = w.eye.w;
    vec3 k = vec3(ouCaustic(s - split, t, width), ouCaustic(s, t, width), ouCaustic(s + split, t, width));
    vec3 tint = w.sunColor.rgb / max(max(w.sunColor.r, w.sunColor.g), max(w.sunColor.b, 1e-3)); // the sun's hue
    return 1.0 + amount * tint * (2.0 * k - 0.3);
}
