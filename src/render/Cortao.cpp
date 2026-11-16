#include "render/Cortao.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "CORTAOParameters.h" // third_party/cortao/src: only included here (non-inline definitions)

namespace ou::render::cortao {

static_assert(sizeof(PrepassParams) == sizeof(CORTAO::CORTAOPrepassParameters));
static_assert(sizeof(MainParams) == sizeof(CORTAO::CORTAOMainPassParameters));
static_assert(sizeof(FilterParams) >= sizeof(CORTAO::CORTAOFilterPassParameters));

uint32_t mipCount(uint32_t width, uint32_t height) {
    const uint32_t shortSide = std::max(1u, std::min(width, height));
    return std::clamp(uint32_t(std::bit_width(shortSide / 64u)), 1u, 8u);
}

Params params(uint32_t width, uint32_t height, uint32_t seed, const glm::mat4& projection, float nearZ, float farZ, bool flipY) {
    CORTAO::CORTAOConfigurationParameters c{};
    c.width = width, c.height = height;
    c.mipCount = mipCount(width, height);
    c.randomSeed = seed;
    c.flipY = flipY;
    c.isPerspective = true;
    const glm::mat4 inverse = glm::inverse(projection);
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            c.projectionMatrix[col][row] = projection[col][row], c.inverseProjectionMatrix[col][row] = inverse[col][row];
    c.nearPlane = nearZ, c.farPlane = farZ;
    c.maxRadius = kMaxRadius;
    // The remaining tunables keep CORTAO's documented defaults (maxScreenspaceRadiusHeightRatio 0.2, mipBias -4,
    // maxSlopeCompensation 4, backfaceThreshold 0.5, backfaceBoost 0, filter depth / normal sensitivity 512 / 128).
    CORTAO::CORTAOPrepassParameters pre;
    CORTAO::CORTAOMainPassParameters main;
    CORTAO::CORTAOFilterPassParameters filter;
    CORTAO::populatePrepassParameters(c, &pre);
    CORTAO::populateMainPassParameters(c, &main);
    CORTAO::populateFilterPassParameters(c, &filter);
    Params p{};
    std::memcpy(&p.prepass, &pre, sizeof pre);
    std::memcpy(&p.main, &main, sizeof main);
    std::memcpy(&p.filter, &filter, sizeof filter);
    return p;
}

} // namespace ou::render::cortao
