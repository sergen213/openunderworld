// Enhanced under-water particles: lighting and fog of a point in the water (fragment shaders, after
// shaders/underwater.glsl, sun.glsl and shaders/atmosphere.glsl). The particles composite in the post pass after
// ouWaterPass has fogged the scene behind them (post.frag / GL kPostFsMain), so each fogs itself by its own distance
// with the same model (uwFogAlpha / uwFogColour mirror ouWaterPass for a point instead of a depth-buffer pixel).
// Everything is in the post pass's display-referred space (the water colours are gamma, as ouWaterPass mixes them).
// Surface z of the water volume holding p, or -1e30 outside every volume (the particles exist only in water).
float uwSurfaceAt(vec3 p) {
    for (int i = 0; i < int(w.detail.w); ++i) {
        vec3 lo = w.volumes[2 * i].xyz, hi = w.volumes[2 * i + 1].xyz;
        if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi))) return hi.z;
    }
    return -1e30;
}
// ouWaterPass's colour for a view direction: deep (looking down) to surface (looking up); a preset's hue.
vec3 uwFogColour(vec3 dir) {
    vec3 col = mix(w.waterPass[1].rgb, w.waterPass[0].rgb, clamp(0.5 + 0.5 * dir.z, 0.0, 1.0));
    return w.optics[0].w > 0.5 ? ouPresetHue(col) : col;
}
// ouWaterPass's alpha at p (per channel with a preset): how much of p's own light the water replaces on the way to the eye.
vec3 uwFogAlpha(vec3 p) {
    if (w.waterPass[2].x <= 0.0) return vec3(0.0);
    float viewZ = max((w.viewProj * vec4(p, 1.0)).w, 0.0);
    if (w.optics[0].w > 0.5)
        return clamp((1.0 - (1.0 - w.waterPass[0].w) * exp(-w.optics[0].rgb * viewZ)) * w.waterPass[2].x, 0.0, 1.0);
    float a = mix(w.waterPass[0].w, 1.0, clamp(viewZ / max(w.waterPass[1].w, 1.0), 0.0, 1.0)) * w.waterPass[2].x;
    return vec3(clamp(a, 0.0, 1.0));
}
// Downwelling attenuation per unit of depth: a preset's Kd (render::waterOptics [2]), else the unit's view extinction
// as ouShafts derives it from its surface colour and fog distance, halved (light from above is less scattered off the
// path than the view ray [I]).
vec3 uwKd() {
    if (w.optics[0].w > 0.5) return w.optics[2].rgb;
    bool unit = w.waterPass[1].w > 0.0;
    vec3 sf = unit ? w.waterPass[0].rgb : vec3(0.447, 0.780, 0.792);
    return 0.5 * (1.0 - 0.85 * pow(sf, vec3(2.2))) * (30.0 / (unit ? w.waterPass[1].w : 100000.0));
}
// Sunlight reaching p, surf - p.z below the surface: the refracted sun's path through the water x the sun shadow
// (abyssal-garden sunE x shadow1); zero without a sun.
vec3 uwSun(vec3 p, float surf) {
    if (w.sunColor.w <= 0.0 || w.sunDir.z < 0.05) return vec3(0.0);
    float l = -refract(-w.sunDir.xyz, vec3(0.0, 0.0, 1.0), 1.0 / 1.333).z; // the sun's elevation under water
    float h = max(surf - p.z, 0.0);
    return w.sunColor.rgb * exp(-uwKd() * (h / max(l, 0.05))) * ouSunShadowVisibility(p);
}
// The opaque scene in front of p at this pixel (the pass has no depth attachment; uDepth is the opaque depth copy).
bool uwOccluded(vec3 p) {
    float sd = texelFetch(uDepth, ivec2(gl_FragCoord.xy), 0).r;
    if (sd >= 0.999999) return false;
    vec3 s = ouWorld(gl_FragCoord.xy * w.screen.zw, sd, w.invViewProj);
    return dot(s - w.eye.xyz, s - w.eye.xyz) < dot(p - w.eye.xyz, p - w.eye.xyz);
}
