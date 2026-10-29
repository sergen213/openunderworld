#version 460
// Writes UV-space motion (previous - current position, y down) to attachment 0 and reactivity to attachment 1; each
// pipeline's colour write mask keeps the other attachment untouched.
layout(location = 0) in vec4 vCurrent;
layout(location = 1) in vec4 vPrevious;
layout(location = 2) in vec2 vUv;
layout(location = 3) in float vAlpha;
layout(push_constant) uniform Push { mat4 jittered; mat4 current; mat4 previous; vec4 params; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
void main() {
    const float alpha = texture(uTex, vUv).a * vAlpha;
    if (alpha < pc.params.x) discard;
    oMotion = (vPrevious.xy / vPrevious.w - vCurrent.xy / vCurrent.w) * vec2(0.5, -0.5);
    oReactive = pc.params.y * (pc.params.z > 0.5 ? alpha : 1.0);
}
