#version 460
// FSR reactivity of Enhanced water splash droplets (splash.vert outputs; coverage as splash.frag), 90 %.
layout(location = 0) in vec2 vUv;
layout(location = 1) in float vAlpha;
layout(location = 0) out vec2 oMotion;
layout(location = 1) out float oReactive;
void main() {
    const float a = (1.0 - smoothstep(0.35, 1.0, length(vUv))) * vAlpha * 0.85;
    if (a < 0.004) discard;
    oMotion = vec2(0.0);
    oReactive = min(a, 1.0) * 0.9;
}
