#version 460
layout(location = 0) in vec2 vUv;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() { if (texture(uTex, vUv).a < pc.alphaRef) discard; }
