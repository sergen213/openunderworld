#include "collision/Collision.hpp"
#include "formats/Formats.hpp"
#include <glm/common.hpp>
#include <gtest/gtest.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>

using namespace ou;
using collision::CollisionMesh;
using collision::vec3;

namespace {

void addTri(CollisionMesh& m, vec3 a, vec3 b, vec3 c) {
    const auto base = uint32_t(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {a, b, c});
    m.faces.push_back({{base, base + 1, base + 2}});
}

// Floor z = 0 over [-500, 500]^2 split into 2x10x10 triangles, plus a wall x = 200.
CollisionMesh room() {
    CollisionMesh m;
    for (int i = 0; i < 10; ++i)
        for (int j = 0; j < 10; ++j) {
            const float x0 = -500 + 100.f * i, y0 = -500 + 100.f * j, x1 = x0 + 100, y1 = y0 + 100;
            addTri(m, {x0, y0, 0}, {x1, y0, 0}, {x1, y1, 0});
            addTri(m, {x0, y0, 0}, {x1, y1, 0}, {x0, y1, 0});
        }
    addTri(m, {200, -500, 0}, {200, 500, 0}, {200, 500, 400});
    addTri(m, {200, -500, 0}, {200, 500, 400}, {200, -500, 400});
    m.build();
    return m;
}

} // namespace

TEST(Collision, RaycastFloorAndWall) {
    const auto m = room();
    auto h = m.raycast({10, 20, 100}, {0, 0, -5}, 1000);
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->t, 100, 1e-3);
    EXPECT_NEAR(h->point.z, 0, 1e-3);
    EXPECT_NEAR(h->normal.z, 1, 1e-5); // faces the ray origin
    h = m.raycast({10, 20, -100}, {0, 0, 1}, 1000);
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->normal.z, -1, 1e-5); // double-sided, normal flipped toward the origin
    EXPECT_FALSE(m.raycast({10, 20, 100}, {0, 0, -1}, 99));     // out of range
    EXPECT_FALSE(m.raycast({10, 20, 100}, {1, 0, 0}, 50));      // parallel, wall too far
    h = m.raycast({0, 0, 100}, {1, 0, 0}, 1000);
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->point.x, 200, 1e-3);
    EXPECT_NEAR(h->normal.x, -1, 1e-5);
    // Ray exactly on BVH slab planes (axis-aligned through vertices) must still hit.
    h = m.groundProbe({100, 100, 50}, 100);
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->point.z, 0, 1e-3);
}

TEST(Collision, MaskAndIgnoredFaces) {
    CollisionMesh m;
    addTri(m, {-10, -10, 0}, {10, -10, 0}, {0, 10, 0});
    addTri(m, {-10, -10, -5}, {10, -10, -5}, {0, 10, -5});
    addTri(m, {-10, -10, -9}, {10, -10, -9}, {0, 10, -9});
    m.faces[0].flags = 0x40; // skipped by tru.exe queries
    m.faces[1].mask = 2;
    m.build();
    EXPECT_NEAR(m.groundProbe({0, 0, 10}, 100)->point.z, -5, 1e-4);
    EXPECT_NEAR(m.groundProbe({0, 0, 10}, 100, 1)->point.z, -9, 1e-4);
}

TEST(Collision, MatchesBruteForce) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u(-1000, 1000), s(-60, 60);
    CollisionMesh m;
    for (int i = 0; i < 2000; ++i) {
        const vec3 c(u(rng), u(rng), u(rng));
        addTri(m, c + vec3(s(rng), s(rng), s(rng)), c + vec3(s(rng), s(rng), s(rng)), c + vec3(s(rng), s(rng), s(rng)));
    }
    m.build();
    // Brute force: every face as its own single-face mesh.
    int hits = 0;
    for (int r = 0; r < 300; ++r) {
        const vec3 o(u(rng), u(rng), u(rng)), d(u(rng), u(rng), u(rng));
        const vec3 p(u(rng), u(rng), u(rng));
        float bestRay = INFINITY, bestPt = INFINITY;
        for (uint32_t f = 0; f < m.faces.size(); ++f) {
            CollisionMesh t;
            for (uint32_t v : m.faces[f].v) t.vertices.push_back(m.vertices[v]);
            t.faces.push_back({{0, 1, 2}});
            t.build();
            if (auto h = t.raycast(o, d, 5000)) bestRay = std::min(bestRay, h->t);
            if (auto h = t.closestPoint(p, 5000)) bestPt = std::min(bestPt, h->t);
        }
        const auto h = m.raycast(o, d, 5000);
        if (std::isinf(bestRay)) EXPECT_FALSE(h);
        else {
            ++hits;
            ASSERT_TRUE(h);
            EXPECT_NEAR(h->t, bestRay, 1e-2);
        }
        const auto c = m.closestPoint(p, 5000);
        ASSERT_EQ(bool(c), !std::isinf(bestPt));
        if (c) { EXPECT_NEAR(c->t, bestPt, 1e-2); }
    }
    EXPECT_GT(hits, 10);
}

TEST(Collision, SphereAndCapsuleSweeps) {
    const auto m = room();
    // Sphere r=10 at z=50 dropping 100: touches the floor after 40 -> t = 0.4.
    auto h = m.sweepSphere({0, 0, 50}, 10, {0, 0, -100});
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->t, 0.4, 1e-3);
    EXPECT_NEAR(h->normal.z, 1, 1e-3);
    EXPECT_NEAR(h->point.z, 0, 1e-3);
    EXPECT_FALSE(m.sweepSphere({0, 0, 50}, 10, {0, 0, -30}));  // stops short
    EXPECT_FALSE(m.sweepSphere({0, 0, 50}, 10, {100, 0, 0}));  // slides parallel, wall not reached
    h = m.sweepSphere({0, 0, 5}, 10, {50, 0, 0});              // already touching
    ASSERT_TRUE(h);
    EXPECT_EQ(h->t, 0);
    // Upright capsule (z 30..130, r 20) walking +x into the wall at x=200: contact when x+20 = 200.
    h = m.sweepCapsule({0, 0, 30}, {0, 0, 130}, 20, {400, 0, 0});
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->t, 180.f / 400, 1e-3);
    EXPECT_NEAR(h->normal.x, -1, 1e-3);
    EXPECT_NEAR(h->point.x, 200, 1e-2);
    // Capsule passing over the wall top (z 400) misses; its bottom sphere clipping the top edge hits.
    EXPECT_FALSE(m.sweepCapsule({0, 0, 430}, {0, 0, 530}, 20, {400, 0, 0}));
    h = m.sweepCapsule({0, 0, 410}, {0, 0, 510}, 20, {400, 0, 0});
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->point.z, 400, 1e-2); // contact on the edge
    const float dx = std::sqrt(20.f * 20 - 10.f * 10);
    EXPECT_NEAR(h->t, (200 - dx) / 400, 1e-3);
}

TEST(Collision, ClosestPointAndMarkupFlags) {
    const auto m = room();
    auto c = m.closestPoint({150, 0, 30}, 100);
    ASSERT_TRUE(c);
    EXPECT_NEAR(c->t, 30, 1e-3); // floor (30) beats wall (50)
    EXPECT_NEAR(c->normal.z, 1, 1e-4);
    EXPECT_FALSE(m.closestPoint({0, 0, 300}, 100));
    EXPECT_EQ(collision::markupFlags(1), 0x40000001u);  // ledgegrab | polyline
    EXPECT_EQ(collision::markupFlags(10), 0x100000u);   // water (volume, no polyline bit)
    EXPECT_EQ(collision::markupFlags(16), 0x4000u | 0x8001u);
    EXPECT_EQ(collision::markupFlags(30), 0x80000000u); // unknown -> disabled
    EXPECT_EQ(collision::markupTypeName(14), "balancebeam");
}

// ---- Real data (skipped without OU_GAME_DIR) -------------------------------------------
namespace {

struct Unit {
    cdc::Drm drm;
    CollisionMesh col;
    CollisionMesh render; // render terrain triangles, for agreement checks
};

Unit loadUnit(const std::string& path) {
    const char* g = std::getenv("OU_GAME_DIR");
    const cdc::BigfileSet bf(g);
    auto bytes = bf.read(path);
    if (!bytes) throw cdc::FormatError("missing " + path);
    Unit u{cdc::parseDrm(std::move(*bytes)), {}, {}};
    std::vector<std::string> warn;
    u.col = collision::loadLevelCollision(u.drm, &warn);
    EXPECT_TRUE(warn.empty()) << warn.front();
    std::vector<std::string> mw;
    for (const auto& mesh : formats::decodeModels(u.drm, mw, path)) {
        if (mesh.origin.find("terrain") == std::string::npos) continue;
        const auto base = uint32_t(u.render.vertices.size());
        for (const auto& v : mesh.vertices) u.render.vertices.push_back({v.position[0], v.position[1], v.position[2]});
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            u.render.faces.push_back({{base + mesh.indices[i], base + mesh.indices[i + 1], base + mesh.indices[i + 2]}});
    }
    u.render.build();
    return u;
}

// Vertical probes on a 100-unit grid over the collision bounds, stepping down through every
// collision surface. For each floor crossing (face normal by CCW winding has z > 0.7) the render
// terrain surface nearest that height on the same vertical (searched +-64 units) gives |dz|.
// (Top-down first hits are not comparable: render roofs are thick shells, collision has thin lids.)
struct Agreement { int probes = 0, floorHits = 0, matched = 0; std::vector<float> dz; };
Agreement probeAgreement(const Unit& u) {
    vec3 lo(INFINITY), hi(-INFINITY);
    for (const auto& v : u.col.vertices) lo = glm::min(lo, v), hi = glm::max(hi, v);
    Agreement a;
    for (float x = lo.x + 50; x < hi.x; x += 100)
        for (float y = lo.y + 50; y < hi.y; y += 100) {
            ++a.probes;
            // Step below each hit; at |z| ~ 5e5 (l1_seatop) a fixed 0.01 rounds back to the same float.
            for (float z = hi.z + 10; auto c = u.col.groundProbe({x, y, z}, z - lo.z + 10, 0x0F);
                 z = c->point.z - std::max(0.01f, std::abs(c->point.z) * 1e-6f)) {
                if (u.col.faceNormal(c->face).z <= 0.7f) continue;
                ++a.floorHits;
                const auto r = u.render.raycast({x, y, c->point.z + 64}, {0, 0, -1}, 128);
                if (!r) continue;
                ++a.matched;
                a.dz.push_back(std::abs(r->point.z - c->point.z));
            }
        }
    std::sort(a.dz.begin(), a.dz.end());
    return a;
}

} // namespace

TEST(CollisionRealData, PoolCollisionMarkupAndRenderAgreement) {
    if (!std::getenv("OU_GAME_DIR")) GTEST_SKIP() << "OU_GAME_DIR not set";
    const Unit u = loadUnit("pc-w\\l4_pool.drm");
    // Terrain groups 0-7 (signal group skipped): 5811+8+14+84+38+44+44 faces.
    EXPECT_EQ(u.col.faces.size(), 6043u);
    EXPECT_EQ(u.col.markup.size(), 73u);
    std::map<std::string_view, int> types;
    for (const auto& mk : u.col.markup) {
        ++types[collision::markupTypeName(mk.type)];
        EXPECT_GE(mk.points.size(), 2u);
    }
    EXPECT_EQ(types["ledgegrab"], 61);
    EXPECT_EQ(types["balancebeam"], 10);
    EXPECT_EQ(types["jumplanding"], 2);

    const Agreement a = probeAgreement(u);
    ASSERT_FALSE(a.dz.empty());
    auto within = [&](float tol) {
        return float(std::upper_bound(a.dz.begin(), a.dz.end(), tol) - a.dz.begin()) / float(a.dz.size());
    };
    std::printf("l4_pool grid probes %d, collision floor crossings %d, render surface within 64: %d (%.1f%%)\n"
                "|dz| median %.2f p90 %.2f p99 %.2f; within 1: %.1f%%, 4: %.1f%%, 16: %.1f%%\n",
                a.probes, a.floorHits, a.matched, 100.f * a.matched / a.floorHits, a.dz[a.dz.size() / 2],
                a.dz[a.dz.size() * 9 / 10], a.dz[a.dz.size() * 99 / 100], 100 * within(1), 100 * within(4),
                100 * within(16));
    // Observed 2026-10-18: 88.6% matched, median 8.4, p90 11.9, 92.3% of matched within 16 (collision
    // floors sit 0-12 units above the render floors, e.g. pool floor -3477 vs -3487; COLLISION.md).
    EXPECT_GT(float(a.matched) / float(a.floorHits), 0.85f);
    EXPECT_LT(a.dz[a.dz.size() / 2], 12.f);
    EXPECT_GT(within(16), 0.9f);

    // Ledge markup lies on collision geometry.
    int near = 0, total = 0;
    for (const auto& mk : u.col.markup)
        if (mk.type == 1)
            for (const auto& p : mk.points) ++total, near += bool(u.col.closestPoint(p, 16));
    EXPECT_GT(near, total * 9 / 10) << near << " of " << total;
}
