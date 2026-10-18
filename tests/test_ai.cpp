// Enemy AI (src/game/ai): perception, chase, attack timing, hit/death, ledge avoidance, ranged fire with line of
// fire, flyers, death clips, render-rate independence; real data: decoded senses / attack / death records, enemy
// intros of the surveyed units resolve to skinned enemy types and spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
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

    // Sharks are enemies (flyer tree like bats) with health and senses but no attack records; not spawned (no water
    // volume is found in their units to swim in).
    const auto shark = game::ai::loadEnemyType(bf, "shark_blueshark");
    EXPECT_EQ(shark->enemyClass, 1);
    EXPECT_TRUE(shark->flyer());
    EXPECT_EQ(shark->tuning.health, 500);
    cone(shark->tuning.senses.eye, 11378, 180, 180), cone(shark->tuning.senses.body[0], 6321, 180, 90);
    EXPECT_TRUE(shark->tuning.attacks.empty());
    EXPECT_FALSE(shark->spawnable());
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
    const auto& e = *es.all().back();
    EXPECT_EQ(e.type().name, "mayan_thrall");
    EXPECT_EQ(e.generator, 5637);
    EXPECT_EQ(e.state().mode, Mode::Spawning);
    EXPECT_EQ(e.state().clip, "spawn_from_pool");
    for (int i = 0; i < 6.7 / kDt + 2; ++i) es.tick(unit.col, {g->pos + vec3(1500, 0, 0)}, {}, kDt);
    EXPECT_NE(e.state().mode, Mode::Spawning); // 6.7 s clip done
    es.tick(unit.col, {g->pos + vec3(1500, 0, 0)}, {}, kDt);
    EXPECT_EQ(es.all().size(), placed + 1); // generators fire once
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
            for (const auto& s : es.shots) ++shots[s.uid];
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
