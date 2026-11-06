#include "render/OriginalShading.hpp"
#include "formats/Formats.hpp"
#include "render/RenderCommon.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

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
    return camera.clipped(camera.jittered(glm::perspectiveRH_ZO(glm::radians(camera.verticalFovDeg), aspect, camera.nearZ, camera.farZ)), true) *
           camera.view();
}

void fillFrame(sm3::FrameBlock& f, const Camera& camera, Viewport vp, float t, const scene::Environment& env, bool flipY,
               const Underwater* underwater, std::span<const uint32_t> cookies) {
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
    // SkinMatrices bone 0 (c85-c87 = 3x4 rows, c88 = the bone's bend vector; tru.exe 0x50F0E0 uploads 4 rows per palette
    // bone, row 3 from the vec4s after the pose's matrices). Our programs are the unskinned slots (CPU skinning), but the
    // bend_* foliage VS (kelp, jungle broadleaf, bamboo) read bone 0 statically: pos = c85..c87 x (v0 + bend terms of
    // c88 and texcoord3-5). Identity + zero bend = the rest pose; zeros collapsed those objects to a point. The sway
    // (game::Bends) is applied to their vertices on the CPU (ou-game BendMeshes), so c88 stays zero.
    put(f.vs, 85, 1, 0, 0, 0);
    put(f.vs, 86, 0, 1, 0, 0);
    put(f.vs, 87, 0, 0, 1, 0);
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
    // IrradianceState (c70-c76, per object: FrameBlock::irradiance slots, IrradianceSlots). Slot 0 = tru.exe's default
    // state, the SH of the ambient alone (L00 = ambient · 1.128, 0x507D60): Base = ambient, every other term 0. Objects
    // in units with light probes get theirs (IrradianceSlots); point/spot lights reach them through their light set.
    if (env.valid) put(f.irradiance, 6, env.ambient[0], env.ambient[1], env.ambient[2], 0);
    // GlobalParams: c140-c146 player position / bones (foliage interaction; unknown here, so far away),
    // c148/c149 light-map scale sets (default 1, 0x7731B0).
    for (int r = 140; r < 147; ++r) put(f.ps, r, 0, 0, -1e9f, 1); // ponytail: no player -> no foliage bending
    put(f.ps, 148, 1, 1, 1, 1);
    put(f.ps, 149, 1, 1, 1, 1);
    f.bools[0] = 0; // vs b0-15
    // Sun = light slot 6 (c208 + 5*6): c209 direction, c212 colour (w = luminance), b6 (0x50A5F0, 0x5042B0).
    fillLights(f, env, t, cookies);
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

void fillWaves(sm3::ObjectBlock& o, std::span<const WaterVolume> volumes, glm::vec3 center, float radius) {
    for (const auto& v : volumes) { // the first waved volume whose xy box the mesh bounds reach; z: max is the waved surface
        const bool waved = v.waves[3][0] != 0 || v.waves[3][1] != 0 || v.waves[3][2] != 0 || v.waves[3][3] != 0;
        if (waved && center.x + radius >= v.min[0] && center.x - radius <= v.max[0] && center.y + radius >= v.min[1] &&
            center.y - radius <= v.max[1]) {
            std::memcpy(o.waves, v.waves, sizeof o.waves);
            return;
        }
    }
}

void irradianceState(const float sh[9][3], float out[7][4]) {
    // 0x508570: c1 = 0.429043, c2 = 0.511664, c3 = 0.743125, c4 = 0.886227, c5 = 0.247708 (Ramamoorthi-Hanrahan).
    for (int c = 0; c < 3; ++c) {
        out[c][0] = sh[4][c] * 0.429043f * 2, out[c][1] = sh[7][c] * 0.429043f * 2;
        out[c][2] = sh[5][c] * 0.429043f * 2, out[c][3] = sh[6][c] * 0.743125f;
        out[3 + c][0] = sh[3][c] * 0.511664f * 2, out[3 + c][1] = sh[1][c] * 0.511664f * 2;
        out[3 + c][2] = sh[2][c] * 0.511664f * 2, out[3 + c][3] = sh[8][c] * 0.429043f;
        out[6][c] = sh[0][c] * 0.886227f - sh[6][c] * 0.247708f;
    }
    out[6][3] = 0;
}

void IrradianceSlots::begin(size_t meshes) {
    if (cache_.size() != meshes) cache_.assign(meshes, {});
    slot_.assign(meshes, 0);
    states_.clear();
}

uint32_t IrradianceSlots::place(const scene::Environment& env, size_t mesh, glm::vec3 center) {
    Cached& c = cache_[mesh];
    if (c.center != center) {
        float sh[9][3];
        if (!formats::sampleIrradiance(env.irradiance, center, sh)) return 0;
        c.center = center;
        irradianceState(sh, reinterpret_cast<float(*)[4]>(c.state.data()));
    }
    for (size_t i = 0; i < states_.size(); ++i)
        if (states_[i] == c.state) return uint32_t(i + 1);
    if (states_.size() + 1 >= size_t(sm3::kIrradianceSlots)) return 0; // ponytail: full -> ambient; grow the table if a scene hits it
    states_.push_back(c.state);
    return uint32_t(states_.size());
}

void IrradianceSlots::fill(sm3::FrameBlock& f) const {
    for (size_t i = 0; i < states_.size(); ++i)
        std::memcpy(f.irradiance[(i + 1) * sm3::kIrradianceRegs], states_[i].data(), sizeof(State));
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

void fillLights(sm3::FrameBlock& f, const scene::Environment& env, float t, std::span<const uint32_t> cookies) {
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
        // Rows 5-7: WorldToModulationMapSpot (c170 + 3·slot for slots 0/1, s12/s13), 0x50B910 spot branch: light space
        // q about the apex (rows X, Y, direction Z), rotated by rotation deg/s (sign as the sun cookie [I]); projection
        // x = s·qx + (su + 0.5)/radius·qz, y alike, w = qz/radius with s = 0.5 / (tan(outer/2) · (radius + apex) · scale),
        // su = frac(scroll · t) -> uv = s·radius·qx/qz + su + 0.5. No cookie: w row (0, 0, 0, 1), layer -1 (white).
        const auto layer = std::find(cookies.begin(), cookies.end(), l.cookie);
        put(r, 7, 0, 0, 0, 1);
        put(r, 8, l.cookie && layer != cookies.end() ? float(layer - cookies.begin()) : -1.0f);
        if (!l.cookie || layer == cookies.end()) continue;
        glm::vec3 x0 = glm::make_vec3(l.axes[0]), y0 = glm::make_vec3(l.axes[1]);
        if (glm::dot(x0, x0) < 1e-6f) { // dynamic lights without a record: any basis about the direction
            x0 = glm::normalize(glm::cross(std::abs(dir.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0), dir));
            y0 = glm::cross(dir, x0);
        }
        const float a = glm::radians(l.cookieRotation * t), ca = std::cos(a), sa = std::sin(a);
        const glm::vec3 x = ca * x0 + sa * y0, y = ca * y0 - sa * x0;
        const float s = 0.5f / std::max(std::tan(l.coneOuter * 0.5f) * (radius + apex) * l.cookieScale, 1e-4f);
        const float su = std::fmod(l.cookieScroll[0] * t, 1.0f) + 0.5f, sv = std::fmod(l.cookieScroll[1] * t, 1.0f) + 0.5f;
        const glm::vec4 qx(x, -glm::dot(x, pos)), qy(y, -glm::dot(y, pos)), qz = glm::vec4(dir, -glm::dot(dir, pos)) / radius;
        const glm::vec4 rx = s * qx + su * qz, ry = s * qy + sv * qz;
        put(r, 5, rx.x, rx.y, rx.z, rx.w);
        put(r, 6, ry.x, ry.y, ry.z, ry.w);
        put(r, 7, qz.x, qz.y, qz.z, qz.w);
    }
}

std::vector<uint32_t> cookieIds(const scene::Environment& env) {
    std::vector<uint32_t> ids;
    for (const auto& l : env.lights) // ponytail: > 16 distinct cookies light uncut
        if (l.cookie && ids.size() < size_t(kCookieLayers) && std::find(ids.begin(), ids.end(), l.cookie) == ids.end()) ids.push_back(l.cookie);
    return ids;
}

EngineTexture cookieArray(std::span<const uint32_t> ids, std::span<const scene::TextureData> textures) {
    EngineTexture a{kCookieSize, kCookieSize, std::vector<uint8_t>(size_t(kCookieSize) * kCookieSize * 4 * kCookieLayers, 255), kCookieLayers};
    for (size_t layer = 0; layer < ids.size() && layer < size_t(kCookieLayers); ++layer) {
        const auto t = std::find_if(textures.begin(), textures.end(), [&](const auto& x) { return x.id == ids[layer]; });
        if (t == textures.end() || t->cube || t->mips.empty()) continue;
        const auto& m = t->mips[0];
        const std::vector<uint8_t> rgba = formats::toRgba8(t->format, m.width, m.height, m.data);
        if (m.width == 0 || m.height == 0 || rgba.size() < size_t(m.width) * m.height * 4) continue;
        uint8_t* dst = &a.rgba[layer * kCookieSize * kCookieSize * 4];
        for (int y = 0; y < kCookieSize; ++y) // nearest resample to the array's size (cookies are soft gobos)
            for (int x = 0; x < kCookieSize; ++x)
                std::memcpy(dst + (size_t(y) * kCookieSize + x) * 4,
                            &rgba[(size_t(y) * m.height / kCookieSize * m.width + size_t(x) * m.width / kCookieSize) * 4], 4);
    }
    return a;
}

namespace {
// Slots of the lights `reaches` accepts, in env.lights order (see lightSet below).
template <class Reaches>
LightSet pickLights(const scene::Environment& env, Reaches reaches) {
    // tru.exe per drawn entity (0x55FB60 -> device vfunc 0x70 = 0x5076A0 -> 0x505530) lists the scene lights touching
    // it in scene order; 0x504CC0 keeps, in that order, up to 3 "special" lights (cookie spots, at most 2, and shadow
    // casters, DTP +0x144C) and 3 plain ones (a 4th+ plain light takes a free special place, else is dropped); 0x5042B0
    // then gives cookie spots slots 0, 1.., the other special lights the next slots, plain lights 3-5 (overflow after
    // the special ones). Read under Wine (OriginalShading.BedroomLightSetMatchesOriginal). Ours: "touching" = the
    // light's sphere reaches the object's bounding sphere, or a terrain group's leaf boxes (terrain takes its lights
    // per leaf the same way, 0x511810). [I] Not done: receiver masks (record +0x8C..+0xB7 vs the entity's +0xE4 /
    // cell terrain data +0x08; 0x8300000F and 1 in l3_main_bedroom), spot cones (tested as spheres), and tru.exe's
    // per-frame light visibility (+0x2CA: lights whose volume is not seen through the portals are left out, which
    // only frees slots).
    std::array<int, sm3::kLightSlots> taken;
    int nTaken = 0, special = 0, plain = 0, cookies = 0;
    const size_t n = std::min(env.lights.size(), size_t(sm3::kMaxLights));
    for (size_t i = 0; i < n && nTaken < sm3::kLightSlots; ++i) {
        const scene::Light& l = env.lights[i];
        if (!reaches(l)) continue;
        const bool cookie = l.type == 1 && l.cookie && cookies < 2;
        if (cookie || l.shadow) {
            if (special == 3) continue;
            ++special, cookies += cookie;
        } else if (plain < 3) ++plain;
        else if (special < 3) ++special;
        else continue;
        taken[size_t(nTaken++)] = int(i);
    }
    LightSet s{{~0u, ~0u}};
    int cookieSlot = 0, specialSlot = cookies, plainSlot = 3;
    for (int k = 0; k < nTaken; ++k) {
        const scene::Light& l = env.lights[size_t(taken[size_t(k)])];
        const int slot = l.type == 1 && l.cookie ? cookieSlot++ : l.shadow || plainSlot == 6 ? specialSlot++ : plainSlot++;
        if (slot >= sm3::kLightSlots) continue;
        s.packed[slot >> 2] &= ~(0xFFu << (8 * (slot & 3)));
        s.packed[slot >> 2] |= uint32_t(taken[size_t(k)]) << (8 * (slot & 3));
    }
    return s;
}
} // namespace

LightSet lightSet(const scene::Environment& env, glm::vec3 center, float radius) {
    return pickLights(env, [&](const scene::Light& l) { return glm::distance(glm::make_vec3(l.position), center) - radius < l.radius; });
}

LightSet lightSet(const scene::Environment& env, std::span<const std::array<float, 6>> boxes, const glm::mat4& model) {
    // World boxes: centre moved by `model`, half extents by |rotation-scale| (terrain models are translations).
    thread_local std::vector<std::array<float, 6>> world;
    world.clear();
    std::array<float, 6> all{1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    const glm::mat3 a(glm::abs(model[0]), glm::abs(model[1]), glm::abs(model[2]));
    for (const auto& b : boxes) {
        const glm::vec3 lo(b[0], b[1], b[2]), hi(b[3], b[4], b[5]);
        const glm::vec3 c(model * glm::vec4((lo + hi) * 0.5f, 1.0f)), e = a * ((hi - lo) * 0.5f);
        std::array<float, 6>& w = world.emplace_back();
        for (int k = 0; k < 3; ++k) {
            w[size_t(k)] = c[k] - e[k], w[size_t(k) + 3] = c[k] + e[k];
            all[size_t(k)] = std::min(all[size_t(k)], w[size_t(k)]), all[size_t(k) + 3] = std::max(all[size_t(k) + 3], w[size_t(k) + 3]);
        }
    }
    return pickLights(env, [&](const scene::Light& l) {
        if (!scene::sphereTouchesBox(l.position, l.radius, all)) return false;
        return std::ranges::any_of(world, [&](const auto& w) { return scene::sphereTouchesBox(l.position, l.radius, w); });
    });
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
    // Translations are memoised process-wide by (ShaderLib hash, backend, Material::skinParams): every streamed unit
    // re-uploads the whole scene's materials, and translating them all again cost the join frame ~4 ms. ShaderLib
    // hashes identify the bytecode (the scene keeps one program per hash).
    static std::mutex memoMutex;
    static std::map<std::tuple<uint64_t, bool, bool>, sm3::Glsl> memo;
    std::map<std::pair<uint64_t, bool>, const sm3::Glsl*> cache; // (hash, Material::skinParams)
    auto glsl = [&](uint64_t hash, bool pixel, std::string& why, bool skinParams = false) -> const sm3::Glsl* {
        if (auto it = cache.find({hash, skinParams}); it != cache.end()) return it->second->error.empty() ? it->second : (why = it->second->error, nullptr);
        const auto c = code.find(hash);
        if (c == code.end() || c->second->pixel != pixel) { why = "program not loaded"; return nullptr; }
        const std::lock_guard lock(memoMutex);
        auto [slot, fresh] = memo.try_emplace({hash, vulkan, skinParams});
        if (fresh) try {
                const auto prog = formats::d3d9::parse(std::span<const uint8_t>(c->second->bytecode));
                slot->second = sm3::translate(prog, vulkan, skinParams);
            } catch (const std::exception& e) {
                slot->second.error = e.what();
            }
        const sm3::Glsl* g = cache[{hash, skinParams}] = &slot->second; // std::map nodes stay put
        if (!g->error.empty()) { why = g->error; return nullptr; }
        return g;
    };
    std::map<std::tuple<uint64_t, uint64_t, bool>, int> pairs;
    for (const auto& m : scene.materials) {
        out.materials[m.id] = &m;
        std::string why;
        const sm3::Glsl* v = m.vertexShader ? glsl(m.vertexShader, false, why, m.skinParams) : nullptr;
        const sm3::Glsl* p = m.pixelShader ? glsl(m.pixelShader, true, why) : nullptr;
        if (!v || !p) {
            ++out.fallback;
            out.warnings.push_back("material " + std::to_string(m.id) + ": generic shading (" +
                                   (why.empty() ? "no composite programs" : why) + ")");
            continue;
        }
        auto [it, fresh] = pairs.try_emplace({m.vertexShader, m.pixelShader, m.skinParams}, int(out.programs.size()));
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

uint32_t reflectionSamplers(const scene::Material& mat) {
    uint32_t mask = 0;
    for (const auto& s : mat.samplers)
        if (s.reg < 16 && s.source == 1 && s.cls == 5) mask |= 1u << s.reg;
    return mask;
}

float reflectionPlaneZ(const scene::MeshData& m, const glm::mat4& model,
                       const std::unordered_map<uint32_t, const scene::Material*>& materials) {
    double z = 0;
    size_t n = 0;
    for (const auto& s : m.submeshes) {
        const auto it = materials.find(s.materialId);
        if (it == materials.end() || !reflectionSamplers(*it->second)) continue;
        for (uint32_t i = s.firstIndex; i < s.firstIndex + s.indexCount && i < m.indices.size(); ++i, ++n)
            z += (model * glm::vec4(glm::make_vec3(m.vertices[m.indices[i]].position), 1.0f)).z;
    }
    return n ? float(z / double(n)) : std::numeric_limits<float>::quiet_NaN();
}

Reflection reflection(const Camera& camera, float aspect, float planeZ, bool flipY) {
    Reflection r;
    Camera& m = r.camera;
    m = camera;
    m.position.z = 2.0f * planeZ - camera.position.z;
    m.pitch = -camera.pitch;
    m.jitter = glm::vec2(0.0f);
    m.clipPlane = glm::vec4(0, 0, 1, -planeZ);
    // InstanceParams 0-2 (0x5CE800 -> 0x5C5B80) hold the columns of a 3x3 map: q = R.x c132 + R.y c133 + R.z c134 of
    // the reflected eye ray R, uv = q.xy / q.z on the reflection target. The mirror camera sees along R what the plane
    // reflects there.
    const glm::mat4 pv = m.projection(aspect) * m.view();
    auto row = [&](int i) { return glm::vec3(pv[0][i], pv[1][i], pv[2][i]); };
    const glm::vec3 rows[3] = {0.5f * (row(0) + row(3)), 0.5f * ((flipY ? 1.0f : -1.0f) * row(1) + row(3)), row(3)};
    for (int c = 0; c < 3; ++c) r.rows[c][0] = rows[0][c], r.rows[c][1] = rows[1][c], r.rows[c][2] = rows[2][c], r.rows[c][3] = 0;
    return r;
}

std::vector<uint32_t> spirv(const std::string& glsl, bool pixel, std::string& error) {
    // ponytail: one lock, cleared past kMax entries (a few levels' programs), never compiled under the lock.
    static constexpr size_t kMax = 4096;
    static std::mutex m;
    static std::map<std::string, std::vector<uint32_t>> memo; // key: 'v' / 'p' + source
    const std::string key = (pixel ? "p" : "v") + glsl;
    {
        const std::lock_guard lock(m);
        if (const auto it = memo.find(key); it != memo.end()) return it->second;
    }
    auto spv = sm3::compileSpirv(glsl, pixel, true, error);
    if (spv.empty()) return spv; // failures are not kept: the error message comes with a retry
    const std::lock_guard lock(m);
    if (memo.size() >= kMax) memo.clear();
    memo.emplace(key, spv);
    return spv;
}

void warmSpirv(const scene::SceneData& scene, bool rays) {
    const auto programs = prepare(scene, true).programs;
    std::atomic<size_t> next{0};
    auto work = [&] { // one program per task, its variants in order (VkRenderer drops a program without its VS)
        std::string error;
        for (size_t i; (i = next++) < programs.size();) {
            const auto& p = programs[i];
            if (spirv(p.vsGlsl.source, false, error).empty()) continue;
            spirv(p.psGlsl.source, true, error);
            spirv(originalPixelSource(p.psGlsl.source, true, false), true, error);
            if (rays) spirv(originalPixelSource(p.psGlsl.source, true, true), true, error);
        }
    };
    // Half the cores: the render thread keeps the others.
    std::vector<std::jthread> pool(std::clamp<size_t>(std::thread::hardware_concurrency() / 2, 1, 8) - 1);
    for (auto& t : pool) t = std::jthread(work);
    work();
}

} // namespace ou::render::original
