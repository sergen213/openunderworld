// Parsers for TRU PC texture (PCD9) and render-model ("Mesh") sections into the
// renderer-neutral ou::scene types. Evidence and open questions: docs/formats/TEXTURES.md,
// docs/formats/MESHES.md.
#pragma once

#include "cdc/Cdc.hpp"
#include "scene/Assets.hpp"

#include <cstdint>
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
// (skinning data dropped; skinned meshes are returned in bind pose). One MeshData per model
// batch / terrain vertex buffer. SubMesh::textureId = the material's category-1 (diffuse)
// texture, else its category-0 slot-0 texture (heuristic), else 0. Batches without texcoords
// and material textures (occlusion/fog volumes, hypothesis) are skipped with a warning.
std::vector<scene::MeshData> decodeModels(const cdc::Drm& drm, std::vector<std::string>& warnings,
                                          std::string_view origin = {});

// Texture ids of a material section (type 10), in reference order, with their category.
struct MaterialTexture { uint32_t textureId, category, slot; };
std::vector<MaterialTexture> materialTextures(const cdc::Section& material);

// ---- Scene ----------------------------------------------------------------------------
// Loads `drmPath` (e.g. "pc-w\\lara.drm", "pc-w\\l4_pool.drm"), its object dependencies
// ("pc-w\\<name>.drm") when `withDependencies` (all placed at the origin), decodes every model and every texture of
// the loaded DRMs that a submesh references. Missing/unsupported data → warnings.
// Throws cdc::FormatError if `drmPath` itself is not in the bigfile.
scene::SceneData loadDrmScene(const cdc::Bigfile& bigfile, std::string_view drmPath,
                              bool withDependencies = false);

} // namespace ou::formats
