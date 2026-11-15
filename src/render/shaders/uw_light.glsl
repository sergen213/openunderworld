// Enhanced under-water particles: lighting and fog of a point in the water (fragment shaders, after
// shaders/underwater.glsl, sun.glsl and shaders/atmosphere.glsl). The particles composite in the post pass after
// ouUwFog has fogged the scene behind them (post.frag / GL kPostFsMain), so each fogs itself by its own distance with
// the same medium (shaders/uw_medium.glsl ouUwPointFog). Linear light, as the fog; the post pass composites in linear.
// Surface z of the water volume holding p, or -1e30 outside every volume (the particles exist only in water).
float uwSurfaceAt(vec3 p) {
    for (int i = 0; i < int(w.detail.w); ++i) {
        vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi))) return hi.z;
    }
    return -1e30;
}
// Sunlight reaching p, surf - p.z below the surface: the medium's sun term (ouUwSunLight, the refracted sun's path
// through the water) x the sun shadow (abyssal-garden sunE x shadow1); zero without a sun.
vec3 uwSun(OuUwMedium m, vec3 p, float surf) {
    if (w.sunColor.w <= 0.0 || w.sunDir.z < 0.05) return vec3(0.0);
    return w.sunColor.rgb * ouUwSunLight(m, max(surf - p.z, 0.0) / kM, ouUwSunDir()) * ouSunShadowVisibility(p);
}
// The water's own colour seen from p (h metres under the surface) along unit dir, linear: the medium's in-scatter up to
// the surface (rising) or over an unbounded path, as the fog shows looking that way from there.
vec3 uwWaterToward(OuUwMedium m, vec3 dir, float h) {
    return ouUwFogIn(m, dir, dir.z > 1e-4 ? min(h / dir.z, 1e4) : 1e4, h, ouUwSunDir());
}
// The opaque scene in front of p at this pixel (the pass has no depth attachment; uDepth is the opaque depth copy).
bool uwOccluded(vec3 p) {
    float sd = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd >= 0.999999) return false;
    vec3 s = ouWorld(gl_FragCoord.xy * w.screen.zw, sd, w.invViewProj);
    return dot(s - w.eye.xyz, s - w.eye.xyz) < dot(p - w.eye.xyz, p - w.eye.xyz);
}
