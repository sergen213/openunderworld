#include "game/Game.hpp"
#include "game/Weather.hpp"

#include "cdc/Cdc.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"
#include "script/Vm.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

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
    anim::Clip climbOut = rootClip(47, 0);     // swim_climbout: root ends 308.7 forward, 172.8 up
    for (int i = 0; i < 47; ++i) {
        climbOut.tracks[0][anim::TransY].push_back(-308.7f * float(i) / 46);
        climbOut.tracks[0][anim::TransZ].push_back(172.8f * float(i) / 46);
    }
    a.clips["swim_climbout"] = climbOut;
    a.clips["bbeam_walk"] = rootClip(53, 4.67f);    // ~140 units/s
    a.clips["Crouch_Front"] = rootClip(31, 11.0f);  // ~330 units/s
    a.clips["ng_roll_front"] = rootClip(25, 45.5f); // ~1365 units/s
    anim::Clip drop = rootClip(31, 0);              // drop_to_ledgeb: forward 218.8, down 502.2, half turn
    for (int i = 0; i < 31; ++i) {
        const float u = float(i) / 30;
        drop.tracks[0][anim::TransY].push_back(-218.8f * u);
        drop.tracks[0][anim::TransZ].push_back(-502.2f * u);
        drop.tracks[0][anim::RotZ].push_back(3.14159f * u);
    }
    a.clips["drop_to_ledgeb"] = drop;
    return a;
}

// Pool: walkways (z 0) at x < 0 and x > 2000, a 3000-deep pit between, water surface at z -150.
CollisionMesh poolMesh() {
    CollisionMesh m;
    addBox(m, {-3000, -3000, -3000}, {0, 3000, 0});
    addBox(m, {2000, -3000, -3000}, {5000, 3000, 0});
    addFloor(m, 0, -3000, 2000, 3000, -3000);
    m.markup.push_back({10, 1, {{-10000, -10000, -100150}, {10000, 10000, -150}}});
    return m;
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

TEST(Game, ControlBindings) {
    // Every default input name is known to SDL and round-trips through its canonical name.
    const config::InputConfig defaults;
    for (const auto& [action, names] : defaults.bindings)
        for (const auto& n : names) {
            const auto r = platform::parseInput(n);
            ASSERT_TRUE(r) << action << ": " << n;
            EXPECT_EQ(platform::parseInput(platform::inputName(*r)), r) << n;
        }
    EXPECT_EQ(platform::parseInput("left shift"), platform::parseInput("Left Shift")); // case-insensitive
    EXPECT_FALSE(platform::parseInput("Pad leftx")); // sticks are not bindable
    EXPECT_FALSE(platform::parseInput("Mouse Wheel"));

    bool keys[512] = {};
    const auto press = [&](const char* name, bool down) { keys[platform::parseInput(name)->code] = down; };
    const game::Controls standard;
    press("Space", true);
    EXPECT_TRUE(game::readDevices(standard, keys, nullptr).jump);
    press("Space", false);
    EXPECT_TRUE(game::readDevices(standard, keys, nullptr, 1u << 2).aim); // SDL_BUTTON_RMASK
    press("W", true);
    EXPECT_EQ(game::readDevices(standard, keys, nullptr).move.y, 1);
    press("Down", true); // forward and back cancel
    EXPECT_EQ(game::readDevices(standard, keys, nullptr).move.y, 0);
    press("W", false), press("Down", false);
    press("F5", true); // each action maps to its own Actions field (enum order == bindableActions())
    auto a = game::readDevices(standard, keys, nullptr);
    EXPECT_TRUE(a.save);
    EXPECT_FALSE(a.load || a.grapple || a.heal || a.jump);
    press("F5", false);

    config::InputConfig custom;
    custom.bindings["jump"] = {"J"};
    custom.bindings["grapple"] = {"Space"};
    const game::Controls rebound(custom);
    press("Space", true);
    a = game::readDevices(rebound, keys, nullptr);
    EXPECT_FALSE(a.jump);
    EXPECT_TRUE(a.grapple);
    custom.bindings["heal"] = {"Hyperspace"};
    EXPECT_THROW(game::Controls{custom}, std::runtime_error);
}

// ---- real data (OU_GAME_DIR) ------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
}

TEST(GameReal, LaraClipsAndRootMotion) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    size_t weaponClips = 0;
    for (int i = 0; i < game::kWeaponCount; ++i) weaponClips += game::weaponClipNames(game::Weapon(i)).size();
    EXPECT_EQ(anims.clips.size(), game::kLaraClipNames.size() + weaponClips + game::kBikeClipNames.size());
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
    float lastClimbZ = 0, snap = -1;
    for (int i = 0; i < 90; ++i) {
        w.tick(i == 5 ? jump : Actions{});
        if (s.state == State::ClimbUp) lastClimbZ = s.pos.z;
        else if (snap < 0 && lastClimbZ != 0) snap = std::abs(s.pos.z - lastClimbZ);
    }
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 3571, 1); // rock top
    // The climb starts where ledgeb_climbup_fast expects the ledge, so its root motion ends on the ledge
    // height; the remaining snap is the rock top (3571) sitting 9 above the ledge markup (3562). Played from
    // the hang root the clip ended ~150 above the ledge.
    EXPECT_LT(snap, 10);
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

// ---- swimming / turning (synthetic) ----------------------------------------------------------

// Run off the west walkway into the pool, splash, tread at the surface, swim across, climb out east.
TEST(Game, SwimEnterCrossClimbOut) {
    auto w = makeWorld(poolMesh(), {-400, 0, 0});
    const auto& s = w->player().state();
    bool splashed = false, swam = false;
    for (int i = 0; i < 60 * 12 && s.state != State::SwimClimbOut; ++i) {
        Actions a = forward();
        a.jump = s.state == State::SwimSurface && s.pos.x > 1500 && i % 20 == 0; // presses at the far wall
        w->tick(a);
        splashed |= s.state == State::SwimSplash;
        if (s.state == State::SwimTread || s.state == State::SwimSurface) {
            swam |= s.state == State::SwimSurface;
            ASSERT_NEAR(s.pos.z, -150, 1e-3) << i; // treading / surface swimming: root on the water surface
            ASSERT_LT(s.pos.x, 2000 - w->player().tuning().swimRadius + 0.01f) << i;
        }
        ASSERT_GE(s.pos.z, -3000) << i;
    }
    EXPECT_TRUE(splashed);
    EXPECT_TRUE(swam);
    ASSERT_EQ(s.state, State::SwimClimbOut);
    for (int i = 0; i < 120; ++i) w->tick({});
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_EQ(s.water, -1);
    EXPECT_NEAR(s.pos.z, 0, 1e-3); // the clip's rise ends exactly on the walkway
    EXPECT_GT(s.pos.x, 2000 + 50);
}

TEST(Game, DiveSwimDownAndSurface) {
    auto w = makeWorld(poolMesh(), {-200, 0, 0});
    const auto& s = w->player().state();
    for (int i = 0; i < 240 && s.state != State::SwimTread && s.state != State::SwimSurface; ++i) w->tick(forward());
    for (int i = 0; i < 60; ++i) w->tick({});
    ASSERT_EQ(s.state, State::SwimTread);
    Actions drop;
    drop.drop = true;
    w->tick(drop);
    EXPECT_EQ(s.state, State::SwimDive);
    for (int i = 0; i < 60; ++i) w->tick(drop); // held: swim down
    EXPECT_EQ(s.state, State::SwimUnder);
    EXPECT_LT(s.pitch, -1.2f);
    EXPECT_LT(s.pos.z, -150 - 300);
    const float z = s.pos.z;
    for (int i = 0; i < 30; ++i) w->tick({}); // no buoyancy evidence: she stays put
    EXPECT_NEAR(s.pos.z, z, 1e-3);
    EXPECT_EQ(s.clip, "swim_submerged_idle");
    Actions up;
    up.jump = true;
    int ticks = 0;
    for (; ticks < 300 && s.state != State::SwimTread; ++ticks) w->tick(up);
    EXPECT_EQ(s.state, State::SwimTread);
    EXPECT_NEAR(s.pos.z, -150, 1e-3);
    EXPECT_NEAR(ticks / 60.0, (-150 - z) / w->anims().swimSpeed, 0.4); // rise at swim speed (+ pitch-up time)
}

TEST(Game, SwimSimulationIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 forward 1\n1.5 forward 0\n2 drop 1\n2.6 drop 0\n2.7 right 1\n3.2 jump 1\n");
    auto run = [&](double renderHz) {
        auto w = makeWorld(poolMesh(), {-300, 0, 0});
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 240)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w->ticks() < 240; --n) w->tick(script.at(w->ticks()));
        return w->player().state();
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.pos, o.pos);
        EXPECT_EQ(a.state, o.state);
        EXPECT_EQ(a.pitch, o.pitch);
    }
    EXPECT_NE(a.water, -1);
}

// Turning in place follows the turn clip's root rotation: 1.57 rad over 3 of 4 keys (ng_turn_center).
TEST(Game, TurnInPlaceUsesClipRootRotation) {
    game::AnimLibrary anims = syntheticAnims();
    anim::Clip turn;
    turn.keyCount = 4;
    turn.timePerKey = 200;
    turn.tracks.resize(1);
    turn.tracks[0][anim::RotZ] = {0, -0.52f, -1.05f, -1.57f};
    anims.clips["ng_turn_center"] = turn;
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.build();
    game::World w(std::move(m), std::move(anims), {{0, 0, 0}, 0});
    int ticks = 0;
    for (; ticks < 120; ++ticks) {
        w.tick(forward(-1)); // camera faces +X: turn to -X
        if (w.player().state().state != State::Turn && ticks > 0) break;
    }
    // Looping clip: 2.09 rad per 0.267 s cycle = 7.85 rad/s; pi - 0.2 rad -> 0.375 s (fixed turnRate 6: 0.49 s).
    EXPECT_NEAR(ticks / 60.0, (3.14159 - 0.2) / 7.85, 3.0 / 60);
}

// ---- controller bugs, traversal (synthetic) ----------------------------------------------------
namespace {
constexpr float kPi = 3.14159265f;
vec3 handsOf(const game::World& w, vec3 offset) {
    const auto& s = w.player().state();
    return vec3(w.player().modelMatrix(s.pos, s.yaw) * glm::vec4(offset, 1));
}
} // namespace

// Walking slowly off an edge: the capsule rests on the edge's corner while the root is past it. That used
// to count as landing (Land), the next ground probe found nothing (Fall), and so on. Now: one fall.
TEST(Game, WalkingSlowlyOffEdgeFallsOnce) {
    CollisionMesh m;
    addFloor(m, -1000, -1000, 400, 1000, 300); // platform
    addFloor(m, -1000, -1000, 4000, 1000, 0);  // ground below
    auto w = makeWorld(std::move(m), {0, 0, 300});
    const auto& s = w->player().state();
    int falls = 0, lands = 0;
    State prev = s.state;
    for (int i = 0; i < 300; ++i) {
        w->tick(forward(0.3f)); // walk
        if (s.state != prev) falls += s.state == State::Fall, lands += s.state == State::Land;
        if (s.state == State::Land) { ASSERT_NEAR(s.pos.z, 0, 1e-3) << i; }
        prev = s.state;
    }
    EXPECT_EQ(falls, 1);
    EXPECT_EQ(lands, 1);
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
    EXPECT_GT(s.pos.x, 500);
}

// A 20-unit strip rising 26.7 across the run (normal z 0.6, as l4_ragnarok_room's floor strips at tick 127
// of tests/replays/pool_to_ragnarok.txt) is a bump, not a slope: no Fall/Run flicker, no slide.
TEST(Game, RunsOverSteepSliverWithoutFalling) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 4000, 2000, 0);
    addQuad(m, {300, -10, 0}, {1500, -10, 0}, {1500, 10, 26.7f}, {300, 10, 26.7f});
    auto w = makeWorld(std::move(m), {0, 0, 0});
    const auto& s = w->player().state();
    bool onStrip = false;
    for (int i = 0; i < 120; ++i) {
        w->tick(forward());
        ASSERT_TRUE(s.state == State::Run || s.state == State::Idle) << i << " " << game::stateName(s.state);
        onStrip |= s.pos.z > 10;
    }
    EXPECT_TRUE(onStrip);
    EXPECT_GT(s.pos.x, 1500);
}

// Under water the model is pitched by the swim pitch (the level swim_sub stroke plays at every pitch).
TEST(Game, UnderwaterModelFollowsSwimPitch) {
    auto w = makeWorld(poolMesh(), {-200, 0, 0});
    const auto& s = w->player().state();
    for (int i = 0; i < 240 && s.state != State::SwimTread && s.state != State::SwimSurface; ++i) w->tick(forward());
    for (int i = 0; i < 60; ++i) w->tick({});
    Actions drop;
    drop.drop = true;
    for (int i = 0; i < 61; ++i) w->tick(drop);
    ASSERT_EQ(s.state, State::SwimUnder);
    ASSERT_LT(s.pitch, -1.2f);
    EXPECT_EQ(s.clip, "swim_sub");
    const vec3 head = vec3(w->player().modelMatrix(s.pos, s.yaw, s.pitch) * glm::vec4(0, -1, 0, 0)); // model forward
    EXPECT_NEAR(head.z, std::sin(s.pitch), 1e-5);
    EXPECT_LT(head.z, -0.9f);
    EXPECT_NEAR(glm::length(glm::vec2(head) - glm::vec2(std::cos(s.yaw), std::sin(s.yaw)) * std::cos(s.pitch)), 0, 1e-5);
    Actions up;
    up.jump = true;
    w->tick(up); // pitch turning up: the render state blends it
    EXPECT_NEAR(w->interpolated(0.5).pitch, 0.5f * (w->previous().pitch + s.pitch), 1e-6);
    for (int i = 0; i < 600 && s.state != State::SwimTread; ++i) w->tick(up);
    ASSERT_EQ(s.state, State::SwimTread);
    for (int i = 0; i < 60; ++i) w->tick({});
    EXPECT_EQ(s.pitch, 0); // levelled out at the surface
}

// Balance beam (markup 14) over a pit between two platforms: walk on, along, turn round, step off.
TEST(Game, BalanceBeamWalkTurnStepOff) {
    CollisionMesh m;
    addBox(m, {-2000, -1000, -2000}, {0, 1000, 0});
    addBox(m, {1200, -1000, -2000}, {3000, 1000, 0});
    addFloor(m, -2000, -1000, 3000, 1000, -2000);
    addBox(m, {0, -15, -100}, {1200, 15, 0}); // the beam
    m.markup.push_back({14, 9, {{0, 0, 0}, {1200, 0, 0}}});
    auto w = makeWorld(std::move(m), {-300, 0, 0});
    const auto& s = w->player().state();
    for (int i = 0; i < 240 && s.state != State::Beam; ++i) w->tick(forward(0.3f));
    ASSERT_EQ(s.state, State::Beam);
    EXPECT_EQ(s.ledge, 0);
    const float x0 = s.pos.x;
    for (int i = 0; i < 180; ++i) {
        w->tick(forward(0.3f));
        ASSERT_EQ(s.state, State::Beam) << i;
        ASSERT_EQ(s.pos.y, 0);
        ASSERT_EQ(s.pos.z, 0);
    }
    EXPECT_NEAR(s.pos.x - x0, 4.67f * 30 * 3, 10); // bbeam_walk root speed for 3 s
    const float x1 = s.pos.x;
    for (int i = 0; i < 60; ++i) w->tick(forward(-0.3f)); // stick back: turn round, walk back
    EXPECT_EQ(s.ledgeSide, -1);
    EXPECT_NEAR(std::cos(s.yaw), -1, 1e-4);
    EXPECT_LT(s.pos.x, x1);
    for (int i = 0; i < 1200 && s.state == State::Beam; ++i) w->tick(forward(0.3f));
    EXPECT_EQ(s.state, State::Walk); // stepped off onto the far platform
    EXPECT_EQ(s.ledge, -1);
    for (int i = 0; i < 60; ++i) w->tick(forward(0.3f));
    EXPECT_GT(s.pos.x, 1250);
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
}

// Falling through a beam's line (no collision needed) lands on it; fallHeight reports the drop.
TEST(Game, FallingOntoBeamCatchesIt) {
    CollisionMesh m;
    addFloor(m, -2000, -1000, 3000, 1000, -2000);
    m.markup.push_back({14, 9, {{0, 0, 0}, {1200, 0, 0}}});
    auto w = makeWorld(std::move(m), {600, 0, 400}); // nothing within the spawn probe: starts in the air
    const auto& s = w->player().state();
    float caught = -1;
    for (int i = 0; i < 120 && s.state != State::Beam; ++i) {
        w->tick({});
        if (s.state == State::Beam) caught = s.fallHeight;
    }
    ASSERT_EQ(s.state, State::Beam);
    EXPECT_EQ(s.clip, "bbeam_jumpland");
    EXPECT_EQ(s.pos, vec3(600, 0, 0));
    EXPECT_NEAR(caught, 400, 1e-3);
}

// Jump assist: a forward standing jump whose natural landing (446 units out) is within assistRadius of a
// jump-landing polyline (type 2) lands on it; without the assist it lands at the natural distance.
TEST(Game, AssistedJumpLandsOnJumpLandingMarkup) {
    auto run = [](float assistRadius) {
        CollisionMesh m;
        addFloor(m, -2000, -2000, 4000, 2000, 0);
        m.markup.push_back({2, 5, {{600, -500, 0}, {600, 500, 0}}});
        m.build();
        auto tuning = game::defaultTuning();
        tuning.assistRadius = assistRadius;
        game::World w(std::move(m), syntheticAnims(), {{0, 0, 0}, 0}, tuning);
        Actions go = forward();
        go.jump = true;
        w.tick(go);
        w.tick(forward()); // take-off (no synthetic take-off clip) with the stick held: forward jump
        for (int i = 0; i < 120 && w.player().state().state == State::Jump; ++i) w.tick({});
        EXPECT_EQ(w.player().state().state, State::Land);
        return w.player().state().pos.x;
    };
    EXPECT_NEAR(run(300), 600, 2);
    const auto t = game::defaultTuning();
    EXPECT_NEAR(run(0), t.standJumpSpeed * 2 * t.jumpVz / t.gravity, 15);
}

// Drop (crouch button) standing at an edge with a ledge: drop_to_ledgeb (synthetic: forward 218.8, down
// 502.2, half turn) is placed so its root ends on the hang pose; no jump at the end.
TEST(Game, DropToHangFromEdge) {
    CollisionMesh m;
    addBox(m, {-2000, -1000, -3000}, {0, 1000, 0});
    addFloor(m, -2000, -1000, 3000, 1000, -3000);
    m.markup.push_back({1, 3, {{0, -800, 0}, {0, 800, 0}}});
    auto w = makeWorld(std::move(m), {-100, 50, 0}, kPi); // back to the drop
    const auto& s = w->player().state();
    Actions drop;
    drop.drop = true;
    w->tick(drop);
    ASSERT_EQ(s.state, State::DropToHang);
    EXPECT_GT(std::cos(s.yaw), 0.9f); // the clip starts facing the drop (+X) and turns round
    float maxStep = 0;
    for (int i = 0; i < 120 && s.state == State::DropToHang; ++i) {
        const vec3 p = s.pos;
        w->tick({});
        maxStep = std::max(maxStep, glm::distance(p, s.pos));
    }
    ASSERT_EQ(s.state, State::Hang);
    EXPECT_LT(maxStep, 25); // root motion only: no snap at the end
    const vec3 hands = handsOf(*w, w->anims().hangHandOffset);
    EXPECT_NEAR(hands.x, 0, 1e-2);
    EXPECT_NEAR(hands.y, 50, 1e-2);
    EXPECT_NEAR(hands.z, 0, 1e-2);
    EXPECT_NEAR(std::cos(s.yaw), -1, 1e-4); // facing the wall
}

// Targeted jumps from a hang: up to the ledge above, right to the next one, back across to the far wall.
TEST(Game, HangJumpsUpRightAndBack) {
    CollisionMesh m;
    addBox(m, {0, -3000, -100}, {1000, 3000, 3000});     // wall, face x = 0
    addBox(m, {-1700, -3000, -100}, {-700, 3000, 3000}); // wall behind, face x = -700
    addFloor(m, -700, -3000, 0, 3000, -100);
    m.markup.push_back({1, 1, {{0, 300, 500}, {0, -300, 500}}});
    m.markup.push_back({1, 2, {{0, 300, 900}, {0, -300, 900}}});      // 400 above
    m.markup.push_back({1, 3, {{0, -600, 900}, {0, -1100, 900}}});    // right of it (facing +X, right = -Y)
    m.markup.push_back({1, 4, {{-700, -1100, 700}, {-700, -500, 700}}}); // behind, on the far wall
    auto w = makeWorld(std::move(m), {-300, 0, -100});
    game::PlayerState h;
    h.state = State::Hang, h.ledge = 0, h.ledgeT = 300, h.ledgeSide = -1, h.clip = "ledgeb_idle";
    w->restore(h, 0, -0.25f);
    w->tick({});
    const auto& s = w->player().state();
    auto jump = [&](float right, float fwd, int ledge, vec3 at) {
        Actions a;
        a.jump = true, a.move = {right, fwd};
        w->tick(a);
        ASSERT_EQ(s.state, State::HangJump);
        EXPECT_EQ(s.ledge, ledge);
        float maxStep = 0;
        for (int i = 0; i < 120 && s.state == State::HangJump; ++i) {
            const vec3 p = s.pos;
            w->tick({});
            maxStep = std::max(maxStep, glm::distance(p, s.pos));
        }
        ASSERT_EQ(s.state, State::Hang);
        EXPECT_LT(maxStep, 50); // ballistic flight ends on the hang pose: no snap
        const vec3 hands = handsOf(*w, w->anims().hangHandOffset);
        EXPECT_NEAR(glm::distance(hands, at), 0, 1e-2) << hands.x << " " << hands.y << " " << hands.z;
        w->tick({});
    };
    jump(0, 1, 1, {0, 0, 900});
    jump(1, 0, 2, {0, -600, 900});
    jump(0, -1, 3, {-700, -600, 700});
    EXPECT_NEAR(std::cos(s.yaw), -1, 1e-4); // turned round to face the far wall
}

// Horizontal poles (markup 4): grab from a standing jump, swing, fling to the next pole, drop.
namespace {
CollisionMesh poleMesh() {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 4000, 2000, 0);
    m.markup.push_back({4, 1, {{0, -500, 800}, {0, 500, 800}}});
    m.markup.push_back({4, 2, {{1264, -500, 800}, {1264, 500, 800}}});
    return m;
}
} // namespace
TEST(Game, PoleGrabSwingFlingDrop) {
    // Pole hands sit 17 behind the root: stand 17 in front of the pole's line.
    auto w = makeWorld(poleMesh(), {17, 0, 0});
    const auto& s = w->player().state();
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 120 && s.state != State::PoleHang; ++i) w->tick(i < 2 ? jump : Actions{});
    ASSERT_EQ(s.state, State::PoleHang);
    EXPECT_EQ(s.ledge, 0);
    vec3 hands = handsOf(*w, w->anims().poleHandOffset);
    EXPECT_NEAR(hands.x, 0, 1e-2);
    EXPECT_NEAR(hands.z, 800, 1e-2);
    for (int i = 0; i < 30; ++i) w->tick(forward());
    EXPECT_EQ(s.clip, "HPole_swing");
    Actions fling = forward();
    fling.jump = true;
    w->tick(fling);
    ASSERT_EQ(s.state, State::HangJump);
    for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w->tick({});
    ASSERT_EQ(s.state, State::PoleHang);
    EXPECT_EQ(s.ledge, 1);
    hands = handsOf(*w, w->anims().poleHandOffset);
    EXPECT_NEAR(hands.x, 1264, 1e-2);
    EXPECT_NEAR(hands.z, 800, 1e-2);
    Actions drop;
    drop.drop = true;
    w->tick(drop);
    EXPECT_EQ(s.state, State::Fall);
    for (int i = 0; i < 120; ++i) w->tick({});
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
}

TEST(Game, TraversalIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 jump 1\n0.05 jump 0\n1.2 forward 1\n1.8 jump 1\n1.85 jump 0\n2.5 forward 0\n"
                                                 "3.0 drop 1\n3.1 drop 0\n");
    auto run = [&](double renderHz) {
        auto w = makeWorld(poleMesh(), {17, 0, 0});
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 230)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w->ticks() < 230; --n) w->tick(script.at(w->ticks()));
        return w->player().state();
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.pos, o.pos);
        EXPECT_EQ(a.state, o.state);
    }
    EXPECT_GT(a.pos.x, 1000); // flung to the second pole, then dropped
}

// Crouch (drop held): lower capsule (player data +0x14 = 260) passes under a 350-high slab; letting go
// under it keeps her crouched; past it she stands. Drop pressed while running rolls.
TEST(Game, CrouchUnderSlabAndRoll) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 6000, 2000, 0);
    addBox(m, {500, -2000, 350}, {900, 2000, 450});
    addFloor(m, 500, -2000, 900, 2000, 350); // slab underside
    auto w = makeWorld(std::move(m), {0, 0, 0});
    const auto& s = w->player().state();
    Actions crawl = forward();
    crawl.drop = true;
    for (int i = 0; i < 300 && s.pos.x < 700; ++i) w->tick(crawl);
    ASSERT_EQ(s.state, State::Crouch);
    EXPECT_GT(s.pos.x, 700);
    w->tick(forward()); // let go under the slab: no headroom
    EXPECT_EQ(s.state, State::Crouch);
    for (int i = 0; i < 300 && s.state == State::Crouch; ++i) w->tick(forward());
    EXPECT_NE(s.state, State::Crouch);
    EXPECT_GT(s.pos.x, 900 + w->player().tuning().radius - 1);
    for (int i = 0; i < 60; ++i) w->tick(forward());
    ASSERT_EQ(s.state, State::Run);
    Actions roll = forward();
    roll.drop = true;
    w->tick(roll);
    ASSERT_EQ(s.state, State::Roll);
    const float x = s.pos.x;
    for (int i = 0; i < 49; ++i) w->tick(forward());
    EXPECT_NEAR(s.pos.x - x, 45.5f * 30 * 50 / 60.0f, 50); // ng_roll_front root motion
    w->tick(forward());
    EXPECT_EQ(s.state, State::Run);
}

// A 63-degree slope (normal z 0.45) between a 1000-high top and the ground: walking off the top slides
// down the fall line; the flat ground ends the slide.
TEST(Game, SlidesDownSteepSlope) {
    CollisionMesh m;
    addFloor(m, -1000, -1000, 500, 1000, 1000);
    addQuad(m, {500, -1000, 1000}, {1000, -1000, 0}, {1000, 1000, 0}, {500, 1000, 1000});
    addFloor(m, 1000, -1000, 5000, 1000, 0);
    auto w = makeWorld(std::move(m), {300, 0, 1000});
    const auto& s = w->player().state();
    bool slid = false;
    float maxSpeed = 0;
    for (int i = 0; i < 300; ++i) {
        w->tick(forward(0.3f));
        slid |= s.state == State::Slide;
        if (s.state == State::Slide) maxSpeed = std::max(maxSpeed, glm::length(s.vel));
        ASSERT_GE(s.pos.z, -1e-3);
    }
    EXPECT_TRUE(slid);
    EXPECT_GT(maxSpeed, 1000); // much faster than walking (348)
    EXPECT_NEAR(s.pos.z, 0, 1e-3);
    EXPECT_GT(s.pos.x, 1000);
    EXPECT_NE(s.state, State::Slide);
}

// ---- swimming (real data) ---------------------------------------------------------------------

// l4_pool: the watervolume intro is linked to the goo surface (ca_goo_pool_watersurface, z 2699.3). Lara
// runs off the west walkway (z 2844.4) into the goo, swims east, dives, surfaces, swims back and climbs out.
TEST(GameReal, PoolSwimReplay) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto bytes = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(bytes);
    const auto unit = cdc::parseDrm(std::move(*bytes));
    auto col = collision::loadLevelCollision(unit);
    const auto water = game::findWater(bf, unit);
    ASSERT_EQ(water.size(), 1u);
    EXPECT_EQ(water[0].type, 10);
    EXPECT_EQ(water[0].id, 5608);
    EXPECT_NEAR(water[0].points[1].z, 2699.3f, 0.5f);
    EXPECT_LT(water[0].points[0].x, -5000);
    EXPECT_GT(water[0].points[1].x, 1000);
    col.markup.insert(col.markup.end(), water.begin(), water.end());
    col.build();
    game::World w(std::move(col), game::loadLaraAnims(bf), {{-6000, 0, 2900}, 0});
    const auto& s = w.player().state();
    EXPECT_NEAR(s.pos.z, 2844.4f, 1);
    const float surf = water[0].points[1].z;
    int t = 0;
    auto runUntil = [&](auto&& actions, auto&& done, int maxTicks) {
        for (int i = 0; i < maxTicks && !done(); ++i, ++t) w.tick(actions(i));
        return done();
    };
    auto inState = [&](State x) { return [&s, x] { return s.state == x; }; };
    ASSERT_TRUE(runUntil([](int) { return forward(); }, inState(State::SwimSurface), 360)) << game::stateName(s.state);
    EXPECT_NEAR(s.pos.z, surf, 1e-3);
    const float entryX = s.pos.x;
    runUntil([](int) { return forward(); }, [] { return false; }, 120); // swim east 2 s
    EXPECT_GT(s.pos.x, entryX + 0.8f * 2 * w.anims().swimSpeed);
    Actions drop;
    drop.drop = true;
    runUntil([&](int) { return drop; }, [] { return false; }, 90); // dive, swim down
    EXPECT_EQ(s.state, State::SwimUnder);
    EXPECT_LT(s.pos.z, surf - 600);
    Actions up;
    up.jump = true;
    ASSERT_TRUE(runUntil([&](int) { return up; }, inState(State::SwimTread), 600));
    // Back west; jump presses climb out at the first wall with a top 50..350 above the goo.
    ASSERT_TRUE(runUntil([&](int i) { Actions a = forward(-1); a.jump = i % 20 == 19; return a; }, inState(State::SwimClimbOut), 900))
        << game::stateName(s.state) << " at " << s.pos.x;
    ASSERT_TRUE(runUntil([](int) { return Actions{}; }, inState(State::Idle), 240));
    const auto floor = w.collision().groundProbe(s.pos + vec3(0, 0, 10), 20);
    ASSERT_TRUE(floor);
    EXPECT_NEAR(s.pos.z, floor->point.z, 1e-3);
    EXPECT_GT(s.pos.z, surf + 100);
    std::printf("pool swim replay: %d ticks, climbed out at (%.1f, %.1f, %.1f)\n", t, s.pos.x, s.pos.y, s.pos.z);
}

// ---- traversal (real data) --------------------------------------------------------------------
namespace {
game::World unitWorld(const cdc::BigfileSet& bf, const char* unit, game::Spawn spawn) {
    auto bytes = bf.read(std::string("pc-w\\") + unit + ".drm");
    if (!bytes) throw std::runtime_error(unit);
    return game::World(collision::loadLevelCollision(cdc::parseDrm(std::move(*bytes))), game::loadLaraAnims(bf), spawn);
}
int markupIndex(const game::World& w, uint16_t id) {
    const auto& m = w.collision().markup;
    for (size_t i = 0; i < m.size(); ++i)
        if (m[i].id == id) return int(i);
    return -1;
}
} // namespace

// l5_valaskjalf rail 1562: fall onto its high end, slide the full polyline and leave at the low end.
TEST(GameReal, ValaskjalfRailSlideTraversesAndDetaches) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const vec3 start(7269.15f, -79262.62f, -11757.10f), end(7269.15f, -76525.55f, -13337.35f);
    auto w = unitWorld(bf, "l5_valaskjalf", {start, glm::radians(90.0f)});
    game::PlayerState air;
    air.state = State::Fall, air.pos = start + vec3(0, 0, 200), air.clip = "NG_InAir_Feet_Forward_stage3";
    w.restore(air, 0, -0.25f);
    const auto& s = w.player().state();
    int ticks = 0;
    while (ticks++ < 90 && game::stateName(s.state) != "railslide") w.tick({});
    ASSERT_EQ(game::stateName(s.state), "railslide");
    ASSERT_GE(s.ledge, 0);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1562);
    EXPECT_EQ(s.clip, "slope_slidefront");
    float previousY = s.pos.y, nearestEnd = glm::distance(s.pos, end);
    bool travelled = false;
    for (ticks = 0; ticks < 1800 && game::stateName(s.state) == "railslide"; ++ticks) {
        w.tick({});
        EXPECT_GE(s.pos.y, previousY - 1e-3f) << "rail travel reversed at tick " << ticks;
        previousY = s.pos.y;
        const float u = std::clamp((s.pos.y - start.y) / (end.y - start.y), 0.0f, 1.0f);
        EXPECT_NEAR(s.pos.z, glm::mix(start.z, end.z, u), 1.0f) << "left the rail at tick " << ticks;
        nearestEnd = std::min(nearestEnd, glm::distance(s.pos, end));
        travelled |= s.pos.y > start.y + 500;
    }
    EXPECT_TRUE(travelled);
    EXPECT_LT(nearestEnd, 100.0f);
    EXPECT_NE(game::stateName(s.state), "railslide");
    EXPECT_GE(s.pos.z, end.z - 100.0f) << "fell through the end of the rail";
}

// l4_temple balance beam 1079: (-5590, -8099) -> (-3986, -8099) at z 3061 over a 1734-unit drop (the log it
// lies on reaches no walkable floor: Lara starts on it). Walk, jump ahead along it (the jump assist puts the
// landing back on the line), walk on to the far end (nothing beyond: she stops there).
TEST(GameReal, TempleBalanceBeamWalkAndJump) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l4_temple", {{-5500, -8099, 3100}, 0});
    const auto& s = w.player().state();
    w.tick({});
    ASSERT_EQ(s.state, State::Beam) << game::stateName(s.state);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1079);
    auto onBeam = [&](int i) {
        ASSERT_EQ(s.state, State::Beam) << i;
        ASSERT_NEAR(s.pos.y, -8099, 1);
        ASSERT_NEAR(s.pos.z, 3061, 1);
    };
    const float x0 = s.pos.x;
    for (int i = 0; i < 300; ++i) w.tick(forward(0.3f)), onBeam(i);
    EXPECT_NEAR(s.pos.x - x0, 140 * 5, 60); // bbeam_walk: 501 units per 3.57 s cycle
    Actions jump = forward(0.3f);
    jump.jump = true;
    w.tick(jump);
    EXPECT_EQ(s.state, State::JumpPrep);
    const float x1 = s.pos.x;
    for (int i = 0; i < 120 && s.state != State::Beam; ++i) w.tick(forward(0.3f));
    ASSERT_EQ(s.state, State::Beam);
    EXPECT_EQ(s.clip, "bbeam_jumpland");
    EXPECT_GT(s.pos.x - x1, 300);
    for (int i = 0; i < 900; ++i) w.tick(forward(0.3f)), onBeam(i);
    EXPECT_NEAR(s.pos.x, -3986, 1); // stopped at the far end
}

// Ledge to ledge, from the floor (l4_temple): grab 1187 from a standing jump, jump up 528 to 1162; grab 1158,
// jump back across to 1159 (585 behind, 324 lower), turning round.
TEST(GameReal, TempleLedgeJumpsUpAndBack) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions jump;
    jump.jump = true;
    auto run = [&](game::Spawn spawn, uint16_t first, Actions a, uint16_t second) {
        auto w = unitWorld(bf, "l4_temple", spawn);
        const auto& s = w.player().state();
        for (int i = 0; i < 90; ++i) w.tick(i == 5 ? jump : Actions{});
        ASSERT_EQ(s.state, State::Hang);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, first);
        const float yaw = s.yaw;
        a.jump = true;
        w.tick(a);
        ASSERT_EQ(s.state, State::HangJump);
        for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w.tick({});
        ASSERT_EQ(s.state, State::Hang);
        const auto& m = w.collision().markup[size_t(s.ledge)];
        EXPECT_EQ(m.id, second);
        EXPECT_NEAR(handsOf(w, w.anims().hangHandOffset).z, m.points[size_t(s.ledgeSeg)].z, 1);
        if (a.move.y < 0) EXPECT_NEAR(std::cos(s.yaw - yaw), -1, 1e-3); // turned round
    };
    run({{-8771.2f, -9922.7f, 2600}, 0}, 1187, forward(), 1162);
    run({{-8411.9f, -7173.9f, 1400}, glm::radians(90.0f)}, 1158, forward(-1), 1159);
}

// l4_pool rock ledge 1253 (hands z 3562; the existing grab test's spawn): grab, climb onto the rock top, and
// drop (crouch button) goes back down into the hang on 1253.
TEST(GameReal, PoolDropToHang) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const game::Spawn spawn{{-4291, 4225.6f, 3100}, glm::radians(-157.9f)};
    Actions jump;
    jump.jump = true;
    auto w = unitWorld(bf, "l4_pool", spawn);
    const auto& s = w.player().state();
    for (int i = 0; i < 90; ++i) w.tick(i == 5 ? jump : Actions{});
    for (int i = 0; i < 90; ++i) w.tick(i == 5 ? jump : Actions{}); // climb up
    ASSERT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 3571, 1);
    Actions drop;
    drop.drop = true;
    w.tick(drop);
    ASSERT_EQ(s.state, State::DropToHang);
    for (int i = 0; i < 120 && s.state == State::DropToHang; ++i) w.tick({});
    ASSERT_EQ(s.state, State::Hang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1253);
    EXPECT_NEAR(handsOf(w, w.anims().hangHandOffset).z, 3562, 1);
}

// l4_pool ledges 1250 -> 1251 (z 3864 + 495.6 hands): jump right along the wall (~420 across).
TEST(GameReal, PoolSideJump) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l4_pool", {{-5274, 3618, 3400}, 0});
    const int from = markupIndex(w, 1250);
    ASSERT_GE(from, 0);
    const auto& m = w.collision().markup[size_t(from)];
    game::PlayerState h;
    h.state = State::Hang, h.ledge = from, h.ledgeT = 0.5f * glm::length(glm::vec2(m.points[1] - m.points[0])), h.ledgeSide = 1;
    h.clip = "ledgeb_idle";
    w.restore(h, 0, -0.25f);
    w.tick({});
    const auto& s = w.player().state();
    w.restore(s, s.yaw, -0.25f); // camera behind her: stick right = her right
    w.tick({});
    Actions right;
    right.jump = true, right.move.x = 1;
    w.tick(right);
    ASSERT_EQ(s.state, State::HangJump);
    for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w.tick({});
    ASSERT_EQ(s.state, State::Hang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1251);
}

// l4_traprooms poles 1336 and 1337 (markup 4, z 7133, 1264 apart in y, floor 6358 beside them): a standing
// jump towards 1336 from 345 short grabs it; swing, fling to 1337.
TEST(GameReal, TrapRoomsPoleFling) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l4_traprooms", {{14300, -21200, 6400}, glm::radians(90.0f)});
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    Actions go = forward();
    go.jump = true;
    for (int i = 0; i < 120 && s.state != State::PoleHang; ++i) w.tick(i < 20 ? go : Actions{});
    ASSERT_EQ(s.state, State::PoleHang) << game::stateName(s.state) << " at " << s.pos.y << " z " << s.pos.z;
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1336);
    EXPECT_NEAR(handsOf(w, w.anims().poleHandOffset).z, 7133, 1);
    for (int i = 0; i < 90; ++i) w.tick(forward());
    Actions fling = forward();
    fling.jump = true;
    w.tick(fling);
    ASSERT_EQ(s.state, State::HangJump);
    for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w.tick({});
    ASSERT_EQ(s.state, State::PoleHang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1337);
    EXPECT_NEAR(handsOf(w, w.anims().poleHandOffset).y, -19591, 1);
}

// ---- interactive water (src/game/Water) ----
namespace {
float maxRipple(const game::WaterSim& ws) {
    float m = 0;
    for (float h : ws.heights()) m = std::max(m, std::abs(h));
    return m;
}
} // namespace

TEST(Game, WaterEntrySplashMakesRingsAndDrops) {
    auto w = makeWorld(poolMesh(), {1000, 0, 400});
    size_t maxDrops = 0;
    float peak = 0;
    for (int i = 0; i < 90; ++i) {
        w->tick({});
        maxDrops = std::max(maxDrops, w->water().drops().size());
        peak = std::max(peak, maxRipple(w->water()));
    }
    ASSERT_TRUE(w->water().active());
    ASSERT_EQ(w->player().state().state, State::SwimTread);
    EXPECT_GT(peak, 1.0f) << "the entry must displace the surface";
    EXPECT_GT(maxDrops, 0u) << "the entry must throw droplets";
    for (int i = 0; i < 240; ++i) w->tick({});
    EXPECT_LT(maxRipple(w->water()), peak * 0.6f) << "the rings decay";
}

TEST(Game, WaterExternalSplash) {
    auto w = makeWorld(poolMesh(), {1000, 0, 400});
    for (int i = 0; i < 90; ++i) w->tick({});
    ASSERT_TRUE(w->water().active());
    w->splashWater(w->player().state().pos, 1.5f); // as a shot / falling object impact
    EXPECT_GT(maxRipple(w->water()), 5.0f);
    EXPECT_FALSE(w->water().drops().empty());
}

TEST(Game, WaterWakeFollowsLara) {
    auto w = makeWorld(poolMesh(), {1000, 0, 400});
    for (int i = 0; i < 90; ++i) w->tick({});
    const float ox = w->water().originX(), oy = w->water().originY();
    for (int i = 0; i < 300; ++i) w->tick(forward());
    EXPECT_GT(maxRipple(w->water()), 0.5f) << "surface swimming leaves a wake";
    EXPECT_TRUE(w->water().originX() != ox || w->water().originY() != oy) << "the field follows Lara";
    EXPECT_EQ(std::fmod(w->water().originX(), game::WaterSim::kCell), 0.0f);
}

TEST(Game, WaterAirDrainsRefillsAndDrowns) {
    auto w = makeWorld(poolMesh(), {1000, 0, 400});
    for (int i = 0; i < 90; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::SwimTread);
    EXPECT_NEAR(w->air(), 1.0f, 1e-3);
    Actions dive;
    dive.drop = true;
    for (int i = 0; i < 120; ++i) w->tick(dive);
    ASSERT_TRUE(w->submerged());
    ASSERT_LT(w->air(), 1.0f);
    const float low = w->air();
    Actions up;
    up.jump = true;
    for (int i = 0; i < 300 && w->player().state().state != State::SwimTread; ++i) w->tick(up);
    ASSERT_EQ(w->player().state().state, State::SwimTread);
    EXPECT_GT(w->air(), low) << "surfacing refills the meter";
    for (int i = 0; i < 60 * 50 && w->health().value() >= w->health().max(); ++i) w->tick(dive);
    EXPECT_EQ(w->air(), 0.0f);
    EXPECT_LT(w->health().value(), w->health().max()) << "an empty meter drowns Lara";
}

// ---- free climbing (src/game/Climb) ----
namespace {
// A climbable wall at x = 400 (z 0..1200) with a floor on top, and an unflagged twin for the negative case.
CollisionMesh climbMesh(bool flagged) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 1200}, {400, -2000, 1200});
    addQuad(m, {400, -2000, 1200}, {2000, -2000, 1200}, {2000, 2000, 1200}, {400, 2000, 1200});
    if (flagged)
        for (auto& f : m.faces) f.flags = 0x3F;
    return m;
}
} // namespace

TEST(Game, ClimbAttachClimbUpTopOut) {
    auto w = makeWorld(climbMesh(true), {300, 0, 0});
    w->tick(forward()); // pushing into the rock attaches
    ASSERT_EQ(w->player().state().state, State::Climb);
    for (int i = 0; i < 120; ++i) w->tick({}); // the attach clip finishes
    const float z0 = w->player().state().pos.z;
    for (int i = 0; i < 90; ++i) w->tick(forward()); // into the wall = climb up
    EXPECT_GT(w->player().state().pos.z, z0 + 200.0f);
    EXPECT_NEAR(w->player().state().pos.x, 300.0f, 1.0f) << "pinned at capsule distance from the rock";
    for (int i = 0; i < 1200 && w->player().state().state == State::Climb; ++i) w->tick(forward());
    EXPECT_EQ(w->player().state().state, State::Idle);
    EXPECT_NEAR(w->player().state().pos.z, 1200.0f, 1.0f) << "climbed onto the wall's top";
}

TEST(Game, ClimbJumpOff) {
    auto w = makeWorld(climbMesh(true), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    for (int i = 0; i < 120; ++i) w->tick({});
    Actions jump;
    jump.jump = true;
    w->tick(jump);
    EXPECT_EQ(w->player().state().state, State::Jump);
    EXPECT_LT(w->player().state().vel.x, 0.0f) << "leaps away from the wall";
    EXPECT_GT(w->player().state().vel.z, 0.0f);
}

TEST(Game, ClimbIgnoresUnflaggedWall) {
    auto w = makeWorld(climbMesh(false), {300, 0, 0});
    w->tick(forward());
    EXPECT_NE(w->player().state().state, State::Climb) << "an unflagged wall is not climbable";
}

// ---- grapple (src/game/Grapple) ----
TEST(Game, GrappleSwingAndRelease) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    auto w = makeWorld(std::move(m), {0, 0, 0});
    w->attachGrapple({0, 0, 3000}, 1);
    ASSERT_EQ(w->player().state().state, State::GrappleThrow);
    for (int i = 0; i < 90; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::GrappleSwing);
    const float len = w->player().state().ropeLength;
    EXPECT_GT(len, 2000.0f);
    for (int i = 0; i < 120; ++i) {
        w->tick(forward()); // pumping must never stretch the rope
        const glm::vec3 hand = w->player().state().pos + glm::vec3(0, 0, 400);
        EXPECT_LE(glm::distance(hand, w->player().state().grappleAnchor), len + 1.0f) << "tick " << i;
    }
    Actions jump;
    jump.jump = true;
    w->tick(jump);
    EXPECT_EQ(w->player().state().state, State::Jump);
    EXPECT_EQ(w->player().state().grappleUid, 0);
}

TEST(GameReal, PoolGrappleTargetAndSwing) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), "l4_pool",
                  game::loadLaraAnims(bf), game::Spawn{{4362, -400, 2000}, 0});
    w.restore(w.player().state(), 1.5707963f, 0.7f); // look up at the ceiling anchor
    const auto t = w.grappleTarget();
    ASSERT_TRUE(t) << "the pool's ceiling anchor is selectable";
    EXPECT_EQ(t->uid, 5511);
    w.attachGrapple(t->pos, t->uid);
    for (int i = 0; i < 90; ++i) w.tick({});
    EXPECT_EQ(w.player().state().state, State::GrappleSwing);
    EXPECT_EQ(w.player().state().grappleUid, 5511);
}

TEST(GameReal, MountedGrapplePullsBlockWithoutSwinging) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, "l4_traprooms");
    const auto target = std::find_if(unit.grapples.begin(), unit.grapples.end(), [](const auto& g) { return g.uid == 5884; });
    ASSERT_NE(target, unit.grapples.end());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_traprooms", game::loadLaraAnims(bf), game::Spawn{target->pos + vec3(-1000, 0, 0), 0});
    w.script().setBigfile(&bf);
    w.attachGrapple(target->pos, target->uid, game::GrappleKind::Mounted);
    for (int i = 0; i < 90; ++i) w.tick({});
    EXPECT_NE(w.player().state().state, State::GrappleSwing);
    const auto* block = w.script().find(5884);
    ASSERT_TRUE(block);
    const vec3 initial = block->offset;
    Actions pull;
    pull.interact = true;
    for (int i = 0; i < 60; ++i) w.tick(pull);
    EXPECT_NE(w.player().state().state, State::GrappleSwing);
    EXPECT_GT(glm::length(block->offset - initial), 100.0f);
}

// ---- ladders / pillars / perches (src/game/Traverse) ----
TEST(Game, LadderAttachClimbOffTop) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.markup.push_back({9, 1, {{400, 0, 1200}, {400, 0, 0}, {242, 0, 0}}}); // outward -X, toward Lara
    auto w = makeWorld(std::move(m), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Ladder);
    const float z0 = w->player().state().pos.z;
    for (int i = 0; i < 60; ++i) w->tick(forward());
    EXPECT_GT(w->player().state().pos.z, z0 + 100.0f);
    for (int i = 0; i < 600 && w->player().state().state != State::Idle; ++i) w->tick(forward());
    EXPECT_EQ(w->player().state().state, State::Idle);
    EXPECT_NEAR(w->player().state().pos.z, 1200.0f, 1.0f);
    EXPECT_NEAR(w->player().state().pos.x, 400.0f, 1.0f);
}

TEST(Game, PillarAttach) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.markup.push_back({15, 1, {{400, 0, 1600}, {400, 0, 0}, {242, 0, 0}}});
    auto w = makeWorld(std::move(m), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Pillar);
    Actions side;
    side.move.x = 1.0f; // rotate around the shaft
    const float x0 = w->player().state().pos.x;
    for (int i = 0; i < 30; ++i) w->tick(side);
    EXPECT_NE(w->player().state().pos.x, x0);
}

TEST(Game, PerchAttachAndJumpOff) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.markup.push_back({13, 1, {{400, -50, 500}, {400, 50, 500}}});
    auto w = makeWorld(std::move(m), {350, 0, 400});
    Actions jump;
    jump.jump = true;
    w->tick(jump);
    ASSERT_EQ(w->player().state().state, State::Perch);
    for (int i = 0; i < 30; ++i) w->tick({});
    EXPECT_EQ(w->player().state().state, State::Perch);
    w->tick(jump);
    EXPECT_EQ(w->player().state().state, State::Jump);
}

TEST(Game, RailSlideJumpAndDropDetach) {
    auto attached = [] {
        CollisionMesh m;
        addFloor(m, -500, -500, 500, 500, 0);
        m.markup.push_back({8, 1, {{0, 0, 0}, {0, 2000, -1000}}});
        auto w = makeWorld(std::move(m), {0, 0, 0}, kPi / 2);
        w->tick({});
        return w;
    };
    auto jumping = attached();
    ASSERT_EQ(jumping->player().state().state, State::RailSlide); // deterministic ground attachment
    Actions jump;
    jump.jump = true;
    jumping->tick(jump);
    EXPECT_EQ(jumping->player().state().state, State::Jump);
    EXPECT_EQ(jumping->player().state().ledge, -1);
    EXPECT_GT(jumping->player().state().vel.z, 0);

    auto dropping = attached();
    ASSERT_EQ(dropping->player().state().state, State::RailSlide);
    Actions drop;
    drop.drop = true;
    dropping->tick(drop);
    EXPECT_EQ(dropping->player().state().state, State::Fall);
    EXPECT_EQ(dropping->player().state().ledge, -1);
}

TEST(Game, RailSlideAirAttachAndEndDetach) {
    CollisionMesh m; // deliberately no floor: the end must enter a normal fall, not reattach or stick
    m.markup.push_back({8, 1, {{0, 0, 500}, {0, 1000, 0}}});
    auto w = makeWorld(std::move(m), {0, 0, 700}, kPi / 2);
    const auto& s = w->player().state();
    for (int i = 0; i < 90 && s.state != State::RailSlide; ++i) w->tick({});
    ASSERT_EQ(s.state, State::RailSlide);
    for (int i = 0; i < 600 && s.state == State::RailSlide; ++i) w->tick({});
    ASSERT_EQ(s.state, State::Fall);
    EXPECT_EQ(s.ledge, -1);
    EXPECT_NEAR(glm::distance(s.pos, vec3(0, 1000, 0)), 0, 1e-3f);
    w->tick({});
    EXPECT_EQ(s.state, State::Fall); // regrab cooldown prevents an endpoint reattachment loop
}

TEST(GameReal, ShipSinkingHallLadder) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l1_ship_sinking_hall", {{1550, 29, 1000}, glm::radians(180.0f)});
    for (int i = 0; i < 10 && w.player().state().state != State::Ladder; ++i) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Ladder);
    EXPECT_EQ(w.collision().markup[size_t(w.player().state().ledge)].id, 234);
}

TEST(GameReal, TerrainGroupNativeUpdatesMergedCollision) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l2_hubhall", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.tick({});
    const auto* plane = w.script().find(2571);
    ASSERT_TRUE(plane);
    const glm::mat4 rot = glm::rotate(glm::mat4(1), plane->rot.x, {1, 0, 0}) *
                          glm::rotate(glm::mat4(1), plane->rot.y, {0, 1, 0}) *
                          glm::rotate(glm::mat4(1), plane->rot.z, {0, 0, 1});
    const vec3 normal = glm::normalize(vec3(rot * glm::vec4(0, 1, 0, 0)));
    const vec3 before = plane->pos + normal * 1000.0f, after = plane->pos - normal * 1000.0f;
    w.script().tick({before, 0, {}, false}, &w.collision());
    w.script().tick({after, 0, {}, false}, &w.collision());
    EXPECT_EQ(w.collision().terrainGroupEnabled(5), false);
    EXPECT_EQ(w.collision().terrainGroupEnabled(1), true);
    EXPECT_FALSE(w.lightEnabled("l2_hubhall", 4));
    EXPECT_TRUE(w.lightEnabled("l2_hubhall", 3));
    EXPECT_GT(w.lightsVersion(), 0u);
}

TEST(GameReal, RampUberCommandMovesCollision) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_staircase_upper", game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.tick({});
    const auto it = std::find_if(w.collision().dynamics.begin(), w.collision().dynamics.end(), [](const auto& d) {
        return d.id == 6045;
    });
    ASSERT_NE(it, w.collision().dynamics.end());
    const glm::mat4 initial = it->transform;
    ASSERT_TRUE(w.sendUberCommand(6045, 1)); // second command with the same numeric ID: raise, not lower
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_NE(it->transform, initial);
}

TEST(GameReal, ElevatorUberCommandCreatesMovingCollision) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l2_puzzleroom");
    vec3 at;
    for (const auto& in : formats::parseUnit(*unit.drm).intros)
        if (in.uniqueId == 2961) at = {in.position[0], in.position[1], in.position[2]};
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l2_puzzleroom", game::loadLaraAnims(bf), game::Spawn{at, 0});
    w.script().setBigfile(&bf);
    w.tick({});
    const auto it = std::find_if(w.collision().dynamics.begin(), w.collision().dynamics.end(), [](const auto& d) {
        return d.id == 2973;
    });
    ASSERT_NE(it, w.collision().dynamics.end());
    const glm::mat4 initial = it->transform;
    Actions use;
    use.interact = true;
    for (int i = 0; i < 30; ++i) w.tick(use);
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_NE(it->transform, initial);
}

TEST(GameReal, ElevatorCarriesPlayer) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l2_puzzleroom");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l2_puzzleroom", game::loadLaraAnims(bf), unit.spawn);
    w.script().setBigfile(&bf);
    w.tick({});
    const auto it = std::find_if(w.collision().dynamics.begin(), w.collision().dynamics.end(), [](const auto& d) {
        return d.id == 2973;
    });
    ASSERT_NE(it, w.collision().dynamics.end());
    const auto* sw = w.script().find(2961);
    ASSERT_TRUE(sw);
    for (int i = 0; i < 30; ++i) w.script().tick({sw->pos, 0, {}, true}, &w.collision());
    game::PlayerState ride;
    ride.pos = vec3(it->transform[3]) + vec3(0, 0, 500);
    w.restore(ride, 0, 0);
    const float startZ = ride.pos.z;
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_LT(w.player().state().pos.z, startZ - 100.0f);
}

TEST(GameReal, BallcourtLeverReplayPassesDoor) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_ballcourt", game::loadLaraAnims(bf), game::Spawn{{-20066, 14150, 3540}, 0});
    w.script().setBigfile(&bf);
    // Physics-only replay: enabling the poacher interrupts this route with genuine hit reactions.
    const auto replay = game::InputScript::parse(
        "0.3 interact 1\n0.4 interact 0\n0.6 forward 1\n1.75 forward 0\n1.8 right 1\n5.0 right 0\n");
    for (int i = 0; i < 360; ++i) w.tick(replay.at(i));
    const auto& p = w.player().state().pos;
    EXPECT_GT(p.x, -18520.0f);
    EXPECT_LT(p.y, 13700.0f);
}

TEST(GameReal, BallcourtLeverDrivesDoorJoint) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_ballcourt", game::loadLaraAnims(bf), game::Spawn{{-20066, 14150, 3540}, 0});
    w.script().setBigfile(&bf);
    Actions use;
    use.interact = true;
    for (int i = 0; i < 30; ++i) w.tick(use);
    for (int i = 0; i < 240; ++i) w.tick({});
    ASSERT_EQ(w.physics().hinges.size(), 2u);
    const float a = w.physics().angle(0), b = w.physics().angle(1);
    EXPECT_NEAR(std::abs(b), 2.0f * std::abs(a), 0.05f);
    EXPECT_NEAR(std::abs(a), glm::radians(40.0f), 0.05f);
    EXPECT_GT(std::max(std::abs(a), std::abs(b)), 0.1f);
}

// Script coverage survey (docs/formats/SCRIPTS.md "Survey"). Static: native call sites per (native type, function)
// and opcodes the VM lacks, over every distinct script type of the 82 units. Dynamic: each unit in a streaming World
// (its natives bound) runs constructors/main scripts, every trigger volume entered, plane crossed and switch used,
// then 300 ticks; handler failures are grouped by reason. Measurement only: run with OU_SCRIPT_SURVEY=1.
TEST(GameReal, ScriptSurvey) {
    if (!gameDir() || !std::getenv("OU_SCRIPT_SURVEY")) GTEST_SKIP() << "OU_GAME_DIR and OU_SCRIPT_SURVEY not set";
    const cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    std::vector<std::string> units;
    const std::string text(list->begin(), list->end());
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        if (bf.read("pc-w\\" + name + ".drm")) units.push_back(name);
    }
    const auto implemented = [](uint8_t op) {
        return op <= 0x15 || (op >= 0x2E && op <= 0x31) || op == 0x34 || (op >= 0x3D && op <= 0x6C) || op == 0x77 ||
               (op >= 0x7B && op <= 0x7E) || op == 0x80;
    };
    std::map<std::string, size_t> calls;
    std::map<uint8_t, size_t> missingOps;
    std::set<uint32_t> seenTypes;
    std::map<std::string, game::UnitData> cache;
    std::map<std::string, std::set<std::string>> failures; // reason -> "unit uid name"
    for (const auto& unit : units) {
        const auto drm = cdc::parseDrm(*bf.read("pc-w\\" + unit + ".drm"));
        std::vector<script::ScriptType> types;
        for (uint32_t i = 0; i < drm.sections.size(); ++i)
            if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(script::parseScriptType(drm, i));
        auto program = std::make_shared<script::vm::Program>(types);
        for (const auto& t : types) {
            if (!seenTypes.insert(t.id).second) continue;
            for (const auto& f : t.functions)
                for (const auto& in : f.code) {
                    if (!implemented(in.op)) ++missingOps[in.op];
                    if (in.op < 0x65 || in.op > 0x6C) continue;
                    for (const auto& p : in.ptrs)
                        if (const auto* callee = program->method(p); callee && callee->native)
                            ++calls[std::string(program->nativeName(program->owner(callee))) + " fn " +
                                    std::to_string(program->functionIndex(callee))];
                }
        }
        game::World w(game::UnitLoader([&](const std::string& name) {
                          auto& d = cache[name];
                          if (!d.drm) d = game::loadUnitData(bf, name);
                          return d;
                      }),
                      unit, game::loadLaraAnims(bf), game::Spawn{{0, 0, 0}, 0});
        w.script().setBigfile(&bf);
        w.tick({});
        auto& rt = w.script();
        const vec3 away(1e7f, 1e7f, 1e7f);
        std::vector<std::pair<script::Kind, std::pair<vec3, vec3>>> todo;
        for (const auto& o : rt.objects()) {
            if (o.unit != unit) continue;
            const glm::mat4 rot = glm::rotate(glm::mat4(1), o.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), o.rot.y, {0, 1, 0}) *
                                  glm::rotate(glm::mat4(1), o.rot.z, {0, 0, 1});
            const vec3 n = glm::normalize(vec3(rot * glm::vec4(0, 1, 0, 0)));
            const vec3 c = o.pos - vec3(0, 0, script::kTestPointHeight);
            todo.push_back({o.kind, {c + n * 1000.0f, c - n * 1000.0f}});
            if (o.kind == script::Kind::Volume) todo.back().second = {c, away};
            if (o.kind == script::Kind::Switch) todo.back().second = {o.pos, o.pos};
        }
        for (const auto& [kind, path] : todo) {
            if (kind == script::Kind::Switch) {
                for (int i = 0; i < 60; ++i) rt.tick({path.first, 0, {}, i < 30}, &w.collision());
            } else if (kind == script::Kind::Volume || kind == script::Kind::Plane) {
                rt.tick({away, 0, {}, false}, &w.collision());
                rt.tick({path.first, 0, {}, false}, &w.collision());
                rt.tick({path.second, 0, {}, false}, &w.collision());
            }
        }
        for (int i = 0; i < 300; ++i) rt.tick({away, 0, {}, false}, &w.collision());
        for (const auto& [key, count] : rt.failures()) {
            const auto* o = rt.find(key.first);
            failures[key.second].insert(unit + " " + std::to_string(key.first) + " " + (o ? o->name : std::string("?")));
        }
    }
    std::vector<std::pair<size_t, std::string>> byCount;
    for (const auto& [k, n] : calls) byCount.push_back({n, k});
    std::sort(byCount.rbegin(), byCount.rend());
    size_t sites = 0;
    for (const auto& [n, k] : byCount) sites += n;
    std::printf("== native call sites (distinct script types): %zu sites, %zu distinct native functions; top 40 ==\n",
                sites, byCount.size());
    for (size_t i = 0; i < byCount.size() && i < 40; ++i) std::printf("%6zu %s\n", byCount[i].first, byCount[i].second.c_str());
    std::printf("== opcodes without VM support ==\n");
    for (const auto& [op, n] : missingOps) std::printf("  0x%02X %zu\n", op, n);
    std::vector<std::pair<size_t, std::string>> failed;
    size_t objects = 0;
    for (const auto& [k, v] : failures) failed.push_back({v.size(), k}), objects += v.size();
    std::sort(failed.rbegin(), failed.rend());
    std::printf("== dynamic handler failures: %zu (start unit, object, reason) triples ==\n", objects);
    for (const auto& [n, k] : failed) {
        std::printf("%6zu %s:", n, k.c_str());
        size_t shown = 0;
        for (const auto& o : failures[k]) if (shown++ < 4) std::printf(" [%s]", o.c_str());
        std::printf("\n");
    }
}

// The three chapter ends reached only through other scripts (docs/GAMEPLAY.md "Level ends"): a trigger (or the
// preceding cinematic's post-stream helper) runs its real bytecode, which activates the closing cinematic_object;
// World plays it and starts the next chapter.
TEST(GameReal, ScriptedChapterEndChainsStartNextChapter) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    struct Chain { const char* unit; int chapter; int32_t trigger; int32_t cinematic; int next; };
    for (const Chain c : {Chain{"l1_ship_sinking_hall", 1, 1074, 1005, 2}, Chain{"l6_ship_engine_hall", 6, 7565, 1387, 7},
                          Chain{"l3_main_hall", 3, 4128, 4070, 4}}) {
        game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), c.unit,
                      game::loadLaraAnims(bf));
        w.script().setBigfile(&bf);
        w.chapter = c.chapter;
        std::vector<std::string> played;
        w.onCinematic = [&](const std::string& name, bool) { played.push_back(name); };
        w.tick({});
        const auto* t = w.script().find(c.trigger);
        ASSERT_TRUE(t) << c.unit;
        const glm::mat4 rot = glm::rotate(glm::mat4(1), t->rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), t->rot.y, {0, 1, 0}) *
                              glm::rotate(glm::mat4(1), t->rot.z, {0, 0, 1});
        const vec3 n = glm::normalize(vec3(rot * glm::vec4(0, 1, 0, 0)));
        const vec3 base = t->pos - vec3(0, 0, script::kTestPointHeight);
        game::PlayerState s = w.player().state();
        for (const vec3 p : t->kind == script::Kind::Plane ? std::vector<vec3>{base + n * 300.0f, base - n * 300.0f}
                                                           : std::vector<vec3>{base + vec3(5000, 0, 0), base}) {
            s.pos = p;
            w.restore(s, 0, 0);
            w.tick({});
        }
        for (int i = 0; i < 1800 && w.chapter == c.chapter; ++i) w.tick({});
        for (const auto& [key, n] : w.script().failures())
            EXPECT_NE(key.first, c.trigger) << c.unit << ": trigger handler stopped (" << key.second << "), legacy fallback";
        EXPECT_EQ(w.chapter, c.next) << c.unit;
        EXPECT_FALSE(played.empty()) << c.unit;
    }
}

// Weather objects of every unit (docs/RENDERING.md "Weather"): the precipitation Enhanced weather shows per unit.
TEST(GameReal, UnitWeatherSurvey) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto objects = bf.read("\\tr8\\pc-w\\objectlist.txt");
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(objects && list);
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(objects->data()), objects->size()});
    const std::string text(list->begin(), list->end());
    std::map<std::string, game::UnitWeather> weather;
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        const auto bytes = bf.read("pc-w\\" + name + ".drm");
        if (!bytes) continue;
        const auto drm = cdc::parseDrm(*bytes);
        if (const auto w = game::unitWeather(names, drm); w.kind != game::Precipitation::None) weather[name] = w;
    }
    std::string got;
    for (const auto& [u, w] : weather)
        got += u + (w.kind == game::Precipitation::Snow ? " snow " : " rain ") + std::to_string(int(w.intensity * 100)) + "\n";
    EXPECT_EQ(got, "l5_startsnow snow 100\nl5_valaskjalf snow 55\nl5_valgrind snow 55\nl6_ship_exterior rain 80\n"
                   "l7_himalaya_end snow 100\nl7_monastery snow 55\nl7_thin_ice snow 100\n");
}
