#version 460
// FSR reactivity of Enhanced water's splash fluid (fullscreen over the blurred splat target, splash set binding 1:
// render/Water.hpp splash passes), 90 % where the liquid covers the pixel as splash_composite.frag draws it.
layout(set = 0, binding = 1) uniform sampler2D uSplash;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
void main() {
    vec4 s = texelFetch(uSplash, ivec2(gl_FragCoord.xy), 0);
    float a = s.r > -1e19 ? smoothstep(0.05, 2.0, s.g) : 0.0;
    if (a < 0.004) discard;
    oMotion = vec2(0.0);
    oReactive = min(a, 1.0) * 0.9;
}
