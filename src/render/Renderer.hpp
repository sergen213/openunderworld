// Backend interface. Apps and scene code talk to this; only src/render/<backend> sees the GPU API.
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "render/Camera.hpp"
#include "scene/Assets.hpp"

struct SDL_Window;

namespace ou::render {

struct Viewport {
    int width = 0, height = 0; // drawable size in pixels
};

// World-space line segment for debug overlays (collision, markup).
struct DebugLine {
    float a[3], b[3];
    uint32_t color = 0xFFFFFFFF; // RGBA8, little-endian (as scene::Vertex::color)
};

// Screen-space quad vertex of the text/UI pass. u < 0: solid quad; 0 <= u <= 1: font atlas (RenderCommon
// kGlyph/kAtlas*), colour alpha-masked by the glyph; 2 <= u < 4: FrameOptions::uiImages[0] at (u - 2, v); u >= 4:
// uiImages[1] at (u - 4, v); images are RGBA x colour.
struct TextVertex {
    float x, y, u, v; // pixels from top-left
    uint32_t color;   // RGBA8
};

// Optional hardware ray tracing (Vulkan ray queries, Enhanced only). Full = shadows + water reflections.
enum class RayTracing : uint8_t { Off, Shadows, Full };

// ---- water (docs/RENDERING.md "Water") ----
// Water surface meshes draw with TRU's own shaders (Original) or with our water shading (Enhanced: waves, refraction,
// depth absorption, shore foam, reflections, interactive ripples, caustics). Written from scratch for this project.
enum class WaterQuality : uint8_t { Original, Enhanced };
// Interactive ripple height field on one water surface around Lara (game::WaterSim), sampled by Enhanced water.
struct WaterRipples {
    float originX = 0, originY = 0; // world x, y of the corner of texel (0, 0)
    float cellSize = 0;             // world units per texel; 0 = no field
    float surfaceZ = 0;             // world z of the surface the field rides on
    int size = 0;                   // texels per side; heights.size() == size * size, row-major, x fastest
    std::span<const float> heights; // world units above the rest surface
    uint64_t version = 0;           // changes whenever heights change; backends re-upload only then
};
// Water volume: world AABB, max[2] = surface z (game water markup, type 10). Underwater post and caustics.
struct WaterVolume { float min[3], max[3]; };
// Splash / spray droplet: world position, radius (world units), opacity 0..1; drawn as a camera-facing soft disc.
struct WaterParticle { float pos[3]; float radius; float alpha; };

struct RgbaImage {
    std::span<const uint8_t> rgba; // tightly packed RGBA8, top row first
    int width = 0, height = 0;
    uint64_t version = 0; // backends upload only when this changes
};

struct FrameOptions {
    bool lighting = true;                   // simple lambert vs unlit
    bool cullBackFaces = true;              // front = counter-clockwise (cdc meshes are CCW)
    std::span<const std::string> overlay;   // diagnostic text lines, top-left; leading '!' = highlight
    // Drawn after the scene, depth-tested (biased slightly toward the camera), unlit. Backends may
    // cache the upload while the span's data pointer and size stay the same.
    std::span<const DebugLine> debugLines;
    // Graphics settings (presets: ou::config::resolve). Changing them rebuilds render targets.
    int msaa = 1;          // 1, 2, 4, 8 samples
    bool shadows = false;  // sun shadow map (RenderCommon shadowViewProj), PCF 3x3
    bool fxaa = false;     // FXAA on the resolved image
    bool hdr = false;      // RGBA16F scene target + ACES tonemap
    // Materials draw with TRU's own shaders translated from D3D9 SM3 (docs/formats/SHADERS.md) where they translate;
    // others (and lines, overlay, shadow map) use the generic shading. `time` (seconds) feeds TimeVector (wind).
    bool originalShaders = false;
    float time = 0.0f;
    std::span<const TextVertex> ui = {}; // screen-space triangle list (src/ui menus, HUD), drawn before the overlay
    // Sampled by `ui` vertices with u >= 2 (bilinear, clamped): [0] original menu art atlas (ui::Art), [1] menu movie.
    // Backends upload only when an image's version changes (versions start at 1); empty = keep the previous upload.
    std::array<RgbaImage, 2> uiImages;
    // Replaces the sun shadow map when the backend supports it; otherwise the shadow map is used.
    RayTracing rt = RayTracing::Off;
    int rtRays = 2; // sun-cone shadow rays per pixel (then averaged over the 2x2 pixel quad)
    float brightness = 1.0f; // display gamma of the post pass: out = in^(1 / brightness); 1 = neutral
    WaterQuality water = WaterQuality::Original;  // ou::config presets choose (Enhanced by default)
    WaterRipples ripples;                          // empty = calm (procedural waves only)
    std::span<const WaterVolume> waterVolumes;     // loaded units' water volumes
    std::span<const WaterParticle> splashes;       // drawn blended after the water surfaces
};

// Ray-tracing state for diagnostics and tests.
struct RtStatus {
    bool supported = false;   // device + driver can (and may: OU_VK_NO_RT unset) do ray queries
    bool active = false;      // the last frame traced rays
    uint32_t blas = 0, instances = 0, geometries = 0, waterInstances = 0; // current scene
    uint32_t tlasBuilds = 0, tlasUpdates = 0, blasRefits = 0, blasRebuilds = 0; // since init
    std::string reason; // why unsupported
};
// One overlay line: requested mode, whether it is traced or falls back to the shadow map, scene counts.
std::string rtSummary(const RtStatus& s, RayTracing requested, int rays);

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb; // tightly packed RGB8, top row first
};

class Renderer {
public:
    virtual ~Renderer() = default;

    // SDL_WindowFlags the window must be created with (e.g. SDL_WINDOW_OPENGL).
    virtual uint64_t windowFlags() const = 0;
    // Called before window creation (e.g. GL attributes, Vulkan instance + device probe).
    // Returns false + error when the backend is unavailable on this system (apps treat it as skip).
    virtual bool preWindowSetup(std::string& error) = 0;
    virtual bool init(SDL_Window* window, std::string& error) = 0;
    // Replaces all GPU resources. Returns warnings (unsupported formats etc.).
    virtual std::vector<std::string> upload(const scene::SceneData& scene) = 0;
    // Replaces the vertices of uploaded mesh `meshIndex` (index into the last upload's SceneData::meshes;
    // same vertex count). Used for CPU skinning. false = no such mesh or vertex count mismatch.
    virtual bool updateVertices(size_t meshIndex, std::span<const scene::Vertex> vertices) = 0;
    virtual void resize(Viewport viewport) = 0;
    virtual void drawFrame(const Camera& camera, Viewport viewport, const FrameOptions& options) = 0;
    virtual Image readBackbuffer(Viewport viewport) = 0; // after drawFrame, before present
    virtual void present() = 0;
    virtual bool setVsync(bool enabled) = 0;
    virtual std::string description() const = 0;  // device / API version for diagnostics
    virtual RtStatus rtStatus() const { return {}; } // backends without ray tracing: unsupported
    virtual void shutdown() = 0;
};

std::unique_ptr<Renderer> createGlRenderer();
std::unique_ptr<Renderer> createVulkanRenderer(); // Vulkan 1.3, dynamic rendering
inline std::unique_ptr<Renderer> createBackend(bool vulkan) { return vulkan ? createVulkanRenderer() : createGlRenderer(); }

// Backend by config name: "opengl", "vulkan", or "auto" = Vulkan when its preWindowSetup succeeds, else OpenGL with
// `note` saying why. Explicit names never fall back. Returns a renderer whose preWindowSetup succeeded (`chosen` = its
// name), or null with `error`. `create(vulkan)` makes a backend (createBackend; tests inject fakes).
std::unique_ptr<Renderer> selectRenderer(std::string_view name, const std::function<std::unique_ptr<Renderer>(bool)>& create,
                                         std::string& chosen, std::string& note, std::string& error);

} // namespace ou::render
