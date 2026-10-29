// Enhanced water: shared uniform block and GLSL for both backends (Original water keeps TRU's shaders).
// Technique is ours, written for this project: procedural wave normals + the game's ripple height field,
// screen-space refraction with Beer-Lambert absorption, shore/crest foam, screen-space reflections,
// sun specular, caustics and an underwater post effect. docs/RENDERING.md "Water".
#pragma once

#include "render/Renderer.hpp"
#include "scene/Assets.hpp"

#include <glm/glm.hpp>

#include <string>

namespace ou::render {

// Water-surface meshes are objects named *watersurface* (l4_pool: ca_goo_pool_watersurface) [I].
bool isWaterSurface(std::string_view meshOrigin);

// std140 block shared by the water, splash and post (underwater, atmosphere, weather) shaders. GL: UBO binding 4.
// Vulkan: water set 0 / post set 1, binding 0. Shaders may declare a prefix of it (splash stops at `shallow`).
struct WaterBlock {
    glm::mat4 viewProj;    // world -> clip of the backend (VK includes the z remap)
    glm::mat4 invViewProj; // clip -> world, for depth reconstruction
    glm::vec4 eye;         // xyz camera position, w time (seconds)
    glm::vec4 screen;      // width, height, 1 / width, 1 / height
    glm::vec4 sunDir;      // xyz toward the sun, w specular strength
    glm::vec4 sunColor;    // rgb, w caustics strength
    glm::vec4 fogColor;    // rgb, w 1 = the camera is submerged (underwater post)
    glm::vec4 fogRange;    // x fog near, y fog far, z ripple normal strength, w reflection strength
    glm::vec4 ripple;      // origin x, origin y, cell size, surface z
    glm::vec4 rippleInfo;  // x grid size, y 1 = a ripple field is bound, z/w unused
    glm::vec4 absorb;      // rgb deep-water tint, w absorption rate per world unit
    glm::vec4 shallow;     // rgb shallow tint, w foam depth (world units)
    // ---- v0.10 extras (FrameOptions waterDetail / waterDebug / atmosphere / weather) ----
    glm::vec4 detail;      // x 1 = water detail, y debug view, z geometry wave amplitude, w water volume count
    glm::vec4 lamps[4];    // nearest point lights: xyz world, w radius (0 = unused); water reflects them
    glm::vec4 lampColor[4];
    glm::vec4 haze;        // x density per unit, y height falloff per unit, z base height (world z), w atmosphere level
    glm::vec4 mist;        // x density per unit, y height falloff per unit, z base height, w 0 no sun, 1 sun, 2 + shafts
    glm::vec4 weather;     // x kind (0 none, 1 rain, 2 snow), y intensity, zw wind xy
    glm::vec4 volumes[16]; // water volumes: [2i] min, [2i + 1] max (max.z = surface), nearest first
};
static_assert(sizeof(WaterBlock) == 2 * 64 + 10 * 16 + 28 * 16);
inline constexpr int kWaterDebugViews = 6; // 0 off, 1 normals, 2 water depth, 3 reflection hits, 4 sun glint, 5 fresnel

bool cameraUnderwater(const Camera& camera, std::span<const WaterVolume> volumes);
WaterBlock waterBlock(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const scene::Environment& env,
                      const FrameOptions& options);

// Water surface shaders (same mesh vertex layout as the scene pass).
std::string waterVertexSource(bool vulkan);
std::string waterFragmentSource(bool vulkan);
// Splash droplets: camera-facing quads expanded on the CPU (SplashVertex), soft disc in the fragment.
struct SplashVertex {
    float pos[3], uv[2], alpha;
};
std::vector<SplashVertex> splashVertices(const Camera& camera, std::span<const WaterParticle> splashes);
std::string splashVertexSource(bool vulkan);
std::string splashFragmentSource(bool vulkan);
// Post pass: declarations + `vec3 ouUnderwater(vec3 c, vec2 uv)` and `vec3 ouAtmosphere(vec3 c, vec2 uv)`
// (shaders/atmosphere.glsl: height fog, sun shafts, precipitation). Both sample uDepth at uv themselves and return
// `c` unchanged when their effect is off (Original output stays byte-identical).
std::string underwaterPostSource(bool vulkan);

} // namespace ou::render
