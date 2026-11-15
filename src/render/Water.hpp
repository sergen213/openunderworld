// Enhanced water: shared uniform block and GLSL for both backends (Original water keeps TRU's shaders).
// The surface look is smallcableboi's Godot water (CC0: Fresnel, scrolling wave layers, absorption, traced / simple
// refraction, traced SSR, edge foam, Snell's window) with afl_ext's procedural ocean waves (MIT) on open seas, plus the
// interactive wave field (game::WaveField), splash droplets and the underwater post effect. docs/RENDERING.md "Water".
#pragma once

#include "render/Renderer.hpp"
#include "scene/Assets.hpp"

#include <glm/glm.hpp>

#include <array>
#include <string>
#include <string_view>

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
    glm::vec4 detail;      // x 1 = water detail, y debug view, z Godot height-layer amplitude (water.vert), w volume count
    glm::vec4 lamps[4];    // nearest point lights: xyz world, w radius (0 = unused); water reflects them
    glm::vec4 lampColor[4];
    glm::vec4 haze;        // x density per unit, y height falloff per unit, z base height (world z), w atmosphere level
    glm::vec4 mist;        // x density per unit, y height falloff per unit, z base height, w 0 no sun, 1 sun, 2 + shafts
    glm::vec4 weather;     // x kind (0 none, 1 rain, 2 snow), y intensity, zw wind xy
    glm::vec4 volumes[16]; // water volumes: [2i] min, [2i + 1] max (max.z = surface), nearest first
    // Original screen filters (FrameOptions::screenFilter; post pass, shaders/filter.glsl): [0] (alphaA, blurA, dof,
    // colourMatrix), [1] (tintA rgb, finishMatrix), [2] (amountC, blurC, dofScale, dofBias), [3] additive tint, [4] alpha
    // tint, [5] (feedback keep, amount, blur, zoom x), [6] (feedback rgb, angle), [7] (zoom y, noise alpha, noise cells,
    // previous frame copied: set by the renderer, 1 when last frame's feedback copy exists), [8] (noise rgb, seed); off =
    // all 0 except feedback keep / zoom 1.
    glm::vec4 filter[9];
    glm::uvec4 filterLut[ScreenFilter::kLutRows * 64]; // ScreenFilter::lut, 4 RGBA8 texels per element
    glm::vec4 waterPass[3]; // FrameOptions::underwater: [0] surface rgb + alpha at the eye, [1] deep rgb + distance,
                            // [2] x strength (0 = off), y light-ray weight (Underwater::rayWeight); both water qualities
    glm::uvec4 rays[157];   // Underwater::rays, 4 texels per uint (ouRays)
    glm::mat4 seaWaves;     // render::waterWaves at the camera: tru.exe's sea waves c60-c63 (zero = no sea; caustics)
    // Enhanced water colour (FrameOptions::waterColor, render::waterOptics; all 0 = the unit's own colours): [0] rgb
    // extinction per unit of the view path, w 1 = on; [1] rgb in-scatter hue (linear, luminance 1: the unit's deep
    // colour keeps its brightness); [2] rgb downwelling attenuation per unit (caustics fade with depth, red first).
    glm::vec4 optics[3];
    // FFT ocean (render/Ocean.hpp; shaders/waves.glsl ouOcean*): [0] x 1 = the uOcean array holds this frame's sea
    // (else open seas fall back to afl_ext's waves), y its mip level (grid 128: 0, 64: 1), z grid N, w the swell +
    // gravity height RMS (m); [1] xyz the cascades' tiles (world units), w whitecap strength.
    glm::vec4 ocean[2];
};
static_assert(sizeof(WaterBlock) == 2 * 64 + 9 * 16 + 28 * 16 + 9 * 16 + ScreenFilter::kLutRows * 1024 + 3 * 16 + 157 * 16 + 64 + 3 * 16 + 2 * 16);
inline constexpr int kWaterDebugViews = 6; // 0 off, 1 normals, 2 water depth, 3 reflection hits, 4 sun glint, 5 fresnel

// Enhanced water colour (docs/RENDERING.md "Water colour"). NeuralSpecOcean's bio-optical palettes (MIT, Sheng Choi;
// demo.js WATER_PALETTES, "calibrated to Morel & Prieur (1977) & Jerlov (1976) Case-1/Case-2 IOPs"): per-metre RGB
// absorption a and scatter colour b (their scatterColor, the backscatter-driven body colour), plus our clear pool
// (Pope & Fry 1997 pure-water absorption at 650 / 550 / 450 nm [I band picks]).
struct WaterOpticsPreset {
    std::string_view name; // config::kWaterColors spelling
    glm::vec3 absorption;  // 1/m
    glm::vec3 scatter;     // 1/m
};
const WaterOpticsPreset* waterPreset(WaterColor c); // nullptr for Original / Auto
// Chapter (game::kChapters index) -> optical type chosen for its setting [I]: prologue / Croft Manor clear, the
// Mediterranean and the Andaman Sea's open water pelagic cobalt, Coastal Thailand a coral lagoon, the Southern Mexico
// cenotes mineral jade, Jan Mayen / the Arctic Sea / the epilogue subpolar slate; anything else Original.
WaterColor chapterWaterColor(int chapter);
// The setting resolved for a chapter: Auto -> chapterWaterColor(chapter), the rest unchanged.
WaterColor waterColorFor(WaterColor setting, int chapter);
// config::GraphicsSettings::waterColor (config::kWaterColors index: 0 auto, 1 original, 2.. presets) -> the setting.
WaterColor waterColorSetting(int configIndex);
std::string_view waterColorName(WaterColor c); // "original", "emerald", ..., "auto"
// WaterBlock::optics for a resolved colour (all zero for Original): extinction a x 1.16 (NeuralSpecOcean's surface
// transmittance exp(-a path 1.16)) and downwelling Kd = a x (0.22, 0.34, 0.48) (their syncKdAttenuation), both per
// TRU unit (290 per metre) and divided by kWaterClarity; in-scatter hue b / (a + b) normalised to luminance 1.
inline constexpr float kWaterClarity = 4.0f; // [I] game visibility: real ocean IOPs would hide a reef 10 m down
std::array<glm::vec4, 3> waterOptics(WaterColor c);

bool cameraUnderwater(const Camera& camera, std::span<const WaterVolume> volumes);
WaterBlock waterBlock(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const scene::Environment& env,
                      const FrameOptions& options);

// tru.exe's under-water light rays (RENDERING.md "Under water"): the water record's grid of light columns (0x6D6640:
// the four side walls of every masked cell), hanging from the waved surface along the refracted sun (VS lib 403
// slot 32), drawn additively into a 50 x 50 target (0x6D8C20, material +0x68, PS lib 402) that the +0x6C material adds
// over the screen. Each column corner relights on its own clock (0x6D7CB0). The app steps it and draws it on the CPU.
struct UnderwaterRays {
    static constexpr int kSize = 50;
    scene::Environment::WaterRays rec;
    std::vector<glm::vec2> life;  // per top vertex: lit from, to (s); the envelope peaks at a quarter, ends at half
    std::vector<float> darkUntil; // per top vertex: no new life before then
    float time = 0;
    size_t cursor = 0;
    uint32_t rng = 1;
    void reset(const scene::Environment::WaterRays& r);
    void step(float dt); // 0x6D7CB0: 100 corners per call
    // Draws into out (Underwater::rays); false = nothing drawn (no grid, the camera above depthZ[0]). The caller only
    // asks with the camera fully below the surface. waves: the water volume's (WaterVolume::waves).
    bool draw(const Camera& camera, float aspect, const float (&waves)[4][4], std::array<uint8_t, 2500>& out);
};

// Water surface shaders (same mesh vertex layout as the scene pass).
std::string waterVertexSource(bool vulkan);
std::string waterFragmentSource(bool vulkan);
// Splash droplets as liquid (screen-space fluid rendering, docs/RENDERING.md "Water"; technique and shaders adapted
// from talvinckb/OpenGL-Water-Simulation, MIT). Camera-facing quads expanded on the CPU (SplashVertex), then:
//  1 splat:     splash.vert + splash_splat.frag -> RG32F target cleared to (kSplashEmpty, 0), blend MAX (factors ONE),
//               no depth attachment (the shader tests the opaque depth copy: GL unit 6 / VK set 0 binding 1).
//  2 blur:      splash_screen.vert + splash_blur.frag, twice (dir (1,0) then (0,1); GL uniform location 0, VK push
//               constant vec2), RG32F -> RG32F, no blending. Input: GL unit 10 / VK set 0 binding 1.
//  3 composite: splash_screen.vert + splash_composite.frag into the scene colour, blend ONE, ONE_MINUS_SRC_ALPHA,
//               no depth test. Input: GL unit 10 / VK set 0 binding 1.
// All three read the WaterBlock prefix up to `ambient` (GL UBO binding 4 / VK set 0 binding 0); targets are render-size.
struct SplashVertex {
    float pos[3], uv[2], alpha;
};
inline constexpr float kSplashEmpty = -1e20f;
std::vector<SplashVertex> splashVertices(const Camera& camera, std::span<const WaterParticle> splashes);
std::string splashVertexSource(bool vulkan);
std::string splashSplatSource(bool vulkan);
std::string splashScreenVertexSource(bool vulkan);
std::string splashBlurSource(bool vulkan);
std::string splashCompositeSource(bool vulkan);
// Enhanced under-water particles (docs/RENDERING.md "Under water: marine snow and bubbles"; Noniv/abyssal-garden's
// vsSnow / fsSnow / vsBub / fsBub, MIT, ported literally in its "ag space"): with Enhanced water (uwParticlesOn), drawn
// after the scene's effects into a render-size RGBA16F target cleared to 0, blend ONE, ONE_MINUS_SRC_ALPHA (snow: alpha
// 0 = additive), no depth attachment (the fragment shaders test the opaque depth copy and fog themselves by their own
// distance); the post pass then composites it over the fogged scene in linear light: c = c (1 - a) + rgb (VK post set 1
// binding 9 + push `uw` bit 0; GL unit 17 + uniform location 5 bit 0).
//  snow:    uw_snow.vert + uw_snow.frag, no vertex input, 6 vertices x params.x instances (a 44 m box around the eye).
//  bubbles: uw_bubble.vert + uw_bubble.frag, one Bubble per instance (location 0 vec4 pos + radius, 1 vec4 alpha, seed,
//           rise, wobble).
// The fragment shaders read the post pass's water set (VK set 1: WaterBlock, opaque depth; GL UBO 4, unit 6) and the
// sun shadow (VK set 2; GL UBO 3, unit 16); UwParticlePush is VK push constants (vertex + fragment) / GL UBO binding 7.
struct UwParticlePush {
    glm::mat4 viewProj; // the backend's (as WaterBlock::viewProj)
    glm::vec4 eye;      // xyz camera, w time (WaterBlock::eye.w)
    glm::vec4 right;    // xyz camera right
    glm::vec4 up;       // xyz camera up
    glm::vec4 params;   // x snow instances, y surface z of the eye's water volume, z units per metre
};
static_assert(sizeof(UwParticlePush) == 128); // Vulkan's guaranteed push-constant space
inline constexpr int kUwSnowCount = 9000; // abyssal-garden world.js: new Float32Array(9000 * 4)
inline bool uwParticlesOn(const WaterBlock& wb) { return wb.fogColor.w > 0.5f; } // the backends zero it for Original water
UwParticlePush uwParticlePush(const Camera& camera, const WaterBlock& wb);
std::string uwSnowVertexSource(bool vulkan);
std::string uwSnowFragmentSource(bool vulkan);
std::string uwBubbleVertexSource(bool vulkan);
std::string uwBubbleFragmentSource(bool vulkan);
// Enhanced under-water composite (docs/RENDERING.md "Under water"; abyssal-garden's W_BLIT bloom + W_POST, MIT): with
// the eye under the surface of its water volume (`on`), the post pass writes the linear HDR frame (fogged scene, shafts,
// particles; no tone map; push / uniform `uw` bit 1) into a render-size RGBA16F target, uw_bloom.frag builds the demo's
// 6-level bloom chain from it (W >> (i + 1); down0, down x5, up x5 additive), and uw_grade.frag writes the display frame
// (bloom mix, exposure, vignette, ACES fit, S-curve, gamma, dither) instead of the post pass's output. Both fullscreen
// with the post vertex shader; VK: their own layout (set 0 / set 1: one combined image sampler each, push vec4 =
// (bloom mode, exposure, screen fade, strength)); GL: units 0 / 1, uniform location 0 = that vec4.
struct UwGrade {
    bool on = false;
    float exposure = 1, strength = 0; // main.js m[56] = EXPO (0.6 + 0.4 ek); the pass strength (WaterBlock waterPass[2].x)
};
UwGrade uwGrade(const WaterBlock& wb);
inline constexpr int kUwBloomLevels = 6; // main.js NB
std::string uwBloomSource();
std::string uwGradeSource();
// Post pass: declarations + `vec3 ouUnderwater(vec3 c, vec2 uv)` and `vec3 ouAtmosphere(vec3 c, vec2 uv)`
// (shaders/atmosphere.glsl: height fog, sun shafts, precipitation). Both sample uDepth at uv themselves and return
// `c` unchanged when their effect is off (Original output stays byte-identical).
std::string underwaterPostSource(bool vulkan);
// Enhanced under-water sun shafts (shaders/underwater.glsl, docs/RENDERING.md "Under water"; after abyssal-garden, MIT):
// three passes before the post pass, drawn with the post layout / units (fullscreen triangle): 0 the surface's focusing
// into a kUnderwaterShaftTile^2 R16F tile, 1 the half-resolution march (RGBA16F: light + view depth; VK push constant /
// GL uniform 0 = steps), 2 its depth-aware blur (RGBA16F, half resolution), which the post pass upsamples. VK post set 1
// bindings 6 (march output), 7 (blur output), 8 (tile); GL units 13, 14, 15.
inline constexpr int kUnderwaterShaftTile = 512; // = underwater.glsl kShaftTile
// The passes run this frame (= underwater.glsl ouShaftsOn on the uploaded block); `block` is the frame's WaterBlock.
bool underwaterShaftsActive(const WaterBlock& block);
// March steps: 0 with graphics.light_shafts off (the passes still run and add nothing), 32 at atmosphere High, else 20.
int underwaterShaftSteps(const FrameOptions& options);
std::string underwaterShaftSource(int pass); // GL fragment source of pass 0, 1 or 2 (includes spliced)
// Enhanced water's animated water texture (shaders/uw_water.frag: abyssal-garden's W_WATER, r = caustic, ba = surface
// slope; docs/RENDERING.md "Under water"): kAgWaterSize^2 RGBA16F with kAgWaterLevels mips, repeat / trilinear /
// anisotropic 8, regenerated every frame Enhanced water is drawn (camera above or below: the bed caustics use it too):
// a fullscreen triangle into mip 0 (VK push constant / GL uniform 0 = WaterBlock eye.w, the demo's time), then the mips
// box-filtered down (the demo's fsMip; VK linear blits, GL glGenerateTextureMipmap). Sampled as `uAgWater`: VK post set
// 1 binding 10 and water set 0 binding 5; GL unit 18 (post, water, shafts, particles).
inline constexpr int kAgWaterSize = 512, kAgWaterLevels = 5; // = uw_water.frag kAgWaterSize; shaders.js WATER (high)
std::string agWaterSource(); // GL fragment source

} // namespace ou::render
