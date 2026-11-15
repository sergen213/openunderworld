#version 460
// Enhanced under-water sun shafts, pass 2 of 3 (shaders/underwater.glsl ouShaftMarch): the half-resolution ray march,
// rgb = in-scattered sun light, a = linear view depth. Fullscreen triangle (VK post.vert / GL kPostVs).
layout(location = 0) out vec4 oShafts;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
#ifdef VULKAN
layout(push_constant) uniform UwShafts { layout(offset = 24) int steps; } uws; // after post.frag's Push in the post range
#define OU_SHAFT_STEPS uws.steps
#else
layout(location = 0) uniform int uShaftSteps;
#define OU_SHAFT_STEPS uShaftSteps
#endif
void main() { oShafts = ouShaftMarch(ivec2(gl_FragCoord.xy), OU_SHAFT_STEPS); }
