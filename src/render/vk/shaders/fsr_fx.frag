#version 460
// FSR reactivity of the original particle effects (fx.vert outputs; docs/RENDERING.md "FSR"): their contribution by
// blend mode (params.x = FxBatch::blend: 0 alpha, 1 additive, 2 subtract, 3 multiply), 90 %. Attachment 0 masked.
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(push_constant) uniform Push { layout(offset = 64) vec4 params; } pc;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
void main() {
    const vec4 c = texture(uTex, vUv) * vColor;
    const vec3 d = pc.params.x < 2.5 ? c.rgb * c.a : abs(c.rgb + c.a - 1.0); // multiply: dst * (rgb + a)
    const float r = pc.params.x < 0.5 ? c.a : max(d.r, max(d.g, d.b));
    if (r < 0.004) discard;
    oMotion = vec2(0.0);
    oReactive = min(r, 1.0) * 0.9;
}
