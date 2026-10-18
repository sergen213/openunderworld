#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

using namespace ou;
using collision::CollisionMesh;
using game::Actions;
using game::State;
using game::vec3;

namespace {

void addTri(CollisionMesh& m, vec3 a, vec3 b, vec3 c) {
    const auto base = uint32_t(m.vertices.size());
    m.vertices.insert(m.vertices.end(), {a, b, c});
    m.faces.push_back({{base, base + 1, base + 2}});
}
// Axis-aligned quad split in two (corners in winding order).
void addQuad(CollisionMesh& m, vec3 a, vec3 b, vec3 c, vec3 d) {
    addTri(m, a, b, c);
    addTri(m, a, c, d);
}
void addFloor(CollisionMesh& m, float x0, float y0, float x1, float y1, float z) {
    for (float x = x0; x < x1; x += 200)
        for (float y = y0; y < y1; y += 200)
            addQuad(m, {x, y, z}, {std::min(x + 200, x1), y, z}, {std::min(x + 200, x1), std::min(y + 200, y1), z}, {x, std::min(y + 200, y1), z});
}
// Box [lo, hi] (top + four sides).
void addBox(CollisionMesh& m, vec3 lo, vec3 hi) {
    addFloor(m, lo.x, lo.y, hi.x, hi.y, hi.z);
    addQuad(m, {lo.x, lo.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, hi.y, hi.z}, {lo.x, lo.y, hi.z});
    addQuad(m, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {hi.x, lo.y, hi.z});
    addQuad(m, {lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, lo.y, hi.z}, {lo.x, lo.y, hi.z});
    addQuad(m, {lo.x, hi.y, lo.z}, {hi.x, hi.y, lo.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z});
}

// Clip whose root moves forward (model -Y) by `perKey` units per key, `keys` keys of 100 units (1/30 s).
anim::Clip rootClip(int keys, float perKey) {
    anim::Clip c;
    c.keyCount = uint16_t(keys);
    c.timePerKey = 100;
    c.tracks.resize(1);
    if (perKey != 0)
        for (int i = 0; i < keys; ++i) c.tracks[0][anim::TransY].push_back(-perKey * float(i + 1));
    return c;
}

// Synthetic Lara: one-bone skeleton; locomotion clips with the root-motion speeds measured on lara.drm.
game::AnimLibrary syntheticAnims() {
    game::AnimLibrary a;
    a.skeleton.bones.resize(1);
    a.clips["ng_idle"] = rootClip(50, 0);
    a.clips["ng_walk"] = rootClip(38, 11.6f);   // ~348 units/s
    a.clips["ng_run"] = rootClip(20, 42.47f);   // ~1274 units/s
    a.clips["ledgeb_idle"] = rootClip(15, 0);
    anim::Clip shimmy = rootClip(21, 0);        // ledgeb_right: root moves model -X (Lara's right)
    shimmy.tracks[0][anim::TransX].clear();
    for (int i = 0; i < 21; ++i) shimmy.tracks[0][anim::TransX].push_back(-9.8f * float(i + 1));
    a.clips["ledgeb_right"] = shimmy;
    return a;
}

std::unique_ptr<game::World> makeWorld(CollisionMesh m, vec3 spawn, float yaw = 0) {
    m.build();
    return std::make_unique<game::World>(std::move(m), syntheticAnims(), game::Spawn{spawn, yaw});
}

Actions forward(float f = 1) {
    Actions a;
    a.move.y = f;
    return a;
}

} // namespace

TEST(Game, RootTranslationUnwrapsLoops) {
    const auto c = rootClip(20, 42.5f);
    const double dur = c.durationSeconds();
    // Constant root speed: displacement over any window is speed * window, also across the loop seam.
    for (double t0 : {0.0, 0.1, 0.6, 0.66, 1.3}) {
        const vec3 d = game::rootTranslation(c, t0 + 0.05, true) - game::rootTranslation(c, t0, true);
        EXPECT_NEAR(d.y, -42.5 * 30 * 0.05, 1e-2) << t0;
    }
    EXPECT_NEAR((game::rootTranslation(c, dur, true) - game::rootTranslation(c, 0, true)).y, -42.5f * 20, 1e-3);
    // Non-looping clips clamp.
    EXPECT_EQ(game::rootTranslation(c, dur + 5, false), game::rootTranslation(c, dur, false));
}

TEST(Game, WalkingIntoWallSlides) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 1000}, {400, -2000, 1000});
    auto w = makeWorld(std::move(m), {0, 0, 0});
    Actions a;
    a.move = {0.5f, 0.866f}; // camera faces +X: forward + a bit right (-Y)
    for (int i = 0; i < 180; ++i) w->tick(a);
    const auto& s = w->player().state();
    EXPECT_EQ(s.state, State::Run);
    EXPECT_LE(s.pos.x, 400 - w->player().tuning().radius + 0.01f); // never inside the wall
    EXPECT_GE(s.pos.x, 400 - w->player().tuning().radius - 5);     // pressed against it
    EXPECT_LT(s.pos.y, -500);                                      // slid sideways along it
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
}

TEST(Game, GravityJumpAndLanding) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    auto w = makeWorld(std::move(m), {0, 0, 0});
    const auto& t = w->player().tuning();
    ASSERT_GT(t.gravity, 0);
    ASSERT_GT(t.jumpVz, 0);
    Actions jump;
    jump.jump = true;
    float apex = 0;
    int airTicks = 0;
    for (int i = 0; i < 240; ++i) {
        w->tick(i < 2 ? jump : Actions{});
        const auto& s = w->player().state();
        apex = std::max(apex, s.pos.z);
        airTicks += s.state == State::Jump;
        ASSERT_GE(s.pos.z, -1e-3);
    }
    // Semi-implicit Euler at 60 Hz: apex within 2 % of v^2 / 2g, air time within 3 ticks of 2v/g.
    EXPECT_NEAR(apex, t.jumpVz * t.jumpVz / (2 * t.gravity), 0.02f * apex);
    EXPECT_NEAR(airTicks, 2 * t.jumpVz / t.gravity * 60, 3);
    EXPECT_EQ(w->player().state().state, State::Idle); // landed (no clip: land ends at once)
    EXPECT_NEAR(w->player().state().pos.z, 0, 1e-3);
}

TEST(Game, RunsOffEdgeFallsAndLands) {
    CollisionMesh m;
    addFloor(m, -1000, -1000, 400, 1000, 300); // platform
    addFloor(m, -1000, -1000, 4000, 1000, 0);  // ground below, extends past the platform edge
    auto w = makeWorld(std::move(m), {0, 0, 300});
    bool fell = false;
    for (int i = 0; i < 120; ++i) {
        w->tick(forward());
        fell |= w->player().state().state == State::Fall;
        ASSERT_GE(w->player().state().pos.z, -1e-3);
    }
    EXPECT_TRUE(fell);
    EXPECT_NEAR(w->player().state().pos.z, 0, 1e-3);
    EXPECT_GT(w->player().state().pos.x, 400);
}

TEST(Game, StepsUpAndDownSmallLedgesButNotWalls) {
    CollisionMesh m;
    addFloor(m, -1000, -1000, 4000, 1000, 0);
    addBox(m, {300, -1000, 0}, {700, 1000, 50});      // 50-unit step: walked onto
    addBox(m, {1500, -1000, 0}, {1700, 1000, 200});   // 200-unit block: stops her
    auto w = makeWorld(std::move(m), {0, 0, 0});
    float maxZ = 0;
    bool fell = false;
    for (int i = 0; i < 300; ++i) {
        w->tick(forward(0.4f)); // walk
        const auto& s = w->player().state();
        if (s.pos.x > 350 && s.pos.x < 650) maxZ = std::max(maxZ, s.pos.z);
        fell |= s.state == State::Fall;
    }
    const auto& s = w->player().state();
    EXPECT_NEAR(maxZ, 50, 1e-3);  // stepped up onto the box
    EXPECT_FALSE(fell);           // and stepped down without falling
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
    EXPECT_LE(s.pos.x, 1500 - w->player().tuning().radius + 0.01f);
    EXPECT_GT(s.pos.x, 1300);
}

TEST(Game, LedgeGrabHangShimmyDrop) {
    const game::AnimLibrary anims = syntheticAnims();
    const auto tuning = game::defaultTuning();
    const float apex = tuning.jumpVz * tuning.jumpVz / (2 * tuning.gravity);
    const float ledgeZ = anims.hangHandOffset.z + apex * 0.6f; // reachable on the way down
    CollisionMesh m;
    addFloor(m, -1000, -1000, 300, 1000, 0);
    addBox(m, {300, -1000, 0}, {1000, 1000, ledgeZ});
    m.markup.push_back({1, 7, {{300, -800, ledgeZ}, {300, 0, ledgeZ}, {300, 800, ledgeZ}}});
    const float startX = 300 + anims.hangHandOffset.y - 10; // hands 10 units short of the wall
    auto w = makeWorld(std::move(m), {startX, 0, 0});
    Actions jump;
    jump.jump = true;
    bool grabbed = false;
    for (int i = 0; i < 120 && !grabbed; ++i) {
        w->tick(i < 2 ? jump : Actions{});
        grabbed = w->player().state().state == State::Hang;
    }
    ASSERT_TRUE(grabbed);
    const auto& s = w->player().state();
    const glm::mat4 mm = w->player().modelMatrix(s.pos, s.yaw);
    const vec3 hands = vec3(mm * glm::vec4(anims.hangHandOffset, 1));
    EXPECT_NEAR(hands.z, ledgeZ, 1e-2);
    EXPECT_NEAR(hands.x, 300, 1e-2);
    EXPECT_NEAR(std::cos(s.yaw), 1, 1e-4); // facing the wall (+X)

    // Shimmy right (= -Y when facing +X), crossing the polyline's middle point, stopped at its end.
    Actions right;
    right.move.x = 1;
    for (int i = 0; i < 60; ++i) w->tick(right);
    EXPECT_EQ(w->player().state().state, State::Shimmy);
    EXPECT_NEAR(w->player().state().pos.y, -9.8f * 30, 10);
    for (int i = 0; i < 600; ++i) w->tick(right);
    EXPECT_NEAR(w->player().state().pos.y, -800 - anims.hangHandOffset.x, 1e-2);
    EXPECT_NEAR(w->player().state().pos.z, ledgeZ - anims.hangHandOffset.z, 1e-2);
    // Shimmy left goes back across the joint into the other segment.
    Actions left;
    left.move.x = -1;
    for (int i = 0; i < 180; ++i) w->tick(left);
    EXPECT_GT(w->player().state().pos.y, -800 + 9.8f * 30 * 2.5f);

    Actions drop;
    drop.drop = true;
    w->tick(drop);
    EXPECT_EQ(w->player().state().state, State::Fall);
    for (int i = 0; i < 240; ++i) w->tick({});
    EXPECT_EQ(w->player().state().state, State::Idle);
    EXPECT_NEAR(w->player().state().pos.z, 0, 1e-3);
}

TEST(Game, SimulationIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 forward 1\n0.2 right 0.5\n1.0 jump 1\n1.1 jump 0\n2.0 right -1\n2.5 forward 0\n");
    auto run = [&](double renderHz) {
        CollisionMesh m;
        addFloor(m, -4000, -4000, 4000, 4000, 0);
        addBox(m, {1500, -4000, 0}, {1700, 4000, 300});
        auto w = makeWorld(std::move(m), {0, 0, 0});
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 210) // 3.5 s
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w->ticks() < 210; --n) w->tick(script.at(w->ticks()));
        return w->player().state();
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.pos, o.pos);
        EXPECT_EQ(a.yaw, o.yaw);
        EXPECT_EQ(a.state, o.state);
        EXPECT_EQ(a.clip, o.clip);
    }
    EXPECT_GT(glm::length(glm::vec2(a.pos)), 500); // it actually moved
}

TEST(Game, InputScriptParse) {
    const auto s = game::InputScript::parse("# comment\n0 forward 1\n0.5 jump   # pulse\n0.6 jump 0\n\n1 lookx -2\n");
    EXPECT_EQ(s.at(0).move.y, 1);
    EXPECT_FALSE(s.at(29).jump);
    EXPECT_TRUE(s.at(30).jump);
    EXPECT_FALSE(s.at(36).jump);
    EXPECT_EQ(s.at(60).look.x, -2);
    EXPECT_THROW(game::InputScript::parse("0 fly 1\n"), std::runtime_error);
    EXPECT_THROW(game::InputScript::parse("zero forward\n"), std::runtime_error);
}

// ---- real data (OU_GAME_DIR) ------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
}

TEST(GameReal, LaraClipsAndRootMotion) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    EXPECT_EQ(anims.clips.size(), game::kLaraClipNames.size());
    EXPECT_GE(anims.skeleton.bones.size(), 116u);
    // ng_run: root moves -Y 806.9 units over 19 key intervals, 20 keys per 0.667 s cycle -> ~1274 units/s.
    const auto* run = anims.find("ng_run");
    ASSERT_TRUE(run);
    const double d = run->durationSeconds();
    const vec3 cyc = game::rootTranslation(*run, d, true) - game::rootTranslation(*run, 0, true);
    EXPECT_NEAR(cyc.y / d, -1274, 5);
    EXPECT_NEAR(cyc.x, 0, 1e-3);
    // Hang pose: hands ~496 units up and ~142 in front of the root.
    EXPECT_NEAR(anims.hangHandOffset.z, 495.6f, 1);
    EXPECT_NEAR(anims.hangHandOffset.y, -141.7f, 1);
}

TEST(GameReal, PoolRunReplayStaysOnFloor) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto bytes = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(bytes);
    const auto unit = cdc::parseDrm(std::move(*bytes));
    const auto spawn = game::findSpawn(bf, unit);
    ASSERT_TRUE(spawn);
    EXPECT_TRUE(spawn->fromIntro);
    auto anims = game::loadLaraAnims(bf);
    // Reference: the same replay on an unobstructed flat floor gives the expected distance.
    double expected = 0;
    {
        CollisionMesh flat;
        addFloor(flat, -20000, -20000, 20000, 20000, 0);
        flat.build();
        game::World ref(std::move(flat), anims, {{0, 0, 0}, 0});
        for (int i = 0; i < 90; ++i) ref.tick(forward());
        expected = glm::length(glm::vec2(ref.player().state().pos));
    }
    EXPECT_GT(expected, 1000); // ng_run_start + ng_run root motion over 1.5 s
    game::World w(collision::loadLevelCollision(unit), std::move(anims), *spawn);
    const vec3 start = w.player().state().pos;
    for (int i = 0; i < 90; ++i) {
        w.tick(forward());
        const auto& s = w.player().state();
        const auto floor = w.collision().groundProbe(s.pos + vec3(0, 0, 50), 2000);
        ASSERT_TRUE(floor) << "tick " << i;
        ASSERT_GE(s.pos.z, floor->point.z - 1) << "tick " << i;
    }
    const float moved = glm::length(glm::vec2(w.player().state().pos - start));
    EXPECT_GT(moved, 0.5 * expected);
    EXPECT_LT(moved, 1.05 * expected);
}

// l4_pool ledge markup id 1253 (rock ledge at z 3562, lower floor 523 units below): a standing jump
// from the lower floor grabs it, a climb-up ends standing on the rock top.
TEST(GameReal, PoolLedgeGrabAndClimb) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto bytes = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(bytes);
    game::World w(collision::loadLevelCollision(cdc::parseDrm(std::move(*bytes))), game::loadLaraAnims(bf),
                  {{-4291, 4225.6f, 3100}, glm::radians(-157.9f)});
    EXPECT_NEAR(w.player().state().pos.z, 3039.5f, 1);
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 90; ++i) w.tick(i == 5 ? jump : Actions{});
    const auto& s = w.player().state();
    ASSERT_EQ(s.state, State::Hang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1253);
    const vec3 hands = vec3(w.player().modelMatrix(s.pos, s.yaw) * glm::vec4(w.anims().hangHandOffset, 1));
    EXPECT_NEAR(hands.z, 3562, 1);
    for (int i = 0; i < 90; ++i) w.tick(i == 5 ? jump : Actions{});
    EXPECT_EQ(w.player().state().state, State::Idle);
    EXPECT_NEAR(w.player().state().pos.z, 3571, 15); // rock top
}

// Animation pops: per-tick change of the bones' model-space orientations over a scripted run / walk / stop /
// jump replay, rendering the raw player clip (no blending: the pre-AnimController path) vs World::pose
// (crossfades + phase sync). At each clip change: raw = the jump on the switch tick; blended = the largest
// jump over the fade window. Bones: every skinned body bone, and the major ones (pelvis, spine, head, legs,
// arms; fingers and face excluded). Numbers are reported in docs/formats/ANIMATION.md.
TEST(GameReal, TransitionsReducePops) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    CollisionMesh flat;
    addFloor(flat, -20000, -20000, 20000, 20000, 0);
    flat.build();
    game::World w(std::move(flat), game::loadLaraAnims(bf), {{0, 0, 0}, 0});
    const auto& sk = w.anims().skeleton;
    ASSERT_GT(sk.bones.size(), 152u);
    // Bones that move vertices (some helper bones flip by ~180 degrees between clips but deform nothing).
    std::vector<bool> skinned(sk.bones.size()), major(sk.bones.size());
    for (const auto& m : formats::loadDrmScene(bf, "pc-w\\lara.drm").meshes)
        for (const auto& v : m.skin)
            for (int k = 0; k < 4; ++k)
                if (v.weight[k] && v.bone[k] < skinned.size()) skinned[v.bone[k]] = true;
    for (size_t b = 1; b < 116; ++b) major[b] = skinned[b] && (b <= 18 || (b >= 52 && b <= 55) || (b >= 77 && b <= 80));
    auto rot = [](const glm::mat4& m) { return glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])), glm::normalize(glm::vec3(m[2]))); };
    auto angle = [&](const glm::mat4& a, const glm::mat4& b) {
        const glm::mat3 r = glm::transpose(rot(a)) * rot(b);
        return glm::degrees(std::acos(std::clamp((r[0][0] + r[1][1] + r[2][2] - 1) * 0.5f, -1.0f, 1.0f)));
    };
    auto jump = [&](const std::vector<glm::mat4>& a, const std::vector<glm::mat4>& b, const std::vector<bool>& set) {
        float m = 0;
        for (size_t i = 1; i < 116; ++i)
            if (set[i]) m = std::max(m, angle(a[i], b[i]));
        return m;
    };
    auto raw = [&] {
        const auto& s = w.player().state();
        auto local = anim::sample(*w.anims().find(s.clip), sk, s.clipTime, s.loop);
        local[0] = anim::bindPose(sk)[0];
        return anim::worldMatrices(sk, local);
    };
    struct Transition { std::string name; int end; float raw[2], blended[2]; };
    std::vector<Transition> tr;
    std::vector<glm::mat4> prevRaw = raw(), prevPose = w.pose(1);
    const auto& hair = w.anims().hair;
    ASSERT_EQ(hair.particles.size(), 37u);
    float hairStrain = 0; // largest relative length error of a hair constraint
    for (int t = 0; t < 560; ++t) {
        Actions a;
        if (t < 90 || (t >= 140 && t < 170) || (t >= 320 && t < 460)) a.move.y = 1; // run, walk, run
        a.walk = (t >= 140 && t < 170) || (t >= 50 && t < 70);                       // + run -> walk -> run
        a.jump = t == 200 || t == 420;                                                 // standing, running jump
        const std::string before = w.player().state().clip;
        w.tick(a);
        const auto& s = w.player().state();
        const auto r = raw(), p = w.pose(1);
        if (s.clip != before)
            tr.push_back({before + " -> " + s.clip, t + int(std::ceil(game::clipBlend(before, s.clip).blendSeconds * 60)),
                          {jump(prevRaw, r, skinned), jump(prevRaw, r, major)}, {0, 0}});
        if (!tr.empty() && t <= tr.back().end) {
            tr.back().blended[0] = std::max(tr.back().blended[0], jump(prevPose, p, skinned));
            tr.back().blended[1] = std::max(tr.back().blended[1], jump(prevPose, p, major));
        }
        for (size_t b = 116; b < p.size(); ++b)
            ASSERT_TRUE(std::isfinite(p[b][3].x) && std::isfinite(p[b][3].y) && std::isfinite(p[b][3].z)) << t;
        for (const auto& l : hair.links) {
            const float d = glm::distance(glm::vec3(p[hair.particles[l.a].bone][3]), glm::vec3(p[hair.particles[l.b].bone][3]));
            hairStrain = std::max(hairStrain, std::abs(d - l.rest) / l.rest);
        }
        prevRaw = r;
        prevPose = p;
    }
    float worst[2][2] = {};
    for (const auto& x : tr) {
        std::printf("%-58s raw %6.1f / %5.1f deg   blended %6.1f / %5.1f deg (skinned / major)\n", x.name.c_str(), x.raw[0], x.raw[1],
                    x.blended[0], x.blended[1]);
        for (int k = 0; k < 2; ++k) worst[0][k] = std::max(worst[0][k], x.raw[k]), worst[1][k] = std::max(worst[1][k], x.blended[k]);
    }
    std::printf("worst transition: raw %.1f / %.1f deg, blended %.1f / %.1f deg\n", worst[0][0], worst[0][1], worst[1][0], worst[1][1]);
    EXPECT_GE(tr.size(), 12u);
    EXPECT_LT(worst[1][1], 0.5f * worst[0][1]);
    EXPECT_LT(worst[1][0], worst[0][0]);
    std::printf("hair: largest constraint strain %.1f %%\n", 100 * hairStrain);
    EXPECT_LT(hairStrain, 0.35f);
}
