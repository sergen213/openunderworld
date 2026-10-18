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
};

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    uint32_t color = 0xFFFFFFFF; // RGBA8, little-endian
};

struct SubMesh {
    uint32_t firstIndex = 0, indexCount = 0;
    uint32_t textureId = 0;    // TextureData::id; 0 = untextured
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices; // triangle list
    std::vector<SubMesh> submeshes;
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major model matrix
    std::string origin;        // provenance, as for TextureData::origin
};

// A loaded set ready for rendering. `placeholder` = true when any content is synthetic.
struct SceneData {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    bool placeholder = false;
    std::vector<std::string> warnings; // unsupported / failed resources, shown in diagnostics
};

} // namespace ou::scene
