// Enemy AI (src/game/ai): perception, chase, attack timing, hit/death, ledge avoidance, ranged fire with line of
// fire, flyers, death clips, render-rate independence; real data: decoded senses / attack / death records, enemy
// intros of the surveyed units resolve to skinned enemy types and spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

using namespace ou;
using collision::CollisionMesh;
using game::vec3;
using game::ai::Mode;
using game::ai::Role;

namespace {

constexpr float kDt = float(game::kTickSeconds);

void addTri(CollisionMesh& m, vec3 a, vec3 b, vec3 c) {
    const auto base = uint32_t(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {a, b, c});
    m.faces.push_back({{base, base + 1, base + 2}});
}
void addQuad(CollisionMesh& m, vec3 a, vec3 b, vec3 c, vec3 d) {
    addTri(m, a, b, c);
    addTri(m, a, c, d);
}
void addFloor(CollisionMesh& m, float x0, float y0, float x1, float y1, float z) {
    for (float x = x0; x < x1; x += 500)
        for (float y = y0; y < y1; y += 500)
            addQuad(m, {x, y, z}, {std::min(x + 500, x1), y, z}, {std::min(x + 500, x1), std::min(y + 500, y1), z}, {x, std::min(y + 500, y1), z});
}
// Wall: box [lo, hi] (top + four sides).
void addBox(CollisionMesh& m, vec3 lo, vec3 hi) {
    addFloor(m, lo.x, lo.y, hi.x, hi.y, hi.z);
    addQuad(m, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {lo.x, lo.y, hi.z});
    addQuad(m, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
    addQuad(m, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
    addQuad(m, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
}
CollisionMesh built(CollisionMesh m) {
    m.build();
    return m;
}

// Clip whose root moves forward (model -Y) by `perKey` units per 1/30 s key.
anim::Clip rootClip(int keys, float perKey) {
    anim::Clip c;
    c.keyCount = uint16_t(keys);
    c.timePerKey = 100;
    c.tracks.resize(1);
    if (perKey != 0)
        for (int i = 0; i < keys; ++i) c.tracks[0][anim::TransY].push_back(-perKey * float(i + 1));
    return c;
}

// Synthetic panther: one bone; run 600 units/s; 1 s attack (hit window 0.35-0.6 s); 0.5 s hit; 1 s death.
std::shared_ptr<const game::ai::EnemyType> syntheticType() {
    auto t = std::make_shared<game::ai::EnemyType>();
    t->name = "synthetic";
    t->skeleton.bones.resize(1);
    const std::pair<Role, anim::Clip> clips[] = {{Role::Idle, rootClip(40, 0)},  {Role::Alert, rootClip(40, 0)},
                                                 {Role::Run, rootClip(20, 20)}, {Role::Attack, rootClip(30, 0)},
                                                 {Role::Hit, rootClip(15, 0)},  {Role::Death, rootClip(30, 0)}};
    for (const auto& [r, c] : clips) {
        t->roles[size_t(r)] = std::string(game::ai::kRoleCandidates[size_t(r)][0]);
        t->clips[t->roles[size_t(r)]] = c;
    }
    t->tuning.health = 700;
    t->tuning.staggerDamage = 1; // reacts to every hit
    return t;
}

struct Hurt {
    std::vector<std::pair<int, float>> blows; // tick, damage
    int tick = 0;
    game::ai::HurtFn fn() {
        return [this](float d, vec3, int) { blows.push_back({tick, d}); };
    }
};


constexpr float kDeg = 3.14159265f / 180.0f;

// Synthetic poacher: the decoded poacher numbers (AiReal.DecodedSensesAttacksDeaths) on the synthetic skeleton.
std::shared_ptr<const game::ai::EnemyType> syntheticPoacher() {
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    t->name = "synthetic_poacher";
    t->behaviour = 0, t->weapon = 114;
    t->clips["burst"] = rootClip(20, 0);  // 0.67 s
    t->clips["reload"] = rootClip(40, 0); // 1.33 s
    game::ai::Attack fire;
    fire.node = 0x68, fire.shotClip = "burst", fire.followClip = "reload", fire.damage = 75;
    fire.shots = 3, fire.gap = 5, fire.startDelay = 30, fire.cooldown = 7, fire.maxYaw = 10 * kDeg;
    t->tuning.attacks = {fire};
    t->tuning.senses.eye = {5720, 45 * kDeg, 30 * kDeg};
    t->tuning.senses.body = {{{5689, 10 * kDeg, 25 * kDeg}, {3792, 180 * kDeg, 30 * kDeg}}};
    t->tuning.combatSenses = t->tuning.senses;
    t->tuning.attackDamage = 100;
    t->tuning.health = 500;
    return t;
}

// Synthetic bat: flyer (behaviour 2), bat senses, 0.37 s bite of 75.
std::shared_ptr<const game::ai::EnemyType> syntheticBat() {
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    t->name = "synthetic_bat";
    t->behaviour = 2;
    t->clips[t->roles[size_t(Role::Attack)]] = rootClip(11, 0);
    t->clips[t->roles[size_t(Role::Death)]] = rootClip(24, 0);
    auto& u = t->tuning;
    u.height = 150, u.health = 2, u.attackDamage = 75;
    u.attackRange = u.radius + 150, u.attackReach = u.radius + 250, u.attackCooldown = 1.5f;
    u.senses.eye = {6321, kDeg * 180, kDeg * 90};
    u.senses.body = {{{3792, kDeg * 180, kDeg * 90}, {}}};
    u.senses.ray = false;
    u.combatSenses = u.senses;
    u.combatSenses.eye.range = u.combatSenses.body[0].range = 4740;
    return t;
}

// Synthetic shark: the same flyer family, distinguished by its swimming clips rather than its object name.
std::shared_ptr<const game::ai::EnemyType> syntheticShark() {
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticBat());
    t->name = "synthetic_shark";
    t->roles[size_t(Role::Run)] = "swim_forward_fast";
    t->clips["swim_forward_fast"] = rootClip(20, 20);
    t->roles[size_t(Role::Attack)] = "burst_attack";
    t->clips["burst_attack"] = rootClip(21, 0);
    t->tuning.health = 500;
    return t;
}

game::Hit shotFrom(vec3 dir, float damage = 1000, bool headshot = false) { return {damage, {}, dir, game::Team::Player, headshot}; }
} // namespace

TEST(Ai, PerceptionConeDistanceAndWalls) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto open = built(m);
    addBox(m, {900, -1000, 0}, {1000, 1000, 1500});
    const auto walled = built(m);
    game::ai::Enemy e(syntheticType(), 1, {0, 0, 0}, 0); // facing +X
    const auto& s = e.type().tuning.senses; // synthetic: the [I] defaults (eye 10 m ±60°, 2.5 m all round)
    EXPECT_TRUE(e.perceives(open, {{2000, 0, 0}}));
    EXPECT_FALSE(e.perceives(walled, {{2000, 0, 0}}));                      // wall in the line of sight
    EXPECT_FALSE(e.perceives(open, {{s.eye.range + 100, 0, 0}}));           // too far
    EXPECT_FALSE(e.perceives(open, {{-2000, 0, 0}}));                       // behind, outside the sense radius
    EXPECT_TRUE(e.perceives(open, {{-(s.body[0].range - 50), 0, 0}}));     // behind but close: sensed
    EXPECT_FALSE(e.perceives(open, {{1000, 1000 * std::tan(s.eye.yaw + 0.1f), 0}})); // outside the cone
    EXPECT_TRUE(e.perceives(open, {{1000, 1000 * std::tan(s.eye.yaw - 0.1f), 0}}));
}

TEST(Ai, NoticesChasesAndAttacksWithHitWindow) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticType(), 1, {0, 0, 0}, 0);
    const game::ai::Target lara{{2500, 300, 0}};
    Hurt hurt;
    int noticed = -1, attack = -1;
    for (hurt.tick = 0; hurt.tick < 600 && hurt.blows.size() < 2; ++hurt.tick) {
        es.tick(col, lara, hurt.fn(), kDt);
        if (noticed < 0 && e.state().mode != Mode::Idle) noticed = hurt.tick;
        if (attack < 0 && e.state().mode == Mode::Attack) attack = hurt.tick;
    }
    EXPECT_EQ(noticed, 0);
    ASSERT_GE(attack, 0);
    const float d = glm::length(glm::vec2(lara.pos - e.state().pos));
    EXPECT_LE(d, e.type().tuning.attackRange + 1);          // converged into attack range
    EXPECT_GE(d, e.type().tuning.radius + 100.0f - 1);      // without walking into Lara
    ASSERT_EQ(hurt.blows.size(), 2u);
    // First blow inside the hit window of the 1 s attack clip (35 %..60 % -> ticks 21..36 after it starts).
    const int into = hurt.blows[0].first - attack;
    EXPECT_GE(into, 20);
    EXPECT_LE(into, 37);
    EXPECT_FLOAT_EQ(hurt.blows[0].second, e.type().tuning.attackDamage);
    // One blow per attack: the next lands a full clip + cooldown later.
    EXPECT_GE(hurt.blows[1].first - hurt.blows[0].first, int((1.0f + e.type().tuning.attackCooldown) / kDt) - 2);
}

TEST(Ai, HitReactionThenDeath) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    std::vector<game::Damageable*> targets;
    game::ai::Enemies es(&targets);
    auto& e = es.add(syntheticType(), 1, {0, 0, 0}, 0);
    ASSERT_EQ(targets.size(), 1u);
    EXPECT_EQ(targets[0]->team(), game::Team::Enemy);
    targets[0]->onHit({40, {0, 0, 300}, {-1, 0, 0}, game::Team::Player});
    EXPECT_FLOAT_EQ(e.health(), 660);
    EXPECT_EQ(e.state().mode, Mode::Hit);
    EXPECT_EQ(e.state().clip, e.type().clipName(Role::Hit));
    for (int i = 0; i < 40; ++i) es.tick(col, {{3000, 3000, 0}}, {}, kDt);
    EXPECT_EQ(e.state().mode, Mode::Chase); // reacted, then hunts the shooter
    while (e.alive()) e.onHit({40, {}, {-1, 0, 0}, game::Team::Player});
    EXPECT_EQ(e.state().mode, Mode::Dead);
    const vec3 at = e.state().pos;
    for (int i = 0; i < 120; ++i) es.tick(col, {{300, 0, 0}}, [](float, vec3, int) { FAIL() << "dead enemies do not attack"; }, kDt);
    EXPECT_EQ(e.state().mode, Mode::Dead);
    EXPECT_EQ(e.state().pos, at);
    EXPECT_EQ(e.state().clipTime, e.type().clip(Role::Death)->durationSeconds()); // holds the last frame
}

TEST(Ai, LedgesAreAvoided) {
    // Enemy on a 600-high platform (x < 1000, deeper than maxDrop); Lara below it, in sight past the edge.
    CollisionMesh m;
    addBox(m, {-3000, -3000, -600}, {1000, 3000, 0});
    addFloor(m, 1000, -3000, 6000, 3000, -600);
    const auto col = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticType(), 1, {0, 0, 0}, 0);
    for (int i = 0; i < 600; ++i) es.tick(col, {{2500, 0, -600}}, {}, kDt);
    EXPECT_NE(e.state().mode, Mode::Idle);    // saw her
    EXPECT_EQ(e.state().pos.z, 0);             // never stepped off
    EXPECT_GT(e.state().pos.x, 700);            // ran up to the edge
    EXPECT_LT(e.state().pos.x, 1000);
}

TEST(Ai, SimulationIndependentOfRenderRate) {
    auto run = [](double renderHz) {
        CollisionMesh m;
        addFloor(m, -6000, -6000, 6000, 6000, 0);
        addBox(m, {1500, -300, 0}, {1700, 300, 600}); // pillar to slide around
        const auto col = built(m);
        game::ai::Enemies es;
        auto& e = es.add(syntheticType(), 1, {0, 0, 0}, 0.3f);
        Hurt hurt;
        engine::FrameClock clock(game::kTickSeconds);
        int ticks = 0;
        while (ticks < 360)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && ticks < 360; --n, ++ticks) {
                hurt.tick = ticks;
                const float t = float(ticks) * kDt; // Lara walks a circle
                es.tick(col, {{3000 * std::cos(t * 0.4f), 3000 * std::sin(t * 0.4f), 0}}, hurt.fn(), kDt);
            }
        return std::make_pair(e.state(), hurt.blows);
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.first.pos, o.first.pos);
        EXPECT_EQ(a.first.yaw, o.first.yaw);
        EXPECT_EQ(a.first.mode, o.first.mode);
        EXPECT_EQ(a.first.clip, o.first.clip);
        EXPECT_EQ(a.second, o.second);
    }
    EXPECT_GT(glm::length(glm::vec2(a.first.pos)), 500); // it actually chased
}

// Lara's death resets the enemies to the last checkpoint: those dead when it was reached stay dead, the rest are
// back at their placement at full health; generator spawns go and the generator fires again.
TEST(Ai, ResetOnRespawnKeepsCheckpointDead) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    std::vector<game::Damageable*> targets;
    game::ai::Enemies es(&targets);
    es.addType(syntheticType());
    auto& a = es.add(syntheticType(), 1, {0, 0, 0}, 0);
    auto& b = es.add(syntheticType(), 2, {0, 2000, 0}, 0);
    es.addGenerator({9, "", "synthetic", {-1500, 0, 0}, 0});
    while (b.alive()) b.onHit({100, {}, {1, 0, 0}, game::Team::Player});
    es.checkpoint();
    a.onHit({100, {}, {1, 0, 0}, game::Team::Player});
    for (int i = 0; i < 120; ++i) es.tick(col, {{-1000, 0, 0}}, {}, kDt);
    ASSERT_EQ(es.all().size(), 3u); // the generator fired
    EXPECT_NE(a.state().pos, vec3(0));
    es.reset();
    ASSERT_EQ(es.all().size(), 2u);
    EXPECT_EQ(targets.size(), 2u);
    EXPECT_EQ(a.state().pos, vec3(0));
    EXPECT_EQ(a.state().mode, Mode::Idle);
    EXPECT_EQ(a.health(), 700);
    EXPECT_EQ(b.state().mode, Mode::Dead);
    EXPECT_FALSE(es.generators()[0].fired);
    es.tick(col, {{-1000, 0, 0}}, {}, kDt);
    EXPECT_EQ(es.all().size(), 3u);
}

// A poacher shot at from behind a wall cannot see Lara: it walks towards where the shot came from and never hurts
// her. With the wall gone it stands, waits the start delay (30 counts) and fires bursts of 3 shots 5 counts apart
// ([I] 30 Hz counts), 75 per hit; then reload and the 7 s record cooldown before the next burst.
TEST(Ai, RangedFireNeedsLineOfSight) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto open = built(m);
    addBox(m, {800, -400, 0}, {900, 400, 1500});
    const auto walled = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticPoacher(), 1, {0, 0, 0}, 0);
    const game::ai::Target lara{{3000, 0, 0}};
    Hurt hurt;
    e.onHit(shotFrom({-1, 0, 0}, 1));
    for (hurt.tick = 0; hurt.tick < 180; ++hurt.tick) {
        es.tick(walled, lara, hurt.fn(), kDt);
        EXPECT_TRUE(es.shots.empty());
    }
    EXPECT_TRUE(hurt.blows.empty());
    EXPECT_GT(e.state().pos.x, 300); // repositioned towards the shooter, up to the wall
    EXPECT_LT(e.state().pos.x, 800);
    std::vector<std::pair<int, game::ai::Shot>> shots;
    int fireTick = -1;
    for (; hurt.tick < 180 + 900; ++hurt.tick) {
        es.tick(open, lara, hurt.fn(), kDt);
        if (fireTick < 0 && e.state().mode == Mode::Fire) fireTick = hurt.tick;
        for (const auto& s : es.shots) shots.push_back({hurt.tick, s});
    }
    ASSERT_GE(fireTick, 0);
    ASSERT_EQ(shots.size(), 6u); // two bursts in 15 s
    EXPECT_NEAR(shots[0].first - fireTick, 30 * 2, 1); // start delay
    EXPECT_NEAR(shots[1].first - shots[0].first, 5 * 2, 1);
    EXPECT_NEAR(shots[2].first - shots[1].first, 5 * 2, 1);
    EXPECT_GE(shots[3].first - shots[0].first, int((0.667f + 7.0f + 1.0f) / kDt) - 2); // clip, cooldown, start delay
    size_t hits = 0;
    for (const auto& [tick, s] : shots) {
        hits += s.hit;
        if (s.hit) { EXPECT_LT(glm::length(glm::vec2(s.to - lara.pos)), 101.0f); }
    }
    EXPECT_GE(hits, 1u);
    EXPECT_EQ(hurt.blows.size(), hits);
    for (const auto& b : hurt.blows) EXPECT_EQ(b.second, 75);
}

// Line of fire: shots fired after a wall comes between them stop at the wall and do no damage.
TEST(Ai, ShotsStopAtWalls) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto open = built(m);
    addBox(m, {1500, -1000, 0}, {1600, 1000, 1500});
    const auto walled = built(m);
    game::ai::Enemies es;
    es.add(syntheticPoacher(), 1, {0, 0, 0}, 0);
    const game::ai::Target lara{{3000, 0, 0}};
    Hurt hurt;
    for (hurt.tick = 0; hurt.tick < 300 && es.shots.empty(); ++hurt.tick) es.tick(open, lara, hurt.fn(), kDt);
    ASSERT_EQ(es.shots.size(), 1u);
    const size_t before = hurt.blows.size();
    std::vector<game::ai::Shot> later;
    for (int i = 0; i < 40; ++i, ++hurt.tick) {
        es.tick(walled, lara, hurt.fn(), kDt);
        later.insert(later.end(), es.shots.begin(), es.shots.end());
    }
    ASSERT_EQ(later.size(), 2u); // the rest of the burst
    for (const auto& s : later) {
        EXPECT_FALSE(s.hit);
        EXPECT_LE(s.to.x, 1501.0f);
    }
    EXPECT_EQ(hurt.blows.size(), before);
}

// A bat hovering 1000 up notices Lara (all-round 3792), flies at her head, bites (75) once per 1.5 s [I]; shot, it
// plays its death clip and drops to the floor.
TEST(Ai, BatFliesToLaraAndBites) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticBat(), 1, {0, 0, 1000}, 0);
    const game::ai::Target lara{{2500, 300, 0}};
    Hurt hurt;
    float lowest = 1e9f, reachAtBite = 0;
    for (hurt.tick = 0; hurt.tick < 900 && hurt.blows.size() < 2; ++hurt.tick) {
        es.tick(col, lara, hurt.fn(), kDt);
        lowest = std::min(lowest, e.state().pos.z);
        if (hurt.blows.size() == 1 && reachAtBite == 0)
            reachAtBite = glm::distance(e.state().pos + vec3(0, 0, 75), lara.pos + vec3(0, 0, 512 * 0.85f));
    }
    ASSERT_EQ(hurt.blows.size(), 2u);
    EXPECT_EQ(hurt.blows[0].second, 75);
    EXPECT_LE(reachAtBite, e.type().tuning.attackReach);
    EXPECT_GE(hurt.blows[1].first - hurt.blows[0].first, int((0.37f + 1.5f) / kDt) - 2);
    EXPECT_GT(lowest, 100); // flew at her head, never down to the floor
    e.onHit(shotFrom({-1, 0, 0}, 10));
    EXPECT_EQ(e.state().mode, Mode::Dead);
    for (int i = 0; i < 180; ++i) es.tick(col, lara, {}, kDt);
    EXPECT_EQ(e.state().pos.z, 0); // fell to the floor
}

// An aquatic flyer uses the same 3D sphere collision as a bat but cannot chase out of its type-10 water box.
TEST(Ai, SharkStaysInsideWaterVolume) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, -2000);
    m.markup.push_back({10, 1, {{-1000, -1000, -1000}, {1000, 1000, 1000}}});
    const auto col = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticShark(), 1, {0, 0, 0}, 0);
    for (int i = 0; i < 600; ++i) es.tick(col, {{3000, 0, 3000}, true, 512, 100, true}, {}, kDt); // swimming: sharks hunt her
    const float r = e.type().tuning.radius, half = e.type().tuning.height * 0.5f;
    EXPECT_GT(e.state().pos.x, 500); // pursued the visible target to the boundary
    EXPECT_LE(e.state().pos.x, 1000 - r + 1e-3f);
    EXPECT_LE(e.state().pos.z + half, 1000 - r + 1e-3f);
    EXPECT_EQ(e.state().mode, Mode::Chase); // swimming, not falling or walking on the floor below
    EXPECT_GT(e.state().pos.z, -1000 - half);
}

// [I] A shark does not sense Lara out of the water (on a deck above it): it never starts hunting her.
TEST(Ai, SharkIgnoresLaraOutOfWater) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, -2000);
    const auto col = built(m);
    game::ai::Enemies es;
    auto& e = es.add(syntheticShark(), 1, {0, 0, -1000}, 0);
    for (int i = 0; i < 600; ++i) es.tick(col, {{1500, 0, 50}}, {}, kDt);
    EXPECT_EQ(e.state().mode, Mode::Idle);
    for (int i = 0; i < 60; ++i) es.tick(col, {{1500, 0, 50}, true, 512, 100, true}, {}, kDt);
    EXPECT_NE(e.state().mode, Mode::Idle); // the same Lara swimming is seen
}

// Death clips as tru.exe FUN_00642210: by the attacker's quadrant (left -> category 3, behind -> 5) with the
// heading turned to match; head shot -> flag 3; hit while running -> flag 5; nothing usable -> the role clip. Clips
// ending upright tip the body onto its side in 0.5 s [I]; lying ones do not.
TEST(Ai, DeathClipByQuadrantAndFlags) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    for (const char* n : {"death_left", "death_back", "death_adren", "death_running"}) t->clips[n] = rootClip(30, 0);
    t->tuning.deaths = {{"death_left", 3, 0}, {"death_back", 5, 0}, {"", 1, 0}, {"death_adren", 1, 3}, {"death_running", 1, 5}};
    t->lyingDeaths = {"death_left"};
    struct Case { vec3 dir; bool head, running; const char* clip; float yaw; };
    for (const Case c : {Case{{0, -1, 0}, false, false, "death_left", 0}, Case{{1, 0, 0}, false, false, "death_back", 0},
                         Case{{-1, 0, 0}, false, false, "death_static", 0}, Case{{-1, 0, 0}, true, false, "death_adren", 0},
                         Case{{-1, 0, 0}, false, true, "death_running", 0}}) {
        game::ai::Enemies es;
        auto& e = es.add(t, 1, {0, 0, 0}, 0);
        if (c.running)
            for (int i = 0; i < 60 && e.state().mode != Mode::Chase; ++i) es.tick(col, {{3000, 0, 0}}, {}, kDt);
        e.onHit(shotFrom(c.dir, 1000, c.head));
        EXPECT_EQ(e.state().mode, Mode::Dead);
        EXPECT_EQ(e.state().clip, c.clip);
        EXPECT_NEAR(std::remainder(e.state().yaw - c.yaw, 6.2831853f), 0, 1e-4f) << c.clip;
        for (int i = 0; i < 120; ++i) es.tick(col, {{3000, 0, 0}}, {}, kDt);
        EXPECT_FLOAT_EQ(e.state().tip, std::string(c.clip) == "death_left" ? 0.0f : 1.5707963f) << c.clip;
    }
}

TEST(Ai, PoacherAndBatIndependentOfRenderRate) {
    auto run = [](double renderHz) {
        CollisionMesh m;
        addFloor(m, -6000, -6000, 6000, 6000, 0);
        addBox(m, {1500, -300, 0}, {1700, 300, 600});
        const auto col = built(m);
        game::ai::Enemies es;
        auto& p = es.add(syntheticPoacher(), 1, {0, 0, 0}, 0.3f);
        auto& b = es.add(syntheticBat(), 2, {-1000, 500, 800}, 0);
        Hurt hurt;
        std::vector<std::tuple<int, vec3, vec3, bool>> shots;
        engine::FrameClock clock(game::kTickSeconds);
        int ticks = 0;
        while (ticks < 900)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && ticks < 900; --n, ++ticks) {
                hurt.tick = ticks;
                const float t = float(ticks) * kDt; // Lara walks a circle
                es.tick(col, {{3000 * std::cos(t * 0.3f), 3000 * std::sin(t * 0.3f), 0}}, hurt.fn(), kDt);
                for (const auto& s : es.shots) shots.push_back({ticks, s.from, s.to, s.hit});
            }
        return std::make_tuple(p.state(), b.state(), hurt.blows, shots);
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        for (int k = 0; k < 2; ++k) {
            const auto& x = k ? std::get<1>(a) : std::get<0>(a);
            const auto& y = k ? std::get<1>(o) : std::get<0>(o);
            EXPECT_EQ(x.pos, y.pos);
            EXPECT_EQ(x.yaw, y.yaw);
            EXPECT_EQ(x.mode, y.mode);
            EXPECT_EQ(x.clip, y.clip);
        }
        EXPECT_EQ(std::get<2>(a), std::get<2>(o));
        EXPECT_EQ(std::get<3>(a), std::get<3>(o));
    }
    EXPECT_FALSE(std::get<3>(a).empty()); // the poacher fired
    EXPECT_FALSE(std::get<2>(a).empty()); // something hurt Lara
}

// Patrol (an intro path in ping-pong order: PatrolMove, the point's idle, PatrolIncr), alert on seeing Lara, chase,
// search after losing her (to where she was, idles, possible positions), then back on its path.
TEST(Ai, PatrolsAlertsSearchesAndReturnsToPost) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    t->roles[size_t(Role::Walk)] = "walk_forward_2";
    t->clips["walk_forward_2"] = rootClip(20, 5); // 150 units/s
    t->clips["look"] = rootClip(30, 0);
    t->patrol.idles = {{"look", 1, 1.0f, 1.0f}};
    t->patrol.arrive = 100;
    t->search.idles = {{"look", 1, 1.0f, 1.0f}};
    t->search.time = 12, t->search.step = 600, t->search.angle = 1.5f, t->search.arrive = 100;
    game::ai::Enemies es;
    auto& e = es.add(t, 1, {0, 0, 0}, 0);
    const std::vector<game::ai::PatrolPoint> path{{{1500, 0, 0}}, {{1500, 1500, 0}}};
    e.patrolPaths = {path};
    e.patrolMode = 2; // ping-pong
    e.setPatrol(0);
    const game::ai::Target away{{0, 0, -50000}};
    std::vector<Mode> modes;
    std::array<float, 2> nearest{1e9f, 1e9f};
    const auto run = [&](const game::ai::Target& lara, float seconds) {
        for (int i = 0; i < int(seconds / kDt); ++i) {
            es.tick(col, lara, {}, kDt);
            if (modes.empty() || modes.back() != e.state().mode) modes.push_back(e.state().mode);
            for (size_t k = 0; k < 2; ++k) nearest[k] = std::min(nearest[k], glm::length(glm::vec2(e.position() - path[k].pos)));
        }
    };
    run(away, 26); // 10 s to the first point, 1 s there, 10 s to the second, 1 s, back
    EXPECT_LT(nearest[0], 100.5f);
    EXPECT_LT(nearest[1], 100.5f);
    EXPECT_EQ(e.state().mode, Mode::Patrol);
    EXPECT_EQ(e.state().patrolIndex, 0); // ping-pong: back towards the first point
    const vec3 seen = e.position() + vec3(std::cos(e.state().yaw), std::sin(e.state().yaw), 0) * 1500.0f;
    run({seen}, 1);    // in front of it: alert, then chase
    run(away, 5);      // gone for longer than the 4 s lose time: search
    EXPECT_EQ(e.state().mode, Mode::Search);
    EXPECT_LT(glm::length(glm::vec2(e.state().lastSeen - seen)), 1.0f);
    run(away, 7);
    EXPECT_LT(glm::length(glm::vec2(e.position() - seen)), 1000.0f); // went to where she was, looking round near it
    nearest = {1e9f, 1e9f};
    run(away, 30); // search over (12 s): back on its path
    EXPECT_EQ(e.state().mode, Mode::Patrol);
    EXPECT_LT(std::min(nearest[0], nearest[1]), 100.5f);
    const std::vector<Mode> order{Mode::Patrol, Mode::Alert, Mode::Chase, Mode::Search, Mode::Idle, Mode::Patrol};
    size_t k = 0;
    for (const Mode md : modes) k += k < order.size() && md == order[k];
    EXPECT_EQ(k, order.size());
    for (const Mode md : modes) std::fprintf(stderr, "%s ", std::string(game::ai::modeName(md)).c_str());
    std::fprintf(stderr, "\n");
}

namespace {
// Nav graph area from CCW corners; adjacency filled by navLink.
formats::NavGraph::Area navArea(std::vector<vec3> v) {
    formats::NavGraph::Area a;
    for (const vec3 p : v) a.centre += p / float(v.size());
    a.verts = std::move(v);
    a.adj.assign(a.verts.size(), -1);
    return a;
}
// Joins every pair of areas sharing an edge (reversed vertex order).
void navLink(formats::NavGraph& g) {
    for (size_t a = 0; a < g.areas.size(); ++a)
        for (size_t k = 0; k < g.areas[a].verts.size(); ++k)
            for (size_t b = 0; b < g.areas.size(); ++b)
                for (size_t l = 0; l < g.areas[b].verts.size() && b != a; ++l) {
                    const auto& va = g.areas[a].verts;
                    const auto& vb = g.areas[b].verts;
                    if (va[k] == vb[(l + 1) % vb.size()] && va[(k + 1) % va.size()] == vb[l]) g.areas[a].adj[k] = int32_t(b);
                }
}
// An L of three 1000-unit squares: A [0,1]x[0,1], B [0,1]x[1,2], C [1,2]x[1,2] (thousands); [1,2]x[0,1] is a wall.
formats::NavGraph lGraph() {
    formats::NavGraph g;
    g.radius = 100;
    g.areas = {navArea({{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}, {0, 1000, 0}}),
               navArea({{0, 1000, 0}, {1000, 1000, 0}, {1000, 2000, 0}, {0, 2000, 0}}),
               navArea({{1000, 1000, 0}, {2000, 1000, 0}, {2000, 2000, 0}, {1000, 2000, 0}})};
    navLink(g);
    return g;
}
} // namespace

// The nav mesh routes around the missing square through the corner both areas share; off the mesh there is no route.
TEST(Ai, NavMeshRoutesAroundAWall) {
    game::ai::NavMesh nav;
    nav.add(lGraph(), vec3(0));
    EXPECT_EQ(nav.locate({500, 500, 30}), 0);
    EXPECT_EQ(nav.locate({1500, 1500, 0}), 2);
    EXPECT_EQ(nav.locate({1500, 500, 0}), -1);
    EXPECT_EQ(nav.locate({500, 500, 900}), -1); // too far above the floor
    const auto p = nav.path({500, 500, 0}, {1800, 1100, 0}); // the straight line would cross the missing square
    ASSERT_EQ(p.size(), 2u);
    EXPECT_LT(glm::distance(p[0], vec3(1000, 1000, 0)), 1.0f); // the inner corner
    EXPECT_EQ(p[1], vec3(1800, 1100, 0));
    EXPECT_EQ(nav.path({500, 500, 0}, {500, 1500, 0}).size(), 1u); // straight through the shared edge
    EXPECT_TRUE(nav.path({500, 500, 0}, {1500, 500, 0}).empty());
    EXPECT_LT(glm::distance(nav.steer({500, 500, 0}, {1800, 1100, 0}).at, vec3(1000, 1000, 0)), 1.0f);
}

namespace {
// Two 1000-unit floors 1000 apart in y, the far one 800 lower, and a one-way strip from the near one's edge
// (y 1000) down to the far one's (y 2000).
std::pair<formats::NavGraph, formats::NavLink> gapGraph() {
    formats::NavGraph g;
    g.radius = 100;
    g.areas = {navArea({{0, 0, 0}, {1000, 0, 0}, {1000, 1000, 0}, {0, 1000, 0}}),
               navArea({{0, 2000, -800}, {1000, 2000, -800}, {1000, 3000, -800}, {0, 3000, -800}})};
    formats::NavLink l;
    l.strip = true, l.fromA = true, l.usage = 0xFFFF, l.maxSnap = 632.1f, l.mayUse = 474.1f;
    l.a = {vec3(200, 1000, 0), vec3(800, 1000, 0)}, l.b = {vec3(200, 2000, -800), vec3(800, 2000, -800)};
    return {g, l};
}
} // namespace

// NavPower links join areas no edge joins: a route takes the strip only with a usage bit in common, one way, at
// the take-off point closest to where it walks from, landing straight across.
TEST(Ai, NavMeshRoutesOverALink) {
    const auto [g, l] = gapGraph();
    game::ai::NavMesh nav;
    nav.add(g, vec3(0));
    nav.addLink(l, vec3(0));
    ASSERT_EQ(nav.links(), 1u);
    EXPECT_TRUE(nav.route({500, 500, 0}, {500, 2500, -800}, 0).empty()); // no link usage: no route
    const auto r = nav.route({300, 500, 0}, {300, 2500, -800}, 4);
    ASSERT_EQ(r.size(), 2u);
    EXPECT_EQ(r[0].link, 0);
    EXPECT_FALSE(r[0].reverse);
    EXPECT_LT(glm::distance(r[0].at, vec3(300, 1000, 0)), 1.0f);
    EXPECT_LT(glm::distance(r[0].land, vec3(300, 2000, -800)), 1.0f);
    EXPECT_EQ(r[1].at, vec3(300, 2500, -800));
    EXPECT_TRUE(nav.route({300, 2500, -800}, {300, 500, 0}, 4).empty()); // one way
    EXPECT_EQ(nav.steer({300, 990, 0}, {300, 2500, -800}, 4).link, 0); // a take-off is never skipped as too near
}

// A cat chasing Lara across the gap runs to the take-off, faces the landing, plays jump_down_start, flies with
// jump_down_inair_static, lands with jump_down_end on the far floor and chases on; without link usage it waits at the
// edge (ledge avoidance).
TEST(Ai, CatJumpsDownALink) {
    const auto [g, l] = gapGraph();
    game::ai::NavMesh nav;
    nav.add(g, vec3(0));
    nav.addLink(l, vec3(0));
    CollisionMesh m;
    addFloor(m, -500, -500, 1500, 1000, 0);
    addFloor(m, -500, 2000, 1500, 3500, -800);
    const auto col = built(m);
    const auto run = [&](uint16_t usage, std::vector<std::string>* clips) {
        auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
        t->linkUsage = usage;
        for (const char* c : {"jump_down_start", "jump_down_inair_static", "jump_down_end"}) t->clips.emplace(c, t->clips.at(t->clipName(Role::Idle)));
        t->jumpDown = {"jump_down_start", "jump_down_inair_static", "jump_down_end"};
        game::ai::Enemy e(t, 1, {300, 300, 0}, 1.5708f);
        e.nav = &nav;
        const game::ai::Target lara{{300, 2600, -800}};
        e.alert(lara.pos);
        std::vector<game::ai::EnemyState> states;
        for (int i = 0; i < int(10 / kDt); ++i) {
            e.tick(col, lara, {}, kDt);
            if (clips && (clips->empty() || clips->back() != e.state().clip)) clips->push_back(e.state().clip);
            states.push_back(e.state());
        }
        return states;
    };
    std::vector<std::string> clips;
    const auto a = run(4, &clips);
    for (const auto& c : clips) std::fprintf(stderr, "%s ", c.c_str());
    std::fprintf(stderr, "\n");
    const auto at = [&](const char* c) { return std::find(clips.begin(), clips.end(), c) - clips.begin(); };
    ASSERT_LT(at("jump_down_start"), at("jump_down_inair_static"));
    ASSERT_LT(at("jump_down_inair_static"), at("jump_down_end"));
    ASSERT_LT(at("jump_down_end"), std::ssize(clips));
    const auto air = std::find_if(a.begin(), a.end(), [](const auto& s) { return s.leap == 2; });
    ASSERT_NE(air, a.end());
    EXPECT_LT(std::abs(air->leapFrom.y - 1000), 200.0f); // took off at the edge (within its radius + 60)
    EXPECT_EQ(air->leapTo, vec3(300, 2000, -800));
    EXPECT_NEAR(a.back().pos.z, -800, 1.0f); // on the far floor, near Lara
    EXPECT_GT(a.back().pos.y, 2000);
    EXPECT_EQ(a.back().leap, 0);
    const auto b = run(0, nullptr); // a type without link usage stays on its floor
    EXPECT_NEAR(b.back().pos.z, 0, 1.0f);
    EXPECT_TRUE(std::ranges::none_of(b, [](const auto& s) { return s.leap != 0; }));
}

namespace {
// The merc's decoded flank record (every soldier type's: AiReal.ShipNavGraphFlankAndInvestigate).
game::ai::FlankStyle mercFlank() {
    game::ai::FlankStyle f;
    f.valid = f.inside = true;
    f.maxCost = f.closeCost = f.nearCost = 1e11f;
    f.minRange = 1580278.1f, f.maxRange = 0, f.frontRange = 3160.56f, f.frontAngle = 100;
    f.closeDist = 1264, f.nearDist = 252, f.gather = 7901;
    return f;
}
formats::PlannerNode node(vec3 p) {
    formats::PlannerNode n;
    n.frame[3] = glm::vec4(p, 1);
    return n;
}
} // namespace

// After a burst without cover a soldier flanks (EnemyBehavior_Flank): of the planner nodes around it, the one 800 from
// Lara costs closeCost and the one under its feet nearCost, so it runs to the node beside her, along the nav mesh.
TEST(Ai, MercFlanksToANodeBesideLara) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticPoacher());
    t->flank = mercFlank();
    const std::vector<formats::PlannerNode> nodes{node({2200, 0, 0}), node({3000, 1500, 0}), node({50, 0, 0})};
    const auto run = [&](std::vector<Mode>* modes) {
        game::ai::Enemy e(t, 1, {0, 0, 0}, 0);
        e.nodes = nodes; // what Enemies hands over each tick: the loaded units' planner nodes
        const game::ai::Target lara{{3000, 0, 0}};
        std::vector<game::ai::EnemyState> states;
        for (int i = 0; i < int(12 / kDt); ++i) {
            e.tick(col, lara, {}, kDt);
            if (modes && (modes->empty() || modes->back() != e.state().mode)) modes->push_back(e.state().mode);
            states.push_back(e.state());
        }
        return states;
    };
    std::vector<Mode> modes;
    const auto a = run(&modes), b = run(nullptr);
    for (const Mode md : modes) std::fprintf(stderr, "%s ", std::string(game::ai::modeName(md)).c_str());
    std::fprintf(stderr, "\n");
    const auto flanking = std::find_if(a.begin(), a.end(), [](const auto& s) { return s.mode == Mode::Flank; });
    ASSERT_NE(flanking, a.end());
    EXPECT_EQ(flanking->goal, vec3(3000, 1500, 0));
    EXPECT_LT(glm::length(glm::vec2(a.back().pos - vec3(3000, 1500, 0))), 120.0f); // got there (arrival: its radius)
    EXPECT_EQ(a.back().recentCount, 1);
    EXPECT_GT(a.back().flankWait, 0.0f);
    for (size_t i = 0; i < a.size(); ++i) // deterministic
        ASSERT_TRUE(a[i].pos == b[i].pos && a[i].yaw == b[i].yaw && a[i].mode == b[i].mode) << i;
    for (const Mode md : modes) std::fprintf(stderr, "%s ", std::string(game::ai::modeName(md)).c_str());
    std::fprintf(stderr, "\n");
}

// A gunshot (Enemies::disturb) behind a calm enemy with an investigate record: it walks there (InvestigateMove) and
// goes back to idle; a second shot within 1580 of the first does not restart it, one farther does; out of hearing
// (+0x18) it stays put.
TEST(Ai, InvestigatesAGunshot) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    t->clips["walk_forward_2"] = rootClip(20, 5); // 150 units/s
    t->investigate.walk = "walk_forward_2", t->investigate.arrive = 100, t->investigate.hear = 4740.83f;
    game::ai::Enemies es;
    auto& e = es.add(t, 1, {0, 0, 0}, 0);
    auto& deaf = es.add(t, 2, {9000, 0, 0}, 0);
    const game::ai::Target away{{0, 0, -50000}};
    es.disturb({-2000, 0, 0});
    EXPECT_EQ(e.state().mode, Mode::Investigate);
    EXPECT_EQ(deaf.state().mode, Mode::Idle); // 11000 away
    for (int i = 0; i < int(2 / kDt); ++i) es.tick(col, away, {}, kDt);
    const float walked = e.state().phaseTime;
    es.disturb({-2500, 500, 0}); // the same disturbance
    EXPECT_EQ(e.state().phaseTime, walked);
    EXPECT_EQ(e.state().goal, vec3(-2000, 0, 0));
    bool arrived = false;
    for (int i = 0; i < int(20 / kDt) && !arrived; ++i) {
        es.tick(col, away, {}, kDt);
        arrived = e.state().mode == Mode::Idle;
    }
    EXPECT_TRUE(arrived);
    EXPECT_LT(glm::length(glm::vec2(e.position() - vec3(-2000, 0, 0))), 110.0f);
    es.disturb({1000, 0, 0});
    EXPECT_EQ(e.state().mode, Mode::Investigate);
    EXPECT_EQ(e.state().goal, vec3(1000, 0, 0));
}

// Lara inside the sidestep trigger (+0x08) but out of reach: the cat steps to a random side with its strafe clip;
// then waits +0x0C before the next one.
TEST(Ai, SidestepsWhenLaraIsClose) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    for (const auto& [name, x] : {std::pair{"strafe_left", 10.0f}, std::pair{"strafe_right", -10.0f}}) { // model +X is its left
        anim::Clip c = rootClip(15, 0);
        for (int i = 0; i < 15; ++i) c.tracks[0][anim::TransX].push_back(x * float(i + 1));
        t->clips[name] = c;
    }
    t->dodge = {"strafe_left", "strafe_right", 300, 948.17f, 0.5f, 2.0f};
    game::ai::Enemies es;
    auto& e = es.add(t, 1, {0, 0, 0}, 0);
    const game::ai::Target lara{{800, 0, 0}};
    bool dodged = false;
    for (int i = 0; i < int(2 / kDt) && !dodged; ++i) {
        es.tick(col, lara, {}, kDt);
        dodged = e.state().mode == Mode::Dodge;
    }
    ASSERT_TRUE(dodged);
    while (e.state().mode == Mode::Dodge) es.tick(col, lara, {}, kDt);
    EXPECT_GT(std::abs(e.position().y), 100.0f); // stepped sideways
    EXPECT_LT(std::abs(e.position().x), 60.0f);
    EXPECT_GT(e.state().dodgeWait, 0.0f);
}

// PatrolLookTurn: a point whose node has the look flag turns it to the node's facing before the idle; PatrolAttack:
// at a point of mode 2 it attacks a Lara in reach with the patrol record's +0x10 attack, without an alert. Flyers
// patrol too (straight through the air).
TEST(Ai, PatrolLookTurnAttackAndFlyers) {
    CollisionMesh m;
    addFloor(m, -6000, -6000, 6000, 6000, 0);
    const auto col = built(m);
    auto t = std::make_shared<game::ai::EnemyType>(*syntheticType());
    t->clips["look"] = rootClip(30, 0);
    t->clips["pounce"] = rootClip(30, 0);
    t->patrol.idles = {{"look", 1, 1.0f, 1.0f}};
    t->patrol.arrive = 100;
    game::ai::Attack pounce;
    pounce.clip = "pounce", pounce.damage = 40;
    t->tuning.attacks = {pounce};
    t->patrol.attack = {0, -1};
    game::ai::Enemies es;
    auto& e = es.add(t, 1, {0, 0, 0}, 0);
    e.patrolPaths = {{{{1000, 0, 0}, {0, 1, 0}, true, 0}, {{0, 0, 0}, {0, -1, 0}, false, 2}}};
    e.setPatrol(0);
    const game::ai::Target away{{0, 0, -50000}};
    float facing = 0;
    for (int i = 0; i < int(12 / kDt) && e.state().patrolIndex == 0; ++i) {
        es.tick(col, away, {}, kDt);
        if (e.state().phase == 1) facing = e.state().yaw;
    }
    EXPECT_NEAR(facing, 3.14159265f / 2, 0.06f); // turned to +Y, the node's facing
    for (int i = 0; i < int(20 / kDt) && e.state().phase != 1; ++i) es.tick(col, away, {}, kDt); // at the mode-2 point
    Hurt hurt;
    std::vector<Mode> modes;
    for (int i = 0; i < int(2 / kDt); ++i, ++hurt.tick) {
        es.tick(col, {e.position() + vec3(std::cos(e.state().yaw), std::sin(e.state().yaw), 0) * 400.0f}, hurt.fn(), kDt);
        if (modes.empty() || modes.back() != e.state().mode) modes.push_back(e.state().mode);
    }
    ASSERT_GE(modes.size(), 2u);
    EXPECT_EQ(modes[0], Mode::Attack); // straight from the patrol
    ASSERT_FALSE(hurt.blows.empty());
    EXPECT_EQ(hurt.blows[0].second, 40.0f);

    auto& bat = es.add(syntheticBat(), 2, {0, 3000, 500}, 0);
    bat.patrolPaths = {{{{0, 5000, 1500}}, {{0, 3000, 500}}}};
    bat.setPatrol(0);
    float nearest = 1e9f;
    for (int i = 0; i < int(10 / kDt); ++i) es.tick(col, away, {}, kDt), nearest = std::min(nearest, glm::distance(bat.position(), vec3(0, 5000, 1500)));
    EXPECT_LT(nearest, 2 * bat.radius() + 1);
}

// ---- real data (OU_GAME_DIR) ------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
} // namespace

// Every enemy intro (ObjectBase oflags2 0x400000) and generator spawn of Mexico (l4_*) and of Thailand's first enemy
// units resolves to an enemy object with a model-0 skeleton and enemy data (magic, hit points); the spawnable ones
// (cats, thralls, spiders, poachers, bats) also to their role clips and are spawned.
TEST(AiReal, EnemyIntrosResolveToSkinnedEnemyTypes) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    ASSERT_TRUE(list);
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    const char* units[] = {"l4_ballcourt", "l4_pool", "l4_ragnarok_room", "l4_staircase_mictlan", "l4_staircase_upper",
                           "l4_staircase_xibalba", "l4_temple", "l4_templepath", "l4_trailstart", "l4_trailtube",
                           "l4_traprooms", "l2_junglehub", "l2_towerroom", "l2_hubhall", "l2_cliffclimb"};
    game::ai::Enemies es;
    es.setBigfile(&bf);
    std::vector<script::UnitRef> refs;
    std::map<std::string, int> intros; // enemy object -> intros
    for (const char* u : units) {
        auto bytes = bf.read(std::string("pc-w\\") + u + ".drm");
        ASSERT_TRUE(bytes) << u;
        auto drm = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
        for (const auto& in : formats::parseUnit(*drm).intros)
            if (const auto n = names.find(in.objectId); n != names.end() && game::ai::isEnemyObject(bf, n->second)) ++intros[n->second];
        refs.push_back({u, std::move(drm), vec3(0)});
    }
    es.setUnits(refs);
    std::set<std::string> objects;
    for (const auto& [n, c] : intros) objects.insert(n);
    for (const auto& g : es.generators()) objects.insert(g.object);
    EXPECT_TRUE(objects.contains("mayan_thrall")); // l4_pool's 13 mayan_thrall_pool_generator
    std::map<std::string, int> spawned;
    for (const auto& e : es.all()) ++spawned[e->type().name];
    for (const auto& n : objects) {
        std::shared_ptr<const game::ai::EnemyType> t;
        ASSERT_NO_THROW(t = game::ai::loadEnemyType(bf, n)) << n;
        EXPECT_GT(t->skeleton.bones.size(), 1u) << n;
        EXPECT_GT(t->tuning.health, 0) << n;
        EXPECT_GT(t->tuning.height, t->tuning.radius) << n;
        std::fprintf(stderr, "enemy %-24s id %3u class %d bones %3zu hp %6.1f radius %5.1f height %6.1f intros %2d spawned %2d |",
                     n.c_str(), t->objectId, t->enemyClass, t->skeleton.bones.size(), t->tuning.health, t->tuning.radius,
                     t->tuning.height, intros[n], spawned[n]);
        for (size_t r = 0; r < size_t(Role::Count); ++r) std::fprintf(stderr, " %s", t->roles[r].empty() ? "-" : t->roles[r].c_str());
        std::fprintf(stderr, "\n");
        if (t->spawnable()) { EXPECT_EQ(spawned[n], intros[n]) << n; } // every spawnable enemy intro is alive in the world
    }
    for (const auto& w : es.warnings) {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
        EXPECT_NE(w.find("not spawned"), std::string::npos) << w; // only the documented skips
    }
    for (const char* m : {"cat_panther", "panther_thrall", "tiger", "mayan_thrall", "small_albino_spider", "giant_albino_tarantula",
                          "poacher", "poacher_long_range", "bat_enemy"})
        EXPECT_TRUE(game::ai::loadEnemyType(bf, m)->spawnable()) << m;
    EXPECT_EQ(spawned["cat_panther"], 13); // ballcourt 4, temple 5, templepath 1, trailstart 3
    EXPECT_EQ(spawned["panther_thrall"], 1);
    EXPECT_EQ(spawned["poacher"], 12);     // ballcourt, temple
    EXPECT_EQ(spawned["poacher_long_range"], 4);
    EXPECT_EQ(spawned["bat_enemy"], 20);   // l4_temple 5250..5254 + Thailand
    // Panther numbers [O]: enemy data +0x2C radius 0.3 m, Normal hit points 700.
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    EXPECT_NEAR(panther->tuning.radius, 94.8167f, 1e-3);
    EXPECT_EQ(panther->tuning.health, 700);
    EXPECT_EQ(panther->enemyClass, 3);
    // Its run clip moves the root forward (model -Y).
    const auto* run = panther->clip(Role::Run);
    ASSERT_TRUE(run);
    const double d = run->durationSeconds();
    const vec3 cyc = game::rootTranslation(*run, d, true) - game::rootTranslation(*run, 0, true);
    std::fprintf(stderr, "panther run_forward_2: %.1f units over %.3f s -> %.0f units/s (x %.1f z %.1f)\n", -cyc.y, d, -cyc.y / d, cyc.x, cyc.z);
    EXPECT_LT(cyc.y, -100);
}

// Decoded enemy-data records [O] (values: openunderworld-work scratch dumps, agent report): senses (node +0x30 /
// +0x38), attack records (+0x68..+0x9C) with resolved clips, the melee damage per blow, death records (+0x64).
TEST(AiReal, DecodedSensesAttacksDeaths) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto cone = [](const game::ai::Cone& c, float range, float yaw, float pitch) {
        EXPECT_EQ(c.range, range);
        EXPECT_NEAR(c.yaw, yaw * kDeg, 1e-5f);
        EXPECT_NEAR(c.pitch, pitch * kDeg, 1e-5f);
    };
    const auto poacher = game::ai::loadEnemyType(bf, "poacher");
    const auto& p = poacher->tuning;
    cone(p.senses.eye, 5720, 45, 30), cone(p.senses.body[0], 5689, 10, 25), cone(p.senses.body[1], 3792, 180, 30);
    cone(p.combatSenses.body[1], 3792, 180, 45);
    EXPECT_TRUE(p.senses.ray);
    EXPECT_NEAR(p.senses.loseTrack, 2528.45f, 0.01f);
    EXPECT_EQ(poacher->behaviour, 0);
    ASSERT_GE(p.attacks.size(), 6u);
    const auto& fire = p.attacks[0];
    EXPECT_EQ(fire.node, 0x68u);
    EXPECT_EQ(fire.damage, 75);
    EXPECT_EQ(fire.shots, 3);
    EXPECT_EQ(fire.gap, 5);
    EXPECT_EQ(fire.startDelay, 30);
    EXPECT_FLOAT_EQ(fire.cooldown, 7);
    EXPECT_NEAR(fire.maxYaw, 10 * kDeg, 1e-5f);
    EXPECT_EQ(fire.shotClip, "combat_stand_burstfire_2");
    EXPECT_EQ(fire.followClip, "reload2");
    EXPECT_EQ(p.attacks[2].shotClip, "combat_crouch_burstfire"); // +0x70
    EXPECT_EQ(p.attacks[4].clip, "rifle_bash");                  // +0x84
    EXPECT_TRUE(poacher->ranged());
    EXPECT_EQ(poacher->weapon, 114); // wp_akrifle
    EXPECT_EQ(p.attackDamage, 100); // rifle_bash record, light
    EXPECT_EQ(p.attackType, game::kDamageLight);
    ASSERT_EQ(p.deaths.size(), 6u);
    EXPECT_EQ(p.deaths[0].clip, "death_shot_3");
    EXPECT_EQ(p.deaths[0].category, 3);
    EXPECT_EQ(p.deaths[3].clip, "death_shot_back");
    EXPECT_EQ(p.deaths[3].category, 5);
    EXPECT_EQ(p.deaths[2].clip, ""); // type-2 ref (front): unresolved

    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    const auto& c = panther->tuning;
    cone(c.senses.eye, 11061, 120, 60), cone(c.senses.body[0], 9481, 60, 30), cone(c.senses.body[1], 2370, 180, 90);
    EXPECT_FALSE(c.senses.ray); // byte +0x44 set
    EXPECT_FALSE(panther->ranged()); // +0x9C names a +0x7C clip, but cats carry no weapon
    EXPECT_EQ(c.attackDamage, 50); // simple_attack_static1 (+0x88), light
    EXPECT_EQ(c.attackType, game::kDamageLight);
    EXPECT_EQ(c.attacks.size(), 8u);
    const auto death = [&](int cat, int flag) {
        for (const auto& d : c.deaths)
            if (d.category == cat && d.flag == flag) return d.clip;
        return std::string("?");
    };
    EXPECT_EQ(death(1, 5), "death_running");
    EXPECT_EQ(death(1, 3), "death_adren");
    EXPECT_EQ(death(5, 0), "hitreact_thrown_forward");

    const auto bat = game::ai::loadEnemyType(bf, "bat_enemy");
    const auto& b = bat->tuning;
    cone(b.senses.eye, 6321, 180, 90), cone(b.senses.body[0], 3792, 180, 90);
    EXPECT_EQ(b.senses.body[1].range, 0);
    cone(b.combatSenses.eye, 4740, 180, 90);
    EXPECT_TRUE(bat->flyer());
    EXPECT_EQ(b.health, 2);
    EXPECT_EQ(b.attackDamage, 75); // +0x80 attach_bite_loop
    EXPECT_EQ(bat->clipName(Role::Attack), "attack_bite");
    ASSERT_EQ(b.deaths.size(), 1u);
    EXPECT_EQ(b.deaths[0].clip, "death");

    // Sharks use the same behaviour-tree family as bats, but their clips and domain are aquatic.
    const auto shark = game::ai::loadEnemyType(bf, "shark_blueshark");
    EXPECT_EQ(shark->enemyClass, 1);
    EXPECT_TRUE(shark->flyer());
    EXPECT_EQ(shark->tuning.health, 500);
    cone(shark->tuning.senses.eye, 11378, 180, 180), cone(shark->tuning.senses.body[0], 6321, 180, 90);
    EXPECT_TRUE(shark->tuning.attacks.empty());
    EXPECT_TRUE(shark->spawnable());
    EXPECT_EQ(shark->clipName(Role::Run), "swim_forward_fast");
    EXPECT_EQ(shark->clipName(Role::Attack), "bite_2");
    EXPECT_EQ(game::ai::loadEnemyType(bf, "shark_greenland")->tuning.health, 800);

    for (const char* n : {"death_static", "death_running", "death_adren"}) EXPECT_TRUE(panther->lyingDeaths.count(n)) << n;
    for (const char* n : {"death_shot", "death_shot_3", "death_shot_back"}) EXPECT_TRUE(poacher->lyingDeaths.count(n)) << n;
    EXPECT_EQ(game::ai::loadEnemyType(bf, "mayan_thrall")->tuning.attackDamage, 120); // no records: [I] default

    // Death poses (the root bone is a fixed dummy): the last frame's bone heights; lying = median under a third of the
    // bind pose's median (helper bones such as the dropped rifle fly far off).
    for (const auto& t : {panther, poacher, bat})
        for (const auto& [n, clip] : t->clips) {
            if (n.find("death") == std::string::npos && n.find("thrown") == std::string::npos) continue;
            float top = -1e9f, low = 1e9f;
            std::vector<float> zs, bind;
            for (const auto& m : anim::worldMatrices(t->skeleton, anim::sample(clip, t->skeleton, clip.durationSeconds(), false)))
                top = std::max(top, m[3].z), low = std::min(low, m[3].z), zs.push_back(m[3].z);
            for (const auto& p : anim::bindPositions(t->skeleton)) bind.push_back(p.z);
            std::sort(zs.begin(), zs.end());
            std::nth_element(bind.begin(), bind.begin() + long(bind.size() / 2), bind.end());
            std::fprintf(stderr, "%s %s: %.2f s, bones z %.0f..%.0f median %.0f p90 %.0f (bind median %.0f, height %.0f) -> %s\n",
                         t->name.c_str(), n.c_str(), clip.durationSeconds(), low, top, zs[zs.size() / 2], zs[zs.size() * 9 / 10],
                         bind[bind.size() / 2], t->tuning.height, t->lyingDeaths.count(n) ? "lying" : "upright (tips over)");
        }
}

// The placed sharks are live enemies, not discarded intros. A real blueshark leaves its placement, swims to Lara,
// plays its data's bite clip and hurts her, then responds normally to a fatal hit.
TEST(AiReal, SharksSpawnSwimBiteAndDie) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto objectBytes = bf.read("\\tr8\\pc-w\\objectlist.txt");
    ASSERT_TRUE(objectBytes);
    const auto objectNames = formats::parseObjectList({reinterpret_cast<const char*>(objectBytes->data()), objectBytes->size()});
    struct Population { const char* unit; int first, last, count; };
    for (const Population p : {Population{"l1_seafloor", 980, 985, 6}, Population{"l2_cliffclimb", 4020, 4023, 4},
                               Population{"l7_arctic_sea", 9125, 9133, 8}}) { // 9129 is a triggervolume, not a ninth shark
        auto unit = game::loadUnitData(bf, p.unit);
        unit.col.build();
        for (const auto& in : formats::parseUnit(*unit.drm).intros)
            if (in.uniqueId >= p.first && in.uniqueId <= p.last)
                std::fprintf(stderr, "%s intro %d: %s\n", p.unit, in.uniqueId,
                             objectNames.count(in.objectId) ? objectNames.at(in.objectId).c_str() : "?");
        game::ai::Enemies es;
        es.setBigfile(&bf);
        es.setUnits({{unit.name, unit.drm, vec3(0)}});
        std::set<int32_t> sharks;
        for (const auto& e : es.all())
            if (e->uid() >= p.first && e->uid() <= p.last) {
                sharks.insert(e->uid());
                EXPECT_TRUE(e->type().name.starts_with("shark_")) << p.unit << " " << e->uid();
            }
        EXPECT_EQ(sharks.size(), size_t(p.count)) << p.unit;
        std::fprintf(stderr, "%s: %zu sharks, %zu water volumes\n", p.unit, sharks.size(),
                     size_t(std::count_if(unit.col.markup.begin(), unit.col.markup.end(), [](const auto& m) { return m.type == 10; })));
        for (const auto& m : unit.col.markup)
            if (m.type == 10 && m.points.size() >= 2)
                std::fprintf(stderr, "  water %u (%.0f %.0f %.0f)..(%.0f %.0f %.0f)\n", m.id,
                             m.points[0].x, m.points[0].y, m.points[0].z, m.points[1].x, m.points[1].y, m.points[1].z);
    }

    auto unit = game::loadUnitData(bf, "l2_cliffclimb");
    unit.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{unit.name, unit.drm, vec3(0)}});
    const auto it = std::find_if(es.all().begin(), es.all().end(), [](const auto& e) { return e->uid() == 4020; });
    ASSERT_NE(it, es.all().end());
    auto& shark = **it;
    const vec3 start = shark.state().pos;
    const game::ai::Target lara{start + vec3(1200, 0, 0), true, 512, 100, true}; // swimming
    std::vector<float> damage;
    bool attacked = false;
    for (int i = 0; i < 900 && damage.empty(); ++i) {
        es.tick(unit.col, lara, [&](float d, vec3, int) { damage.push_back(d); }, kDt);
        attacked |= shark.state().mode == Mode::Attack && shark.state().clip == shark.type().clipName(Role::Attack);
    }
    EXPECT_TRUE(attacked);
    ASSERT_EQ(damage.size(), 1u);
    EXPECT_GT(damage[0], 0);
    EXPECT_LT(glm::distance(shark.state().pos, lara.pos), glm::distance(start, lara.pos));
    shark.onHit(shotFrom({-1, 0, 0}, shark.health() + 1));
    EXPECT_EQ(shark.state().mode, Mode::Dead);
    for (int i = 0; i < 600; ++i)
        es.tick(unit.col, lara, [](float, vec3, int) { FAIL() << "dead sharks do not bite"; }, kDt);
    EXPECT_EQ(shark.state().mode, Mode::Dead);
    EXPECT_EQ(shark.state().clip, "death_float_loop");
}

// l4_pool's mayan_thrall_pool_generator 5637 spawns a mayan_thrall that plays spawn_from_pool, then hunts Lara.
TEST(AiReal, PoolGeneratorSpawnsThrall) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l4_pool");
    unit.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{unit.name, unit.drm, vec3(0)}});
    const auto g = std::find_if(es.generators().begin(), es.generators().end(), [](const auto& x) { return x.uid == 5637; });
    ASSERT_NE(g, es.generators().end());
    EXPECT_EQ(g->object, "mayan_thrall");
    const size_t placed = es.all().size(); // panther_thrall 5645
    const vec3 lara = g->pos + vec3(g->range + 200, 0, 0);
    es.tick(unit.col, {lara}, {}, kDt);
    EXPECT_EQ(es.all().size(), placed); // Lara out of range
    es.tick(unit.col, {g->pos + vec3(1500, 0, 0)}, {}, kDt);
    ASSERT_EQ(es.all().size(), placed + 1);
    const auto& e = **std::ranges::find_if(es.all(), [](const auto& x) { return x->generator == 5637; }); // kept in uid order
    EXPECT_EQ(e.type().name, "mayan_thrall");
    EXPECT_EQ(e.generator, 5637);
    EXPECT_EQ(e.state().mode, Mode::Spawning);
    EXPECT_EQ(e.state().clip, "spawn_from_pool");
    for (int i = 0; i < 6.7 / kDt + 2; ++i) es.tick(unit.col, {g->pos + vec3(1500, 0, 0)}, {}, kDt);
    EXPECT_NE(e.state().mode, Mode::Spawning); // 6.7 s clip done
    es.tick(unit.col, {g->pos + vec3(1500, 0, 0)}, {}, kDt);
    EXPECT_EQ(es.all().size(), placed + 1); // generators fire once
}

// Save games: l4_pool with the thrall spawned by generator 5637 mid-chase and panther 5645 killed after the checkpoint.
// serialize() through JSON text into fresh Enemies, with the unit at another stream offset: every enemy is back in
// the same state (shifted by the offset), the generator stays fired, both runs tick on identically, and a respawn
// reset still revives the panther (alive at the checkpoint) and removes the generator spawn.
TEST(AiReal, EnemyStateSaveRestore) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l4_pool");
    unit.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{unit.name, unit.drm, vec3(0)}});
    const auto g = std::find_if(es.generators().begin(), es.generators().end(), [](const auto& x) { return x.uid == 5637; });
    ASSERT_NE(g, es.generators().end());
    const vec3 lara = g->pos + vec3(1500, 0, 0);
    es.checkpoint();
    for (int i = 0; i < 8.0 / kDt; ++i) es.tick(unit.col, {lara}, {}, kDt);
    game::ai::Enemy* panther = nullptr;
    for (const auto& e : es.all())
        if (e->uid() == 5645) panther = e.get();
    ASSERT_TRUE(panther);
    while (panther->alive()) panther->onHit(shotFrom({-1, 0, 0}, panther->health() + 1));
    for (int i = 0; i < 30; ++i) es.tick(unit.col, {lara}, {}, kDt);
    ASSERT_EQ(es.all().size(), 2u);

    const vec3 offset(1000, -2000, 300);
    game::ai::Enemies loaded;
    loaded.setBigfile(&bf);
    loaded.setUnits({{unit.name, unit.drm, offset}}); // a different stream: placements, then replaced below
    loaded.restore(nlohmann::json::parse(es.serialize().dump()));
    EXPECT_TRUE(loaded.all().empty());
    loaded.setUnits({{unit.name, unit.drm, offset}});
    ASSERT_EQ(loaded.all().size(), es.all().size());
    const auto byUid = [](const game::ai::Enemies& s, int32_t uid) -> const game::ai::Enemy& {
        for (const auto& e : s.all())
            if (e->uid() == uid) return *e;
        throw std::runtime_error("no enemy " + std::to_string(uid));
    };
    const auto same = [&](const game::ai::Enemies& other, int32_t uid, vec3 shift) {
        const auto &a = byUid(es, uid).state(), &b = byUid(other, uid).state();
        EXPECT_EQ(a.mode, b.mode) << uid;
        EXPECT_NEAR(glm::distance(a.pos + shift, b.pos), 0, 0.01) << uid;
        EXPECT_EQ(a.health, b.health) << uid;
        EXPECT_EQ(a.clip, b.clip) << uid;
        EXPECT_NEAR(a.clipTime, b.clipTime, 1e-9) << uid;
        EXPECT_EQ(a.rng, b.rng) << uid;
    };
    same(loaded, 5645, offset);
    same(loaded, 5637, offset);
    EXPECT_EQ(byUid(loaded, 5637).generator, 5637);
    EXPECT_EQ(byUid(loaded, 5645).state().mode, Mode::Dead);
    for (const auto& gen : loaded.generators())
        if (gen.uid == 5637) EXPECT_TRUE(gen.fired);
    // Restored in place, the chase goes on exactly as the original.
    game::ai::Enemies again;
    again.setBigfile(&bf);
    again.restore(es.serialize());
    again.setUnits({{unit.name, unit.drm, vec3(0)}});
    for (int i = 0; i < 120; ++i) {
        es.tick(unit.col, {lara}, {}, kDt);
        again.tick(unit.col, {lara}, {}, kDt);
    }
    same(again, 5637, vec3(0));
    same(again, 5645, vec3(0));
    loaded.reset(); // respawn at the checkpoint, where the panther was alive
    EXPECT_EQ(loaded.all().size(), 1u);
    EXPECT_EQ(byUid(loaded, 5645).state().mode, Mode::Idle);
    EXPECT_THROW(loaded.restore(nlohmann::json{{"states", {{"1", {{"mode", 99}}}}}}), std::runtime_error);
}

// Real units: Lara standing 1500 in front of l4_ballcourt poacher 6541 (spot found with a line-of-sight probe) is shot
// at in bursts of 3 with the decoded damage; on l4_temple's platform above bats 5250..5254 (z 1725 over their 1043) a
// bat flies up and bites. No placed enemy falls through its floor (6541 stands 96 below it).
TEST(AiReal, PoachersFireAndBatsBite) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    struct Run { const char* unit; vec3 lara; int ticks; };
    for (const Run r : {Run{"l4_ballcourt", {17520, 10383, 2579}, 900}, Run{"l4_temple", {-5096, -10909, 1725}, 900}}) {
        auto unit = game::loadUnitData(bf, r.unit);
        unit.col.build();
        game::ai::Enemies es;
        es.setBigfile(&bf);
        es.setUnits({{unit.name, unit.drm, vec3(0)}});
        std::map<int32_t, float> startZ;
        for (const auto& e : es.all()) startZ[e->uid()] = e->state().pos.z;
        std::vector<float> hurt;
        std::map<int32_t, int> shots, bites;
        for (int i = 0; i < r.ticks; ++i) {
            es.tick(unit.col, {r.lara}, [&](float d, vec3, int) { hurt.push_back(d); }, kDt);
            for (const auto& s : es.shots) {
                ++shots[s.uid];
                // from the wp_akrifle model riding the poacher's marker -75 (segment 48): its shot origin, marker -30
                const auto e = std::find_if(es.all().begin(), es.all().end(), [&](const auto& x) { return x->uid() == s.uid; });
                ASSERT_NE(e, es.all().end());
                const auto& t = (*e)->type();
                EXPECT_EQ(t.gunMarker, -75);
                EXPECT_EQ(t.gunSegment, 48);
                EXPECT_NEAR(glm::distance(s.from, vec3(s.gun * t.muzzle[3])), 0, 1e-2f);
                EXPECT_NEAR(t.muzzle[3].y, -152.0f, 0.1f);
                // its barrel (-Y) along the shot: the burst clips hold it within ~20 degrees of Lara
                EXPECT_GT(glm::dot(glm::normalize(vec3(-s.gun[1])), glm::normalize(s.to - s.from)), std::cos(glm::radians(30.0f)));
            }
            for (const auto& e : es.all())
                if (e->state().blowLanded && !e->previous().blowLanded) ++bites[e->uid()];
        }
        for (const auto& e : es.all()) // (wall-hung spiders drop: they have no climbing)
            if (e->type().name.starts_with("poacher") || e->type().flyer()) {
                EXPECT_GT(e->state().pos.z, startZ[e->uid()] - 300) << r.unit << " " << e->uid();
            }
        for (const auto& [uid, n] : shots) std::fprintf(stderr, "%s: enemy %d fired %d shots\n", r.unit, uid, n);
        for (const auto& [uid, n] : bites) std::fprintf(stderr, "%s: enemy %d landed %d blows\n", r.unit, uid, n);
        if (std::string(r.unit) == "l4_ballcourt") {
            EXPECT_GE(shots[6541], 3); // at least one burst
            EXPECT_EQ(shots[6541] % 3, 0);
        } else {
            int batBites = 0;
            for (const auto& [uid, n] : bites) batBites += uid >= 5250 && uid <= 5254 ? n : 0;
            EXPECT_GE(batBites, 1);
        }
        for (const float d : hurt) EXPECT_TRUE(d == 75 || d == 50 || d == 100) << d; // shot / bite, panther, bash
    }
}

namespace {
game::Hit gunHit(vec3 dir, float damage) { // Lara's bullets: pr_*bullet class 0 / type 1
    game::Hit h{damage, {}, dir, game::Team::Player};
    h.damageType = 1;
    return h;
}
game::Hit boltHit(vec3 dir) { // Mjolnir: pr_thorbolt 1000, class 13 / type 3
    game::Hit h{1000, {}, dir, game::Team::Player};
    h.damageClass = 13, h.damageType = 3;
    return h;
}
// A spot for Lara in view of a clinging crawler: off its wall along its up axis, dropped to the floor below.
std::optional<vec3> floorInFront(const CollisionMesh& col, const game::ai::Enemy& e, float out) {
    const vec3 up = vec3(game::ai::Enemy::modelMatrix({}, e.state().yaw, 0, e.state().tilt) * glm::vec4(0, 0, 1, 0));
    vec3 p = e.state().pos + up * out;
    const auto g = col.groundProbe(p + vec3(0, 0, 300), 8000);
    if (!g) return std::nullopt;
    return g->point;
}
// Ticks until `done` (true) or `ticks` run out; returns the tick or -1.
template <class F>
int tickUntil(game::ai::Enemies& es, const CollisionMesh& col, const game::ai::Target& lara, const game::ai::HurtFn& hurt, int ticks, F done) {
    for (int i = 0; i < ticks; ++i) {
        es.tick(col, lara, hurt, kDt);
        if (done()) return i;
    }
    return -1;
}
} // namespace

// Thailand's nagas (naga_green class 6, naga_red class 5) spawn in l2_norsehub / l2_puzzleroom; tilted intros cling to
// the walls and ceilings. A clinging naga notices Lara below, walks down its wall, stands up at its foot, closes in
// and bites (simple_attack1, red 175 / green 125 per record); a green naga spits (spit_attack, 150) from out of reach;
// tranquilizer darts send it to sleep (tranq_sleep_start / _loop); bullets kill it (dead_shot_front).
TEST(AiReal, NagasClingDropBiteSpitAndDie) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    for (const auto& [unit, green, red, cling] : {std::tuple{"l2_norsehub", 5, 6, 7}, std::tuple{"l2_puzzleroom", 12, 4, 4}}) {
        auto u = game::loadUnitData(bf, unit);
        game::ai::Enemies es;
        es.setBigfile(&bf);
        es.setUnits({{u.name, u.drm, vec3(0)}});
        int g = 0, r = 0, c = 0;
        for (const auto& e : es.all()) {
            g += e->type().name == "naga_green", r += e->type().name == "naga_red";
            c += e->type().name.starts_with("naga_") && e->state().mode == Mode::Cling;
        }
        EXPECT_EQ(g, green) << unit;
        EXPECT_EQ(r, red) << unit;
        EXPECT_EQ(c, cling) << unit;
    }
    const auto red = game::ai::loadEnemyType(bf, "naga_red"), green = game::ai::loadEnemyType(bf, "naga_green");
    for (const auto& t : {red, green}) {
        EXPECT_TRUE(t->spawnable()) << t->name;
        EXPECT_TRUE(t->crawler());
        EXPECT_EQ(t->clipName(Role::Attack), "simple_attack1");
        EXPECT_EQ(t->clipName(Role::Death), "dead_shot_front");
        EXPECT_EQ(t->fallClip, "falling_loop");
        EXPECT_EQ(t->landClip, "fall_land_front");
        EXPECT_TRUE(t->sleeps);
        EXPECT_EQ(t->sleepClips[0], "tranq_sleep_start");
    }
    EXPECT_EQ(red->tuning.attackDamage, 175);
    EXPECT_EQ(green->tuning.attackDamage, 125);
    ASSERT_GE(green->spit, 0);
    EXPECT_EQ(green->tuning.attacks[size_t(green->spit)].clip, "spit_attack");
    EXPECT_EQ(red->spit, -1);

    // Real geometry: red naga 2417 on an l2_norsehub wall.
    auto u = game::loadUnitData(bf, "l2_norsehub");
    u.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{u.name, u.drm, vec3(0)}});
    const auto it = std::find_if(es.all().begin(), es.all().end(), [](const auto& e) { return e->uid() == 2417; });
    ASSERT_NE(it, es.all().end());
    auto& naga = **it;
    ASSERT_EQ(naga.state().mode, Mode::Cling);
    const vec3 wall = naga.state().pos;
    const auto spot = floorInFront(u.col, naga, 900);
    ASSERT_TRUE(spot);
    const game::ai::Target lara{*spot};
    std::vector<float> bites; // this naga's (the unit's spider generator and bats hunt Lara too)
    const auto hurt = [&](float d, vec3 from, int) { if (glm::distance(from, naga.state().pos) < 1) bites.push_back(d); };
    ASSERT_GE(tickUntil(es, u.col, {*spot + vec3(0, 0, 20000)}, hurt, 120, [&] { return naga.state().mode != Mode::Cling; }), -1);
    EXPECT_EQ(naga.state().mode, Mode::Cling); // Lara out of its sight: holds on
    EXPECT_EQ(naga.state().pos, wall);
    const int dropped = tickUntil(es, u.col, lara, hurt, 60, [&] { return naga.state().mode == Mode::Crawl; });
    ASSERT_GE(dropped, 0);
    EXPECT_NE(naga.state().tilt, glm::vec2(0)); // walks down its wall
    const int landed = tickUntil(es, u.col, lara, hurt, 300, [&] { return naga.state().tilt == glm::vec2(0); });
    ASSERT_GE(landed, 0);
    EXPECT_EQ(naga.state().mode, Mode::Chase); // stood up at the foot of the wall (no fall)
    EXPECT_LT(naga.state().pos.z, wall.z - 100); // on the floor below its wall
    ASSERT_GE(tickUntil(es, u.col, lara, hurt, 900, [&] { return !bites.empty(); }), 0);
    EXPECT_EQ(bites[0], 175);
    std::fprintf(stderr, "naga 2417: wall (%.0f %.0f %.0f), Lara (%.0f %.0f %.0f), crawls at tick %d, on the floor %d, bites at (%.0f %.0f %.0f)\n",
                 wall.x, wall.y, wall.z, spot->x, spot->y, spot->z, dropped, landed, naga.state().pos.x, naga.state().pos.y, naga.state().pos.z);
    for (int i = 0; i < 50 && naga.alive(); ++i) naga.onHit(gunHit({1, 0, 0}, 40));
    EXPECT_EQ(naga.state().mode, Mode::Dead);

    // Green naga spit, on a flat floor: Lara 1500 away (out of reach, in its sight).
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    game::ai::Enemies flat;
    auto& spitter = flat.add(green, 1, {0, 0, 0}, 0);
    std::vector<std::pair<float, std::string>> hits;
    const int spat = tickUntil(flat, col, {{1500, 0, 0}}, [&](float d, vec3, int) { hits.push_back({d, spitter.state().clip}); }, 600,
                               [&] { return !hits.empty(); });
    ASSERT_GE(spat, 0);
    EXPECT_EQ(hits[0].first, 150);
    EXPECT_EQ(hits[0].second, "spit_attack");
    EXPECT_LT(spitter.state().pos.x, 1500 - green->tuning.attackRange); // spat from out of reach
    // Darts: the dose reaches its health -> asleep.
    game::Hit dart{500, {}, {-1, 0, 0}, game::Team::Player};
    dart.tranq = true;
    spitter.onHit(dart);
    EXPECT_EQ(spitter.state().mode, Mode::Sleep);
    EXPECT_EQ(spitter.state().clip, "tranq_sleep_start");
}

// Spiders (class 7) on l2_hubhall's wall and l2_maproom's walls cling; a spider that sees Lara walks down and bites; a spider shot
// on the wall falls off it dead; clinging survives a save round trip.
TEST(AiReal, SpidersClingDropAndBite) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto u = game::loadUnitData(bf, "l2_hubhall");
    u.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{u.name, u.drm, vec3(0)}});
    std::vector<game::ai::Enemy*> spiders;
    for (const auto& e : es.all())
        if (e->uid() >= 3155 && e->uid() <= 3160) spiders.push_back(e.get());
    ASSERT_EQ(spiders.size(), 6u);
    for (const auto* s : spiders) EXPECT_EQ(s->state().mode, Mode::Cling) << s->uid();
    // Save round trip keeps them on the wall.
    game::ai::Enemies loaded;
    loaded.setBigfile(&bf);
    loaded.restore(nlohmann::json::parse(es.serialize().dump()));
    loaded.setUnits({{u.name, u.drm, vec3(0)}});
    for (const auto& e : loaded.all())
        if (e->uid() == 3155) {
            EXPECT_EQ(e->state().tilt, spiders[0]->state().tilt);
            EXPECT_EQ(e->state().mode, Mode::Cling);
        }

    auto& spider = *spiders[0];
    const vec3 wall = spider.state().pos;
    const auto spot = floorInFront(u.col, spider, 700);
    ASSERT_TRUE(spot);
    std::vector<float> bites;
    const game::ai::Target lara{*spot};
    ASSERT_GE(tickUntil(es, u.col, lara, [&](float d, vec3, int) { bites.push_back(d); }, 60, [&] { return spider.state().mode == Mode::Crawl; }), 0);
    ASSERT_GE(tickUntil(es, u.col, lara, [&](float d, vec3, int) { bites.push_back(d); }, 300, [&] {
        return spider.state().tilt == glm::vec2(0) && (spider.state().mode == Mode::Chase || spider.state().clip == "jump_end");
    }), 0); // walked down its pillar, or let go where the surface ended and landed
    EXPECT_LT(spider.state().pos.z, wall.z - 100);
    ASSERT_GE(tickUntil(es, u.col, lara, [&](float d, vec3, int) { bites.push_back(d); }, 900, [&] { return !bites.empty(); }), 0);
    EXPECT_EQ(bites[0], spider.type().tuning.attackDamage);
    std::fprintf(stderr, "spider 3155: wall (%.0f %.0f %.0f) -> floor %.0f, bite %.0f\n", wall.x, wall.y, wall.z, spider.state().pos.z, bites[0]);
    // Shot on the wall: dies and falls off it.
    game::ai::Enemies fresh;
    fresh.setBigfile(&bf);
    fresh.setUnits({{u.name, u.drm, vec3(0)}});
    auto& other = **std::find_if(fresh.all().begin(), fresh.all().end(), [](const auto& e) { return e->uid() == 3156; });
    ASSERT_EQ(other.state().mode, Mode::Cling);
    const float z0 = other.state().pos.z;
    other.onHit(gunHit({1, 0, 0}, 40));
    EXPECT_EQ(other.state().mode, Mode::Dead);
    EXPECT_EQ(other.state().tilt, glm::vec2(0));
    for (int i = 0; i < 240; ++i) fresh.tick(u.col, {{0, 0, -100000}, false}, {}, kDt);
    EXPECT_LT(other.state().pos.z, z0 - 100);
}

// Thralls' regenerate record (death record +0x20..+0x70): bullets, blows and explosions only knock viking / mayan
// thralls down for 15 s (450 counts) and yetis for 7 s; they get back up at full health (death_reanimate /
// regenerate_01). Hits while down are absorbed, except Mjolnir's, which finishes them; a bolt on a standing thrall
// kills it outright. Panther thralls (l3_secretchamber, l4_pool) fall with their role death clip and get up after
// 13.3 s (death_thrall_regen).
TEST(AiReal, ThrallsGetUpUnlessMjolnir) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    {
        auto u = game::loadUnitData(bf, "l5_valaskjalf_entry");
        game::ai::Enemies es;
        es.setBigfile(&bf);
        es.setUnits({{u.name, u.drm, vec3(0)}});
        EXPECT_EQ(std::count_if(es.all().begin(), es.all().end(), [](const auto& e) { return e->type().name == "viking_thrall"; }), 7);
    }
    const auto viking = game::ai::loadEnemyType(bf, "viking_thrall"), yeti = game::ai::loadEnemyType(bf, "yeti_thrall"),
               mayan = game::ai::loadEnemyType(bf, "mayan_thrall"), panther = game::ai::loadEnemyType(bf, "panther_thrall");
    EXPECT_EQ(viking->tuning.regenerate.clip, "death");
    EXPECT_EQ(viking->tuning.regenerate.up, "death_reanimate");
    EXPECT_FLOAT_EQ(viking->tuning.regenerate.time, 15);
    EXPECT_EQ(mayan->tuning.regenerate.up, "death_reanimate");
    EXPECT_EQ(yeti->tuning.regenerate.up, "regenerate_01");
    EXPECT_FLOAT_EQ(yeti->tuning.regenerate.time, 7);
    EXPECT_TRUE(panther->tuning.regenerate.clip.empty()); // a type-2 ref: the role death clip
    EXPECT_EQ(panther->tuning.regenerate.up, "death_thrall_regen");
    EXPECT_NEAR(panther->tuning.regenerate.time, 400 / 30.0f, 1e-4f);
    ASSERT_EQ(viking->tuning.regenerate.kills.size(), 4u); // {any, type 1}, classes 1, 2, 10
    EXPECT_EQ(yeti->tuning.regenerate.kills.size(), 3u);   // no class 10

    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    const game::ai::Target far{{4000, 4000, 0}};
    for (const auto& type : {viking, yeti, panther}) {
        SCOPED_TRACE(type->name);
        game::ai::Enemies es;
        auto& t = es.add(type, 1, {0, 0, 0}, 0);
        const float full = t.health();
        for (int i = 0; i < 50 && t.state().mode != Mode::Down; ++i) t.onHit(gunHit({1, 0, 0}, 100));
        ASSERT_EQ(t.state().mode, Mode::Down);
        EXPECT_EQ(t.health(), full);
        EXPECT_EQ(t.state().clip, type->tuning.regenerate.clip.empty() ? type->clipName(Role::Death) : type->tuning.regenerate.clip);
        t.onHit(gunHit({1, 0, 0}, 5000)); // absorbed while down
        EXPECT_EQ(t.state().mode, Mode::Down);
        EXPECT_EQ(t.health(), full);
        const int rose = tickUntil(es, col, far, {}, 2000, [&] { return t.state().mode == Mode::Rise; });
        EXPECT_NEAR(rose * kDt, type->tuning.regenerate.time, 2 * kDt);
        EXPECT_EQ(t.state().clip, type->tuning.regenerate.up);
        ASSERT_GE(tickUntil(es, col, far, {}, 600, [&] { return t.state().mode == Mode::Chase; }), 0);
        // Down again, then Mjolnir finishes it.
        for (int i = 0; i < 50 && t.state().mode != Mode::Down; ++i) t.onHit(gunHit({1, 0, 0}, 300));
        for (int i = 0; i < 5 && t.alive(); ++i) t.onHit(boltHit({1, 0, 0}));
        EXPECT_EQ(t.state().mode, Mode::Dead);
        for (int i = 0; i < 1200; ++i) es.tick(col, far, {}, kDt);
        EXPECT_EQ(t.state().mode, Mode::Dead); // for good
    }
    { // a bolt on a standing viking: dead at once
        game::ai::Enemies es;
        auto& t = es.add(viking, 1, {0, 0, 0}, 0);
        t.onHit(boltHit({1, 0, 0}));
        EXPECT_EQ(t.state().mode, Mode::Dead);
        // grenades (class 10) knock a viking down, not a yeti
        auto& v = es.add(viking, 2, {1000, 0, 0}, 0);
        game::Hit blast{5000, {}, {1, 0, 0}, game::Team::Player};
        blast.damageClass = 10;
        v.onHit(blast);
        EXPECT_EQ(v.state().mode, Mode::Down);
        auto& y = es.add(yeti, 3, {2000, 0, 0}, 0);
        y.onHit(blast);
        EXPECT_EQ(y.state().mode, Mode::Dead);
    }
}

// Death timer and chunk data (docs/formats/PHYSICS.md "Ragdolls"): death entries' ragdoll delays (record +4 / +8,
// counts), the retry timer +0x1C, and the thralls' chunk objects (+0x46): viking / mayan thralls break into the 14
// pieces of viking_thrall_chunkdoll. A bolt kills a viking; physics takes it 6..45 counts later: chunks.
TEST(AiReal, DeathTimerAndThrallChunks) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto poacher = game::ai::loadEnemyType(bf, "poacher"), viking = game::ai::loadEnemyType(bf, "viking_thrall"),
               mayan = game::ai::loadEnemyType(bf, "mayan_thrall"), panther = game::ai::loadEnemyType(bf, "panther_thrall");
    ASSERT_EQ(poacher->tuning.deaths.size(), 6u);
    EXPECT_EQ(poacher->tuning.deaths[0].min, 52);
    EXPECT_EQ(poacher->tuning.deaths[0].max, 60);
    EXPECT_FLOAT_EQ(poacher->tuning.deathTimer, 4.0f); // 120 counts
    EXPECT_FALSE(poacher->chunks);
    ASSERT_TRUE(viking->chunks && mayan->chunks && panther->chunks);
    EXPECT_EQ(viking->chunks->object, "viking_thrall_chunkdoll");
    EXPECT_EQ(mayan->chunks->object, "viking_thrall_chunkdoll");
    EXPECT_EQ(panther->chunks->object, "panther_thrall_chunkdoll");
    const auto& c = *viking->chunks;
    ASSERT_EQ(c.pieces.size(), 14u);
    EXPECT_EQ(c.minimum, -1);
    EXPECT_EQ(c.pieces[1].model, 2);
    EXPECT_EQ(c.pieces[1].segA, 42);
    EXPECT_EQ(c.pieces[1].segB, 43);
    EXPECT_EQ(c.pieces[6].flags, 1u);
    EXPECT_EQ(c.models.size(), 6u);
    for (const auto& p : c.pieces) {
        EXPECT_LT(size_t(std::max(p.segA, p.segB)), viking->skeleton.bones.size());
        const auto& m = c.models[size_t(p.model)];
        EXPECT_FALSE(m.prims.empty()) << "model " << p.model;
        EXPECT_GT(physics::massProps(m).mass, 0) << "model " << p.model;
    }

    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto col = built(m);
    game::ai::Enemies es;
    auto& t = es.add(viking, 1, {0, 0, 0}, 0);
    t.onHit(boltHit({1, 0, 0}));
    ASSERT_EQ(t.state().mode, Mode::Dead);
    EXPECT_GE(t.state().ragdollAt, 6 / 30.0f - 1e-5f);
    EXPECT_LE(t.state().ragdollAt, 45 / 30.0f + 1e-5f);
    const int ticks = tickUntil(es, col, {{4000, 4000, 0}}, {}, 200, [&] { return t.state().ragdollPhase != 0; });
    EXPECT_NEAR(ticks * kDt, t.state().ragdollAt, 2 * kDt);
    EXPECT_EQ(t.state().ragdollPhase, 4) << "chunks, not the ragdoll";
}

// l1_ship_exterior's mercs: each intro script's constructor calls enemybase 27 GuardNode(GetUnit(), noderef, r1, r2)
// with its own planner node {0, n} (1650: node 127, 2500 / 4000); a merc outside its zone walks back into it, a chase
// stops at 0.8 x the outer radius; the first to see Lara barks and alerts the soldiers around it; after a burst a merc
// runs to a planner node that hides it crouched and crouches there; a dead merc's rifle gives ammo (Stream.cpp).
TEST(AiReal, MercsGuardZonesCoverAndAlert) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto u = game::loadUnitData(bf, "l1_ship_exterior");
    std::vector<std::vector<vec3>> nodes; // positions
    for (const auto& r : formats::parseUnit(*u.drm).plannerNodes)
        for (auto& n = nodes.emplace_back(); const auto& p : r) n.push_back(p.pos());
    ASSERT_EQ(nodes.size(), 1u);
    ASSERT_EQ(nodes[0].size(), 187u);
    EXPECT_NEAR(nodes[0][127].x, 4260.1f, 0.5f);
    EXPECT_NEAR(nodes[0][127].y, 15066.9f, 0.5f);

    // The scripts' GuardNode calls, through the World.
    {
        game::World w(game::UnitLoader([&bf](const std::string& n) { return game::loadUnitData(bf, n); }), "l1_ship_exterior",
                      game::loadLaraAnims(bf), game::Spawn{{-71, 9794, 5706}, 0});
        w.script().setBigfile(&bf);
        w.enemies().setBigfile(&bf);
        w.tick({}); // the mercs are dormant intros (+0x72 0x80): born as dispatchers 1645 / 1646 / 1648 do (instanceref Birth)
        for (const int32_t uid : {1650, 1651, 1652, 1653, 1660}) w.script().birth(uid);
        for (int i = 0; i < 3; ++i) w.tick({});
        const std::map<int32_t, std::tuple<int, float, float>> expected{
            {1650, {127, 2500, 4000}}, {1651, {129, 3000, 4500}}, {1652, {168, 1400, 2800}}, {1653, {8, 500, 1250}}, {1660, {77, 2000, 3500}}};
        int seen = 0;
        for (const auto& e : w.enemies().all()) {
            const auto it = expected.find(e->uid());
            if (it == expected.end()) continue;
            ++seen;
            const auto [node, inner, outer] = it->second;
            ASSERT_EQ(e->zones.size(), 1u) << e->uid();
            EXPECT_LT(glm::distance(e->zones[0].point, nodes[0][size_t(node)]), 1.0f) << e->uid(); // the start unit sits at offset 0
            EXPECT_EQ(e->zones[0].inner, inner) << e->uid();
            EXPECT_EQ(e->zones[0].outer, outer) << e->uid();
            std::fprintf(stderr, "merc %d at (%.0f %.0f %.0f): zone node %d (%.0f %.0f %.0f) %.0f / %.0f\n", e->uid(), e->position().x,
                         e->position().y, e->position().z, node, e->zones[0].point.x, e->zones[0].point.y, e->zones[0].point.z, inner, outer);
        }
        EXPECT_EQ(seen, 5);
        // saved with the enemies
        const auto j = w.enemies().serialize();
        EXPECT_EQ(j.at("guards").at("1650").at(0).at(1).get<int>(), 127);
    }

    u.col.build();
    const auto fresh = [&] {
        auto es = std::make_unique<game::ai::Enemies>();
        es->setBigfile(&bf);
        es->setUnits({{u.name, u.drm, vec3(0)}});
        return es;
    };
    const auto find = [](game::ai::Enemies& es, int32_t uid) -> game::ai::Enemy& {
        return **std::find_if(es.all().begin(), es.all().end(), [&](const auto& e) { return e->uid() == uid; });
    };
    const game::ai::Target away{{0, 0, -50000}};
    // Back into its zone: 1650 stands 2 700 from node 127 (inner 2 500).
    {
        auto es = fresh();
        es->guard(1650, 0, 127, 2500, 4000);
        auto& m = find(*es, 1650);
        m.patrolPaths.clear(); // zone return only (its patrol path below)
        m.setPatrol(0);
        const vec3 node = nodes[0][127];
        EXPECT_GT(glm::length(glm::vec2(m.position() - node)), 2500);
        bool returned = false;
        for (int i = 0; i < 1200; ++i) {
            es->tick(u.col, away, {}, kDt);
            returned |= m.state().mode == Mode::Return;
        }
        EXPECT_TRUE(returned);
        EXPECT_EQ(m.state().mode, Mode::Idle);
        EXPECT_LT(glm::length(glm::vec2(m.position() - node)), 2000 + 50);
        std::fprintf(stderr, "merc 1650 back in its zone at (%.0f %.0f), %.0f from node 127\n", m.position().x, m.position().y,
                     glm::length(glm::vec2(m.position() - node)));
    }
    // A chase stops at the zone's edge: Lara last seen 9 000 beyond node 8 (outer 1 250).
    {
        auto es = fresh();
        es->guard(1653, 0, 8, 500, 1250);
        auto& m = find(*es, 1653);
        const vec3 node = nodes[0][8], far = node + vec3(0, -9000, 0);
        m.alert(far);
        float maxOut = 0;
        for (int i = 0; i < 600; ++i) {
            es->tick(u.col, {far}, {}, kDt);
            maxOut = std::max(maxOut, glm::length(glm::vec2(m.position() - node)));
        }
        EXPECT_LT(maxOut, 1250 * 0.8f + m.radius() + 20);
        EXPECT_GT(maxOut, 1250 * 0.8f - 2 * m.radius());
        std::fprintf(stderr, "merc 1653 chase held %.0f from node 8 (outer 1250)\n", maxOut);
    }
    // Spotting, barking, alerting the squad; then cover. Lara stands on a planner node that one merc sees, a squad mate
    // within 3 000 does not, and from which a node near the spotter is hidden at crouch height.
    {
        auto es = fresh();
        const auto hidden = [&](vec3 from, vec3 at) {
            const vec3 chest = at + vec3(0, 0, 512 * 0.6f), low = from + vec3(0, 0, 150);
            return u.col.raycast(low, glm::normalize(chest - low), glm::distance(chest, low)).has_value();
        };
        game::ai::Enemy* spotter = nullptr;
        std::optional<vec3> spot;
        for (const auto& m : es->all()) {
            if (!m->type().ranged()) continue;
            for (const vec3& n : nodes[0]) {
                const float d = glm::length(glm::vec2(n - m->position()));
                if (d < 1500 || d > 4000 || std::abs(n.z - m->position().z) > 200 || !m->perceives(u.col, {n})) continue;
                const bool cover = std::any_of(nodes[0].begin(), nodes[0].end(), [&](const vec3& c) {
                    const float dc = glm::length(glm::vec2(c - m->position()));
                    return dc > 95 && dc < 1500 && std::abs(c.z - m->position().z) < 320 &&
                           glm::length(glm::vec2(c - n)) >= 0.5f * d && hidden(c, n);
                });
                const bool squad = std::any_of(es->all().begin(), es->all().end(), [&](const auto& o) {
                    return o != m && o->type().ranged() && glm::distance(o->position(), m->position()) < 3000 && !o->perceives(u.col, {n});
                });
                if (cover && squad) { spotter = m.get(), spot = n; break; }
            }
            if (spotter) break;
        }
        ASSERT_TRUE(spot);
        const game::ai::Target lara{*spot};
        std::vector<game::ai::Enemy*> blind;
        for (const auto& e : es->all())
            if (e.get() != spotter && e->type().ranged() && glm::distance(e->position(), spotter->position()) < 3000 && !e->perceives(u.col, lara))
                blind.push_back(e.get());
        es->tick(u.col, lara, {}, kDt);
        EXPECT_EQ(spotter->state().mode, Mode::Alert);
        EXPECT_TRUE(spotter->state().clip.starts_with("combat_bark_0")) << spotter->state().clip;
        ASSERT_EQ(es->barks.size(), 1u);
        EXPECT_EQ(es->barks[0].category, "EAR");
        EXPECT_EQ(es->barks[0].uid, spotter->uid());
        for (const auto* b : blind) EXPECT_EQ(b->state().mode, Mode::Alert) << b->uid(); // told by the spotter
        std::fprintf(stderr, "merc %d spots Lara at (%.0f %.0f %.0f): %zu squad mates who cannot see her alerted\n", spotter->uid(),
                     spot->x, spot->y, spot->z, blind.size());
        int hurt = 0;
        game::ai::Enemy* covering = nullptr;
        for (int i = 0; i < 3600 && !covering; ++i) {
            es->tick(u.col, lara, [&](float, vec3, int) { ++hurt; }, kDt);
            for (const auto& e : es->all())
                if (e->state().mode == Mode::Cover) covering = e.get();
        }
        ASSERT_NE(covering, nullptr);
        EXPECT_GT(hurt, 0);
        const vec3 cover = covering->state().cover;
        const vec3 chest = lara.pos + vec3(0, 0, lara.height * 0.6f);
        const vec3 low = cover + vec3(0, 0, 150);
        EXPECT_TRUE(u.col.raycast(low, glm::normalize(chest - low), glm::distance(chest, low)).has_value());
        bool crouched = false;
        for (int i = 0; i < 600 && covering->state().mode == Mode::Cover; ++i) {
            es->tick(u.col, lara, {}, kDt);
            crouched |= covering->state().clip == "combat_idle_crouch";
        }
        EXPECT_TRUE(crouched);
        std::fprintf(stderr, "merc %d took cover at (%.0f %.0f %.0f)\n", covering->uid(), cover.x, cover.y, cover.z);
    }
    // Their guns: wp_akrifle (the body's rounds, World::collectPickups).
    EXPECT_EQ(find(*fresh(), 1650).type().weaponObject, "wp_akrifle");
}

// Merc patrols from their intro data (+0x34 planner record, +0x38 path, +0x88 order) with the patrol / search
// records of merc.drm; 1650 walks its ping-pong path, idles at the points, searches after losing Lara and returns
// to its path.
TEST(AiReal, MercPatrolsSearchesAndReturns) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto u = game::loadUnitData(bf, "l1_ship_exterior");
    const auto parsed = formats::parseUnit(*u.drm);
    const auto in = std::find_if(parsed.intros.begin(), parsed.intros.end(), [](const formats::Intro& i) { return i.uniqueId == 1650; });
    ASSERT_NE(in, parsed.intros.end());
    EXPECT_EQ(in->patrolRecord, 0);
    EXPECT_EQ(in->patrolMode, 2u);
    ASSERT_EQ(in->patrolPaths.size(), 1u);
    EXPECT_EQ(in->patrolPaths[0].nodes, (std::vector<uint32_t>{128, 126, 108, 114, 117, 131}));
    EXPECT_EQ(in->patrolPaths[0].pointMode, 4);
    int patrolling = 0, once = 0;
    for (const auto& i : parsed.intros) patrolling += !i.patrolPaths.empty() && !i.patrolPaths[0].nodes.empty(), once += i.patrolMode == 1 && !i.patrolPaths.empty();
    EXPECT_EQ(patrolling, 18); // 1650-1652, 1654-1659, 1661-1669 (1653 / 1660 have a record but no path)
    std::fprintf(stderr, "l1_ship_exterior: %d intros with a patrol path, %d of them once\n", patrolling, once);

    u.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{u.name, u.drm, vec3(0)}});
    es.guard(1650, 0, 127, 2500, 4000);
    auto& m = **std::find_if(es.all().begin(), es.all().end(), [](const auto& e) { return e->uid() == 1650; });
    const auto& ty = m.type();
    EXPECT_EQ(ty.patrol.walk, "walk_forward_passive");
    ASSERT_EQ(ty.patrol.idles.size(), 5u);
    EXPECT_EQ(ty.patrol.idles[0].clip, "idle_passive");
    EXPECT_EQ(ty.patrol.idles[0].weight, 70u);
    EXPECT_NEAR(ty.patrol.idles[0].minTime, 3.0f, 1e-4f);
    EXPECT_NEAR(ty.patrol.idles[0].maxTime, 4.0f, 1e-4f);
    EXPECT_EQ(ty.patrol.arrive, 205.0f);
    EXPECT_EQ(ty.search.walk, "combat_walk_search");
    EXPECT_EQ(ty.search.start, "combat_idle_search");
    EXPECT_NEAR(ty.search.time, 1600.0f / 30.0f, 1e-3f);
    ASSERT_EQ(ty.search.idles.size(), 2u);
    EXPECT_EQ(ty.search.idles[1].clip, "idle_combat_stand");
    ASSERT_EQ(m.patrolPaths.size(), 1u);
    EXPECT_LT(glm::distance(m.patrolPaths[0][0].pos, parsed.plannerNodes[0][128].pos()), 1.0f);

    const game::ai::Target away{{0, 0, -50000}};
    const auto run = [&](float seconds, auto&& each) {
        for (int i = 0; i < int(seconds / kDt); ++i) es.tick(u.col, away, {}, kDt), each();
    };
    std::set<int> reached; // patrol points it idled at
    run(60, [&] {
        if (m.state().mode == Mode::Patrol && m.state().phase == 1) reached.insert(m.state().patrolIndex);
    });
    EXPECT_GE(reached.size(), 2u);
    EXPECT_TRUE(reached.count(0));
    std::fprintf(stderr, "merc 1650 idled at %zu patrol points in 60 s, now at (%.0f %.0f %.0f) [%s]\n", reached.size(), m.position().x,
                 m.position().y, m.position().z, m.state().clip.c_str());
    // A squad mate's alert, nothing seen after it: chase, search, back on its path.
    m.alert(m.position() + vec3(std::cos(m.state().yaw), std::sin(m.state().yaw), 0) * 800.0f);
    bool searched = false, back = false;
    run(80, [&] {
        searched |= m.state().mode == Mode::Search;
        back |= searched && m.state().mode == Mode::Patrol;
    });
    EXPECT_TRUE(searched);
    EXPECT_TRUE(back);
}

// l1_ship_exterior: the NavPower graph (Level +0x60) decodes to 398 convex areas with symmetric adjacency; mercs walk
// its layer 3 (radius 100); every soldier reads the same flank record; a gunshot on the deck makes merc 1650
// investigate it along the mesh; fighting Lara it flanks to a free planner node at least 4 m from her, inside its
// guard zone; a save taken as it flanks continues bit for bit.
TEST(AiReal, ShipNavGraphFlankAndInvestigate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto u = game::loadUnitData(bf, "l1_ship_exterior");
    const auto parsed = formats::parseUnit(*u.drm);
    ASSERT_EQ(parsed.navGraphs.size(), 1u);
    const auto& g = parsed.navGraphs[0];
    EXPECT_EQ(g.layer, 3u);
    EXPECT_EQ(g.radius, 100.0f);
    EXPECT_EQ(g.height, 700.0f);
    ASSERT_EQ(g.areas.size(), 398u);
    size_t links = 0;
    for (size_t a = 0; a < g.areas.size(); ++a)
        for (const int32_t b : g.areas[a].adj)
            if (b >= 0) {
                ++links;
                EXPECT_TRUE(std::ranges::count(g.areas[size_t(b)].adj, int32_t(a))) << a << " -> " << b;
            }
    EXPECT_EQ(links, 710u);
    EXPECT_EQ(g.areas[0].verts.size(), 3u);
    EXPECT_EQ(g.areas[0].centre, vec3(4000, 27328, 5472));

    u.col.build();
    const auto fresh = [&] {
        auto es = std::make_unique<game::ai::Enemies>();
        es->setBigfile(&bf);
        es->setUnits({{u.name, u.drm, vec3(0)}});
        es->guard(1650, 0, 127, 2500, 4000); // its script's GuardNode
        return es;
    };
    auto es = fresh();
    const auto find = [](game::ai::Enemies& e, int32_t uid) -> game::ai::Enemy& {
        return **std::find_if(e.all().begin(), e.all().end(), [&](const auto& x) { return x->uid() == uid; });
    };
    auto& m = find(*es, 1650);
    const auto& f = m.type().flank;
    EXPECT_TRUE(f.valid && f.inside);
    EXPECT_EQ(f.closeDist, 1264.0f);
    EXPECT_EQ(f.nearDist, 252.0f);
    EXPECT_EQ(f.gather, 7901.0f);
    EXPECT_EQ(f.travelW + f.laraW + f.dirW + f.coneW, 0.0f);
    EXPECT_NEAR(f.closeCost, 1e11f, 1e6f);
    EXPECT_NEAR(m.type().investigate.hear, 4740.83f, 0.01f);
    EXPECT_EQ(m.type().investigate.walk, "combat_walk_search");
    EXPECT_TRUE(m.type().dodge.left.empty()); // the merc's sidestep clips are anim requests
    const auto* nav = es->navFor(m.type());
    ASSERT_NE(nav, nullptr);
    EXPECT_EQ(nav->radius, 100.0f);
    const auto& path = m.patrolPaths[0];
    EXPECT_FALSE(nav->path(path[0].pos, path[5].pos).empty()); // its patrol's ends are joined on the mesh

    // A gunshot 1.5..4 km from it on the mesh.
    std::optional<vec3> shot;
    for (const auto& r : parsed.plannerNodes)
        for (const auto& n : r) {
            const float d = glm::length(glm::vec2(n.pos() - m.position()));
            if (!shot && d > 1500 && d < 4000 && std::abs(n.pos().z - m.position().z) < 200 && !nav->path(m.position(), n.pos()).empty()) shot = n.pos();
        }
    ASSERT_TRUE(shot);
    const game::ai::Target away{{0, 0, -50000}};
    es->disturb(*shot);
    EXPECT_EQ(m.state().mode, Mode::Investigate);
    float nearest = 1e9f;
    for (int i = 0; i < int(45 / kDt) && m.state().mode == Mode::Investigate; ++i)
        es->tick(u.col, away, {}, kDt), nearest = std::min(nearest, glm::length(glm::vec2(m.position() - *shot)));
    EXPECT_LT(nearest, 400.0f);
    EXPECT_NE(m.state().mode, Mode::Investigate);
    std::fprintf(stderr, "merc 1650 investigated (%.0f %.0f %.0f), came within %.0f (arrival %.0f), now %s at (%.0f %.0f %.0f)\n", shot->x,
                 shot->y, shot->z, nearest, m.type().investigate.arrive, std::string(game::ai::modeName(m.state().mode)).c_str(),
                 m.position().x, m.position().y, m.position().z);

    // A fight: Lara on a node it sees.
    std::optional<vec3> spot;
    for (const auto& r : parsed.plannerNodes)
        for (const auto& n : r) {
            const float d = glm::length(glm::vec2(n.pos() - m.position()));
            if (!spot && d > 1500 && d < 4000 && std::abs(n.pos().z - m.position().z) < 200 && m.perceives(u.col, {n.pos()})) spot = n.pos();
        }
    ASSERT_TRUE(spot);
    const game::ai::Target lara{*spot};
    std::vector<Mode> modes;
    for (int i = 0; i < int(60 / kDt) && m.state().mode != Mode::Flank; ++i) {
        es->tick(u.col, lara, {}, kDt);
        if (modes.empty() || modes.back() != m.state().mode) modes.push_back(m.state().mode);
    }
    for (const Mode md : modes) std::fprintf(stderr, "%s ", std::string(game::ai::modeName(md)).c_str());
    std::fprintf(stderr, "\n");
    ASSERT_EQ(m.state().mode, Mode::Flank);
    const vec3 goal = m.state().goal;
    EXPECT_TRUE(std::ranges::any_of(parsed.plannerNodes[0], [&](const auto& n) { return glm::distance(n.pos(), goal) < 1; }));
    EXPECT_GE(glm::length(glm::vec2(goal - *spot)), 1264.0f);
    EXPECT_GE(glm::length(glm::vec2(goal - m.position())), 252.0f);
    ASSERT_FALSE(m.zones.empty());
    EXPECT_TRUE(m.zones[0].contains(goal, true));
    std::fprintf(stderr, "merc 1650 flanks from (%.0f %.0f) to (%.0f %.0f), Lara at (%.0f %.0f)\n", m.position().x, m.position().y, goal.x,
                 goal.y, spot->x, spot->y);

    // Save now, load into fresh enemies: both continue identically.
    auto loaded = fresh();
    loaded->restore(es->serialize());
    loaded->setUnits({{u.name, u.drm, vec3(0)}});
    ASSERT_EQ(loaded->all().size(), es->all().size());
    float closest = 1e9f;
    for (int i = 0; i < int(10 / kDt); ++i) {
        es->tick(u.col, lara, {}, kDt), loaded->tick(u.col, lara, {}, kDt);
        closest = std::min(closest, glm::length(glm::vec2(m.position() - goal)));
        for (size_t k = 0; k < es->all().size(); ++k) {
            const auto &a = es->all()[k]->state(), &b = loaded->all()[k]->state();
            ASSERT_TRUE(a.pos == b.pos && a.yaw == b.yaw && a.mode == b.mode && a.clipTime == b.clipTime && a.rng == b.rng) << i << " " << k;
        }
    }
    EXPECT_LT(closest, std::max(f.arrive, m.radius()) + 30.0f);
}

// Survey (OU_AI_SURVEY=1): every enemy intro and generator spawn of every unit, and each enemy type's data and clips.
TEST(AiReal, Survey) {
    if (!gameDir() || !std::getenv("OU_AI_SURVEY")) GTEST_SKIP() << "OU_GAME_DIR / OU_AI_SURVEY not set";
    const cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    const auto ul = bf.read("\\tr8\\pc-w\\unitlist.txt");
    const std::string text(ul->begin(), ul->end());
    std::vector<std::string> units;
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty()) units.push_back(line.substr(line.rfind('\\') + 1));
    }
    std::map<std::string, bool> enemy;
    std::set<std::string> types;
    const char* keys[] = {"natla", "amanda", "dopp", "kraken", "tentacle", "boss", "jorm", "thrall", "merc", "spider", "yeti",
                          "guard", "lara_", "shadow", "rutland", "zip", "winston", "alister"};
    for (const auto& u : units) {
        const auto bytes = bf.read("pc-w\\" + u + ".drm");
        if (!bytes) continue;
        const auto drm = cdc::parseDrm(*bytes);
        std::map<std::string, std::vector<int>> found;
        for (const auto& in : formats::parseUnit(drm).intros) {
            const auto n = names.find(in.objectId);
            if (n == names.end()) continue;
            auto [it, fresh] = enemy.try_emplace(n->second, false);
            if (fresh) it->second = game::ai::isEnemyObject(bf, n->second);
            bool hit = it->second || n->second.ends_with("_generator");
            for (const char* k : keys) hit |= n->second.find(k) != std::string::npos;
            if (hit) found[n->second].push_back(in.uniqueId);
            if (it->second && std::getenv("OU_AI_SURVEY_POS"))
                std::fprintf(stderr, "POS %s %s %d %.0f %.0f %.0f\n", u.c_str(), n->second.c_str(), in.uniqueId, in.position[0], in.position[1], in.position[2]);
            if (it->second && (std::abs(in.rotation[0]) > 0.05f || std::abs(in.rotation[1]) > 0.05f))
                std::fprintf(stderr, "TILTED %s %s %d rot %.2f %.2f %.2f pos %.0f %.0f %.0f flags %02x %02x %02x %02x\n", u.c_str(), n->second.c_str(),
                             in.uniqueId, in.rotation[0], in.rotation[1], in.rotation[2], in.position[0], in.position[1], in.position[2],
                             in.flags[0], in.flags[1], in.flags[2], in.flags[3]);
            if (it->second) types.insert(n->second);
        }
        for (const auto& [n, ids] : found) {
            std::fprintf(stderr, "UNIT %s %s x%zu:", u.c_str(), n.c_str(), ids.size());
            for (int id : ids) std::fprintf(stderr, " %d", id);
            std::fprintf(stderr, "\n");
            if (n.ends_with("_generator"))
                if (const auto d = bf.read("pc-w\\" + n + ".drm")) {
                    const auto gd = cdc::parseDrm(*d);
                    if (const auto b = formats::objectBaseSection(gd)) {
                        uint16_t id;
                        std::memcpy(&id, gd.sections[*b].data.data() + 0xDE, 2);
                        if (names.count(id)) types.insert(names.at(id)), std::fprintf(stderr, "  -> spawns %s\n", names.at(id).c_str());
                    }
                }
        }
    }
    for (const auto& n : types) {
        try {
            const auto t = game::ai::loadEnemyType(bf, n);
            std::fprintf(stderr, "TYPE %s class %d beh %d hp %.0f/%.0f/%.0f stagger %.0f r %.0f h %.0f weapon %u spawnable %d attacks %zu\n", n.c_str(),
                         t->enemyClass, t->behaviour, t->tuning.healthLevels[0], t->tuning.health, t->tuning.healthLevels[2],
                         t->tuning.staggerDamage, t->tuning.radius, t->tuning.height, t->weapon, t->spawnable(), t->tuning.attacks.size());
            const auto& rg = t->tuning.regenerate;
            std::fprintf(stderr, "  regen clip '%s' up '%s' time %.2f kills", rg.clip.c_str(), rg.up.c_str(), rg.time);
            for (const auto& k : rg.kills) std::fprintf(stderr, " {src %d type %d flag %d}", k.source, k.type, k.flag);
            std::fprintf(stderr, "\n  deaths");
            for (const auto& d : t->tuning.deaths) std::fprintf(stderr, " %s/%d/%d", d.clip.c_str(), d.category, d.flag);
            std::fprintf(stderr, "\n");
            for (const auto& a : t->tuning.attacks)
                std::fprintf(stderr, "  atk +%X clip %s shot %s follow %s dmg %.0f type %d\n", a.node, a.clip.c_str(), a.shotClip.c_str(),
                             a.followClip.c_str(), a.damage, a.type);
        } catch (const std::exception& e) { std::fprintf(stderr, "TYPE %s error %s\n", n.c_str(), e.what()); }
        const auto d = bf.read("pc-w\\" + n + ".drm");
        if (!d) continue;
        std::fprintf(stderr, "  CLIPS");
        for (const auto& a : anim::animList(cdc::parseDrm(*d))) std::fprintf(stderr, " %s", a.name.c_str());
        std::fprintf(stderr, "\n");
    }
}

// NavPower links (unit data +0x44 / +0x48): l4_ballcourt has 4 strips (drops of ~1058 from the court terrace), each
// type 0, usage 0xFFFF; the panther's usage (node +0x14C) is bit 2, the poacher's bit 1. On the panther's layer the
// first strip joins the terrace to the ground below: chasing Lara down there, a panther runs to the edge and jumps
// down with jump_down_start / jump_down_inair_static / jump_down_end.
float flatDistance(vec3 a, vec3 b) { return glm::length(glm::vec2(b - a)); }

TEST(AiReal, BallcourtPantherJumpsDownALink) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto u = game::loadUnitData(bf, "l4_ballcourt");
    const auto parsed = formats::parseUnit(*u.drm);
    ASSERT_EQ(parsed.navLinks.size(), 4u);
    for (const auto& l : parsed.navLinks) {
        EXPECT_TRUE(l.strip);
        EXPECT_EQ(l.type, 0);
        EXPECT_EQ(l.usage, 0xFFFF);
        EXPECT_NEAR(l.maxSnap, 632.11f, 0.01f);
        EXPECT_NEAR(l.mayUse, 474.08f, 0.01f);
        EXPECT_EQ(l.penalty, 1.0f);
    }
    const auto& l0 = parsed.navLinks[0];
    EXPECT_NEAR(l0.a[0].x, -19054.7f, 0.1f);
    EXPECT_NEAR(l0.a[0].z - l0.b[0].z, 1058.5f, 1.0f);
    EXPECT_TRUE(l0.fromA);
    EXPECT_FALSE(l0.fromB);
    const auto panther = game::ai::loadEnemyType(bf, "cat_panther");
    const auto poacher = game::ai::loadEnemyType(bf, "poacher");
    EXPECT_EQ(panther->linkUsage, 4);
    EXPECT_EQ(poacher->linkUsage, 2);
    EXPECT_EQ(panther->jumpDown.start, "jump_down_start");
    EXPECT_EQ(panther->jumpUp.air, "jump_up_inair_static");
    EXPECT_EQ(panther->jumpAcross.land, "jump_across_end");
    EXPECT_FALSE(poacher->jumpDown.valid()); // soldiers have no jump clips

    u.col.build();
    game::ai::Enemies es;
    es.setBigfile(&bf);
    es.setUnits({{u.name, u.drm, vec3(0)}});
    const game::ai::NavMesh* nav = es.navFor(*panther);
    ASSERT_NE(nav, nullptr);
    std::fprintf(stderr, "l4_ballcourt: panther layer radius %.0f, %zu areas, %zu links snapped\n", nav->radius, nav->size(), nav->links());
    ASSERT_GE(nav->links(), 1u);
    const vec3 a = (l0.a[0] + l0.a[1]) * 0.5f, b = (l0.b[0] + l0.b[1]) * 0.5f;
    const vec3 across = vec3(glm::normalize(glm::vec2(b - a)), 0);
    const vec3 from = a - across * 500.0f, to = b + across * 500.0f;
    ASSERT_GE(nav->locate(from), 0) << from.x << " " << from.y << " " << from.z;
    ASSERT_GE(nav->locate(to), 0) << to.x << " " << to.y << " " << to.z;
    EXPECT_TRUE(nav->route(from, to, 0).empty() || nav->route(from, to, 0).size() > 3); // no direct way down
    const auto r = nav->route(from, to, panther->linkUsage);
    ASSERT_FALSE(r.empty());
    const auto jump = std::find_if(r.begin(), r.end(), [](const auto& s) { return s.link >= 0; });
    ASSERT_NE(jump, r.end());
    EXPECT_LT(flatDistance(jump->at, a), 400.0f);
    EXPECT_LT(flatDistance(jump->land, b), 400.0f);

    game::ai::Enemy e(panther, 1, from, std::atan2(across.y, across.x));
    e.nav = nav;
    const game::ai::Target lara{to};
    e.alert(to);
    std::vector<std::string> clips;
    std::vector<game::ai::EnemyState> states;
    for (int i = 0; i < int(15 / kDt); ++i) {
        e.tick(u.col, lara, {}, kDt);
        if (clips.empty() || clips.back() != e.state().clip) clips.push_back(e.state().clip);
        states.push_back(e.state());
    }
    for (const auto& c : clips) std::fprintf(stderr, "%s ", c.c_str());
    std::fprintf(stderr, "\nend (%.0f %.0f %.0f)\n", e.position().x, e.position().y, e.position().z);
    const auto at = [&](const char* c) { return std::find(clips.begin(), clips.end(), c) - clips.begin(); };
    ASSERT_LT(at("jump_down_start"), at("jump_down_inair_static"));
    ASSERT_LT(at("jump_down_inair_static"), at("jump_down_end"));
    ASSERT_LT(at("jump_down_end"), std::ssize(clips));
    EXPECT_LT(e.position().z, b.z + 150.0f); // down on the ground
    EXPECT_LT(flatDistance(e.position(), to), 600.0f);
}
