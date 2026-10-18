#version 460
// Debug lines (FrameOptions::debugLines): unlit, depth-tested with a small bias toward the camera.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; } pc;
layout(location = 0) out vec4 vColor;
void main() {
    vColor = aColor;
    gl_Position = pc.viewProj * vec4(aPos, 1.0);
    gl_Position.z -= 0.5e-4 * gl_Position.w; // = GL's 1e-4*w bias: Vulkan clip z spans [0,w], GL's [-w,w]
}
