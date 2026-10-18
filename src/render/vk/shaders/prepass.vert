#version 460
// Depth pre-pass of the ray-traced variant: same position math as scene.vert (invariant), so the
// colour pass can depth-test LEQUAL against it and trace rays once per visible opaque pixel.
layout(location = 0) in vec3 aPos;
layout(location = 2) in vec2 aUv;
layout(location = 3) in vec4 aColor;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; int lighting; float alphaRef; } pc;
layout(location = 0) out vec2 vUv;
layout(location = 1) out float vAlpha;
invariant gl_Position;
void main() {
    vec4 world = pc.model * vec4(aPos, 1.0);
    vUv = aUv; vAlpha = aColor.a;
    gl_Position = pc.viewProj * world;
}
