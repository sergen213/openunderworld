// D3D9 SM3 parser/disassembler (formats/Shader), TRU ShaderLib container, SM3 -> GLSL translator (render/ShaderTranslate).
#include "cdc/Cdc.hpp"
#include "formats/Shader.hpp"
#include "formats/Formats.hpp"
#include "render/Fluid.hpp"
#include "render/OriginalShading.hpp"
#include "render/Ocean.hpp"
#include "render/RenderCommon.hpp"
#include "render/ShaderTranslate.hpp"
#include "render/Water.hpp"
#include "render/WaterField.hpp"

#include <gtest/gtest.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
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
                EXPECT_NE(g.source.find("OU_LIGHT_ROW(3, 4u)"), std::string::npos);
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
            irradiance += p.psGlsl.readsIrradiance && p.psGlsl.source.find("ouFrame.irradiance[") != std::string::npos;
            std::string err;
            EXPECT_FALSE(render::sm3::compileSpirv(p.vsGlsl.source, false, vk, err).empty()) << err;
            EXPECT_FALSE(render::sm3::compileSpirv(p.psGlsl.source, true, vk, err).empty()) << err;
        }
        EXPECT_GT(irradiance, 0u);
    }
}

// Light probes (formats::sampleIrradiance, LEVELS.md "Irradiance volume") at l1_kraken_entrance's snake head 751 and
// press plate 752 match tru.exe: their SceneEntity irradiance caches (+0x13C: bounding centre +0x10, raw SH +0x60, region
// tint +0x20, scale +0x34, L00 offset +0x30) read under Wine, and the IrradianceState Base hooked at 0x5043D0 (~0.095,
// 0.098, 0.143).
TEST(OriginalShading, KrakenHeadIrradianceMatchesOriginal) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto unit = formats::parseUnit(cdc::parseDrm(*bf.read("pc-w\\l1_kraken_entrance.drm")));
    const auto& v = unit.environment.irradiance;
    ASSERT_EQ(v.probes.size(), 84u);
    ASSERT_EQ(v.regions.size(), 2u);
    // Bounding centres: tru.exe world minus the cell group origin (558.18, -2202.97, -882.69) = our unit frame.
    const glm::vec3 origin(558.176f, -2202.97f, -882.692f);
    struct Case { glm::vec3 world; float raw[9][3]; };
    const Case cases[] = {
        {{-1155.5076f, -5346.541f, 1459.2693f}, // head 751
         {{0.02855f, 0.03177f, 0.04444f}, {0.00051f, 0.00024f, -0.00069f}, {0.00058f, 0.00062f, 0.00068f},
          {0.00135f, 0.00112f, 0.00084f}, {0.00131f, 0.00147f, 0.00185f}, {-0.00064f, -0.00061f, -0.00077f},
          {0.00003f, -0.00007f, -0.00006f}, {0.00062f, 0.00081f, 0.00083f}, {-0.00031f, -0.00061f, -0.00045f}}},
        {{-1155.3252f, -4853.8184f, -866.5247f}, // plate 752
         {{0.02845f, 0.03167f, 0.04418f}, {0.00116f, 0.00121f, 0.00104f}, {-0.00053f, -0.00045f, -0.00124f},
          {0.00297f, 0.00311f, 0.00385f}, {0.00050f, 0.00071f, 0.00081f}, {0.00002f, 0.00009f, -0.00053f},
          {0.00058f, 0.00064f, 0.00105f}, {-0.00065f, -0.00025f, -0.00043f}, {-0.00007f, -0.00028f, -0.00003f}}},
    };
    const float tint[3] = {0.0f, 17.0f / 255.0f, 1.0f}, scale = 1.20432f, add = 0.10752f;
    float sh[9][3];
    for (const auto& c : cases) {
        ASSERT_TRUE(formats::sampleIrradiance(v, c.world - origin, sh));
        for (int k = 0; k < 9; ++k)
            for (int i = 0; i < 3; ++i)
                EXPECT_NEAR(sh[k][i], c.raw[k][i] * tint[i] * scale + (k == 0 ? add : 0.0f), 2e-5f) << k << " " << i;
    }
    ASSERT_TRUE(formats::sampleIrradiance(v, cases[0].world - origin, sh));
    const glm::vec3 centre = cases[0].world - origin;
    float state[7][4];
    render::original::irradianceState(sh, state);
    EXPECT_NEAR(state[6][0], 0.095f, 2e-3f);
    EXPECT_NEAR(state[6][1], 0.098f, 2e-3f);
    EXPECT_NEAR(state[6][2], 0.143f, 2e-3f);
    // Our head meshes (scene " @intro 751") are centred where tru.exe's entity is: the renderers sample there.
    const auto scene = formats::loadLevelScene(bf, "pc-w\\l1_kraken_entrance.drm");
    glm::vec3 lo(INFINITY), hi(-INFINITY);
    for (const auto& m : scene.meshes)
        if (m.origin.ends_with(" @intro 751")) {
            const glm::vec4 s = render::original::boundingSphere(m.vertices, glm::make_mat4(m.transform));
            lo = glm::min(lo, glm::vec3(s) - s.w), hi = glm::max(hi, glm::vec3(s) + s.w);
        }
    ASSERT_LE(lo.x, hi.x);
    EXPECT_LT(glm::distance((lo + hi) * 0.5f, centre), 100.0f);
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
    constexpr int S = render::sm3::kLightRegs; // the spot's rows
    const float apex = 100 / std::tan(1.0f);
    EXPECT_NEAR(f.lights[S + 0][2], apex, 1e-3);
    EXPECT_NEAR(f.lights[S + 2][2], -apex / 500, 1e-6);
    EXPECT_NEAR(f.lights[S + 3][0] * std::cos(0.5f) + f.lights[S + 3][1], 1.f, 1e-4); // inner edge = 1
    EXPECT_NEAR(f.lights[S + 3][0] * std::cos(1.0f) + f.lights[S + 3][1], 0.f, 1e-4); // outer edge = 0
    EXPECT_NE(f.lights[2][3], f.lights[S + 2][3]); // distinct falloff rows
    const auto lut = render::original::attenuationLut(env);
    const int row1 = int(f.lights[S + 2][3] * 128);
    EXPECT_EQ(lut.rgba[(size_t(row1) * 256 + 10) * 4], 7);
    // No cookies: w row (0, 0, 0, 1), layer -1.
    EXPECT_FLOAT_EQ(f.lights[7][3], 1.f);
    EXPECT_FLOAT_EQ(f.lights[8][0], -1.f);
    // Light sets: only lights whose radius reaches the sphere; plain lights take slots 3-5, unused slots 0xFF.
    auto slot = [](render::original::LightSet s, int k) { return int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF); };
    const auto near = render::original::lightSet(env, {900, 0, 0}, 10);
    EXPECT_EQ(near.packed[0], 0x00FFFFFFu); // the point light in slot 3 (spot is 900 away, radius 500)
    EXPECT_EQ(near.packed[1], ~0u);
    const auto both = render::original::lightSet(env, {500, 0, 0}, 100);
    EXPECT_EQ(slot(both, 3), 0);
    EXPECT_EQ(slot(both, 4), 1);
    EXPECT_EQ(render::original::lightSet(env, {5000, 0, 0}, 10).packed[0], ~0u);
}

// Spot cookie rows (0x50B910): a point on the cone axis maps to the cookie centre, the cone edge at the light to
// 0.5 ± 0.5 · radius / (radius + apex); the layer is the cookie's index in the backend's table.
TEST(OriginalShading, SpotCookieProjection) {
    scene::Environment env;
    scene::Light spot;
    spot.type = 1, spot.radius = 1000, spot.coneOuter = 1.0f, spot.coneInner = 0.5f, spot.nearWidth = 50, spot.cookie = 0x42;
    spot.direction[2] = -1, spot.axes[0][0] = 1, spot.axes[1][1] = -1;
    spot.falloff.assign(256, 255);
    env.lights = {spot};
    render::sm3::FrameBlock f{};
    const uint32_t ids[] = {7, 0x42};
    render::original::fillLights(f, env, 0, ids);
    EXPECT_FLOAT_EQ(f.lights[8][0], 1.f);
    auto uv = [&](glm::vec3 p) {
        const glm::vec4 q(p, 1);
        const float w = glm::dot(glm::make_vec4(f.lights[7]), q);
        return glm::vec2(glm::dot(glm::make_vec4(f.lights[5]), q), glm::dot(glm::make_vec4(f.lights[6]), q)) / w;
    };
    EXPECT_NEAR(uv({0, 0, -400}).x, 0.5f, 1e-5);
    EXPECT_NEAR(uv({0, 0, -400}).y, 0.5f, 1e-5);
    const float apex = 50 / std::tan(0.5f), r = 1000;
    EXPECT_NEAR(uv({50, 0, 0}).x, 0.5f + 0.5f * r / (r + apex), 1e-4); // the cone's edge at the light, +X
    EXPECT_NEAR(uv({0, 50, 0}).y, 0.5f - 0.5f * r / (r + apex), 1e-4); // +Y = -axis Y
    render::original::fillLights(f, env, 0, {}); // not in the table: no cookie
    EXPECT_FLOAT_EQ(f.lights[8][0], -1.f);
}

// tru.exe light slots (0x504CC0 / 0x5042B0): cookie spots 0-1, shadow casters next (3 special at most), plain lights
// 3-5, a 4th plain light in a free special slot, later lights dropped; scene order decides.
TEST(OriginalShading, LightSetGroups) {
    scene::Environment env;
    auto add = [&](uint8_t type, bool shadow, uint32_t cookie) {
        scene::Light l;
        l.type = type, l.shadow = shadow, l.cookie = cookie, l.radius = 1000, l.color[0] = 1;
        env.lights.push_back(l);
    };
    add(0, false, 0); // 0 plain -> 3
    add(1, true, 0);  // 1 shadow spot -> after the cookies: 1
    add(0, false, 0); // 2 plain -> 4
    add(1, false, 9); // 3 cookie spot -> 0
    add(0, false, 0); // 4 plain -> 5
    add(0, false, 0); // 5 4th plain -> free special slot 2
    add(0, false, 0); // 6 dropped (3 special + 3 plain)
    add(1, true, 0);  // 7 dropped
    const auto s = render::original::lightSet(env, {0, 0, 0}, 1);
    auto slot = [&](int k) { return int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF); };
    constexpr int kExpected[6] = {3, 1, 5, 0, 2, 4};
    for (int k = 0; k < 6; ++k) EXPECT_EQ(slot(k), kExpected[k]) << k;
}

// Lara at the l3_main_bedroom start, read in tru.exe under Wine (2026-11-06): the entity's light list at the SetLights
// call (0x55FDF0) holds the corridor's two cold point lights and the shadow-casting torch spot (tru.exe world = ours +
// (9623, 331, -2978)); 0x5042B0 puts the spot in slot 0 and the points in slots 3 and 4.
TEST(OriginalShading, BedroomLightSetMatchesOriginal) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto bed = formats::loadLevelScene(bf, "pc-w\\l3_main_bedroom.drm");
    const auto& L = bed.environment.lights;
    const glm::vec3 offset(-9623, -331, 2978);
    auto find = [&](glm::vec3 tru) {
        for (size_t i = 0; i < L.size(); ++i)
            if (glm::distance(glm::make_vec3(L[i].position), tru + offset) < 2) return int(i);
        return -1;
    };
    const int spot = find({5410, 186, -1567}), a = find({5308, 4586, -1905}), b = find({5308, 8099, -1905});
    ASSERT_GE(spot, 0);
    ASSERT_GE(a, 0);
    ASSERT_GE(b, 0);
    EXPECT_TRUE(L[size_t(spot)].shadow);
    // Lara standing at tru.exe (5358, 6291, -2890): bounding sphere about her hips.
    const auto s = render::original::lightSet(bed.environment, glm::vec3(5358, 6291, -2890 + 90) + offset, 110);
    auto slot = [&](int k) { return int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF); };
    EXPECT_EQ(slot(0), spot);
    EXPECT_EQ(slot(1), 0xFF);
    EXPECT_EQ(slot(2), 0xFF);
    EXPECT_EQ(std::min(slot(3), slot(4)), std::min(a, b)); // the order of these two follows tru.exe heap addresses
    EXPECT_EQ(std::max(slot(3), slot(4)), std::max(a, b));
    EXPECT_EQ(slot(5), 0xFF);
}

// Light switch by camera distance (tru.exe 0x5CC190 -> SceneLight::SetEnabled 0x5740C0; LEVELS.md "Lights"). Wine,
// l3_main_bedroom start (openunderworld-work/tlp/hook5.py, 2026-11-06): with the camera at tru.exe (5358, 7553,
// -2422) every frame switches lights 8, 13, 14, 15 and 17 off and the other 13 on.
TEST(OriginalShading, BedroomLightsSwitchByCameraDistance) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto bed = formats::loadLevelScene(bf, "pc-w\\l3_main_bedroom.drm");
    ASSERT_EQ(bed.environment.lights.size(), 18u);
    const glm::vec3 eye = glm::vec3(5358, 7553, -2422) + glm::vec3(-9623, -331, 2978);
    std::vector<int> off;
    for (size_t i = 0; i < bed.environment.lights.size(); ++i)
        if (render::original::lightFade(bed.environment.lights[i], eye) <= 0) off.push_back(int(i));
    EXPECT_EQ(off, (std::vector<int>{8, 13, 14, 15, 17}));
    // Walking into the far rooms switches them on: light 8 at full intensity from its own position.
    EXPECT_EQ(render::original::lightFade(bed.environment.lights[8], glm::make_vec3(bed.environment.lights[8].position)), 1.0f);
    // Lights left out of the light sets while off.
    const auto& l8 = bed.environment.lights[8];
    const auto s = render::original::lightSet(bed.environment, glm::make_vec3(l8.position), 10, ~0u, &eye);
    for (int k = 0; k < 6; ++k) EXPECT_NE(int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF), 8);
}

// tru.exe's PSSM (0x50CCB0) at the l2_junglehub start, read under Wine (openunderworld-work/tlp/hook4.py + dump4.py,
// 2026-11-06): the camera context (tru.exe frame: position, forward, 45 deg, 1.7778, near 16, far 105585.445) and the
// registers the device got: VS c81-c84 WorldToPSSM0, PS c80-c85 PSSMToMap, c96 PSSMDistances. The sun's rows were its
// PCRenderLight matrix; the unit's render settings give the split fractions.
TEST(OriginalShading, PssmSplitsMatchOriginal) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto hub = formats::loadLevelScene(bf, "pc-w\\l2_junglehub.drm");
    scene::Environment env = hub.environment;
    ASSERT_TRUE(env.sun);
    const glm::vec3 X(-0.422193f, -0.867489f, -0.263089f), Y(0.671411f, -0.104266f, -0.733715f), Z(-0.609056f, 0.48641f, -0.626464f);
    EXPECT_LT(glm::distance(glm::make_vec3(env.sunAxes[0]), X), 1e-4f);
    EXPECT_LT(glm::distance(glm::make_vec3(env.sunAxes[1]), Y), 1e-4f);
    EXPECT_LT(glm::distance(glm::make_vec3(env.sunDirection), Z), 1e-4f);
    EXPECT_NEAR(env.shadowBias[0], 1.52587891e-5f, 1e-10f);
    EXPECT_NEAR(env.shadowBias[1], 2.2f, 1e-6f);
    // The unit's own split fractions (render settings +0x28..); tru.exe blends them with l2_cliffhall_b's by 0x5C7CA0
    // (tracker weights 0.6815 / 0.3185 there, RENDERING.md "Neighbour units"), so the fitting below gets the fractions it used.
    EXPECT_NEAR(env.pssmSplits[0], 0.02622f, 1e-6f);
    EXPECT_NEAR(env.pssmSplits[2], 0.14847f, 1e-6f);
    const float dist[4] = {105585.445f, 17098.0059f, 7683.95605f, 2986.06616f};
    for (int k = 0; k < 4; ++k) env.pssmSplits[k] = dist[3 - k] / dist[0];
    EXPECT_NEAR(env.farPlane, 110619, 1); // blended likewise (cliffhall_b's 94817): the camera context had 105585.445
    env.farPlane = dist[0];
    render::Camera cam;
    cam.position = {-14841.0146f, -12219.7852f, -9989.26562f};
    cam.lookAt(cam.position + 10000.0f * glm::vec3(-0.000770510756f, 0.999991059f, 0.00415489636f));
    cam.verticalFovDeg = 45.0f, cam.nearZ = 16.0f, cam.farZ = 200000.0f;
    const auto p = render::original::pssm(cam, 1.7778f, env);
    ASSERT_TRUE(p.active);
    for (int k = 0; k < 4; ++k) EXPECT_NEAR(p.distances[k], dist[k], dist[k] * 1e-5f) << k;
    EXPECT_EQ(p.distances[4], 16.0f);
    render::sm3::FrameBlock f{};
    render::original::fillPssm(f, p, cam);
    const float ps[6][4] = {{6.17538881f, 6.17538548f, 2.0030396f, 0}, {-3.89644623f, -2.81223202f, 0.120937169f, 1},
                            {13.7412071f, 13.7411957f, 3.14637947f, 0}, {-9.84380817f, -6.10245466f, -0.192649007f, 1},
                            {34.8941994f, 34.8941689f, 3.34432364f, 0}, {-25.7916279f, -16.5638409f, -0.165199697f, 1}};
    for (int r = 0; r < 6; ++r)
        for (int c = 0; c < 4; ++c) EXPECT_NEAR(f.ps[80 + r][c], ps[r][c], 2e-3f + std::abs(ps[r][c]) * 1e-4f) << "c" << 80 + r << "." << c;
    for (int c = 0; c < 4; ++c) EXPECT_NEAR(f.ps[96][c], dist[c], dist[c] * 1e-5f);
    const float vs[4][4] = {{-5.9157594e-07f, -1.25437225e-06f, -2.91122637e-06f, -0.000770510756f},
                            {-1.21552341e-06f, 1.94796243e-07f, 2.32498951e-06f, 0.999991059f},
                            {-3.68640144e-07f, 1.37077336e-06f, -2.99443559e-06f, 0.00415489636f},
                            {0.725708008f, 0.47639975f, 0.285529315f, 12249.7441f}};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            EXPECT_NEAR(f.vs[81 + r][c], vs[r][c], r < 3 ? std::abs(vs[r][c]) * 1e-3f + 1e-9f : (c < 3 ? 1e-5f : 0.05f))
                << "vs c" << 81 + r << "." << c;
    EXPECT_EQ(f.bools[2], 1u);
    // A point on the split-3 view axis maps inside split 3's tile (1024..2048, 0..1024) at depth in (0, 1).
    const glm::vec4 q = p.map[3] * glm::vec4(cam.position + 1000.0f * cam.forward(), 1);
    EXPECT_GT(q.x * 4096, 1024);
    EXPECT_LT(q.x * 4096, 2048);
    EXPECT_GT(q.y * 3072, 0);
    EXPECT_LT(q.y * 3072, 1024);
    EXPECT_GT(q.z, 0);
    EXPECT_LT(q.z, 1);
}

// Terrain lights per node-tree leaf (tru.exe 0x56ABF0 / 0x55DC20 masks, drawn by 0x511810). Wine, l3_main_bedroom
// start (openunderworld-work/tlp/dump3.py, 2026-11-06): each leaf took the cell lights whose sphere reaches its box; on top of that tru.exe
// left out lights not seen this frame (L1, L11: SceneLight +0x2CA = 0), which we keep (they only free slots).
TEST(OriginalShading, TerrainLeafLightsMatchOriginal) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto bed = formats::loadLevelScene(bf, "pc-w\\l3_main_bedroom.drm");
    ASSERT_EQ(bed.environment.lights.size(), 18u);
    auto lightsAt = [&](glm::vec3 centre) {
        for (const auto& m : bed.meshes)
            for (const auto& boxes : m.lightBoxes)
                for (const auto& b : boxes)
                    if (glm::distance(centre, glm::vec3(b[0] + b[3], b[1] + b[4], b[2] + b[5]) * 0.5f) < 1) {
                        const auto s = render::original::lightSet(bed.environment, boxes, glm::make_mat4(m.transform));
                        std::set<int> out;
                        for (int k = 0; k < 6; ++k)
                            if (const int i = int(s.packed[k >> 2] >> (8 * (k & 3)) & 0xFF); i != 0xFF) out.insert(i);
                        return out;
                    }
        ADD_FAILURE() << "no terrain leaf at " << centre.x << " " << centre.y << " " << centre.z;
        return std::set<int>{};
    };
    // Corridor (cell 2): the cell-1 torch spot L7 reaches the corridor's terrain sphere but none of its leaves.
    EXPECT_EQ(lightsAt({-4237, 8441, 766}), (std::set<int>{2, 4, 6, 11}));
    EXPECT_EQ(lightsAt({-4259, 2812, 933}), (std::set<int>{2, 4, 5}));
    EXPECT_EQ(lightsAt({-4307, 5624, 942}), (std::set<int>{2, 4, 6}));
    EXPECT_EQ(lightsAt({-5409, 169, 1087}), (std::set<int>{0, 1, 2, 5, 7}));
    // Without lights each vertex buffer keeps one group holding all its leaves.
    std::vector<std::string> warnings;
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\l3_main_bedroom.drm"));
    for (const auto& [id, meshes] : formats::decodeRenderResources(drm, warnings))
        for (const auto& m : meshes) EXPECT_LE(m.lightBoxes.size(), 1u) << m.origin;
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

// tru.exe's under-water light rays (render::UnderwaterRays, docs/RENDERING.md "Under water"): a lit grid seen from
// under it adds light to the 50 x 50 image; looking away adds none; above the first depth marker nothing is drawn.
TEST(UnderwaterRays, DrawsColumnsInView) {
    scene::Environment::WaterRays r;
    r.grid = 4, r.mask.assign(16, 0xFF), r.halfSize = 2000, r.length = 20000;
    r.sun[2] = 1e6f;                            // straight above: vertical columns
    r.depthZ[0] = 1000, r.depthZ[1] = -1000;    // full strength below -1000
    r.life[0] = 3, r.life[1] = 10, r.extinction[1] = 3, r.vsExtinction = 2.11e-5f; // no dark spells
    render::UnderwaterRays rays;
    rays.reset(r);
    for (int i = 0; i < 25 * 60; ++i) rays.step(1.0f / 60); // past the first dark spells (up to 20 s)
    const float waves[4][4] = {};
    std::array<uint8_t, 2500> img{};
    render::Camera cam;
    cam.position = {-6000, 0, -3000};
    cam.lookAt({0, 0, -3000});
    ASSERT_TRUE(rays.draw(cam, 16.0f / 9.0f, waves, img));
    EXPECT_GT(*std::max_element(img.begin(), img.end()), 0);
    cam.lookAt({-12000, 0, -3000});
    ASSERT_TRUE(rays.draw(cam, 16.0f / 9.0f, waves, img));
    EXPECT_EQ(*std::max_element(img.begin(), img.end()), 0);
    cam.position.z = 2000;
    EXPECT_FALSE(rays.draw(cam, 16.0f / 9.0f, waves, img));
}

// ---- the splash fluid (render/Fluid.hpp) ----
namespace {
render::FluidBurst fluidBurst(glm::vec3 at, uint32_t count) {
    return {{at.x, at.y, at.z}, at.z, {0, 0, 0}, 100.0f, 300.0f, 1000.0f, 40.0f, 0u, count, 7u};
}
} // namespace

// The PBF kernel constants: a lattice of kSpacing inside the support sums to more than the particle's own poly6 weight;
// its gradient denominator is positive; the shaders' spacing constant is the C++ one.
TEST(SplashFluid, KernelConstantsAndShaderSpacing) {
    EXPECT_GT(render::fluid::restDensity(), 315.0f / (64.0f * 3.14159265f) * 2.0f);
    EXPECT_GT(render::fluid::latticeGradient(), 0.0f);
    char spacing[64];
    std::snprintf(spacing, sizeof spacing, "kOuFluidSpacing = %.1f;", render::fluid::kSpacing);
    EXPECT_NE(render::fluid::computeSource().find(spacing), std::string::npos) << "shaders/fluid.glsl and Fluid.hpp disagree";
    EXPECT_EQ(render::fluid::computeSource().find("\n#include"), std::string::npos) << "every include expanded for GL";
}

// The GL flavours of the splash passes compile (Vulkan builds the same files at build time with glslc).
TEST(SplashFluid, GlPassSourcesCompile) {
    std::string err;
    EXPECT_FALSE(render::sm3::compileSpirv(render::splashVertexSource(false), false, false, err).empty()) << err;
    EXPECT_FALSE(render::sm3::compileSpirv(render::splashSplatSource(false), true, false, err).empty()) << err;
    EXPECT_FALSE(render::sm3::compileSpirv(render::splashBlurSource(false), true, false, err).empty()) << err;
    EXPECT_FALSE(render::sm3::compileSpirv(render::splashCompositeSource(false), true, false, err).empty()) << err;
}

// The fixed-step clock: game time in kStep steps, at most kMaxSteps a frame (the rest dropped), none while paused; the
// first frame and a jump of more than a second start over (clear the pool).
TEST(SplashFluid, ClockRunsFixedSteps) {
    render::fluid::Sim sim;
    render::FrameOptions o;
    o.time = 10.0f;
    auto f = sim.frame(o, glm::vec3(0));
    EXPECT_TRUE(f.clear);
    EXPECT_EQ(f.steps, 0);
    o.time = 10.0f + 2.5f * render::fluid::kStep;
    EXPECT_EQ(sim.frame(o, glm::vec3(0)).steps, 2);
    EXPECT_EQ(sim.frame(o, glm::vec3(0)).steps, 0) << "paused";
    o.time += 1.6f * render::fluid::kStep; // the half step left over + 1.6
    EXPECT_EQ(sim.frame(o, glm::vec3(0)).steps, 2);
    o.time += 0.2f;
    EXPECT_EQ(sim.frame(o, glm::vec3(0)).steps, render::fluid::kMaxSteps);
    o.time += 0.5f * render::fluid::kStep;
    EXPECT_LE(sim.frame(o, glm::vec3(0)).steps, 1) << "the dropped time does not catch up";
    o.time += 5.0f;
    f = sim.frame(o, glm::vec3(0));
    EXPECT_EQ(f.steps, 0);
    EXPECT_FALSE(f.active);
}

// Bursts take consecutive ring slots (wrapping), far ones are skipped or thinned, a frame emits at most kMaxPerFrame;
// the fluid is active for kMaxLife after the last burst, and a later burst clears what an idle pool may still hold.
TEST(SplashFluid, BurstSlotsAndLifetime) {
    using render::fluid::kCapacity;
    render::fluid::Sim sim;
    render::FrameOptions o;
    o.time = 1.0f;
    const glm::vec3 eye(0);
    const render::FluidBurst bursts[3] = {fluidBurst({100, 0, 0}, 100), fluidBurst({20000, 0, 0}, 100),
                                          fluidBurst({0.75f * render::fluid::kRange, 0, 0}, 100)};
    o.fluidBursts = bursts;
    auto f = sim.frame(o, eye);
    ASSERT_EQ(f.bursts.size(), 2u);
    EXPECT_EQ(f.bursts[0].info.x, 0u);
    EXPECT_EQ(f.bursts[0].info.y, 100u);
    EXPECT_EQ(f.bursts[1].info.x, 100u);
    EXPECT_EQ(f.bursts[1].info.y, 50u) << "half the particles at three quarters of the range";
    EXPECT_EQ(f.emitted, 150);
    EXPECT_FLOAT_EQ(f.bursts[0].shape.y, 1000.0f);
    EXPECT_TRUE(f.active);
    o.fluidBursts = {};
    for (int i = 0; i < 6; ++i) { // 3 s: still alive
        o.time += 0.5f;
        EXPECT_TRUE(sim.frame(o, eye).active) << i;
    }
    o.time += 1.0f;
    EXPECT_FALSE(sim.frame(o, eye).active);
    const render::FluidBurst huge[2] = {fluidBurst({0, 0, 0}, uint32_t(kCapacity)), fluidBurst({0, 0, 0}, 10)};
    o.fluidBursts = huge;
    o.time += 0.1f;
    f = sim.frame(o, eye);
    EXPECT_TRUE(f.clear) << "particles of the idle run are cleared";
    ASSERT_EQ(f.bursts.size(), 1u);
    EXPECT_EQ(f.emitted, render::fluid::kMaxPerFrame);
    EXPECT_EQ(f.bursts[0].info.x, 150u);
    o.time += 0.1f;
    f = sim.frame(o, eye);
    EXPECT_FALSE(f.clear);
    ASSERT_EQ(f.bursts.size(), 1u);
    EXPECT_EQ(f.bursts[0].info.x, uint32_t(150 + render::fluid::kMaxPerFrame));
    EXPECT_EQ(f.emitted, render::fluid::kMaxPerFrame);
    o.time += 0.1f;
    EXPECT_EQ(sim.frame(o, eye).bursts[0].info.x, 150u) << "the ring wraps";
}


// ---- the interactive wave field's CPU side (render/WaterField.hpp; the waves run in shaders/water_field.comp) ----
namespace {
ou::render::FrameOptions fieldFrame(float time, float originX, float originY, uint64_t generation = 1) {
    ou::render::FrameOptions o;
    o.water = ou::render::WaterQuality::Enhanced;
    o.time = time;
    o.waterField.active = true;
    o.waterField.originX = originX, o.waterField.originY = originY;
    o.waterField.cellSize = 290.0f * 40.0f / ou::render::waterfield::kN;
    o.waterField.depth = 2.0f * 290.0f;
    o.waterField.generation = generation;
    return o;
}
} // namespace

// Floating bodies ride the drawn FFT swell (render::ocean::Swell): at cascade 0's texel centres it equals ocean.comp's
// transform (h(k, t) = h0 e^{i w t} + conj(h0(-k)) e^{-i w t}, quantized w, inverse DFT, x amplitude; cascade 1, whose
// texels lie elsewhere, as the same Fourier sum between them) minus water.vert's 0.5 sigma centring, here as a plain
// DFT; the 256 strongest wave vectors leave a small residual.
TEST(Ocean, CpuSwellIsTheDrawnHeight) {
    using namespace ou::render::ocean;
    constexpr double kPi = 3.14159265358979;
    const auto buffer = spectrumBuffer();
    for (const int n : {kMaxN, kMaxN / 2}) {
        const Grid& g = grid(n);
        const Swell all(n, size_t(2 * n * n)), strong(n, kSwellModes);
        const float w0 = 2.0f * float(kPi) / kLoopSeconds;
        float worst = 0, worstStrong = 0;
        for (const double t : {3.0, 77.25, 1000.5})
            for (const glm::ivec2 X : {glm::ivec2(0, 0), glm::ivec2(5, 17), glm::ivec2(n - 1, n / 3), glm::ivec2(n / 2, n - 2)}) {
                const glm::dvec2 pm = (glm::dvec2(X) + 0.5) / double(n) * double(g.length[0]); // metres
                double h = 0;
                for (int c = 0; c < 2; ++c)
                    for (int y = 0; y < n; ++y)
                        for (int x = 0; x < n; ++x) {
                            const glm::vec4 s = buffer[spectrumOffset(n) + size_t(c * n * n + y * n + x)];
                            const glm::vec2 k = glm::vec2(x <= n / 2 ? x : x - n, y <= n / 2 ? y : y - n) * float(2 * kPi / g.length[c]);
                            const float kl = glm::length(k);
                            const float w = std::sqrt((9.81f * kl + 0.073f / 1025.0f * kl * kl * kl) * std::tanh(kl * 100.0f));
                            const double ph = w0 * std::fmod(std::floor(w / w0) * float(std::fmod(t, double(kLoopSeconds))), kLoopSeconds);
                            const double hr = s.x * std::cos(ph) - s.y * std::sin(ph) + s.z * std::cos(ph) + s.w * std::sin(ph);
                            const double hi = s.x * std::sin(ph) + s.y * std::cos(ph) - s.z * std::sin(ph) + s.w * std::cos(ph);
                            const glm::dvec2 at = pm - 0.5 * double(g.length[c]) / n; // texel X at (X + 0.5) / n of the tile
                            const double a = double(k.x) * at.x + double(k.y) * at.y;
                            h += g.amplitude[c] * (hr * std::cos(a) - hi * std::sin(a));
                        }
                const float drawn = float(h * kMetre) - 0.5f * kMetre * std::sqrt(g.sigma[0] * g.sigma[0] + g.sigma[1] * g.sigma[1]);
                const glm::vec2 p = glm::vec2(pm * double(kMetre));
                worst = std::max(worst, std::abs(drawn - all.height(p, t)));
                worstStrong = std::max(worstStrong, std::abs(drawn - strong.height(p, t)));
            }
        std::fprintf(stderr, "ocean grid %d: swell RMS %.1f units, %zu modes leave %.2f units RMS; worst error all modes %.3f, %zu modes %.2f\n",
                     n, strong.rms(), strong.modes(), strong.residualRms(), worst, strong.modes(), worstStrong);
        EXPECT_LT(worst, 0.5f) << n << ": the CPU sum is the GPU transform";
        EXPECT_LT(strong.residualRms(), 0.1f * strong.rms()) << n << ": the strong modes hold the height";
        EXPECT_GT(strong.rms(), 50.0f) << n;
    }
}

// The GL flavours of the Enhanced water surface: every include expanded, the tessellated vertex stage selected by its
// define right after #version, and the vertex / fragment stages compile both ways (Vulkan builds them with glslc;
// the tessellation stages compile on the GL driver at startup, falling back to per-vertex displacement).
TEST(WaterSurface, GlSourcesExpandAndCompile) {
    const std::string tessVs = render::waterVertexSource(false, true), plainVs = render::waterVertexSource(false);
    for (const std::string& s : {tessVs, plainVs, render::waterTessControlSource(false), render::waterTessEvalSource(false),
                                 render::waterFragmentSource(false)})
        EXPECT_EQ(s.find("\n#include"), std::string::npos);
    EXPECT_EQ(tessVs.find("#define OU_TESS 1\n"), tessVs.find('\n') + 1);
    EXPECT_EQ(plainVs.find("OU_TESS 1"), std::string::npos);
    EXPECT_NE(render::waterTessControlSource(false).find("layout(vertices = 3) out;"), std::string::npos);
    std::string err;
    EXPECT_FALSE(render::sm3::compileSpirv(tessVs, false, false, err).empty()) << err;
    EXPECT_FALSE(render::sm3::compileSpirv(plainVs, false, false, err).empty()) << err;
}

// The dispersion relation: deep water omega^2 = g k (phase speed sqrt(g / k): a 10 m swell at 3.95 m/s, a 10 cm ripple
// at 0.43 m/s plus capillarity), shallow water omega = k sqrt(g h) (the depth bounds the speed), the minimum phase
// speed ~0.23 m/s at the 1.7 cm capillary-gravity crossover; damping grows with k.
TEST(WaterField, DispersionAndDamping) {
    using namespace ou::render::waterfield;
    constexpr float kPi = 3.14159265f;
    const auto speed = [](float lambda, float depth) { const float k = 2 * kPi / lambda; return omega(k, depth) / k; };
    EXPECT_NEAR(speed(10.0f, 100.0f), std::sqrt(9.81f * 10.0f / (2 * kPi)), 0.01f);
    EXPECT_NEAR(speed(0.1f, 100.0f), 0.40f, 0.04f);
    EXPECT_NEAR(speed(100.0f, 0.5f), std::sqrt(9.81f * 0.5f), 0.02f);
    float slowest = 1e9f;
    for (float l = 0.005f; l < 0.05f; l += 0.0005f) slowest = std::min(slowest, speed(l, 100.0f));
    EXPECT_NEAR(slowest, 0.23f, 0.01f);
    const float cell = 40.0f / kN;
    EXPECT_LT(damping(2 * kPi / 5.0f, cell), damping(2 * kPi / 0.3f, cell));
    EXPECT_GT(damping(kPi / cell, cell), 5.0f) << "grid-scale noise dies within a fraction of a second";
    EXPECT_LT(damping(2 * kPi / 2.0f, cell), 0.35f) << "metre waves live for seconds";
}

// Stepper: the first frame restarts from rest, later ones scroll by the window's whole-cell moves and step by the
// frame's game time (clamped); a new generation or a jump past the window restarts; inactive frames rest; inputs are
// packed in the shader's layouts (contract C3 for the kicks).
TEST(WaterField, StepperScrollsRestartsAndPacks) {
    using namespace ou::render::waterfield;
    Stepper st;
    auto kicks = std::make_unique<KickBuffer>();
    auto frame = std::make_unique<FrameBuffer>();
    auto o = fieldFrame(10.0f, 0.0f, 0.0f);
    const float cell = o.waterField.cellSize;
    const std::vector<ou::render::WaveKick> k = {{100, 200, -45, 64}, {-50, 0, 3, 30}};
    const std::vector<ou::render::WaveBody> b = {{1, 2, 90, 20, 400, 0, 0.5f, 0}};
    std::vector<uint8_t> mask(size_t(kMaskN * kMaskN), 255);
    mask[1] = 0;
    o.waterField.kicks = k, o.waterField.bodies = b, o.waterField.mask = mask, o.waterField.maskSize = kMaskN;
    auto p = st.frame(o, *kicks, *frame);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->cfg.w & kReset, kReset);
    EXPECT_EQ(p->par.x, 0.0f);
    EXPECT_EQ(kicks->count, 2u);
    EXPECT_EQ(kicks->kicks[0].amplitude, -45.0f);
    EXPECT_EQ(std::memcmp(&kicks->kicks[1], &k[1], sizeof k[1]), 0);
    EXPECT_EQ(frame->bodyCount, 1u);
    EXPECT_EQ(frame->bodies[0].wall, 0.5f);
    EXPECT_EQ(frame->mask[0], 0xFFFF00FFu) << "mask bytes packed little-endian, 4 per uint";
    EXPECT_NEAR(p->par.z * kMetre, cell, 1e-4f);

    o.time = 10.02f, o.waterField.originX = 4 * cell, o.waterField.originY = -2 * cell;
    o.waterField.kicks = {};
    p = st.frame(o, *kicks, *frame);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->cfg.w & kReset, 0);
    EXPECT_EQ(p->cfg.y, 4);
    EXPECT_EQ(p->cfg.z, -2);
    EXPECT_NEAR(p->par.x, 0.02f, 1e-5f);
    EXPECT_EQ(kicks->count, 0u);

    o.time = 13.0f; // a hitch: at most 1/15 s
    EXPECT_NEAR(st.frame(o, *kicks, *frame)->par.x, 1.0f / 15.0f, 1e-6f);
    o.time = 12.0f; // time went back (a load): holds
    EXPECT_EQ(st.frame(o, *kicks, *frame)->par.x, 0.0f);
    o.waterField.originX += float(kN) * cell; // past the window
    EXPECT_EQ(st.frame(o, *kicks, *frame)->cfg.w & kReset, kReset);
    o.waterField.generation = 2;
    EXPECT_EQ(st.frame(o, *kicks, *frame)->cfg.w & kReset, kReset);
    o.waterField.active = false;
    EXPECT_FALSE(st.frame(o, *kicks, *frame));
    o.waterField.active = true;
    EXPECT_EQ(st.frame(o, *kicks, *frame)->cfg.w & kReset, kReset) << "waking restarts from rest";
    o.water = ou::render::WaterQuality::Original;
    EXPECT_FALSE(st.frame(o, *kicks, *frame)) << "Original water draws no field";
}
