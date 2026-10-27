// See Water.hpp for the technique and the uniform layout. Shader sources live in
// src/render/shaders/ (embedded below for GL runtime compilation; Vulkan compiles the same
// files with glslc -DVULKAN=1 at build time).
#include "render/Water.hpp"

#include "render/RenderCommon.hpp"

#include <glm/gtc/matrix_inverse.hpp>

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
    const auto& r = options.ripples;
    w.ripple = {r.originX, r.originY, r.cellSize, r.surfaceZ};
    w.rippleInfo = {float(r.size), r.cellSize > 0 && r.size > 0 && !r.heights.empty() ? 1.0f : 0.0f, 0, 0};
    // TRU's seas and pools read blue-green; absorption removes red first (Beer-Lambert). [I]
    w.absorb = {0.045f, 0.22f, 0.27f, 1.0f / 2600.0f};
    w.shallow = {0.30f, 0.62f, 0.62f, 110.0f};
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
    return std::string(vulkan ? "#define VULKAN 1\n" : "") + kUnderwater;
}

} // namespace ou::render
