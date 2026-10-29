#version 460
// Same shading as the GL backend (kSceneFs): alpha test, two-sided sun lambert, sun shadow from sun.glsl (cascaded
// shadow map). Compiled twice: as is (scene.frag.inc) and with -DOU_RT (scene_rt.frag.inc), which traces the sun
// shadow with ray queries against the scene TLAS (set 2) and adds water reflections. RENDERING.md "Ray tracing".
#ifdef OU_RT
#extension GL_EXT_ray_query : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#define OU_RT_SET 2
#endif
#define OU_SHADOW_SET 1
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 4) in vec3 vWorld;
layout(push_constant) uniform Push {
    mat4 viewProj; mat4 model; int lighting; float alphaRef;
    int rt; // bit 0 ray-traced sun shadow, bit 1 reflective (water) draw
    float opacity; // instance opacity; < 0: opaque material, alpha = -opacity
} pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec4 oColor;
#include "sun.glsl"

void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    float len = length(vNormal);
    vec3 n = len > 1e-6 ? (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len : vec3(0.0);
    float ndl = max(dot(n, ouShadow.sun.xyz), 0.0);
    // Before the discard: traced visibility is averaged over the 2x2 quad (uniform control flow); helper,
    // alpha-cut and unlit lanes trace nothing.
    float vis = ouSunVisibility(vWorld, n, pc.lighting != 0 && ndl > 0.0 && c.a >= pc.alphaRef, (pc.rt & 1) != 0);
    if (c.a < pc.alphaRef) discard;
    if (abs(pc.opacity) < 1.0) c.a = (pc.opacity < 0.0 ? 1.0 : c.a) * abs(pc.opacity);
    if (pc.lighting != 0 && len > 1e-6) c.rgb *= 0.45 + 0.55 * ndl * vis;
#ifdef OU_RT
    if ((pc.rt & 2) != 0 && len > 1e-6) c = ouRtWater(c, vWorld, n, pc.lighting != 0);
#endif
    oColor = c;
}
