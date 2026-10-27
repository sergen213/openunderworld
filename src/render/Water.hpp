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

// std140 block shared by the water, splash and post (underwater) shaders. GL: UBO binding 4.
// Vulkan: water set 0 / post set 1, binding 0.
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
};
static_assert(sizeof(WaterBlock) == 2 * 64 + 10 * 16);

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
// Post pass: declarations + `vec3 ouUnderwater(vec3 c, vec2 uv)`. Samples uDepth at uv itself.
// Returns `c` unchanged unless the camera is submerged (Original output stays byte-identical).
std::string underwaterPostSource(bool vulkan);

} // namespace ou::render
