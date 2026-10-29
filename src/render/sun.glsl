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
// One record per BLAS geometry (= traced submesh), indexed by instance custom index + geometry index.
struct OuGeom { uint64_t vertices; uint64_t indices; uint firstIndex; uint texture; float alphaRef; uint pad; };
layout(set = OU_RT_SET, binding = 1, std430) readonly buffer OuGeoms { OuGeom ouGeoms[]; };
layout(set = OU_RT_SET, binding = 2) uniform sampler2D ouRtTextures[];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer OuWords { uint w[]; };
const uint kOuVertexWords = 18u; // scene::Vertex: position 0-2, normal 3-5, uv 6-7, RGBA8 colour 8, original-shader inputs 9-17
const uint kOuMaskSolid = 1u;    // instance masks: 1 solid, 2 water (water never reflects itself)
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
// (shadow.frag); vertex alpha is ignored for casters.
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
    rayQueryInitializeEXT(rq, ouTlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, o, 0.0, d, kOuTMax);
    ouResolveCandidates(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
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
// Mirror reflection of the solid scene (water surfaces excluded): texture * vertex colour with the generic sun
// lambert at the hit; no shadow ray at the hit. Misses return the clear colour.
vec3 ouRtReflect(vec3 p, vec3 r, bool lit) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, ouTlas, gl_RayFlagsNoneEXT, kOuMaskSolid, p, 1.0, r, kOuTMax);
    ouResolveCandidates(rq);
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return vec3(0.08, 0.09, 0.11);
    OuGeom g = ouGeoms[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionGeometryIndexEXT(rq, true))];
    OuWords vb = OuWords(g.vertices);
    OuTri t = ouCorners(g, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)));
    vec3 b = ouBary(rayQueryGetIntersectionBarycentricsEXT(rq, true));
    float dist = rayQueryGetIntersectionTEXT(rq, true);
    // ponytail: mip from hit distance only (no ray cones); fine for the flat mirror of water.
    vec4 c = textureLod(ouRtTextures[nonuniformEXT(g.texture)], ouAttr2(vb, t, 6u, b), clamp(log2(dist / 400.0), 0.0, 8.0)) *
             ouColorAt(vb, t, b);
    vec3 n = mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * ouAttr3(vb, t, 3u, b);
    if (!lit || dot(n, n) < 1e-12) return c.rgb;
    n = normalize(n);
    if (dot(n, r) > 0.0) n = -n; // two-sided, as gl_FrontFacing in the raster pass
    return c.rgb * (0.45 + 0.55 * max(dot(n, ouShadow.sun.xyz), 0.0));
}
// Ray query origins / directions must be finite (Vulkan: OpRayQueryInitializeKHR); RADV hangs the GPU
// on non-finite ones. Translated shaders derive p from 1 / gl_FragCoord.w and n from its derivatives, which are not
// finite on some fragments (l4_ballcourt, l1_kraken_room spawns lost the device on the first RT frame). NaN fails every
// comparison, so it is excluded too.
bool ouRayInputsFinite(vec3 p, vec3 n) {
    return all(lessThan(abs(p), vec3(1.0e30))) && all(lessThan(abs(n), vec3(2.0)));
}
// Water surface colour `c` at p (normal n) with the mirrored scene mixed in by Schlick Fresnel (water F0 = 0.02).
vec4 ouRtWater(vec4 c, vec3 p, vec3 n, bool lit) {
    if (!ouRayInputsFinite(p, n)) return c;
    vec3 v = normalize(p - ouShadow.eye.xyz);
    float f = 0.02 + 0.98 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0);
    // ponytail: no material roughness decoded (MATERIALS.md), so a perfect mirror; jitter r by roughness when it is.
    return vec4(mix(c.rgb, ouRtReflect(p + n * 2.0, reflect(v, n), lit), f), mix(c.a, 1.0, f));
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
// After the program: RT water reflections on materials the backend flags reflective (fixedParams.w bit 2).
vec4 ouSunEnd(vec4 c) {
#ifdef OU_RT
    if ((int(ouMat.fixedParams.w) & 4) != 0 && ouShadow.screen.z >= 2.0) return ouRtWater(c, ouP, ouN, true);
#endif
    return c;
}
#endif
