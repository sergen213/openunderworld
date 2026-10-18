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

// A loaded set ready for rendering. `placeholder` = true when any content is synthetic.
struct SceneData {
    std::vector<MeshData> meshes;
    std::vector<TextureData> textures;
    std::vector<Skeleton> skeletons;
    bool placeholder = false;
    std::vector<std::string> warnings; // unsupported / failed resources, shown in diagnostics
};

} // namespace ou::scene
