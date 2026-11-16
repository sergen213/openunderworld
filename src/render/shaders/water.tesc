#version 460
// Enhanced water surface, tessellation control (GL 4.5 / Vulkan with tessellationShader; render/Water.hpp). TRU's water
// meshes are coarse (~3.5 m quads), so the interactive wave field's crater, rebound jet, ring trains and wakes (cells of
// w.ripple.z, ~7.8 cm; shaders/water_surface.glsl) would only show in the shading. Inside the field's window each
// triangle is split until its edges are about kCells field cells long near the camera, or kPixels pixels farther away;
// outside the window (and while no field runs) the factors are 1 and water.tese displaces the mesh's own vertices
// exactly as water.vert does without tessellation. Each edge's factor depends only on its two end points (symmetric
// in them), so the triangles sharing an edge split it identically: no cracks. Triangles wholly outside the view
// (with a margin for the displacement) are dropped.
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
    vec4 detail; vec4 lamps[4]; vec4 lampColor[4]; vec4 haze; vec4 mist; vec4 weather; vec4 volumes[16];
    vec4 filters[9]; uvec4 filterLut[640]; vec4 waterPass[3]; uvec4 rays[157]; mat4 seaWaves; vec4 optics[3]; vec4 ocean[2]; // as water.frag (GL links blocks across stages)
} w;
layout(vertices = 3) out;
layout(location = 0) in vec3 vWorld[];
layout(location = 1) in vec3 vNormal[];
layout(location = 2) in vec2 vUv[];
layout(location = 3) in vec4 vColor[];
layout(location = 0) out vec3 tWorld[];
layout(location = 1) out vec3 tNormal[];
layout(location = 2) out vec2 tUv[];
layout(location = 3) out vec4 tColor[];

const float kCells = 1.5;      // target edge length near the camera, in field cells (the field's waves span >= 4 cells)
const float kPixels = 12.0;    // target edge length on screen past the cell-limited range
// Beyond ~12 m the field's few-cm waves project to under a few pixels: its subdivision fades out to 20 m.
const float kFadeNear = 12.0 * 290.0, kFadeFar = 20.0 * 290.0;
const float kMaxLevel = 63.0;  // fractional_odd_spacing's largest level (the spec's minimum maxTessellationGenerationLevel is 64)
const float kMargin = 4.0 * 290.0; // world units the displacement may move a vertex (TRU waves + FFT ocean + field), view test

// How deep inside the field's window world point p lies, 0 at its edge (outside: < 0), 1 at its centre-ish: the
// subdivision fades out over the outer 2-10 % (the field itself fades over the outer 4 %).
float ouWindowWeight(vec2 p) {
    vec2 uv = (p - w.ripple.xy) / (w.ripple.z * w.rippleInfo.x);
    vec2 in2 = min(uv, 1.0 - uv);
    return smoothstep(0.02, 0.10, min(in2.x, in2.y));
}
// The level of edge a-b (symmetric in a and b: a + b, distance and max / min are exact under the swap).
float ouEdgeLevel(vec3 a, vec3 b) {
    if (w.rippleInfo.y < 0.5 || w.rippleInfo.z <= 0.0) return 1.0;
    vec3 m = (a + b) * 0.5;
    float win = max(ouWindowWeight(m.xy), max(ouWindowWeight(a.xy), ouWindowWeight(b.xy)));
    if (win <= 0.0) return 1.0;
    float d = min(distance(m, w.eye.xyz), min(distance(a, w.eye.xyz), distance(b, w.eye.xyz)));
    float target = max(kCells * w.ripple.z, d * kPixels / w.rippleInfo.z); // world units per segment
    win *= 1.0 - smoothstep(kFadeNear, kFadeFar, d);
    return mix(1.0, clamp(distance(a, b) / target, 1.0, kMaxLevel), win);
}
// Every corner, raised and lowered by the margin, outside the same clip plane (x, y, or behind the eye)?
bool ouOutsideView() {
    vec4 allOut = vec4(1.0); // per plane: 1 while every corner so far lies outside it
    bool behind = true;
    for (int i = 0; i < 3; ++i)
        for (int s = -1; s <= 1; s += 2) {
            vec4 c = w.viewProj * vec4(vWorld[i] + vec3(0.0, 0.0, float(s) * kMargin), 1.0);
            float m = abs(c.w) * 0.05 + 1.0; // a little slack against the per-vertex rounding
            allOut *= vec4(c.x < -c.w - m, c.x > c.w + m, c.y < -c.w - m, c.y > c.w + m);
            behind = behind && c.w <= 0.0;
        }
    return any(greaterThan(allOut, vec4(0.0))) || behind;
}

void main() {
    tWorld[gl_InvocationID] = vWorld[gl_InvocationID];
    tNormal[gl_InvocationID] = vNormal[gl_InvocationID];
    tUv[gl_InvocationID] = vUv[gl_InvocationID];
    tColor[gl_InvocationID] = vColor[gl_InvocationID];
    if (gl_InvocationID == 0) {
        if (ouOutsideView()) {
            gl_TessLevelOuter[0] = gl_TessLevelOuter[1] = gl_TessLevelOuter[2] = 0.0;
            gl_TessLevelInner[0] = 0.0;
        } else {
            // Outer level i belongs to the edge opposite corner i.
            float e0 = ouEdgeLevel(vWorld[1], vWorld[2]), e1 = ouEdgeLevel(vWorld[2], vWorld[0]), e2 = ouEdgeLevel(vWorld[0], vWorld[1]);
            gl_TessLevelOuter[0] = e0, gl_TessLevelOuter[1] = e1, gl_TessLevelOuter[2] = e2;
            gl_TessLevelInner[0] = max(e0, max(e1, e2));
        }
    }
}
