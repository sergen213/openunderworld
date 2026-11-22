// Enhanced snow accumulation, backend-neutral part (FrameOptions::snow, docs/RENDERING.md "Weather"). Ours.
//
// Each frame with a SnowLayer the backend renders the scene's opaque casters into a top-down orthographic depth map
// (kTopSize² texels over kExtent units, centred kAhead x kExtent ahead of the camera along its horizontal view direction,
// z from the eye - kBelow up to the eye + kAbove), then runs
// shaders/snow_mask.comp over the world-anchored mask (kMaskSize² texels of kMaskTexel units, addressed by world texel
// modulo kMaskSize, so it scrolls with the camera without copying): per texel rgba = (ground z, snow 0..1, world texel
// x, y). A texel whose world texel changed (it entered the map) starts at SnowLayer::seed; while something stands on the
// column (its top above the ground by more than kSurfaceStep: Lara, a moved object) its snow is kept negated, and when
// it leaves half remains (a trodden trail); the rest grow / melt. The scene shaders (sun.glsl ouSnowApply) blend snow
// where a fragment is its column's top surface, by its normal . up.
#pragma once

#include "render/RenderCommon.hpp"
#include "render/Renderer.hpp"

#include <glm/glm.hpp>

namespace ou::render::snow {

inline constexpr int kTopSize = 2048;
inline constexpr float kExtent = 6144.0f;
inline constexpr float kAhead = 0.3f;
inline constexpr int kMaskSize = 512;
inline constexpr float kMaskTexel = kExtent / float(kMaskSize); // 12 units
inline constexpr float kBelow = 4000.0f, kAbove = 6000.0f;
inline constexpr float kSurfaceStep = 24.0f;

// This frame's map: origin (min corner, snapped to mask texels so the depth texels under a mask texel stay put), z
// range, GL clip view-projection (x, y -> uv * 2 - 1, z: the highest point nearest: depth = (zHigh - z) / span).
struct Map {
    glm::vec2 origin{0};
    float zLow = 0, zHigh = 0;
    glm::ivec2 window{0}; // world mask texel of the origin
    glm::mat4 viewProj{1};
};
Map map(glm::vec3 eye, glm::vec3 forward);

// shaders/snow_mask.comp parameters (Vulkan push constants / GL UBO binding 8, std140).
struct MaskPush {
    glm::vec4 map;     // origin xy, 1 / extent, z low
    glm::vec4 z;       // z span, mask texel size, mask texels, surface step
    glm::ivec4 window; // world mask texel of the origin xy
    glm::vec4 rate;    // grow this frame, melt this frame, seed, 0
};
MaskPush maskPush(const Map& m, const SnowLayer& s);
// The scene shaders' fields of SunShadowBlock (snowMap .. snowFogFar) for this frame; off when `s` is null. The fog is the
// translated programs' (FrameOptions::fog, else the under-water fog, else the unit's: original::fillFrame).
void fillBlock(SunShadowBlock& b, const Map& m, const SnowLayer* s, const scene::Environment& env, const Underwater* underwater,
               const Fog* fog);

const char* maskComputeSource(); // shaders/snow_mask.comp (GL; Vulkan compiles the same file with glslc, -DVULKAN)

} // namespace ou::render::snow
