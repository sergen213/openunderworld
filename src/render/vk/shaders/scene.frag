#version 460
// Same shading as the GL backend (kSceneFs): alpha test, two-sided sun lambert, optional 3x3 PCF.
layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;
layout(location = 2) in vec4 vColor;
layout(location = 3) in vec4 vLight;
layout(push_constant) uniform Push { mat4 viewProj; mat4 model; mat4 lightViewProj; int lighting; int shadows; float alphaRef; } pc;
layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 0) uniform sampler2DShadow uShadowMap;
layout(location = 0) out vec4 oColor;
float sunVisibility() {
    // lightViewProj already maps z to [0, 1]; the shadow pass uses a y-up viewport, so uv = ndc * 0.5 + 0.5 as in GL.
    vec3 p = vec3(vLight.xy / vLight.w * 0.5 + 0.5, vLight.z / vLight.w);
    if (any(lessThan(p, vec3(0.0))) || any(greaterThan(p, vec3(1.0)))) return 1.0;
    vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
    float s = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x) s += texture(uShadowMap, vec3(p.xy + vec2(x, y) * texel, p.z));
    return s / 9.0;
}
void main() {
    vec4 c = texture(uTex, vUv) * vColor;
    if (c.a < pc.alphaRef) discard;
    float len = length(vNormal);
    if (pc.lighting != 0 && len > 1e-6) {
        vec3 n = (gl_FrontFacing ? 1.0 : -1.0) * vNormal / len;
        float ndl = max(dot(n, normalize(vec3(-0.4, -0.5, 0.75))), 0.0);
        if (pc.shadows != 0 && ndl > 0.0) ndl *= sunVisibility();
        c.rgb *= 0.45 + 0.55 * ndl;
    }
    oColor = c;
}
