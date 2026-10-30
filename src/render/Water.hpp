// Enhanced water: shared uniform block and GLSL for both backends (Original water keeps TRU's shaders).
// The surface look is Wars and Warriors: Joan of Arc's port's shade_water (same author, GPLv3): saturated teal-to-deep-
// blue absorption over the refracted bed, the interactive wave field (game::WaveField) bending refraction / reflection
// and lighting crests, Fresnel reflection, GGX sun glints, shoreline lace foam. Plus ours: screen-space reflections,
// splash droplets and the underwater post effect. docs/RENDERING.md "Water".
#pragma once

#include "render/Renderer.hpp"
#include "scene/Assets.hpp"

#include <glm/glm.hpp>

#include <string>

namespace ou::render {

// Water-surface meshes are objects named *watersurface* (l4_pool: ca_goo_pool_watersurface) [I].
bool isWaterSurface(std::string_view meshOrigin);
// Enhanced water also draws TRU's planar-reflection water (the submeshes with original::reflectionSamplers: seas and
// lakes such as l2_cliffclimb's sea, one terrain mesh) when the mesh's reflection plane `reflectZ` lies in a water
// volume: its bounds reach the box in x, y and the plane is between the box floor and 500 above the surface.
bool reflectsWater(glm::vec3 center, float radius, float reflectZ, std::span<const WaterVolume> volumes);
// The sea waves water.vert lowers such a mesh by (columns c60-c63 as original::fillWaves; zero = flat).
glm::mat4 waterWaves(glm::vec3 center, float radius, std::span<const WaterVolume> volumes);

// std140 block shared by the water, splash and post (underwater, atmosphere, weather) shaders. GL: UBO binding 4.
// Vulkan: water set 0 / post set 1, binding 0. Shaders may declare a prefix of it (splash stops at `ambient`).
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
    glm::vec4 rippleInfo;  // x grid size, y 1 = a wave field is bound, z/w unused
    glm::vec4 ambient;     // rgb scene ambient (scene::Environment::ambient), w unused
    // ---- v0.10 extras (FrameOptions waterDetail / waterDebug / atmosphere / weather) ----
    glm::vec4 detail;      // x 1 = water detail, y debug view, z geometry wave amplitude, w water volume count
    glm::vec4 lamps[4];    // nearest point lights: xyz world, w radius (0 = unused); water reflects them
    glm::vec4 lampColor[4];
    glm::vec4 haze;        // x density per unit, y height falloff per unit, z base height (world z), w atmosphere level
    glm::vec4 mist;        // x density per unit, y height falloff per unit, z base height, w 0 no sun, 1 sun, 2 + shafts
    glm::vec4 weather;     // x kind (0 none, 1 rain, 2 snow), y intensity, zw wind xy
    glm::vec4 volumes[16]; // water volumes: [2i] min, [2i + 1] max (max.z = surface), nearest first
    // Original screen filters (FrameOptions::screenFilter; post pass, shaders/filter.glsl): [0] (alphaA, blurA, dof,
    // colourMatrix), [1] (tintA rgb, finishMatrix), [2] (amountC, blurC, dofScale, dofBias), [3] additive tint, [4] alpha
    // tint, [5] (feedback keep, amount, blur, zoom x), [6] (feedback rgb, angle), [7] (zoom y, noise alpha, noise cells),
    // [8] (noise rgb, seed); off = all 0 except feedback keep / zoom 1.
    glm::vec4 filter[9];
    glm::uvec4 filterLut[ScreenFilter::kLutRows * 64]; // ScreenFilter::lut, 4 RGBA8 texels per element
    glm::vec4 waterPass[3]; // FrameOptions::underwater: [0] surface rgb + alpha at the eye, [1] deep rgb + distance,
                            // [2] x strength (0 = off); both water qualities
};
static_assert(sizeof(WaterBlock) == 2 * 64 + 9 * 16 + 28 * 16 + 9 * 16 + ScreenFilter::kLutRows * 1024 + 3 * 16);
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
