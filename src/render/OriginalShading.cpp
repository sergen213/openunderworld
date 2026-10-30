#include "render/OriginalShading.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

namespace ou::render::original {
namespace {

void put(float (*regs)[4], int reg, float x, float y = 0, float z = 0, float w = 0) {
    regs[reg][0] = x, regs[reg][1] = y, regs[reg][2] = z, regs[reg][3] = w;
}
// Matrix columns into 4 consecutive registers: the shaders compute clip = v.x*c[n] + v.y*c[n+1] + v.z*c[n+2] + c[n+3].
void putMatrix(float (*regs)[4], int reg, const glm::mat4& m) {
    for (int c = 0; c < 4; ++c) put(regs, reg + c, m[c][0], m[c][1], m[c][2], m[c][3]);
}

constexpr int kLutWidth = 256, kLutRows = 128, kFogRow = 96, kLightRow = 0;

} // namespace

glm::mat4 viewProjD3D(const Camera& camera, float aspect) {
    return camera.jittered(glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDeg), aspect, camera.nearZ, camera.farZ)) * camera.view();
}

void fillFrame(sm3::FrameBlock& f, const Camera& camera, Viewport vp, float t, const scene::Environment& env, bool flipY,
               const Underwater* underwater) {
    std::memset(&f, 0, sizeof f);
    const float aspect = vp.height > 0 ? float(vp.width) / float(vp.height) : 1.0f;
    const glm::vec3 eye = camera.position;
    const float time = std::fmod(t, 1000.0f), dt = 1.0f / 30.0f; // TimeVector = (ms % 1e6 * 0.001, dt) (0x509630)
    // ---- vertex shader registers ----
    putMatrix(f.vs, 12, viewProjD3D(camera, aspect)); // ViewProject
    put(f.vs, 21, 1, 0);                             // NormalScaleOffset (1, 0): FLOAT3 normals (0x529020); ours are [-1, 1]
    // fogScaleAndOffset (-n/(f-n), 1/(f-n), ., f) from the unit render settings (0x514680); the translated VS reads it
    // only for materials with flag 0x04 (ShaderTranslate: material fixedParams.w bit 0), as tru.exe uploads it.
    if (env.valid) {
        const float n = env.fogNear, range = std::max(env.fogFar - env.fogNear, 1.0f);
        put(f.vs, 22, -n / range, 1.0f / range, 0, env.fogFar);
    }
    put(f.vs, 23, time, dt, 0, 0);                   // TimeVector (wind phase)
    put(f.vs, 24, eye.x, eye.y, eye.z, 0);           // CameraPos
    put(f.vs, 254, 1, 1, 0, 0);                      // TexcoordScales: uv / uv1 are pre-scaled by the loader
    // ---- pixel shader registers ----
    if (env.valid) put(f.ps, 0, env.fogColor[0], env.fogColor[1], env.fogColor[2], 0); // FogColor
    if (const Underwater* u = underwater; u && u->fog) { // under water: the unit's water fog set / filter fog (0x5C1F50)
        const float range = std::max(u->fogFar - u->fogNear, 1.0f);
        put(f.vs, 22, -u->fogNear / range, 1.0f / range, 0, u->fogFar);
        put(f.ps, 0, u->fogColor[0], u->fogColor[1], u->fogColor[2], 0);
    }
    put(f.ps, 1, eye.x, eye.y, eye.z, 0);            // g_CameraPosition
    if (env.valid) put(f.ps, 15, env.ambient[0], env.ambient[1], env.ambient[2], 0); // g_Ambient
    put(f.ps, 53, time, dt, 0, 0);                   // TimeVectorPS
    put(f.ps, 54, 0, 0, (kFogRow + 0.5f) / kLutRows, 0); // HeightFogParams: no fog volumes -> (0, 0, fog row)
    // IrradianceState (dynamic objects' SH, c70-c76 = tru.exe 0x508570's layout: Red0/Green0/Blue0 = xy, xz, yz, zz
    // terms, Red1.. = x, y, z, x²-y² terms, Base = constant). The engine builds the SH from the ambient (L00 =
    // ambient · 1.128, 0x507D60) plus directional terms of lights it selects [U]; with the ambient alone the
    // conversion leaves Base = ambient and every other term 0. The point/spot lights reach dynamic objects through
    // their light set (slots 0-5) as on level geometry.
    if (env.valid) put(f.ps, 76, env.ambient[0], env.ambient[1], env.ambient[2], 0);
    // GlobalParams: c140-c146 player position / bones (foliage interaction; unknown here, so far away),
    // c148/c149 light-map scale sets (default 1, 0x7731B0).
    for (int r = 140; r < 147; ++r) put(f.ps, r, 0, 0, -1e9f, 1); // ponytail: no player -> no foliage bending
    put(f.ps, 148, 1, 1, 1, 1);
    put(f.ps, 149, 1, 1, 1, 1);
    f.bools[0] = 0; // vs b0-15
    // Sun = light slot 6 (c208 + 5*6): c209 direction, c212 colour (w = luminance), b6 (0x50A5F0, 0x5042B0).
    fillLights(f, env, t);
    f.bools[1] = 0;
    if (env.valid && env.sun) {
        const float* c = env.sunColor;
        put(f.ps, 209, env.sunDirection[0], env.sunDirection[1], env.sunDirection[2], 0);
        put(f.ps, 212, c[0], c[1], c[2], 0.3f * c[0] + 0.6f * c[1] + 0.1f * c[2]);
        f.bools[1] |= 1u << 6;
    }
    // WorldToModulationMapSun c176/c177 (sun cookie s11, 0x50B910 type 2): uv = light-space xy · 0.5 / (scale · 1000)
    // + 0.5 + scroll, light space from the record's rows (X, Y) about its origin, rotated by rotation deg/s [I signs].
    if (env.valid && env.sun && env.sunCookie) {
        const float s = 0.5f / std::max(env.cookieScale * 1000.0f, 1e-3f);
        const float a = glm::radians(env.cookieRotation * t), ca = std::cos(a), sa = std::sin(a);
        const glm::vec3 x0 = glm::make_vec3(env.sunAxes[0]), y0 = glm::make_vec3(env.sunAxes[1]), o = glm::make_vec3(env.sunOrigin);
        const glm::vec3 x = ca * x0 + sa * y0, y = ca * y0 - sa * x0;
        put(f.ps, 176, s * x.x, s * x.y, s * x.z, -s * glm::dot(x, o) + 0.5f + std::fmod(env.cookieScroll[0] * t, 1.0f));
        put(f.ps, 177, s * y.x, s * y.y, s * y.z, -s * glm::dot(y, o) + 0.5f + std::fmod(env.cookieScroll[1] * t, 1.0f));
    }
    f.screen[0] = float(vp.width), f.screen[1] = float(vp.height);
    f.screen[2] = vp.width ? 1.0f / float(vp.width) : 0.0f, f.screen[3] = vp.height ? 1.0f / float(vp.height) : 0.0f;
    // Screen-space reads of the back/depth buffer copies (globalSamplers). ScreenExtents c51: uv = vPos · zw + xy with
    // the D3D9 half-texel offset [I: the engine's values were not traced]; DepthFactors c3 dots the depth texel's .wxy
    // (ours: R = window depth); DepthToW c77: 1 / view depth = d · x + y for our [0, 1] perspective depth;
    // CameraDirection c97 (the PS dots it with the view vector).
    const float iw = f.screen[2], ih = f.screen[3];
    if (flipY) put(f.ps, 51, 0.5f * iw, 1.0f - 0.5f * ih, iw, -ih);
    else put(f.ps, 51, 0.5f * iw, 0.5f * ih, iw, ih);
    put(f.ps, 3, 0, 1, 0, 0);
    const float n = camera.nearZ, fz = camera.farZ;
    put(f.ps, 77, -(fz - n) / (fz * n), 1.0f / n, 0, 0);
    const glm::vec3 fwd = camera.forward();
    put(f.ps, 97, fwd.x, fwd.y, fwd.z, 0);
}

void fillMaterial(sm3::MaterialBlock& m, const scene::Material& mat, const scene::SubMesh& sub) {
    std::memset(&m, 0, sizeof m);
    std::copy_n(mat.vsParams.begin(), std::min(mat.vsParams.size(), size_t(4 * sm3::kMaterialVs)), &m.vs[0][0]);
    std::copy_n(mat.psParams.begin(), std::min(mat.psParams.size(), size_t(4 * sm3::kMaterialPs)), &m.ps[0][0]);
    // PCStateManager_SetBlendMode 0x527A80 -> 0x527940 writes |ref| / 255 to c2: mode 1 the material threshold,
    // modes 2-6 ref 1, others 0. The fixed-function alpha test (ALPHAFUNC GREATEREQUAL) follows SubMesh::alphaRef.
    const int ref = mat.blendMode == 1 ? mat.alphaThreshold : (mat.blendMode >= 2 && mat.blendMode <= 6) ? 1 : 0;
    m.fixed[0] = float(ref) / 255.0f;
    m.fixed[1] = 1.0f; // MaterialOpacity: 1 (instance fades: ObjectBlock::opacity)
    m.fixed[2] = sub.alphaRef ? float(sub.alphaRef) / 255.0f : -1.0f;
    // Bit 0: fogged material (flags 0x04) -> VS c22; bit 1: additive modes 5/6 -> PS FogColor 0 (0x5068E6, 0x5066A0);
    // value 8: opaque (mode 0, alpha-blended as mode 4 while the instance fades, 0x5066A0).
    m.fixed[3] = float(((mat.flags & 4) ? 1 : 0) | ((mat.blendMode == 5 || mat.blendMode == 6) ? 2 : 0) |
                       (sub.blend == scene::Blend::Opaque ? 8 : 0));
}

void fillObject(sm3::ObjectBlock& o, const glm::mat4& viewProj, const glm::mat4& model, LightSet lights, float opacity) {
    putMatrix(o.v, 0, viewProj * model);
    putMatrix(o.v, 4, model);
    o.lightSet[0] = lights.packed[0], o.lightSet[1] = lights.packed[1], o.opacity = opacity;
}

std::vector<int> falloffRows(const scene::Environment& env) {
    std::vector<int> rows;
    std::map<std::vector<uint8_t>, int> unique;
    for (const auto& l : env.lights) {
        auto [it, fresh] = unique.try_emplace(l.falloff, int(unique.size()));
        if (it->second >= kFogRow) it->second = 0; // ponytail: > 96 distinct curves share row 0
        rows.push_back(l.falloff.size() == 256 ? it->second : -1);
    }
    return rows;
}

void fillLights(sm3::FrameBlock& f, const scene::Environment& env, float t) {
    // Light table: ShadowLights/NonShadowLights registers per light (PCRenderLight 0x50B910, uploaded per slot by
    // 0x50A5F0): R0 position (spots: moved back to the cone apex), R1 direction, R2 (1/radius, 0, -apex/radius,
    // falloff row v) -> AttenuationSampler u = (d - apex) / radius, R3 (cone scale, cone bias, apex plane, 0),
    // R4 colour x intensity (w = luminance). Points: cone (0, 1) and apex plane -radius - 1 = never clipped.
    const std::vector<int> rows = falloffRows(env);
    const size_t n = std::min(env.lights.size(), size_t(sm3::kMaxLights)); // ponytail: first 64 lights of the scene
    for (size_t i = 0; i < n; ++i) {
        const scene::Light& l = env.lights[i];
        float (*r)[4] = f.lights + i * sm3::kLightRegs;
        const glm::vec3 dir = glm::make_vec3(l.direction);
        const float radius = std::max(l.radius, 1.0f);
        float apex = 0, scale = 0, bias = 1, plane = -radius - 1;
        if (l.type == 1) {
            // 0x50A280: scale = 1 / (cos(inner/2) - cos(outer/2)), bias = -cos(outer/2) · scale [I half angles];
            // apex offset = near width / tan(outer/2) (0x50B910).
            const float ci = std::cos(l.coneInner * 0.5f), co = std::cos(l.coneOuter * 0.5f);
            scale = 1.0f / std::max(ci - co, 1e-4f), bias = -co * scale;
            apex = l.nearWidth / std::max(std::tan(l.coneOuter * 0.5f), 1e-4f);
            plane = apex;
        }
        // Flicker: intensity x curve[phase · 255] lerped, phase = frac(t / period) (0x50A500).
        float k = 1;
        if (l.flickerPeriod > 0 && l.flicker.size() == 256) {
            const float p = std::fmod(t / l.flickerPeriod, 1.0f) * 255.0f;
            const int i0 = int(p);
            k = glm::mix(l.flicker[size_t(i0)], l.flicker[size_t(std::min(i0 + 1, 255))], p - float(i0));
        }
        const glm::vec3 pos = glm::make_vec3(l.position) - apex * dir, c = glm::make_vec3(l.color) * k;
        const int row = rows[i] < 0 ? kLightRow : rows[i];
        put(r, 0, pos.x, pos.y, pos.z, 1);
        put(r, 1, dir.x, dir.y, dir.z, 0);
        put(r, 2, 1.0f / radius, 0, -apex / radius, (float(row) + 0.5f) / kLutRows);
        put(r, 3, scale, bias, plane, 0);
        put(r, 4, c.x, c.y, c.z, 0.3f * c.x + 0.6f * c.y + 0.1f * c.z);
    }
    // WorldToModulationMapSpot c170-c175 (slots 0/1 cookies s12/s13, white: not bound per object): third row
    // (0, 0, 0, 1) so the projective lookup divides by 1.
    put(f.ps, 172, 0, 0, 0, 1);
    put(f.ps, 175, 0, 0, 0, 1);
}

LightSet lightSet(const scene::Environment& env, glm::vec3 center, float radius) {
    // tru.exe binds per drawable up to 3 shadow lights (slots 0-2, cookie + shadow map) and 3 others (3-5) (0x5042B0).
    // Without spot cookies / light shadow maps both slot groups compute the same, so the strongest lights whose
    // sphere reaches the object fill slots 0..5 in order (the PS nests slot n + 1 inside slot n) [I: the engine's
    // selection rule and receiver masks (record +0x8C..+0xB7) are not decoded].
    std::array<std::pair<float, int>, sm3::kLightSlots> best;
    best.fill({0.0f, 255});
    const size_t n = std::min(env.lights.size(), size_t(sm3::kMaxLights));
    for (size_t i = 0; i < n; ++i) {
        const scene::Light& l = env.lights[i];
        const float d = glm::distance(glm::make_vec3(l.position), center) - radius;
        if (d >= l.radius) continue;
        const float w = (0.3f * l.color[0] + 0.6f * l.color[1] + 0.1f * l.color[2]) * (1.0f - std::max(d, 0.0f) / l.radius) + 1e-6f;
        if (w <= best.back().first) continue;
        // Insert keeping heavier first, ties in arrival order (what std::sort's insertion sort did for 6 entries).
        size_t j = best.size() - 1;
        for (; j > 0 && best[j - 1].first < w; --j) best[j] = best[j - 1];
        best[j] = {w, int(i)};
    }
    LightSet s{{~0u, ~0u}};
    for (int k = 0; k < sm3::kLightSlots; ++k) {
        s.packed[k >> 2] &= ~(0xFFu << (8 * (k & 3)));
        s.packed[k >> 2] |= uint32_t(best[size_t(k)].second) << (8 * (k & 3));
    }
    return s;
}

glm::vec4 boundingSphere(std::span<const scene::Vertex> vertices, const glm::mat4& model) {
    glm::vec3 lo(1e30f), hi(-1e30f);
    for (const auto& v : vertices) {
        const glm::vec3 p(model * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
        lo = glm::min(lo, p), hi = glm::max(hi, p);
    }
    if (vertices.empty()) return glm::vec4(0);
    return glm::vec4((lo + hi) * 0.5f, glm::length(hi - lo) * 0.5f);
}

EngineTexture attenuationLut(const scene::Environment& env) {
    EngineTexture t{kLutWidth, kLutRows, std::vector<uint8_t>(size_t(kLutWidth) * kLutRows * 4, 0)};
    std::vector<const std::vector<uint8_t>*> curves(kFogRow, nullptr);
    const std::vector<int> rows = falloffRows(env);
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i] >= 0) curves[size_t(rows[i])] = &env.lights[i].falloff;
    for (int y = 0; y < kLutRows; ++y)
        for (int x = 0; x < kLutWidth; ++x) {
            uint8_t* p = &t.rgba[(size_t(y) * kLutWidth + x) * 4];
            // Fog row: the engine's default curve is the identity ramp k -> k in R and G (0x512CF0): linear fog.
            // Light rows: each distinct light falloff curve (Light::falloff, 0x50AFB0); unused rows linear.
            int v = 0;
            if (y == kFogRow) v = x;
            else if (y < kFogRow) v = curves[size_t(y)] ? (*curves[size_t(y)])[size_t(x)] : 255 - x;
            p[0] = p[1] = uint8_t(v);
            p[3] = 255;
        }
    return t;
}

Prepared prepare(const scene::SceneData& scene, bool vulkan) {
    Prepared out;
    std::unordered_map<uint64_t, const scene::ShaderCode*> code;
    for (const auto& c : scene.shaders) code.emplace(c.hash, &c);
    std::map<uint64_t, sm3::Glsl> cache;
    auto glsl = [&](uint64_t hash, bool pixel, std::string& why) -> const sm3::Glsl* {
        if (auto it = cache.find(hash); it != cache.end()) return it->second.error.empty() ? &it->second : (why = it->second.error, nullptr);
        const auto c = code.find(hash);
        if (c == code.end() || c->second->pixel != pixel) { why = "program not loaded"; return nullptr; }
        sm3::Glsl g;
        try {
            const auto prog = formats::d3d9::parse(std::span<const uint8_t>(c->second->bytecode));
            g = sm3::translate(prog, vulkan);
        } catch (const std::exception& e) {
            g.error = e.what();
        }
        auto& slot = cache[hash] = std::move(g);
        if (!slot.error.empty()) { why = slot.error; return nullptr; }
        return &slot;
    };
    std::map<std::pair<uint64_t, uint64_t>, int> pairs;
    for (const auto& m : scene.materials) {
        out.materials[m.id] = &m;
        std::string why;
        const sm3::Glsl* v = m.vertexShader ? glsl(m.vertexShader, false, why) : nullptr;
        const sm3::Glsl* p = m.pixelShader ? glsl(m.pixelShader, true, why) : nullptr;
        if (!v || !p) {
            ++out.fallback;
            out.warnings.push_back("material " + std::to_string(m.id) + ": generic shading (" +
                                   (why.empty() ? "no composite programs" : why) + ")");
            continue;
        }
        auto [it, fresh] = pairs.try_emplace({m.vertexShader, m.pixelShader}, int(out.programs.size()));
        if (fresh) out.programs.push_back({m.vertexShader, m.pixelShader, *v, *p});
        out.materialProgram[m.id] = it->second;
        ++out.translated;
    }
    return out;
}

uint32_t samplerTexture(const scene::Material& mat, std::span<const uint32_t> inst, int reg) {
    for (const auto& s : mat.samplers) {
        if (s.reg != reg) continue;
        if (s.source == 2 && s.cls >= 1 && s.cls <= inst.size() && inst[s.cls - 1]) return inst[s.cls - 1];
        return s.textureId;
    }
    return 0;
}

uint32_t screenSamplers(const scene::Material& mat, std::span<const uint32_t> inst, bool readsDepth) {
    uint32_t mask = 0;
    for (const auto& s : mat.samplers) {
        if (s.reg >= 16) continue;
        const bool unsupplied = s.source == 2 && s.cls >= 1 && (s.cls > inst.size() || !inst[s.cls - 1]);
        if ((s.source == 1 && s.cls == 9) || (readsDepth && unsupplied && s.cls == 2)) mask |= 1u << s.reg;
        if ((s.source == 1 && s.cls == 10) || (readsDepth && unsupplied && s.cls == 3)) mask |= 1u << (16 + s.reg);
    }
    return mask;
}

} // namespace ou::render::original
