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

// Frame registers: transforms, time, fog, ambient, lights (FrameBlock layout: ShaderTranslate.hpp).
void fillFrame(sm3::FrameBlock& f, const Camera& camera, Viewport vp, float timeSeconds, const scene::Environment& env);
// Material registers: MaterialParams, InstanceParams (zero, as tru.exe's default 0x848F78), AlphaThreshold, opacity.
void fillMaterial(sm3::MaterialBlock& m, const scene::Material& mat, const scene::SubMesh& sub);
// Object registers: WorldViewProject (c0-3) and World (c4-7), as matrix columns.
void fillObject(sm3::ObjectBlock& o, const glm::mat4& viewProjD3D, const glm::mat4& model);

// Engine-owned textures bound to the high samplers when no material entry claims them.
inline constexpr int kAttenuationSampler = 15, kShadowMapSampler = 14;
struct EngineTexture { int width = 0, height = 0; std::vector<uint8_t> rgba; };
// AttenuationSampler (s15): 256x128 curve atlas (tru.exe PCLightManager, filled by 0x504A60): rows 0-95 light
// falloff curves (R), rows 96+ fog (R fog curve, G height-fog curve); v = (row + 0.5) / 128.
EngineTexture attenuationLut();

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

} // namespace ou::render::original
