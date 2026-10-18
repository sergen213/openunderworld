// Combat: health/fall damage/death, hitscan, lock-on, ammo, render-rate independence (src/game/Combat.*).
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"

#include <gtest/gtest.h>

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
    EXPECT_NEAR(t.hp, 700 - float(t.hits) * w->anims().pistols.damageAt(dist), 3.0f * float(t.hits));

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
        EXPECT_GE(c.clip[0], 0);
        EXPECT_GE(c.clip[1], 0);
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

TEST(Combat, InputScriptCombatActions) {
    const auto s = game::InputScript::parse("0 draw\n0.1 draw 0\n0.2 aim 1\n0.3 fire 1\n0.4 switch 1\n");
    EXPECT_TRUE(s.at(0).draw);
    EXPECT_FALSE(s.at(6).draw);
    EXPECT_TRUE(s.at(12).aim);
    EXPECT_TRUE(s.at(18).fire);
    EXPECT_TRUE(s.at(24).switchTarget);
}

// ---- real data (OU_GAME_DIR) --------------------------------------------------------------------
TEST(CombatReal, WeaponData) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(dir);
    const auto p = game::loadWeapon(bf, "wp_handgun");
    ASSERT_TRUE(p);
    const game::WeaponDef def; // documented defaults = decoded pistols
    EXPECT_EQ(p->ammo, "pr_handgunbullet");
    EXPECT_EQ(p->clip, def.clip);
    EXPECT_EQ(p->pellets, 1);
    EXPECT_TRUE(p->infinite);
    EXPECT_TRUE(p->dual);
    EXPECT_EQ(p->mode, 1);
    EXPECT_EQ(p->shortRange, def.shortRange);
    EXPECT_EQ(p->maxRange, def.maxRange);
    EXPECT_EQ(p->spreadDeg, def.spreadDeg);
    for (int i = 0; i < 3; ++i) EXPECT_EQ(p->damage[i], def.damage[i]);
    const auto uzi = game::loadWeapon(bf, "wp_uzi"), shotgun = game::loadWeapon(bf, "wp_shotgun");
    ASSERT_TRUE(uzi && shotgun);
    EXPECT_EQ(uzi->ammo, "pr_uzibullet");
    EXPECT_EQ(uzi->clip, 40);
    EXPECT_FALSE(uzi->infinite);
    EXPECT_TRUE(uzi->dual);
    EXPECT_EQ(shotgun->pellets, 7);
    EXPECT_EQ(shotgun->spreadDeg, 15);
    const auto anims = game::loadLaraAnims(bf);
    for (const auto n : game::kPistolClipNames) EXPECT_TRUE(anims.find(n)) << n;
    EXPECT_EQ(anims.find("combat_pistol_RfireE")->durationSeconds(), 10.0 / 30.0);
}
