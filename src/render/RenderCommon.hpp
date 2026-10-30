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

// Fallback key light ("sun"), unit vector from the surface toward the light, for scenes whose unit has no sun light.
inline const glm::vec3 kSunDirection = glm::normalize(glm::vec3(-0.4f, -0.5f, 0.75f));
// Toward the unit's sun (Environment::sunDirection is the direction the light travels, c209) or kSunDirection.
glm::vec3 sunToward(const scene::Environment& env);
constexpr int kShadowMapSize = 2048;   // one cascade tile; the atlas is kShadowCascades tiles side by side
constexpr int kShadowCascades = 3;
inline constexpr float kCascadeEnd[kShadowCascades] = {1500.0f, 5000.0f, 16000.0f}; // view distance each cascade covers

struct Bounds {
    glm::vec3 lo{1e30f}, hi{-1e30f};
    bool empty() const { return lo.x > hi.x; }
};
// World-space bounds of every vertex of every mesh (mesh transforms applied).
Bounds sceneBounds(const scene::SceneData& scene);
// Bounds of mesh `m` in world space.
Bounds meshBounds(const scene::MeshData& m);

// Orthographic sun view-projection (GL clip convention, z in [-w, w]) fitted to the camera frustum slice
// [sliceNear, sliceFar], texel-snapped against shimmering; depth spans the whole scene so off-screen casters still cast.
glm::mat4 shadowViewProj(const Camera& camera, float aspect, const Bounds& scene, glm::vec3 toSun, float sliceNear, float sliceFar);
// True when the world-space sphere lies entirely outside x or y of an orthographic view-projection's clip volume
// ([-1, 1]): none of it can rasterise, so skipping its draw leaves the target unchanged (shadow cascades).
bool outsideOrtho(const glm::mat4& viewProj, glm::vec3 center, float radius);

// std140 `OuShadow` block of src/render/sun.glsl (keep in step).
struct SunShadowBlock {
    glm::mat4 cascade[kShadowCascades]; // world -> tile uv (xy) + depth (z), [0, 1]
    glm::vec4 rayBase, rayU, rayV;      // fragment world = eye + (rayBase + u rayU + v rayV) / gl_FragCoord.w
    glm::vec4 sun;                      // toward the sun; w 1 = sun shadows on
    glm::vec4 eye;                      // camera position; w = shadow rays per pixel
    glm::vec4 screen;                   // 1 / width, 1 / height, ray tracing mode (0, 1 shadows, 2 full), 0
};
static_assert(sizeof(SunShadowBlock) == kShadowCascades * 64 + 6 * 16);
struct SunShadow {
    glm::mat4 lightViewProj[kShadowCascades]; // per cascade tile, GL clip convention (backends remap z as needed)
    SunShadowBlock block;
};
// Cascaded sun shadow of a frame. `vulkanY`: gl_FragCoord.y runs down (Vulkan with the negative-height viewport).
SunShadow sunShadow(const Camera& camera, Viewport vp, const Bounds& scene, glm::vec3 toSun, bool vulkanY, bool shadows,
                    int rtMode, int rays);

// sun.glsl source, and a translated pixel shader (ShaderTranslate) with its OU_SUN hooks wired to it: Vulkan puts
// OuShadow in set 2 and (`rt`) the ray-tracing set in set 3; GL uses UBO binding 3 / texture unit 16.
const char* sunGlsl();
std::string originalPixelSource(const std::string& translated, bool vulkan, bool rt);

// Draw order: exterior submeshes first (SubMesh::exterior; the renderers clear the depth after them), then opaque
// submeshes in mesh order, then blended ones back to front by mesh centre distance from `eye` (the original game sorts
// its translucent pass; per-mesh granularity here); exterior ones are ordered the same way among themselves.
// Shadow-only submeshes (SubMesh::shadowOnly) are left out.
// `opacity`: per mesh (empty = all 1); meshes below 1 draw all their submeshes in the blended part, 0 not at all.
struct DrawRef { uint32_t mesh, sub; };
// The blend a submesh draws with at `opacity` (FrameOptions::meshOpacity): opaque materials alpha-blend while fading.
inline scene::Blend drawBlend(const scene::SubMesh& s, float opacity) {
    return opacity < 1 && s.blend == scene::Blend::Opaque ? scene::Blend::Alpha : s.blend;
}
// FrameOptions::meshOpacity -> one value per mesh (out stays empty when nothing fades).
inline void meshOpacities(std::span<const MeshOpacity> list, size_t meshes, std::vector<float>& out) {
    out.clear();
    if (list.empty()) return;
    out.assign(meshes, 1.0f);
    for (const auto& m : list)
        if (m.mesh < meshes) out[m.mesh] = std::clamp(m.opacity, 0.0f, 1.0f);
}
template <class GpuMesh> // needs .center (glm::vec3) and .submeshes (scene::SubMesh)
void drawOrder(const std::vector<GpuMesh>& meshes, glm::vec3 eye, std::vector<DrawRef>& out, std::span<const float> opacity = {}) {
    out.clear();
    auto op = [&](uint32_t m) { return m < opacity.size() ? opacity[m] : 1.0f; };
    auto dist = [&](const DrawRef& d) { const glm::vec3 v = meshes[d.mesh].center - eye; return glm::dot(v, v); };
    for (const bool exterior : {true, false}) {
        for (uint32_t m = 0; m < meshes.size(); ++m)
            for (uint32_t s = 0; s < meshes[m].submeshes.size(); ++s)
                if (meshes[m].submeshes[s].exterior == exterior && !meshes[m].submeshes[s].shadowOnly && drawBlend(meshes[m].submeshes[s], op(m)) == scene::Blend::Opaque)
                    out.push_back({m, s});
        const size_t blended = out.size();
        for (uint32_t m = 0; m < meshes.size(); ++m)
            for (uint32_t s = 0; s < meshes[m].submeshes.size(); ++s)
                if (meshes[m].submeshes[s].exterior == exterior && !meshes[m].submeshes[s].shadowOnly && op(m) > 0 &&
                    drawBlend(meshes[m].submeshes[s], op(m)) != scene::Blend::Opaque)
                    out.push_back({m, s});
        std::stable_sort(out.begin() + std::ptrdiff_t(blended), out.end(),
                         [&](const DrawRef& a, const DrawRef& b) { return dist(a) > dist(b); });
    }
}
// Number of exterior draws at the front of a drawOrder list.
template <class GpuMesh>
size_t exteriorCount(const std::vector<GpuMesh>& meshes, std::span<const DrawRef> order) {
    size_t n = 0;
    while (n < order.size() && meshes[order[n].mesh].submeshes[order[n].sub].exterior) ++n;
    return n;
}

// ---- Stream-unit portals (docs/RENDERING.md "Portal clipping") ----
// One loaded unit: its scene meshes and its portals to other loaded units (world space; `normal` points into this unit,
// as the StreamUnitPortal +0xF0 normals of l1_seatop, l1_cineseatop, l4_pool and l4_ragnarok_room do).
struct PortalUnit {
    struct Link { size_t to; glm::vec3 quad[4]; glm::vec3 normal; };
    uint32_t firstMesh = 0, meshCount = 0;
    std::vector<Link> links;
};
// tru.exe binds every stream portal (0x5CEC60 / 0x5CF070) as a cdc::ScenePortal of the unit's cell group: a neighbour
// is drawn only through its portal. From the camera's unit, depth first through the portals facing the camera, each
// unit gets the union of its portal paths' screen rects (projected quad bounds, intersected along the path). A camera
// in a portal of its own unit, or behind it within 1500 units of the quad, sees that neighbour whole; behind it farther
// away (past the plane only, as l1_seatop's sky portal to l1_cineseatop) the portal is back-facing. Unreached units are
// hidden.
std::vector<UnitClip> portalClips(std::span<const PortalUnit> units, size_t cameraUnit, const Camera& camera, float aspect);
// Index into `clips` of scene mesh `mesh`, -1 when it is in no clip range.
int clipIndex(std::span<const UnitClip> clips, size_t mesh);
inline bool clipHidden(const UnitClip& c) { return c.rect[0] >= c.rect[2] || c.rect[1] >= c.rect[3]; }
struct PixelRect { int x = 0, y = 0, width = 0, height = 0; };
// Scissor of a clip on a width x height target, grown by a pixel; `yDown`: y from the top (Vulkan), else bottom (GL).
PixelRect clipPixels(const UnitClip& c, int width, int height, bool yDown);

} // namespace ou::render
