#include "render/RenderCommon.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

#include <SDL3/SDL.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ou::render {

std::vector<uint8_t> buildFontAtlas() {
    std::vector<uint8_t> out(kAtlasW * kAtlasH, 0);
    SDL_Surface* surf = SDL_CreateSurface(kAtlasW, kAtlasH, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return out;
    if (SDL_Renderer* r = SDL_CreateSoftwareRenderer(surf)) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        for (int c = 32; c < 127; ++c) {
            const char s[2] = {static_cast<char>(c), 0};
            int i = c - 32;
            SDL_RenderDebugText(r, float(i % kAtlasCols * kGlyph), float(i / kAtlasCols * kGlyph), s);
        }
        SDL_RenderPresent(r);
        const auto* px = static_cast<const uint8_t*>(surf->pixels);
        for (int y = 0; y < kAtlasH; ++y)
            for (int x = 0; x < kAtlasW; ++x) out[y * kAtlasW + x] = px[y * surf->pitch + x * 4];
        SDL_DestroyRenderer(r);
    }
    SDL_DestroySurface(surf);
    return out;
}

std::vector<TextVertex> buildOverlayVertices(Viewport vp, std::span<const std::string> lines, std::span<const TextVertex> ui) {
    const float scale = float(std::max(1, vp.height / 800)); // 2x glyphs on 1440p+ / 4K
    const float g = kGlyph * scale, lineH = g + 2 * scale, pad = 4 * scale;
    std::vector<TextVertex> v(ui.begin(), ui.end());
    auto quad = [&](float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, uint32_t c) {
        v.insert(v.end(), {{x0, y0, u0, v0, c}, {x1, y0, u1, v0, c}, {x1, y1, u1, v1, c},
                           {x0, y0, u0, v0, c}, {x1, y1, u1, v1, c}, {x0, y1, u0, v1, c}});
    };
    float y = pad;
    for (const auto& raw : lines) {
        bool hi = !raw.empty() && raw[0] == '!';
        std::string_view s = hi ? std::string_view(raw).substr(1) : std::string_view(raw);
        uint32_t color = hi ? 0xFF40C0FFu : 0xFFFFFFFFu; // 0xAABBGGRR: amber / white
        quad(pad - 2 * scale, y - scale, pad + s.size() * g + 2 * scale, y + g + scale, -1, -1, -1, -1, 0xA0000000u);
        float x = pad;
        for (char ch : s) {
            int i = (ch >= 32 && ch < 127) ? ch - 32 : '?' - 32;
            float u0 = float(i % kAtlasCols * kGlyph) / kAtlasW, v0 = float(i / kAtlasCols * kGlyph) / kAtlasH;
            quad(x, y, x + g, y + g, u0, v0, u0 + float(kGlyph) / kAtlasW, v0 + float(kGlyph) / kAtlasH, color);
            x += g;
        }
        y += lineH;
    }
    return v;
}

size_t blockBytes(scene::PixelFormat format) {
    switch (format) {
    case scene::PixelFormat::BC1: return 8;
    case scene::PixelFormat::BC2:
    case scene::PixelFormat::BC3: return 16;
    default: return 0;
    }
}

uint32_t usableMips(const scene::TextureData& t, std::string& problem) {
    if (t.mips.empty() || t.mips[0].width == 0 || t.mips[0].height == 0) { problem = "no mip 0"; return 0; }
    const size_t block = blockBytes(t.format);
    const uint32_t w0 = t.mips[0].width, h0 = t.mips[0].height;
    uint32_t levels = 0;
    for (const auto& m : t.mips) {
        uint32_t w = std::max(1u, w0 >> levels), h = std::max(1u, h0 >> levels);
        size_t expect = (block ? size_t((w + 3) / 4) * ((h + 3) / 4) * block : size_t(w) * h * 4) * (t.cube ? 6 : 1);
        if (m.width != w || m.height != h || m.data.size() != expect) break;
        ++levels;
    }
    if (levels == 0) problem = "mip 0 size does not match its dimensions";
    else if (levels < t.mips.size())
        problem = "using " + std::to_string(levels) + " of " + std::to_string(t.mips.size()) + " mips (bad chain)";
    return levels;
}

Bounds meshBounds(const scene::MeshData& m) {
    const glm::mat4 t = glm::make_mat4(m.transform);
    Bounds b;
    for (const auto& v : m.vertices) {
        const glm::vec3 p(t * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
        b.lo = glm::min(b.lo, p);
        b.hi = glm::max(b.hi, p);
    }
    return b;
}

Bounds sceneBounds(const scene::SceneData& scene) {
    Bounds b;
    for (const auto& m : scene.meshes) {
        const Bounds mb = meshBounds(m);
        b.lo = glm::min(b.lo, mb.lo);
        b.hi = glm::max(b.hi, mb.hi);
    }
    return b;
}

glm::vec3 sunToward(const scene::Environment& env) {
    const glm::vec3 d(env.sunDirection[0], env.sunDirection[1], env.sunDirection[2]);
    return env.valid && env.sun && glm::dot(d, d) > 1e-12f ? -glm::normalize(d) : kSunDirection;
}

bool outsideOrtho(const glm::mat4& viewProj, glm::vec3 center, float radius) {
    for (int i = 0; i < 2; ++i) {
        const glm::vec3 row(viewProj[0][i], viewProj[1][i], viewProj[2][i]);
        const float d = glm::dot(row, center) + viewProj[3][i], e = radius * glm::length(row) + 1e-3f;
        if (d - e > 1.0f || d + e < -1.0f) return true;
    }
    return false;
}

glm::mat4 shadowViewProj(const Camera& camera, float aspect, const Bounds& scene, glm::vec3 toSun, float sliceNear, float sliceFar) {
    // Bounding sphere of the view frustum slice; a sphere keeps the box size fixed while the camera
    // turns, which together with texel snapping keeps shadow edges stable.
    const glm::mat4 proj = camera.projection(aspect);
    const glm::mat4 invViewProj = glm::inverse(proj * camera.view());
    auto ndcZ = [&](float d) { // perspective depth of view distance d
        const glm::vec4 c = proj * glm::vec4(0, 0, -std::clamp(d, camera.nearZ, camera.farZ), 1);
        return c.z / c.w;
    };
    const float z0 = ndcZ(sliceNear), z1 = ndcZ(sliceFar);
    glm::vec3 corners[8];
    glm::vec3 center(0.0f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 p = invViewProj * glm::vec4(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? z1 : z0, 1);
        corners[i] = glm::vec3(p) / p.w;
        center += corners[i] / 8.0f;
    }
    float radius = 0;
    for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
    radius = std::ceil(radius);

    const glm::vec3 up = std::abs(toSun.z) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0), -toSun, up);
    glm::vec3 c = glm::vec3(lightView * glm::vec4(center, 1));
    const float texel = 2 * radius / float(kShadowMapSize);
    c.x = std::floor(c.x / texel) * texel;
    c.y = std::floor(c.y / texel) * texel;
    // Depth range: the whole scene along the light, plus the slice itself.
    float zMin = c.z - radius, zMax = c.z + radius;
    if (!scene.empty())
        for (int i = 0; i < 8; ++i) {
            const glm::vec3 p(i & 1 ? scene.hi.x : scene.lo.x, i & 2 ? scene.hi.y : scene.lo.y, i & 4 ? scene.hi.z : scene.lo.z);
            const float z = (lightView * glm::vec4(p, 1)).z;
            zMin = std::min(zMin, z), zMax = std::max(zMax, z);
        }
    // View space looks down -z: near = -zMax, far = -zMin.
    return glm::ortho(c.x - radius, c.x + radius, c.y - radius, c.y + radius, -zMax - 1.0f, -zMin + 1.0f) * lightView;
}

SunShadow sunShadow(const Camera& camera, Viewport vp, const Bounds& scene, glm::vec3 toSun, bool vulkanY, bool shadows,
                    int rtMode, int rays) {
    SunShadow s;
    const float aspect = vp.height > 0 ? float(vp.width) / float(vp.height) : 1.0f;
    // GL clip -> [0, 1]^3: tile uv (both APIs render the map with an upright viewport) and depth.
    const glm::mat4 bias = glm::translate(glm::mat4(1), glm::vec3(0.5f)) * glm::scale(glm::mat4(1), glm::vec3(0.5f));
    for (int c = 0; c < kShadowCascades; ++c) {
        s.lightViewProj[c] = shadowViewProj(camera, aspect, scene, toSun, c ? kCascadeEnd[c - 1] : camera.nearZ, kCascadeEnd[c]);
        s.block.cascade[c] = bias * s.lightViewProj[c];
    }
    // Camera ray through window uv: view depth w (= 1 / gl_FragCoord.w) times right/up/forward offsets.
    const glm::mat4 invView = glm::inverse(camera.view());
    const glm::vec3 right(invView[0]), up(invView[1]), forward = -glm::vec3(invView[2]);
    const float tanY = std::tan(glm::radians(camera.verticalFovDeg) * 0.5f), tanX = tanY * aspect, sy = vulkanY ? -1.0f : 1.0f;
    // ndc x = 2u - 1, ndc y = sy (2v - 1)
    s.block.rayBase = glm::vec4(forward - tanX * right - sy * tanY * up, 0);
    s.block.rayU = glm::vec4(2 * tanX * right, 0);
    s.block.rayV = glm::vec4(2 * sy * tanY * up, 0);
    s.block.sun = glm::vec4(toSun, shadows ? 1.0f : 0.0f);
    s.block.eye = glm::vec4(camera.position, float(rays));
    s.block.screen = glm::vec4(vp.width ? 1.0f / float(vp.width) : 0.0f, vp.height ? 1.0f / float(vp.height) : 0.0f, float(rtMode), 0);
    return s;
}

const char* sunGlsl() {
    return
#include "sun_glsl.inc"
        ;
}

std::string originalPixelSource(const std::string& translated, bool vulkan, bool rt) {
    std::string head = "#define OU_SUN 1\n";
    if (vulkan) head += "#define OU_SHADOW_SET 2\n";
    if (rt)
        head += "#extension GL_EXT_ray_query : require\n#extension GL_EXT_buffer_reference : require\n"
                "#extension GL_EXT_nonuniform_qualifier : require\n"
                "#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require\n#define OU_RT 1\n#define OU_RT_SET 3\n";
    std::string s = translated;
    if (rt && s.starts_with("#version 450")) s.replace(9, 3, "460"); // GL_EXT_ray_query needs GLSL 4.60
    s.insert(s.find('\n') + 1, head); // after #version
    return s + sunGlsl();
}

std::string rtSummary(const RtStatus& s, RayTracing requested, int rays) {
    static constexpr const char* kModes[] = {"off", "shadows", "full (shadows + water reflections)"};
    std::string line = std::string("RT ") + kModes[size_t(requested)];
    if (!s.supported)
        return line + " | unsupported" + (s.reason.empty() ? "" : " (" + s.reason + ")") +
               (requested != RayTracing::Off ? " -> shadow map" : "");
    if (requested != RayTracing::Off) line += " | " + std::to_string(rays) + " ray(s)/px" + (s.active ? "" : " | NOT ACTIVE");
    return line + " | " + std::to_string(s.blas) + " BLAS, " + std::to_string(s.geometries) + " geometries, " +
           std::to_string(s.instances) + " instances (" + std::to_string(s.waterInstances) + " water) | TLAS builds " +
           std::to_string(s.tlasBuilds) + " updates " + std::to_string(s.tlasUpdates) + " | BLAS refits " +
           std::to_string(s.blasRefits) + " rebuilds " + std::to_string(s.blasRebuilds);
}

std::unique_ptr<Renderer> selectRenderer(std::string_view name, const std::function<std::unique_ptr<Renderer>(bool)>& create,
                                         std::string& chosen, std::string& note, std::string& error) {
    if (name != "auto" && name != "opengl" && name != "vulkan") {
        error = "renderer must be auto, opengl or vulkan, got " + std::string(name);
        return nullptr;
    }
    if (name != "opengl") {
        auto r = create(true);
        if (r && r->preWindowSetup(error)) {
            chosen = "vulkan";
            return r;
        }
        if (name == "vulkan") return nullptr;
        note = "Vulkan unavailable (" + error + "), falling back to OpenGL";
        error.clear();
    }
    auto r = create(false);
    if (!r || !r->preWindowSetup(error)) return nullptr;
    chosen = "opengl";
    return r;
}

} // namespace ou::render
