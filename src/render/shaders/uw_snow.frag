#version 460
// Marine snow, abyssal-garden fsSnow (MIT) literally: a soft dot ss(1, 0.2, r)^2 lit by (sunE x shadow x 0.8 + amb(up) x
// 0.3), hue-varied toward (0.7, 1, 0.9) by a.w, x fade x twinkle x e^(-SIG.g d), at most 8 (shaders/uw_medium.glsl agSunE,
// agAmb; shadow1 = our sun shadow map). Additive (alpha 0) into the premultiplied particle target, which the post pass
// composites over the fogged scene. Only inside a water volume and in front of the opaque scene (uw_light.glsl).
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in vec4 vP;
layout(location = 2) flat in vec2 vA;
layout(location = 0) out vec4 oColor;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
#include "shaders/uw_particles.glsl"
#include "shaders/uw_light.glsl"
void main() {
    float m = agSs(1.0, 0.2, length(vUv));
    if (m <= 0.0 || uwSurfaceAt(vP.xyz) < -1e29 || uwOccluded(vP.xyz)) discard;
    vec3 wp = agPos(vP.xyz);
    vec3 c = (agSunE(wp.y, kAgZone) * ouSunShadowVisibility(vP.xyz) * 0.8 + agAmb(wp, vec3(0.0, 1.0, 0.0), 1.0, kAgZone) * 0.3) *
             mix(vec3(1.0), vec3(0.7, 1.0, 0.9), vA.x);
    oColor = vec4(min(max(c * m * m * vP.w * exp(-kAgSig.g * vA.y), vec3(0.0)), vec3(8.0)), 0.0);
}
