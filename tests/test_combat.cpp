// Combat: health/fall damage/death, hitscan, lock-on, ammo, render-rate independence (src/game/Combat.*).
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

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
    for (int i = 0; i < 120 && d->player().state().state == State::Dead; ++i) d->tick({});
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
    for (int i = 0; i < 30; ++i) w->tick(guns(false)); // fire draws; aim locks
    EXPECT_FALSE(w->combat().drawn); // aim alone does not draw
    for (int i = 0; i < 60; ++i) w->tick(guns(true)); // 1 s of fire
    const auto& c = w->combat();
    EXPECT_TRUE(c.drawn);
    EXPECT_EQ(c.target, &t);
    EXPECT_TRUE(c.locked);
    // drawn after 0.2 s, then one shot per 1/3 s
    EXPECT_GE(c.shotsFired, 2u);
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
    EXPECT_NEAR(5000 - t.hp, 1000, 45); // 1000 damage fills it [I]
    Actions lock = guns(false), go = guns(false);
    go.interact = true;
    w->tick(lock);
    w->tick(go); // full + locked + close + interact: the window opens
    ASSERT_GT(w->combat().slowMo, 0);
    for (int i = 0; i < 15; ++i) w->tick(lock);
    EXPECT_NEAR(w->combat().timeScale, 0.2f, 1e-4); // eased down in 0.25 s
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
    for (int i = 0; i < 16; ++i) w->tick({});
    EXPECT_EQ(w->combat().timeScale, 1); // time scale restored
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
    for (const char* n : {"hit_light_front", "hit_mediumback", "hit_light_side_swim"}) EXPECT_TRUE(anims.find(n)) << n;
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
    // death_fall_normal is 1.067 s; respawn 1 s after it ends
    EXPECT_NEAR(respawnedAt - diedAt, std::lround((anims.find("death_fall_normal")->durationSeconds() + 1.0) * 60), 2);
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
    EXPECT_EQ(t.hp, 700 - 2 * 250);
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

// A grenade thrown at a locked dummy 800 ahead sticks to it, goes off after the fuse, kills it and hurts Lara.
TEST(Gadgets, GrenadeKillsDummyAndHurtsLara) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    auto w = makeWorld(m, {0, 0, 0});
    const auto& g = w->anims().grenade;
    ASSERT_EQ(w->grenades, g.maxCarried);
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
    EXPECT_EQ(w->grenades, g.maxCarried - 1);
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
    EXPECT_EQ(g->maxCarried, 6);
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
    EXPECT_TRUE(game::loadUnitData(bf, "l4_pool").flashlightSpot);
    EXPECT_FALSE(game::loadUnitData(bf, "l3_main_bedroom").flashlightSpot);
    EXPECT_TRUE(game::loadLaraAnims(bf).find("g_grenade_thrower:combat_grenade_Throw1"));
}
