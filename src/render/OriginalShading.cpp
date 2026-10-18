#include "render/OriginalShading.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <glm/gtc/matrix_transform.hpp>

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
    return glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDeg), aspect, camera.nearZ, camera.farZ) * camera.view();
}

void fillFrame(sm3::FrameBlock& f, const Camera& camera, Viewport vp, float t, const scene::Environment& env) {
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
    put(f.ps, 1, eye.x, eye.y, eye.z, 0);            // g_CameraPosition
    if (env.valid) put(f.ps, 15, env.ambient[0], env.ambient[1], env.ambient[2], 0); // g_Ambient
    put(f.ps, 53, time, dt, 0, 0);                   // TimeVectorPS
    put(f.ps, 54, 0, 0, (kFogRow + 0.5f) / kLutRows, 0); // HeightFogParams: no fog volumes -> (0, 0, fog row)
    // IrradianceState (dynamic objects' SH ambient, c70-c76) [U: volumes not decoded]: constant term = g_Ambient.
    for (int c = 0; c < 3; ++c) put(f.ps, 70 + 2 * c, 0, 0, 0, env.valid ? env.ambient[c] : 0);
    // GlobalParams: c140-c146 player position / bones (foliage interaction; unknown here, so far away),
    // c148/c149 light-map scale sets (default 1, 0x7731B0).
    for (int r = 140; r < 147; ++r) put(f.ps, r, 0, 0, -1e9f, 1); // ponytail: no player -> no foliage bending
    put(f.ps, 148, 1, 1, 1, 1);
    put(f.ps, 149, 1, 1, 1, 1);
    f.bools[0] = 0; // vs b0-15
    // Sun = light slot 6 (c208 + 5*6): c209 direction, c212 colour (w = luminance), b6 (0x50A5F0, 0x5042B0). Its cookie
    // (s11 via c176/c177) is not reproduced: white. Point/spot lights (slots 0-5, b0-b5) stay off [U placement masks].
    f.bools[1] = 0;
    if (env.valid && env.sun) {
        const float* c = env.sunColor;
        put(f.ps, 209, env.sunDirection[0], env.sunDirection[1], env.sunDirection[2], 0);
        put(f.ps, 212, c[0], c[1], c[2], 0.3f * c[0] + 0.6f * c[1] + 0.1f * c[2]);
        f.bools[1] |= 1u << 6;
    }
    f.screen[0] = float(vp.width), f.screen[1] = float(vp.height);
    f.screen[2] = vp.width ? 1.0f / float(vp.width) : 0.0f, f.screen[3] = vp.height ? 1.0f / float(vp.height) : 0.0f;
}

void fillMaterial(sm3::MaterialBlock& m, const scene::Material& mat, const scene::SubMesh& sub) {
    std::memset(&m, 0, sizeof m);
    std::memcpy(m.vs, mat.vsParams.data(), std::min(mat.vsParams.size(), size_t(4 * sm3::kMaterialVs)) * sizeof(float));
    std::memcpy(m.ps, mat.psParams.data(), std::min(mat.psParams.size(), size_t(4 * sm3::kMaterialPs)) * sizeof(float));
    // PCStateManager_SetBlendMode 0x527A80 -> 0x527940 writes |ref| / 255 to c2: mode 1 the material threshold,
    // modes 2-6 ref 1, others 0. The fixed-function alpha test (ALPHAFUNC GREATEREQUAL) follows SubMesh::alphaRef.
    const int ref = mat.blendMode == 1 ? mat.alphaThreshold : (mat.blendMode >= 2 && mat.blendMode <= 6) ? 1 : 0;
    m.fixed[0] = float(ref) / 255.0f;
    m.fixed[1] = 1.0f; // MaterialOpacity: no instance fade
    m.fixed[2] = sub.alphaRef ? float(sub.alphaRef) / 255.0f : -1.0f;
    // Bit 0: fogged material (flags 0x04) -> VS c22; bit 1: additive modes 5/6 -> PS FogColor 0 (0x5068E6, 0x5066A0).
    m.fixed[3] = float(((mat.flags & 4) ? 1 : 0) | ((mat.blendMode == 5 || mat.blendMode == 6) ? 2 : 0));
}

void fillObject(sm3::ObjectBlock& o, const glm::mat4& viewProj, const glm::mat4& model) {
    putMatrix(o.v, 0, viewProj * model);
    putMatrix(o.v, 4, model);
}

EngineTexture attenuationLut() {
    EngineTexture t{kLutWidth, kLutRows, std::vector<uint8_t>(size_t(kLutWidth) * kLutRows * 4, 0)};
    for (int y = 0; y < kLutRows; ++y)
        for (int x = 0; x < kLutWidth; ++x) {
            uint8_t* p = &t.rgba[(size_t(y) * kLutWidth + x) * 4];
            // Fog row: the engine's default curve is the identity ramp k -> k in R and G (0x512CF0): linear fog.
            // Light rows: falloff curves per light resource (lightRes+0x448) [U]; a linear falloff stands in.
            const int v = y == kFogRow ? x : y >= kLightRow && y < kFogRow ? 255 - x : 0;
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
            // Dynamic-object lighting (IrradianceState SH from irradiance volumes + the unit/cinematic light rig) is not
            // fed yet: such materials (Lara, enemies, cutscene actors) would render near black, so they stay generic.
            if (prog.constant("IrradianceStateBase")) g.error = "needs irradiance-volume lighting (not decoded)";
            else g = sm3::translate(prog, vulkan);
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

} // namespace ou::render::original
