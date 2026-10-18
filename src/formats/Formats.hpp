// Parsers for TRU PC texture (PCD9) and render-model ("Mesh") sections into the
// renderer-neutral ou::scene types. Evidence and open questions: docs/formats/TEXTURES.md,
// docs/formats/MESHES.md.
#pragma once

#include "cdc/Cdc.hpp"
#include "scene/Assets.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::formats {

// ---- Textures (section type 5, "PCD9") ------------------------------------------------
struct PcdHeader {
    uint32_t format = 0;      // 0x15 = A8R8G8B8 (bytes B,G,R,A), or FourCC DXT1/DXT3/DXT5
    uint32_t dataSize = 0;    // bytes of pixel data following the 24-byte header
    uint32_t paletteSize = 0; // 0 in every PC texture
    uint16_t width = 0, height = 0;
    uint8_t depth = 0;        // 32 for 0x15, 0 for DXT
    uint8_t mipCount = 0;     // number of levels - 1
    uint16_t flags = 0;       // 0x8000 = cube map (6 faces)
    bool cube() const { return flags & 0x8000; }
};
std::optional<PcdHeader> parsePcdHeader(std::span<const uint8_t> sectionData);

// Decodes a type-5 section. BCn stays compressed; 0x15 is returned as BGRA8.
// Returns nullopt for non-PCD9 data, unknown formats, inconsistent sizes and cube maps
// (scene::TextureData has no cube support; see `why`).
std::optional<scene::TextureData> decodeTexture(const cdc::Section& s, std::string* why = nullptr);

// CPU decode of one mip level to RGBA8 (for dumps/tests). Supports all scene formats.
std::vector<uint8_t> toRgba8(scene::PixelFormat f, uint32_t width, uint32_t height,
                             std::span<const uint8_t> data);

// ---- Models (section type 12, resource type 26, "Mesh") -------------------------------
// Decodes every render model and terrain of the DRM. Vertices are converted to scene::Vertex
// (skinning data dropped; skinned meshes are returned in bind pose; the cdc colour attribute is a
// baked-lighting input, not albedo, and is not copied). One MeshData per model batch / terrain
// vertex buffer. SubMesh::textureId = the material's category-1 (diffuse) texture, else its
// category-0 slot-0 texture (heuristic), else 0; blend/alphaRef/doubleSided = materialState().
// Batches without texcoords and material textures (occlusion/fog volumes, hypothesis) are
// skipped with a warning.
std::vector<scene::MeshData> decodeModels(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                          std::string_view origin = {});
// Same meshes keyed by render-resource section id (failed/skipped resources map to no meshes).
std::map<uint32_t, std::vector<scene::MeshData>> decodeRenderResources(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                                                       std::string_view origin = {});

// Texture ids of a material section (type 10), in reference order, with their category.
struct MaterialTexture { uint32_t textureId, category, slot; };
std::vector<MaterialTexture> materialTextures(const cdc::Section& material);
// Pass state of a material section: blob +0x38 blend mode, +0x40 flags (0x80 = two-sided),
// +0x4C alpha threshold, applied as PCStateManager_SetBlendMode (tru.exe 0x527A80) does.
struct MaterialState { scene::Blend blend = scene::Blend::Opaque; uint8_t alphaRef = 0; bool doubleSided = false; };
MaterialState materialState(const cdc::Section& material);

// ---- Scene ----------------------------------------------------------------------------
// Loads `drmPath` (e.g. "pc-w\\lara.drm", "pc-w\\l4_pool.drm"), its object dependencies
// ("pc-w\\<name>.drm") when `withDependencies` (all placed at the origin), decodes every model and every texture of
// the loaded DRMs that a submesh references. Missing/unsupported data → warnings.
// Throws cdc::FormatError if `drmPath` itself is not in the bigfile.
scene::SceneData loadDrmScene(const cdc::BigfileSet& bigfile, std::string_view drmPath,
                              bool withDependencies = false);

// ---- Levels (unit DRMs). Layouts and evidence: docs/formats/LEVELS.md ----------------
// One intro (placed object instance) of a unit: 0x90-byte record.
struct Intro {
    float rotation[3]; // Euler radians; matrix = Rx(x) * Ry(y) * Rz(z) (cdc_Matrix_Build_XYZOrder 0x49D500)
    float position[3];
    float scale[3];    // only applied when (flags[1] & 0x10), else 1 (INSTANCE_IntroduceInstance 0x5BCC30)
    uint16_t objectId; // objectlist.txt id; 0xFFFA (-6) = marker, no object
    int32_t uniqueId;
    uint8_t flags[4];  // bytes +0x70..+0x73
    bool scaled() const { return flags[1] & 0x10; }
};
inline constexpr uint16_t kMarkerObjectId = 0xFFFA;

// Instanced static mesh ("IMF") placement from the unit's cell group data: 0x70-byte record.
struct ImfPlacement {
    float matrix[16];  // column-major model matrix (stored D3D row-major, row 3 = translation)
    uint32_t dtpId;    // DTPDATA section {?, renderModel id @+4}
    std::string name;
};

struct Unit {
    std::vector<Intro> intros;
    std::vector<std::string> streamGroups; // stream DRM base names ("pc-w\\streamgroups\\<name>.drm")
    std::vector<ImfPlacement> imfs;
};
// Parses the level struct (section 0) of a unit DRM. Throws cdc::FormatError on layout mismatch.
Unit parseUnit(const cdc::Drm& unit);

// objectlist.txt ("count\r\n" then "id,name" lines) -> id → lower-case name.
std::map<uint16_t, std::string> parseObjectList(std::string_view text);

// Render-model resource ids of an object DRM's models, in Object model-list order (0 = no render
// model). Object section (type 11) +0 → ObjectBase DTP; +0xA0 u16 numModels, +0xA4 Model**;
// Model DTP +0x64 → render model. Empty if the DRM has no object section.
std::vector<uint32_t> objectRenderModels(const cdc::Drm& object);
// ObjectBase +0 flags ("oflags") of an object DRM; 0 if it has no object section.
// INSTANCE_IntroduceInstance (0x5BCC30) sets instance flag 0x800 (not drawn) for kObjectHiddenAtBirth.
uint32_t objectFlags(const cdc::Drm& object);
inline constexpr uint32_t kObjectHiddenAtBirth = 0x800000;

// Column-major T * Rx * Ry * Rz * S, as tru.exe builds instance matrices.
void introMatrix(const Intro& in, float out[16]);

// Whole level: unit terrain, stream-group DRM terrain, IMF placements and intro instances
// (model 0 of each object, placed with the intro transform). Objects hidden at birth
// (objectFlags & kObjectHiddenAtBirth: trigger/sound volumes, markers) are skipped. Textures as
// loadDrmScene. Unresolved objects, missing DRMs and objects without a model are reported as warnings.
scene::SceneData loadLevelScene(const cdc::BigfileSet& bigfile, std::string_view unitPath);

} // namespace ou::formats
