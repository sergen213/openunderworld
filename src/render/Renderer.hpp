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
// kGlyph/kAtlas*), colour alpha-masked by the glyph; 2 <= u < 4: FrameOptions::uiImages[0] at (u - 2, v); 4 <= u < 6:
// uiImages[1] at (u - 4, v); u >= 6: uiImages[2] at (u - 6, v); images are RGBA x colour.
struct TextVertex {
    float x, y, u, v; // pixels from top-left
    uint32_t color;   // RGBA8
};

// Optional hardware ray tracing (Vulkan ray queries, Enhanced only). Full = shadows + water reflections.
enum class RayTracing : uint8_t { Off, Shadows, Full };
// Temporal upscaler of the Vulkan backend.
enum class Upscaler : uint8_t { Off, Fsr };

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
// waves: the sea's VS InstanceParams 3-6 (c60-c63) this frame, tru.exe 0x69DFB0 (rows: x and y wave numbers, phases,
// heights; the surface is lowered by sum heights x (1 - cos(x c60 + y c61 + c62))); zero = flat (original::fillWaves).
struct WaterVolume { float min[3], max[3]; float waves[4][4] = {}; };
// Splash / spray droplet: world position, radius (world units), opacity 0..1; drawn as a camera-facing soft disc.
struct WaterParticle { float pos[3]; float radius; float alpha; };
// ---- original particle effects (src/fx, docs/formats/EFFECTS.md) ----
// Camera-facing sprite corner; colour bytes R,G,B,A with 0x80 = 1.0 (shaders scale by 2), drawn tex * colour.
struct FxVertex { float pos[3]; float uv[2]; uint32_t rgba; };
// `count` vertices (triangle list) from `first`; textureId = SceneData texture id (0 = white); blend:
// 0 alpha (SRCALPHA/INVSRCALPHA), 1 additive (SRCALPHA/ONE), 2 subtract (REVSUBTRACT), 3 multiply (DESTCOLOR/SRCALPHA).
// Material particles (fx::ParticleMaterial): distort > 0 draws the scene copy offset by normalId's xy (lib-21 decode:
// (x, y * a) * 2 - 1) x the texture's alpha x distort screen uv, alpha = texture alpha x colour alpha (blend 0);
// softScale > 0 fades alpha by sat((scene view depth - fragment view depth - (1 - texture alpha) softBias) softScale).
struct FxBatch {
    uint32_t first = 0, count = 0, textureId = 0;
    uint8_t blend = 0;
    uint32_t normalId = 0;
    float distort = 0, softScale = 0, softBias = 0;
    bool screen() const { return distort > 0 || softScale > 0; } // reads the scene colour / depth copies
};
// Original screen filters (class-5 effects with a 0x31C0 filter resource, docs/formats/EFFECTS.md "Screen filters"),
// summed over the running ones by fx::System::screenFilter. Post pass, on the tonemapped scene:
//   scene = M1(scene) when colourMatrix; scene = mix(scene, M2(blur(scene, blurA)) x tintA, alphaA [x DOF curve]);
//   scene += MF(blur(scene, blurC)) x amountC;
//   scene = scene x feedbackKeep + blur(scene, feedbackBlur) zoomed / turned about the centre x feedbackColour x
//   feedbackAmount (PCFeedbackEffect); scene += tintAdd; scene = mix(scene, tintMix.rgb, tintMix.a);
//   scene = mix(scene, noiseColour x noise, noiseAlpha) (PCNoiseEffect: 10 x 8 cells of `noiseCells` / 64 of a noise
//   texture at random offsets, `noiseSeed` per frame).
// blur(r): the half-resolution scene at mip level r (trilinear, as tru.exe's ceil(r) down/up chain with the fraction
// blended). M = 3x3 matrix of 256-entry curves: lut row 3k + c, texel x = the curve input channel c's value, RGB = its
// contributions (rows 0-2 M1, 3-5 M2, 6-8 MF, 9 the DOF curve in R over sat(view depth x dofScale + dofBias)).
struct ScreenFilter {
    static constexpr int kLutRows = 10;
    bool colourMatrix = false, dof = false, finishMatrix = false;
    float alphaA = 0, blurA = 0;           // ColorDOF overlay 0..1, blur 0..4
    float tintA[3] = {1, 1, 1};
    float dofScale = 0, dofBias = 0;
    float amountC = 0, blurC = 0;          // Finish (bloom): additive 0..1, blur 0..4
    float feedbackKeep = 1, feedbackAmount = 0, feedbackBlur = 0, feedbackZoom[2] = {1, 1}, feedbackAngle = 0;
    float feedbackColour[3] = {1, 1, 1};
    float tintAdd[3] = {0, 0, 0}, tintMix[4] = {0, 0, 0, 0};
    float noiseAlpha = 0, noiseCells = 0, noiseColour[3] = {1, 1, 1}, noiseSeed = 0;
    std::array<uint8_t, 256 * kLutRows * 4> lut{}; // RGBA8, row-major
    bool feedback() const { return feedbackAmount > 0 || feedbackKeep < 1; }
    bool active() const {
        return colourMatrix || alphaA > 0 || amountC > 0 || feedback() || noiseAlpha > 0 || tintMix[3] > 0 ||
               tintAdd[0] + tintAdd[1] + tintAdd[2] > 0;
    }
};
// ---- atmosphere + weather (docs/RENDERING.md "Atmosphere", "Weather"; Enhanced only, post pass) ----
// Height fog (haze + ground mist) integrated along the view ray up to the scene or the nearest water surface,
// plus screen-space sun shafts from Medium up. Off = the post pass leaves the image untouched.
enum class Atmosphere : uint8_t { Off, Low, Medium, High };
// Screen-space precipitation layers, depth-tested against the scene; rain adds splash rings on upward surfaces.
struct Weather {
    enum class Kind : uint8_t { None, Rain, Snow } kind = Kind::None;
    float intensity = 0; // 0..1, already reduced where the camera is sheltered
    float wind[2] = {0.25f, 0.1f}; // world xy drift per unit of fall
};

struct RgbaImage {
    std::span<const uint8_t> rgba; // tightly packed RGBA8, top row first
    int width = 0, height = 0;
    uint64_t version = 0; // backends upload only when this changes
};

// Opacity of one uploaded mesh (SceneData::meshes index) for a frame: instance fades (fx::System::fade, tru.exe instance
// +0x1B4). As tru.exe's translucent pass 0x5066A0 draws a fading instance: opaque materials alpha-blended (mode 4) with
// alpha = opacity [I: the opacity PS variant's alpha], other materials with their own blend and alpha x opacity; all
// without depth writes, after the opaque ones. 0 = not drawn.
struct MeshOpacity { uint32_t mesh = 0; float opacity = 1; };
// Scene meshes [firstMesh, firstMesh + meshCount) of one streamed unit draw only inside `rect` (NDC x0, y0, x1, y1, y up)
// in the camera passes; an empty rect hides them. Neighbour units are seen through their stream portals (RenderCommon
// portalClips). Shadow / ray-traced passes ignore it.
struct UnitClip { uint32_t firstMesh = 0, meshCount = 0; float rect[4] = {-1, -1, 1, 1}; };
// Under water (docs/RENDERING.md "Under water"): the camera unit's look for this frame, filled by the app from
// scene::Environment while the camera is below Lara's water surface or less than 600 units above it (tru.exe 0x6D9DA0).
struct Underwater {
    float strength = 0;    // full-screen pass alpha scale: 1 below the surface, 1 - height / 600 above; 0 = no pass
    float pass[2][4] = {}; // scene::Environment::waterPass (surface rgb + alpha at the eye, deep rgb + full-fog distance)
    bool fog = false;      // below the surface: this fog replaces the unit fog (TRU's shaders, water, atmosphere)
    float fogColor[3] = {}, fogNear = 0, fogFar = 0;
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
    std::array<RgbaImage, 3> uiImages;
    // Replaces the sun shadow map when the backend supports it; otherwise the shadow map is used.
    RayTracing rt = RayTracing::Off;
    int rtRays = 2; // sun-cone shadow rays per pixel (then averaged over the 2x2 pixel quad)
    float brightness = 1.0f; // display gamma of the post pass: out = in^(1 / brightness); 1 = neutral
    float screenFade = 0.0f; // scripted screen wipe (globaleffect FadeOut/FadeIn, script::Runtime::screenFade): 1 = black
    WaterQuality water = WaterQuality::Original;  // ou::config presets choose (Enhanced by default)
    WaterRipples ripples;                          // empty = calm (procedural waves only)
    std::span<const WaterVolume> waterVolumes;     // loaded units' water volumes
    std::span<const WaterParticle> splashes;       // drawn blended after the water surfaces
    // Moving lights (the flashlight): replace the first size() entries of the uploaded scene's environment lights for
    // this frame. The app reserves those slots in SceneData::environment (same falloff curves, so the attenuation LUT
    // already has their rows); colour 0 = off.
    std::span<const scene::Light> dynamicLights;
    bool waterDetail = false; // Enhanced water extras: swell, flow, glitter, shadowed sun path, thickness SSR, lamps
    int waterDebug = 0;       // Enhanced water debug view (render/Water.hpp kWaterDebugViews); 0 = off
    Atmosphere atmosphere = Atmosphere::Off;
    bool lightShafts = true;  // sun shafts of Atmosphere Medium / High
    Weather weather;
    // AMD FSR (Vulkan only, docs/RENDERING.md "FSR"): the scene renders at the quality mode's resolution with sub-pixel
    // jitter and FSR upscales it (MSAA and FXAA are then off); the UI is drawn after the upscale. frameGeneration also
    // presents one generated frame between real ones. Backends without FSR ignore it (Renderer::upscalerSummary).
    Upscaler upscaler = Upscaler::Off;
    int fsrQuality = 1;           // 0 native AA, 1 quality (1.5x), 2 balanced (1.7x), 3 performance (2x), 4 ultra (3x)
    float sharpness = 0.5f;       // FSR sharpening (RCAS) 0..1, 0 = off
    bool frameGeneration = false;
    // Original effects, drawn after the water surfaces / splashes, depth-tested, no depth writes, batches in order.
    std::span<const FxVertex> fxVertices;
    std::span<const FxBatch> fxBatches;
    const ScreenFilter* screenFilter = nullptr; // original class-5 screen filters (post pass); nullptr / inactive = none
    std::span<const MeshOpacity> meshOpacity; // unlisted meshes: 1
    std::span<const UnitClip> unitClips;     // meshes in no range: unclipped
    const Underwater* underwater = nullptr;  // nullptr = above water
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
    // One overlay line on upscaling: requested / active mode, resolutions, frame generation, or why not.
    virtual std::string upscalerSummary() const { return "FSR: Vulkan only"; }
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
