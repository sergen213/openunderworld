#version 460
// Camera motion of every pixel from its depth (static geometry and sky; docs/RENDERING.md "FSR"); moving meshes are
// drawn over it by fsr_mask. Clears the reactivity attachment. Drawn with post.vert (uv (0,0) = image row 0).
layout(location = 0) in vec2 vUv;
// reproject: previous unjittered view-projection * inverse(this frame's jittered one); jitter: this frame's jitter in UV.
layout(push_constant) uniform Push { mat4 reproject; vec2 jitter; } pc;
layout(set = 0, binding = 0) uniform sampler2D uDepth;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
void main() {
    const float depth = textureLod(uDepth, vUv, 0.0).r;
    vec4 previous = pc.reproject * vec4(vUv.x * 2.0 - 1.0, 1.0 - vUv.y * 2.0, depth, 1.0);
    previous.xy /= previous.w;
    oMotion = vec2(previous.x * 0.5 + 0.5, 0.5 - previous.y * 0.5) - (vUv - pc.jitter);
    oReactive = 0.0;
}
