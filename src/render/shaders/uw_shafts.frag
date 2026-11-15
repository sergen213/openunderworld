#version 460
// Enhanced under-water sun shafts, pass 1 of 2 (shaders/underwater.glsl agRays = abyssal-garden W_RAYS): the
// half-resolution ray march, rgb = in-scattered sun light, a = linear view depth (metres). Fullscreen triangle (VK
// post.vert / GL kPostVs).
layout(location = 0) out vec4 oShafts;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
void main() { oShafts = agRays(ivec2(gl_FragCoord.xy)); }
