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

TEST(Texture, CubeFacesAndBadSizes) {
    // 4x4 DXT1 cube, 2 mips per face (8 + 8 bytes), faces stored one after another; face f filled with f.
    std::vector<uint8_t> px;
    for (uint8_t f = 0; f < 6; ++f) px.insert(px.end(), 16, f);
    const auto cube = formats::decodeTexture(section(cdc::SectionType::Texture, 1, pcd(0x31545844, 4, 4, 1, 0x8003, px)));
    ASSERT_TRUE(cube);
    EXPECT_TRUE(cube->cube);
    ASSERT_EQ(cube->mips.size(), 2u);
    ASSERT_EQ(cube->mips[1].data.size(), 6u * 8);
    EXPECT_EQ(cube->mips[1].data[5 * 8], 5); // level 1 of face 5
    std::string why;
    px.pop_back();
    EXPECT_FALSE(formats::decodeTexture(section(cdc::SectionType::Texture, 1, pcd(0x31545844, 4, 4, 1, 0x8003, px)), &why));
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
    const cdc::BigfileSet bf(g);
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\lara.drm"));
    int ok = 0;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Texture) {
            std::string why;
            const bool decoded = formats::decodeTexture(s, &why).has_value();
            EXPECT_TRUE(decoded) << s.id << ": " << why;
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
    const cdc::BigfileSet bf(g);
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
    const cdc::BigfileSet bf(g);
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\streamgroups\\l2_maproom_second_maproom.drm"));
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

// ---- Levels ---------------------------------------------------------------------------
namespace {
struct Ext { uint32_t patch, section, target; };
// Relocation table: internal {patch -> target}, external, and type-5 resource refs {patch, type}.
std::vector<uint8_t> relocs(std::vector<std::pair<uint32_t, uint32_t>> in, std::vector<Ext> ex = {},
                            std::vector<std::pair<uint32_t, uint32_t>> res = {}) {
    std::vector<uint8_t> v;
    put32(v, uint32_t(in.size())); put32(v, uint32_t(ex.size())); put32(v, 0); put32(v, 0); put32(v, uint32_t(res.size()));
    for (auto [p, t] : in) { put32(v, p); put32(v, t); }
    for (auto e : ex) { put32(v, e.section | (e.patch / 4 & 0x3FFFF) << 14); put32(v, (e.patch / 4 >> 18) | e.target << 6); }
    for (auto [p, type] : res) put32(v, p / 4 | type << 25);
    return v;
}
void setf(std::vector<uint8_t>& v, size_t off, float f) { uint32_t u; std::memcpy(&u, &f, 4); set32(v, off, u); }
} // namespace

TEST(Level, ParseUnitIntrosStreamGroupsImfs) {
    // Section 0: Level (+0x74 -> unit data, +0x90 -> cell group data).
    std::vector<uint8_t> level(0x94), unitData(0x30 + 2 * 0x90), cells(0x110);
    const auto levelRel = relocs({}, {{0x74, 1, 0}, {0x90, 2, 0}});
    // Unit data: 2 intros at 0x30 (first list), second list empty.
    set32(unitData, 0x0C, 2);
    for (int k = 0; k < 2; ++k) {
        const size_t b = 0x30 + 0x90 * k;
        setf(unitData, b + 8, 1.5f * k);                    // rotation z
        setf(unitData, b + 0x10, 100.f + k);                // position x
        setf(unitData, b + 0x18, -50.f);                    // position z
        for (int i = 0; i < 3; ++i) setf(unitData, b + 0x40 + 4 * i, 2.f);
        unitData[b + 0x50] = uint8_t(k ? 0xFA : 3), unitData[b + 0x51] = uint8_t(k ? 0xFF : 0); // 3, marker
        set32(unitData, b + 0x58, 5000 + k);
        unitData[b + 0x71] = k ? 0x10 : 0;
    }
    const auto unitRel = relocs({{0x10, 0x30}});
    // Cell group: header @0x20 (2 stream groups, 1 IMF), stream groups @0x40, IMF @0x80, strings @0xF0.
    set32(cells, 0x20 + 0x0C, 2);
    set32(cells, 0x20 + 0x1C, 1);
    for (int i = 0; i < 4; ++i) setf(cells, 0x80 + 4 * (5 * i), 1.f); // identity
    setf(cells, 0x80 + 0x30, 10.f), setf(cells, 0x80 + 0x34, 20.f), setf(cells, 0x80 + 0x38, 30.f);
    set32(cells, 0x80 + 0x48, 77);
    std::memcpy(&cells[0xF0], "unit_group_a", 13);
    std::memcpy(&cells[0x100], "bush", 5);
    const auto cellRel = relocs({{0x00, 0x20}, {0x08, 0x40}, {0x18, 0x80}, {0x40 + 20 + 12, 0xF0}, {0x80 + 0x44, 0x100}});

    cdc::Drm d;
    d.sections = {section(cdc::SectionType::General, 0, level, levelRel), section(cdc::SectionType::DtpData, 9, unitData, unitRel),
                  section(cdc::SectionType::General, 0, cells, cellRel)};
    const auto u = formats::parseUnit(d);
    ASSERT_EQ(u.intros.size(), 2u);
    EXPECT_EQ(u.intros[0].objectId, 3u);
    EXPECT_EQ(u.intros[1].objectId, formats::kMarkerObjectId);
    EXPECT_EQ(u.intros[1].uniqueId, 5001);
    EXPECT_FLOAT_EQ(u.intros[1].position[0], 101.f);
    EXPECT_FLOAT_EQ(u.intros[1].position[2], -50.f);
    EXPECT_FLOAT_EQ(u.intros[1].rotation[2], 1.5f);
    EXPECT_FALSE(u.intros[0].scaled());
    EXPECT_TRUE(u.intros[1].scaled());
    EXPECT_EQ(u.streamGroups, (std::vector<std::string>{"unit_group_a"})); // entry 0 has no stream file
    ASSERT_EQ(u.imfs.size(), 1u);
    EXPECT_EQ(u.imfs[0].dtpId, 77u);
    EXPECT_EQ(u.imfs[0].name, "bush");
    EXPECT_FLOAT_EQ(u.imfs[0].matrix[0], 1.f);
    EXPECT_FLOAT_EQ(u.imfs[0].matrix[13], 20.f); // D3D row 3 = column-major translation

    // Missing Level pointers and truncated intro arrays are errors, not garbage.
    d.sections[0] = section(cdc::SectionType::General, 0, level);
    EXPECT_THROW(formats::parseUnit(d), cdc::FormatError);
    d.sections[0] = section(cdc::SectionType::General, 0, level, levelRel);
    set32(unitData, 0x0C, 3);
    EXPECT_THROW(formats::parseUnit(d), cdc::FormatError);
}

TEST(Level, ObjectRenderModelsFollowsObjectBase) {
    constexpr uint32_t kDtp = 7, kRender = 12;
    std::vector<uint8_t> object(0x8C), root(0xB8), model(0x68), modelNoRender(0x68);
    set32(object, 0, 10);
    root[0xA0] = 2;
    set32(root, 0xB0, 11), set32(root, 0xB4, 12);
    set32(model, 0x64, 500);
    cdc::Drm d;
    const auto objRel = relocs({}, {}, {{0, kDtp}}), rootRel = relocs({{0xA4, 0xB0}}, {}, {{0xB0, kDtp}, {0xB4, kDtp}}),
               modelRel = relocs({}, {}, {{0x64, kRender}});
    d.sections = {section(cdc::SectionType::Object, 572, object, objRel), section(cdc::SectionType::DtpData, 10, root, rootRel),
                  section(cdc::SectionType::DtpData, 11, model, modelRel), section(cdc::SectionType::DtpData, 12, modelNoRender)};
    EXPECT_EQ(formats::objectRenderModels(d), (std::vector<uint32_t>{500, 0}));
    set32(root, 0, 0x10801004); // triggervolume's oflags
    d.sections[1].data = root;
    EXPECT_EQ(formats::objectFlags(d), 0x10801004u);
    EXPECT_TRUE(formats::objectFlags(d) & formats::kObjectHiddenAtBirth);
    d.sections.erase(d.sections.begin());
    EXPECT_TRUE(formats::objectRenderModels(d).empty()); // no object section
    EXPECT_EQ(formats::objectFlags(d), 0u);
}

TEST(Model, MaterialStateFromBlob) {
    // blob +0x38 blend mode, +0x40 flags, +0x4C alpha threshold (PCStateManager_SetBlendMode 0x527A80).
    auto state = [](uint32_t mode, uint32_t flags, uint8_t ref) {
        std::vector<uint8_t> blob(0x60);
        set32(blob, 0x38, mode), set32(blob, 0x40, flags);
        blob[0x4C] = ref;
        return formats::materialState(section(cdc::SectionType::Material, 1, blob));
    };
    auto s = state(0, 0x1024, 127);
    EXPECT_EQ(s.blend, scene::Blend::Opaque);
    EXPECT_EQ(s.alphaRef, 0);       // mode 0: no alpha test
    EXPECT_FALSE(s.doubleSided);
    s = state(1, 0x10a5, 127);      // alpha-tested foliage (ballcourt 13395)
    EXPECT_EQ(s.blend, scene::Blend::Opaque);
    EXPECT_EQ(s.alphaRef, 127);
    EXPECT_TRUE(s.doubleSided);     // flags 0x80
    s = state(2, 0x10bc, 127);      // blended leaves (ballcourt 13393)
    EXPECT_EQ(s.blend, scene::Blend::Alpha);
    EXPECT_EQ(s.alphaRef, 1);
    EXPECT_EQ(state(4, 0, 0).blend, scene::Blend::Alpha);
    EXPECT_EQ(state(5, 0, 0).blend, scene::Blend::Additive);
    EXPECT_EQ(state(8, 0, 0).blend, scene::Blend::Average);
    EXPECT_EQ(state(8, 0, 0).alphaRef, 0);
    EXPECT_TRUE(state(0, 0x800, 0).doubleSided); // "render twice"
    EXPECT_EQ(formats::materialState(section(cdc::SectionType::Material, 1, std::vector<uint8_t>(8))).blend,
              scene::Blend::Opaque); // too short: defaults
}

TEST(Level, IntroMatrixMatchesExeOrder) {
    formats::Intro in{};
    in.rotation[0] = float(M_PI / 2), in.rotation[2] = float(M_PI / 2);
    in.position[0] = 1, in.position[1] = 2, in.position[2] = 3;
    in.scale[0] = in.scale[1] = in.scale[2] = 3;
    float m[16];
    formats::introMatrix(in, m);
    // Rz first (+X -> +Y), then Rx (+Y -> +Z): column 0 = +Z. Scale ignored without flag 0x10.
    EXPECT_NEAR(m[0], 0, 1e-6); EXPECT_NEAR(m[1], 0, 1e-6); EXPECT_NEAR(m[2], 1, 1e-6);
    EXPECT_FLOAT_EQ(m[12], 1); EXPECT_FLOAT_EQ(m[14], 3); EXPECT_FLOAT_EQ(m[15], 1);
    in.flags[1] = 0x10;
    formats::introMatrix(in, m);
    EXPECT_NEAR(m[2], 3, 1e-5);
}

TEST(Level, ObjectList) {
    const auto l = formats::parseObjectList("3\r\n1,generalbank\r\n8,Lara\r\n567,ca_skydome_a\r\n");
    ASSERT_EQ(l.size(), 3u);
    EXPECT_EQ(l.at(8), "lara");
    EXPECT_EQ(l.at(567), "ca_skydome_a");
}

TEST(RealData, PatchOverridesBigfile) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    ASSERT_TRUE(bf.hasPatch());
    EXPECT_EQ(bf.read("pc-w\\l2_puzzleroom.drm")->size(), 13751556u); // patch.000 copy
}

TEST(RealData, LevelL4PoolInstancesOnTerrain) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto u = formats::parseUnit(cdc::parseDrm(*bf.read("pc-w\\l4_pool.drm")));
    EXPECT_EQ(u.intros.size(), 174u);
    EXPECT_TRUE(u.streamGroups.empty());
    EXPECT_TRUE(u.imfs.empty());

    const auto sc = formats::loadLevelScene(bf, "pc-w\\l4_pool.drm");
    checkMeshes(sc);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    size_t placed = 0;
    for (const auto& m : sc.meshes) {
        if (m.origin.find("terrain") != std::string::npos)
            for (const auto& v : m.vertices)
                for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v.position[k]), hi[k] = std::max(hi[k], v.position[k]);
        placed += m.origin.find("@intro") != std::string::npos;
    }
    EXPECT_GT(placed, 50u);
    // Observed: 161/173 object intros lie inside the terrain box; the rest are cinematic props
    // parked at (0, 0, -11542) and one trigger volume. Markers (object -6) are not counted.
    size_t inside = 0, objects = 0;
    for (const auto& in : u.intros) {
        bool in3 = true;
        for (int k = 0; k < 3; ++k) {
            ASSERT_TRUE(std::isfinite(in.position[k]) && std::isfinite(in.rotation[k])) << in.uniqueId;
            in3 = in3 && in.position[k] >= lo[k] - 500.f && in.position[k] <= hi[k] + 500.f;
        }
        if (in.objectId == formats::kMarkerObjectId) continue;
        ++objects;
        inside += in3;
    }
    EXPECT_GE(inside, objects * 9 / 10) << inside << "/" << objects;
}

TEST(RealData, LevelBallcourtStreamGroupsAndImfs) {
    const char* g = gameDir();
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(g);
    const auto u = formats::parseUnit(cdc::parseDrm(*bf.read("pc-w\\l4_ballcourt.drm")));
    EXPECT_EQ(u.intros.size(), 483u);
    EXPECT_EQ(u.streamGroups.size(), 8u); // 10 groups, 2 without a stream DRM
    EXPECT_EQ(u.imfs.size(), 3293u);
    for (const auto& sg : u.streamGroups) EXPECT_TRUE(bf.read("pc-w\\streamgroups\\" + sg + ".drm")) << sg;
    for (const auto& p : u.imfs) {
        for (float f : p.matrix) ASSERT_TRUE(std::isfinite(f));
        EXPECT_FLOAT_EQ(p.matrix[15], 1.f);
    }
    // Editor objects hidden at birth (oflags 0x800000) are not placed; foliage carries blend state.
    const auto sc = formats::loadLevelScene(bf, "pc-w\\l4_ballcourt.drm");
    size_t blended = 0, alphaTested = 0, twoSided = 0;
    for (const auto& m : sc.meshes) {
        EXPECT_EQ(m.origin.find("triggervolume.drm"), std::string::npos) << m.origin;
        EXPECT_EQ(m.origin.find("cameraanchor_object.drm"), std::string::npos) << m.origin;
        for (const auto& s : m.submeshes) {
            blended += s.blend == scene::Blend::Alpha;
            alphaTested += s.alphaRef > 1;
            twoSided += s.doubleSided;
        }
    }
    EXPECT_GT(blended, 0u);
    EXPECT_GT(alphaTested, 0u);
    EXPECT_GT(twoSided, 0u);
}
