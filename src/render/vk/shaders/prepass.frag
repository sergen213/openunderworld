#version 460
// Alpha test exactly as scene.frag (texture alpha * vertex alpha), depth only.
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
void main() { if (texture(uTex, vUv).a * vAlpha < pc.alphaRef) discard; }
