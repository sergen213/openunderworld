// D3D9 SM3 parser/disassembler (formats/Shader), TRU ShaderLib container, SM3 -> GLSL translator (render/ShaderTranslate).
#include "cdc/Cdc.hpp"
#include "formats/Shader.hpp"
#include "formats/Formats.hpp"
#include "render/OriginalShading.hpp"
#include "render/RenderCommon.hpp"
#include "render/ShaderTranslate.hpp"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

using namespace ou;
using namespace ou::formats::d3d9;

namespace {

constexpr uint32_t dst(Reg t, uint32_t n, uint32_t mask = 0xF, uint32_t mod = 0) {
    return 0x80000000u | ((uint32_t(t) & 7) << 28) | ((uint32_t(t) >> 3 & 3) << 11) | (mask << 16) | (mod << 20) | n;
}
constexpr uint32_t src(Reg t, uint32_t n, uint32_t swizzle = 0xE4, uint32_t mod = 0, bool rel = false) {
    return 0x80000000u | ((uint32_t(t) & 7) << 28) | ((uint32_t(t) >> 3 & 3) << 11) | (swizzle << 16) | (mod << 24) |
           (rel ? 1u << 13 : 0) | n;
}
constexpr uint32_t ins(uint32_t op, uint32_t len, uint32_t control = 0) { return op | (control << 16) | (len << 24); }
uint32_t fbits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }

// vs_3_0 with a CTAB ("WorldViewProject" c0[4], "SkinMatrices" c85[3]), dcls, def, relative addressing, flow control.
std::vector<uint32_t> syntheticVs() {
    std::vector<uint32_t> t{0xFFFE0300};
    // CTAB comment: header (28 B) + 2 entries (40 B) + strings.
    std::vector<uint8_t> c(4 + 28 + 40);
    auto put = [&](size_t o, uint32_t v) { std::memcpy(&c[o], &v, 4); };
    auto put16 = [&](size_t o, uint16_t v) { std::memcpy(&c[o], &v, 2); };
    std::memcpy(c.data(), "CTAB", 4);
    auto str = [&](const char* s) { const uint32_t o = uint32_t(c.size() - 4); c.insert(c.end(), s, s + std::strlen(s) + 1); return o; };
    const uint32_t creator = str("ou test"), n0 = str("WorldViewProject"), n1 = str("SkinMatrices");
    while (c.size() % 4) c.push_back(0);
    put(4, 28), put(8, creator), put(12, 0xFFFE0300), put(16, 2), put(20, 28);
    put(4 + 28, n0), put16(4 + 32, 2), put16(4 + 34, 0), put16(4 + 36, 4);
    put(4 + 48, n1), put16(4 + 52, 2), put16(4 + 54, 85), put16(4 + 56, 3);
    t.push_back(Comment | uint32_t(c.size() / 4) << 16);
    for (size_t i = 0; i < c.size(); i += 4) { uint32_t w; std::memcpy(&w, &c[i], 4); t.push_back(w); }
    t.insert(t.end(), {ins(Def, 5), dst(Reg::Const, 8), fbits(0.5f), fbits(1.f), fbits(2.f), fbits(-1.f)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | 0 /*position0*/, dst(Reg::Input, 0)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | 5 | (1u << 16) /*texcoord1*/, dst(Reg::Input, 1)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | 0, dst(Reg::Output, 0)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | 5, dst(Reg::Output, 1, 0x3)});
    // mova a0.x, v1.x
    t.insert(t.end(), {ins(MovA, 2), dst(Reg::Addr, 0, 0x1), src(Reg::Input, 1, 0x00)});
    // mad r0, -v0.yyyy, c[a0.x + 85], c8
    t.insert(t.end(), {ins(Mad, 5), dst(Reg::Temp, 0), src(Reg::Input, 0, 0x55, 1), src(Reg::Const, 85, 0xE4, 0, true),
                       src(Reg::Addr, 0, 0x00), src(Reg::Const, 8)});
    // if b0 / m4x4 o0, v0, c0 / else / mov_sat o0, r0 / endif
    t.insert(t.end(), {ins(If, 1), src(Reg::ConstBool, 0)});
    t.insert(t.end(), {ins(M4x4, 3), dst(Reg::Output, 0), src(Reg::Input, 0), src(Reg::Const, 0)});
    t.insert(t.end(), {ins(Else, 0)});
    t.insert(t.end(), {ins(Mov, 2), dst(Reg::Output, 0, 0xF, 1), src(Reg::Temp, 0)});
    t.insert(t.end(), {ins(EndIf, 0)});
    t.insert(t.end(), {ins(Mov, 2), dst(Reg::Output, 1, 0x3), src(Reg::Input, 1, 0x1B /*wzyx*/)});
    t.push_back(0x0000FFFF);
    return t;
}

// ps_3_0: texld, texkill, cmp on vFace, MaterialParams c100 tint, fog lerp, a NonShadowLight slot (b3, colour c197).
std::vector<uint32_t> syntheticPs() {
    std::vector<uint32_t> t{0xFFFF0300};
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | 5, dst(Reg::Input, 0, 0x3)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u, dst(Reg::Misc, 1, 0x1)});
    t.insert(t.end(), {ins(Dcl, 2), 0x80000000u | (2u << 27), dst(Reg::Sampler, 1)});
    t.insert(t.end(), {ins(Tex, 3), dst(Reg::Temp, 0), src(Reg::Input, 0), src(Reg::Sampler, 1)});
    t.insert(t.end(), {ins(Add, 3), dst(Reg::Temp, 1), src(Reg::Temp, 0, 0xFF), src(Reg::Const, 2, 0x00, 1)});
    t.insert(t.end(), {ins(TexKill, 1), dst(Reg::Temp, 1)});
    t.insert(t.end(), {ins(Cmp, 4), dst(Reg::Temp, 2, 0x7), src(Reg::Misc, 1, 0x00), src(Reg::Const, 100), src(Reg::Const, 101)});
    t.insert(t.end(), {ins(Mul, 3), dst(Reg::Temp, 0, 0x7), src(Reg::Temp, 0), src(Reg::Temp, 2)});
    t.insert(t.end(), {ins(Lrp, 4), dst(Reg::ColorOut, 0, 0x7), src(Reg::Const, 54, 0xAA), src(Reg::Const, 0), src(Reg::Temp, 0)});
    t.insert(t.end(), {ins(Mul, 3), dst(Reg::ColorOut, 0, 0x8), src(Reg::Temp, 0, 0xFF), src(Reg::Const, 98, 0x00)});
    t.insert(t.end(), {ins(If, 1), src(Reg::ConstBool, 3)});
    t.insert(t.end(), {ins(Add, 3), dst(Reg::ColorOut, 0, 0x7), src(Reg::Temp, 0), src(Reg::Const, 197)});
    t.insert(t.end(), {ins(EndIf, 0)});
    t.push_back(0x0000FFFF);
    return t;
}

} // namespace

TEST(Sm3Parser, SyntheticVertexShader) {
    const auto t = syntheticVs();
    const Program p = parse(std::span<const uint32_t>(t));
    EXPECT_FALSE(p.pixel);
    EXPECT_EQ(p.major, 3);
    EXPECT_EQ(p.tokenCount, t.size());
    ASSERT_EQ(p.constants.size(), 2u);
    EXPECT_EQ(p.creator, "ou test");
    ASSERT_NE(p.constant("SkinMatrices"), nullptr);
    EXPECT_EQ(p.constant("SkinMatrices")->registerIndex, 85);
    EXPECT_EQ(p.constant("SkinMatrices")->registerCount, 3);
    EXPECT_EQ(p.constant("WorldViewProject")->registerSet, 2);
    ASSERT_EQ(p.instructions.size(), 13u);
    const auto& d = p.instructions[2];
    EXPECT_EQ(d.op, Dcl);
    EXPECT_EQ(d.usage, Usage::Texcoord);
    EXPECT_EQ(d.usageIndex, 1);
    const auto& mad = p.instructions[6];
    EXPECT_EQ(mad.op, Mad);
    ASSERT_EQ(mad.src.size(), 3u);
    EXPECT_EQ(mad.src[0].mod, 1);
    EXPECT_EQ(mad.src[0].swizzle, 0x55);
    EXPECT_TRUE(mad.src[1].relative);
    EXPECT_EQ(mad.src[1].relType, Reg::Addr);
    EXPECT_EQ(mad.src[1].num, 85);
    EXPECT_EQ(p.instructions[10].dst.mod, 1); // saturate
    const std::string dis = disassemble(p);
    EXPECT_NE(dis.find("vs_3_0"), std::string::npos);
    EXPECT_NE(dis.find("// SkinMatrices c85 [3]"), std::string::npos);
    EXPECT_NE(dis.find("mad r0, -v0.yyyy, c[a0.x + 85], c8"), std::string::npos) << dis;
    EXPECT_NE(dis.find("def c8, 0.5, 1, 2, -1"), std::string::npos) << dis;
    EXPECT_NE(dis.find("dcl_texcoord1 v1"), std::string::npos);
    EXPECT_NE(dis.find("mov_sat o0, r0"), std::string::npos);
    EXPECT_NE(dis.find("mov o1.xy, v1.wzyx"), std::string::npos) << dis;
}

TEST(Sm3Parser, RejectsMalformed) {
    auto t = syntheticVs();
    t.pop_back(); // no end token
    EXPECT_THROW(parse(std::span<const uint32_t>(t)), cdc::FormatError);
    auto u = syntheticPs();
    u[u.size() - 6] = ins(Add, 9); // the last arithmetic instruction's length runs past the end
    EXPECT_THROW(parse(std::span<const uint32_t>(u)), cdc::FormatError);
    const uint32_t sm1[] = {0xFFFE0101, 0xFFFF};
    EXPECT_THROW(parse(std::span<const uint32_t>(sm1)), cdc::FormatError);
    const uint8_t odd[] = {0, 3, 0xFF};
    EXPECT_THROW(parse(std::span<const uint8_t>(odd)), cdc::FormatError);
}

TEST(Sm3Parser, ShaderLibContainer) {
    // kind 1 (pixel), 4 slots: slot 0 and 2 present, 1 and 3 empty.
    const auto ps = syntheticPs();
    std::vector<uint32_t> d{1, 16, 0, 0, ~0u, 0, ~0u};
    const uint32_t entryWords = 4 + uint32_t(ps.size());
    d[3] = uint32_t((d.size() - 1) * 4);
    auto entry = [&](uint32_t hashLo) {
        d.insert(d.end(), {0xAABBCCDD, (entryWords * 4) | (2u << 16), hashLo, 0x01020304});
        d.insert(d.end(), ps.begin(), ps.end());
    };
    entry(7);
    d[5] = uint32_t((d.size() - 1) * 4);
    entry(9);
    const auto* bytes = reinterpret_cast<const uint8_t*>(d.data());
    const ShaderLib lib = parseShaderLib({bytes, d.size() * 4});
    EXPECT_TRUE(lib.pixel);
    EXPECT_EQ(lib.slots, 4u);
    ASSERT_EQ(lib.entries.size(), 2u);
    EXPECT_EQ(lib.entries[1].index, 2u);
    EXPECT_EQ(lib.entries[1].hash, 0x0102030400000009ull);
    EXPECT_EQ(lib.entries[1].flags, 2);
    EXPECT_EQ(lib.entries[0].bytecode.size(), ps.size() * 4);
    EXPECT_EQ(parse(lib.find(2)->bytecode).instructions.size(), parse(std::span<const uint32_t>(ps)).instructions.size());
    d.resize(d.size() - 8); // truncated last entry
    EXPECT_THROW(parseShaderLib({bytes, d.size() * 4}), cdc::FormatError);
}

TEST(Sm3Translate, SyntheticProgramsCompile) {
    for (const auto& t : {syntheticVs(), syntheticPs()}) {
        const Program p = parse(std::span<const uint32_t>(t));
        for (bool vk : {true, false}) {
            const auto g = render::sm3::translate(p, vk);
            ASSERT_TRUE(g.error.empty()) << g.error;
            std::string err;
            EXPECT_FALSE(render::sm3::compileSpirv(g.source, p.pixel, vk, err).empty()) << err << "\n" << g.source;
            if (p.pixel) {
                EXPECT_EQ(g.samplers, 1u << 1);
                EXPECT_NE(g.source.find("ouMat.ps[0]"), std::string::npos);        // c100 -> material
                EXPECT_NE(g.source.find("ouMat.fixedParams.x"), std::string::npos); // c2 AlphaThreshold
                EXPECT_NE(g.source.find("discard"), std::string::npos);
                // Light slot 3 reads the object's light set: b3 and c197 = slot 3 register 4 (colour).
                EXPECT_NE(g.source.find("if (OU_LIGHT(3) != 255u)"), std::string::npos);
                EXPECT_NE(g.source.find("ouFrame.lights[min(OU_LIGHT(3), 63u) * 5u + 4u]"), std::string::npos);
                EXPECT_NE(g.source.find("gl_FrontFacing"), std::string::npos);
                // Sun shadow / RT hooks: the program compiles with render/sun.glsl as the backends build it.
                EXPECT_FALSE(g.sunLit); // no SunLight colour (c212) read
                EXPECT_NE(g.source.find("ouSunEnd(ouColor)"), std::string::npos);
                for (bool rt : {false, vk})
                    EXPECT_FALSE(render::sm3::compileSpirv(render::originalPixelSource(g.source, vk, rt), true, vk, err).empty()) << err;
            } else {
                EXPECT_EQ(g.attributes, (1u << render::sm3::kAttrPosition) | (1u << render::sm3::kAttrTexcoord1));
                EXPECT_NE(g.source.find("ouObj.v[0]"), std::string::npos);     // WorldViewProject -> object block
                EXPECT_NE(g.source.find("c8_def"), std::string::npos);
                EXPECT_NE(g.source.find("clamp(a0.x + 85"), std::string::npos); // relative -> frame file
            }
        }
    }
}

// Every ShaderLib shader of the install parses, translates (both flavours) and compiles to SPIR-V.
TEST(Sm3Real, EveryInstallShaderTranslates) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto all = collectInstallShaders(g);
    EXPECT_EQ(all.errors.size(), 0u);
    EXPECT_EQ(all.hashMismatches, 0u);
    EXPECT_GT(all.shaders.size(), 5000u);
    std::atomic<size_t> next{0}, vs{0}, ps{0};
    std::mutex m;
    std::map<std::string, size_t> failures;
    auto work = [&] {
        for (size_t i; (i = next++) < all.shaders.size();) {
            const auto& sh = all.shaders[i];
            std::string why;
            try {
                const Program p = parse(std::span<const uint8_t>(sh.bytecode));
                for (bool vk : {true, false}) {
                    const auto t = render::sm3::translate(p, vk);
                    if (!t.error.empty()) { why = "translate: " + t.error; break; }
                    std::string err;
                    if (render::sm3::compileSpirv(t.source, p.pixel, vk, err).empty()) {
                        why = std::string(vk ? "vulkan" : "opengl") + " compile: " + err.substr(0, 160);
                        break;
                    }
                    // With the sun shadow / RT hooks (render/sun.glsl) as the backends compile them (Vulkan ray-traced, GL
                    // map); sun-lit programs only (the hooks of the rest differ only in the constant argument).
                    if (t.sunLit && render::sm3::compileSpirv(render::originalPixelSource(t.source, vk, vk), true, vk, err).empty()) {
                        why = std::string(vk ? "vulkan" : "opengl") + " sun compile: " + err.substr(0, 160);
                        break;
                    }
                }
                (p.pixel ? ps : vs)++;
            } catch (const std::exception& e) {
                why = std::string("parse: ") + e.what();
            }
            if (!why.empty()) {
                std::lock_guard l(m);
                if (failures[why]++ < 3)
                    std::printf("lib %u slot %u %016llx: %s\n", sh.sectionId, sh.index, (unsigned long long)sh.hash, why.c_str());
            }
        }
    };
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < std::max(2u, std::min(8u, std::thread::hardware_concurrency())); ++i) threads.emplace_back(work);
    for (auto& t : threads) t.join();
    size_t failed = 0;
    for (const auto& [k, n] : failures) failed += n;
    std::printf("%zu DRMs, %zu ShaderLib sections, %zu entries -> %zu distinct shaders: %zu vs + %zu ps parsed, %zu failed\n",
                all.drms, all.libs, all.entries, all.shaders.size(), size_t(vs), size_t(ps), failed);
    EXPECT_EQ(failed, 0u);
}

// Engine inputs decoded from the units (SHADERS.md "Engine inputs"): fog/ambient render settings, the ballcourt sun,
// materials with composite programs, light maps on terrain submeshes.
TEST(Sm3Real, LevelEnvironmentAndMaterials) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto bed = formats::loadLevelScene(bf, "pc-w\\l3_main_bedroom.drm");
    ASSERT_TRUE(bed.environment.valid);
    EXPECT_NEAR(bed.environment.fogColor[0], 76 / 255.0f, 1e-4);
    EXPECT_NEAR(bed.environment.fogFar, 63211.1f, 1.0f);
    EXPECT_NEAR(bed.environment.ambient[2], 0.05f, 1e-3);
    EXPECT_FALSE(bed.environment.sun);
    // 18 light records (LEVELS.md "Lights"): torches are orange point lights (record colour override 246,154,36).
    EXPECT_EQ(bed.environment.lights.size(), 18u);
    size_t torches = 0;
    for (const auto& l : bed.environment.lights) torches += l.type == 0 && l.color[0] > 1.5f * l.color[2] && l.flickerPeriod > 0;
    EXPECT_GT(torches, 0u);
    size_t withPrograms = 0, lightMapped = 0;
    for (const auto& m : bed.materials) withPrograms += m.vertexShader && m.pixelShader;
    for (const auto& m : bed.meshes)
        for (const auto& s : m.submeshes) lightMapped += s.instanceTextures[0] != 0;
    EXPECT_GT(withPrograms, bed.materials.size() * 9 / 10);
    EXPECT_GT(lightMapped, 100u);
    const auto prepared = render::original::prepare(bed, true);
    // Every material with composite programs translates (irradiance-lit dynamic-object programs included).
    EXPECT_EQ(prepared.translated + prepared.fallback, bed.materials.size());
    EXPECT_EQ(prepared.translated, withPrograms);
    const auto bc = formats::loadLevelScene(bf, "pc-w\\l4_ballcourt.drm");
    ASSERT_TRUE(bc.environment.sun);
    EXPECT_NEAR(bc.environment.sunDirection[0], -0.54406f, 1e-3);
    EXPECT_NEAR(bc.environment.sunDirection[2], -0.76227f, 1e-3);
    EXPECT_NEAR(bc.environment.sunColor[2], 1.2f, 1e-3);
    EXPECT_EQ(bc.environment.sunCookie, 0x131Du); // sun DTP +0x828, loaded with the level textures
    EXPECT_TRUE(std::any_of(bc.textures.begin(), bc.textures.end(), [](const auto& t) { return t.id == 0x131D; }));
}

// Character programs (Lara: IrradianceState SH + light slots, skinned VS variants unused: CPU skinning) translate and
// compile for both APIs.
TEST(Sm3Real, CharacterProgramsCompile) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto lara = formats::loadDrmScene(bf, "pc-w\\lara.drm", true);
    for (bool vk : {true, false}) {
        const auto prep = render::original::prepare(lara, vk);
        size_t withPrograms = 0, irradiance = 0;
        for (const auto& m : lara.materials) withPrograms += m.vertexShader && m.pixelShader;
        EXPECT_GT(withPrograms, 5u);
        EXPECT_EQ(prep.translated, withPrograms);
        for (const auto& p : prep.programs) {
            irradiance += p.psGlsl.source.find("ouFrame.ps[76]") != std::string::npos;
            std::string err;
            EXPECT_FALSE(render::sm3::compileSpirv(p.vsGlsl.source, false, vk, err).empty()) << err;
            EXPECT_FALSE(render::sm3::compileSpirv(p.psGlsl.source, true, vk, err).empty()) << err;
        }
        EXPECT_GT(irradiance, 0u);
    }
}

// Light registers and light sets (render/OriginalShading, SHADERS.md "Engine inputs").
TEST(OriginalShading, LightTableAndSets) {
    scene::Environment env;
    env.valid = true;
    scene::Light point;
    point.position[0] = 1000, point.radius = 500, point.color[0] = point.color[1] = point.color[2] = 2;
    point.falloff.assign(256, 0);
    for (int i = 0; i < 255; ++i) point.falloff[size_t(i)] = uint8_t(255 - i);
    scene::Light spot = point;
    spot.type = 1, spot.position[0] = 0, spot.direction[2] = -1, spot.coneInner = 1.0f, spot.coneOuter = 2.0f, spot.nearWidth = 100;
    spot.falloff[10] = 7; // a second distinct curve
    env.lights = {point, spot};
    render::sm3::FrameBlock f{};
    render::original::fillLights(f, env, 0);
    // Point: (1/r, 0, 0, row), cone (0, 1), apex plane -r - 1, colour w = luminance.
    EXPECT_FLOAT_EQ(f.lights[2][0], 1 / 500.f);
    EXPECT_FLOAT_EQ(f.lights[3][1], 1.f);
    EXPECT_FLOAT_EQ(f.lights[3][2], -501.f);
    EXPECT_FLOAT_EQ(f.lights[4][3], 2.f);
    // Spot: apex moved back along the direction by nearWidth / tan(outer / 2); u offset -apex / r.
    const float apex = 100 / std::tan(1.0f);
    EXPECT_NEAR(f.lights[5 + 0][2], apex, 1e-3);
    EXPECT_NEAR(f.lights[5 + 2][2], -apex / 500, 1e-6);
    EXPECT_NEAR(f.lights[5 + 3][0] * std::cos(0.5f) + f.lights[5 + 3][1], 1.f, 1e-4); // inner edge = 1
    EXPECT_NEAR(f.lights[5 + 3][0] * std::cos(1.0f) + f.lights[5 + 3][1], 0.f, 1e-4); // outer edge = 0
    EXPECT_NE(f.lights[2][3], f.lights[5 + 2][3]); // distinct falloff rows
    const auto lut = render::original::attenuationLut(env);
    const int row1 = int(f.lights[5 + 2][3] * 128);
    EXPECT_EQ(lut.rgba[(size_t(row1) * 256 + 10) * 4], 7);
    // Light sets: only lights whose radius reaches the sphere, strongest first, unused slots 0xFF.
    const auto near = render::original::lightSet(env, {900, 0, 0}, 10);
    EXPECT_EQ(near.packed[0] & 0xFF, 0u);                 // the point light (spot is 900 away, radius 500)
    EXPECT_EQ(near.packed[0] >> 8 & 0xFF, 0xFFu);
    EXPECT_EQ(near.packed[1], ~0u);
    const auto both = render::original::lightSet(env, {500, 0, 0}, 100);
    EXPECT_NE(both.packed[0] >> 8 & 0xFF, 0xFFu);
    EXPECT_EQ(render::original::lightSet(env, {5000, 0, 0}, 10).packed[0], ~0u);
}

// Light sets keep the six heaviest lights, heaviest first, equal weights in light order.
TEST(OriginalShading, LightSetOrder) {
    scene::Environment env;
    env.valid = true;
    for (int i = 0; i < 9; ++i) {
        scene::Light l;
        l.radius = 1000;
        l.color[0] = l.color[1] = l.color[2] = i == 7 ? 5.0f : i % 2 ? 1.0f : 2.0f; // 7 heaviest, evens next, odds last
        env.lights.push_back(l);
    }
    const auto s = render::original::lightSet(env, {0, 0, 0}, 1);
    auto slot = [&](int k) { return int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF); };
    constexpr int kExpected[6] = {7, 0, 2, 4, 6, 8};
    for (int k = 0; k < 6; ++k) EXPECT_EQ(slot(k), kExpected[k]) << k;
}

// Shadow cascade culling: only spheres wholly outside the orthographic x / y clip range are skipped.
TEST(RenderCommon, OutsideOrtho) {
    const glm::mat4 vp = glm::ortho(-100.0f, 100.0f, -50.0f, 50.0f, -1000.0f, 1000.0f);
    EXPECT_FALSE(render::outsideOrtho(vp, {0, 0, 0}, 1));
    EXPECT_FALSE(render::outsideOrtho(vp, {105, 0, 0}, 10));  // overlaps the right edge
    EXPECT_TRUE(render::outsideOrtho(vp, {120, 0, 0}, 10));
    EXPECT_TRUE(render::outsideOrtho(vp, {0, -70, 0}, 10));
    EXPECT_FALSE(render::outsideOrtho(vp, {0, 0, 5000}, 10)); // depth is never culled
}

// Neighbour units draw through their stream portals (RenderCommon portalClips, docs/RENDERING.md "Portal clipping").
TEST(PortalClips, NeighbourSeenOnlyThroughItsPortal) {
    using render::PortalUnit;
    const glm::vec3 q[4] = {{-1000, -100, -100}, {-1000, 100, -100}, {-1000, 100, 100}, {-1000, -100, 100}};
    std::vector<PortalUnit> units(3);
    units[0] = {0, 10, {{1, {q[0], q[1], q[2], q[3]}, {1, 0, 0}}}};   // A: portal at x -1000 to B, normal into A (+x)
    units[1] = {10, 5, {{0, {q[0], q[1], q[2], q[3]}, {-1, 0, 0}}}}; // B: the back portal
    units[2] = {15, 5, {}};                                           // C: loaded, no portal from A
    render::Camera cam;
    cam.position = {0, 0, 0};
    cam.lookAt({-1, 0, 0});
    auto clips = render::portalClips(units, 0, cam, 1.0f);
    ASSERT_EQ(clips.size(), 3u);
    EXPECT_FLOAT_EQ(clips[0].rect[0], -1.0f);
    EXPECT_FLOAT_EQ(clips[0].rect[2], 1.0f);
    const float e = 100.0f / 1000.0f / std::tan(glm::radians(cam.verticalFovDeg) * 0.5f); // quad half-size in NDC
    EXPECT_NEAR(clips[1].rect[0], -e, 1e-4f);
    EXPECT_NEAR(clips[1].rect[3], e, 1e-4f);
    EXPECT_TRUE(render::clipHidden(clips[2]));
    EXPECT_EQ(render::clipIndex(clips, 12), 1);
    EXPECT_EQ(render::clipIndex(clips, 25), -1);
    const auto px = render::clipPixels(clips[1], 1000, 1000, true);
    EXPECT_NEAR(px.x + px.width * 0.5f, 500.0f, 1.5f);
    EXPECT_NEAR(px.width, e * 1000.0f + 2, 2.5f);

    cam.lookAt({1, 0, 0}); // portal behind the camera
    EXPECT_TRUE(render::clipHidden(render::portalClips(units, 0, cam, 1.0f)[1]));
    cam.position = {-1500, 0, 0}; // through the portal: B whole
    cam.lookAt({0, 0, 0});
    clips = render::portalClips(units, 0, cam, 1.0f);
    EXPECT_FLOAT_EQ(clips[1].rect[0], -1.0f);
    EXPECT_FLOAT_EQ(clips[1].rect[3], 1.0f);
    cam.position = {-999.5f, 0, 0}; // in the doorway: the quad lies behind the near plane
    clips = render::portalClips(units, 0, cam, 1.0f);
    EXPECT_FLOAT_EQ(clips[1].rect[2], 1.0f);
    cam.position = {-1500, 5000, 0}; // past the portal's plane far from its quad: still in A, the portal faces away
    cam.lookAt({-3000, 5000, 0});
    EXPECT_TRUE(render::clipHidden(render::portalClips(units, 0, cam, 1.0f)[1]));
}
