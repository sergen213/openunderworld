// See Water.hpp for the technique and the uniform layout. Shader sources live in
// src/render/shaders/ (embedded below for GL runtime compilation; Vulkan compiles the same
// files with glslc -DVULKAN=1 at build time).
#include "render/Water.hpp"

#include "render/RenderCommon.hpp"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cstring>

namespace ou::render {

namespace {
const char* kWaterVert =
#include "water.vert.inc"
    ;
const char* kWaterFrag =
#include "water.frag.inc"
    ;
const char* kSplashVert =
#include "splash.vert.inc"
    ;
const char* kSplashFrag =
#include "splash.frag.inc"
    ;
const char* kUnderwater =
#include "underwater.glsl.inc"
    ;
const char* kAtmosphere =
#include "atmosphere.glsl.inc"
    ;
const char* kFilter =
#include "filter.glsl.inc"
    ;
} // namespace

bool isWaterSurface(std::string_view meshOrigin) { return meshOrigin.find("watersurface") != std::string_view::npos; }

bool cameraUnderwater(const Camera& camera, std::span<const WaterVolume> volumes) {
    for (const auto& v : volumes) {
        const glm::vec3 p = camera.position;
        if (p.x >= v.min[0] && p.x <= v.max[0] && p.y >= v.min[1] && p.y <= v.max[1] && p.z >= v.min[2] &&
            p.z <= v.max[2])
            return true;
    }
    return false;
}

WaterBlock waterBlock(const Camera& camera, Viewport vp, const glm::mat4& viewProj, const scene::Environment& env,
                      const FrameOptions& options) {
    WaterBlock w{};
    w.viewProj = viewProj;
    w.invViewProj = glm::inverse(viewProj);
    w.eye = glm::vec4(camera.position, options.time);
    w.screen = {float(vp.width), float(vp.height), 1.0f / vp.width, 1.0f / vp.height};
    w.sunDir = glm::vec4(sunToward(env), 1.0f);
    w.sunColor = glm::vec4(env.sun ? glm::vec3(env.sunColor[0], env.sunColor[1], env.sunColor[2]) : glm::vec3(1), 0.6f);
    const glm::vec3 fog = env.valid ? glm::vec3(env.fogColor[0], env.fogColor[1], env.fogColor[2]) : glm::vec3(0.08f, 0.09f, 0.11f);
    w.fogColor = glm::vec4(fog, cameraUnderwater(camera, options.waterVolumes) ? 1.0f : 0.0f);
    w.fogRange = {env.valid ? env.fogNear : 1000.0f, env.valid && env.fogFar > env.fogNear ? env.fogFar : 30000.0f, 1.0f, 1.0f};
    if (const Underwater* u = options.underwater) {
        if (u->fog) {
            w.fogColor = glm::vec4(u->fogColor[0], u->fogColor[1], u->fogColor[2], w.fogColor.w);
            w.fogRange.x = u->fogNear, w.fogRange.y = std::max(u->fogFar, u->fogNear + 1.0f);
        }
        if (u->strength > 0 && u->pass[1][3] > 0) {
            w.waterPass[0] = glm::vec4(u->pass[0][0], u->pass[0][1], u->pass[0][2], u->pass[0][3]);
            w.waterPass[1] = glm::vec4(u->pass[1][0], u->pass[1][1], u->pass[1][2], u->pass[1][3]);
            w.waterPass[2].x = u->strength;
        }
    }
    if (const ScreenFilter* f = options.screenFilter; f && f->active()) {
        w.filter[0] = {f->alphaA, f->blurA, f->dof ? 1.0f : 0.0f, f->colourMatrix ? 1.0f : 0.0f};
        w.filter[1] = {f->tintA[0], f->tintA[1], f->tintA[2], f->finishMatrix ? 1.0f : 0.0f};
        w.filter[2] = {f->amountC, f->blurC, f->dofScale, f->dofBias};
        w.filter[3] = {f->tintAdd[0], f->tintAdd[1], f->tintAdd[2], 0};
        w.filter[4] = {f->tintMix[0], f->tintMix[1], f->tintMix[2], f->tintMix[3]};
        w.filter[5] = {f->feedbackKeep, f->feedbackAmount, f->feedbackBlur, f->feedbackZoom[0]};
        w.filter[6] = {f->feedbackColour[0], f->feedbackColour[1], f->feedbackColour[2], f->feedbackAngle};
        w.filter[7] = {f->feedbackZoom[1], f->noiseAlpha, f->noiseCells, 0};
        w.filter[8] = {f->noiseColour[0], f->noiseColour[1], f->noiseColour[2], f->noiseSeed};
        std::memcpy(w.filterLut, f->lut.data(), sizeof w.filterLut);
    } else {
        w.filter[5] = {1, 0, 0, 1}, w.filter[7] = {1, 0, 0, 0}; // feedback off: keep the scene
    }
    const auto& r = options.ripples;
    w.ripple = {r.originX, r.originY, r.cellSize, r.surfaceZ};
    w.rippleInfo = {float(r.size), r.cellSize > 0 && r.size > 0 && !r.heights.empty() ? 1.0f : 0.0f, 0, 0};
    // TRU's seas and pools read blue-green; absorption removes red first (Beer-Lambert). [I]
    w.absorb = {0.045f, 0.22f, 0.27f, 1.0f / 2600.0f};
    w.shallow = {0.30f, 0.62f, 0.62f, 110.0f};

    const glm::vec3 eye = camera.position;
    // Water volumes, nearest first (box distance): the atmosphere ends its rays on their surfaces (the Enhanced
    // water pass does not write depth) and the debug view marks them.
    std::vector<std::pair<float, const WaterVolume*>> vols;
    for (const auto& v : options.waterVolumes) {
        const glm::vec3 lo(v.min[0], v.min[1], v.min[2]), hi(v.max[0], v.max[1], v.max[2]);
        vols.push_back({glm::distance(glm::clamp(eye, lo, hi), eye), &v});
    }
    std::sort(vols.begin(), vols.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t nv = std::min<size_t>(vols.size(), 8);
    for (size_t i = 0; i < nv; ++i) {
        w.volumes[2 * i] = {vols[i].second->min[0], vols[i].second->min[1], vols[i].second->min[2], 0};
        w.volumes[2 * i + 1] = {vols[i].second->max[0], vols[i].second->max[1], vols[i].second->max[2], 0};
    }
    const float debug = float(std::clamp(options.waterDebug, 0, kWaterDebugViews - 1));
    w.detail = {options.waterDetail ? 1.0f : 0.0f, debug, options.waterDetail ? 6.0f : 0.0f, float(nv)};
    if (options.waterDetail) { // the four lamps nearest the camera, reflected by the water [I radius cut]
        std::vector<std::pair<float, const scene::Light*>> lamps;
        for (const auto& l : env.lights)
            if (l.radius > 0) lamps.push_back({glm::distance(glm::vec3(l.position[0], l.position[1], l.position[2]), eye), &l});
        std::sort(lamps.begin(), lamps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < std::min<size_t>(lamps.size(), 4); ++i) {
            const auto& l = *lamps[i].second;
            w.lamps[i] = {l.position[0], l.position[1], l.position[2], l.radius};
            w.lampColor[i] = {l.color[0], l.color[1], l.color[2], 0};
        }
    }
    // Atmosphere [I values, tuned on l2 jungle / l5 Jan Mayen / l4 pool]: haze over the whole view thinning with
    // height above the camera's floor, plus a denser ground mist hugging the lowest water surface or the ground.
    const int level = int(options.atmosphere);
    if (level > 0) {
        const float floorZ = nv ? std::min(eye.z, w.volumes[1].z) : eye.z - 250.0f;
        w.haze = {1.0f / 60000.0f, 1.0f / 2200.0f, floorZ, float(level)};
        const bool sun = env.valid && env.sun; // mist.w: 0 no sun, 1 sun scattering, 2 + shafts
        const float flags = !sun ? 0.0f : options.lightShafts && level >= 2 ? 2.0f : 1.0f;
        w.mist = {1.0f / 30000.0f, 1.0f / 300.0f, floorZ, flags};
    }
    using K = Weather::Kind;
    if (options.weather.kind != K::None && options.weather.intensity > 0)
        w.weather = {options.weather.kind == K::Rain ? 1.0f : 2.0f, std::clamp(options.weather.intensity, 0.0f, 1.0f),
                     options.weather.wind[0], options.weather.wind[1]};
    return w;
}

namespace {
std::string withVersion(const char* source, bool vulkan) {
    (void)vulkan; // the files carry #version 460; glslc defines VULKAN for the descriptor syntax
    return source;
}
} // namespace

std::string waterVertexSource(bool vulkan) { return withVersion(kWaterVert, vulkan); }
std::string waterFragmentSource(bool vulkan) { return withVersion(kWaterFrag, vulkan); }

std::vector<SplashVertex> splashVertices(const Camera& camera, std::span<const WaterParticle> splashes) {
    std::vector<SplashVertex> out;
    out.reserve(splashes.size() * 6);
    const glm::vec3 right = camera.right(), up = glm::cross(camera.forward(), right);
    for (const auto& p : splashes) {
        if (p.alpha <= 0 || p.radius <= 0) continue;
        const glm::vec3 c(p.pos[0], p.pos[1], p.pos[2]);
        const glm::vec3 r = right * p.radius, u = up * p.radius;
        const glm::vec3 corners[4] = {c - r - u, c + r - u, c + r + u, c - r + u};
        const float uvs[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        for (int i : {0, 1, 2, 0, 2, 3}) {
            const glm::vec3& q = corners[i];
            out.push_back({{q.x, q.y, q.z}, {uvs[i][0], uvs[i][1]}, p.alpha});
        }
    }
    return out;
}

std::string splashVertexSource(bool vulkan) { return withVersion(kSplashVert, vulkan); }
std::string splashFragmentSource(bool vulkan) { return withVersion(kSplashFrag, vulkan); }

std::string underwaterPostSource(bool vulkan) {
    return std::string(vulkan ? "#define VULKAN 1\n" : "") + kUnderwater + kAtmosphere + kFilter;
}

} // namespace ou::render
