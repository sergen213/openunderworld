#include "render/Snow.hpp"

#include <cmath>

namespace ou::render::snow {

Map map(glm::vec3 eye, glm::vec3 forward) {
    Map m;
    const glm::vec2 f(forward);
    const glm::vec2 centre = glm::vec2(eye) + (glm::dot(f, f) > 1e-6f ? glm::normalize(f) * (kAhead * kExtent) : glm::vec2(0));
    m.window = glm::ivec2(glm::floor((centre - kExtent * 0.5f) / kMaskTexel));
    m.origin = glm::vec2(m.window) * kMaskTexel;
    m.zLow = eye.z - kBelow, m.zHigh = eye.z + kAbove;
    const float span = m.zHigh - m.zLow, s = 2.0f / kExtent;
    m.viewProj = glm::mat4(1.0f);
    m.viewProj[0][0] = s, m.viewProj[1][1] = s, m.viewProj[2][2] = -2.0f / span;
    m.viewProj[3] = glm::vec4(-m.origin.x * s - 1.0f, -m.origin.y * s - 1.0f, 2.0f * m.zHigh / span - 1.0f, 1.0f);
    return m;
}

MaskPush maskPush(const Map& m, const SnowLayer& s) {
    MaskPush p;
    p.map = {m.origin, 1.0f / kExtent, m.zLow};
    p.z = {m.zHigh - m.zLow, kMaskTexel, float(kMaskSize), kSurfaceStep};
    p.window = {m.window, 0, 0};
    p.rate = {s.grow * s.dt, s.melt * s.dt, s.seed, 0.0f};
    return p;
}

void fillBlock(SunShadowBlock& b, const Map& m, const SnowLayer* s, const scene::Environment& env, const Underwater* underwater,
               const Fog* fog) {
    if (!s) {
        b.snowMap = b.snowZ = b.snowLight = b.laraSnow = b.laraBody = b.snowFog = b.snowFogFar = glm::vec4(0);
        return;
    }
    b.snowMap = {m.origin, 1.0f / kExtent, 1.0f};
    b.snowZ = {m.zLow, m.zHigh - m.zLow, kMaskTexel, float(kMaskSize)};
    b.snowLight = {s->light[0], s->light[1], s->light[2], s->sparkle};
    b.laraSnow = {s->lara[0], s->lara[1], s->lara[2], s->lara[3]};
    b.laraBody = {s->laraFeet[0], s->laraFeet[1], s->laraFeet[2], s->laraHeight};
    // As original::fillFrame's VS c22 / PS c0: the frame's fog, else under water its water fog, else the unit's.
    b.snowFog = {env.fogColor[0], env.fogColor[1], env.fogColor[2], env.fogNear};
    b.snowFogFar = {env.fogFar, 0, 0, 0};
    if (underwater && underwater->fog)
        b.snowFog = {underwater->fogColor[0], underwater->fogColor[1], underwater->fogColor[2], underwater->fogNear},
        b.snowFogFar.x = underwater->fogFar;
    if (fog) b.snowFog = {fog->color[0], fog->color[1], fog->color[2], fog->nearZ}, b.snowFogFar.x = fog->farZ;
    if (!env.valid && !fog && !(underwater && underwater->fog)) b.snowFogFar.x = 0; // no fog
}

const char* maskComputeSource() {
    return
#include "snow_mask.comp.inc"
        ;
}

} // namespace ou::render::snow
