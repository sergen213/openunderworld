#version 460
// Splash fluid particle (render/Fluid.hpp pool, shaders/fluid.glsl) as a camera-facing quad: no vertex input, 6 vertices
// per instance, instance = particle slot; dead slots collapse to one point off screen. Spray (few neighbours) is drawn
// finer; spray and sheets are flagged as (part) foam; particles fade over their last 0.3 s.
#include "shaders/fluid.glsl"
#ifdef VULKAN
layout(set = 0, binding = 0) uniform Water {
#else
layout(binding = 4) uniform Water {
#endif
    mat4 viewProj; mat4 invViewProj;
    vec4 eye; vec4 screen; vec4 sunDir; vec4 sunColor; vec4 fogColor; vec4 fogRange;
    vec4 ripple; vec4 rippleInfo; vec4 ambient;
} w;
#ifdef VULKAN
layout(set = 0, binding = 2, std430) readonly buffer Particles { OuFluidParticle P[]; };
#define OU_VERTEX_ID gl_VertexIndex
#define OU_INSTANCE_ID gl_InstanceIndex
#else
layout(std430, binding = 0) readonly buffer Particles { OuFluidParticle P[]; };
#define OU_VERTEX_ID gl_VertexID
#define OU_INSTANCE_ID gl_InstanceID
#endif
layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;
layout(location = 2) out vec3 vWorld;   // splash_splat.frag: sphere depth (the radius comes from its derivatives)
layout(location = 3) out float vFoam;   // 0 liquid .. 1 spray
layout(location = 4) out float vSurface; // the surface z under the particle (fluid.comp; nothing is drawn below it)
const vec2 kCorners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, -1.0), vec2(1.0, 1.0),
                                vec2(-1.0, 1.0));
void main() {
    OuFluidParticle q = P[OU_INSTANCE_ID];
    vUv = kCorners[OU_VERTEX_ID];
    vAlpha = clamp(q.x.w / 0.3, 0.0, 1.0);
    vFoam = ouFluidFoam(q.d.w);
    vSurface = q.p.w;
    if (q.x.w <= 0.0) {
        vWorld = vec3(0.0);
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        return;
    }
    // camera right / up: the first two rows of the view-projection (jitter skews them negligibly)
    vec3 right = normalize(vec3(w.viewProj[0][0], w.viewProj[1][0], w.viewProj[2][0]));
    vec3 up = normalize(vec3(w.viewProj[0][1], w.viewProj[1][1], w.viewProj[2][1]));
    float r = kOuFluidRadius * mix(1.0, 0.5, ouFluidSpray(q.d.w));
    vWorld = q.d.xyz + (right * vUv.x + up * vUv.y) * r;
    gl_Position = w.viewProj * vec4(vWorld, 1.0);
}
