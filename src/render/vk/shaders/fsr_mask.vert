#version 460
// FSR inputs of one mesh (docs/RENDERING.md "FSR"): motion vectors of moving meshes, reactivity of transparent ones.
// Binding 0 holds this frame's vertices, binding 1 the previous frame's (the same buffer for meshes that did not move).
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(location = 4) in vec3 aPrevPos;
// jittered / current / previous: model-view-projection of the raster pass, this frame unjittered, the previous frame
// unjittered. params: x alpha test reference, y reactivity, z 1 = scale reactivity by the texture alpha.
layout(push_constant) uniform Push { mat4 jittered; mat4 current; mat4 previous; vec4 params; } pc;
layout(location = 0) out vec4 vCurrent;
layout(location = 1) out vec4 vPrevious;
layout(location = 2) out vec2 vUv;
layout(location = 3) out float vAlpha;
void main() {
    vCurrent = pc.current * vec4(aPos, 1.0);
    vPrevious = pc.previous * vec4(aPrevPos, 1.0);
    vUv = aUv;
    vAlpha = aColor.a;
    gl_Position = pc.jittered * vec4(aPos, 1.0);
}
