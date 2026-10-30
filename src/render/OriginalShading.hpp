// CPU side of the Original Fidelity path shared by both backends (docs/formats/SHADERS.md "Engine inputs"):
// translation of each material's composite-pass programs, and the register files the engine would upload.
#pragma once

#include "render/Camera.hpp"
#include "render/Renderer.hpp"
#include "render/ShaderTranslate.hpp"
#include "scene/Assets.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

namespace ou::render::original {

// D3D-convention view-projection (clip z in [0, w]) of `camera`: the WorldViewProject / ViewProject the shaders expect.
glm::mat4 viewProjD3D(const Camera& camera, float aspect);

// Frame registers: transforms, time, fog, ambient, sun, sun cookie, light table (FrameBlock layout: ShaderTranslate.hpp).
// `flipY`: the backend's screen copies are stored bottom-up (GL), so ScreenExtents maps vPos.y to 1 - v.
// `underwater` with fog: its fog replaces the unit's (FrameOptions::underwater).
void fillFrame(sm3::FrameBlock& f, const Camera& camera, Viewport vp, float timeSeconds, const scene::Environment& env,
               bool flipY = false, const Underwater* underwater = nullptr);
// The point/spot light table (FrameBlock::lights, first kMaxLights of env.lights) at time t (flicker); part of fillFrame.
void fillLights(sm3::FrameBlock& f, const scene::Environment& env, float timeSeconds);
// Material registers: MaterialParams, InstanceParams (zero, as tru.exe's default 0x848F78), AlphaThreshold, opacity.
void fillMaterial(sm3::MaterialBlock& m, const scene::Material& mat, const scene::SubMesh& sub);
// Light set of one object: env.lights indices for PS light slots 0-5, one byte each (0xFF = empty).
struct LightSet { uint32_t packed[2] = {~0u, ~0u}; };
// The strongest lights whose radius reaches the world-space sphere (center, radius).
LightSet lightSet(const scene::Environment& env, glm::vec3 center, float radius);
// World-space bounding sphere (xyz centre, w radius) of vertices under `model` (lightSet input; skinned meshes
// recompute it in updateVertices).
glm::vec4 boundingSphere(std::span<const scene::Vertex> vertices, const glm::mat4& model);
// Object registers: WorldViewProject (c0-3) and World (c4-7), as matrix columns, the object's light set and opacity.
// PS InstanceParams 0-2 (c132-c134, ObjectBlock::inst) stay zero; reflective draws set them (Reflection::rows).
void fillObject(sm3::ObjectBlock& o, const glm::mat4& viewProjD3D, const glm::mat4& model, LightSet lights = {}, float opacity = 1);
// VS InstanceParams 3-6 (ObjectBlock::waves): the waves of the first waved volume whose xy box the mesh's bounds
// (`center`, `radius`) reach (tru.exe attaches them to the water volume's sea drawable; only the sea programs read
// c60-c63). The bounds, not the centre: l2_cliffclimb's sea is part of one 176000-radius terrain mesh centred outside it.
void fillWaves(sm3::ObjectBlock& o, std::span<const WaterVolume> volumes, glm::vec3 center, float radius);
// AttenuationSampler row of each env.lights entry (distinct falloff curves share rows 0-95).
std::vector<int> falloffRows(const scene::Environment& env);

// Engine-owned textures bound to the high samplers when no material entry claims them.
// s11 = sun cookie (Environment::sunCookie, else white); s12/s13 spot cookies stay white (not bound per object).
inline constexpr int kAttenuationSampler = 15, kShadowMapSampler = 14, kSunCookieSampler = 11;
struct EngineTexture { int width = 0, height = 0; std::vector<uint8_t> rgba; };
// AttenuationSampler (s15): 256x128 curve atlas (tru.exe PCLightManager, filled by 0x504A60): rows 0-95 light
// falloff curves (R, one per distinct env.lights curve), rows 96+ fog (R fog curve, G height-fog curve);
// v = (row + 0.5) / 128. Per scene: backends rebuild it in upload().
EngineTexture attenuationLut(const scene::Environment& env = {});

// Translated programs of a scene. `materialProgram`: material id -> programs index (-1 / absent = generic path).
struct Program {
    uint64_t vs = 0, ps = 0;
    sm3::Glsl vsGlsl, psGlsl;
};
struct Prepared {
    std::vector<Program> programs;
    std::unordered_map<uint32_t, int> materialProgram;
    std::unordered_map<uint32_t, const scene::Material*> materials; // points into the SceneData passed to prepare()
    size_t translated = 0, fallback = 0;
    std::vector<std::string> warnings;
    int program(uint32_t materialId) const {
        const auto it = materialProgram.find(materialId);
        return it == materialProgram.end() ? -1 : it->second;
    }
};
Prepared prepare(const scene::SceneData& scene, bool vulkan);

// Texture id for sampler register `reg` of a material, with a submesh's per-instance textures
// (SubMesh::instanceTextures) applied; 0 when the material has no entry for `reg`.
uint32_t samplerTexture(const scene::Material& mat, std::span<const uint32_t> instanceTextures, int reg);
// Samplers that read engine render targets: low 16 bits = back buffer (the scene drawn so far), high 16 = depth buffer.
// - Global table entries (texture entry source 1, FUN_00505A90's per-view table): class 9 = BackBufferTexture,
//   class 10 = DepthBufferTexture [O: CTAB names] (l2_cliffclimb sea, the l1_seatop yacht's glass).
// - Water drawables (PS reads DepthToW c77): instance entries (source 2) the submesh supplies no texture for, class 2 =
//   back buffer, class 3 = depth [I: the l1_seatop sea material 5990 samples them at ScreenExtents coordinates and
//   decodes class 3 with DepthFactors; which drawable passes the targets was not traced].
// Their submeshes are drawn after the opaque scene from single-sample copies.
uint32_t screenSamplers(const scene::Material& mat, std::span<const uint32_t> instanceTextures, bool readsDepth);

// Planar reflection (tru.exe 0x5C5B80 registers a reflecting drawable's plane via 0x5C5890, "Multiple reflection planes
// visible at once!"; 0x5CE800 renders the scene from the camera mirrored in it, 0x5C5740, into the target 0x5CAE70
// creates with vfunc 0xCC(50, 50, 25), half the screen; bound as engine texture class 5, device 0x507150 slot 0).
// Samplers reading it: global table entries (source 1) of class 5 (the l1_seatop / l2_cliffclimb sea).
uint32_t reflectionSamplers(const scene::Material& mat);
// World z of the plane of m's reflecting submeshes (their vertices' mean under `model`); NaN when none reflects.
float reflectionPlaneZ(const scene::MeshData& m, const glm::mat4& model,
                       const std::unordered_map<uint32_t, const scene::Material*>& materials);
struct Reflection {
    Camera camera;      // mirrored in z = planeZ, oblique near plane on the water (Camera::clipPlane)
    float rows[3][4];   // PS c132-c134 of the reflecting draws (direction -> target uv, projective)
};
// `flipY`: the target's rows are stored bottom-up (GL).
Reflection reflection(const Camera& camera, float aspect, float planeZ, bool flipY);

} // namespace ou::render::original
