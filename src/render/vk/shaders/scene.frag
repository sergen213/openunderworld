#version 460
// Same lambert as the GL backend: fixed key light, 0.45 ambient, texture alpha ignored.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; int lighting; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    float len = length(vNormal);
    if (pc.lighting != 0 && len > 1e-6) {
        float ndl = max(dot(vNormal / len, normalize(vec3(-0.4, -0.5, 0.75))), 0.0);
        c.rgb *= 0.45 + 0.55 * ndl;
    }
    oColor = vec4(c.rgb, 1.0);
}
