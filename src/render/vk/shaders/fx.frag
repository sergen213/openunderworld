#version 460
// Original particle effects (FrameOptions::fxBatches). Material particles: params.x = refraction screen-uv scale (the
// scene copy offset by the normal map's xy x the texture's alpha), y / z = soft fade scale / bias against the depth copy,
// w > 0 lit (lit0 / lit1 = light 0 / 1 rgb + litLow / litHigh; ShaderLib 655's math, FxBatch); depthToW = OriginalShading
// c77 (1 / view depth = d x + y).
layout(location = 0) in vec2 vUv;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vRaw;
layout(location = 3) in vec3 vNormal;
layout(location = 4) in vec3 vTangent;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform sampler2D uScene; // waterSets_: the opaque copies (recordWaterCopies)
layout(set = 1, binding = 2) uniform sampler2D uDepth;
layout(set = 2, binding = 0) uniform sampler2D uNormal;
layout(push_constant) uniform Push { layout(offset = 64) vec4 params; vec4 depthToW; vec4 lit0; vec4 lit1; } pc;
layout(location = 0) out vec4 oColor;
float viewDepth(float d) { return 1.0 / max(d * pc.depthToW.x + pc.depthToW.y, 1e-6); }
void main() {
    const vec4 t = texture(uTex, vUv);
    if (pc.params.w > 0.0) {
        const vec4 n = texture(uNormal, vUv);
        const vec2 xy = vec2(n.x, n.y * n.w) * 2.0 - 1.0;
        const vec3 w = clamp(sqrt(abs(1.0 - dot(xy, xy))), 0.0, 1.0) * vNormal + xy.x * vTangent + xy.y * cross(vTangent, vNormal);
        const float f = clamp(-w.z * inversesqrt(max(dot(w, w), 1e-12)), 0.0, 1.0);
        float s = clamp((2.0 * f - pc.lit0.w) / (pc.lit1.w - pc.lit0.w), 0.0, 1.0);
        s = s * s * (3.0 - 2.0 * s);
        oColor = vec4(t.rgb * (mix(pc.lit1.rgb, pc.lit0.rgb, s) + vRaw.rgb), t.a * vRaw.a);
        return;
    }
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
