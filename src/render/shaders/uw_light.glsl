// Enhanced under-water particles: where a point in the water is visible (fragment shaders, after shaders/underwater.glsl).
// The light itself is shaders/uw_medium.glsl's literal abyssal-garden medium (agSunE, agAmb, agFogIn). The particles
// composite in the post pass after the fog (post.frag / GL kPostFsMain), so each fogs itself by its own distance.
// Surface z of the water volume holding p, or -1e30 outside every volume (the particles exist only in water: TRU's water
// is bounded volumes, the demo's is one open sea).
float uwSurfaceAt(vec3 p) {
    for (int i = 0; i < int(w.detail.w); ++i) {
        vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi))) return hi.z;
    }
    return -1e30;
}
// The opaque scene in front of p at this pixel (the pass has no depth attachment; uDepth is the opaque depth copy): the
// demo's depth test.
bool uwOccluded(vec3 p) {
    float sd = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd >= 0.999999) return false;
    vec3 s = ouWorld(gl_FragCoord.xy * w.screen.zw, sd, w.invViewProj);
    return dot(s - w.eye.xyz, s - w.eye.xyz) < dot(p - w.eye.xyz, p - w.eye.xyz);
}
