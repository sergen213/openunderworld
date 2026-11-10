// Combat: health/fall damage/death, hitscan, lock-on, ammo, render-rate independence (src/game/Combat.*).
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include "app/game/Autopilot.hpp"

#include <cstdlib>
#include <functional>
#include <memory>
#include <set>

using namespace ou;
using collision::CollisionMesh;
using game::Actions;
using game::State;
using game::vec3;

namespace {

void addFloor(CollisionMesh& m, float x0, float y0, float x1, float y1, float z) {
    for (float x = x0; x < x1; x += 200)
        for (float y = y0; y < y1; y += 200) {
            const float x2 = std::min(x + 200, x1), y2 = std::min(y + 200, y1);
            const auto b = uint32_t(m.vertices.size());
            m.vertices.insert(m.vertices.end(), {{x, y, z}, {x2, y, z}, {x2, y2, z}, {x, y2, z}});
            m.faces.push_back({{b, b + 1, b + 2}});
            m.faces.push_back({{b, b + 2, b + 3}});
        }
}
void addWall(CollisionMesh& m, float x, float y0, float y1, float h) { // plane x = const, facing -X
    const auto b = uint32_t(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {{x, y0, 0}, {x, y1, 0}, {x, y1, h}, {x, y0, h}});
    m.faces.push_back({{b, b + 1, b + 2}});
    m.faces.push_back({{b, b + 2, b + 3}});
}

// Synthetic Lara: one bone (guns sit at shoulder height), idle + run with lara.drm's run speed.
std::unique_ptr<game::World> makeWorld(CollisionMesh m, vec3 spawn) {
    m.build();
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    anim::Clip still, run;
    still.keyCount = run.keyCount = 20, still.timePerKey = run.timePerKey = 100;
    still.tracks.resize(1), run.tracks.resize(1);
    for (int i = 0; i < 20; ++i) run.tracks[0][anim::TransY].push_back(-42.47f * float(i + 1)); // ~1274 units/s
    a.clips["ng_idle"] = still;
    a.clips["ng_run"] = run;
    return std::make_unique<game::World>(std::move(m), std::move(a), game::Spawn{spawn, 0});
}
Actions guns(bool fire, bool aim = true) {
    Actions a;
    a.draw = false, a.fire = fire, a.aim = aim;
    return a;
}

} // namespace

TEST(Combat, FallDamageThresholds) {
    const game::Health h;
    EXPECT_EQ(h.fallDamage(0), 0);
    EXPECT_EQ(h.fallDamage(1280), 0);
    EXPECT_NEAR(h.fallDamage(1817.32f), 500, 0.01);
    EXPECT_NEAR(h.fallDamage(2000), 500 + 500 * (2000 - 1817.32f) / (2215.55f - 1817.32f), 0.01);
    EXPECT_NEAR(h.fallDamage(2215.55f), 1000, 0.01);
    EXPECT_EQ(h.fallDamage(2215.6f), 1000);
    EXPECT_NEAR(h.fallDamage(1548.7f), 500 * (1548.7f - 1280.03f) / (1817.32f - 1280.03f), 0.01);
}

TEST(Combat, InvulnerabilityWindowAndPriority) {
    game::Health h;
    EXPECT_TRUE(h.damage(100, game::kDamageLight));
    EXPECT_FALSE(h.damage(100, game::kDamageLight)); // same priority within 1 s
    EXPECT_TRUE(h.damage(100, game::kDamageHeavy));  // priority 2 > 1
    EXPECT_EQ(h.value(), 800);
    h.tick(0.5f);
    EXPECT_FALSE(h.damage(100, game::kDamageLight));
    h.tick(0.51f);
    EXPECT_TRUE(h.damage(100, game::kDamageLight));
    h.fall(5000); // absolute, ignores the window
    EXPECT_TRUE(h.dead());
}

TEST(Combat, RegeneratesToQuarterOnly) {
    game::Health h;
    h.damage(900, game::kDamageLight);
    EXPECT_EQ(h.value(), 100);
    for (int i = 0; i < 90; ++i) h.tick(1.0f / 60); // 1.5 s delay
    EXPECT_NEAR(h.value(), 100, 1e-3);
    for (int i = 0; i < 400; ++i) h.tick(1.0f / 60); // half of 400 frames (13.3 s)
    const float t = 400.0f / 60 / (400.0f / 30);   // fraction of the regen time
    EXPECT_NEAR(h.value(), 100 + 250 * (0.3f * t + 0.7f * t * t * t), 0.5);
    for (int i = 0; i < 1200; ++i) h.tick(1.0f / 60);
    EXPECT_NEAR(h.value(), 250, 1e-3); // cap 0.25 x max
    game::Health o;
    o.heal(400, true);
    EXPECT_EQ(o.value(), 1400);
    for (int i = 0; i < 900; ++i) o.tick(1.0f / 60); // 15 s of the 30 s decay
    EXPECT_NEAR(o.value(), 1200, 0.5);
    for (int i = 0; i < 1200; ++i) o.tick(1.0f / 60);
    EXPECT_NEAR(o.value(), 1000, 1e-3);
}

TEST(Combat, FallDamageDeathAndRespawn) {
    // Platform at z 1500 ends at x 400; ground at 0. Running off: fall 1500 -> 204.7 damage.
    CollisionMesh m;
    addFloor(m, -1000, -1000, 400, 1000, 1500);
    addFloor(m, -1000, -1000, 6000, 1000, 0);
    auto w = makeWorld(m, {0, 0, 1500});
    Actions run;
    run.move.y = 1;
    w->tick(run);
    for (int i = 0; i < 240 && w->player().state().pos.z > 1; ++i) w->tick(run);
    w->tick({});
    EXPECT_NEAR(w->health().value(), 1000 - 500 * (1500 - 1280.03f) / (1817.32f - 1280.03f), 1.0);
    EXPECT_NE(w->player().state().state, State::Dead);

    CollisionMesh high;
    addFloor(high, -1000, -1000, 400, 1000, 2500);
    addFloor(high, -1000, -1000, 6000, 1000, 0);
    auto d = makeWorld(high, {0, 0, 2500});
    bool died = false;
    for (int i = 0; i < 600 && !died; ++i) {
        d->tick(run);
        died = d->player().state().state == State::Dead;
    }
    ASSERT_TRUE(died);
    EXPECT_TRUE(d->health().dead());
    EXPECT_EQ(d->player().state().clip, "death_fall_normal");
    for (int i = 0; i < 200 && d->player().state().state == State::Dead; ++i) d->tick({}); // 3 s death fade
    EXPECT_EQ(d->player().state().state, State::Idle); // respawned at the spawn with full health
    EXPECT_NEAR(d->player().state().pos.z, 2500, 1e-3);
    EXPECT_EQ(d->health().value(), 1000);

    auto k = makeWorld(m, {0, 0, 1500}); // enemy blows down to zero: death_toknee
    for (int i = 0; i < 20; ++i) {
        k->damageLara(100, {}, game::kDamageBypass);
        k->tick({});
    }
    EXPECT_EQ(k->player().state().state, State::Dead);
    EXPECT_EQ(k->player().state().clip, "death_toknee");
}

TEST(Combat, RayCapsule) {
    const vec3 base(1000, 0, 0);
    auto t = game::rayCapsule({0, 0, 300}, {1, 0, 0}, base, 100, 500, 5000);
    ASSERT_TRUE(t);
    EXPECT_NEAR(*t, 900, 1e-3);
    t = game::rayCapsule({0, 0, 600}, {1, 0, 0}, base, 100, 500, 5000); // over the top
    EXPECT_FALSE(t);
    t = game::rayCapsule({1000, 0, 1000}, {0, 0, -1}, base, 100, 500, 5000); // onto the top cap
    ASSERT_TRUE(t);
    EXPECT_NEAR(*t, 500, 1e-3);
    EXPECT_FALSE(game::rayCapsule({0, 0, 300}, {1, 0, 0}, base, 100, 500, 800)); // out of range
    EXPECT_FALSE(game::rayCapsule({0, 250, 300}, {1, 0, 0}, base, 100, 500, 5000)); // passes beside
}

TEST(Combat, PistolsLockFireAndHit) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {2000, 300, 0};
    w->targets.push_back(&t);
    for (int i = 0; i < 30; ++i) w->tick(guns(false)); // target lock draws and locks (tru.exe Target Lock)
    EXPECT_TRUE(w->combat().drawn);
    EXPECT_EQ(w->combat().shotsFired, 0u);
    for (int i = 0; i < 60; ++i) w->tick(guns(true)); // 1 s of fire
    const auto& c = w->combat();
    EXPECT_TRUE(c.drawn);
    EXPECT_EQ(c.target, &t);
    EXPECT_TRUE(c.locked);
    // already drawn: one shot per 1/3 s
    EXPECT_GE(c.shotsFired, 3u);
    EXPECT_LE(c.shotsFired, 4u);
    EXPECT_EQ(t.hits, int(c.shotsFired));
    const float dist = glm::length(t.aimPoint() - vec3(0, 0, 418)); // roughly from the gun
    EXPECT_NEAR(t.hp, 700 - float(t.hits) * w->anims().weapons[0].damageAt(dist), 3.0f * float(t.hits));

    // a wall between: no lock-on, free aim along the camera, shots stop at the wall
    CollisionMesh walled = m;
    addWall(walled, 1000, -5000, 5000, 3000);
    auto v = makeWorld(walled, {0, 0, 0});
    game::DummyTarget behind;
    behind.pos = {2000, 0, 0};
    v->targets.push_back(&behind);
    for (int i = 0; i < 90; ++i) v->tick(guns(true));
    EXPECT_EQ(v->combat().target, nullptr);
    EXPECT_GT(v->combat().shotsFired, 0u);
    EXPECT_EQ(behind.hits, 0);
}

TEST(Combat, LockOnSelectionAndSwitch) {
    CollisionMesh m;
    addFloor(m, -9000, -9000, 9000, 9000, 0);
    auto w = makeWorld(m, {0, 0, 0}); // facing +X; camera behind looking +X
    game::DummyTarget front, side, near, dead;
    front.pos = {3000, 200, 0};  // in the soft cone (27.5 deg), 3000 away
    side.pos = {2000, 2500, 0};  // 51 deg off Lara's facing
    near.pos = {-900, 300, 0};   // behind, within 1264: tier 1 for a hard lock
    dead.pos = {1500, 0, 0}, dead.hp = 0;
    struct Neutral : game::DummyTarget { game::Team team() const override { return game::Team::Neutral; } } other;
    other.pos = {1000, 0, 0};
    for (game::Damageable* t : std::initializer_list<game::Damageable*>{&front, &side, &near, &dead, &other}) w->targets.push_back(t);
    Actions draw;
    draw.draw = true;
    w->tick(draw);
    w->tick({});
    EXPECT_EQ(w->combat().target, &front); // soft: nearest in the cone in front, the dead and neutral ones skipped
    EXPECT_FALSE(w->combat().locked);
    w->tick(guns(false));
    EXPECT_EQ(w->combat().target, &near); // hard lock: tier 1 (nearest within 1264) wins
    EXPECT_TRUE(w->combat().locked);
    for (int i = 0; i < 5; ++i) w->tick(guns(false));
    EXPECT_EQ(w->combat().target, &near); // kept
    Actions sw = guns(false);
    sw.switchTarget = true;
    w->tick(sw);
    const auto* second = w->combat().target;
    EXPECT_NE(second, &near);
    w->tick(guns(false));
    w->tick(sw);
    EXPECT_NE(w->combat().target, second); // cycles
    near.hp = 0;                           // the lock drops when the target dies and re-acquires
    w->tick(guns(false));
    w->tick(guns(false));
    EXPECT_NE(w->combat().target, &near);
    EXPECT_NE(w->combat().target, nullptr);
}

TEST(Combat, AmmoClipsAndReload) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {1500, 0, 0}, t.hp = 1e9f;
    w->targets.push_back(&t);
    int reloads = 0;
    bool wasReloading = false;
    for (int i = 0; i < 60 * 12; ++i) { // 12 s of fire
        w->tick(guns(true));
        const auto& c = w->combat();
        EXPECT_GE(c.clip[0][0], 0);
        EXPECT_GE(c.clip[0][1], 0);
        reloads += c.reload > 0 && !wasReloading;
        wasReloading = c.reload > 0;
    }
    // 24 rounds per reload (12 per gun), 1/3 s per shot + 0.7 s reload
    EXPECT_GE(reloads, 1);
    EXPECT_GE(w->combat().shotsFired, 24u);
    EXPECT_EQ(t.hits, int(w->combat().shotsFired));
}

TEST(Combat, FireIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 aim 1\n0.1 fire 1\n1.5 right 1\n2.0 fire 0\n2.2 fire 1\n3.0 right 0\n");
    auto run = [&](double hz) {
        CollisionMesh m;
        addFloor(m, -5000, -5000, 5000, 5000, 0);
        auto w = makeWorld(m, {0, 0, 0});
        game::DummyTarget t;
        t.pos = {2500, 600, 0};
        w->targets.push_back(&t);
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 240)
            for (int n = clock.advance(1.0 / hz); n > 0 && w->ticks() < 240; --n) w->tick(script.at(w->ticks()));
        w->targets.clear();
        return std::tuple(w->combat().shotsFired, t.hp, w->combat().rng, w->player().state().pos);
    };
    const auto a = run(30), b = run(60), c = run(144);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_GT(std::get<0>(a), 5u);
}

TEST(Combat, HealthPacks) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    w->damageLara(800, {}, game::kDamageBypass);
    w->found[2] = 2;
    Actions heal;
    heal.heal = true;
    w->tick(heal);
    EXPECT_NEAR(w->health().value(), 840, 1e-3); // 200 + 640
    EXPECT_EQ(w->found[2], 1);
    w->tick({});
    w->tick(heal); // within the 60-frame cooldown: nothing
    EXPECT_NEAR(w->health().value(), 840, 1e-3);
    EXPECT_EQ(w->found[2], 1);
    for (int i = 0; i < 120; ++i) w->tick({});
    w->tick(heal);
    EXPECT_EQ(w->health().value(), 1000); // clamped at max, the pack is used anyway (0x6CBB20 does not check)
    EXPECT_EQ(w->found[2], 0);
    w->damageLara(500, {}, game::kDamageBypass);
    for (int i = 0; i < 130; ++i) w->tick({});
    w->tick(heal); // none left
    EXPECT_NEAR(w->health().value(), 500, 1e-3);
    EXPECT_EQ(w->combat().healthPacksUsed, 2);
}

TEST(Combat, WeaponSwitchKeepsRoundsAndReloadsFromReserve) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {1500, 0, 0}, t.hp = 1e9f;
    w->targets.push_back(&t);
    w->giveWeapon(game::Weapon::Uzi, 1000);
    EXPECT_EQ(w->combat().reserve[1], 240); // capped at the max reserve
    w->giveWeapon(game::Weapon::Uzi, 100);
    EXPECT_EQ(w->combat().reserve[1], 100);
    for (int i = 0; i < 60; ++i) w->tick(guns(true)); // pistols: draw, a few shots
    const int pistols0 = w->combat().clip[0][0], pistols1 = w->combat().clip[0][1];
    EXPECT_LT(pistols0 + pistols1, 24);
    Actions sw = guns(false);
    sw.weapon = true;
    w->tick(sw);
    EXPECT_EQ(w->combat().weapon, game::Weapon::Pistols); // the pistols holster first (unequip 0.633 s)
    for (int i = 0; i < 60; ++i) w->tick(guns(false));
    EXPECT_EQ(w->combat().weapon, game::Weapon::Uzi);
    EXPECT_EQ(w->combat().previous, game::Weapon::Pistols);
    EXPECT_TRUE(w->combat().drawn); // drawn again after the switch
    const uint64_t before = w->combat().shotsFired;
    for (int i = 0; i < 60; ++i) w->tick(guns(true)); // 1 s of uzi: one shot per 5/30 s, hands alternate
    EXPECT_GE(w->combat().shotsFired - before, 5u);
    EXPECT_EQ(w->combat().clip[0][0], pistols0); // the pistols kept their rounds
    EXPECT_EQ(w->combat().clip[0][1], pistols1);
    for (int i = 0; i < 60 * 16 && w->combat().reload <= 0; ++i) w->tick(guns(true)); // empty both clips (80 rounds, 6 / s)
    ASSERT_GT(w->combat().reload, 0);
    EXPECT_EQ(w->combat().clip[1][0] + w->combat().clip[1][1], 0);
    for (int i = 0; i < 60; ++i) w->tick(guns(false)); // reload 0.7 s: 80 rounds from the 100 spare
    EXPECT_EQ(w->combat().clip[1][0], 40);
    EXPECT_EQ(w->combat().clip[1][1], 40);
    EXPECT_EQ(w->combat().reserve[1], 20);
    w->tick(sw);
    for (int i = 0; i < 90; ++i) w->tick(guns(false));
    EXPECT_EQ(w->combat().weapon, game::Weapon::Pistols);
    EXPECT_EQ(w->combat().clip[1][0], 40); // and the uzi keeps its own
    EXPECT_EQ(w->combat().reserve[1], 20);
}

// Mjolnir: Change Weapon cycles pistols -> secondary -> hammer (tru.exe 0x704930); fire swings and the bolt leaves at
// frame 10 of the swing (1000 damage: a 700-hp target dies), no clip or reload; target lock + interact = the ground pound,
// which hits targets by its two decoded rings at frame 12 (0x6E68B0) and spares those beyond 3000.
TEST(Combat, MjolnirCycleSwingAndGroundPound) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    w->giveWeapon(game::Weapon::Uzi, 100);
    EXPECT_FALSE(w->hasHammer());
    w->giveWeapon(game::Weapon::ThorHammer, 0);
    EXPECT_TRUE(w->hasHammer());
    EXPECT_EQ(w->combat().secondary, game::Weapon::Uzi); // her own slot: the secondary stays
    Actions sw;
    sw.weapon = true;
    for (const auto want : {game::Weapon::Uzi, game::Weapon::ThorHammer, game::Weapon::Pistols, game::Weapon::Uzi, game::Weapon::ThorHammer}) {
        w->tick(sw);
        EXPECT_EQ(w->combat().want, want);
        w->tick({});
    }
    game::DummyTarget t;
    t.pos = {1500, 0, 0};
    w->targets.push_back(&t);
    Actions draw = guns(false);
    draw.draw = true;
    w->tick(draw);
    for (int i = 0; i < 70; ++i) w->tick(guns(false)); // equip 26/30 s
    ASSERT_EQ(w->combat().weapon, game::Weapon::ThorHammer);
    ASSERT_EQ(w->combat().drawT, 1.0f);
    w->tick(guns(true));
    EXPECT_EQ(w->combat().hammer.clip, "ground_fire_idle");
    int shotAt = -1;
    for (int i = 1; i <= 30 && shotAt < 0; ++i) {
        w->tick(guns(false));
        if (!w->combat().shots.empty()) shotAt = i;
    }
    EXPECT_NEAR(shotAt, 20, 1); // frame 10 at 30 Hz
    EXPECT_EQ(t.hits, 1);
    EXPECT_FALSE(t.alive());
    EXPECT_EQ(w->combat().clip[size_t(game::Weapon::ThorHammer)][0], -1); // no clip, no reload
    EXPECT_EQ(w->combat().reload, 0);
    for (int i = 0; i < 60; ++i) w->tick(guns(false));
    EXPECT_TRUE(w->combat().hammer.clip.empty()); // the swing (29/30 s) is over

    game::DummyTarget nearT, midT, farT; // rings (game::kPoundRings): 1200 inside 500; 400 at 500 .. 100 at 3000
    nearT.pos = {300, 0, 0}, midT.pos = {0, 1500, 0}, farT.pos = {-3500, 0, 0};
    nearT.hp = midT.hp = farT.hp = 1e6f;
    w->targets = {&nearT, &midT, &farT};
    for (int i = 0; i < 10; ++i) w->tick(guns(false)); // lock on
    ASSERT_TRUE(w->combat().locked);
    Actions pound = guns(false);
    pound.interact = true;
    w->tick(pound);
    EXPECT_EQ(w->combat().hammer.clip, "ground_melee_idle");
    for (int i = 0; i < 22; ++i) w->tick(guns(false));
    EXPECT_EQ(nearT.hits, 0);
    for (int i = 0; i < 3; ++i) w->tick(guns(false)); // frame 12
    EXPECT_EQ(nearT.hits, 1);
    EXPECT_FLOAT_EQ(1e6f - nearT.hp, 1200);
    EXPECT_EQ(midT.hits, 1);
    EXPECT_NEAR(1e6f - midT.hp, 280, 0.5f); // 1000 of 2500 into the ring
    EXPECT_EQ(farT.hits, 0);
    w->targets.clear();
}

TEST(Combat, ShotgunPelletsInTheCone) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {1000, 0, 0}, t.hp = 1e9f, t.r = 300; // wide enough for the whole cone
    w->targets.push_back(&t);
    w->giveWeapon(game::Weapon::Shotgun, 10);
    Actions sw;
    sw.weapon = true;
    w->tick(sw);
    EXPECT_EQ(w->combat().weapon, game::Weapon::Shotgun); // holstered: switches at once
    for (int i = 0; i < 120 && w->combat().shots.empty(); ++i) w->tick(guns(true));
    const auto& c = w->combat();
    ASSERT_EQ(c.shots.size(), 7u); // one trigger pull, 7 pellets (wp_shotgun S+5)
    EXPECT_EQ(c.shotsFired, 1u);
    EXPECT_EQ(c.clip[2][0], 5);
    int hits = 0;
    for (const auto& s : c.shots) {
        const vec3 d = glm::normalize(s.to - s.from), aim = glm::normalize(c.aimPoint - s.from);
        EXPECT_LE(glm::degrees(std::acos(std::min(1.0f, glm::dot(d, aim)))), 7.5f * 1.4143f); // within +-7.5 deg yaw and pitch
        hits += s.target == &t;
    }
    EXPECT_EQ(t.hits, hits);
    EXPECT_EQ(hits, 7);
    // next shot only after the fire clip + pump (38/30 s)
    int ticks = 0;
    for (; ticks < 120 && w->combat().shotsFired == 1; ++ticks) w->tick(guns(true));
    EXPECT_NEAR(ticks, 76, 2);
}

TEST(Combat, HitReactionPlaysAndReturns) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.build();
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    anim::Clip still, hit;
    still.keyCount = hit.keyCount = 10, still.timePerKey = hit.timePerKey = 100; // 1/3 s
    still.tracks.resize(1), hit.tracks.resize(1);
    for (int i = 0; i < 10; ++i) hit.tracks[0][anim::TransY].push_back(10.0f * float(i)); // pushed back (+Y model = backward)
    a.clips["ng_idle"] = still;
    for (const char* n : {"hit_light_front", "hit_light_back", "hit_light_side", "hit_mediumfront", "hit_mediumback", "hit_mediumright"}) a.clips[n] = hit;
    game::World w(std::move(m), std::move(a), game::Spawn{{0, 0, 0}, 0}); // facing +X
    w.tick(guns(true)); // draws the pistols
    w.damageLara(100, {500, 0, 0}, game::kDamageLight); // in front
    EXPECT_EQ(w.player().state().state, State::Hit);
    EXPECT_EQ(w.player().state().clip, "hit_light_front");
    w.damageLara(100, {500, 0, 0}, game::kDamageLight); // ignored inside the invulnerability window: no restart
    EXPECT_EQ(w.health().value(), 900);
    for (int i = 0; i < 10; ++i) w.tick(guns(false));
    EXPECT_EQ(w.player().state().state, State::Hit);
    EXPECT_TRUE(w.combat().drawn); // the guns stay out
    for (int i = 0; i < 20; ++i) w.tick({});
    EXPECT_EQ(w.player().state().state, State::Idle);
    EXPECT_NEAR(w.player().state().pos.x, -90, 1); // root motion: 90 units back
    for (int i = 0; i < 60; ++i) w.tick({});
    w.damageLara(100, {-500, 0, 0}, game::kDamageHeavy); // behind, heavy
    EXPECT_EQ(w.player().state().clip, "hit_mediumback");
    for (int i = 0; i < 90; ++i) w.tick({});
    w.damageLara(100, w.player().state().pos + vec3(0, -500, 0), game::kDamageLight); // her right
    EXPECT_EQ(w.player().state().clip, "hit_light_side");
    for (int i = 0; i < 90; ++i) w.tick({});
    w.damageLara(100, {}, game::kDamageBypass); // other damage types: no reaction
    EXPECT_EQ(w.player().state().state, State::Idle);
    EXPECT_EQ(game::stateName(State::Hit), "hit");
}

TEST(Combat, AdrenalineHeadShot) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {900, 0, 0}, t.hp = 5000;
    w->targets.push_back(&t);
    for (int i = 0; i < 60 * 15 && w->combat().adrenaline < 1; ++i) w->tick(guns(true)); // fill by damage dealt
    ASSERT_EQ(w->combat().adrenaline, 1);
    EXPECT_GE(5000 - t.hp, 500); // Lara data +0x2D0: 500 damage dealt fills it, plus what drained (20/s) meanwhile
    EXPECT_LE(5000 - t.hp, 700);
    Actions lock = guns(false), go = guns(false);
    go.interact = true;
    w->tick(lock);
    w->tick(go); // meter + locked + interact: adrenaline mode
    w->tick(lock);
    ASSERT_TRUE(w->combat().adrenalineMode);
    w->tick(go); // in the mode + close + interact: the window opens
    ASSERT_GT(w->combat().slowMo, 0);
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_NEAR(w->combat().timeScale, 0.2f, 1e-4); // +0x2C8, eased down in +0x2CC = 10 frames
    const vec3 at = w->player().state().pos;
    for (int i = 0; i < 120 && glm::length(w->combat().pip) > 0.3f; ++i) { // steer the pip into the reticle
        Actions steer = lock;
        steer.move = -glm::normalize(w->combat().pip);
        w->tick(steer);
    }
    EXPECT_EQ(w->player().state().pos, at); // the stick steers the pip, not Lara
    ASSERT_LE(glm::length(w->combat().pip), 1);
    const float hp = t.hp;
    Actions shoot = lock;
    shoot.fire = true;
    w->tick(shoot);
    EXPECT_FALSE(t.alive()); // head shot: damage = its remaining health
    ASSERT_FALSE(w->combat().shots.empty());
    EXPECT_EQ(w->combat().shots.back().damage, hp);
    EXPECT_EQ(w->combat().slowMo, 0);
    EXPECT_EQ(w->combat().adrenaline, 0);
    for (int i = 0; i < 21; ++i) w->tick({});
    EXPECT_EQ(w->combat().timeScale, 1); // time scale restored
}

// Adrenaline mode (0x718BD0): meter not empty + lock + interact toggles it; world time 0.5 over 10 frames, the meter
// drains 225 / s, the mode ends with it.
TEST(Combat, AdrenalineModeDrainsAndSlowsTime) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {3000, 0, 0}, t.hp = 1e6f; // out of the close range: no head shot
    w->targets.push_back(&t);
    for (int i = 0; i < 60 * 6; ++i) w->tick(guns(true));
    const float meter = w->combat().adrenaline;
    ASSERT_GT(meter, 0.5f);
    Actions lock = guns(false), go = guns(false);
    go.interact = true;
    w->tick(lock);
    w->tick(go);
    ASSERT_TRUE(w->combat().adrenalineMode);
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_NEAR(w->combat().timeScale, 0.5f, 1e-4);
    const float before = w->combat().adrenaline;
    for (int i = 0; i < 60; ++i) w->tick(lock);
    EXPECT_NEAR(before - w->combat().adrenaline, 225.0f / 500.0f, 0.01f);
    for (int i = 0; i < 60 * 4 && w->combat().adrenalineMode; ++i) w->tick(lock);
    EXPECT_FALSE(w->combat().adrenalineMode);
    EXPECT_EQ(w->combat().adrenaline, 0);
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().timeScale, 1);
}

TEST(Combat, ReplayWithSwitchingIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 aim 1\n0.1 fire 1\n1.0 weapon 1\n1.1 weapon 0\n2.5 right 1\n3.0 weapon 1\n"
                                                 "3.1 weapon 0\n3.5 right 0\n4.0 heal 1\n4.1 heal 0\n");
    auto run = [&](double hz) {
        CollisionMesh m;
        addFloor(m, -5000, -5000, 5000, 5000, 0);
        auto w = makeWorld(m, {0, 0, 0});
        game::DummyTarget t;
        t.pos = {2000, 600, 0}, t.hp = 1e6f;
        w->targets.push_back(&t);
        w->giveWeapon(game::Weapon::Shotgun, 20);
        w->found[2] = 1;
        w->damageLara(300, {}, game::kDamageBypass);
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 300)
            for (int n = clock.advance(1.0 / hz); n > 0 && w->ticks() < 300; --n) w->tick(script.at(w->ticks()));
        w->targets.clear();
        const auto& c = w->combat();
        return std::tuple(c.shotsFired, t.hp, c.rng, w->player().state().pos, c.weapon, c.clip[2][0], c.reserve[2], c.clip[0][0],
                          w->health().value(), w->found[2]);
    };
    const auto a = run(30), b = run(60), c = run(144);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_EQ(std::get<4>(a), game::Weapon::Pistols); // switched to the shotgun and back
    EXPECT_LT(std::get<5>(a), 6);                     // the shotgun fired
    EXPECT_EQ(std::get<9>(a), 0);                     // the health pack was used
}

TEST(Combat, InputScriptCombatActions) {
    const auto s = game::InputScript::parse("0 draw\n0.1 draw 0\n0.2 aim 1\n0.3 fire 1\n0.4 switch 1\n0.5 weapon 1\n0.6 heal 1\n");
    EXPECT_TRUE(s.at(0).draw);
    EXPECT_FALSE(s.at(6).draw);
    EXPECT_TRUE(s.at(12).aim);
    EXPECT_TRUE(s.at(18).fire);
    EXPECT_TRUE(s.at(24).switchTarget);
    EXPECT_FALSE(s.at(29).weapon);
    EXPECT_TRUE(s.at(30).weapon);
    EXPECT_TRUE(s.at(36).heal);
}

// ---- real data (OU_GAME_DIR) --------------------------------------------------------------------
// The ground pound's rings (game::kPoundRings) as lara.drm holds them: section 850, attack record +0x35EB8 (class 13,
// 2 rings at +0x24 -> +0x399F0, 0x1C bytes each).
TEST(CombatReal, GroundPoundRings) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\lara.drm"));
    const auto s = std::find_if(drm.sections.begin(), drm.sections.end(), [](const cdc::Section& x) { return x.id == 850; });
    ASSERT_NE(s, drm.sections.end());
    const auto u32 = [&](uint32_t o) { uint32_t v; std::memcpy(&v, s->data.data() + o, 4); return v; };
    constexpr uint32_t kRecord = 0x35EB8, kRings = 0x399F0;
    EXPECT_EQ(u32(kRecord + 0xC), 13u);
    EXPECT_EQ(u32(kRecord + 0x20), std::size(game::kPoundRings));
    EXPECT_EQ(cdc::internalPointer(*s, kRecord + 0x24), kRings);
    for (size_t i = 0; i < std::size(game::kPoundRings); ++i) {
        game::PoundRing r;
        std::memcpy(&r, s->data.data() + kRings + 0x1C * i, sizeof r);
        const auto& want = game::kPoundRings[i];
        EXPECT_EQ(std::memcmp(&r, &want, sizeof r), 0) << "ring " << i;
    }
}

TEST(CombatReal, WeaponData) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    for (int i = 0; i < game::kWeaponCount; ++i) { // decoded = the documented defaults
        const auto wpn = game::Weapon(i);
        const auto p = game::loadWeapon(bf, game::weaponObject(wpn));
        ASSERT_TRUE(p) << game::weaponName(wpn);
        const game::WeaponDef def = game::weaponDefaults(wpn);
        SCOPED_TRACE(def.object);
        EXPECT_EQ(p->ammo, def.ammo);
        EXPECT_EQ(p->ammoSlot, def.ammoSlot);
        EXPECT_EQ(p->maxReserve, def.maxReserve);
        EXPECT_EQ(p->clip, def.clip);
        EXPECT_EQ(p->pellets, def.pellets);
        EXPECT_EQ(p->infinite, def.infinite);
        EXPECT_EQ(p->dual, def.dual);
        EXPECT_EQ(p->mode, def.mode);
        EXPECT_EQ(p->shortRange, def.shortRange);
        EXPECT_EQ(p->maxRange, def.maxRange);
        EXPECT_EQ(p->spreadDeg, def.spreadDeg);
        EXPECT_EQ(p->maxSpreadDeg, def.maxSpreadDeg);
        EXPECT_EQ(p->spreadPerShot, def.spreadPerShot);
        EXPECT_FLOAT_EQ(p->spreadRecovery, def.spreadRecovery);
        for (int k = 0; k < 3; ++k) EXPECT_EQ(p->damage[k], def.damage[k]);
        for (int k = 0; k < 3; ++k) EXPECT_EQ(p->wetDamage[k], def.wetDamage[k]);
        EXPECT_EQ(p->hitClass, def.hitClass);
        EXPECT_EQ(p->hitType, def.hitType);
        EXPECT_EQ(p->hand, def.hand);
        EXPECT_EQ(p->stow, def.stow);
        EXPECT_EQ(p->muzzleMarker, def.muzzleMarker);
    }
    // Lara's clips of every weapon load (loadLaraAnims throws on a missing one), with the lengths Combat.cpp times by
    const auto anims = game::loadLaraAnims(bf);
    for (int i = 0; i < game::kWeaponCount; ++i)
        for (const auto n : game::weaponClipNames(game::Weapon(i))) EXPECT_TRUE(anims.weaponClip(game::Weapon(i), n)) << n;
    const auto len = [&](game::Weapon w, const char* n) { return anims.weaponClip(w, n)->durationSeconds(); };
    EXPECT_EQ(len(game::Weapon::Pistols, "combat_pistol_RfireE"), 10.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Pistols, "combat_pistols_equip"), 6.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Uzi, "combat_pistols_equip"), 15.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Uzi, "combat_pistols_reload"), 21.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Shotgun, "combat_shotgun_fire_F"), 13.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Shotgun, "combat_shotgun_pump"), 25.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Shotgun, "combat_shotgun_reload"), 117.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Shotgun, "combat_shotgun_equip"), 24.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Speargun, "combat_speargun_reload"), 31.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Speargun, "combat_shotgun_equip_submerged"), 17.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Speargun, "combat_shotgun_unequip_submerged"), 19.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::ThorHammer, "ground_equip"), 26.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::ThorHammer, "ground_fire_idle"), 29.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::ThorHammer, "ground_melee_idle"), 61.0 / 30.0);
    for (const char* n : {"hit_light_front", "hit_mediumback", "hit_light_side_swim"}) EXPECT_TRUE(anims.find(n)) << n;
    // Mount markers on Lara: every weapon's exist; stowed rifles across her back (15, chest segment), Mjolnir's 31 as
    // measured before (R = Rx Ry Rz of its Euler angles).
    for (int i = 0; i < game::kWeaponCount; ++i)
        for (const int id : {anims.weapons[size_t(i)].hand[0], anims.weapons[size_t(i)].stow[0]}) EXPECT_TRUE(anims.markers.contains(id)) << id;
    EXPECT_EQ(anims.markers.at(15).first, 15);
    const auto& [seg, m] = anims.markers.at(31);
    EXPECT_EQ(seg, 15);
    EXPECT_NEAR(m[0].y, 0.99f, 0.01f);
    EXPECT_NEAR(m[1].x, -0.98f, 0.01f);
    EXPECT_NEAR(m[3].y, 48.2f, 0.1f);
}

// l4_pool, on the stairs below the player intro (floor z 4721.2): a 1800-unit drop hurts, 2900 kills, the death
// clip plays and she respawns; then pistols lock onto a dummy down the stairs and hit it.
TEST(CombatReal, PoolFallDamageDeathAndPistols) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto anims = game::loadLaraAnims(bf);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const float floorZ = 4721.2f;
    {
        game::World w(loader, "l4_pool", anims, game::Spawn{{-14885, -42, floorZ + 1800}, 0});
        for (int i = 0; i < 120; ++i) w.tick({});
        EXPECT_NEAR(w.player().state().pos.z, floorZ, 1);
        EXPECT_NEAR(w.health().value(), 1000 - 500 * (1800 - 1280.03f) / (1817.32f - 1280.03f), 2);
        EXPECT_NE(w.player().state().state, State::Dead);
    }
    game::World w(loader, "l4_pool", anims, game::Spawn{{-14885, -42, floorZ + 2900}, 0});
    int diedAt = -1, respawnedAt = -1;
    for (int i = 0; i < 300 && respawnedAt < 0; ++i) {
        w.tick({});
        const bool dead = w.player().state().state == State::Dead;
        if (dead && diedAt < 0) {
            diedAt = i;
            EXPECT_EQ(w.player().state().clip, "death_fall_normal");
            EXPECT_NEAR(w.player().state().pos.z, floorZ, 1);
        }
        if (!dead && diedAt >= 0) respawnedAt = i;
    }
    ASSERT_GE(diedAt, 0);
    ASSERT_GE(respawnedAt, 0);
    // DeathState: the screen fades to black over 90 frames, then the checkpoint reloads (3 s after the death)
    EXPECT_NEAR(respawnedAt - diedAt, 180, 2);
    EXPECT_EQ(w.health().value(), 1000);

    game::World p(loader, "l4_pool", anims);
    game::DummyTarget t;
    t.pos = {-13385, -600, 4500};
    p.targets.push_back(&t);
    for (int i = 0; i < 120; ++i) p.tick(guns(i >= 12));
    EXPECT_EQ(p.combat().target, &t);
    EXPECT_TRUE(p.combat().locked);
    EXPECT_GE(p.combat().shotsFired, 4u);
    EXPECT_EQ(t.hits, int(p.combat().shotsFired));
    EXPECT_LT(t.hp, 700 - 4 * 30);
    // the aim layer put the hands at shoulder height, pointing at the target
    const auto pose = p.pose(1);
    const auto& s = p.player().state();
    const glm::vec3 hand(p.player().modelMatrix(s.pos, s.yaw) * pose[80][3]);
    EXPECT_NEAR(hand.z - s.pos.z, 400, 60);
    const glm::vec3 toTarget = glm::normalize(t.aimPoint() - (s.pos + vec3(0, 0, 418)));
    EXPECT_GT(glm::dot(glm::normalize(hand - (s.pos + vec3(0, 0, 418))), toTarget), 0.8f);
}

// l4_pool's goo is water (surface 2699.3): under water the pistols stay holstered, the speargun draws and fires, and
// each shot is followed by a reload from the reserve.
TEST(CombatReal, PoolSpeargunUnderWater) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l4_pool", game::loadLaraAnims(bf), game::Spawn{{-4000, 0, 2000}, 0});
    w.tick({});
    Actions dive;
    dive.drop = true;
    w.tick(dive); // surfacing -> under water
    for (int i = 0; i < 10; ++i) w.tick({});
    ASSERT_EQ(w.player().state().state, State::SwimUnder);
    for (int i = 0; i < 60; ++i) w.tick(guns(true)); // pistols: holstered under water
    EXPECT_FALSE(w.combat().drawn);
    EXPECT_EQ(w.combat().shotsFired, 0u);
    game::DummyTarget t;
    t.pos = w.player().state().pos + vec3(900, 0, -250);
    w.targets.push_back(&t);
    w.giveWeapon(game::Weapon::Speargun, 5);
    Actions sw;
    sw.weapon = true;
    w.tick(sw);
    EXPECT_EQ(w.combat().weapon, game::Weapon::Speargun);
    for (int i = 0; i < 150; ++i) w.tick(guns(true)); // equip 17/30 s, then shot, reload 31/30 s, shot
    EXPECT_TRUE(w.combat().drawn);
    EXPECT_EQ(w.player().state().state, State::SwimUnder);
    EXPECT_EQ(w.combat().shotsFired, 2u);
    EXPECT_EQ(t.hits, 2);
    EXPECT_EQ(t.hp, 700 - 2 * 550); // under water the +0x84 record: 550 a spear
    EXPECT_LE(w.combat().reserve[3], 4);
    w.targets.clear();
}

// ---- gadgets (src/game/Gadgets.cpp) -------------------------------------------------------------
TEST(Gadgets, FlashlightToggleCooldownAndPose) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    Actions light;
    light.light = true;
    w->tick(light); // held from the first tick: no press edge
    EXPECT_FALSE(w->flashlight);
    w->tick({});
    w->tick(light);
    EXPECT_TRUE(w->flashlight);
    for (int i = 0; i < 10; ++i) w->tick(light); // holding does not toggle again
    EXPECT_TRUE(w->flashlight);
    w->tick({});
    w->tick(light);
    EXPECT_FALSE(w->flashlight);
    w->tick({});
    w->tick(light); // within the 4-frame cooldown
    EXPECT_FALSE(w->flashlight);
    for (int i = 0; i < 10; ++i) w->tick({});
    w->tick(light);
    EXPECT_TRUE(w->flashlight);
    EXPECT_FALSE(w->flashlightSpot()); // no unit: omni only
    const auto [pos, dir] = w->flashlightPose(1);
    EXPECT_GT(dir.x, 0.99f); // facing +X
    EXPECT_NEAR(pos.x, 25, 1e-3);
}

// PLAYER TAILORING presets: Lara's max health keeps her health fraction, ammunition capacity caps reserves and grenades.
TEST(Tailoring, PresetsScaleHealthAndCapacity) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    EXPECT_EQ(w->tailoring().preset(), 1);
    w->giveWeapon(game::Weapon::Uzi, 1000);
    EXPECT_EQ(w->combat().reserve[size_t(game::Weapon::Uzi)], 240);
    w->setHealth(500);
    w->setTailoring(game::Tailoring::preset(0)); // casual: 1.3x health, extra ammunition
    EXPECT_FLOAT_EQ(w->health().max(), 1300);
    EXPECT_FLOAT_EQ(w->health().value(), 650);
    EXPECT_EQ(w->maxReserve(game::Weapon::Uzi), 400);
    EXPECT_EQ(w->maxGrenades(), 10);
    w->setTailoring(game::Tailoring::preset(2)); // master survivalist: 0.8x, less ammunition (excess dropped)
    EXPECT_FLOAT_EQ(w->health().max(), 800);
    EXPECT_EQ(w->combat().reserve[size_t(game::Weapon::Uzi)], 160);
    EXPECT_EQ(w->grenades, 4);
    EXPECT_FLOAT_EQ(w->player().tuning().savingGrabSeconds, 0.5f);
    game::Tailoring custom;
    custom.ammo = 0;
    EXPECT_EQ(custom.preset(), -1);
}

// A grenade thrown at a locked dummy 800 ahead sticks to it, goes off after the fuse, kills it and hurts Lara.
TEST(Gadgets, GrenadeKillsDummyAndHurtsLara) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    const auto& g = w->anims().grenade;
    ASSERT_EQ(w->grenades, w->maxGrenades());
    ASSERT_EQ(w->maxGrenades(), g.maxCarried[1]);
    game::DummyTarget t;
    t.pos = {800, 0, 0};
    w->targets.push_back(&t);
    Actions draw;
    draw.draw = true;
    w->tick(draw);
    w->tick({});
    for (int i = 0; i < 30; ++i) w->tick(guns(false)); // aim locks
    ASSERT_EQ(w->combat().target, &t);
    Actions throwIt = guns(false);
    throwIt.grenade = true;
    w->tick(throwIt);
    int exploded = -1;
    for (int i = 0; i < 600 && exploded < 0; ++i) {
        w->tick(guns(false));
        if (!w->explosions().empty()) exploded = i;
        if (!w->liveGrenades().empty() && w->liveGrenades()[0].stuck) EXPECT_EQ(w->liveGrenades()[0].target, &t);
    }
    ASSERT_GE(exploded, 0);
    EXPECT_EQ(w->grenades, g.maxCarried[1] - 1);
    EXPECT_NEAR(exploded / 60.0, g.fuse + 0.3, 0.4); // release + flight, then the fuse
    EXPECT_FALSE(t.alive());
    EXPECT_LT(w->health().value(), 1000);
    EXPECT_GT(w->health().value(), 1000 - g.damage);
    EXPECT_TRUE(w->liveGrenades().empty());
    w->targets.clear();
}

TEST(GadgetsReal, TuningLightsAndSpotUnits) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto g = game::loadGrenadeTuning(bf);
    ASSERT_TRUE(g);
    EXPECT_EQ(g->maxCarried, (std::array<int, 3>{10, 6, 4}));
    EXPECT_FLOAT_EQ(g->cooldown, 20 / 30.0f);
    EXPECT_FLOAT_EQ(g->minRange, 200);
    EXPECT_FLOAT_EQ(g->maxRange, 1000);
    EXPECT_FLOAT_EQ(g->damage, 1000);
    EXPECT_FLOAT_EQ(g->radius, 1100);
    EXPECT_FLOAT_EQ(g->edgeDamage, 200);
    EXPECT_FLOAT_EQ(g->fuse, 4);
    auto b = bf.read("pc-w\\g_personal_light.drm");
    ASSERT_TRUE(b);
    const auto lights = formats::objectLights(cdc::parseDrm(std::move(*b)));
    ASSERT_EQ(lights.size(), 2u);
    const auto spot = std::find_if(lights.begin(), lights.end(), [](const auto& l) { return l.type == 1; });
    const auto omni = std::find_if(lights.begin(), lights.end(), [](const auto& l) { return l.type == 0; });
    ASSERT_NE(spot, lights.end());
    ASSERT_NE(omni, lights.end());
    EXPECT_FLOAT_EQ(spot->radius, 5000);
    EXPECT_FLOAT_EQ(omni->radius, 1800);
    EXPECT_TRUE(spot->shadow); // DTP +0x144C: the spot is a shadow light (RT local-light shadows trace it), the omni not
    EXPECT_FALSE(omni->shadow);
    EXPECT_TRUE(game::loadUnitData(bf, "l4_pool").flashlightSpot);
    EXPECT_FALSE(game::loadUnitData(bf, "l3_main_bedroom").flashlightSpot);
    EXPECT_TRUE(game::loadLaraAnims(bf).find("g_grenade_thrower:combat_grenade_Throw1"));
}

// The assault rifle (wp_assault_rifle) and tranquilizer (wp_tranq): Lara's combat_rifle_* clips load with the lengths
// Combat.cpp times by; chapter secondaries follow the units' weapon objects.
TEST(CombatReal, RifleAndTranquilizerClips) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto anims = game::loadLaraAnims(bf);
    const auto len = [&](game::Weapon w, const char* n) { return anims.weaponClip(w, n)->durationSeconds(); };
    for (const auto w : {game::Weapon::AssaultRifle, game::Weapon::Tranquilizer}) {
        EXPECT_EQ(len(w, "combat_rifle_fire_F"), 3.0 / 30.0);
        EXPECT_EQ(len(w, "combat_rifle_equip"), 19.0 / 30.0);
        EXPECT_EQ(len(w, "combat_rifle_unequip"), 24.0 / 30.0);
    }
    EXPECT_EQ(len(game::Weapon::AssaultRifle, "combat_rifle_reload"), 41.0 / 30.0);
    EXPECT_EQ(len(game::Weapon::Tranquilizer, "combat_tranqgun_reload"), 19.0 / 30.0);
    EXPECT_EQ(game::ammoWeapon("wp_akrifle"), game::Weapon::AssaultRifle);
    const auto akrifle = game::loadWeapon(bf, "wp_akrifle");
    ASSERT_TRUE(akrifle);
    EXPECT_EQ(akrifle->ammoSlot, anims.weapons[size_t(game::Weapon::AssaultRifle)].ammoSlot); // the same ammunition
    EXPECT_EQ(game::chapterSecondary(4), game::Weapon::Tranquilizer);
    EXPECT_EQ(game::chapterSecondary(6), game::Weapon::AssaultRifle);
}

// l4_pool: the rifle empties its 30-round clip at one shot per 3 frames (27 damage within its 5000 short range), then
// reloads from the reserve; tranquilizer darts put a real panther to sleep (sleep_start / sleep_loop) without hurting
// it, and it wakes after the nap.
TEST(CombatReal, PoolRifleAndTranquilizer) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    {
        game::World w(loader, "l4_pool", anims);
        game::DummyTarget t;
        t.hp = 1e6f;
        t.pos = {-13385, -600, 4500};
        w.targets.push_back(&t);
        w.giveWeapon(game::Weapon::AssaultRifle, 300);
        Actions sw;
        sw.weapon = true;
        w.tick(sw);
        EXPECT_EQ(w.combat().want, game::Weapon::AssaultRifle);
        for (int i = 0; i < 400; ++i) w.tick(guns(i >= 12));
        const auto r = size_t(game::Weapon::AssaultRifle);
        EXPECT_GE(w.combat().shotsFired, 30u); // a full clip, the reload (41 frames), then more
        EXPECT_EQ(w.combat().reserve[r], 210 - 30); // Normal max reserve 210, one clip moved in
        EXPECT_GT(t.hits, 20);
        EXPECT_FLOAT_EQ(1e6f - t.hp, 27.0f * float(t.hits));
        w.targets.clear();
    }
    game::World w(loader, "l4_pool", anims);
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    ASSERT_TRUE(panther->sleeps);
    const vec3 lara = w.player().state().pos;
    auto& cat = w.enemies().add(panther, 990001, lara + vec3(-1500, -600, 0), 0);
    const float full = cat.health();
    w.giveWeapon(game::Weapon::Tranquilizer, 15);
    Actions sw;
    sw.weapon = true;
    w.tick(sw);
    int slept = -1;
    for (int i = 0; i < 600 && slept < 0; ++i) {
        w.tick(guns(true));
        if (cat.state().mode == game::ai::Mode::Sleep) slept = i;
    }
    ASSERT_GE(slept, 0) << "mode " << game::ai::modeName(cat.state().mode);
    EXPECT_EQ(cat.health(), full);
    EXPECT_GE(w.combat().shotsFired, 2u); // 500 per dart against 700
    int woke = -1;
    for (int i = 0; i < 60 * 40 && woke < 0; ++i) {
        w.tick({});
        if (i == 600) EXPECT_EQ(cat.state().clip, "sleep_loop");
        if (cat.state().mode != game::ai::Mode::Sleep) woke = i;
    }
    EXPECT_GT(woke, 60 * 19);
}


// Shots and their flashes come from the gun model riding Lara's grip marker (7 right / 5 left; two-handed 5) with the
// marker's frame, at the gun's shot origin (weapon data S+0x14: pistols HInfo marker -10 at the barrel tip, barrel
// -Y; shotgun segment 0). Each Shot carries the gun's world frame, where its "fire" events (flashes) play.
TEST(CombatReal, MuzzleAtGunMarkers) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    const auto near = [](vec3 a, vec3 b) { return glm::distance(a, b) < 0.1f; };
    const auto& pistols = anims.weapons[size_t(game::Weapon::Pistols)];
    EXPECT_EQ(pistols.muzzleMarker, -10);
    EXPECT_PRED2(near, vec3(pistols.muzzle[3]), vec3(0, -71.9f, 24.6f));
    EXPECT_PRED2(near, vec3(pistols.muzzle[1]), vec3(0, 1, 0)); // the marker's axes are the model's: -Y the barrel
    const auto& shotgun = anims.weapons[size_t(game::Weapon::Shotgun)];
    EXPECT_EQ(shotgun.muzzleMarker, 0);
    EXPECT_PRED2(near, vec3(shotgun.muzzle[3]), vec3(0, 13.7f, -9.3f));
    EXPECT_EQ(anims.weapons[size_t(game::Weapon::Tranquilizer)].muzzleMarker, -15);
    for (const auto wp : {game::Weapon::Pistols, game::Weapon::Shotgun}) {
        SCOPED_TRACE(game::weaponName(wp));
        const auto& wd = anims.weapons[size_t(wp)];
        game::World w(loader, "l4_pool", anims);
        game::DummyTarget t;
        t.hp = 1e9f;
        t.pos = {-13385, -600, 4500};
        w.targets.push_back(&t);
        if (wp != game::Weapon::Pistols) {
            w.giveWeapon(wp, 100);
            Actions sw;
            sw.weapon = true;
            w.tick(sw);
        }
        std::set<int> hands;
        for (int i = 0; i < 300; ++i) {
            w.tick(guns(i >= 12));
            for (const auto& s : w.combat().shots) {
                hands.insert(s.hand);
                const auto mount = w.gunMount(wp, s.hand);
                ASSERT_TRUE(mount);
                EXPECT_EQ(*mount, anims.markers.at(wd.hand[size_t(s.hand)]));
                EXPECT_PRED2(near, s.from, vec3(s.gun * wd.muzzle[3]));
                // the barrel points at the target (within the aim grid's fit: pistols ~10, shotgun ~20 degrees)
                EXPECT_GT(glm::dot(glm::normalize(vec3(-s.gun[1])), glm::normalize(t.aimPoint() - s.from)), std::cos(glm::radians(30.0f)));
            }
        }
        EXPECT_EQ(hands.size(), wp == game::Weapon::Pistols ? 2u : 1u);
    }
}

// Decoded numbers (docs/GAMEPLAY.md "Combat"): fire rates from the clips carrying the shot event 0x75, the projectiles'
// speed / gravity / damage / class 4, the gun-to-hand / stow / Mjolnir event frames, Lara's adrenaline data.
TEST(CombatReal, DecodedCombatNumbers) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    using game::Weapon;
    for (int i = 0; i < game::kWeaponCount; ++i) {
        const auto p = game::loadWeapon(bf, game::weaponObject(Weapon(i)));
        ASSERT_TRUE(p);
        const auto d = game::weaponDefaults(Weapon(i));
        SCOPED_TRACE(d.object);
        EXPECT_FLOAT_EQ(p->refire, d.refire);
        EXPECT_FLOAT_EQ(p->projectileSpeed, d.projectileSpeed);
        EXPECT_FLOAT_EQ(p->projectileGravity[0], d.projectileGravity[0]);
        EXPECT_FLOAT_EQ(p->projectileGravity[1], d.projectileGravity[1]);
    }
    const auto spear = *game::loadWeapon(bf, "wp_speargun"), dart = *game::loadWeapon(bf, "wp_tranq");
    EXPECT_EQ(spear.mode, 2);
    EXPECT_EQ(spear.hitClass, 4);
    EXPECT_EQ(spear.hitType, 0);
    EXPECT_EQ(spear.damage[0], 250.0f);
    EXPECT_EQ(spear.wetDamage[0], 550.0f);
    EXPECT_FLOAT_EQ(spear.projectileSpeed, 30000.0f);                       // S+0x10 = 1000 per frame
    EXPECT_FLOAT_EQ(spear.projectileGravity[0], 1800.0f);                   // record +0x4C = -2 per frame^2
    EXPECT_FLOAT_EQ(spear.projectileGravity[1], 5400.0f);                   // +0x84 record: -6
    EXPECT_FLOAT_EQ(dart.projectileSpeed, 45000.0f);
    EXPECT_EQ(dart.hitClass, 4);
    const auto hg = *game::loadWeapon(bf, "wp_handgun"), uzi = *game::loadWeapon(bf, "wp_uzi"),
               sg = *game::loadWeapon(bf, "wp_shotgun"), ham = *game::loadWeapon(bf, "wp_thorhammer");
    EXPECT_EQ(hg.commandFrame("combat_pistol_RfireA", {0x75}), 0);
    EXPECT_EQ(hg.commandFrame("combat_pistols_equip", {0x76}), 3);
    EXPECT_EQ(hg.commandFrame("combat_pistols_unequip", {0x74}), 10);
    EXPECT_EQ(uzi.commandFrame("combat_pistols_equip", {0x76}), 7);
    EXPECT_EQ(uzi.commandFrame("combat_fire_overlay", {0x75}), 0);
    EXPECT_EQ(sg.commandFrame("combat_shotgun_equip", {0x76}), 5);
    EXPECT_EQ(sg.commandFrame("combat_shotgun_unequip", {0x74}), 10);
    EXPECT_EQ(hg.commandFrame("combat_pistol_motorcycle_arm", {0x76, 0x77, 0x78}), 15);
    EXPECT_EQ(ham.commandFrame("ground_fire_idle", {0x75}), 10);
    EXPECT_EQ(ham.commandFrame("swim_submerge_thorhammer_attack_front", {0x75}), 12);
    EXPECT_EQ(ham.commandFrame("ground_melee_idle", {0x4B}), 12);
    EXPECT_EQ(ham.commandFrame("ground_equip", {0x76}), 2);
    EXPECT_EQ(ham.commandFrame("ground_unequip", {0x74}), 14);
    const auto a = game::loadAdrenalineTuning(bf);
    ASSERT_TRUE(a);
    const game::AdrenalineTuning def;
    EXPECT_EQ(a->max, 500.0f);
    EXPECT_EQ(a->drain, 20.0f);
    EXPECT_EQ(a->modeDrain, 225.0f);
    EXPECT_EQ(a->modeScale, 0.5f);
    EXPECT_FLOAT_EQ(a->headShotScale, 0.2f);
    EXPECT_FLOAT_EQ(a->ease, 10 / 30.0f);
    EXPECT_EQ(a->max, def.max);
    EXPECT_EQ(a->modeScale, def.modeScale);
    EXPECT_FLOAT_EQ(a->ease, def.ease);
}

TEST(Combat, HitReactionThresholds) {
    EXPECT_EQ(game::hitReaction(1, 5), 1);
    EXPECT_EQ(game::hitReaction(2, 5), 2);
    EXPECT_EQ(game::hitReaction(6, 99), 0);
    EXPECT_EQ(game::hitReaction(6, 100), 1);
    EXPECT_EQ(game::hitReaction(6, 250), 2);
    EXPECT_EQ(game::hitReaction(3, 900), 0);
}

// ---- close combat and carried objects (src/game/Melee.cpp; docs/GAMEPLAY.md "Close combat", "Carried objects") ----
namespace {
Actions press(bool game::Actions::*key, game::vec2 move = {0, 0}) {
    Actions a;
    a.*key = true, a.move = move;
    return a;
}
} // namespace

// lara.drm's melee numbers decode to the defaults: the chart states' attack records, the target height classes, the
// melee slot and the hit windows of the kick clips.
TEST(CombatReal, MeleeDataDecodes) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto t = game::loadMeleeTuning(bf);
    ASSERT_TRUE(t);
    const game::MeleeTuning def;
    EXPECT_EQ(t->kick, (game::AttackRecord{60, 2}));
    EXPECT_EQ(t->sprintKick, (game::AttackRecord{3, 2}));
    EXPECT_EQ(t->fatality, (game::AttackRecord{100, game::kFinisherClass}));
    EXPECT_EQ(t->swing, (game::AttackRecord{100, 2}));
    EXPECT_EQ(t->vault, (game::AttackRecord{0, 1}));
    EXPECT_FLOAT_EQ(t->low, def.low); // 63.21 / 316.06 / 537.29: 0.2 / 1 / 1.7 m
    EXPECT_FLOAT_EQ(t->mid, def.mid);
    EXPECT_FLOAT_EQ(t->high, def.high);
    EXPECT_NEAR(t->mid, 316.056f, 0.01f);
    EXPECT_EQ(t->range, 640.0f);
    EXPECT_EQ(t->coneDeg, 90.0f);
    EXPECT_EQ(t->windows, def.windows);
    EXPECT_EQ(t->window("melee_kick_high"), (game::MeleeTuning::Window{3, 13}));
    EXPECT_EQ(t->window("melee_thrall_fatality"), (game::MeleeTuning::Window{23, 23}));
    const auto anims = game::loadLaraAnims(bf);
    for (const auto n : game::kMeleeClipNames) EXPECT_TRUE(anims.find(n)) << n;
    EXPECT_EQ(anims.melee.kick, def.kick);
}

// Interact next to a poacher kicks him (by his height: the highest kick), the blow lands in the clip's hit window for
// the kick record's 60 and knocks him off his feet; he gets up and fights on. Interact with nothing there kicks the air.
TEST(CombatReal, KickKnocksAPoacherDown) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    game::World w(loader, "l4_pool", anims);
    const auto poacher = game::ai::loadEnemyType(bf, "poacher");
    ASSERT_FALSE(poacher->knockFront.fall.empty());
    const vec3 lara = w.player().state().pos;
    const float yaw = w.player().state().yaw;
    const vec3 fwd(std::cos(yaw), std::sin(yaw), 0);
    auto& e = w.enemies().add(poacher, 990001, lara + fwd * 330.0f, yaw + 3.14159f);
    const float full = e.health();
    for (int i = 0; i < 3; ++i) w.tick({});
    ASSERT_EQ(w.meleeTarget(), &e);
    w.tick(press(&Actions::interact));
    EXPECT_EQ(w.player().state().state, State::Melee);
    // by his top over her feet: under 537.3 the high kick, else the highest
    const float top = e.position().z - w.player().state().pos.z + poacher->tuning.top;
    EXPECT_GT(top, anims.melee.mid);
    const std::string kick = top < anims.melee.high ? "melee_kick_high" : "melee_kick_highest";
    EXPECT_EQ(w.player().state().clip, kick) << top;
    EXPECT_EQ(w.melee().move, game::MeleeState::Move::Kick);
    int hitTick = -1, knocked = -1, up = -1, idle = -1;
    std::string fall;
    for (int i = 1; i < 60 * 8 && up < 0; ++i) {
        w.tick({});
        if (hitTick < 0 && e.health() < full) hitTick = i;
        if (knocked < 0 && e.state().mode == game::ai::Mode::Knocked) knocked = i, fall = e.state().clip;
        if (knocked >= 0 && e.state().mode != game::ai::Mode::Knocked) up = i;
        if (idle < 0 && w.player().state().state == State::Idle) idle = i;
    }
    // inside the clip's hit window (30 Hz frames; 60 Hz ticks)
    const auto win = anims.melee.window(kick);
    EXPECT_GE(hitTick, 2 * win.open - 1) << kick;
    EXPECT_LE(hitTick, 2 * win.close + 3);
    EXPECT_FLOAT_EQ(e.health(), full - 60.0f);
    EXPECT_EQ(knocked, hitTick);
    EXPECT_EQ(fall, poacher->knockFront.fall);
    EXPECT_GT(up, knocked + 30) << "the fall and the get-up clips";
    EXPECT_TRUE(e.alive());
    EXPECT_GT(idle, 30);
    EXPECT_LE(idle, 70) << "the kick clips last 0.93 / 1.03 s";
}

namespace {
vec3 introPos(const cdc::BigfileSet& bf, const std::string& unit, int32_t uid) {
    for (const auto& in : formats::parseUnit(*game::loadUnitData(bf, unit).drm).intros)
        if (in.uniqueId == uid) return {in.position[0], in.position[1], in.position[2]};
    return vec3(0);
}
vec3 objectPos(const game::World& w, int32_t uid) {
    const auto* o = w.script().find(uid);
    return o ? o->pos + o->offset : vec3(0);
}
// Ticks `n` times with `a`, then releases the keys for one tick.
void hold(game::World& w, Actions a, int n) {
    for (int i = 0; i < n; ++i) w.tick(a);
    w.tick({});
}

// Runs an autopilot plan (app/game/Autopilot.hpp: ordinary inputs) to its end; `each` after every tick.
bool runPlan(game::World& w, const std::string& plan, const std::function<void()>& each = {}) {
    auto pilot = app::Autopilot::parse(plan);
    const uint64_t start = w.ticks();
    while (!pilot.done() && w.ticks() - start < 60 * 120) {
        w.tick(pilot.step(w.ticks() - start, w));
        if (each) each();
    }
    EXPECT_FALSE(pilot.failed()) << pilot.failure();
    return pilot.done() && !pilot.failed();
}

// A synthetic library with the melee / knock-down clips as still clips of the originals' lengths (30 Hz keys).
std::unique_ptr<game::World> meleeWorld(std::initializer_list<std::pair<const char*, int>> clips) {
    CollisionMesh m;
    addFloor(m, -4000, -4000, 4000, 4000, 0);
    m.build();
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    anim::Clip still;
    still.keyCount = 20, still.timePerKey = 100, still.tracks.resize(1);
    a.clips["ng_idle"] = still;
    a.clips["ng_run"] = still;
    for (const auto& [name, keys] : clips) {
        anim::Clip c = still;
        c.keyCount = keys;
        a.clips[name] = c;
    }
    return std::make_unique<game::World>(std::move(m), std::move(a), game::Spawn{{0, 0, 0}, 0});
}
} // namespace

// Synthetic: interact by a target kicks it by its height class; the kick record's 60 lands once, inside the clip's hit
// window, and only on a target within her reach. With nothing there she kicks the air; without the clips nothing happens.
TEST(Combat, KickLandsInItsWindow) {
    auto w = meleeWorld({{"melee_kick_low", 21}, {"melee_kick_mid", 36}, {"melee_kick_high", 28}, {"melee_kick_highest", 31}, {"melee_kick_move", 21}});
    game::DummyTarget t, far, low;
    t.pos = {260, 0, 0};       // 500 tall: the high class (316..537)
    far.pos = {0, 3000, 0};
    low.pos = {0, -3000, 0}, low.h = 50; // under 63.2: the low kick
    w->targets = {&t, &far, &low};
    for (int i = 0; i < 3; ++i) w->tick({});
    EXPECT_EQ(w->meleeTarget(), &t);
    w->tick(press(&Actions::interact));
    ASSERT_EQ(w->player().state().state, State::Melee);
    EXPECT_EQ(w->player().state().clip, "melee_kick_high");
    int hit = -1, idle = -1;
    for (int i = 1; i < 90; ++i) {
        w->tick(press(&Actions::interact)); // held: one press, one kick
        if (hit < 0 && t.hits) hit = i;
        if (idle < 0 && w->player().state().state == State::Idle) idle = i;
    }
    EXPECT_GE(hit, 5); // frames 3..13 at 30 Hz
    EXPECT_LE(hit, 27);
    EXPECT_EQ(t.hits, 1);
    EXPECT_FLOAT_EQ(t.hp, 700.0f - 60.0f);
    EXPECT_EQ(far.hits, 0);
    EXPECT_NEAR(idle, 56, 3); // 28 keys
    EXPECT_EQ(w->player().state().state, State::Idle) << "the held key does not kick again";
    // out of reach (the melee slot's 640 from her eye; her leg reaches 190 beyond the capsules): the kick misses
    t.pos = {560, 0, 0};
    w->tick({});
    ASSERT_EQ(w->meleeTarget(), &t);
    hold(*w, press(&Actions::interact), 2);
    EXPECT_EQ(w->player().state().state, State::Melee);
    for (int i = 0; i < 90; ++i) w->tick({});
    EXPECT_EQ(t.hits, 1) << "she closes 1200 units/s for 0.1 s before the window: still out of reach";
    // nothing in the slot: the mid kick at the air
    t.pos = {5000, 0, 0};
    w->tick({});
    EXPECT_EQ(w->meleeTarget(), nullptr);
    hold(*w, press(&Actions::interact), 2);
    EXPECT_EQ(w->player().state().clip, "melee_kick_mid");
    for (int i = 0; i < 90; ++i) w->tick({});
    EXPECT_EQ(w->player().state().state, State::Idle);
    EXPECT_EQ(t.hits + far.hits + low.hits, 1);
    // a low target: the low kick
    low.pos = {200, 0, 0};
    w->tick({});
    hold(*w, press(&Actions::interact), 2);
    EXPECT_EQ(w->player().state().clip, "melee_kick_low");
    for (int i = 0; i < 60; ++i) w->tick({});
    EXPECT_EQ(low.hits, 1);
    // a library without the clips: interact is no kick
    auto plain = makeWorld([] { CollisionMesh m; addFloor(m, -1000, -1000, 1000, 1000, 0); return m; }(), {0, 0, 0});
    hold(*plain, press(&Actions::interact), 2);
    EXPECT_EQ(plain->player().state().state, State::Idle);
}

// The launch of a thrown portable as tru.exe 0x6A6F40 solves it: the horizontal speed fixed, the flight time from the
// distance, gravity 3 per frame^2, the vertical speed capped.
TEST(Combat, ThrowVelocityAsTheExe) {
    const game::PortableDef d; // the carved weights' numbers
    EXPECT_FLOAT_EQ(d.aimDist, 2528.45f);
    EXPECT_FLOAT_EQ(d.speed, 53.7295f);
    // a far aim point (8 m ahead, 358 down): 47.06 frames, 1.5 x 47.06 - 7.6 = 63 > the cap: capped at the speed
    const vec3 far = game::throwVelocity({0, 0, 358}, {2528.45f, 0, 0}, d.speed, d.speed);
    EXPECT_NEAR(far.x, 53.7295f * 30, 0.01f);
    EXPECT_EQ(far.y, 0);
    EXPECT_NEAR(far.z, 53.7295f * 30, 0.01f);
    // a near one (a wall 300 ahead, level): 5.58 frames, up 1.5 x 5.58 a frame
    const vec3 nearV = game::throwVelocity({0, 0, 358}, {0, 300, 358}, d.nearSpeed, d.speed);
    EXPECT_NEAR(nearV.y, 53.7295f * 30, 0.01f);
    EXPECT_NEAR(nearV.z, 1.5f * (300 / 53.7295f) * 30, 0.01f);
}

// Thrown off her feet (World::throwLara, the blast of a grenade): the thrown clip skidding with the knock, the landing,
// lying, the get-up; jump while she lies gets her up at once; the input is locked meanwhile.
TEST(Combat, KnockedDownAndGetsUp) {
    const auto clips = {std::pair{"Hit_ThrownFront", 11}, std::pair{"Hit_LandFront", 12}, std::pair{"Hit_getupback", 14},
                        std::pair{"hitReact_thrownBack", 11}, std::pair{"hitReact_landBack", 8}, std::pair{"Hit_GetupFront", 13}};
    const auto run = [&](vec3 vel, bool tapJump) {
        auto w = meleeWorld(clips);
        for (int i = 0; i < 3; ++i) w->tick({});
        w->throwLara(vel);
        EXPECT_EQ(w->player().state().state, State::Knocked);
        std::vector<std::string> seen{w->player().state().clip};
        int up = -1;
        Actions run;
        run.move = {0, 1};
        for (int i = 0; i < 400 && up < 0; ++i) {
            Actions a = run; // the stick does nothing while she is down
            a.jump = tapJump && i % 10 < 5;
            w->tick(a);
            if (w->player().state().clip != seen.back()) seen.push_back(w->player().state().clip);
            if (w->player().state().state != State::Knocked) up = i;
        }
        return std::tuple{seen, up, w->player().state().pos};
    };
    const auto [front, upFront, posFront] = run({-600, 0, 0}, false); // a blast in front of her (she faces +x)
    EXPECT_EQ(front[0], "Hit_ThrownFront");
    EXPECT_EQ(front[1], "Hit_LandFront");
    EXPECT_EQ(front[2], "Hit_getupback");
    EXPECT_LT(posFront.x, -50.0f) << "skidded away from the blast";
    EXPECT_GT(posFront.x, -400.0f);
    EXPECT_NEAR(upFront, 22 + 24 + 60 + 28, 6) << "thrown 11 keys, landing 12, lying 1 s, get-up 14";
    const auto [back, upBack, posBack] = run({600, 0, 0}, true);
    EXPECT_EQ(back[0], "hitReact_thrownBack");
    EXPECT_EQ(back[1], "hitReact_landBack");
    EXPECT_EQ(back[2], "Hit_GetupFront");
    EXPECT_NEAR(upBack, 22 + 16 + 26, 12) << "jump gets her up without the wait";
    EXPECT_GT(posBack.x, 50.0f);
}

// Target lock + crouch with the stick: the evasive roll that way; + jump with the stick aside or back: the evasive leap.
TEST(Combat, EvasiveRollsAndLeapsUnderLock) {
    const auto clips = {std::pair{"combat_rollfront", 11}, std::pair{"combat_rollback", 13}, std::pair{"combat_rollleft", 12},
                        std::pair{"combat_rollright", 12}, std::pair{"combat_jumpl_takeoff", 6}, std::pair{"combat_jumpl_inair", 10},
                        std::pair{"combat_jumpr_takeoff", 6}, std::pair{"combat_jumpr_inair", 10}, std::pair{"combat_jumpb_takeoff", 6},
                        std::pair{"combat_jumpb_inair", 10}};
    const auto move = [&](game::vec2 stick, bool game::Actions::*key, bool lock) {
        auto w = meleeWorld(clips);
        game::DummyTarget t;
        t.pos = {1500, 0, 0};
        w->targets = {&t};
        for (int i = 0; i < 30; ++i) w->tick(guns(false, lock));
        EXPECT_EQ(w->combat().locked, lock);
        Actions a = guns(false, lock);
        a.move = stick, a.*key = true;
        w->tick(a);
        const std::string clip = w->player().state().clip;
        const State st = w->player().state().state;
        a.*key = false, a.move = {0, 0};
        float peak = 0;
        for (int i = 0; i < 120; ++i) w->tick(a), peak = std::max(peak, w->player().state().pos.z);
        w->targets.clear();
        return std::tuple{clip, st, w->player().state().pos, peak};
    };
    // she faces +x (the camera behind her): stick right = -y
    EXPECT_EQ(std::get<0>(move({-1, 0}, &Actions::drop, true)), "combat_rollleft");
    EXPECT_EQ(std::get<0>(move({1, 0}, &Actions::drop, true)), "combat_rollright");
    EXPECT_EQ(std::get<0>(move({0, 1}, &Actions::drop, true)), "combat_rollfront");
    EXPECT_EQ(std::get<0>(move({0, -1}, &Actions::drop, true)), "combat_rollback");
    EXPECT_EQ(std::get<1>(move({-1, 0}, &Actions::drop, true)), State::Melee);
    EXPECT_NE(std::get<1>(move({-1, 0}, &Actions::drop, false)), State::Melee) << "no lock: the plain crouch";
    const auto [clip, st, pos, peak] = move({-1, 0}, &Actions::jump, true);
    EXPECT_EQ(clip, "combat_jumpl_takeoff");
    EXPECT_GT(pos.y, 300.0f) << "leapt to her left";
    EXPECT_GT(peak, 50.0f);
    EXPECT_NEAR(pos.z, 0, 1);
    EXPECT_LT(std::get<2>(move({1, 0}, &Actions::jump, true)).y, -300.0f);
    EXPECT_LT(std::get<2>(move({0, -1}, &Actions::jump, true)).x, -300.0f);
    EXPECT_NE(std::get<0>(move({0, 1}, &Actions::jump, true)), "combat_jumpb_takeoff") << "forward: the plain jump";
}

// A spider under her foot: the kick class of a target under 63.2 is the low kick (the stomp), and its 60 kills it.
TEST(CombatReal, StompKillsASpider) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l4_pool", game::loadLaraAnims(bf));
    const auto spider = game::ai::loadEnemyType(bf, "small_brown_spider");
    EXPECT_GT(spider->tuning.top, 0);
    EXPECT_LT(spider->tuning.top, game::MeleeTuning{}.low) << "a small spider's back is under the low class";
    const auto& s = w.player().state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    auto& e = w.enemies().add(spider, 990001, s.pos + fwd * 230.0f, s.yaw + 3.14159f);
    for (int i = 0; i < 2; ++i) w.tick({});
    ASSERT_EQ(w.meleeTarget(), &e);
    hold(w, press(&Actions::interact), 2);
    EXPECT_EQ(w.player().state().clip, "melee_kick_low");
    for (int i = 0; i < 60 && e.alive(); ++i) w.tick({});
    EXPECT_FALSE(e.alive());
    EXPECT_EQ(e.state().mode, game::ai::Mode::Dead);
}

// From the sprint, interact is the flying kick (melee_kick_move) with its own record: 3 damage, and the target goes
// down all the same. Standing with nothing near: the mid kick at the air; running: the moving kick.
TEST(CombatReal, SprintKickAndAirKicks) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    game::World w(loader, "l1_baldrs_tomb", anims, game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    for (int i = 0; i < 30; ++i) w.tick({});
    hold(w, press(&Actions::interact), 2);
    EXPECT_EQ(w.player().state().clip, "melee_kick_mid");
    for (int i = 0; i < 90; ++i) w.tick({});
    ASSERT_EQ(w.player().state().state, State::Idle);
    Actions run;
    run.move = {0, 1};
    for (int i = 0; i < 40; ++i) w.tick(run);
    ASSERT_EQ(w.player().state().state, State::Run);
    Actions kick = run;
    kick.interact = true;
    w.tick(kick);
    EXPECT_EQ(w.player().state().clip, "melee_kick_move");
    EXPECT_FALSE(w.melee().sprint);
    for (int i = 0; i < 60; ++i) w.tick({});
    ASSERT_EQ(w.player().state().state, State::Idle);
    // the sprint kick into a poacher (a fresh run down the room)
    game::World w2(loader, "l1_baldrs_tomb", anims, game::Spawn{{-6900, -10000, 2560}, 0});
    for (int i = 0; i < 20; ++i) w2.tick({});
    const auto poacher = game::ai::loadEnemyType(bf, "poacher");
    Actions sprint = run;
    sprint.sprint = true;
    int n = 0;
    while (w2.player().state().clip != "sprint_loop" && ++n < 200) w2.tick(sprint);
    ASSERT_EQ(w2.player().state().clip, "sprint_loop");
    const auto& s = w2.player().state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    auto& e = w2.enemies().add(poacher, 990001, s.pos + fwd * 520.0f, s.yaw + 3.14159f);
    const float full = e.health();
    kick = sprint, kick.interact = true;
    w2.tick(kick);
    EXPECT_EQ(w2.player().state().clip, "melee_kick_move");
    EXPECT_TRUE(w2.melee().sprint);
    bool knocked = false;
    for (int i = 0; i < 50; ++i) w2.tick({}), knocked |= e.state().mode == game::ai::Mode::Knocked;
    EXPECT_FLOAT_EQ(e.health(), full - anims.melee.sprintKick.damage); // 3
    EXPECT_TRUE(knocked);
}

// Running at the locked melee target, jump vaults off it: her clip is the enemy's hosted_enemy_vault, it plays its
// vault reaction and takes no damage (the vault record: 0). A tiger; soldiers have no plain vault reaction.
TEST(CombatReal, VaultOffAnEnemy) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    const auto tiger = game::ai::loadEnemyType(bf, "tiger");
    ASSERT_TRUE(tiger->laraVault);
    EXPECT_EQ(tiger->vaultClip, "hitreact_vault");
    EXPECT_TRUE(game::ai::loadEnemyType(bf, "poacher")->vaultClip.empty());
    EXPECT_EQ(game::ai::loadEnemyType(bf, "naga_red")->vaultClip, "hit_react_vault");
    for (int i = 0; i < 10; ++i) w.tick({});
    const auto& s = w.player().state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    auto& e = w.enemies().add(tiger, 990001, s.pos + fwd * 1500.0f, s.yaw + 3.14159f);
    const float full = e.health();
    Actions run = guns(false, true); // target lock held
    run.move = {0, 1};
    int n = 0;
    while (!(w.meleeTarget() == &e && w.player().state().state == State::Run && w.combat().locked) && ++n < 300) w.tick(run);
    ASSERT_EQ(w.meleeTarget(), &e);
    ASSERT_TRUE(w.combat().locked);
    ASSERT_EQ(w.player().state().state, State::Run);
    Actions jump = run;
    jump.jump = true;
    w.tick(jump);
    EXPECT_EQ(w.player().state().state, State::Melee);
    EXPECT_EQ(w.player().state().clip, "vault:tiger");
    EXPECT_EQ(w.melee().move, game::MeleeState::Move::Vault);
    EXPECT_EQ(e.state().mode, game::ai::Mode::Knocked);
    EXPECT_EQ(e.state().clip, tiger->vaultClip);
    EXPECT_GE(e.health(), full - 1.0f);
    for (int i = 0; i < 200 && w.player().state().state == State::Melee; ++i) w.tick({});
    EXPECT_NE(w.player().state().state, State::Melee);
}

// A thrall shot down lies to regenerate; interact beside it is the finishing move (melee_thrall_fatality): its blow at
// frame 23 ends the thrall for good (it breaks into its chunks). One left alone gets up again at full health.
TEST(CombatReal, FatalityEndsADownedThrall) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    const auto viking = game::ai::loadEnemyType(bf, "viking_thrall");
    ASSERT_TRUE(viking->regenerates());
    EXPECT_EQ(viking->permanentDeath, "death_permanent");
    for (int i = 0; i < 10; ++i) w.tick({});
    const auto& s = w.player().state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0), left(-fwd.y, fwd.x, 0);
    auto& a = w.enemies().add(viking, 990001, s.pos + fwd * 900.0f, s.yaw + 3.14159f);
    auto& b = w.enemies().add(viking, 990002, s.pos - fwd * 1500.0f + left * 800.0f, s.yaw);
    // the far one: knocked down by a lethal pistol round (set-up); the near one: shot down with the pistols
    game::Hit bullet{b.health(), b.aimPoint(), -fwd, game::Team::Player};
    bullet.damageClass = 0, bullet.damageType = 1;
    b.onHit(bullet);
    ASSERT_EQ(b.state().mode, game::ai::Mode::Down);
    int n = 0;
    while (a.state().mode != game::ai::Mode::Down && ++n < 60 * 20) w.tick(guns(true));
    ASSERT_EQ(a.state().mode, game::ai::Mode::Down) << "the pistols only knock it down";
    EXPECT_TRUE(a.alive());
    // walk up to it and finish it
    n = 0;
    Actions walk;
    walk.move = {0, 1};
    while (w.meleeTarget(true) != &a && ++n < 600) w.tick(walk);
    ASSERT_EQ(w.meleeTarget(true), &a) << "the fatality slot: a regenerating thrall within 640";
    EXPECT_EQ(w.meleeTarget(), nullptr) << "not a kick target";
    hold(w, press(&Actions::interact), 2);
    EXPECT_EQ(w.player().state().clip, "melee_thrall_fatality");
    EXPECT_EQ(w.melee().move, game::MeleeState::Move::Fatality);
    int dead = -1;
    for (int i = 3; i < 90 && dead < 0; ++i) {
        w.tick({});
        if (!a.alive()) dead = i;
    }
    EXPECT_NEAR(dead, 46, 3) << "event 0x6D at frame 23";
    EXPECT_EQ(a.state().mode, game::ai::Mode::Dead);
    bool chunks = false;
    for (int i = 0; i < 60 * 17; ++i) {
        w.tick({});
        chunks |= a.state().ragdollPhase == 4;
    }
    EXPECT_TRUE(chunks) << "the dead thrall breaks into its chunks";
    EXPECT_FALSE(a.alive()) << "it stays dead past the 15 s it would have lain";
    EXPECT_TRUE(b.alive());
    EXPECT_NE(b.state().mode, game::ai::Mode::Down) << "the other one got up again";
    EXPECT_FLOAT_EQ(b.health(), viking->tuning.health);
}

// l1_baldrs_sideroom: interact by treasure urn 94 kicks it; the kick's hit breaks it (its uber section 0 -> 2 on any
// damage) and it gives birth to its treasure. A contact slower than its 379.3 does not break an urn, a faster one does.
TEST(CombatReal, KickBreaksAnUrn) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_sideroom", game::loadLaraAnims(bf), game::Spawn{{4686, 4078, 2075}, 1.5708f});
    w.script().setBigfile(&bf);
    for (int i = 0; i < 20; ++i) w.tick({});
    const vec3 urn = objectPos(w, 94);
    ASSERT_TRUE(w.uberState(94));
    EXPECT_EQ((*w.uberState(94))[0], 0u);
    ASSERT_TRUE(runPlan(w, "go " + std::to_string(urn.x) + " " + std::to_string(urn.y - 230) + " 25 walk\nface 90\nwait 0.5\n"));
    w.impulseObject(94, 0, 300.0f); // a body touching it slowly: under its trigger 7's 379.27
    w.tick({});
    EXPECT_EQ((*w.uberState(94))[0], 0u);
    hold(w, press(&Actions::interact), 2);
    ASSERT_EQ(w.player().state().state, State::Melee);
    EXPECT_TRUE(w.player().state().clip.starts_with("melee_kick_")) << w.player().state().clip;
    bool broke = false;
    for (int i = 0; i < 120; ++i) {
        w.tick({});
        broke |= (*w.uberState(94))[0] != 0u;
    }
    EXPECT_TRUE(broke);
    EXPECT_TRUE(w.born.contains(94 | 0x20000000)) << "its treasure";
    // a body running into an urn faster than 379.27 breaks it (a thrown or falling weight)
    game::World w2(loader, "l1_baldrs_sideroom", game::loadLaraAnims(bf), game::Spawn{{4686, 4078, 2075}, 1.5708f});
    w2.script().setBigfile(&bf);
    for (int i = 0; i < 20; ++i) w2.tick({});
    ASSERT_TRUE(w2.uberState(94));
    w2.impulseObject(94, 0, 400.0f);
    w2.tick({});
    EXPECT_NE((*w2.uberState(94))[0], 0u);
}

// l1_baldrs_tomb (walkthrough requirement C04-03): the carved weight 220. Interact picks it up (its hosted_pickup clip:
// in her hands from the clip's event), she walks with the portable_encumbered clips, Interact elsewhere swings it and
// keeps it, Crouch puts it down (hosted_putdown) on press plate 219, where it lies as a loose body.
TEST(CombatReal, WeightPickedUpCarriedAndPutDown) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    w.script().setBigfile(&bf);
    for (int i = 0; i < 30; ++i) w.tick({});
    const auto* def = w.portableDef("ms_pt_weight_c");
    ASSERT_TRUE(def);
    EXPECT_TRUE(def->encumbers);
    EXPECT_EQ(def->frame("hosted_pickup", 0x4E), 17);
    EXPECT_EQ(def->frame("hosted_putdown", 0x4F), 20);
    EXPECT_EQ(def->frame("hosted_throw", 0x53), 27);
    EXPECT_NEAR(def->aimDist, 2528.45f, 0.01f); // 8 m
    EXPECT_NEAR(def->speed, 53.7295f, 0.0001f);
    EXPECT_NEAR(def->nearDist, 632.111f, 0.01f);
    const auto* pole = w.portableDef("ar_pt_pole_a");
    ASSERT_TRUE(pole);
    EXPECT_FALSE(pole->encumbers);
    EXPECT_NEAR(pole->aimDist, 1264.22f, 0.01f);
    EXPECT_EQ(w.portableDef("lara"), nullptr);
    ASSERT_TRUE(runPlan(w, "engage 220 walk\n"));
    const vec3 rest = objectPos(w, 220);
    w.tick(press(&Actions::interact));
    EXPECT_EQ(w.player().state().state, State::Melee);
    EXPECT_EQ(w.player().state().clip, *def->clip("hosted_pickup"));
    int took = -1;
    for (int i = 1; i < 120; ++i) {
        w.tick({});
        if (took < 0 && w.heldPortable() == 220) took = i;
        if (took < 0) { EXPECT_EQ(objectPos(w, 220), rest) << "on the floor until her hands take it"; }
    }
    EXPECT_NEAR(took, 34, 2) << "event 0x4E at frame 17";
    EXPECT_EQ(w.player().state().state, State::Idle);
    EXPECT_EQ(w.player().state().clip, "portable_encumbered_idle");
    EXPECT_GT(objectPos(w, 220).z, w.player().state().pos.z + 200) << "in her hands";
    // the encumbered walk: slower than a run, whatever the stick
    Actions run;
    run.move = {0, 1}, run.sprint = true;
    const vec3 from = w.player().state().pos;
    for (int i = 0; i < 120; ++i) w.tick(run);
    EXPECT_EQ(w.player().state().clip, "portable_encumbered_walk");
    const float speed = glm::distance(from, w.player().state().pos) / 2.0f;
    EXPECT_GT(speed, 150.0f);
    EXPECT_LT(speed, 600.0f) << speed;
    for (int i = 0; i < 60; ++i) w.tick({});
    // Interact with nothing to insert it into swings it; it stays in her hands
    hold(w, press(&Actions::interact), 2);
    EXPECT_EQ(w.player().state().clip, *def->clip("hosted_melee"));
    EXPECT_EQ(w.melee().move, game::MeleeState::Move::Swing);
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_EQ(w.heldPortable(), 220);
    // onto plate 219, Crouch puts it down
    const vec3 plate = introPos(bf, "l1_baldrs_tomb", 219);
    ASSERT_TRUE(runPlan(w, "go " + std::to_string(plate.x - 120) + " " + std::to_string(plate.y) + " 30 walk\nface 0\nwait 0.5\n"));
    ASSERT_EQ(w.heldPortable(), 220);
    w.tick(press(&Actions::drop));
    EXPECT_EQ(w.player().state().clip, *def->clip("hosted_putdown"));
    int down = -1;
    for (int i = 1; i < 120; ++i) {
        w.tick({});
        if (down < 0 && !w.heldPortable()) down = i;
    }
    EXPECT_NEAR(down, 40, 2) << "event 0x4F at frame 20";
    EXPECT_EQ(w.player().state().state, State::Idle);
    EXPECT_EQ(w.player().state().clip, "ng_idle");
    // she walks off the plate: the weight stays where she put it
    ASSERT_TRUE(runPlan(w, "go -5000 -10000 60\nwait 3\n"));
    EXPECT_LT(glm::distance(game::vec2(objectPos(w, 220)), game::vec2(plate)), 260.0f) << "it lies on the plate";
    EXPECT_LT(objectPos(w, 220).z, w.player().state().pos.z + 150.0f) << "down from her hands";
}

// C04-03: standing on plate 219's door-side edge with the weight (the plate holds door 223 open only while loaded),
// Grenade throws it (hosted_throw: it leaves her hands on event 0x53 at frame 27) on its arc through the doorway; it
// lands beyond the door and comes to rest by physics. A poacher in its way takes the swung-portable record's 100 and
// goes down.
TEST(CombatReal, WeightThrownThroughTheDoor) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const vec3 plate = introPos(bf, "l1_baldrs_tomb", 219), doorAt = introPos(bf, "l1_baldrs_tomb", 223);
    const auto poacher = game::ai::loadEnemyType(bf, "poacher");
    for (const bool enemy : {false, true}) {
        game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
        w.script().setBigfile(&bf);
        for (int i = 0; i < 30; ++i) w.tick({});
        ASSERT_TRUE(runPlan(w, "engage 220 walk\npress\nwait 2\ngo " + std::to_string(plate.x + 150) + " " + std::to_string(plate.y) + " 30 walk\nface 0\nwait 1\n"));
        ASSERT_EQ(w.heldPortable(), 220);
        const auto* p = w.script().find(219);
        ASSERT_TRUE(p && p->joint);
        ASSERT_LT(p->joint->position, -30.0f) << "she stands on the plate";
        const vec3 lara = w.player().state().pos;
        const int grenades = w.grenades;
        w.tick(press(&Actions::grenade));
        EXPECT_EQ(w.player().state().state, State::Melee);
        EXPECT_EQ(w.player().state().clip, *w.portableDef("ms_pt_weight_c")->clip("hosted_throw"));
        int released = -1, crossed = -1;
        float apex = -1e9f, health = 0;
        game::ai::Enemy* e = nullptr;
        bool knocked = false;
        vec3 last(0);
        int still = 0;
        for (int i = 1; i < 60 * 30 && still < 60; ++i) {
            w.tick({});
            const vec3 o = objectPos(w, 220);
            if (released < 0 && !w.heldPortable()) {
                released = i;
                EXPECT_TRUE(w.melee().thrown.contains(220));
                if (enemy) { // in its way, 1000 ahead of her: the arc passes at his chest
                    e = &w.enemies().add(poacher, 990001, vec3(lara.x + 1000, o.y, lara.z), 3.14159f);
                    health = e->health();
                }
            }
            if (released < 0) continue;
            apex = std::max(apex, o.z);
            if (crossed < 0 && o.x > doorAt.x) crossed = i;
            if (e) knocked |= e->state().mode == game::ai::Mode::Knocked;
            still = glm::distance(o, last) < 0.5f ? still + 1 : 0;
            last = o;
        }
        EXPECT_NEAR(released, 54, 2) << "event 0x53 at frame 27";
        EXPECT_EQ(w.grenades, grenades) << "the key throws the weight, not a grenade";
        EXPECT_GT(apex, lara.z + 500.0f) << "a lob";
        EXPECT_EQ(w.player().state().state, State::Idle);
        EXPECT_TRUE(w.melee().thrown.empty()) << "a weapon for 1.5 s";
        if (!enemy) {
            EXPECT_GT(crossed, released) << "through the doorway";
            EXPECT_GT(last.x, doorAt.x + 100.0f) << "it rests beyond door 223";
            EXPECT_LT(last.z, lara.z + 250.0f) << "on the floor";
        } else {
            ASSERT_TRUE(e);
            EXPECT_FLOAT_EQ(e->health(), health - 100.0f);
            EXPECT_TRUE(knocked);
        }
    }
}

// A weight thrown onto press plate 219 from across the room presses it by its own weight (nobody stands on it).
TEST(CombatReal, ThrownWeightPressesThePlate) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const vec3 plate = introPos(bf, "l1_baldrs_tomb", 219);
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    w.script().setBigfile(&bf);
    for (int i = 0; i < 30; ++i) w.tick({});
    ASSERT_TRUE(runPlan(w, "engage 220 walk\npress\nwait 2\ngo " + std::to_string(plate.x - 1300) + " " + std::to_string(plate.y) + " 30 walk\nface 0\nwait 1\n"));
    ASSERT_EQ(w.heldPortable(), 220);
    const auto* p = w.script().find(219);
    ASSERT_TRUE(p && p->joint);
    ASSERT_EQ(p->joint->position, 0.0f);
    w.tick(press(&Actions::grenade));
    float pressed = 0, nearest = 1e9f;
    for (int i = 0; i < 60 * 12; ++i) {
        w.tick({});
        pressed = std::min(pressed, p->joint->position);
        nearest = std::min(nearest, glm::distance(game::vec2(objectPos(w, 220)), game::vec2(plate)));
    }
    EXPECT_LT(nearest, 300.0f) << "it came down on the plate";
    EXPECT_LT(pressed, -1.0f) << "and pressed it down";
}
