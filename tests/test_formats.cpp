#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

using namespace ou;

namespace {
void put32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i)));
}
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void putf(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }
void set32(std::vector<uint8_t>& v, size_t off, uint32_t x) { for (int i = 0; i < 4; ++i) v[off + i] = uint8_t(x >> (8 * i)); }

std::vector<uint8_t> pcd(uint32_t format, uint16_t w, uint16_t h, uint8_t mips, uint16_t flags, std::vector<uint8_t> pixels) {
    std::vector<uint8_t> v;
    put32(v, 0x39444350); put32(v, format); put32(v, uint32_t(pixels.size())); put32(v, 0);
    put16(v, w); put16(v, h); v.push_back(format == 0x15 ? 32 : 0); v.push_back(mips); put16(v, flags);
    v.insert(v.end(), pixels.begin(), pixels.end());
    return v;
}

cdc::Section section(cdc::SectionType t, uint32_t id, const std::vector<uint8_t>& data, const std::vector<uint8_t>& relocs = {}) {
    cdc::Section s{};
    s.type = t;
    s.id = id;
    s.data = data;
    s.relocations = relocs;
    return s;
}
} // namespace

TEST(Texture, Bc1MipChainAndDecode) {
    // 4x4 BC1: c0 = red (0xF800), c1 = blue (0x001F); pixel i uses index i%4. Levels 4x4,2x2,1x1.
    std::vector<uint8_t> block = {0x00, 0xF8, 0x1F, 0x00, 0xE4, 0xE4, 0xE4, 0xE4};
    std::vector<uint8_t> px;
    for (int i = 0; i < 3; ++i) px.insert(px.end(), block.begin(), block.end());
    const auto data = pcd(0x31545844, 4, 4, 2, 3, px);
    const auto t = formats::decodeTexture(section(cdc::SectionType::Texture, 7, data));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->id, 7u);
    EXPECT_EQ(t->format, scene::PixelFormat::BC1);
    ASSERT_EQ(t->mips.size(), 3u);
    EXPECT_EQ(t->mips[1].width, 2u);
    EXPECT_EQ(t->mips[2].data.size(), 8u);
    const auto rgba = formats::toRgba8(t->format, 4, 4, t->mips[0].data);
    ASSERT_EQ(rgba.size(), 64u);
    const uint8_t expect[4][4] = {{255, 0, 0, 255}, {0, 0, 255, 255}, {170, 0, 85, 255}, {85, 0, 170, 255}};
    for (int i = 0; i < 4; ++i) EXPECT_EQ(0, std::memcmp(&rgba[4 * i], expect[i], 4)) << i;
}

TEST(Texture, Bgra8SwizzleAndBc3Alpha) {
    const auto t = formats::decodeTexture(section(cdc::SectionType::Texture, 1, pcd(0x15, 1, 1, 0, 0, {10, 20, 30, 40})));
    ASSERT_TRUE(t);
    EXPECT_EQ(t->format, scene::PixelFormat::BGRA8);
    EXPECT_EQ(formats::toRgba8(t->format, 1, 1, t->mips[0].data), (std::vector<uint8_t>{30, 20, 10, 40}));
    // BC3 alpha: a0=255, a1=0, all indices 1 -> alpha 0; colour all c0 white.
    std::vector<uint8_t> b = {255, 0, 0x49, 0x92, 0x24, 0x49, 0x92, 0x24, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
    const auto rgba = formats::toRgba8(scene::PixelFormat::BC3, 4, 4, b);
    EXPECT_EQ(rgba[0], 255);
    EXPECT_EQ(rgba[3], 0);
}

TEST(Texture, RejectsCubeAndBadSizes) {
    std::vector<uint8_t> px(6 * 8, 0);
    std::string why;
    EXPECT_FALSE(formats::decodeTexture(section(cdc::SectionType::Texture, 1, pcd(0x31545844, 4, 4, 0, 0x8003, px)), &why));
    EXPECT_NE(why.find("cube"), std::string::npos);
    EXPECT_FALSE(formats::decodeTexture(section(cdc::SectionType::Texture, 1, pcd(0x31545844, 4, 4, 0, 0, std::vector<uint8_t>(9)))));
    EXPECT_FALSE(formats::decodeTexture(section(cdc::SectionType::Texture, 1, {1, 2, 3})));
}

// Synthetic RenderResource type 26 with the layout documented in MESHES.md:
// one batch of 3 vertices {float3 pos, D3DCOLOR normal, SHORT2 uv}, one primgroup with
// material 0 -> material section 50 -> texture 9 (category 1).
TEST(Model, SyntheticMesh) {
    std::vector<uint8_t> d(16, 0); // resource header {Mesh*, tex*, mat*, ?}
    const size_t M = d.size();
    d.resize(M + 0x80, 0);
    set32(d, M, 0x6873654D);
    set32(d, M + 0x0C, 3);            // numIndices
    const size_t decl = d.size();
    put32(d, 0); put32(d, 0); put16(d, 3); put16(d, 20); put32(d, 0);
    put32(d, 0xD2F7D823); put16(d, 0); put16(d, 2);
    put32(d, 0x36F5E414); put16(d, 12); put16(d, 5);
    put32(d, 0x8317902A); put16(d, 16); put16(d, 0x13);
    const size_t verts = d.size();
    const float p[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    for (auto& q : p) {
        for (float f : q) putf(d, f);
        d.insert(d.end(), {127, 0, 128, 0});   // B,G,R: normal (0,-1,0) after *2-1 -> x=R
        put16(d, 2048); put16(d, 1024);       // uv (1, 0.5)
    }
    const size_t idx = d.size();
    put16(d, 0); put16(d, 1); put16(d, 2); put16(d, 0);
    const size_t batch = d.size();
    d.resize(batch + 0x34, 0);
    set32(d, batch + 0x08, 1);
    set32(d, batch + 0x14, uint32_t(verts - M));
    set32(d, batch + 0x24, uint32_t(decl - M));
    set32(d, batch + 0x28, 3);
    set32(d, batch + 0x30, 1);
    const size_t pg = d.size();
    d.resize(pg + 0x40, 0);
    set32(d, pg + 0x14, 1);
    set32(d, pg + 0x28, 0);
    const size_t mats = d.size();
    put32(d, 1); put32(d, 50);
    set32(d, M + 0x6C, uint32_t(pg - M));
    set32(d, M + 0x70, uint32_t(batch - M));
    set32(d, M + 0x78, uint32_t(idx - M));
    d[M + 0x7C] = 1; d[M + 0x7E] = 1;
    std::vector<uint8_t> rel;
    put32(rel, 2); for (int i = 0; i < 4; ++i) put32(rel, 0);
    put32(rel, 0); put32(rel, uint32_t(M));
    put32(rel, 8); put32(rel, uint32_t(mats));

    // Material: one texture entry at offset 16, referenced by a type-5 resource relocation.
    std::vector<uint8_t> md(32, 0);
    set32(md, 16, 9);
    set32(md, 24, 1); // category 1 = diffuse
    std::vector<uint8_t> mrel;
    put32(mrel, 0); put32(mrel, 0); put32(mrel, 0); put32(mrel, 0); put32(mrel, 1); // table 5
    put32(mrel, (16 / 4) | (5u << 25));

    cdc::Drm drm;
    auto s = section(cdc::SectionType::RenderResource, 1, d, rel);
    s.packed = 26u << 1;
    drm.sections.push_back(s);
    drm.sections.push_back(section(cdc::SectionType::Material, 50, md, mrel));
    std::vector<std::string> w;
    const auto meshes = formats::decodeModels(drm, w, "synthetic");
    ASSERT_EQ(meshes.size(), 1u) << (w.empty() ? "" : w[0]);
    const auto& m = meshes[0];
    ASSERT_EQ(m.vertices.size(), 3u);
    EXPECT_EQ(m.indices, (std::vector<uint32_t>{0, 1, 2}));
    ASSERT_EQ(m.submeshes.size(), 1u);
    EXPECT_EQ(m.submeshes[0].textureId, 9u);
    EXPECT_FLOAT_EQ(m.vertices[1].position[0], 1);
    EXPECT_NEAR(m.vertices[0].normal[1], -1, 1e-3);
    EXPECT_FLOAT_EQ(m.vertices[0].uv[0], 1);
    EXPECT_FLOAT_EQ(m.vertices[0].uv[1], 0.5f);

    // Without texcoords and without material textures the batch is skipped with a warning.
    set32(d, decl + 32, 0x12345678); // texcoord element -> unknown hash (sections view `d`)
    drm.sections[1].relocations = {};
    w.clear();
    EXPECT_TRUE(formats::decodeModels(drm, w, "synthetic").empty());
    ASSERT_EQ(w.size(), 1u);
    EXPECT_NE(w[0].find("skipped"), std::string::npos);
}

// ---- Real data (skipped without OU_GAME_DIR) -------------------------------------------
namespace {
const char* gameDir() {
    const char* g = std::getenv("OU_GAME_DIR");
    return g && std::filesystem::exists(std::filesystem::path(g) / "bigfile.000") ? g : nullptr;
}

void checkMeshes(const scene::SceneData& sc) {
    for (const auto& m : sc.meshes) {
        ASSERT_FALSE(m.indices.empty()) << m.origin;
        for (uint32_t i : m.indices) ASSERT_LT(i, m.vertices.size()) << m.origin;
        for (const auto& v : m.vertices) {
            const float n = std::sqrt(v.normal[0] * v.normal[0] + v.normal[1] * v.normal[1] + v.normal[2] * v.normal[2]);
            ASSERT_NEAR(n, 1.f, 1e-3f) << m.origin;
            ASSERT_TRUE(std::isfinite(v.position[0]) && std::isfinite(v.position[1]) && std::isfinite(v.position[2]));
        }
    }
}
} // namespace

TEST(RealData, LaraTexturesAndModel) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::Bigfile bf(std::filesystem::path(g) / "bigfile.000");
    const auto drm = cdc::parseDrm(bf.read(*bf.find("pc-w\\lara.drm")));
    int ok = 0;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Texture) {
            std::string why;
            const bool decoded = formats::decodeTexture(s, &why).has_value();
            EXPECT_TRUE(decoded || why.find("cube") != std::string::npos) << s.id << ": " << why;
            ok += decoded;
        }
    EXPECT_GT(ok, 150);

    const auto sc = formats::loadDrmScene(bf, "pc-w\\lara.drm");
    ASSERT_EQ(sc.meshes.size(), 7u); // 7 vertex batches in the main Lara model
    checkMeshes(sc);
    float zmax = -1e9f;
    for (const auto& m : sc.meshes) for (const auto& v : m.vertices) zmax = std::max(zmax, v.position[2]);
    EXPECT_NEAR(zmax, 511.0f, 1.0f); // Mesh header boxMax.z = 510.97
    EXPECT_FALSE(sc.textures.empty());
}

TEST(RealData, UnitTerrain) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::Bigfile bf(std::filesystem::path(g) / "bigfile.000");
    const auto sc = formats::loadDrmScene(bf, "pc-w\\l4_pool.drm");
    size_t terrain = 0;
    for (const auto& m : sc.meshes) terrain += m.origin.find("terrain") != std::string::npos;
    EXPECT_GT(terrain, 0u);
    checkMeshes(sc);
    size_t skipped = 0;
    for (const auto& w : sc.warnings) skipped += w.find("skipped (no texcoords") != std::string::npos;
    EXPECT_EQ(skipped, 3u); // sections 13938/13941/13944: level-enclosing boxes, material 0xd78
}

TEST(RealData, Bgra8ChannelOrder) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::Bigfile bf(std::filesystem::path(g) / "bigfile.000");
    const auto drm = cdc::parseDrm(bf.read(*bf.find("pc-w\\streamgroups\\l2_maproom_second_maproom.drm")));
    // Texture 0xcb3 is a 1024x1024 A8R8G8B8 tangent-space normal map: mean must be blue (~127,128,240).
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Texture && s.id == 0xcb3) {
            const auto t = formats::decodeTexture(s);
            ASSERT_TRUE(t);
            ASSERT_EQ(t->format, scene::PixelFormat::BGRA8);
            const auto px = formats::toRgba8(t->format, t->mips[0].width, t->mips[0].height, t->mips[0].data);
            double sum[3] = {};
            for (size_t i = 0; i < px.size(); i += 4) for (int k = 0; k < 3; ++k) sum[k] += px[i + k];
            const double n = double(px.size() / 4);
            EXPECT_NEAR(sum[0] / n, 127, 10);
            EXPECT_NEAR(sum[1] / n, 128, 10);
            EXPECT_GT(sum[2] / n, 220);
            return;
        }
    FAIL() << "texture 0xcb3 not found";
}
