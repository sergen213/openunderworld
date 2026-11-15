#version 460
// Enhanced under-water sun shafts, pass 1 of 3 (shaders/underwater.glsl ouShaftPatternTexel): the focused light entering
// the water surface, baked into a kShaftTile^2 R16F tile around the camera. Fullscreen triangle (VK post.vert / GL
// kPostVs) over the tile; post's descriptor sets 1 + 2 (VK) / the post pass's units (GL).
layout(location = 0) out float oPattern;
#include "shaders/underwater.glsl"
#ifdef VULKAN
#define OU_SHADOW_SET 2
#endif
#include "sun.glsl"
#include "shaders/atmosphere.glsl"
void main() { oPattern = ouShaftPatternTexel(ivec2(gl_FragCoord.xy)); }
