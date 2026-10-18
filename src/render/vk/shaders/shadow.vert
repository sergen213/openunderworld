#version 460
// Sun shadow map (viewProj = light view-projection), as GL kShadowVs.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef; } pc;
layout(location = 0) out vec2 vUv;
void main() { vUv = aUv; gl_Position = pc.viewProj * pc.model * vec4(aPos, 1.0); }
