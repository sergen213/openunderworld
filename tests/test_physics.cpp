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

TEST(PhysicsRealData, BallcourtLeverObject) {
    const auto* bf = bigfile();
    if (!bf) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto o = physics::loadObjectPhysics(cdc::parseDrm(*bf->read("pc-w\\ca_pt_phole_lever.drm")));
    EXPECT_EQ(o.physClass, 6u);
    ASSERT_EQ(o.markers.size(), 1u);
    EXPECT_EQ(o.markers[0].kind, 2u);
    EXPECT_NEAR(o.markers[0].pos.z, 1062.76f, 0.01f);
    EXPECT_NEAR(physics::massProps(o).mass, 50.0f, 1e-4f);
}

// The gameplay camera collides only with objects whose oflags have bit 0x02 (tru.exe 0x5E94C0): the Thor slide door
// does, a pole does not (its shape lacks kCameraObjectMask, so camera queries pass through it).
TEST(PhysicsRealData, CameraCollidesOnlyWithFlaggedObjects) {
    const auto* bf = bigfile();
    if (!bf) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto mask = [&](const char* name) {
        const auto o = physics::loadObjectPhysics(cdc::parseDrm(*bf->read(std::string("pc-w\\") + name + ".drm")));
        for (int s = 0; s < int(o.segments.size()); ++s)
            if (const auto shape = physics::solidShape(o, s)) return int(shape->faces[0].mask);
        return -1;
    };
    // Both also have oflags bit 0x08, so they block the targeting line of sight (kSightObjectMask, tru.exe 0x713090).
    EXPECT_EQ(mask("ar_ei_thor_slide_door"), physics::kObjectMask | physics::kCameraObjectMask | physics::kSightObjectMask);
    EXPECT_EQ(mask("ar_pt_pole_a"), physics::kObjectMask | physics::kSightObjectMask);
    EXPECT_EQ(physics::kSightObjectMask & collision::kSightMask, physics::kSightObjectMask);
    EXPECT_EQ(physics::kObjectMask & collision::kSightMask, 0);
    EXPECT_EQ(physics::kCameraObjectMask & collision::kCameraMask, physics::kCameraObjectMask);
    EXPECT_EQ(physics::kObjectMask & collision::kCameraMask, 0);
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

// Uber joints on the Jolt scene (one step per 60 Hz tick): a vertical prismatic body falls to its lower limit and
// stops there, its linked follower at half its travel; a motor whose impulse bound exceeds the body's weight per step
// (mass 100, 395: tru.exe's hallway ramps) lifts it to the upper limit; a weaker one (250 < m * g per step, 286) lets it
// slide down; same steps, same result.
TEST(Physics, UberJointFallsLiftsAndLinks) {
    physics::Scene s;
    physics::Joint1 ramp;
    ramp.mass = 100, ramp.axis = {0, 0, 1}, ramp.pivot = {1000, 2000, 500};
    ramp.limited = true, ramp.lo = -948.167f, ramp.hi = 0;
    physics::Joint1 follower = ramp;
    follower.pivot.x += 500, follower.limited = false;
    for (int i = 0; i < 120; ++i) s.setJoints({{1, &ramp}, {2, &follower}}, {{1, 2, 0.5f}}), s.step(kDt);
    EXPECT_NEAR(ramp.x, ramp.lo, 2.0f);
    EXPECT_NEAR(ramp.v, 0.0f, 0.5f);
    EXPECT_NEAR(follower.x, 0.5f * ramp.x, 2.0f);
    const auto run = [&](physics::Joint1& j, int n) { for (int i = 0; i < n; ++i) s.setJoints({{1, &j}}, {}), s.step(kDt); };
    ramp.motor = true, ramp.speed = 316.056f / 30.0f, ramp.force = 395.07f;
    float before = ramp.x;
    run(ramp, 1);
    EXPECT_GT(ramp.x, before) << "the motor beats gravity per step";
    run(ramp, 400);
    EXPECT_NEAR(ramp.x, 0.0f, 2.0f);
    ramp.force = 250;
    before = ramp.x;
    run(ramp, 10);
    EXPECT_LT(ramp.x, before - 1.0f);
    physics::Joint1 a = ramp, b = ramp;
    physics::Scene sa, sb;
    for (int i = 0; i < 100; ++i) sa.setJoints({{1, &a}}, {}), sa.step(kDt), sb.setJoints({{1, &b}}, {}), sb.step(kDt);
    EXPECT_EQ(a.x, b.x);
    EXPECT_EQ(a.v, b.v);
    // A hinge with its centre of mass beside the axis swings down toward hanging below the pivot.
    physics::Joint1 arm;
    arm.hinge = true, arm.mass = 10, arm.inertia = 10 * 100.0f * 100.0f, arm.axis = {1, 0, 0}, arm.arm = {0, 100, 0};
    for (int i = 0; i < 60; ++i) s.setJoints({{3, &arm}}, {}), s.step(kDt);
    EXPECT_LT(arm.x, -0.1f) << "turned about +X so the arm points down";
}

// Loose bodies (World: grapple-pulled weights): boxes dropped onto the floor land, stack and settle; one dropped
// tilted tumbles onto a face.
TEST(Physics, LooseBodiesLandStackAndTumble) {
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {0, 0, -50}), {6000, 6000, 100});
    floor.build();
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim p;
    p.kind = physics::PrimKind::Box, p.size = {100, 100, 100}, p.pos = {0, 0, 50}, p.mass = 50, p.head[1] = 0x11;
    o.prims = {p};
    physics::Scene s;
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    const std::vector<physics::Scene::Loose> loose{
        {1, &o, glm::translate(glm::mat4(1), {0, 0, 300})},
        {2, &o, glm::translate(glm::mat4(1), {10, 0, 500})},
        {3, &o, glm::translate(glm::mat4(1), {1500, 0, 400}) * glm::rotate(glm::mat4(1), 0.6f, {1, 0, 0})}};
    for (int i = 0; i < 6 * 60; ++i) s.setTerrain(terrain), s.setLoose(loose), s.step(kDt);
    const auto m1 = *s.loose(1), m2 = *s.loose(2), m3 = *s.loose(3);
    EXPECT_NEAR(m1[3].z, 0, 5);
    EXPECT_NEAR(m2[3].z, 100, 8) << "stacked on the first";
    EXPECT_NEAR(std::max({std::abs(m3[0].z), std::abs(m3[1].z), std::abs(m3[2].z)}), 1.0f, 0.01f) << "tumbled onto a face";
    EXPECT_LT(m3[3].z, 60) << "fell";
    s.setLoose({}), s.step(kDt);
    EXPECT_FALSE(s.loose(1));
}

// Scene::save / restore: a scene saved mid-tumble (bodies in contact, a body removed earlier so IDs are not in creation
// order) and restored into a fresh scene whose caller re-creates the same entities continues bit for bit.
TEST(Physics, SavedSceneContinuesBitIdentically) {
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {0, 0, -50}), {6000, 6000, 100});
    floor.build();
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim p;
    p.kind = physics::PrimKind::Box, p.size = {100, 100, 100}, p.pos = {0, 0, 50}, p.mass = 50;
    o.prims = {p};
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    std::vector<physics::Scene::Loose> loose{
        {7, &o, glm::translate(glm::mat4(1), {0, 0, 200})},
        {1, &o, glm::translate(glm::mat4(1), {20, 0, 400}) * glm::rotate(glm::mat4(1), 0.6f, {1, 0, 0})},
        {2, &o, glm::translate(glm::mat4(1), {-30, 10, 650}) * glm::rotate(glm::mat4(1), 0.4f, {0, 1, 0})}};
    physics::Joint1 ramp; // a joint too: its constraint zero and warm start travel with the state
    ramp.mass = 100, ramp.axis = {0, 0, 1}, ramp.pivot = {3000, 0, 500}, ramp.limited = true, ramp.lo = 400, ramp.hi = 400;
    physics::Scene a;
    const auto tick = [&](physics::Scene& s, physics::Joint1& j) { s.setJoints({{9, &j}}, {}), s.setTerrain(terrain), s.setLoose(loose), s.step(kDt); };
    for (int i = 0; i < 40; ++i) tick(a, ramp);
    loose.erase(loose.begin()); // body 7 goes: its ID is free again
    for (int i = 0; i < 10; ++i) tick(a, ramp);
    const std::string saved = a.save();
    physics::Joint1 copy = ramp;
    physics::Scene b;
    ASSERT_TRUE(b.restore(saved));
    for (int i = 0; i < 90; ++i) tick(a, ramp), tick(b, copy);
    for (const int32_t id : {1, 2}) {
        const auto ma = *a.loose(id), mb = *b.loose(id);
        EXPECT_EQ(std::memcmp(&ma, &mb, sizeof ma), 0) << "body " << id;
    }
    EXPECT_EQ(ramp.x, copy.x);
    EXPECT_EQ(a.save(), b.save());
    EXPECT_FALSE(b.restore("not a state"));
}

// Pressure plates (World::stepPhysics): a plate body on a prismatic joint whose motor lifts it stays up unloaded; a
// kinematic weight (Lara, 55 kg) over it presses it to its limit and it rises once she leaves; a loose box landing on
// it presses it by contact. Lara's proxy shoves a loose box she walks into. A scene saved mid-press continues bit for bit.
TEST(Physics, PlatesWeighedByContactsAndProxyPushesLooseBodies) {
    auto plateMesh = std::make_shared<collision::CollisionMesh>();
    collision::addBox(*plateMesh, glm::translate(glm::mat4(1), {0, 0, -10}), {200, 200, 20});
    plateMesh->build();
    const std::vector<collision::Dynamic> meshes{{1, plateMesh, glm::mat4(1)}};
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {2000, 0, -50}), {2000, 2000, 100});
    floor.build();
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim p;
    p.kind = physics::PrimKind::Box, p.size = {60, 60, 60}, p.pos = {0, 0, 30}, p.mass = 50;
    o.prims = {p};
    physics::Joint1 plate;
    plate.mass = 20, plate.limited = true, plate.lo = -60, plate.hi = 0;
    plate.motor = true, plate.speed = 4, plate.force = (20 + 20) * physics::kJointGravity * 0.5f;
    std::vector<physics::Scene::Loose> loose;
    std::vector<physics::Scene::Weight> weights;
    physics::Scene::Character lara{{1500, 0, 0}, 40, 180, 55};
    const auto tick = [&](physics::Scene& s, physics::Joint1& j) {
        s.setPlates(std::vector<physics::Scene::Plate>{{1, meshes}}, weights), s.setJoints({{1, &j}}, {});
        s.setCharacter(&lara), s.setTerrain(terrain), s.setLoose(loose), s.step(kDt);
    };
    physics::Scene s;
    for (int i = 0; i < 30; ++i) tick(s, plate);
    EXPECT_NEAR(plate.x, 0, 1) << "unloaded: held up";
    weights = {{{0, 0, 0}, 55}};
    for (int i = 0; i < 60; ++i) weights[0].at.z = plate.x, tick(s, plate); // she rides it down
    EXPECT_NEAR(plate.x, -60, 2) << "Lara on it";
    weights.clear();
    for (int i = 0; i < 60; ++i) tick(s, plate);
    EXPECT_NEAR(plate.x, 0, 1) << "rises back";
    loose = {{2, &o, glm::translate(glm::mat4(1), {0, 0, 50})}, {3, &o, glm::translate(glm::mat4(1), {1600, 0, 0})}};
    for (int i = 0; i < 60; ++i) tick(s, plate);
    EXPECT_NEAR(plate.x, -60, 2) << "a loose box landed on it";
    EXPECT_NEAR((*s.loose(2))[3].z, -60, 5);
    const float boxX = (*s.loose(3))[3].x;
    for (int i = 0; i < 30; ++i) lara.feet.x += 4, tick(s, plate);
    EXPECT_GT((*s.loose(3))[3].x, boxX + 50) << "shoved by her proxy";
    physics::Joint1 copy = plate;
    physics::Scene b;
    ASSERT_TRUE(b.restore(s.save()));
    for (int i = 0; i < 30; ++i) lara.feet.x += 4, tick(s, plate), tick(b, copy);
    EXPECT_EQ(plate.x, copy.x);
    EXPECT_EQ(s.save(), b.save());
}

// A two-wheel vehicle (Scene::setVehicles, Jolt VehicleConstraint + MotorcycleController) on a flat floor: it settles on
// its springs, drives forward under throttle, brakes to a stop, steers, and a scene saved mid-run continues bit for bit.
TEST(Physics, VehicleDrivesBrakesAndSavesBitIdentically) {
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {0, 0, -50}), {200000, 200000, 100});
    floor.build();
    physics::ObjectPhysics o;
    o.segments = {glm::mat4(1)};
    physics::Prim chassis;
    chassis.kind = physics::PrimKind::Box, chassis.size = {80, 500, 150}, chassis.pos = {0, 0, 250}, chassis.mass = 150;
    o.prims = {chassis};
    physics::VehicleSpec spec;
    spec.physics = &o;
    for (const float y : {-230.0f, 230.0f}) {
        physics::VehicleSpec::Wheel w;
        w.attach = {0, y, 250}, w.travel = 250, w.radius = 100, w.mass = 10, w.frequency = 2.7f, w.damping = 0.35f;
        w.drive = 0.5f, w.brake = 400, w.steer = y < 0 ? 0.5f : 0.0f, w.raise = 0.7f;
        spec.wheels.push_back(w);
    }
    spec.maxTorque = 30, spec.minRpm = 1000, spec.maxRpm = 18000, spec.torque = {1, 1}, spec.ratio = 40, spec.reverseRatio = 160, spec.reverseTorque = 20;
    spec.maxPitchRoll = 0.74f;
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    physics::VehicleDrive drive;
    const auto tick = [&](physics::Scene& s) {
        const physics::Scene::Vehicle v{5, &spec, glm::mat4(1), drive};
        s.setTerrain(terrain), s.setVehicles({&v, 1}), s.step(kDt);
    };
    physics::Scene a;
    for (int i = 0; i < 60; ++i) tick(a);
    auto st = *a.vehicle(5);
    EXPECT_EQ(st.contacts, 3u) << "both wheels down";
    EXPECT_NEAR(st.model[3].z, 100 - 25, 15) << "on its springs: wheels 100 below at full extension, the weight sags them";
    EXPECT_LT(glm::length(st.vel), 20);
    drive.forward = 1;
    for (int i = 0; i < 120; ++i) tick(a);
    st = *a.vehicle(5);
    const float fast = -st.vel.y; // model -Y forward
    EXPECT_GT(fast, 1000) << "accelerated";
    EXPECT_LT(std::abs(st.vel.x), 50) << "straight";
    const std::string saved = a.save();
    physics::Scene b;
    ASSERT_TRUE(b.restore(saved));
    drive.forward = 0, drive.right = 1;
    for (int i = 0; i < 30; ++i) tick(a), tick(b);
    drive.right = 0, drive.brake = 1;
    for (int i = 0; i < 180; ++i) tick(a), tick(b);
    const auto sa = *a.vehicle(5), sb = *b.vehicle(5);
    EXPECT_LT(glm::length(sa.vel), 30) << "braked to a stop";
    EXPECT_GT(std::abs(sa.model[3].x), 50) << "steered off the line";
    EXPECT_EQ(std::memcmp(&sa.model, &sb.model, sizeof sa.model), 0);
    EXPECT_EQ(a.save(), b.save());
    a.setVehicles({}), a.step(kDt);
    EXPECT_FALSE(a.vehicle(5));
}

// Ragdolls (Scene::setRagdolls): a three-bone chain (root, a hinged bone, a cone-twist bone) dropped over a 300-unit
// step falls, lands on both levels and sleeps; its joints keep the bones together; a scene saved mid-fall and restored
// into a fresh scene whose caller re-creates the same ragdoll continues bit for bit.
TEST(Physics, RagdollFallsDownAStepJointsHoldAndSaves) {
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {0, 0, -50}), {6000, 6000, 100});
    collision::addBox(floor, glm::translate(glm::mat4(1), {-1500, 0, 150}), {3000, 6000, 300}); // a 300-unit ledge at x < 0
    floor.build();
    physics::RagdollDef def;
    def.mass = 60;
    physics::RagdollBone root, hinge, cone;
    root.segment = 0, root.endSegment = 1, root.massShare = 50, root.radius = 30;
    hinge.segment = 1, hinge.endSegment = 2, hinge.parent = 0, hinge.massShare = 30, hinge.radius = 20, hinge.joint = 1;
    hinge.axis1 = {1, 0, 0}, hinge.limits[0] = -90, hinge.limits[1] = 0, hinge.friction = 10;
    cone.segment = 2, cone.endSegment = 3, cone.parent = 1, cone.massShare = 20, cone.radius = 15, cone.joint = 3;
    cone.axis1 = {1, 0, 0}, cone.limits[0] = -20, cone.limits[1] = 20, cone.limits[4] = 40, cone.friction = 10;
    def.bones = {root, hinge, cone};
    const std::vector<vec3> bind{{0, 0, 0}, {0, 0, 200}, {0, 0, 400}, {0, 0, 550}};
    // lying along +X across the ledge's edge, 200 above the lower floor
    const glm::mat4 lie = glm::rotate(glm::mat4(1), 1.5707964f, vec3(0, 1, 0));
    std::vector<glm::mat4> frames;
    for (const vec3& b : {bind[0], bind[1], bind[2]}) frames.push_back(glm::translate(glm::mat4(1), vec3(-250, 0, 500)) * lie * glm::translate(glm::mat4(1), b));
    const std::vector<vec3> velocity(3, vec3(0));
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    const physics::Scene::Ragdoll r{4, &def, bind, frames, velocity, vec3(0, 0, 500), vec3(0, 0, -3000)};
    const auto tick = [&](physics::Scene& s) { s.setTerrain(terrain), s.setRagdolls({&r, 1}), s.step(kDt); };
    physics::Scene a;
    for (int i = 0; i < 20; ++i) tick(a);
    const std::string saved = a.save();
    physics::Scene b;
    ASSERT_TRUE(b.restore(saved));
    std::optional<physics::Scene::RagdollPose> pa, pb;
    for (int i = 0; i < 600 && !(pa && pa->asleep); ++i) tick(a), tick(b), pa = a.ragdoll(4), pb = b.ragdoll(4);
    ASSERT_TRUE(pa && pb);
    EXPECT_TRUE(pa->asleep) << "settled";
    ASSERT_EQ(pa->frames.size(), 3u);
    EXPECT_EQ(std::memcmp(pa->frames.data(), pb->frames.data(), sizeof(glm::mat4) * 3), 0) << "bit for bit after the load";
    EXPECT_EQ(a.save(), b.save());
    const auto end = [&](size_t k) { return vec3(pa->frames[k] * glm::vec4(bind[k + 1] - bind[k], 1)); };
    for (size_t k = 0; k < 2; ++k) // each bone's end stays at its child's start (the joint pivots)
        EXPECT_LT(glm::distance(end(k), vec3(pa->frames[k + 1][3])), 5.0f) << "joint " << k + 1;
    EXPECT_NEAR(pa->frames[0][3].z, 300 + 30, 15) << "the root rests on the ledge";
    EXPECT_LT(end(2).z, 300.0f) << "the far end hangs or lies below the ledge top";
    a.setRagdolls({}), a.step(kDt);
    EXPECT_FALSE(a.ragdoll(4));
}

// Poacher ragdoll record (ObjectBase +0x7C): 15 bones, mass shares summing to 100, the pelvis root on segments 2 -> 3.
TEST(PhysicsRealData, PoacherRagdoll) {
    const auto* bf = bigfile();
    if (!bf) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto r = physics::loadRagdoll(cdc::parseDrm(*bf->read("pc-w\\poacher.drm")));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->mass, 60.0f);
    EXPECT_EQ(r->radiusScale, 2.0f);
    ASSERT_EQ(r->bones.size(), 15u);
    float share = 0;
    for (const auto& b : r->bones) share += b.massShare;
    EXPECT_NEAR(share, 100, 1e-3f);
    EXPECT_EQ(r->bones[0].segment, 2);
    EXPECT_EQ(r->bones[0].endSegment, 3);
    EXPECT_EQ(r->bones[0].parent, -1);
    EXPECT_EQ(r->bones[0].joint, 0);
    EXPECT_EQ(r->bones[5].joint, 1);                // forearm hinge
    EXPECT_EQ(r->bones[5].limits[0], -125.0f);
    EXPECT_EQ(r->bones[2].joint, 3);                // head cone
    EXPECT_EQ(r->bones[2].radius, 30.0f);           // 15 x 2
    EXPECT_EQ(r->bones[1].friction, 150.0f);
    EXPECT_FALSE(physics::loadRagdoll(cdc::parseDrm(*bf->read("pc-w\\ca_rb_wall_hpole_door.drm"))));
}

// Buoyancy (Scene::setWater, tru.exe 0x6AD070): a 100-unit box of 10 kg (220 kg/m^3) floats with 22 % of it under the
// surface; one of 100 kg (2200 kg/m^3) sinks to the floor.
TEST(Physics, LooseBodiesFloatOrSinkInWater) {
    collision::CollisionMesh floor;
    collision::addBox(floor, glm::translate(glm::mat4(1), {0, 0, -50}), {6000, 6000, 100});
    floor.build();
    const auto box = [](float mass) {
        physics::ObjectPhysics o;
        o.segments = {glm::mat4(1)};
        physics::Prim p;
        p.kind = physics::PrimKind::Box, p.size = {100, 100, 100}, p.pos = {0, 0, 50}, p.mass = mass, p.head[1] = 0x11;
        o.prims = {p};
        return o;
    };
    const physics::ObjectPhysics light = box(10), heavy = box(100);
    physics::Scene s;
    const std::vector<physics::Scene::Terrain> terrain{{"floor", &floor, vec3(0)}};
    const std::vector<physics::Scene::Loose> loose{{1, &light, glm::translate(glm::mat4(1), {0, 0, 300})},
                                                   {2, &heavy, glm::translate(glm::mat4(1), {1000, 0, 300})}};
    const std::vector<physics::Scene::Water> water{{vec3(0, 0, 0), vec3(3000, 3000, 500), {1, 0}, 500.0f}};
    for (int i = 0; i < 20 * 60; ++i) s.setTerrain(terrain), s.setLoose(loose), s.setWater(water), s.step(kDt);
    const float z = (*s.loose(1))[3].z; // bottom face; Jolt's convex radius rounds the box, so not exactly 22 under
    EXPECT_GT(z, 500 - 60.0f) << "floats";
    EXPECT_LT(z, 500 - 10.0f) << "partly under the surface";
    EXPECT_NEAR((*s.loose(2))[3].z, 0, 5) << "sinks";
}
