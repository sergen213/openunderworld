#include "physics/Physics.hpp"

#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace ou;
using physics::vec3;

namespace {

constexpr float kDt = 1.0f / 60.0f;
constexpr float kDeg = 3.14159265f / 180;

// A 700 x 200 x 1000 door panel (mass 100) hanging from a hinge along X at its top (z = 1000).
physics::Scene doorScene(float lo, float hi) {
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim p;
    p.kind = physics::PrimKind::Box, p.size = {700, 200, 1000}, p.pos = {0, 0, 500}, p.mass = 100;
    o.prims = {p};
    physics::Scene s;
    const int b = s.addBody(physics::massProps(o), glm::translate(glm::mat4(1), {1000, 2000, 0}));
    s.addHinge(b, {0, 0, 1000}, {1, 0, 0}, lo, hi);
    s.hinges[0].maxTorque = 1e9f;
    return s;
}

} // namespace

TEST(Physics, MassPropsBoxMatchesExeFormula) {
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim p;
    p.kind = physics::PrimKind::Box, p.size = {10, 100, 200}, p.pos = {0, 0, 50}, p.mass = 12;
    o.prims = {p};
    const auto m = physics::massProps(o);
    EXPECT_FLOAT_EQ(m.mass, 12);
    EXPECT_EQ(m.com, vec3(0, 0, 50));
    // 0x4E9100: sides clamped to >= 0.2 * longest (x: 10 -> 40).
    EXPECT_FLOAT_EQ(m.inertia[0][0], 12 * (100.f * 100 + 200 * 200) / 12);
    EXPECT_FLOAT_EQ(m.inertia[1][1], 12 * (40.f * 40 + 200 * 200) / 12);
    EXPECT_FLOAT_EQ(m.inertia[2][2], 12 * (40.f * 40 + 100 * 100) / 12);
}

TEST(Physics, HingeMotorOpensToTargetAndHolds) {
    auto s = doorScene(0, 85 * kDeg);
    EXPECT_NEAR(s.angle(0), 0, 1e-5f);
    s.hinges[0].target = 80 * kDeg;
    float maxAngle = 0;
    for (int i = 0; i < 4 * 60; ++i) s.step(kDt), maxAngle = std::max(maxAngle, s.angle(0));
    EXPECT_NEAR(s.angle(0), 80 * kDeg, 1.5f * kDeg);
    EXPECT_LT(maxAngle, 85.5f * kDeg);
    // The anchor stays put and the body turns only about the hinge axis.
    const auto& h = s.hinges[0];
    const vec3 anchor = glm::vec3(s.bodies[0].model() * glm::vec4(h.localAnchor, 1));
    EXPECT_LT(glm::distance(anchor, h.worldAnchor), 1.0f);
    EXPECT_GT(glm::dot(glm::mat3_cast(s.bodies[0].rot) * h.localAxis, h.worldAxis), 0.9999f);
    // Closing: back to the lower limit.
    s.hinges[0].target = 0;
    for (int i = 0; i < 4 * 60; ++i) s.step(kDt);
    EXPECT_NEAR(s.angle(0), 0, 1.5f * kDeg);
}

TEST(Physics, HingeLimitsStopAFreeSwing) {
    auto s = doorScene(-30 * kDeg, 30 * kDeg);
    s.hinges[0].maxTorque = 0;
    s.setAngle(0, 30 * kDeg);
    float lo = 0, hi = 0;
    for (int i = 0; i < 20 * 60; ++i) {
        s.step(kDt);
        lo = std::min(lo, s.angle(0)), hi = std::max(hi, s.angle(0));
        ASSERT_TRUE(std::isfinite(s.angle(0)));
    }
    EXPECT_LT(hi, 31 * kDeg);
    EXPECT_GT(lo, -31 * kDeg);
    EXPECT_NEAR(s.angle(0), 0, 3 * kDeg); // damped back to hanging straight down
}

TEST(Physics, DeterministicAcrossRenderRates) {
    // The sim steps on the fixed tick whatever the render rate: 30, 60 and 144 Hz frames give the same ticks and
    // bit-identical bodies.
    auto run = [](double hz) {
        auto s = doorScene(0, 85 * kDeg);
        s.hinges[0].target = 70 * kDeg;
        engine::FrameClock clock(1.0 / 60.0);
        while (clock.ticks() < 180) // 3 s
            for (int n = clock.advance(1.0 / hz); n > 0; --n) s.step(float(clock.step()));
        return std::pair{clock.ticks(), s.bodies[0]};
    };
    const auto [t30, b30] = run(30);
    const auto [t60, b60] = run(60);
    const auto [t144, b144] = run(144);
    EXPECT_EQ(t30, t60);
    EXPECT_EQ(t60, t144);
    EXPECT_EQ(std::memcmp(&b30, &b60, sizeof b30), 0);
    EXPECT_EQ(std::memcmp(&b60, &b144, sizeof b60), 0);
}

TEST(Physics, MoverPingPongs) {
    physics::Mover m;
    m.points = {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}};
    m.speed = 50;
    m.s = 150;
    EXPECT_EQ(m.position(), vec3(100, 50, 0));
    m.s = 250; // 200 out, 50 back
    EXPECT_EQ(m.position(), vec3(100, 50, 0));
    m.s = 400;
    EXPECT_EQ(m.position(), vec3(0, 0, 0));
}

TEST(Physics, DynamicCollisionQueriesFollowTheTransform) {
    collision::CollisionMesh world;
    auto shape = std::make_shared<collision::CollisionMesh>();
    collision::addBox(*shape, glm::translate(glm::mat4(1), {0, 0, 500}), {700, 200, 1000});
    shape->build();
    world.dynamics.push_back({7, shape, glm::translate(glm::mat4(1), {0, 1000, 0})});
    // Ray toward the closed door: hits its +y face (y = 1100), normal back at the ray.
    auto h = world.raycast({0, 3000, 300}, {0, -1, 0}, 5000);
    ASSERT_TRUE(h);
    EXPECT_NEAR(h->point.y, 1100, 1e-2f);
    EXPECT_EQ(h->dynamic, 0);
    EXPECT_GT(h->normal.y, 0.99f);
    // A capsule walking into it stops at the face; closest point agrees.
    auto c = world.sweepCapsule({0, 1400, 100}, {0, 1400, 412}, 100, {0, -1000, 0});
    ASSERT_TRUE(c);
    EXPECT_NEAR(1400 - 1000 * c->t, 1200, 1.0f);
    auto p = world.closestPoint({0, 1300, 300}, 500);
    ASSERT_TRUE(p);
    EXPECT_NEAR(p->t, 200, 1e-2f);
    // Swing the door up 90 degrees about its top edge (x axis at z = 1000): the doorway is clear below.
    world.dynamics[0].transform = glm::translate(glm::mat4(1), {0, 1000, 1000}) *
                                  glm::rotate(glm::mat4(1), 3.14159265f / 2, {1, 0, 0}) *
                                  glm::translate(glm::mat4(1), {0, 0, -1000});
    EXPECT_FALSE(world.raycast({0, 3000, 300}, {0, -1, 0}, 5000));
    EXPECT_FALSE(world.sweepCapsule({0, 1400, 100}, {0, 1400, 412}, 100, {0, -1000, 0}));
    // ... and now blocks from above.
    auto up = world.groundProbe({0, 1500, 2000}, 3000);
    ASSERT_TRUE(up);
    EXPECT_NEAR(up->point.z, 1100, 1e-2f);
    // Face masks filter dynamics like terrain.
    EXPECT_FALSE(world.groundProbe({0, 1500, 2000}, 3000, 0x00));
}

// ---- real data (skipped without OU_GAME_DIR) -----------------------------------------------------------
namespace {
const cdc::BigfileSet* bigfile() {
    static std::unique_ptr<cdc::BigfileSet> bf;
    if (!bf) {
        const char* g = std::getenv("OU_GAME_DIR");
        if (!g) return nullptr;
        bf = std::make_unique<cdc::BigfileSet>(g);
    }
    return bf.get();
}
} // namespace

TEST(PhysicsRealData, BallcourtDoorObject) {
    const auto* bf = bigfile();
    if (!bf) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto o = physics::loadObjectPhysics(cdc::parseDrm(*bf->read("pc-w\\ca_rb_wall_hpole_door.drm")));
    EXPECT_EQ(o.physClass, physics::kClassRigidBody);
    EXPECT_TRUE(o.oflags & 0x80000000u);
    ASSERT_EQ(o.prims.size(), 3u);
    EXPECT_EQ(o.prims[1].kind, physics::PrimKind::Box);
    EXPECT_EQ(o.prims[0].mass + o.prims[1].mass + o.prims[2].mass, 100);
    EXPECT_NEAR(o.prims[0].size.z, 1026.36f, 0.01f);
    EXPECT_NEAR(o.prims[0].pos.z, 512.86f, 0.01f);
    ASSERT_EQ(o.markers.size(), 1u);
    EXPECT_NEAR(o.markers[0].pos.z, 952.9f, 0.01f);
    EXPECT_EQ(o.markers[0].kind, 2u);
    EXPECT_TRUE(physics::solidShape(o, 0));
}

TEST(PhysicsRealData, AllObjectsParse) {
    const auto* bf = bigfile();
    if (!bf) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto list = bf->read("\\tr8\\pc-w\\objectlist.txt");
    ASSERT_TRUE(list);
    size_t objects = 0, prims = 0, boxes = 0, spheres = 0, capsules = 0, rigid = 0;
    for (const auto& [id, name] : formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()})) {
        const auto bytes = bf->read("pc-w\\" + name + ".drm");
        if (!bytes) continue;
        const auto o = physics::loadObjectPhysics(cdc::parseDrm(*bytes));
        ++objects;
        prims += o.prims.size();
        rigid += o.physClass == physics::kClassRigidBody;
        for (const auto& p : o.prims) {
            boxes += p.kind == physics::PrimKind::Box, spheres += p.kind == physics::PrimKind::Sphere;
            capsules += p.kind == physics::PrimKind::Capsule;
            ASSERT_LT(p.segment, int(o.segments.size())) << name;
            ASSERT_TRUE(std::isfinite(p.pos.x) && std::isfinite(p.size.x)) << name;
        }
    }
    // 2026-10-18 survey: every object DRM decodes; 28 rigid bodies (class 34).
    EXPECT_GT(objects, 700u);
    EXPECT_EQ(rigid, 28u);
    EXPECT_GT(boxes, 1000u);
    EXPECT_GT(spheres, 300u);
    EXPECT_GT(capsules, 50u);
    std::printf("objects %zu prims %zu (boxes %zu spheres %zu capsules %zu) rigid %zu\n", objects, prims, boxes, spheres,
                capsules, rigid);
}
