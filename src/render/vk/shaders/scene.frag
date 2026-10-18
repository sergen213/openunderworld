#version 460
// Same shading as the GL backend (kSceneFs): alpha test, two-sided sun lambert, optional 3x3 PCF.
// Compiled twice: as is (scene.frag.inc) and with -DOU_RT (scene_rt.frag.inc), which replaces the
// shadow map by ray queries against the scene TLAS (set 2) and adds water reflections. RENDERING.md "Ray tracing".
#ifdef OU_RT
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#endif
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec4 vLight;
layout(push_constant) uniform Push {
    mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef;
    int rt;   // bit 0 ray-traced sun shadows, bit 1 reflective (water) draw, bits 8-15 shadow rays per pixel
    vec4 eye; // world-space camera position
} pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 0) uniform sampler2DShadow uShadowMap;
layout(location = 0) out vec4 oColor;
const vec3 kSun = normalize(vec3(-0.4, -0.5, 0.75)); // RenderCommon kSunDirection
float sunVisibility() {
    // lightViewProj already maps z to [0, 1]; the shadow pass uses a y-up viewport, so uv = ndc * 0.5 + 0.5 as in GL.
    vec3 p = vec3(vLight.xy / vLight.w * 0.5 + 0.5, vLight.z / vLight.w);
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) s += texture(uShadowMap, vec3(p.xy + vec2(x, y) * texel, p.z));
    return s / 9.0;
}

#ifdef OU_RT
layout(location = 4) in vec3 vWorld;
layout(set = 2, binding = 0) uniform accelerationStructureEXT uTlas;
// One record per BLAS geometry (= traced submesh), indexed by instance custom index + geometry index.
struct Geom { uint64_t vertices; uint64_t indices; uint firstIndex; uint texture; float alphaRef; uint pad; };
layout(set = 2, binding = 1, std430) readonly buffer Geoms { Geom geoms[]; };
layout(set = 2, binding = 2) uniform sampler2D uTextures[];
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Words { uint w[]; };

const uint kVertexWords = 9u;    // scene::Vertex: position 0-2, normal 3-5, uv 6-7, RGBA8 colour 8
const uint kMaskSolid = 1u, kMaskWater = 2u; // instance masks (water surfaces don't reflect themselves)
const float kSunTan = 0.012;     // sun disc half-angle (tan) of the soft-shadow cone; the real sun is 0.0047
const float kTMax = 1.0e6;

struct Tri { uint v0, v1, v2; };
Tri corners(Geom g, uint prim) {
    Words ib = Words(g.indices);
    uint i = g.firstIndex + prim * 3u;
    return Tri(ib.w[i] * kVertexWords, ib.w[i + 1u] * kVertexWords, ib.w[i + 2u] * kVertexWords);
}
vec3 bary3(vec2 b) { return vec3(1.0 - b.x - b.y, b.x, b.y); }
vec2 attr2(Words vb, Tri t, uint o, vec3 b) {
    return vec2(uintBitsToFloat(vb.w[t.v0 + o]), uintBitsToFloat(vb.w[t.v0 + o + 1u])) * b.x +
           vec2(uintBitsToFloat(vb.w[t.v1 + o]), uintBitsToFloat(vb.w[t.v1 + o + 1u])) * b.y +
           vec2(uintBitsToFloat(vb.w[t.v2 + o]), uintBitsToFloat(vb.w[t.v2 + o + 1u])) * b.z;
}
vec3 attr3(Words vb, Tri t, uint o, vec3 b) {
    return vec3(attr2(vb, t, o, b), uintBitsToFloat(vb.w[t.v0 + o + 2u]) * b.x +
                uintBitsToFloat(vb.w[t.v1 + o + 2u]) * b.y + uintBitsToFloat(vb.w[t.v2 + o + 2u]) * b.z);
}
vec4 colorAt(Words vb, Tri t, vec3 b) {
    return unpackUnorm4x8(vb.w[t.v0 + 8u]) * b.x + unpackUnorm4x8(vb.w[t.v1 + 8u]) * b.y + unpackUnorm4x8(vb.w[t.v2 + 8u]) * b.z;
}

// The any-hit equivalent: alpha test of a non-opaque candidate. Texture alpha only, as the shadow-map
// pass (shadow.frag); vertex alpha is ignored for casters.
bool alphaPasses(uint record, uint prim, vec2 b2) {
    Geom g = geoms[record];
    Tri t = corners(g, prim);
    return textureLod(uTextures[nonuniformEXT(g.texture)], attr2(Words(g.vertices), t, 6u, bary3(b2)), 0.0).a >= g.alphaRef;
}
void resolveCandidates(rayQueryEXT rq) {
    while (rayQueryProceedEXT(rq))
        if (rayQueryGetIntersectionTypeEXT(rq, false) == gl_RayQueryCandidateIntersectionTriangleEXT &&
            alphaPasses(uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, false) +
                             rayQueryGetIntersectionGeometryIndexEXT(rq, false)),
                        uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, false)),
                        rayQueryGetIntersectionBarycentricsEXT(rq, false)))
            rayQueryConfirmIntersectionEXT(rq);
}
float visible(vec3 o, vec3 d) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsTerminateOnFirstHitEXT, 0xFFu, o, 0.0, d, kTMax);
    resolveCandidates(rq);
    return rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0;
}
// Soft sun shadow: `rays` directions in the sun cone (golden-angle spiral, rotated per pixel by
// interleaved gradient noise so the 2x2 quad average sees 4x as many distinct directions).
float rtSun(vec3 p, vec3 n, int rays) {
    vec3 t1 = normalize(cross(kSun, abs(kSun.z) < 0.9 ? vec3(0, 0, 1) : vec3(1, 0, 0))), t2 = cross(kSun, t1);
    float rot = 6.2831853 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    // Normal offset against self-intersection, growing with distance (float precision of far surfaces).
    vec3 o = p + n * (2.0 + 2.0e-4 * distance(p, pc.eye.xyz));
    float v = 0.0;
    for (int i = 0; i < rays; ++i) {
        float r = sqrt((float(i) + 0.5) / float(rays)), a = rot + float(i) * 2.3999632;
        vec2 d = r * kSunTan * vec2(cos(a), sin(a));
        v += visible(o, normalize(kSun + d.x * t1 + d.y * t2));
    }
    return v / float(rays);
}
// Sum over the 2x2 pixel quad via fine derivatives (helper lanes take part). Uniform control flow only.
float quadSum(float v) {
    bool left = (int(gl_FragCoord.x) & 1) == 0, top = (int(gl_FragCoord.y) & 1) == 0;
    float dx = dFdxFine(v);
    float row = 2.0 * v + (left ? dx : -dx);
    float dy = dFdyFine(row);
    return 2.0 * row + (top ? dy : -dy);
}
// Mirror reflection of the solid scene (water surfaces excluded): texture * vertex colour with the
// same lambert as the raster pass at the hit; no shadow ray at the hit. Misses return the clear colour.
vec3 rtReflect(vec3 p, vec3 r) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, uTlas, gl_RayFlagsNoneEXT, kMaskSolid, p, 1.0, r, kTMax);
    resolveCandidates(rq);
    if (rayQueryGetIntersectionTypeEXT(rq, true) == gl_RayQueryCommittedIntersectionNoneEXT) return vec3(0.08, 0.09, 0.11);
    Geom g = geoms[uint(rayQueryGetIntersectionInstanceCustomIndexEXT(rq, true) + rayQueryGetIntersectionGeometryIndexEXT(rq, true))];
    Words vb = Words(g.vertices);
    Tri t = corners(g, uint(rayQueryGetIntersectionPrimitiveIndexEXT(rq, true)));
    vec3 b = bary3(rayQueryGetIntersectionBarycentricsEXT(rq, true));
    float dist = rayQueryGetIntersectionTEXT(rq, true);
    // ponytail: mip from hit distance only (no ray cones); fine for the flat mirror of water.
    vec4 c = textureLod(uTextures[nonuniformEXT(g.texture)], attr2(vb, t, 6u, b), clamp(log2(dist / 400.0), 0.0, 8.0)) *
             colorAt(vb, t, b);
    vec3 n = mat3(rayQueryGetIntersectionObjectToWorldEXT(rq, true)) * attr3(vb, t, 3u, b);
    if (pc.lighting == 0 || dot(n, n) < 1e-12) return c.rgb;
    n = normalize(n);
    if (dot(n, r) > 0.0) n = -n; // two-sided, as gl_FrontFacing in the raster pass
    return c.rgb * (0.45 + 0.55 * max(dot(n, kSun), 0.0));
}
#endif

void main() {
    vec4 c = texture(uTex, vUv) * vColor;
#ifndef OU_RT
    if (c.a < pc.alphaRef) discard;
    float len = length(vNormal);
    if (pc.lighting != 0 && len > 1e-6) {
        vec3 n = (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len;
        float ndl = max(dot(n, kSun), 0.0);
        if (pc.shadows != 0 && ndl > 0.0) ndl *= sunVisibility();
        c.rgb *= 0.45 + 0.55 * ndl;
    }
#else
    // Spatial denoise: the sun visibility is averaged over the lanes of the 2x2 pixel quad that trace
    // (covered, kept by the alpha test, facing the sun); helpers and discarded lanes trace nothing.
    // Quads never span primitives, so nothing bleeds across edges. Hence: trace before the discard.
    float len = length(vNormal);
    vec3 n = len > 1e-6 ? (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len : vec3(0.0);
    float ndl = max(dot(n, kSun), 0.0);
    float vis = 1.0;
    if ((pc.rt & 1) != 0) {
        bool traces = !gl_HelperInvocation && c.a >= pc.alphaRef && ndl > 0.0 && pc.lighting != 0;
        float w = traces ? 1.0 : 0.0;
        vis = quadSum(traces ? rtSun(vWorld, n, (pc.rt >> 8) & 0xFF) : 0.0) / max(quadSum(w), 1.0);
    } else if (pc.shadows != 0 && ndl > 0.0) vis = sunVisibility();
    if (c.a < pc.alphaRef) discard;
    if (pc.lighting != 0 && len > 1e-6) c.rgb *= 0.45 + 0.55 * ndl * vis;
    if ((pc.rt & 2) != 0 && len > 1e-6) {
        vec3 v = normalize(vWorld - pc.eye.xyz);
        vec3 r = reflect(v, n);
        float f = 0.02 + 0.98 * pow(1.0 - clamp(dot(-v, n), 0.0, 1.0), 5.0); // Schlick, water F0 = 0.02
        // ponytail: no material roughness decoded (MATERIALS.md), so a perfect mirror; jitter r by roughness when it is.
        c.rgb = mix(c.rgb, rtReflect(vWorld + n * 2.0, r), f);
        c.a = mix(c.a, 1.0, f);
    }
#endif
    oColor = c;
}
