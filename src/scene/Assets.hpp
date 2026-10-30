// Renderer-neutral CPU asset data. Format parsers (src/formats) produce these;
// renderers (src/render/*) upload them. No GPU or file-format types here.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ou::scene {

enum class PixelFormat : uint8_t { RGBA8, BGRA8, BC1, BC2, BC3 };

struct TextureMip {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> data; // tightly packed (BCn: 4x4 blocks)
};

struct TextureData {
    uint32_t id = 0;           // cdc texture section id
    PixelFormat format = PixelFormat::RGBA8;
    std::vector<TextureMip> mips; // mip 0 first
    std::string origin;        // e.g. "pc-w\\lara.drm#section 10", or "placeholder:checker"
    // Cube map: each mip's data holds the 6 faces back to back in D3D order +X -X +Y -Y +Z -Z (TEXTURES.md).
    bool cube = false;
};

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t color = 0xFFFFFFFF; // RGBA8, little-endian
    // Inputs of the original shaders (Original Fidelity path, docs/formats/SHADERS.md); zero when absent.
    float uv1[2] = {};           // second texcoord (cdc 0x8E54B6F3: light-map UV), scaled like uv
    uint32_t color0 = 0;         // cdc COLOR0 (0x7E7DD623) as D3D feeds it: RGBA8 bytes R,G,B,A
    float tangent[3] = {};       // cdc 0xF1ED11C3, [-1, 1]
    float binormal[3] = {};      // cdc 0x64A86F01, [-1, 1]
};

// Pass/blend state of a cdc material (blob +0x38, see docs/formats/MATERIALS.md). Alpha covers cdc
// modes 2-4 (SRCALPHA/INVSRCALPHA), Additive mode 5 (SRCALPHA/ONE), Average mode 8 (0.5/0.5 blend
// factor). Blended submeshes are drawn after opaque ones without depth writes.
enum class Blend : uint8_t { Opaque, Alpha, Additive, Average };

struct SubMesh {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t textureId = 0;    // TextureData::id; 0 = untextured
    Blend blend = Blend::Opaque;
    uint8_t alphaRef = 0;      // alpha test: keep texels with alpha >= alphaRef / 255; 0 = off
    bool doubleSided = false;  // draw back faces too
    uint32_t materialId = 0;   // cdc material section id (SceneData::materials[].id); 0 = none
    // Per-instance textures of material classes 1-4 (light map, "Setup" blend masks; SHADERS.md): terrain group
    // +0x10, model primgroup +0x2C, overridden by IMF placements (+0x4C). 0 = the material's own texture.
    uint32_t instanceTextures[4] = {};
    // Model primgroup +0x20 draw group: 0 = always drawn, n = hidden while bit n-1 of the instance's hidden-group
    // mask is set (tru.exe 0x4F50F0 / 0x5B4150; Lara's Thor gear: 10, 11 gauntlets, 12 belt).
    uint8_t drawGroup = 0;
    // Sun-shadow caster (shadow map + RT shadow rays): tru.exe queues a drawable for its shadow pass only when the
    // material blob +0x44 has 0x40000000 and blend mode +0x38 < 4, terrain groups also need +0x0C bit 2 clear
    // (models 0x50F7C0 / terrain 0x511E20; docs/formats/MATERIALS.md). True for non-cdc meshes.
    bool castsShadow = true;
    // Exterior terrain (Unit::exteriorTerrains: panorama cylinders, sky domes): tru.exe draws it in the far 2 % of the
    // depth range ([0.98, 1], the rest in [0, 0.98]; PCRenderTerrain +0xC8 -> 0x51D0A0), so it lies behind all other
    // geometry wherever it is placed. Renderers draw exterior submeshes first and then clear the depth.
    bool exterior = false;
};

// One D3D9 SM3 program of a TRU ShaderLib (formats/Shader.hpp parses it).
struct ShaderCode {
    uint64_t hash = 0;         // ShaderLib entry hash
    bool pixel = false;
    std::vector<uint8_t> bytecode;
};

// Material texture bound to sampler register `reg` (blob +0x54 entries, PCMaterial 0x505A90). `source` 1:
// engine-global table slot `cls` (9 back buffer, 10 depth buffer; textureId is the fallback); 2: per-instance
// texture SubMesh::instanceTextures[cls - 1] (light maps), textureId when the instance has none.
struct MaterialSampler {
    uint8_t reg = 0, source = 0, cls = 0;
    uint16_t filter = 0;  // cdc sampler/filter word (entry +0x0E) [U]
    uint32_t textureId = 0;
    uint32_t category = 0; // entry +0x08: 3 = normal map (distortion particles) [I], 0 / 1 colour
};

// cdc material (section type 10) for the translated original shaders: composite-pass programs and
// the constants PCMaterial uploads (docs/formats/MATERIALS.md, SHADERS.md).
struct Material {
    uint32_t id = 0;
    uint64_t vertexShader = 0, pixelShader = 0; // ShaderCode::hash in SceneData::shaders; 0 = none (generic path)
    std::vector<float> vsParams;  // 4 floats per register from c25 (MaterialParams)
    std::vector<float> psParams;  // 4 floats per register from c100 (MaterialParams)
    std::vector<MaterialSampler> samplers;
    uint8_t blendMode = 0;        // cdc blend mode (blob +0x38)
    uint8_t alphaThreshold = 0;   // blob +0x4C
    uint32_t flags = 0;           // blob +0x40 (0x04 = fogged: the VS gets fogScaleAndOffset)
};

// Point / spot light of a unit (0xE0-byte record + light DTP, tru.exe 0x5C6530 / PCRenderLight 0x50B910).
struct Light {
    uint8_t type = 0;            // 0 point, 1 spot
    bool shadow = false;         // DTP +0x144C: the engine binds it to a shadow-light slot (0-2)
    float position[3] = {};      // record row 3
    float direction[3] = {};     // record row 2: the way a spot points
    float color[3] = {};         // colour x intensity (record overrides +0x51 / +0x4C, else DTP +0x18 / +0x08)
    float radius = 0;            // record +0x58 when +0x5C, else DTP +0x04
    float coneInner = 0, coneOuter = 0; // spot: full cone angles, radians (DTP +0x0C / +0x10)
    float nearWidth = 0;         // spot: DTP +0x14, cone radius at the light position (apex moved back)
    std::vector<uint8_t> falloff; // 256 AttenuationSampler texels (R) over d / radius: DTP +0x1C floats x 255, last 0
    float flickerPeriod = 0;     // DTP +0x420 seconds when DTP +0x41C is set, else 0 (steady)
    std::vector<float> flicker;  // DTP +0x424: 256 intensity multipliers over one period
};

// Per-unit render settings (unit DTP at Level+0x70; SHADERS.md "Engine inputs"). valid = false: none loaded.
struct Environment {
    bool valid = false;
    float fogColor[3] = {};   // 0..1 (U+0x18 bytes)
    float fogNear = 0, fogFar = 0; // U+0x40, U+0x3C (0 -> U+0x38)
    float ambient[3] = {};    // g_Ambient: U+0x1E4 bytes x U+0x1E8 / 255
    // Sun: the unit's directional (type 2) light record (unit data +0x04 list; SHADERS.md, LEVELS.md "Lights").
    bool sun = false;
    float sunDirection[3] = {}; // record matrix row 2: the direction light travels (PS SunLight c209)
    float sunColor[3] = {};     // intensity x colour (c212)
    // Sun cookie ("modulation map", s11 via WorldToModulationMapSun c176/c177): texture id (0 = none), projected
    // along the sun's record rows 0/1 (sunAxes) from sunOrigin; DTP +0x82C scale, +0x830 rotation (deg/s),
    // +0x834/+0x838 scroll (1/s).
    uint32_t sunCookie = 0;
    float sunAxes[2][3] = {}, sunOrigin[3] = {};
    float cookieScale = 1, cookieRotation = 0, cookieScroll[2] = {};
    // Under water (docs/RENDERING.md "Under water"). Render settings +0x1C fog colour u8[3], +0x4C near, +0x48 far
    // (0 -> +0x38): the fog while the camera is below Lara's water surface (tru.exe 0x5C1F50).
    float waterFogColor[3] = {};
    float waterFogNear = 0, waterFogFar = 0;
    // +0x84 deep / +0x88 surface screen-filter DTPs, faded over +0x90 units below / +0x8C above the surface (0x5C78A0);
    // +0x68 / +0x6C event lists run on Lara when the camera goes under / comes out (0x5C7CA0). 0 = none.
    uint32_t waterFilter[2] = {}, waterList[2] = {};
    float waterFilterRange[2] = {}; // [0] below, [1] above
    // Full-screen water pass: the unit data +0x160 / +0x164 record with +0x84 bit 0, drawn by 0x6D8C20 with its
    // +0x74 material; that material's PS MaterialParams c100 = surface colour + alpha at the eye, c101 = deep colour +
    // full-fog distance. waterPass[1].w = 0: none.
    float waterPass[2][4] = {};
    std::vector<Light> lights; // point and spot lights (type 0/1 records), unit frame
};

// Optional skinning. Up to 4 influences per vertex; `bone` indexes Skeleton::bones.
struct SkinInfluence {
    uint16_t bone[4] = {};
    uint8_t weight[4] = {};    // sum 255; 0 = unused slot
};

// Bind pose has no rotations: a bone's bind position is the sum of the pivots up its chain.
struct Bone {
    int32_t parent = -1;       // index into Skeleton::bones, -1 = root
    float pivot[3] = {};       // bind offset from the parent
};

struct Skeleton {
    std::vector<Bone> bones;   // parents come before children
    uint32_t renderModelId = 0; // cdc render-model section the skeleton deforms
    std::string origin;
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangle list
    std::vector<SubMesh> submeshes;
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major model matrix
    std::string origin;        // provenance, as for TextureData::origin
    std::vector<SkinInfluence> skin; // empty, or one per vertex (vertices are the bind pose)
    int32_t skeleton = -1;     // SceneData::skeletons index when skinned
};

// Drops m's primgroups of the draw groups whose bit (group - 1) is set in `hidden` (SubMesh::drawGroup). Returns false
// when that left nothing of a mesh that had primgroups: drop the mesh then, as renderers draw a mesh without submeshes
// whole and untextured (lara_jungle_heavy's group-11 gauntlet is a mesh of its own: a white hand).
inline bool hideDrawGroups(MeshData& m, uint32_t hidden) {
    if (m.submeshes.empty()) return true;
    std::erase_if(m.submeshes, [&](const SubMesh& s) { return s.drawGroup && (hidden >> ((s.drawGroup - 1) & 31) & 1); });
    return !m.submeshes.empty();
}

// A loaded set ready for rendering. `placeholder` = true when any content is synthetic.
struct SceneData {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<Skeleton> skeletons;
    std::vector<Material> materials; // referenced by SubMesh::materialId
    std::vector<ShaderCode> shaders; // referenced by Material::{vertex,pixel}Shader (hash), unique
    Environment environment;         // of the (first) level unit
    bool placeholder = false;
    std::vector<std::string> warnings; // unsupported / failed resources, shown in diagnostics
};

} // namespace ou::scene
