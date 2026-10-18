// D3D9 shader-model-3 bytecode (formats/Shader.hpp) -> GLSL 4.50 for the Original Fidelity path of both
// backends (docs/formats/SHADERS.md "Translation"). One source compiles for OpenGL (glCompileShader) and,
// with `#define OU_VULKAN`, for Vulkan (shaderc). Backend-free: no GPU API types.
#pragma once

#include "formats/Shader.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ou::render::sm3 {

// Register files the backends provide (std140 blocks; see kGlslInterface in the .cpp, mirrored here):
//   frame   (VK set 0 binding 0, GL UBO binding 0): vec4 vs[256]; vec4 ps[224]; ivec4 vsI[16]; ivec4 psI[16];
//                                                  uvec4 bools (x = vs b0-15, y = ps b0-15); vec4 screen (w, h, 1/w, 1/h)
//   material (VK set 1 binding 0, GL UBO binding 1): vec4 vs[40] = c25..c64; vec4 ps[40] = c100..c139;
//                                                  vec4 fixed = (c2 AlphaThreshold, c98 MaterialOpacity, alphaRef,
//                                                  flags: 1 = fogged (VS c22 live), 2 = additive (PS c0 FogColor = 0))
//   object  (VK push constant, GL UBO binding 2): vec4 v[8] = vs c0..c7 (WorldViewProject, World columns)
//   samplers s0..s15: VK set 1 binding 1 + n, GL texture unit n.
// Register -> block: VS c0-7 object, c25-64 material (MaterialParams + InstanceParams c57), else frame; PS c2/c98 material.fixed, c100-139 material (MaterialParams + InstanceParams c132),
// else frame. `def` constants shadow every block (D3D9 semantics).
inline constexpr int kFrameVs = 256, kFramePs = 224, kMaterialVsBase = 25, kMaterialVs = 40, kMaterialPsBase = 100,
                     kMaterialPs = 40, kObjectVs = 8;
struct FrameBlock {
    float vs[kFrameVs][4];
    float ps[kFramePs][4];
    int32_t vsI[16][4];
    int32_t psI[16][4];
    uint32_t bools[4];
    float screen[4];
};
struct MaterialBlock {
    float vs[kMaterialVs][4];
    float ps[kMaterialPs][4];
    float fixed[4]; // AlphaThreshold (c2), MaterialOpacity (c98), fixed-function alpha ref (<0 = off), flags (see above)
};
struct ObjectBlock { float v[kObjectVs][4]; };
static_assert(sizeof(FrameBlock) == (kFrameVs + kFramePs + 16 + 16 + 1 + 1) * 16);
static_assert(sizeof(MaterialBlock) == (kMaterialVs + kMaterialPs + 1) * 16);

// Vertex attribute locations of translated vertex shaders (by D3DDECLUSAGE); unlisted inputs read (0, 0, 0, 1).
enum Attr : int { kAttrPosition = 0, kAttrNormal = 1, kAttrTexcoord0 = 2, kAttrColor0 = 4, kAttrTexcoord1 = 5,
                  kAttrTangent = 6, kAttrBinormal = 7 };

struct Glsl {
    std::string source;  // empty on failure
    std::string error;
    uint16_t samplers = 0;       // bit n: s<n> is sampled
    uint8_t samplerType[16] = {}; // D3DSAMPLER_TEXTURE_TYPE >> 27 (2 = 2D, 3 = cube, 4 = volume)
    uint8_t attributes = 0;      // bit n: location n read (VS)
};

// `vulkan` only selects the #define in the prelude; the body is identical.
Glsl translate(const formats::d3d9::Program& p, bool vulkan);

// GLSL -> SPIR-V with libshaderc (target Vulkan 1.3, or OpenGL 4.5 to validate the GL flavour offline).
// Empty result + `error` on failure. Thread-safe.
std::vector<uint32_t> compileSpirv(const std::string& glsl, bool pixel, bool vulkan, std::string& error);

} // namespace ou::render::sm3
