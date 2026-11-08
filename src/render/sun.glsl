// Sun visibility shared by the generic scene shaders (GL kSceneFs, vk/shaders/scene.frag) and the translated original
// pixel shaders (ShaderTranslate: SunLight colour c212 * ouSunVis, OU_SUN hooks). docs/RENDERING.md "Sun shadows".
// Embedded into C++ as sun_glsl.inc (CMake) and #included by glslc. Vulkan: define OU_SHADOW_SET (and OU_RT_SET with
// OU_RT, after the ray-query extensions) before this; OpenGL: OuShadow is UBO binding 3, the shadow map texture unit 16.
#ifdef OU_SHADOW_SET
#define OU_SHADOW_UBO layout(std140, set = OU_SHADOW_SET, binding = 1)
#define OU_SHADOW_TEX layout(set = OU_SHADOW_SET, binding = 0)
#else
#define OU_SHADOW_UBO layout(std140, binding = 3)
#define OU_SHADOW_TEX layout(binding = 16)
#endif
// render::SunShadowBlock (RenderCommon.hpp).
OU_SHADOW_UBO uniform OuShadow {
    mat4 cascade[3];  // world -> cascade tile: xy tile uv, z depth, all in [0, 1]
    vec4 rayBase, rayU, rayV; // fragment world = eye + (rayBase + u rayU + v rayV) / gl_FragCoord.w, uv = fragCoord / size
    vec4 sun;         // xyz unit vector toward the sun; w = 1: sun shadows on (map or rays)
    vec4 eye;         // xyz camera position; w = shadow rays per pixel
    vec4 screen;      // 1 / width, 1 / height, ray tracing (0 off, 1 shadows, 2 + water reflections), unused
    uvec4 lights;     // local-light shadow rays: x / y shadow-light bits (light table 0-31 / 32-63), z rays per light (0 off)
} ouShadow;
OU_SHADOW_TEX uniform sampler2DShadow ouShadowMap; // atlas: the 3 cascades side by side

// 3x3 PCF in the finest cascade whose tile holds p.
float ouShadowMapVisibility(vec3 p) {
    for (int c = 0; c < 3; ++c) {
        vec3 q = (ouShadow.cascade[c] * vec4(p, 1.0)).xyz;
        if (any(lessThan(q, vec3(0.002, 0.002, 0.0))) || any(greaterThan(q, vec3(0.998, 0.998, 1.0)))) continue;
        vec2 texel = 1.0 / vec2(textureSize(ouShadowMap, 0));
        vec2 uv = vec2((float(c) + q.x) / 3.0, q.y);
        float s = 0.0;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) s += texture(ouShadowMap, vec3(uv + vec2(x, y) * texel, q.z));
        return s / 9.0;
    }
    return 1.0;
}

#ifdef OU_RT
layout(set = OU_RT_SET, binding = 0) uniform accelerationStructureEXT ouTlas;
// One record per BLAS geometry (= traced submesh), indexed by instance custom index + geometry index. `transform`: the
// geometry's own model (VkTransformMatrixKHR rows) in the merged static BLASes (VkRenderer::buildRayTracing), else 0.
struct OuGeom { uint64_t vertices; uint64_t indices; uint firstIndex; uint texture; float alphaRef; uint pad; uint64_t transform; };
layout(set = OU_RT_SET, binding = 1, std430) readonly buffer OuGeoms { OuGeom ouGeoms[]; };
layout(set = OU_RT_SET, binding = 2) uniform sampler2D ouRtTextures[];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer OuWords { uint w[]; };
const uint kOuVertexWords = 18u; // scene::Vertex: position 0-2, normal 3-5, uv 6-7, RGBA8 colour 8, original-shader inputs 9-17
// Instance masks (VkRenderer::buildRayTracing): 1 solid (reflection rays: not water, not shadow-only), 2 water (water never
// reflects itself), 4 sun-shadow casters (shadow rays; SubMesh::castsShadow).
const uint kOuMaskSolid = 1u, kOuMaskShadow = 4u;
const float kOuSunTan = 0.012;   // sun disc half-angle (tan) of the soft-shadow cone; the real sun is 0.0047
const float kOuTMax = 1.0e6;

struct OuTri { uint v0, v1, v2; };
OuTri ouCorners(OuGeom g, uint prim) {
    OuWords ib = OuWords(g.indices);
    uint i = g.firstIndex + prim * 3u;
    return OuTri(ib.w[i] * kOuVertexWords, ib.w[i + 1u] * kOuVertexWords, ib.w[i + 2u] * kOuVertexWords);
}
vec3 ouBary(vec2 b) { return vec3(1.0 - b.x - b.y, b.x, b.y); }
vec2 ouAttr2(OuWords vb, OuTri t, uint o, vec3 b) {
    return vec2(uintBitsToFloat(vb.w[t.v0 + o]), uintBitsToFloat(vb.w[t.v0 + o + 1u])) * b.x +
           vec2(uintBitsToFloat(vb.w[t.v1 + o]), uintBitsToFloat(vb.w[t.v1 + o + 1u])) * b.y +
           vec2(uintBitsToFloat(vb.w[t.v2 + o]), uintBitsToFloat(vb.w[t.v2 + o + 1u])) * b.z;
}
vec3 ouAttr3(OuWords vb, OuTri t, uint o, vec3 b) {
    return vec3(ouAttr2(vb, t, o, b), uintBitsToFloat(vb.w[t.v0 + o + 2u]) * b.x +
                uintBitsToFloat(vb.w[t.v1 + o + 2u]) * b.y + uintBitsToFloat(vb.w[t.v2 + o + 2u]) * b.z);
}
vec4 ouColorAt(OuWords vb, OuTri t, vec3 b) {
    return unpackUnorm4x8(vb.w[t.v0 + 8u]) * b.x + unpackUnorm4x8(vb.w[t.v1 + 8u]) * b.y + unpackUnorm4x8(vb.w[t.v2 + 8u]) * b.z;
}

// The any-hit equivalent: alpha test of a non-opaque candidate. Texture alpha only, as the shadow-map pass
// (shadow.frag); vertex alpha is ignored for casters. Which rays see non-casters and shadow-only geometry is the
// instance mask's business (kOuMask*), not the candidate loop's.
bool ouAlphaPasses(uint record, uint prim, vec2 b2) {
    OuGeom g = ouGeoms[record];
    OuTri t = ouCorners(g, prim);
    return textureLod(ouRtTextures[nonuniformEXT(g.texture)], ouAttr2(OuWords(g.vertices), t, 6u, ouBary(b2)), 0.0).a >= g.alphaRef;
}
void ouResolveCandidates(rayQueryEXT rq) {
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            ouAlphaPasses(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false) +
                               rayQueryGetIntersectionGeometryIndexEXT(rq, false)),
                          uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),
                          rayQueryGetIntersectionBarycentricsEXT(rq, false)))
            rayQueryConfirmIntersectionEXT(rq);
}
float ouVisible(vec3 o, vec3 d) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ouTlas, gl_RayFlagsTerminateOnFirstHitEXT, kOuMaskShadow, o, 0.0, d, kOuTMax);
    ouResolveCandidates(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
// Unoccluded segment o -> o + d tMax (unit d): local-light shadow rays stop short of the light.
float ouVisibleWithin(vec3 o, vec3 d, float tMax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ouTlas, gl_RayFlagsTerminateOnFirstHitEXT, kOuMaskShadow, o, 0.0, d, tMax);
    ouResolveCandidates(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
// Local light: a disc of kOuLightSize radius facing p (lamp flame / flashlight lens size [I]); rays stop kOuLightClear
// short of its centre so a fixture's own housing (brazier, lamp glass) around the light does not shadow it [I].
const float kOuLightSize = 15.0, kOuLightClear = 40.0;
// Soft shadow toward the light at lp: `rays` points on its disc (golden-angle spiral, per-pixel rotation as ouRtSun,
// offset by `seed` so the lights of one pixel do not share their pattern).
float ouRtLight(vec3 p, vec3 n, vec3 lp, int rays, float seed) {
    vec3 o = p + n * (2.0 + 2.0e-4 * distance(p, ouShadow.eye.xyz));
    vec3 l = normalize(lp - o);
    vec3 t1 = normalize(cross(l, abs(l.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0))), t2 = cross(l, t1);
    float rot = 6.2831853 * fract(seed + 52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float v = 0.0;
    for (int i = 0; i < rays; ++i) {
        float r = sqrt((float(i) + 0.5) / float(rays)), a = rot + float(i) * 2.3999632;
        vec3 q = lp + kOuLightSize * r * (cos(a) * t1 + sin(a) * t2) - o;
        float len = length(q);
        v += len > kOuLightClear + 1.0 ? ouVisibleWithin(o, q / len, len - kOuLightClear) : 1.0;
    }
    return v / float(rays);
}
// Soft sun shadow: `rays` directions in the sun cone (golden-angle spiral, rotated per pixel by interleaved
// gradient noise so the 2x2 quad average sees 4x as many distinct directions).
float ouRtSun(vec3 p, vec3 n, int rays) {
    vec3 sun = ouShadow.sun.xyz;
    vec3 t1 = normalize(cross(sun, abs(sun.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0))), t2 = cross(sun, t1);
    float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    // Normal offset against self-intersection, growing with distance (float precision of far surfaces).
    vec3 o = p + n * (2.0 + 2.0e-4 * distance(p, ouShadow.eye.xyz));
    float v = 0.0;
    for (int i = 0; i < rays; ++i) {
        float r = sqrt((float(i) + 0.5) / float(rays)), a = rot + float(i) * 2.3999632;
        vec2 d = r * kOuSunTan * vec2(cos(a), sin(a));
        v += ouVisible(o, normalize(sun + d.x * t1 + d.y * t2));
    }
    return v / float(rays);
}
// Sum over the 2x2 pixel quad via fine derivatives (helper lanes take part). Uniform control flow only.
float ouQuadSum(float v) {
    bool left = (int(gl_FragCoord.x) & 1) == 0, top = (int(gl_FragCoord.y) & 1) == 0;
    float dx = dFdxFine(v);
    float row = 2.0 * v + (left ? dx : -dx);
    float dy = dFdyFine(row);
    return 2.0 * row + (top ? dy : -dy);
}
// Closest solid hit (water surfaces excluded) along unit r from p, for mirror reflections. Returns the hit distance, < 0
// on a miss (the caller's own sky / surface colour); `col` = texture * vertex colour, `n` = unit world normal facing back
// along r (two-sided, as gl_FrontFacing in the raster pass), 0 where the mesh has none. Shade with ouRtLit.
// ouRtRough: half-angle (rad) of the caller's reflection lobe; the hit's texture is read at the mip of the lobe's
// footprint there (rough water: its averaged colour, not crisp texels). 0 = a mirror.
float ouRtRough = 0.0;
float ouRtHit(vec3 p, vec3 r, out vec3 col, out vec3 n) {
    col = vec3(0.0), n = vec3(0.0);
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ouTlas, gl_RayFlagsNoneEXT, kOuMaskSolid, p, 1.0, r, kOuTMax);
    ouResolveCandidates(rq);
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return -1.0;
    OuGeom g = ouGeoms[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionGeometryIndexEXT(rq, true))];
    OuWords vb = OuWords(g.vertices);
    OuTri t = ouCorners(g, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)));
    vec3 b = ouBary(rayQueryGetIntersectionBarycentricsEXT(rq, true));
    float dist = rayQueryGetIntersectionTEXT(rq, true);
    // ponytail: mip from hit distance (+ the lobe, ouRtRough) only, no ray cones.
    vec4 c = textureLod(ouRtTextures[nonuniformEXT(g.texture)], ouAttr2(vb, t, 6u, b), clamp(log2(dist / 400.0) + log2(1.0 + ouRtRough * 400.0), 0.0, 10.0)) *
             ouColorAt(vb, t, b);
    n = ouAttr3(vb, t, 3u, b);
    if (g.transform != uint64_t(0)) {
        OuWords x = OuWords(g.transform);
        n = vec3(dot(vec3(uintBitsToFloat(x.w[0]), uintBitsToFloat(x.w[1]), uintBitsToFloat(x.w[2])), n),
                 dot(vec3(uintBitsToFloat(x.w[4]), uintBitsToFloat(x.w[5]), uintBitsToFloat(x.w[6])), n),
                 dot(vec3(uintBitsToFloat(x.w[8]), uintBitsToFloat(x.w[9]), uintBitsToFloat(x.w[10])), n));
    }
    n = mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * n;
    col = c.rgb;
    if (dot(n, n) < 1e-12) n = vec3(0.0);
    else n = dot(n, r) > 0.0 ? -normalize(n) : normalize(n);
    return dist;
}
// Generic sun shading of a reflection hit at h (normal n from ouRtHit): scene.frag's 0.45 + 0.55 n.l, times one hard
// shadow ray toward the sun disc centre so shadowed cliffs mirror dark (no cone, no noise: the wave-bent reflection is
// the only variation).
vec3 ouRtLit(vec3 h, vec3 n, vec3 col) {
    if (dot(n, n) < 0.5) return col;
    float ndl = max(dot(n, ouShadow.sun.xyz), 0.0);
    if (ndl > 0.0 && ouShadow.sun.w != 0.0)
        ndl *= ouVisible(h + n * (2.0 + 2.0e-4 * distance(h, ouShadow.eye.xyz)), ouShadow.sun.xyz);
    return col * (0.45 + 0.55 * ndl);
}
// Ray query origins / directions must be finite (Vulkan: OpRayQueryInitializeKHR); RADV hangs the GPU
// on non-finite ones. Translated shaders derive p from 1 / gl_FragCoord.w and n from its derivatives, which are not
// finite on some fragments (l4_ballcourt, l1_kraken_room spawns lost the device on the first RT frame). NaN fails every
// comparison, so it is excluded too.
bool ouRayInputsFinite(vec3 p, vec3 n) {
    return all(lessThan(abs(p), vec3(1.0e30))) && all(lessThan(abs(n), vec3(2.0)));
}
// Water surface colour `c` at p (normal n) with the mirrored scene mixed in by Schlick Fresnel (water F0 = 0.02). Misses
// keep the material's own colour (its shader's sky / environment term) instead of a made-up sky. `gain` scales the hit
// colour toward the shading around it: 1 under the generic shaders, 2 under the translated ones (they read ~2x brighter
// than the generic shading of the same surface: water_rt.frag, VkRenderer waterPass[2].w).
vec4 ouRtWater(vec4 c, vec3 p, vec3 n, bool lit, float gain) {
    if (!ouRayInputsFinite(p, n)) return c;
    vec3 v = normalize(p - ouShadow.eye.xyz);
    float f = 0.02 + 0.98 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0);
    vec3 o = p + n * 2.0, r = reflect(v, n), refl, hn;
    // ponytail: no material roughness decoded (MATERIALS.md), so a perfect mirror; jitter r by roughness when it is.
    float d = ouRtHit(o, r, refl, hn);
    if (d < 0.0) return c;
    if (lit) refl = ouRtLit(o + r * d, hn, refl);
    return vec4(mix(c.rgb, min(refl * gain, vec3(1.0)), f), mix(c.a, 1.0, f));
}
#endif

// Sun visibility at world point p with unit normal n facing the viewer: traced when `rays` (OU_RT builds), else the
// cascaded shadow map. `lit` false = 1 (nothing to shadow). Call in uniform control flow: the traced term is averaged
// over the lanes of the 2x2 quad that trace (quads never span primitives, so nothing bleeds across edges).
float ouSunVisibility(vec3 p, vec3 n, bool lit, bool rays) {
#ifdef OU_RT
    if (rays) {
        bool traces = lit && !gl_HelperInvocation && dot(n, ouShadow.sun.xyz) > 0.0 && ouRayInputsFinite(p, n);
        float w = ouQuadSum(traces ? 1.0 : 0.0);
        float v = ouQuadSum(traces ? ouRtSun(p, n, int(ouShadow.eye.w)) : 0.0);
        return w > 0.0 ? v / w : 1.0;
    }
#endif
    return lit && ouShadow.sun.w != 0.0 ? ouShadowMapVisibility(p) : 1.0;
}

#ifdef OU_RT
// Ray-traced AO / sun bounce (VkRenderer recordIndirect, RENDERING.md "RT AO / GI"): half resolution, denoised; map rgb =
// bounce light x the receiver's albedo, a = AO factor; geometry .w = view depth (clip w, 0 = no surface).
layout(set = OU_SHADOW_SET, binding = 2) uniform sampler2D ouIndirectMap;
layout(set = OU_SHADOW_SET, binding = 3) uniform sampler2D ouIndirectGeom;
// c x AO + bounce at an opaque, pre-passed fragment (the pass traced from the pre-pass depth): joint-bilateral
// upsampling from the 2x2 half-resolution texels around it, weighted by view-depth agreement (none agrees: the closest
// one in depth, so silhouettes take their own side). screen.w bits 1 (AO) / 2 (GI) clear = the pass did not run: c.
vec3 ouIndirect(vec3 c) {
    if ((int(ouShadow.screen.w) & 3) == 0) return c;
    float z = 1.0 / gl_FragCoord.w;
    vec2 h = (gl_FragCoord.xy - 0.5) * 0.5;
    ivec2 b = ivec2(floor(h)), last = textureSize(ouIndirectMap, 0) - 1;
    vec2 t = h - vec2(b);
    vec4 sum = vec4(0.0), best = vec4(0.0, 0.0, 0.0, 1.0);
    float wsum = 0.0, bestErr = 1.0e30;
    for (int i = 0; i < 4; ++i) {
        ivec2 o = ivec2(i & 1, i >> 1), q = clamp(b + o, ivec2(0), last);
        float d = texelFetch(ouIndirectGeom, q, 0).w;
        if (d <= 0.0) continue;
        vec4 s = texelFetch(ouIndirectMap, q, 0);
        float err = abs(d - z) / z;
        float w = mix(1.0 - t.x, t.x, float(o.x)) * mix(1.0 - t.y, t.y, float(o.y)) * exp(-50.0 * err);
        sum += w * s;
        wsum += w;
        if (err < bestErr) bestErr = err, best = s;
    }
    vec4 s = wsum > 1.0e-4 ? sum / wsum : best;
    return c * s.a + s.rgb;
}
#endif

#ifdef OU_SUN
// Translated original pixel shaders: position from gl_FragCoord (exact view depth 1 / gl_FragCoord.w along the camera
// ray), face normal from its derivatives, both computed first thing in main() (uniform control flow, before texkill).
vec3 ouP, ouN;
float ouSunBegin(bool sunLit) {
    vec2 uv = gl_FragCoord.xy * ouShadow.screen.xy;
    ouP = ouShadow.eye.xyz + (ouShadow.rayBase.xyz + uv.x * ouShadow.rayU.xyz + uv.y * ouShadow.rayV.xyz) / gl_FragCoord.w;
    vec3 n = cross(dFdx(ouP), dFdy(ouP));
    ouN = dot(n, n) > 0.0 ? normalize(n) : vec3(0.0, 0.0, 1.0);
    if (dot(ouN, ouShadow.eye.xyz - ouP) < 0.0) ouN = -ouN;
#ifdef OU_RT
    return sunLit ? ouSunVisibility(ouP, ouN, true, true) : 1.0;
#else
    return sunLit ? ouSunVisibility(ouP, ouN, true, false) : 1.0;
#endif
}
// Local-light shadows (OU_RT with ouShadow.lights.z rays): visibility of the object's light slots `slots` (bit s: the
// program reads slot s's colour, which ShaderTranslate scales by vis[s]) from ouP, after ouSunBegin. tru.exe gives its
// shadow lights (DTP +0x144C, scene::Light::shadow) shadow maps in slots 0-2 (0x5042B0); here the first 4 slots of the
// light set (strongest first, original::lightSet) whose light is a shadow light trace, where the light reaches ouP
// (inside its radius, in front of the spot's cone and of the face). Uniform control flow: slot / light tests are per
// draw, the per-pixel ones only gate the traced value.
void ouLightsBegin(uint slots, out float vis[6]) {
    for (int s = 0; s < 6; ++s) vis[s] = 1.0;
#ifdef OU_RT
    const int rays = int(ouShadow.lights.z);
    if (rays == 0) return;
    for (int s = 0; s < 4; ++s) { // ponytail: 4-light budget (slots 4-5 stay unshadowed); raise with the ray budget
        uint i = OU_LIGHT(s);
        if ((slots >> uint(s) & 1u) == 0u || i > 63u || ((i < 32u ? ouShadow.lights.x >> i : ouShadow.lights.y >> (i - 32u)) & 1u) == 0u)
            continue;
        // Light registers (original::fillLights): R0 the spot apex, R1 direction, R2 (1 / radius, 0, -apex / radius),
        // R3 (cone scale, cone bias, apex plane); the light itself sits apex units ahead of R0.
        vec4 r0 = ouFrame.lights[i * 9u], r1 = ouFrame.lights[i * 9u + 1u], r2 = ouFrame.lights[i * 9u + 2u], r3 = ouFrame.lights[i * 9u + 3u];
        vec3 lp = r0.xyz - r1.xyz * (r2.z / r2.x), fromApex = ouP - r0.xyz;
        float d = length(fromApex);
        bool traces = !gl_HelperInvocation && ouRayInputsFinite(ouP, ouN) && ouRayInputsFinite(lp, vec3(0.0)) && d > 1.0 &&
                      d * r2.x + r2.z < 1.0 && dot(fromApex / d, r1.xyz) * r3.x + r3.y > 0.0 && dot(ouN, lp - ouP) > 0.0;
        float w = ouQuadSum(traces ? 1.0 : 0.0);
        float v = ouQuadSum(traces ? ouRtLight(ouP, ouN, lp, rays, 0.37 * float(s)) : 0.0);
        vis[s] = w > 0.0 ? v / w : 1.0;
    }
#endif
}
// After the program: RT water reflections on materials the backend flags reflective (fixedParams.w bit 2); RT AO /
// sun bounce on the rest (the ray-traced variant draws only opaque, pre-passed submeshes and reflective ones).
vec4 ouSunEnd(vec4 c) {
#ifdef OU_RT
    if ((int(ouMat.fixedParams.w) & 4) != 0 && ouShadow.screen.z >= 2.0) return ouRtWater(c, ouP, ouN, true, 2.0);
    if ((int(ouMat.fixedParams.w) & 4) == 0) c.rgb = ouIndirect(c.rgb);
#endif
    return c;
}
#endif
