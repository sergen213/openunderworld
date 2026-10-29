#version 460
// Original particle effects (FrameOptions::fxBatches). Material particles: params.x = refraction screen-uv scale (the
// scene copy offset by the normal map's xy x the texture's alpha), y / z = soft fade scale / bias against the depth copy;
// depthToW = OriginalShading c77 (1 / view depth = d x + y).
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform sampler2D uScene; // waterSets_: the opaque copies (recordWaterCopies)
layout(set = 1, binding = 2) uniform sampler2D uDepth;
layout(set = 2, binding = 0) uniform sampler2D uNormal;
layout(push_constant) uniform Push { layout(offset = 64) vec4 params; vec4 depthToW; } pc;
layout(location = 0) out vec4 oColor;
float viewDepth(float d) { return 1.0 / max(d * pc.depthToW.x + pc.depthToW.y, 1e-6); }
void main() {
    const vec4 t = texture(uTex, vUv);
    if (pc.params.x > 0.0) {
        const vec2 suv = gl_FragCoord.xy / vec2(textureSize(uScene, 0));
        const vec4 n = texture(uNormal, vUv);
        const vec2 off = (vec2(n.x, n.y * n.w) * 2.0 - 1.0) * t.a * pc.params.x;
        oColor = vec4(texture(uScene, suv + off).rgb, t.a * vColor.a);
        return;
    }
    float a = 1.0;
    if (pc.params.y > 0.0) {
        const vec2 suv = gl_FragCoord.xy / vec2(textureSize(uDepth, 0));
        a = clamp((viewDepth(texture(uDepth, suv).r) - viewDepth(gl_FragCoord.z) - (1.0 - t.a) * pc.params.z) * pc.params.y,
                  0.0, 1.0);
    }
    oColor = t * vColor * vec4(1.0, 1.0, 1.0, a);
}
