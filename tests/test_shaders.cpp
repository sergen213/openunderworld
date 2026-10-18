// D3D9 SM3 parser/disassembler (formats/Shader), TRU ShaderLib container, SM3 -> GLSL translator (render/ShaderTranslate).
#include "cdc/Cdc.hpp"
#include "formats/Shader.hpp"
#include "formats/Formats.hpp"
#include "render/OriginalShading.hpp"
#include "render/ShaderTranslate.hpp"

#include <gtest/gtest.h>

#include <atomic>
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

// ps_3_0: texld, texkill, cmp on vFace, MaterialParams c100 tint, fog lerp.
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
    u[u.size() - 5] = ins(Mul, 9); // last instruction's length runs past the end
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
                EXPECT_NE(g.source.find("gl_FrontFacing"), std::string::npos);
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
    size_t withPrograms = 0, lightMapped = 0;
    for (const auto& m : bed.materials) withPrograms += m.vertexShader && m.pixelShader;
    for (const auto& m : bed.meshes)
        for (const auto& s : m.submeshes) lightMapped += s.instanceTextures[0] != 0;
    EXPECT_GT(withPrograms, bed.materials.size() * 9 / 10);
    EXPECT_GT(lightMapped, 100u);
    const auto prepared = render::original::prepare(bed, true);
    // Programs needing irradiance-volume lighting stay generic (SHADERS.md); the rest translate.
    EXPECT_EQ(prepared.translated + prepared.fallback, bed.materials.size());
    EXPECT_GT(prepared.translated, withPrograms / 2);
    const auto bc = formats::loadLevelScene(bf, "pc-w\\l4_ballcourt.drm");
    ASSERT_TRUE(bc.environment.sun);
    EXPECT_NEAR(bc.environment.sunDirection[0], -0.54406f, 1e-3);
    EXPECT_NEAR(bc.environment.sunDirection[2], -0.76227f, 1e-3);
    EXPECT_NEAR(bc.environment.sunColor[2], 1.2f, 1e-3);
}
