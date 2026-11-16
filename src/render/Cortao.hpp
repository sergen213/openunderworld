// CORTAO screen-space ambient occlusion (third_party/cortao, MIT; docs/RENDERING.md "Ambient occlusion"): the shared
// configuration both renderers build their pass parameters from. The pass structs mirror CORTAOParameters.h's
// CORTAOPrepassParameters / CORTAOMainPassParameters / CORTAOFilterPassParameters byte for byte (push constants / UBOs);
// Cortao.cpp is the only translation unit that includes CORTAOParameters.h (its functions are not inline).
#pragma once

#include <cstdint>

#include <glm/glm.hpp>

namespace ou::render::cortao {

// Shader permutation defines shared by every CORTAO pass of both renderers (CORTAO_PACKED_NORMAL_BITS, rays).
inline constexpr int kPackedNormalBits = 12;
// World-space radius [I]: 300 units ~ 0.95 m (3.16 units / cm), Lara-scale contact shadows; the screen-space cap
// (maxScreenspaceRadiusHeightRatio 0.2) takes over far away.
inline constexpr float kMaxRadius = 300.0f;

// FrameOptions::aoSamples -> CORTAO_SAMPLE_COUNT permutation (8 low, 12 medium, 16 high, 32 ultra).
inline int sampleCount(int aoSamples) { return aoSamples <= 8 ? 8 : aoSamples <= 12 ? 12 : aoSamples <= 16 ? 16 : 32; }
// CORTAO_RAY_COUNT for a sample count (occluder lookup table 24x24x16x{16,32}).
inline int rayCount(int samples) { return samples >= 32 ? 32 : 16; }

struct PrepassParams {
    uint32_t packedResolution;
    float depthScale, depthOffset;
    uint32_t randomSeed, mipCount, pad[3];
};
struct MainParams {
    uint32_t packedResolution, randomSeed;
    float normalBias, radius;
    float unproject[4], projection[4];
    float maxApparentCircleRadiusCoefficient, mipBias, inverseMaxSlopeCompensation, backfaceThreshold;
    float backfaceBoost;
    uint32_t pad[3];
};
struct FilterParams {
    uint32_t packedResolution;
    float depthSensitivity, normalSensitivity;
    uint32_t pad;
};
static_assert(sizeof(PrepassParams) == 32 && sizeof(MainParams) == 80 && sizeof(FilterParams) == 16);

struct Params {
    PrepassParams prepass;
    MainParams main;
    FilterParams filter;
};

// Mip levels of the packed data buffer: down to ~64 px on the short side, 1-8.
uint32_t mipCount(uint32_t width, uint32_t height);
// `projection`: the frame's GL-convention perspective (Camera::projection, FSR jitter included); depth buffer [0, 1],
// standard (not reversed). flipY: texture row 0 is the top of the image (Vulkan's negative-height viewport), false for GL.
Params params(uint32_t width, uint32_t height, uint32_t seed, const glm::mat4& projection, float nearZ, float farZ, bool flipY);

} // namespace ou::render::cortao
