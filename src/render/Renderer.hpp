// Backend interface. Apps and scene code talk to this; only src/render/<backend> sees the GPU API.
#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <string>
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

// Screen-space quad vertex of the text/UI pass (font atlas: RenderCommon kGlyph/kAtlas*).
struct TextVertex {
    float x, y, u, v; // pixels from top-left; u < 0 = solid quad
    uint32_t color;   // RGBA8
};

// Optional hardware ray tracing (Vulkan ray queries, Enhanced only). Full = shadows + water reflections.
enum class RayTracing : uint8_t { Off, Shadows, Full };

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
    std::span<const TextVertex> ui = {}; // screen-space triangle list (src/ui menus, HUD), drawn before the overlay
    // Replaces the sun shadow map when the backend supports it; otherwise the shadow map is used.
    RayTracing rt = RayTracing::Off;
    int rtRays = 2; // sun-cone shadow rays per pixel (then averaged over the 2x2 pixel quad)
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

} // namespace ou::render
