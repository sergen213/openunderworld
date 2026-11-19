#version 460
// Time of day backdrop pass (render/TimeOfDay.hpp SkyPush): one full-screen triangle at the far plane, drawn right after
// the exterior terrain (sky domes) and the depth clear, before the scene. Passes the view ray to sky.frag.
#ifdef VULKAN
layout(push_constant) uniform SkyPush { vec4 right, up, forward, tint, stars, moon; } pc;
#define OU_VERTEX_ID gl_VertexIndex
#else
layout(std140, binding = 5) uniform SkyPush { vec4 right, up, forward, tint, stars, moon; } pc;
#define OU_VERTEX_ID gl_VertexID
#endif
layout(location = 0) out vec3 vRay;

void main() {
    const vec2 ndc = vec2(OU_VERTEX_ID == 1 ? 3.0 : -1.0, OU_VERTEX_ID == 2 ? 3.0 : -1.0);
    vRay = pc.forward.xyz + ndc.x * pc.right.xyz + ndc.y * pc.up.xyz;
    gl_Position = vec4(ndc, 1.0, 1.0);
}
