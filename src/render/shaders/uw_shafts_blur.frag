#version 460
// Enhanced under-water sun shafts, pass 2 of 2 (shaders/underwater.glsl agRaysBlur = abyssal-garden W_RBLUR):
// depth-aware 3x3 blur of pass 1 at half resolution; uw_grade.frag upsamples it. Fullscreen triangle (VK post.vert / GL
// kPostVs).
layout(location = 0) out vec4 oShafts;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
void main() { oShafts = agRaysBlur(ivec2(gl_FragCoord.xy)); }
