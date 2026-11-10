// Combat: health/fall damage/death, hitscan, lock-on, ammo, render-rate independence (src/game/Combat.*).
#include "game/Game.hpp"
#include "script/Vm.hpp"

#include "cdc/Cdc.hpp"
#include "config/Config.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <unistd.h>

#include "app/game/Autopilot.hpp"

#include <algorithm>
#include <cmath>
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

// Adrenaline head shot as decoded (docs/GAMEPLAY.md "Adrenaline (decoded)"): the meter latches at its max (no drain,
// no gain: 0x6F2930 / 0x6F28C0 with flag 0x400000); the Grapple button with a hard lock on a close enemy starts the move
// (state chart T1404), which spends the meter (0x6E6670); its aim window slows the game to 0.2 (0x715020); the look
// input steers the pip; fire with it in the reticle kills; time returns (0x718EA0).
TEST(Combat, AdrenalineHeadShot) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {900, 0, 0}, t.hp = 5000;
    w->targets.push_back(&t);
    for (int i = 0; i < 60 * 15 && w->combat().adrenaline < 1; ++i) w->tick(guns(true)); // fill by damage dealt
    ASSERT_EQ(w->combat().adrenaline, 1);
    EXPECT_TRUE(w->combat().headShotReady);
    EXPECT_GE(5000 - t.hp, 500); // Lara data +0x2D0: 500 damage dealt fills it, plus what drained (20/s) meanwhile
    EXPECT_LE(5000 - t.hp, 700);
    const Actions lock = guns(false);
    for (int i = 0; i < 300; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().adrenaline, 1) << "a latched meter does not drain";
    Actions use = lock;
    use.interact = true; // not the button (ours used it before the decode)
    w->tick(use), w->tick(lock);
    EXPECT_LT(w->combat().headShot, 0);
    EXPECT_TRUE(w->headShotAvailable());
    Actions go = lock;
    go.grapple = true;
    w->tick(go);
    ASSERT_GE(w->combat().headShot, 0);
    EXPECT_EQ(w->combat().adrenaline, 0) << "the move spends the meter";
    EXPECT_FALSE(w->combat().headShotReady);
    EXPECT_EQ(w->player().state().grappleUid, 0);
    EXPECT_EQ(w->combat().timeScale, 1);
    for (int i = 0; i < 60 && w->combat().slowMo <= 0; ++i) w->tick(lock);
    ASSERT_GT(w->combat().slowMo, 0) << "the aim opens inside the move";
    for (int i = 0; i < 10; ++i) w->tick(lock); // half of +0x2CC = 10 frames: the ease 0x78C120 is half way
    EXPECT_NEAR(w->combat().timeScale, 0.6f, 0.02f);
    for (int i = 0; i < 11; ++i) w->tick(lock);
    EXPECT_NEAR(w->combat().timeScale, 0.2f, 1e-4); // +0x2C8
    const vec3 at = w->player().state().pos;
    for (int i = 0; i < 120 && glm::length(w->combat().pip) > 0.3f; ++i) { // steer the pip into the reticle by looking
        Actions steer = lock;
        const auto d = -glm::normalize(w->combat().pip);
        steer.look = {-d.x * 0.3f, d.y * 0.3f}; // look left = pip left
        w->tick(steer);
    }
    EXPECT_EQ(w->player().state().pos, at);
    ASSERT_LE(glm::length(w->combat().pip), 1);
    ASSERT_GT(w->combat().slowMo, 0) << "the window lasts while the game runs at 0.2";
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
    for (int i = 0; i < 240 && w->combat().headShot >= 0; ++i) w->tick({});
    EXPECT_LT(w->combat().headShot, 0);
}

// The head shot needs the latch and a close lock; a miss (fire with the pip outside) wastes it; a hit on Lara ends it.
TEST(Combat, HeadShotConditionsMissAndInterrupt) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 9000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {3000, 0, 0}, t.hp = 1e6f; // beyond the close range
    w->targets.push_back(&t);
    const Actions lock = guns(false);
    Actions go = lock;
    go.grapple = true;
    for (int i = 0; i < 120; ++i) w->tick(guns(true));
    ASSERT_GT(w->combat().adrenaline, 0);
    ASSERT_LT(w->combat().adrenaline, 1);
    w->tick(lock), w->tick(go), w->tick(lock);
    EXPECT_LT(w->combat().headShot, 0) << "not without a full meter";
    for (int i = 0; i < 60 * 15 && w->combat().adrenaline < 1; ++i) w->tick(guns(true));
    ASSERT_TRUE(w->combat().headShotReady);
    EXPECT_FALSE(w->headShotAvailable()) << "too far";
    w->tick(lock), w->tick(go), w->tick(lock);
    EXPECT_LT(w->combat().headShot, 0);
    t.pos = {900, 0, 0};
    w->tick(lock), w->tick(go);
    ASSERT_GE(w->combat().headShot, 0);
    for (int i = 0; i < 60 && w->combat().slowMo <= 0; ++i) w->tick(lock);
    ASSERT_GT(w->combat().slowMo, 0);
    ASSERT_GT(glm::length(w->combat().pip), 1);
    const float hp = t.hp;
    Actions shoot = lock;
    shoot.fire = true;
    w->tick(shoot);
    EXPECT_EQ(t.hp, hp) << "the pip was outside the reticle";
    EXPECT_EQ(w->combat().slowMo, 0);
    for (int i = 0; i < 300; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().timeScale, 1);
    EXPECT_LT(w->combat().headShot, 0);
    // again, interrupted by a blow
    for (int i = 0; i < 60 * 15 && w->combat().adrenaline < 1; ++i) w->tick(guns(true));
    ASSERT_TRUE(w->combat().headShotReady);
    w->tick(lock), w->tick(go);
    ASSERT_GE(w->combat().headShot, 0);
    for (int i = 0; i < 60 && w->combat().slowMo <= 0; ++i) w->tick(lock);
    ASSERT_GT(w->combat().slowMo, 0);
    w->damageLara(50, {500, 0, 0}, game::kDamageLight);
    EXPECT_LT(w->combat().headShot, 0);
    EXPECT_EQ(w->combat().slowMo, 0);
    for (int i = 0; i < 30; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().timeScale, 1);
}

// Focused fire = adrenaline mode (0x718BD0): Manual Aim with a hard lock and the meter not empty toggles it; the game's
// time scale (Lara's too: the ramp is the global one, 0x8ABE94) goes to 0.5 over 10 frames, the meter drains 225 per
// second of that time, the mode ends with the meter, the lock, or the button; starting it gives a latched head shot up.
TEST(Combat, AdrenalineModeDrainsAndSlowsTime) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget t;
    t.pos = {3000, 0, 0}, t.hp = 1e6f;
    w->targets.push_back(&t);
    for (int i = 0; i < 60 * 6; ++i) w->tick(guns(true));
    const float meter = w->combat().adrenaline;
    ASSERT_GT(meter, 0.5f);
    ASSERT_LT(meter, 1.0f);
    Actions lock = guns(false), go = guns(false), use = guns(false);
    go.manualAim = true, use.interact = true;
    w->tick(lock), w->tick(use), w->tick(lock);
    EXPECT_FALSE(w->combat().adrenalineMode) << "interact is not the button";
    w->tick(go);
    ASSERT_TRUE(w->combat().adrenalineMode);
    EXPECT_FALSE(w->combat().manualAim) << "with the lock held the button is focused fire, not the aim view";
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_NEAR(w->combat().timeScale, 0.5f, 1e-4);
    const float before = w->combat().adrenaline;
    for (int i = 0; i < 60; ++i) w->tick(lock);
    EXPECT_NEAR(before - w->combat().adrenaline, 225.0f / 500.0f * 0.5f, 0.01f) << "one real second = half a game second";
    w->tick(go); // the button again: the mode ends
    EXPECT_FALSE(w->combat().adrenalineMode);
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().timeScale, 1);
    w->tick(go);
    ASSERT_TRUE(w->combat().adrenalineMode);
    for (int i = 0; i < 60 * 8 && w->combat().adrenalineMode; ++i) w->tick(lock);
    EXPECT_FALSE(w->combat().adrenalineMode);
    EXPECT_EQ(w->combat().adrenaline, 0);
    for (int i = 0; i < 21; ++i) w->tick(lock);
    EXPECT_EQ(w->combat().timeScale, 1);
    // without the lock it ends; a latched meter loses the latch when the mode starts
    for (int i = 0; i < 60 * 15 && w->combat().adrenaline < 1; ++i) w->tick(guns(true));
    ASSERT_TRUE(w->combat().headShotReady);
    w->tick(lock), w->tick(go);
    ASSERT_TRUE(w->combat().adrenalineMode);
    EXPECT_FALSE(w->combat().headShotReady);
    for (int i = 0; i < 30; ++i) w->tick(lock);
    EXPECT_LT(w->combat().adrenaline, 1);
    w->tick(guns(false, false));
    EXPECT_FALSE(w->combat().adrenalineMode);
}

// Lara fires at the game's pace: in adrenaline mode (time scale 0.5) the pistols' clips, and so their shots, run at half
// the real rate, exactly as every enemy does.
TEST(Combat, TimeScaleSlowsLaraWithTheWorld) {
    const auto shots = [](bool mode) {
        CollisionMesh m;
        addFloor(m, -5000, -5000, 5000, 5000, 0);
        auto w = makeWorld(m, {0, 0, 0});
        game::DummyTarget t;
        t.pos = {3000, 0, 0}, t.hp = 1e9f;
        w->targets.push_back(&t);
        for (int i = 0; i < 60 * 9; ++i) w->tick(guns(true)); // a well filled meter (not latched)
        if (mode) {
            Actions go = guns(true);
            go.manualAim = true;
            w->tick(go);
        }
        for (int i = 0; i < 30; ++i) w->tick(guns(true)); // the ramp
        const uint64_t before = w->combat().shotsFired;
        for (int i = 0; i < 180; ++i) w->tick(guns(true)); // within the mode's 4.4 real seconds of a full-ish meter
        return int(w->combat().shotsFired - before);
    };
    const int normal = shots(false), slowed = shots(true);
    EXPECT_GE(normal, 8);
    EXPECT_NEAR(slowed * 2, normal, 2);
}

// Look input that turns the view's centre onto `at` (a player steering the reticle), per tick.
static game::vec2 lookAt(const game::World& w, vec3 at) {
    const auto& c = w.camera();
    const vec3 now = glm::normalize(c.target - c.position), want = glm::normalize(at - c.position);
    float dyaw = std::atan2(want.y, want.x) - std::atan2(now.y, now.x);
    while (dyaw > 3.1415927f) dyaw -= 6.2831853f;
    while (dyaw < -3.1415927f) dyaw += 6.2831853f;
    const float dpitch = std::asin(want.z) - std::asin(now.z);
    return {std::clamp(dyaw * 20.0f, -2.5f, 2.5f), std::clamp(dpitch * 20.0f, -2.5f, 2.5f)};
}

// Manual (precision) aim, state chart S718 / camera mode 23: Manual Aim without the target lock toggles the close view
// (distance 632.11) and draws; there is no lock: shots go where the view's centre points and hit what it covers, nothing
// else; Target Lock or Manual Aim again leaves it.
TEST(Combat, ManualAimHitsWhatTheReticleCovers) {
    CollisionMesh m;
    addFloor(m, -9000, -9000, 9000, 9000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget a, b;
    a.pos = {2500, -700, 0}, b.pos = {2500, 700, 0}; // both in the soft cone: the lock would take one of them
    a.hp = b.hp = 1e6f;
    w->targets.push_back(&a), w->targets.push_back(&b);
    for (int i = 0; i < 30; ++i) w->tick({});
    EXPECT_EQ(w->cameraMode(), 1);
    EXPECT_NEAR(w->cameraDistance(), 1264.22f, 1.0f);
    Actions press;
    press.manualAim = true;
    w->tick(press);
    ASSERT_TRUE(w->combat().manualAim);
    for (int i = 0; i < 90; ++i) w->tick({});
    EXPECT_TRUE(w->combat().drawn);
    EXPECT_EQ(w->cameraMode(), 23);
    EXPECT_NEAR(w->cameraDistance(), 632.11f, 2.0f);
    EXPECT_EQ(w->combat().target, nullptr) << "no soft target in the aim view";
    const auto aimAt = [&](vec3 at, bool fire, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            Actions in;
            in.look = lookAt(*w, at);
            in.fire = fire;
            w->tick(in);
        }
    };
    // between the two: nothing is hit
    aimAt({2500, 0, 250}, false, 120);
    aimAt({2500, 0, 250}, true, 90);
    EXPECT_GT(w->combat().shotsFired, 3u);
    EXPECT_EQ(a.hits, 0);
    EXPECT_EQ(b.hits, 0);
    EXPECT_NEAR(w->manualAimPoint().y, 0, 120.0f);
    // onto b: only b
    aimAt(b.aimPoint(), false, 120);
    EXPECT_LT(glm::distance(w->manualAimPoint(), b.aimPoint()), b.r + 20);
    aimAt(b.aimPoint(), true, 90);
    EXPECT_GT(b.hits, 3);
    EXPECT_EQ(a.hits, 0);
    // onto a: only a from now on
    const int hitsB = b.hits;
    aimAt(a.aimPoint(), false, 120);
    aimAt(a.aimPoint(), true, 90);
    EXPECT_GT(a.hits, 3);
    EXPECT_EQ(b.hits, hitsB);
    // she turned to face the view while standing
    EXPECT_NEAR(std::atan2(std::sin(w->player().state().yaw - w->camera().yaw), std::cos(w->player().state().yaw - w->camera().yaw)), 0, 0.3f);
    // she steps in it, she does not run
    Actions walk;
    walk.move = {0, 1};
    const vec3 from = w->player().state().pos;
    for (int i = 0; i < 60; ++i) w->tick(walk);
    EXPECT_TRUE(w->combat().manualAim);
    EXPECT_LT(glm::distance(w->player().state().pos, from), 700.0f);
    // Target Lock leaves it and locks as usual
    w->tick(guns(false));
    EXPECT_FALSE(w->combat().manualAim);
    for (int i = 0; i < 60; ++i) w->tick(guns(false));
    EXPECT_TRUE(w->combat().locked);
    EXPECT_EQ(w->cameraMode(), 1);
    // with the lock held the button is not the aim view
    Actions both = guns(false);
    both.manualAim = true;
    w->tick(both);
    EXPECT_FALSE(w->combat().manualAim);
    // toggle on and off by the button
    w->tick({}), w->tick(press);
    EXPECT_TRUE(w->combat().manualAim);
    w->tick({}), w->tick(press);
    EXPECT_FALSE(w->combat().manualAim);
    // not in the air
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 20; ++i) w->tick(jump);
    ASSERT_NE(w->player().state().state, State::Idle);
    Actions pressAir = press;
    w->tick({}), w->tick(pressAir);
    EXPECT_FALSE(w->combat().manualAim);
}

// Dual targeting (0x71E490): with the two pistols and a primary target, the best other candidate in front of her is
// the second target and the left gun fires at it; a one-gun weapon has none.
TEST(Combat, DualTargetsTakeOneGunEach) {
    CollisionMesh m;
    addFloor(m, -9000, -9000, 9000, 9000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    game::DummyTarget a, b;
    a.pos = {2000, -300, 0}, b.pos = {2600, 500, 0};
    a.hp = b.hp = 1e6f;
    w->targets.push_back(&a), w->targets.push_back(&b);
    for (int i = 0; i < 20; ++i) w->tick(guns(false));
    ASSERT_EQ(w->combat().target, &a);
    EXPECT_EQ(w->combat().second, &b);
    for (int i = 0; i < 120; ++i) {
        w->tick(guns(true));
        for (const auto& s : w->combat().shots) EXPECT_EQ(s.target, s.hand == 1 ? &b : &a);
    }
    EXPECT_GT(a.hits, 2);
    EXPECT_GT(b.hits, 2);
    b.hp = 0; // the second one dies: both guns on the first
    const int hitsA = a.hits;
    for (int i = 0; i < 120; ++i) w->tick(guns(true));
    EXPECT_EQ(w->combat().second, nullptr);
    EXPECT_GT(a.hits, hitsA + 4);
    // the shotgun (one gun): no second target
    b.hp = 1e6f;
    w->giveWeapon(game::Weapon::Shotgun, 20);
    Actions sw = guns(false);
    sw.weapon = true;
    w->tick(sw);
    for (int i = 0; i < 180; ++i) w->tick(guns(false));
    ASSERT_EQ(w->combat().weapon, game::Weapon::Shotgun);
    EXPECT_EQ(w->combat().second, nullptr);
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
        EXPECT_EQ(p->startReserve, def.startReserve);
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

// l4_pool's goo is water (surface 2699.3): under water the pistols draw (combat_water_submerged_equip) and fire with
// their under-water record (pr_handgunbullet +0x84: 40 / 40 / 20), then the speargun is swapped in
// (combat_shotgun_equip_submerged), fires, and each spear is followed by a reload from the reserve.
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
    game::DummyTarget t;
    t.pos = w.player().state().pos + vec3(900, 0, -250);
    w.targets.push_back(&t);
    for (int i = 0; i < 60; ++i) w.tick(guns(true)); // pistols under water
    EXPECT_TRUE(w.combat().drawn);
    EXPECT_EQ(w.combat().weapon, game::Weapon::Pistols);
    EXPECT_EQ(w.player().state().state, State::SwimUnder);
    const uint32_t pistolShots = w.combat().shotsFired;
    EXPECT_GE(pistolShots, 2u);
    EXPECT_EQ(t.hits, int(pistolShots));
    EXPECT_EQ(t.hp, 700 - 40 * int(pistolShots)); // 900 away: inside the short range, the +0x84 record's 40
    w.giveWeapon(game::Weapon::Speargun, 5);
    Actions sw = guns(true);
    sw.weapon = true;
    w.tick(sw);
    for (int i = 0; i < 120 && w.combat().weapon != game::Weapon::Speargun; ++i) w.tick(guns(true)); // the pistols' unequip first
    ASSERT_EQ(w.combat().weapon, game::Weapon::Speargun);
    const int hp = t.hp, hits = t.hits;
    for (int i = 0; i < 150; ++i) w.tick(guns(true)); // equip 17/30 s, then shot, reload 31/30 s, shot
    EXPECT_TRUE(w.combat().drawn);
    EXPECT_EQ(w.player().state().state, State::SwimUnder);
    EXPECT_EQ(w.combat().shotsFired - pistolShots, 2u);
    EXPECT_EQ(t.hits - hits, 2);
    EXPECT_EQ(t.hp, hp - 2 * 550); // under water the +0x84 record: 550 a spear
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

// The scripts read the PLAYER TAILORING levels and the hint toggles (uiexperienceadjustmentmenu 11..18): they follow the
// World's; the preset is the last one picked. The config keeps the four levels (default 1 each, the original's).
TEST(Tailoring, ScriptsAndConfigSeeTheLevels) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    const auto& e = w->script().experience();
    EXPECT_EQ(e.levels, (std::array<int, 5>{1, 1, 1, 1, 1}));
    EXPECT_EQ(e.preset, 1);
    EXPECT_TRUE(e.trainingText && e.metaIcons);
    w->setTailoring(game::Tailoring::preset(2));
    EXPECT_EQ(e.levels, (std::array<int, 5>{2, 2, 2, 2, 2}));
    EXPECT_EQ(e.preset, 2);
    game::Tailoring custom = game::Tailoring::preset(2);
    custom.enemyHealth = 0, custom.savingGrab = 1;
    w->setTailoring(custom);
    EXPECT_EQ(e.levels, (std::array<int, 5>{0, 2, 2, 1, 2}));
    EXPECT_EQ(e.preset, 2) << "a single level changed: the preset picked stays";
    w->setHints(false, true);
    EXPECT_FALSE(e.trainingText);
    EXPECT_TRUE(e.metaIcons);

    const auto dir = std::filesystem::temp_directory_path() / ("ou_tailoring_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    config::Config c;
    EXPECT_EQ(c.game.tailoring, (std::array<int, 4>{1, 1, 1, 1}));
    c.game.tailoring = {0, 2, 1, 2};
    config::save(dir / "config.json", c);
    EXPECT_EQ(config::load(dir / "config.json").game.tailoring, (std::array<int, 4>{0, 2, 1, 2}));
    { std::ofstream(dir / "config.json") << R"({"game": {"tailoring": [1, 3, 1, 1]}})"; }
    EXPECT_THROW(config::load(dir / "config.json"), std::runtime_error);
    { std::ofstream(dir / "config.json") << R"({"game": {"sfx_volume": 3}})"; }
    EXPECT_EQ(config::load(dir / "config.json").game.tailoring, (std::array<int, 4>{1, 1, 1, 1})) << "older configs: the defaults";
    std::filesystem::remove_all(dir);
}

// A level start's loadout (tru.exe 0x7198D0 -> 0x6F57F0(1)): the level list's secondary, or the one chosen for a level
// started from the menu in its place; every secondary a full clip and its start amount at the AMMUNITION CAPACITY level
// (the data's start table equals its maximum table); holstered.
TEST(Combat, LevelStartLoadout) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    using game::Weapon;
    const Weapon expect[] = {Weapon::Shotgun, Weapon::Speargun, Weapon::Uzi, Weapon::Shotgun, Weapon::Tranquilizer,
                             Weapon::Uzi, Weapon::AssaultRifle, Weapon::Speargun};
    for (int ch = 0; ch < 8; ++ch) {
        w->chapterLoadout(ch);
        EXPECT_EQ(w->combat().secondary, expect[ch]) << ch;
        EXPECT_EQ(w->combat().weapon, Weapon::Pistols) << ch;
        EXPECT_FALSE(w->combat().drawn) << ch;
    }
    const int start[3][5] = {{400, 90, 40, 300, 25}, {240, 72, 25, 210, 15}, {160, 42, 15, 120, 10}}; // uzi, shotgun, spear, rifle, tranq
    const Weapon guns[] = {Weapon::Uzi, Weapon::Shotgun, Weapon::Speargun, Weapon::AssaultRifle, Weapon::Tranquilizer};
    for (int level = 0; level < 3; ++level) {
        w->setTailoring(game::Tailoring::preset(level));
        w->chapterLoadout(4);
        for (int g = 0; g < 5; ++g) {
            const auto i = size_t(guns[g]);
            EXPECT_EQ(w->combat().reserve[i], start[level][g]) << level << " " << g;
            EXPECT_EQ(w->combat().reserve[i], w->anims().weapons[i].startReserve[size_t(level)]);
            EXPECT_EQ(w->combat().clip[i][0], w->anims().weapons[i].clip);
        }
    }
    w->chapterLoadout(4, Weapon::AssaultRifle); // chosen on the menu's weapon page
    EXPECT_EQ(w->combat().secondary, Weapon::AssaultRifle);
    w->chapterLoadout(4, Weapon::Pistols); // not a secondary: the level's own
    EXPECT_EQ(w->combat().secondary, Weapon::Tranquilizer);
    w->chapterLoadout(4, Weapon::ThorHammer);
    EXPECT_EQ(w->combat().secondary, Weapon::Tranquilizer);
}

// A death in a world without units (no checkpoint save to restore) keeps the secondary, its rounds and the meter; her
// hands are empty and her health full.
TEST(Combat, RespawnKeepsTheLoadout) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    w->giveWeapon(game::Weapon::Shotgun, 17);
    Actions draw;
    draw.draw = true;
    w->tick(draw);
    for (int i = 0; i < 60; ++i) w->tick({});
    ASSERT_TRUE(w->combat().drawn);
    w->damageLara(300, {}, game::kDamageAbsolute);
    w->respawn();
    EXPECT_EQ(w->combat().secondary, game::Weapon::Shotgun);
    EXPECT_EQ(w->combat().reserve[size_t(game::Weapon::Shotgun)], 17);
    EXPECT_EQ(w->combat().clip[size_t(game::Weapon::Shotgun)][0], w->anims().weapons[size_t(game::Weapon::Shotgun)].clip);
    EXPECT_FALSE(w->combat().drawn);
    EXPECT_EQ(w->health().value(), w->health().max());
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

// Script time-scale ramps (gamebase 24 / 23): the value eases as tru.exe 0x78C120 over the given real seconds — 2u^2 in
// the first half, 1 - 2(1 - u)^2 in the second — and a set (gamebase 2 / 1) lands at once.
TEST(Combat, ScriptTimeRampEasesInAndOut) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto wp = makeWorld(m, {0, 0, 0});
    auto& w = *wp;
    w.tick({});
    w.setTimeScale(false, 0.4f, 2.0f);
    w.setTimeScale(true, 0.5f, 2.0f);
    const auto eased = [](float u) { return u < 0.5f ? 2 * u * u : 1 - 2 * (1 - u) * (1 - u); };
    for (int i = 1; i <= 120; ++i) {
        w.tick({});
        const float e = eased(float(i) / 120.0f);
        ASSERT_NEAR(w.combat().timeScale, 1 + (0.4f - 1) * e, 2e-3f) << i;
        ASSERT_NEAR(w.combat().worldScale, 1 + (0.5f - 1) * e, 2e-3f) << i;
    }
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_FLOAT_EQ(w.combat().timeScale, 0.4f);
    EXPECT_FLOAT_EQ(w.combat().worldScale, 0.5f);
    w.setTimeScale(false, 1, -1), w.setTimeScale(true, 1, -1);
    w.tick({});
    EXPECT_FLOAT_EQ(w.combat().timeScale, 1);
    EXPECT_FLOAT_EQ(w.combat().worldScale, 1);
}

// One-handed pistols from a real ledge (docs/GAMEPLAY.md "One-handed shooting"): l4_pool, hanging from ledge 1253 (as
// GameReal.PoolDropToHang reaches it by a jump), a real panther on the floor behind her: Target Lock + Fire shoot it
// with the right-hand pistol from the hang, the left hand stays on the ledge and she keeps hanging.
TEST(CombatReal, PoolLedgeHangShootsPantherOneHanded) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    game::World w(loader, "l4_pool", anims, game::Spawn{{-4291, 4225.6f, 3100}, glm::radians(-157.9f)});
    w.enemies().setBigfile(&bf);
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 80 && s.state != State::Hang; ++i) w.tick(i == 5 ? jump : Actions{});
    ASSERT_EQ(s.state, State::Hang);
    ASSERT_EQ(w.collision().markup[size_t(s.ledge)].id, 1253);
    for (int i = 0; i < 30; ++i) w.tick({});
    // the panther: on the floor behind her, 1500 out from the wall
    const vec3 back = -vec3(std::cos(s.yaw), std::sin(s.yaw), 0);
    const auto floor = w.collision().raycast(s.pos + back * 1500.0f + vec3(0, 0, 200), {0, 0, -1}, 4000, 0x0F);
    ASSERT_TRUE(floor);
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    auto& cat = w.enemies().add(panther, 990001, floor->point, 0);
    const float full = cat.health();
    const vec3 hung = s.pos;
    bool oneHand = false;
    for (int i = 0; i < 400 && cat.alive(); ++i) {
        w.tick(guns(true));
        ASSERT_EQ(s.state, State::Hang) << i;
        for (const auto& shot : w.combat().shots) EXPECT_EQ(shot.hand, 0) << "only the right-hand gun fires from a hold";
        if (w.combat().drawn && s.clip.find("ledgeb") != std::string::npos) oneHand = true;
    }
    std::printf("hang shooting: %u shots, panther %.0f -> %.0f, clip %s\n", w.combat().shotsFired, full, cat.health(), s.clip.c_str());
    EXPECT_GE(w.combat().shotsFired, 3u);
    EXPECT_LT(cat.health(), full) << "the panther took the shots";
    EXPECT_EQ(w.combat().weapon, game::Weapon::Pistols);
    EXPECT_NEAR(glm::distance(s.pos, hung), 0, 1);
    EXPECT_TRUE(oneHand);
}

// Adrenaline head shot on a real enemy (docs/GAMEPLAY.md "Head shot"): l4_pool, the meter filled by pistol damage,
// then a real panther in close range: Grapple starts the move with the clip hosted in the panther's DRM
// (hosted_enemy_aa_handguns, its events 5 / 4 the window), the pip is steered in, Fire kills it.
TEST(CombatReal, PoolHeadShotOnPanther) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    game::World w(loader, "l4_pool", anims);
    w.enemies().setBigfile(&bf);
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    const auto& move = panther->tuning.headShot;
    ASSERT_TRUE(move.valid());
    EXPECT_NEAR(move.distance, 316.06f, 0.01f); // list node +0x54: +0x10
    EXPECT_NEAR(move.blend, 6.0f / 30, 1e-4f);  // +0x14: 6 frames
    EXPECT_EQ(move.clips[0].name, "hosted_enemy_aa_handguns");
    EXPECT_GT(move.clips[0].open, 0);
    EXPECT_LT(move.clips[0].open, move.clips[0].close); // the hosted clip's events 5 / 4
    {
        game::DummyTarget t;
        t.hp = 1e6f;
        t.pos = {-13385, -600, 4500};
        w.targets.push_back(&t);
        for (int i = 0; i < 60 * 20 && w.combat().adrenaline < 1; ++i) w.tick(guns(true));
        w.targets.clear();
        for (int i = 0; i < 5; ++i) w.tick({});
    }
    ASSERT_EQ(w.combat().adrenaline, 1);
    ASSERT_TRUE(w.combat().headShotReady);
    const auto& s = w.player().state();
    auto& cat = w.enemies().add(panther, 990001, s.pos + vec3(-900, 0, 0), 0);
    const Actions lock = guns(false);
    int started = -1;
    for (int i = 0; i < 240 && started < 0; ++i) { // locked on; Grapple as soon as it is offered
        Actions a = lock;
        a.grapple = w.headShotAvailable();
        w.tick(a);
        if (w.combat().headShot >= 0) started = i;
    }
    ASSERT_GE(started, 0) << "no head shot offered; panther at " << glm::distance(cat.state().pos, s.pos);
    EXPECT_EQ(w.combat().adrenaline, 0);
    EXPECT_EQ(s.clip, "cat_panther:hosted_enemy_aa_handguns");
    EXPECT_EQ(s.grappleUid, 0);
    EXPECT_NEAR(glm::distance(glm::vec2(cat.state().pos), glm::vec2(s.pos)), move.distance, 2.0f);
    for (int i = 0; i < 120 && w.combat().slowMo <= 0; ++i) w.tick(lock);
    ASSERT_GT(w.combat().slowMo, 0) << "the window opens on the clip's event 5";
    for (int i = 0; i < 200 && glm::length(w.combat().pip) > 0.3f && w.combat().headShot >= 0; ++i) {
        Actions steer = lock;
        const auto d = -glm::normalize(w.combat().pip);
        steer.look = {-d.x * 0.3f, d.y * 0.3f};
        w.tick(steer);
    }
    ASSERT_GE(w.combat().headShot, 0) << "the move was cut short";
    EXPECT_NEAR(w.combat().timeScale, 0.2f, 0.01f);
    Actions shoot = lock;
    shoot.fire = true;
    w.tick(shoot);
    EXPECT_FALSE(cat.alive());
    for (int i = 0; i < 90; ++i) w.tick({});
    EXPECT_EQ(cat.state().mode, game::ai::Mode::Dead);
    EXPECT_NE(cat.state().clip.find("adren"), std::string::npos) << cat.state().clip;
    EXPECT_FLOAT_EQ(w.combat().timeScale, 1);
    std::printf("head shot: started +%d, window %.2f..%.2f s, death clip %s\n", started, move.clips[0].open, move.clips[0].close, cat.state().clip.c_str());
}

// Manual aim on a real enemy (docs/GAMEPLAY.md "Manual aim"): l4_pool, a real panther; no lock is taken, the shots go
// where the view's centre points and hurt it.
TEST(CombatReal, PoolManualAimHitsPanther) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    const auto anims = game::loadLaraAnims(bf);
    game::World w(loader, "l4_pool", anims);
    w.enemies().setBigfile(&bf);
    for (int i = 0; i < 30; ++i) w.tick({});
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    auto& cat = w.enemies().add(panther, 990001, w.player().state().pos + vec3(-1500, -600, 0), 0);
    const float full = cat.health();
    Actions press;
    press.manualAim = true;
    w.tick(press);
    ASSERT_TRUE(w.combat().manualAim);
    w.tick({});
    EXPECT_EQ(w.cameraMode(), 23);
    int hits = 0;
    for (int i = 0; i < 600 && cat.alive(); ++i) {
        Actions a;
        a.look = lookAt(w, cat.aimPoint());
        a.fire = i > 30;
        const float before = cat.health();
        w.tick(a);
        EXPECT_FALSE(w.combat().locked);
        hits += cat.health() < before;
    }
    std::printf("manual aim: %u shots, %d hits, panther %.0f -> %.0f\n", w.combat().shotsFired, hits, full, cat.health());
    EXPECT_GE(hits, 3);
    EXPECT_LT(cat.health(), full - 60);
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
    // a blast in front of her (she faces +x): thrown onto her back (the clips' root motion in lara.drm: the back landing
    // moves her backwards, the front landing forwards; CombatReal.GrenadeBlastKnocksLaraDownAndSheGetsUp)
    const auto [front, upFront, posFront] = run({-600, 0, 0}, false);
    EXPECT_EQ(front[0], "hitReact_thrownBack");
    EXPECT_EQ(front[1], "hitReact_landBack");
    EXPECT_EQ(front[2], "Hit_getupback");
    EXPECT_LT(posFront.x, -50.0f) << "skidded away from the blast";
    EXPECT_GT(posFront.x, -400.0f);
    EXPECT_NEAR(upFront, 22 + 16 + 60 + 28, 6) << "thrown 11 keys, landing 8, lying 1 s, get-up 14";
    const auto [back, upBack, posBack] = run({600, 0, 0}, true); // from behind: onto her front
    EXPECT_EQ(back[0], "Hit_ThrownFront");
    EXPECT_EQ(back[1], "Hit_LandFront");
    EXPECT_EQ(back[2], "Hit_GetupFront");
    EXPECT_NEAR(upBack, 22 + 24 + 26, 12) << "jump gets her up without the wait";
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

// l1_baldrs_tomb: her own grenade stuck to a target half its radius ahead goes off within its radius of her: the blast's knock
// throws her off her feet in the real unit (the thrown clip, the landing), she lies and gets up again, alive, on the
// room's floor; the stick does nothing meanwhile.
TEST(CombatReal, GrenadeBlastKnocksLaraDownAndSheGetsUp) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    for (int i = 0; i < 10; ++i) w.tick({});
    const auto& s = w.player().state();
    const float floorZ = s.pos.z;
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0), start = s.pos;
    const auto& g = w.anims().grenade;
    game::DummyTarget e; // a still target for the grenade to stick to
    e.pos = s.pos + fwd * (0.5f * g.radius);
    w.targets.push_back(&e);
    struct Drop { game::World& w; ~Drop() { w.targets.clear(); } } drop{w};
    int n = 0;
    while (!(w.combat().locked && w.combat().target == &e) && ++n < 120) w.tick(guns(false));
    ASSERT_EQ(w.combat().target, &e);
    Actions throwIt = guns(false);
    throwIt.grenade = true;
    w.tick(throwIt);
    int knocked = -1, exploded = -1;
    float blast = -1;
    for (int i = 0; i < 600 && knocked < 0; ++i) {
        w.tick(guns(false));
        if (exploded < 0 && !w.explosions().empty()) exploded = i, blast = glm::distance(w.explosions()[0], s.pos);
        if (s.state == State::Knocked) knocked = i;
    }
    ASSERT_GE(exploded, 0) << "her grenade went off";
    ASSERT_GE(knocked, 0) << "the blast threw her: " << blast << " from her, radius " << g.radius << ", state "
                          << game::stateName(s.state) << " health " << w.health().value();
    EXPECT_EQ(s.clip, "hitReact_thrownBack") << "the blast in front of her throws her back";
    EXPECT_GT(w.health().value(), 0.0f);
    std::vector<std::string> seen{s.clip};
    Actions run;
    run.move = {0, 1};
    int up = -1;
    for (int i = 0; i < 600 && up < 0; ++i) {
        w.tick(run);
        if (s.clip != seen.back()) seen.push_back(s.clip);
        if (s.state != State::Knocked) up = i;
    }
    ASSERT_GE(up, 0) << "she got up";
    ASSERT_GE(seen.size(), 3u);
    EXPECT_EQ(seen[1], "hitReact_landBack");
    EXPECT_EQ(seen[2], "Hit_getupback");
    EXPECT_GT(up, 90) << "thrown, landed, lay a second, got up";
    EXPECT_GT(glm::dot(start - s.pos, fwd), 30.0f) << "skidded away from the blast";
    EXPECT_NEAR(s.pos.z, floorZ, 5.0f);
    for (int i = 0; i < 30; ++i) w.tick(run);
    EXPECT_TRUE(s.state == State::Run || s.state == State::Walk) << "on her feet again: the stick moves her";
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

// l1_baldrs_tomb: the carried weight collides with the level. Walked at each wall of the west room in turn, the weight
// (a sphere of its thickness round its origin) never enters a wall and nothing lies between her chest and it; the wall
// holds her off farther than her own radius would. Put down against the wall it is a loose rigid body from her hands,
// resting on the floor, outside the wall.
TEST(CombatReal, CarriedWeightCollidesWithTheLevel) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto loader = [&](const std::string& n) { return game::loadUnitData(bf, n); };
    game::World w(loader, "l1_baldrs_tomb", game::loadLaraAnims(bf), game::Spawn{{-5000, -10000, 2560}, 3.14159f});
    w.script().setBigfile(&bf);
    for (int i = 0; i < 30; ++i) w.tick({});
    const auto* def = w.portableDef("ms_pt_weight_c");
    ASSERT_TRUE(def);
    EXPECT_NEAR(def->thick, 104.0f, 1.0f) << "the smallest half extent of its collision box";
    EXPECT_LT(def->thick, 200.0f);
    ASSERT_TRUE(runPlan(w, "engage 220 walk\npress\nwait 2\n"));
    ASSERT_EQ(w.heldPortable(), 220);
    const auto& s = w.player().state();
    const float floorZ = s.pos.z;
    const auto wallNear = [&](vec3 p, float r) { // distance to the nearest wall face within r, else r
        const auto hit = w.collision().closestPoint(p, r, collision::kSightMask);
        return hit && std::abs(hit->normal.z) < 0.7f ? hit->t : r;
    };
    int walls = 0;
    for (const int deg : {180, 90, 270}) {
        const float yaw = glm::radians(float(deg));
        const vec3 dir(std::cos(yaw), std::sin(yaw), 0);
        const auto wall = w.collision().raycast(s.pos + vec3(0, 0, 250), dir, 4000, collision::kSightMask);
        if (!wall || std::abs(wall->normal.z) > 0.3f) continue;
        ++walls;
        float least = 1e9f;
        bool blocked = false;
        ASSERT_TRUE(runPlan(w, "run " + std::to_string(deg) + " " + std::to_string(wall->t / 250.0f + 3.0f) + "\n", [&] {
            if (w.heldPortable() != 220) return;
            const vec3 at = objectPos(w, 220);
            least = std::min(least, wallNear(at, def->thick));
            const vec3 chest = s.pos + vec3(0, 0, std::max(at.z - s.pos.z, 250.0f));
            if (const float len = glm::length(at - chest); len > 1.0f)
                blocked |= w.collision().raycast(chest, (at - chest) / len, len, collision::kSightMask).has_value();
        }));
        ASSERT_EQ(w.heldPortable(), 220);
        EXPECT_GT(least, def->thick - 12.0f) << "heading " << deg << ": the weight stayed out of the wall";
        EXPECT_FALSE(blocked) << "heading " << deg << ": nothing between her and the weight";
        const auto ahead = w.collision().raycast(s.pos + vec3(0, 0, 250), dir, 1000, collision::kSightMask);
        ASSERT_TRUE(ahead) << "she walked up to the wall";
        EXPECT_GT(ahead->t, w.player().tuning().radius + 20.0f) << "heading " << deg << ": held off by the weight";
        EXPECT_LT(ahead->t, 500.0f);
    }
    EXPECT_GE(walls, 2);
    // put down against the last wall: a loose body from her hands
    hold(w, press(&Actions::drop), 2);
    for (int i = 0; i < 240; ++i) w.tick({});
    EXPECT_EQ(w.heldPortable(), 0);
    const auto* o = w.script().find(220);
    ASSERT_TRUE(o && o->loose);
    ASSERT_TRUE(w.physics().loose(220)) << "a rigid body in the scene";
    const vec3 rest = objectPos(w, 220);
    EXPECT_LT(rest.z, floorZ + 200.0f) << "on the floor";
    EXPECT_GT(rest.z, floorZ - 20.0f);
    EXPECT_GT(wallNear(rest + vec3(0, 0, def->thick), def->thick * 0.5f), def->thick * 0.5f - 1.0f) << "not in the wall";
    EXPECT_LT(glm::length(glm::vec2(rest - s.pos)), 500.0f) << "by her";
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
        std::optional<State> afterThrow; // her state where the throw clip ends (later the knocked-down poacher gets up and shoots)
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
            if (!afterThrow && w.player().state().state != State::Melee) afterThrow = w.player().state().state;
            apex = std::max(apex, o.z);
            if (crossed < 0 && o.x > doorAt.x) crossed = i;
            if (e) knocked |= e->state().mode == game::ai::Mode::Knocked;
            still = glm::distance(o, last) < 0.5f ? still + 1 : 0;
            last = o;
        }
        // The weight lies still on its cube soon after it lands: her throw clip may still be running.
        for (int i = 0; i < 240 && !afterThrow; ++i) {
            w.tick({});
            if (w.player().state().state != State::Melee) afterThrow = w.player().state().state;
        }
        ASSERT_TRUE(afterThrow) << "the throw clip ends";
        EXPECT_NEAR(released, 54, 2) << "event 0x53 at frame 27";
        EXPECT_EQ(w.grenades, grenades) << "the key throws the weight, not a grenade";
        EXPECT_GT(apex, lara.z + 500.0f) << "a lob";
        EXPECT_EQ(*afterThrow, State::Idle);
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

// Natla's fireballs (docs/GAMEPLAY.md "Natla"): the weapon objects the level scripts fire with `weaponbase` 0.
TEST(CombatReal, NatlaFireballWeaponsDecoded) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto ball = game::loadObjectWeapon(bf, "wp_natla_fireball"), slow = game::loadObjectWeapon(bf, "wp_natla_fireball_adren");
    ASSERT_TRUE(ball && slow);
    EXPECT_EQ(ball->ammo, "pr_natla_fireball");
    EXPECT_EQ(slow->ammo, "pr_natla_fireball_adren");
    EXPECT_FLOAT_EQ(ball->speed, 300 * 30.0f); // weapon stats +0x10, per 30 Hz frame
    EXPECT_FLOAT_EQ(slow->speed, 50 * 30.0f);
    EXPECT_FLOAT_EQ(ball->directDamage, 0);    // projectile fire record +0x1C
    EXPECT_FLOAT_EQ(slow->directDamage, 3000); // three times her full health: the timed dodges kill
    EXPECT_FLOAT_EQ(ball->damage, 250);
    EXPECT_FLOAT_EQ(ball->edgeDamage, 50);
    EXPECT_FLOAT_EQ(ball->radius, 650);
    EXPECT_FLOAT_EQ(slow->radius, 200);
    EXPECT_FLOAT_EQ(ball->knock, 50);
    EXPECT_FLOAT_EQ(ball->knockEdge, 20);
    EXPECT_FLOAT_EQ(ball->angle, 10);
    EXPECT_FLOAT_EQ(ball->life, 5);
    EXPECT_FALSE(game::loadObjectWeapon(bf, "wp_handgun")); // a hitscan weapon has no projectile explosion record
}

// l7_yggdrasil_end after 320_hel_boss: on the lower ring the level scripts (manager 8448) send Natla 8403 from warp point
// to warp point (movers 8598.., she hangs on their marker anchor) and have her throw wp_natla_fireball 8433 (hung on her
// marker 13) at the floor targets of Lara's zone (8463, 8464, ...), two per stop. A ball flies straight at 9000 units/s
// from where Natla is, bursts on the floor (250 -> 50 within 650, thrown off her feet), and the target's fire patch
// (volume 8525 at 8463: 100 a second while its damage is on) burns who stands in it.
TEST(CombatReal, NatlaFireballsBlastThrowAndBurn) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    std::map<std::string, game::UnitData> cache;
    const game::UnitLoader loader = [&](const std::string& n) {
        auto& d = cache[n];
        if (!d.drm) d = game::loadUnitData(bf, n);
        return d;
    };
    game::World w(loader, "l7_yggdrasil_end", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.enemies().setBigfile(&bf);
    const auto put = [&](vec3 at) {
        if (const auto floor = w.collision().groundProbe(at + vec3(0, 0, 300), 3000, 0x0F)) at.z = floor->point.z;
        game::PlayerState s;
        s.pos = at;
        w.restore(s, 0, -0.25f);
        w.script().playerJumped();
    };
    for (int i = 0; i < 60; ++i) w.tick({});
    put(w.script().find(8303)->pos); // the arena volume: 320_hel_boss
    for (int i = 0; i < 60 * 105; ++i) w.tick({});
    ASSERT_TRUE(w.hostileShots().empty());
    const auto* target = w.script().find(8463);
    const auto* natla = w.script().find(8403);
    ASSERT_TRUE(target && natla);
    put(target->pos);
    const vec3 stood = w.player().state().pos;
    const float full = w.health().value();
    // the first ball: from her warp point, not her intro; straight at the target
    int fired = -1, burst = -1;
    vec3 from{0}, vel{0}, at{0};
    for (int i = 0; i < 60 * 20 && burst < 0; ++i) {
        w.tick({});
        if (fired < 0 && !w.hostileShots().empty()) {
            fired = i, from = w.hostileShots()[0].pos, vel = w.hostileShots()[0].vel;
            EXPECT_EQ(w.hostileShots()[0].owner, 8433);
            EXPECT_EQ(w.hostileShots()[0].weapon, "wp_natla_fireball");
        }
        if (!w.hostileBlasts().empty()) burst = i, at = w.hostileBlasts()[0];
    }
    ASSERT_GE(fired, 0);
    ASSERT_GT(burst, fired);
    EXPECT_NEAR(glm::length(vel), 9000.0f, 1.0f);
    EXPECT_GT(glm::distance(from, natla->pos), 3000.0f) << "thrown from her warp point";
    EXPECT_LT(glm::length(glm::cross(glm::normalize(vel), target->pos - from)), 60.0f) << "aimed at the floor target";
    EXPECT_LT(glm::distance(at, target->pos), 80.0f) << "bursts on the floor at the target";
    EXPECT_NEAR(float(burst - fired) / 60.0f, glm::distance(from, at) / 9000.0f, 0.05f) << "9000 units a second";
    const float after = w.health().value();
    EXPECT_LT(after, full - 50.0f);   // inside the 650 radius: between the edge's 50 ...
    EXPECT_GE(after, full - 250.0f);  // ... and the centre's 250 (her chest is 375 over the burst)
    EXPECT_EQ(w.player().state().state, State::Knocked) << "thrown off her feet";
    // The patch: lit 1.5 s after the throw was ordered, for 3 s (the target's two clocks). Its volume starts the burn
    // when she enters it lit (OnIn): 100 at once and 100 each second while she stays. She steps aside (to the zone volume's centre), the
    // next ball for this target bursts, and she is back in the fire half a second later.
    put(w.script().find(8652)->pos);
    int again = -1;
    for (int i = 0; i < 60 * 20 && again < 0; ++i) {
        w.tick({});
        for (const vec3& b : w.hostileBlasts())
            if (glm::distance(b, target->pos) < 80.0f) again = i;
    }
    ASSERT_GE(again, 0);
    EXPECT_EQ(w.health().value(), w.health().max()) << "640 from this target and 1100 from the other: nothing reaches her";
    for (int i = 0; i < 30; ++i) w.tick({});
    put(target->pos);
    int burns = 0;
    float last = w.health().value();
    for (int i = 0; i < 150; ++i) {
        w.tick({});
        if (w.health().value() < last - 1.0f) {
            EXPECT_NEAR(last - w.health().value(), 100.0f, 0.5f);
            ++burns, last = w.health().value();
        }
    }
    EXPECT_GE(burns, 2);
    EXPECT_LE(burns, 3);
    put(w.script().find(8652)->pos);
    for (int i = 0; i < 60; ++i) w.tick({});
    // a shot in flight is saved and flies on after a load
    bool round = false;
    for (int i = 0; i < 60 * 30 && !round; ++i) {
        w.tick({});
        if (w.hostileShots().empty()) continue;
        const auto shot = w.hostileShots()[0];
        const auto saved = w.serializeCombat();
        ASSERT_TRUE(saved.contains("hostile"));
        w.restoreCombat(saved);
        ASSERT_EQ(w.hostileShots().size(), 1u);
        EXPECT_EQ(w.hostileShots()[0].pos, shot.pos);
        EXPECT_EQ(w.hostileShots()[0].vel, shot.vel);
        EXPECT_EQ(w.hostileShots()[0].owner, shot.owner);
        round = true;
    }
    EXPECT_TRUE(round);
    for (int i = 0; i < 60 * 2; ++i) w.tick({});
    if (w.hostileShots().empty()) EXPECT_FALSE(w.serializeCombat().contains("hostile")) << "no key without a shot: older saves unchanged";
    for (const auto& [key, n] : w.script().failures()) {
        EXPECT_EQ(key.second.find("weaponbase"), std::string::npos) << key.first;
        EXPECT_NE(key.second, "budget") << key.first << ": a polling thread outlives the instruction budget of one resume";
    }
    // the pool 8309 lies at its marker 8680 during the fight (its constructor's `marker` locals), far under the rings
    for (const auto& m : w.collision().markup)
        if (m.type == 10 && m.id == 8309) EXPECT_LT(m.points[1].z, -10000.0f);
}

// Hitting her [O tru.exe 0x6C64D0, scripts]: a weapon's hit on an instance that has a script calls the script's
// instance prototype 35 (vt 22, 0x799100) with (attacker, victim, damage, damagetype = the hit's damage class). Natla's
// base script 19035 compares the class with its constant 13, Mjolnir's (damagetype 0 = equality, 0x4AA4D0), and posts
// `hit_by_player`; her own override passes hits on only while the upper manager 8450 has not taken over. The lower
// ring's manager 8448 counts them (+124) and at more than two (`2 < count`: the third) ends its phase: vt 66, no more
// fireballs, 8450's flag +44 set. Lara stands between two floor targets, aims by hand (no lock: she is no enemy) and
// fires Mjolnir at her as she is carried from warp point to warp point.
TEST(CombatReal, NatlaCountsMjolnirHits) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    std::map<std::string, game::UnitData> cache;
    const game::UnitLoader loader = [&](const std::string& n) {
        auto& d = cache[n];
        if (!d.drm) d = game::loadUnitData(bf, n);
        return d;
    };
    game::World w(loader, "l7_yggdrasil_end", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.enemies().setBigfile(&bf);
    const auto put = [&](vec3 at) {
        if (const auto floor = w.collision().groundProbe(at + vec3(0, 0, 300), 3000, 0x0F)) at.z = floor->point.z;
        game::PlayerState s;
        s.pos = at;
        w.restore(s, 0, -0.25f);
        w.script().playerJumped();
    };
    for (int i = 0; i < 60; ++i) w.tick({});
    put(w.script().find(8303)->pos); // the arena volume: 320_hel_boss
    for (int i = 0; i < 60 * 105; ++i) w.tick({});
    ASSERT_TRUE(w.script().takesHits(8403));
    for (const int32_t hidden : {8246, 8233}) EXPECT_TRUE(w.script().takesHits(hidden)) << "every natla_end has the callback";
    const auto* a = w.script().find(8463);
    const auto* b = w.script().find(8464);
    ASSERT_TRUE(a && b);
    put((a->pos + b->pos) * 0.5f); // 883 from either floor target: outside both blasts (radius 650)
    w.giveWeapon(game::Weapon::ThorHammer, 0);
    const auto field = [&](int32_t uid, uint32_t off) -> int {
        const auto* o = w.script().find(uid);
        if (!o || !o->script) return -1;
        const auto f = o->script->fields.find(off);
        return f == o->script->fields.end() ? 0 : int(f->second);
    };
    const auto natla = [&]() -> std::optional<vec3> {
        for (const auto& d : w.shotOnlyShapes())
            if (d.id == 8403) return vec3(d.transform[3]);
        return std::nullopt;
    };
    for (int i = 0; i < 60 * 4; ++i) w.tick({}); // the zone's manager starts on her
    ASSERT_EQ(field(8448, 108), 1) << "the lower ring's fight runs";
    ASSERT_TRUE(natla());
    EXPECT_GT(glm::distance(*natla(), w.script().find(8403)->pos), 3000.0f) << "her shapes ride her warp point, not her intro";
    // a hit of another damage class (the pistols' 0) is not counted
    w.script().weaponHit(8403, 50.0f, 0);
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_EQ(field(8448, 124), 0);
    for (int k = 0; k < 4 && w.combat().want != game::Weapon::ThorHammer; ++k) { // Weapon key: to Mjolnir
        game::Actions sw;
        sw.weapon = true;
        w.tick(sw);
        w.tick({});
    }
    ASSERT_EQ(w.combat().want, game::Weapon::ThorHammer);
    game::Actions press;
    press.manualAim = true;
    w.tick(press);
    ASSERT_TRUE(w.combat().manualAim);
    int struck = 0, fired = 0;
    size_t flying = 0;
    for (int i = 0; i < 60 * 150 && field(8448, 110) == 0; ++i) {
        game::Actions in;
        if (const auto at = natla()) {
            in.look = lookAt(w, *at);
            in.fire = glm::distance(*at, w.player().state().pos) < 7500.0f; // Mjolnir reaches 8000
        }
        const int before = field(8448, 124);
        w.tick(in);
        if (field(8448, 124) > before) ++struck;
        ASSERT_FALSE(w.health().dead()) << "tick " << i;
    }
    EXPECT_EQ(struck, 3) << "the third counted hit ends the phase";
    EXPECT_EQ(field(8448, 124), 3);
    EXPECT_EQ(field(8448, 110), 1) << "manager 8448: its fight is over (vt 66)";
    EXPECT_EQ(field(8448, 108), 0);
    EXPECT_EQ(field(8450, 44), 1) << "manager 8450 took over";
    for (int i = 0; i < 60 * 4; ++i) w.tick({}); // a pair already ordered still comes (its second ball 2 s after the first)
    flying = w.hostileShots().size();
    for (int i = 0; i < 60 * 15; ++i) {
        w.tick({});
        if (w.hostileShots().size() > flying) ++fired;
        flying = w.hostileShots().size();
    }
    EXPECT_EQ(fired, 0) << "no more fireballs on the lower ring";
    // a later hit is not passed on (her override asks 8450)
    w.script().weaponHit(8403, 50.0f, 13);
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_EQ(field(8448, 124), 3);
    for (const auto& [key, n] : w.script().failures()) EXPECT_NE(key.first, 8403) << key.second;
}

// Damaging collision posts its amount as a hit of class 6 (tru.exe 0x5D4C60 -> message 0x11C -> 0x6F5030): priority 4,
// so a second touch inside the 30-frame window is ignored and a touch of 12345 (hammers, crusher plates) is death.
TEST(Combat, ContactDamageClassWindowAndKill) {
    game::Health h;
    EXPECT_TRUE(h.damage(400, game::kDamageContact)); // a grinder
    EXPECT_EQ(h.value(), 600);
    for (int i = 0; i < 59; ++i) {
        h.tick(1.0f / 60);
        EXPECT_FALSE(h.damage(400, game::kDamageContact)) << i;
    }
    EXPECT_FALSE(h.damage(100, game::kDamageLight)) << "an enemy's light blow ranks below it";
    h.tick(2.0f / 60);
    EXPECT_TRUE(h.damage(400, game::kDamageContact));
    EXPECT_EQ(h.value(), 200);
    h.tick(1.1f);
    EXPECT_TRUE(h.damage(12345, game::kDamageContact));
    EXPECT_TRUE(h.dead());
}
