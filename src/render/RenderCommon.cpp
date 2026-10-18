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
        size_t expect = block ? size_t((w + 3) / 4) * ((h + 3) / 4) * block : size_t(w) * h * 4;
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

glm::mat4 shadowViewProj(const Camera& camera, float aspect, const Bounds& scene, float shadowDistance) {
    // Bounding sphere of the view frustum slice; a sphere keeps the box size fixed while the camera
    // turns, which together with texel snapping keeps shadow edges stable.
    const glm::mat4 invViewProj = glm::inverse(camera.projection(aspect) * camera.view());
    const float farZ = std::min(camera.farZ, shadowDistance);
    // NDC z of the slice end: perspective depth of view distance farZ.
    const glm::vec4 farClip = camera.projection(aspect) * glm::vec4(0, 0, -farZ, 1);
    const float zFar = farClip.z / farClip.w;
    glm::vec3 corners[8];
    glm::vec3 center(0.0f);
    for (int i = 0; i < 8; ++i) {
        const glm::vec4 p = invViewProj * glm::vec4(i & 1 ? 1 : -1, i & 2 ? 1 : -1, i & 4 ? zFar : -1, 1);
        corners[i] = glm::vec3(p) / p.w;
        center += corners[i] / 8.0f;
    }
    float radius = 0;
    for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
    radius = std::ceil(radius);

    const glm::vec3 up = std::abs(kSunDirection.z) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 0, 1);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0), -kSunDirection, up);
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

} // namespace ou::render
