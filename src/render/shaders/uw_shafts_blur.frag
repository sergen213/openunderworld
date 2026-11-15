#version 460
// Enhanced under-water sun shafts, pass 3 of 3 (shaders/underwater.glsl ouShaftBlur): depth-aware 3x3 blur of pass 2 at
// half resolution; the post pass upsamples it (ouShaftsComposite). Fullscreen triangle (VK post.vert / GL kPostVs).
layout(location = 0) out vec4 oShafts;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
void main() { oShafts = ouShaftBlur(ivec2(gl_FragCoord.xy)); }
