#include "anim/Anim.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>

using namespace ou;

namespace {
void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }
void putf(std::vector<uint8_t>& v, float f) { uint32_t u; std::memcpy(&u, &f, 4); put32(v, u); }
void set32(std::vector<uint8_t>& v, size_t o, uint32_t x) { for (int i = 0; i < 4; ++i) v[o + i] = uint8_t(x >> (8 * i)); }

// 3 keys, 100 units/key, 2 segments. Segment 0: rot X tabulated, rot Z constant, trans Z piece-wise.
std::vector<uint8_t> syntheticClip() {
    std::vector<uint8_t> d(0x50, 0);
    d[0x30] = 7;                      // animID
    d[0x32] = 3;                      // keyCount
    d[0x34] = 100;                    // timePerKey
    d[0x36] = 2;                      // segments
    d[0x37] = 1;                      // sections
    set32(d, 0x44, 0x50 - 0x44);      // channel data offset (self-relative)
    d[0x48] = 0x05;                   // rot + trans streams present
    d[0x49] = 0x05;                   // rot stream: seg0 = X|Z, seg1 = none
    d[0x4A] = 0x04;                   // trans stream: seg0 = Z
    put16(d, 0); put16(d, 0); putf(d, 0.0f); putf(d, 0.5f); putf(d, 1.0f); // rot X: mode 0
    put16(d, 1); put16(d, 0); putf(d, 0.25f);                              // rot Z: mode 1
    put16(d, 2); put16(d, 3);                                              // trans Z: mode 2, 3 floats
    d.push_back(1); d.push_back(1); d.push_back(0); d.push_back(0);       // lengths, padded to 4
    putf(d, 10.0f); putf(d, 5.0f); putf(d, -3.0f);                         // base, deltas
    set32(d, 0x3C, uint32_t(d.size() - 0x3C));
    return d;
}

scene::Skeleton chain() {
    scene::Skeleton s;
    s.bones = {{-1, {0, 0, 0}}, {0, {10, 0, 0}}};
    return s;
}

void expectNear(glm::vec3 a, glm::vec3 b, float eps = 1e-4f) {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}
} // namespace

TEST(Anim, DecodesAllChannelModes) {
    const auto d = syntheticClip();
    const auto c = anim::decodeClip(d);
    EXPECT_EQ(c.id, 7u);
    EXPECT_EQ(c.keyCount, 3);
    EXPECT_EQ(c.timePerKey, 100);
    ASSERT_EQ(c.tracks.size(), 2u);
    EXPECT_EQ(c.tracks[0][anim::RotX], (std::vector<float>{0.0f, 0.5f, 1.0f}));
    EXPECT_EQ(c.tracks[0][anim::RotZ], (std::vector<float>{0.25f, 0.25f, 0.25f}));
    EXPECT_EQ(c.tracks[0][anim::TransZ], (std::vector<float>{10.0f, 15.0f, 12.0f}));
    EXPECT_TRUE(c.tracks[0][anim::RotY].empty());
    EXPECT_TRUE(c.tracks[1][anim::RotX].empty());
    EXPECT_DOUBLE_EQ(c.durationSeconds(), 0.1);
}

TEST(Anim, RejectsTruncatedAndInconsistent) {
    auto d = syntheticClip();
    d.resize(d.size() - 4);
    EXPECT_THROW(anim::decodeClip(d), cdc::FormatError);
    d = syntheticClip();
    set32(d, 0x3C, 0);
    EXPECT_THROW(anim::decodeClip(d), cdc::FormatError);
}

TEST(Anim, SamplingInterpolatesBetweenPreviousAndCurrentKey) {
    const auto c = anim::decodeClip(syntheticClip());
    const auto sk = chain();
    // t = 1.5 keys -> k = 1, lerp(key0, key1, 0.5).
    auto p = anim::sample(c, sk, 150.0 / anim::kTimeUnitsPerSecond, false);
    EXPECT_NEAR(p[0].rot.x, 0.25f, 1e-6f);
    EXPECT_NEAR(p[0].rot.z, 0.25f, 1e-6f);
    EXPECT_NEAR(p[0].trans.z, 12.5f, 1e-5f);
    EXPECT_NEAR(p[0].trans.x, 0.0f, 1e-6f);  // untracked: bind pivot
    expectNear(p[1].trans, {10, 0, 0});      // untracked segment keeps its pivot
    // t = 0: looping wraps key -1 to the last key; one-shot clamps to key 0.
    p = anim::sample(c, sk, 0.0, true);
    EXPECT_NEAR(p[0].rot.x, 1.0f, 1e-6f);
    p = anim::sample(c, sk, 0.0, false);
    EXPECT_NEAR(p[0].rot.x, 0.0f, 1e-6f);
    // Past the end: one-shot holds the last key; looping wraps.
    p = anim::sample(c, sk, 1.0, false);
    EXPECT_NEAR(p[0].trans.z, 12.0f, 1e-5f);
    p = anim::sample(c, sk, 0.1 + 150.0 / anim::kTimeUnitsPerSecond, true);
    EXPECT_NEAR(p[0].trans.z, 12.5f, 1e-3f);
}

TEST(Anim, RotationVectorIsAxisAngle) {
    const glm::mat3 r = anim::rotationFromVector({0, 0, float(M_PI / 2)});
    expectNear(r * glm::vec3(1, 0, 0), {0, 1, 0});
    expectNear(r * glm::vec3(0, 1, 0), {-1, 0, 0});
    const glm::mat3 r2 = anim::rotationFromVector({float(M_PI), 0, 0});
    expectNear(r2 * glm::vec3(0, 1, 0), {0, -1, 0});
    expectNear(anim::rotationFromVector({0, 0, 0}) * glm::vec3(1, 2, 3), {1, 2, 3});
}

TEST(Anim, ShortestWayRotationInterpolation) {
    // Rot keys A = (0.9 turn, 0, 0.25), B = (-0.05 turn, 0, 0.25): they differ by more than half a
    // turn, so A is rewound by one turn along its own axis before the lerp.
    auto d = syntheticClip();
    const float a = 0.9f * 6.2831855f, b = -0.05f * 6.2831855f;
    std::memcpy(&d[0x50 + 4], &a, 4);
    std::memcpy(&d[0x50 + 8], &b, 4);
    const auto c = anim::decodeClip(d);
    const auto p = anim::sample(c, chain(), 150.0 / anim::kTimeUnitsPerSecond, false);
    const glm::vec3 A(a, 0, 0.25f), B(b, 0, 0.25f);
    const glm::vec3 rewound = A - glm::normalize(A) * 6.2831855f;
    expectNear(p[0].rot, (rewound + B) * 0.5f);
    EXPECT_LT(std::abs(p[0].rot.x), 0.5f); // went the short way, not through +0.4 turn
}

TEST(Anim, WorldPoseAndSkinPalette) {
    const auto sk = chain();
    auto local = anim::bindPose(sk);
    // Bind pose: palette is identity, skinning is a no-op.
    auto pal = anim::skinPalette(sk, anim::worldMatrices(sk, local));
    for (const auto& m : pal) EXPECT_EQ(m, glm::mat4(1.0f));
    // Root rotated 90 degrees about Z: the child joint moves from (10,0,0) to (0,10,0).
    local[0].rot = {0, 0, float(M_PI / 2)};
    const auto world = anim::worldMatrices(sk, local);
    expectNear(glm::vec3(world[1][3]), {0, 10, 0});
    pal = anim::skinPalette(sk, world);
    scene::MeshData m;
    m.vertices.push_back({{12, 0, 0}, {1, 0, 0}, {0, 0}});
    m.skin.push_back({{1, 0, 0, 0}, {255, 0, 0, 0}});
    m.vertices.push_back({{5, 0, 0}, {1, 0, 0}, {0, 0}});
    m.skin.push_back({{0, 1, 0, 0}, {128, 127, 0, 0}});
    std::vector<scene::Vertex> out;
    anim::skinVertices(m, pal, out);
    expectNear({out[0].position[0], out[0].position[1], out[0].position[2]}, {0, 12, 0});
    expectNear({out[0].normal[0], out[0].normal[1], out[0].normal[2]}, {0, 1, 0});
    expectNear({out[1].position[0], out[1].position[1], out[1].position[2]}, {0, 5, 0}); // both bones agree
}

// ---- Real data (skipped without OU_GAME_DIR) -------------------------------------------
namespace {
struct Lara {
    cdc::Drm drm;
    std::vector<scene::Skeleton> skeletons;
    std::vector<std::string> warnings;
};
const Lara* lara() {
    static std::optional<Lara> l;
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) return nullptr;
    if (!l) {
        const cdc::BigfileSet big(g);
        auto bytes = big.read("pc-w\\lara.drm");
        if (!bytes) return nullptr;
        l.emplace();
        l->drm = cdc::parseDrm(std::move(*bytes));
        l->skeletons = anim::decodeSkeletons(l->drm, l->warnings);
    }
    return &*l;
}
} // namespace

TEST(AnimReal, LaraSkeletonHierarchy) {
    const Lara* l = lara();
    if (!l) GTEST_SKIP() << "OU_GAME_DIR not set";
    EXPECT_TRUE(l->warnings.empty()) << l->warnings.front();
    const scene::Skeleton* sk = nullptr;
    for (const auto& s : l->skeletons)
        if (s.renderModelId == 2366) sk = &s;
    ASSERT_NE(sk, nullptr);
    ASSERT_EQ(sk->bones.size(), 116u + 37u); // 116 segments + 3 hair chains (7 + 15 + 15)
    const auto bind = anim::bindPositions(*sk);
    EXPECT_NEAR(bind[1].z, 325.72f, 0.01f); // pelvis height
    // Left/right symmetry (mirror in X): leg chains 4..8 vs 9..13, arms 52..55 vs 77..80.
    for (auto [a, b] : {std::pair{4, 9}, {5, 10}, {6, 11}, {7, 12}, {8, 13}, {52, 77}, {53, 78}, {54, 79}, {55, 80}}) {
        EXPECT_NEAR(bind[size_t(a)].x, -bind[size_t(b)].x, 0.05f) << a << "/" << b;
        EXPECT_NEAR(bind[size_t(a)].y, bind[size_t(b)].y, 0.05f) << a << "/" << b;
        EXPECT_NEAR(bind[size_t(a)].z, bind[size_t(b)].z, 0.05f) << a << "/" << b;
    }
    // Feet are near the ground, the head near the top of the 511-unit-tall mesh.
    EXPECT_LT(bind[7].z, 10.0f);
    EXPECT_GT(bind[17].z, 420.0f);
}

TEST(AnimReal, AllLaraClipsDecodeAndKeepBoneLengths) {
    const Lara* l = lara();
    if (!l) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto& sk = l->skeletons.front();
    const auto bind = anim::bindPositions(sk);
    size_t clips = 0;
    for (const auto& s : l->drm.sections) {
        if (s.type != cdc::SectionType::Animation) continue;
        anim::Clip c;
        ASSERT_NO_THROW(c = anim::decodeClip(s.data)) << "section " << s.id;
        EXPECT_EQ(c.id, s.id);
        ++clips;
        if (s.id % 50) continue; // pose checks on a subset
        const auto world = anim::worldMatrices(sk, anim::sample(c, sk, c.durationSeconds() * 0.5, true));
        auto moves = [&](size_t i, bool scaleOnly) { // has scale (or translation) channels
            if (i >= c.tracks.size()) return false;
            for (int ch = anim::ScaleX; ch <= (scaleOnly ? anim::ScaleZ : anim::TransZ); ++ch)
                if (!c.tracks[i][size_t(ch)].empty()) return true;
            return false;
        };
        for (size_t i = 2; i < 113; ++i) { // skip root/pelvis (root motion) and hair
            const int p = sk.bones[i].parent;
            if (moves(i, false) || moves(size_t(p), true)) continue; // own trans/scale or parent scale
            const float bindLen = glm::length(bind[i] - bind[size_t(p)]);
            const float posedLen = glm::length(glm::vec3(world[i][3]) - glm::vec3(world[size_t(p)][3]));
            EXPECT_NEAR(posedLen, bindLen, 0.01f + bindLen * 1e-4f) << "clip " << s.id << " bone " << i;
        }
    }
    EXPECT_EQ(clips, 1744u);
}

TEST(AnimReal, LaraAnimListAndSkinWeights) {
    const Lara* l = lara();
    if (!l) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto list = anim::animList(l->drm);
    ASSERT_EQ(list.size(), 1818u);
    EXPECT_EQ(list[0].animId, 2u);
    bool walk = false;
    for (const auto& e : list) walk |= e.animId == 723 && e.name == "walk_in_place";
    EXPECT_TRUE(walk);

    std::vector<std::string> w;
    const auto meshes = formats::decodeModels(l->drm, w);
    size_t skinned = 0;
    for (const auto& m : meshes) {
        if (m.skin.empty()) continue;
        ++skinned;
        ASSERT_EQ(m.skin.size(), m.vertices.size());
        for (const auto& s : m.skin) {
            EXPECT_EQ(s.weight[0] + s.weight[1] + s.weight[2] + s.weight[3], 255);
            for (int k = 0; k < 4; ++k)
                if (s.weight[k]) { EXPECT_LT(s.bone[k], 153); }
        }
    }
    EXPECT_EQ(skinned, 7u);
}

TEST(AnimReal, LaraSceneLinksSkeletonAndBindPoseIsIdentity) {
    const char* g = std::getenv("OU_GAME_DIR");
    if (!g) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet big(g);
    const auto sc = formats::loadDrmScene(big, "pc-w\\lara.drm");
    size_t linked = 0;
    for (const auto& m : sc.meshes) {
        if (m.skeleton < 0) continue;
        ++linked;
        const auto& sk = sc.skeletons.at(size_t(m.skeleton));
        EXPECT_EQ(sk.renderModelId, 2366u);
        // The full skinning path in the bind pose must reproduce the stored T-pose vertices.
        const auto palette = anim::skinPalette(sk, anim::worldMatrices(sk, anim::bindPose(sk)));
        std::vector<scene::Vertex> out;
        anim::skinVertices(m, palette, out);
        float worst = 0.0f;
        for (size_t i = 0; i < out.size(); ++i)
            for (int k = 0; k < 3; ++k) worst = std::max(worst, std::abs(out[i].position[k] - m.vertices[i].position[k]));
        EXPECT_LT(worst, 1e-3f) << m.origin;
    }
    EXPECT_EQ(linked, 7u);
}
