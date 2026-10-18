// Enemy AI (src/game/ai): perception, chase, attack timing, hit/death, ledge avoidance, render-rate
// independence; real data: enemy intros of the surveyed units resolve to skinned enemy types.
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
        return [this](float d, vec3) { blows.push_back({tick, d}); };
    }
};

} // namespace

TEST(Ai, PerceptionConeDistanceAndWalls) {
    CollisionMesh m;
    addFloor(m, -5000, -5000, 5000, 5000, 0);
    const auto open = built(m);
    addBox(m, {900, -1000, 0}, {1000, 1000, 1500});
    const auto walled = built(m);
    game::ai::Enemy e(syntheticType(), 1, {0, 0, 0}, 0); // facing +X
    const auto& t = e.type().tuning;
    EXPECT_TRUE(e.perceives(open, {{2000, 0, 0}}));
    EXPECT_FALSE(e.perceives(walled, {{2000, 0, 0}}));                      // wall in the line of sight
    EXPECT_FALSE(e.perceives(open, {{t.sightRange + 100, 0, 0}}));          // too far
    EXPECT_FALSE(e.perceives(open, {{-2000, 0, 0}}));                       // behind, outside the sense radius
    EXPECT_TRUE(e.perceives(open, {{-(t.senseRange - 50), 0, 0}}));        // behind but close: sensed
    EXPECT_FALSE(e.perceives(open, {{1000, 1000 * std::tan(t.sightHalfAngle + 0.1f), 0}})); // outside the cone
    EXPECT_TRUE(e.perceives(open, {{1000, 1000 * std::tan(t.sightHalfAngle - 0.1f), 0}}));
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
    for (int i = 0; i < 120; ++i) es.tick(col, {{300, 0, 0}}, [](float, vec3) { FAIL() << "dead enemies do not attack"; }, kDt);
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

// ---- real data (OU_GAME_DIR) ------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
} // namespace

// Every enemy intro (ObjectBase oflags2 0x400000) and generator spawn of Mexico (l4_*) and of Thailand's first enemy
// units resolves to an enemy object with a model-0 skeleton and enemy data (magic, hit points); the melee ones
// (cats, thralls, spiders) also to their role clips and are spawned.
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
        if (t->melee()) EXPECT_EQ(spawned[n], intros[n]) << n; // every melee enemy intro is alive in the world
    }
    for (const auto& w : es.warnings) {
        std::fprintf(stderr, "warning: %s\n", w.c_str());
        EXPECT_NE(w.find("not spawned"), std::string::npos) << w; // only the documented ranged/flying skips
    }
    for (const char* m : {"cat_panther", "panther_thrall", "tiger", "mayan_thrall", "small_albino_spider", "giant_albino_tarantula"})
        EXPECT_TRUE(game::ai::loadEnemyType(bf, m)->melee()) << m;
    EXPECT_EQ(spawned["cat_panther"], 13); // ballcourt 4, temple 5, templepath 1, trailstart 3
    EXPECT_EQ(spawned["panther_thrall"], 1);
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
