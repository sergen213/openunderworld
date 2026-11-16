#version 460
// Alpha test exactly as scene.frag (texture alpha * vertex alpha); writes the normal target of the screen-space AO: the
// octahedral unit world normal (interpolated, facing the viewer as its face does; the face normal where the mesh has
// none), as the translated programs' pre-pass variant does (render/sun.glsl OU_PREPASS).
#define OU_SHADOW_SET 1
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec3 vNormal;
layout(location = 3) in vec3 vWorld;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; int lighting; float alphaRef; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec2 oNormal;
#include "sun.glsl"
void main() {
    vec3 f = cross(dFdx(vWorld), dFdy(vWorld));
    f = dot(f, f) > 0.0 ? normalize(f) : vec3(0.0, 0.0, 1.0);
    if (dot(f, ouShadow.eye.xyz - vWorld) < 0.0) f = -f;
    if (texture(uTex, vUv).a * vAlpha < pc.alphaRef) discard;
    vec3 n = dot(vNormal, vNormal) > 1.0e-6 ? normalize(vNormal) : f;
    oNormal = ouOctEncode(dot(n, f) < 0.0 ? -n : n);
}
