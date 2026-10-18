// Backend-free helpers shared by the GL and Vulkan renderers (no GPU API types here).
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "render/Renderer.hpp"

namespace ou::render {

// 8x8 glyphs for ASCII 32..126 from SDL3's built-in debug font, in a 16x6-glyph R8 atlas.
constexpr int kGlyph = 8, kAtlasCols = 16, kAtlasRows = 6;
constexpr int kAtlasW = kGlyph * kAtlasCols, kAtlasH = kGlyph * kAtlasRows;
std::vector<uint8_t> buildFontAtlas();

// Triangle list of the text pass: `ui` (FrameOptions::ui) first, then the diagnostics overlay (leading '!' =
// highlighted line) on top.
std::vector<TextVertex> buildOverlayVertices(Viewport vp, std::span<const std::string> lines, std::span<const TextVertex> ui = {});

// Bytes per 4x4 block for BCn formats, 0 for uncompressed 4-byte-per-pixel formats.
size_t blockBytes(scene::PixelFormat format);
// Number of leading mips forming a valid chain (0 = unusable). `problem` describes any defect.
uint32_t usableMips(const scene::TextureData& t, std::string& problem);

// Fixed key light ("sun"), unit vector from the surface toward the light. Both the lambert term
// and the Enhanced shadow map use it.
inline const glm::vec3 kSunDirection = glm::normalize(glm::vec3(-0.4f, -0.5f, 0.75f));
constexpr int kShadowMapSize = 2048;

struct Bounds {
    glm::vec3 lo{1e30f}, hi{-1e30f};
    bool empty() const { return lo.x > hi.x; }
};
// World-space bounds of every vertex of every mesh (mesh transforms applied).
Bounds sceneBounds(const scene::SceneData& scene);
// Bounds of mesh `m` in world space.
Bounds meshBounds(const scene::MeshData& m);

// Orthographic sun view-projection (GL clip convention, z in [-w, w]) fitted to the camera frustum
// slice [near, shadowDistance], texel-snapped against shimmering; depth spans the whole scene so
// off-screen casters still cast.
glm::mat4 shadowViewProj(const Camera& camera, float aspect, const Bounds& scene, float shadowDistance = 8000.0f);

// Draw order: opaque submeshes in mesh order, then blended ones back to front by mesh centre
// distance from `eye` (the original game sorts its translucent pass; per-mesh granularity here).
struct DrawRef { uint32_t mesh, sub; };
template <class GpuMesh> // needs .center (glm::vec3) and .submeshes (scene::SubMesh)
void drawOrder(const std::vector<GpuMesh>& meshes, glm::vec3 eye, std::vector<DrawRef>& out) {
    out.clear();
    size_t opaque = 0;
    for (uint32_t m = 0; m < meshes.size(); ++m)
        for (uint32_t s = 0; s < meshes[m].submeshes.size(); ++s)
            if (meshes[m].submeshes[s].blend == scene::Blend::Opaque) out.push_back({m, s});
    opaque = out.size();
    for (uint32_t m = 0; m < meshes.size(); ++m)
        for (uint32_t s = 0; s < meshes[m].submeshes.size(); ++s)
            if (meshes[m].submeshes[s].blend != scene::Blend::Opaque) out.push_back({m, s});
    auto dist = [&](const DrawRef& d) { const glm::vec3 v = meshes[d.mesh].center - eye; return glm::dot(v, v); };
    std::stable_sort(out.begin() + std::ptrdiff_t(opaque), out.end(),
                     [&](const DrawRef& a, const DrawRef& b) { return dist(a) > dist(b); });
}

} // namespace ou::render
