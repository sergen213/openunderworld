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
#include <set>

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

// tru.exe take-offs under Wine (openunderworld-work/screens/stairs j_*.txt, Instance +0x2A0 per 30 Hz frame):
// standing 48.04 up (apex 201.6, 0.56 s air); stick 49.97 up + 40.00 along it; running, or the stick behind
// her (she turns round), 42.53 up + 65.83 forward; the stick to a side jumps that way standing.
TEST(Game, JumpTakeOffsMatchTruTraces) {
    const auto flight = [](Actions hold, int runTicks, float& vz, float& hspeed, float& yaw, float& apex) {
        CollisionMesh m;
        addFloor(m, -20000, -20000, 20000, 20000, 0);
        auto w = makeWorld(std::move(m), {0, 0, 0});
        for (int i = 0; i < runTicks; ++i) w->tick(forward());
        Actions j = hold;
        j.jump = true;
        w->tick(j);
        const auto& s = w->player().state();
        for (int i = 0; i < 60 && s.state != State::Jump; ++i) w->tick(hold);
        vz = s.vel.z, hspeed = glm::length(glm::vec2(s.vel)), yaw = s.yaw, apex = 0;
        for (int i = 0; i < 120 && s.state == State::Jump; ++i) w->tick(hold), apex = std::max(apex, s.pos.z);
    };
    float vz, h, yaw, apex;
    flight({}, 0, vz, h, yaw, apex);
    EXPECT_NEAR(vz, 48.04f * 30, 1);
    EXPECT_NEAR(h, 0, 1e-3);
    EXPECT_NEAR(apex, 201.6f, 4);
    flight(forward(), 0, vz, h, yaw, apex);
    EXPECT_NEAR(vz, 49.97f * 30, 1);
    EXPECT_NEAR(h, 40.0f * 30, 1);
    flight(forward(), 40, vz, h, yaw, apex);
    EXPECT_NEAR(vz, 42.53f * 30, 1);
    EXPECT_NEAR(h, 65.83f * 30, 1);
    EXPECT_NEAR(apex, 158.0f, 4);
    Actions back;
    back.move.y = -1;
    flight(back, 0, vz, h, yaw, apex);
    EXPECT_NEAR(vz, 42.53f * 30, 1) << "stick behind: turn-round running jump";
    EXPECT_NEAR(h, 65.83f * 30, 1);
    EXPECT_NEAR(std::abs(std::remainder(yaw, 2 * glm::pi<float>())), glm::pi<float>(), 0.01f);
    Actions side;
    side.move.x = 1;
    flight(side, 0, vz, h, yaw, apex);
    EXPECT_NEAR(vz, 49.97f * 30, 1) << "stick to a side: standing jump that way";
    EXPECT_NEAR(h, 40.0f * 30, 1);
}

// tru.exe camera (Wine, l3_main_bedroom start; camst.sh / campitch traces): at rest 1260.8 behind and 491.1 above
// her feet, pitch -0.0225; while she moves a looked-down pitch eases back to rest (-0.342 -> -0.103 in 1 s), at rest
// it holds.
TEST(Game, CameraRestAndPitchReturn) {
    CollisionMesh m;
    addFloor(m, -20000, -20000, 20000, 20000, 0);
    auto w = makeWorld(std::move(m), {0, 0, 0});
    for (int i = 0; i < 10; ++i) w->tick({});
    const auto& c = w->camera();
    EXPECT_NEAR(glm::length(glm::vec2(c.position)), 1260.8f, 2);
    EXPECT_NEAR(c.position.z, 491.1f, 2);
    EXPECT_NEAR(c.position.x, -1260.8f, 2) << "behind her";
    w->restore(w->player().state(), c.yaw, -1.0f);
    for (int i = 0; i < 60; ++i) w->tick({});
    EXPECT_NEAR(c.pitch, -1.0f, 1e-4);
    for (int i = 0; i < 60; ++i) w->tick(forward());
    EXPECT_NEAR(c.pitch, -0.0225f + (-1.0f + 0.0225f) * std::exp(-1.3f), 0.03f);
}

// tru.exe swimming (Wine, l2_cliffclimb sea, swim_l2b.txt): 44.95 units per 30 Hz frame at full stick, reached
// ~0.7 s after the press; about half speed 0.2 s after the release.
TEST(Game, SwimSpeedMatchesTru) {
    auto w = makeWorld(poolMesh(), {1000, -2000, 0}, glm::pi<float>() / 2);
    const auto& s = w->player().state();
    for (int i = 0; i < 120; ++i) w->tick({});
    ASSERT_EQ(s.state, State::SwimTread);
    Actions go = forward();
    for (int i = 0; i < 20; ++i) w->tick(go);
    EXPECT_NEAR(glm::length(s.vel), 24.4f * 30, 150) << "0.33 s into the stroke";
    for (int i = 0; i < 60; ++i) w->tick(go);
    EXPECT_EQ(s.state, State::SwimSurface);
    const vec3 p0 = s.pos;
    for (int i = 0; i < 30; ++i) w->tick(go);
    EXPECT_NEAR(glm::length(s.pos - p0) / 0.5f, 44.95f * 30, 5);
    for (int i = 0; i < 12; ++i) w->tick({});
    EXPECT_NEAR(glm::length(s.vel), 21.6f * 30, 150) << "0.2 s after the release";
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

// Render rates dividing 60 Hz tick on exactly every n-th frame (no frame late through 1/240 rounding); a hitch beyond
// 8 ticks drops the excess time instead of running more ticks.
TEST(Game, FrameClockTickAlignedAndHitchCap) {
    for (const int hz : {120, 240, 300, 600, 1200}) {
        engine::FrameClock clock(game::kTickSeconds);
        for (int f = 1; f <= 10 * hz; ++f) {
            clock.advance(1.0 / hz);
            ASSERT_EQ(clock.ticks(), uint64_t(f * 60 / hz)) << hz << " Hz frame " << f;
        }
    }
    engine::FrameClock clock(game::kTickSeconds);
    EXPECT_EQ(clock.advance(1.0), 8);
    EXPECT_EQ(clock.droppedFrames(), 1u);
    EXPECT_EQ(clock.alpha(), 0.0);
    EXPECT_EQ(clock.advance(game::kTickSeconds), 1);
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
    EXPECT_EQ(anims.clips.size(), game::kLaraClipNames.size() + weaponClips + game::kBikeClipNames.size() + 1); // + grenade throw
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
    for (int i = 0; i < 30; ++i) w->tick({}); // released: coasts to a stop (tru.exe swimDecel)
    EXPECT_NEAR(glm::length(s.vel), 0, 1e-3);
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
    const auto& t = w->player().tuning();
    EXPECT_NEAR(ticks / 60.0, (-150 - z) / t.swimSpeed + t.swimSpeed / (2 * t.swimAccel), 0.4); // + pitch-up time
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
    EXPECT_NEAR(run(0), t.standJumpSpeed * 2 * t.standMoveJumpVz / t.gravity, 15);
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

// tru.exe slides without friction or cap: on the l3_main_bedroom roof (normal z 0.45) the along-slope speed went
// 51.0 -> 69.0 -> 91.3 units per 30 Hz frame in 0.25 s (Wine trace slide1.txt), g sin(63.3 deg) = 4601 units/s^2.
TEST(Game, SlideAcceleratesAtGravityAlongSlope) {
    const float nz = 0.45f, run = 6000, drop = run * std::sqrt(1 - nz * nz) / nz;
    CollisionMesh m;
    addFloor(m, -1000, -1000, 500, 1000, drop);
    addQuad(m, {500, -1000, drop}, {500 + run, -1000, 0}, {500 + run, 1000, 0}, {500, 1000, drop});
    addFloor(m, 500 + run, -1000, 20000, 1000, 0);
    auto w = makeWorld(std::move(m), {400, 0, drop});
    const auto& s = w->player().state();
    for (int i = 0; i < 120 && s.state != State::Slide; ++i) w->tick(forward());
    ASSERT_EQ(s.state, State::Slide);
    for (int i = 0; i < 6; ++i) w->tick({});
    const float a0 = glm::length(glm::vec2(s.vel)) / nz;
    for (int i = 0; i < 30; ++i) w->tick({});
    ASSERT_EQ(s.state, State::Slide);
    const float a1 = glm::length(glm::vec2(s.vel)) / nz;
    EXPECT_NEAR((a1 - a0) / 0.5f, w->player().tuning().gravity * std::sqrt(1 - nz * nz), 50);
    EXPECT_GT(a1, 2738) << "no cap below tru.exe's fastest observed slide (91.3 units/frame)";
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
    EXPECT_NEAR(water[0].points[1].z, 2661.5f, 0.5f); // box top (intro data +0x5C half height), 38 under the goo plane
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
    EXPECT_GT(s.pos.x, entryX + 0.8f * 2 * w.player().tuning().swimSpeed);
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
// A rock wall at x = 400 (z 0..1200, facing -x) with a floor on top. `holds`: a column of free-climb holds
// (markup type 16 triangles, 20 off the rock) at y = 0 from z 450 to the top, as the levels place them.
CollisionMesh climbMesh(bool holds) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 1200}, {400, -2000, 1200});
    addQuad(m, {400, -2000, 1200}, {2000, -2000, 1200}, {2000, 2000, 1200}, {400, 2000, 1200});
    if (holds)
        for (float z = 450; z < 1200; z += 160)
            m.markup.push_back({16, uint16_t(z), {{380, -60, z - 40}, {380, 60, z - 40}, {380, 0, z + 60}}});
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

TEST(Game, ClimbNeedsHolds) {
    auto w = makeWorld(climbMesh(false), {300, 0, 0});
    w->tick(forward());
    EXPECT_NE(w->player().state().state, State::Climb) << "rock without holds is not climbable";
    auto far = makeWorld(climbMesh(true), {300, 1500, 0}); // same wall, 1500 from the hold column
    for (int i = 0; i < 10; ++i) far->tick(forward());
    EXPECT_NE(far->player().state().state, State::Climb) << "only the rock around the holds is climbable";
}

TEST(Game, ClimbStaysWithinHolds) {
    auto w = makeWorld(climbMesh(true), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    for (int i = 0; i < 120; ++i) w->tick({});
    Actions right;
    right.move.x = 1;
    for (int i = 0; i < 600; ++i) w->tick(right); // sideways along the wall, away from the column
    EXPECT_EQ(w->player().state().state, State::Climb);
    EXPECT_LT(std::abs(w->player().state().pos.y), 360.0f) << "stops where the holds end";
}

// Hand stance from the nearest hold's markup type (16..20 = B / L / LW / R / RW); climbing to a hold of another type
// plays that pair's DICE_<from>_to_<to>_<dir> once, then that stance's settle clip.
TEST(Game, ClimbStanceFollowsHolds) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 1200}, {400, -2000, 1200});
    addQuad(m, {400, -2000, 1200}, {2000, -2000, 1200}, {2000, 2000, 1200}, {400, 2000, 1200});
    const uint8_t types[] = {17, 19, 18, 20, 16};
    for (int i = 0; i < 5; ++i) {
        const float z = 450 + 160.0f * float(i);
        m.markup.push_back({types[i], uint16_t(z), {{380, -60, z - 40}, {380, 60, z - 40}, {380, 0, z + 60}}});
    }
    auto w = makeWorld(std::move(m), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    EXPECT_EQ(w->player().state().climbStance, 1); // hold 0 (type 17) is nearest the chest
    EXPECT_EQ(w->player().state().clip, "DICE_L_ground_attach");
    for (int i = 0; i < 120; ++i) w->tick({});
    EXPECT_EQ(w->player().state().clip, "DICE_L_Idle");
    std::vector<std::string> clips;
    std::vector<int> stances{1};
    for (int i = 0; i < 300 && w->player().state().state == State::Climb; ++i) {
        w->tick(forward());
        const auto& s = w->player().state();
        if (clips.empty() || clips.back() != s.clip) clips.push_back(s.clip);
        if (stances.back() != s.climbStance) stances.push_back(s.climbStance);
    }
    EXPECT_EQ(stances, (std::vector<int>{1, 3, 2, 4, 0})) << "L, R, LW, RW, B going up the column";
    for (const char* c : {"DICE_L_settle_up", "DICE_L_to_R_UpL", "DICE_R_settle_up", "DICE_R_to_LW_UpR", "DICE_LW_settle_up"})
        EXPECT_NE(std::find(clips.begin(), clips.end(), c), clips.end()) << c;
}

// The exact-name table: lara.drm's capitalisation (Lw/LW, Rw/RW), and the nearest direction it has.
TEST(Game, DiceTransitionNames) {
    EXPECT_EQ(game::diceTransition(2, 0, {-1, 0}), "DICE_Lw_to_B_Left");
    EXPECT_EQ(game::diceTransition(1, 2, {1, 0}), "DICE_L_to_LW_Right");
    EXPECT_EQ(game::diceTransition(4, 3, {-0.866f, 0.5f}), "DICE_Rw_to_R_UpDiagL");
    EXPECT_EQ(game::diceTransition(4, 4, {0, 1}), "DICE_Rw_to_Rw_UpR") << "no straight Up: nearest is UpR";
    EXPECT_EQ(game::diceTransition(4, 4, {1, -0.5f}), "") << "Rw->Rw has only DownL and UpR, both > 60 degrees off";
    int names = 0;
    for (const auto n : game::kLaraClipNames) names += n.find("_to_") != std::string_view::npos && n.substr(0, 4) == "DICE";
    EXPECT_EQ(names, 181);
}

// l2_cliffclimb's hold field (the ClimbOnlyAtHolds wall): climbing up crosses holds of several types, so the stance
// changes through real transition clips, each one loaded from lara.drm.
TEST(GameReal, CliffclimbStances) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    col.build();
    game::World w(std::move(col), anims, {{-23835, -3600, 300}, glm::radians(90.0f)});
    for (int t = 0; t < 60 && w.player().state().state != State::Climb; ++t) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Climb);
    std::set<int> stances;
    std::vector<std::string> transitions;
    for (int t = 0; t < 600 && w.player().state().state == State::Climb; ++t) {
        Actions a = forward();
        a.move.x = (t / 90) % 2 ? 0.5f : -0.5f; // zig-zag up through the field
        w.tick(a);
        const auto& s = w.player().state();
        stances.insert(s.climbStance);
        EXPECT_NE(anims.find(s.clip), nullptr) << s.clip;
        if (s.clip.find("_to_") != std::string::npos && (transitions.empty() || transitions.back() != s.clip)) transitions.push_back(s.clip);
    }
    std::printf("cliffclimb: %zu stances, transitions:", stances.size());
    for (const auto& c : transitions) std::printf(" %s", c.c_str());
    std::printf("\n");
    EXPECT_GE(stances.size(), 3u);
    EXPECT_GE(transitions.size(), 3u);
}

namespace {
// Pushes Lara into up to `limit` walls of `unit` she can walk up to (a floor 250-700 below the face centre 250 in
// front of it, a wall at chest height), skipping walls with a free-climb hold (markup 16-20) within 1000.
// Returns {walls tried, walls that attached}.
std::pair<int, int> pushIntoWalls(const cdc::BigfileSet& bf, const game::AnimLibrary& anims, const std::string& unit,
                                  int limit) {
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\" + unit + ".drm"))));
    col.build();
    std::vector<vec3> holds;
    for (const auto& m : col.markup)
        if (m.type >= 16 && m.type <= 20 && !m.points.empty()) holds.push_back(m.points[0]);
    int tried = 0, attached = 0;
    const size_t stride = std::max<size_t>(1, col.faces.size() / 400);
    for (size_t i = 0; i < col.faces.size() && tried < limit; i += stride) {
        const auto& f = col.faces[i];
        if (f.flags & 0x40) continue;
        const vec3 a = col.vertices[f.v[0]], b = col.vertices[f.v[1]], c = col.vertices[f.v[2]];
        vec3 n = glm::cross(b - a, c - a);
        if (glm::length(n) < 1e-3f) continue;
        n = glm::normalize(n);
        if (std::abs(n.z) > 0.2f) continue;
        const vec3 mid = (a + b + c) / 3.0f;
        if (std::any_of(holds.begin(), holds.end(), [&](vec3 h) { return glm::length(h - mid) < 1000; })) continue;
        for (const float s : {1.0f, -1.0f}) {
            const vec3 out = glm::normalize(vec3(n.x, n.y, 0)) * s;
            const auto g = col.groundProbe(mid + out * 250.0f, 1000);
            if (!g || g->normal.z < 0.7f || mid.z - g->point.z < 250 || mid.z - g->point.z > 700) continue;
            const vec3 spawn(mid.x + out.x * 250, mid.y + out.y * 250, g->point.z);
            const auto wall = col.raycast(spawn + vec3(0, 0, 300), -out, 400);
            if (!wall || std::abs(wall->normal.z) > 0.35f) continue;
            game::World w(col, anims, {spawn + vec3(0, 0, 5), std::atan2(-out.y, -out.x)});
            bool climbed = false;
            for (int t = 0; t < 30 && !climbed; ++t) {
                w.tick(forward());
                climbed = w.player().state().state == State::Climb;
            }
            ++tried;
            attached += climbed;
            break;
        }
    }
    return {tried, attached};
}
} // namespace

// The user's bug: every wall was free-climbable (the old predicate read the face adjacency flags). Ordinary walls
// in units with and without holds refuse; the l2_cliffclimb hold wall accepts a ground attach.
TEST(GameReal, ClimbOnlyAtHolds) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    for (const char* unit : {"l3_main_bedroom", "l4_pool", "l4_ballcourt", "l2_hubhall", "l2_cliffclimb", "l1_baldrsstairs_a"}) {
        const auto [tried, attached] = pushIntoWalls(bf, anims, unit, 12);
        EXPECT_GE(tried, 4) << unit;
        EXPECT_EQ(attached, 0) << unit << ": ordinary walls must not attach";
    }
    // Hold 910 of l2_cliffclimb at (-23835, -3404, 677) on the -y face, its floor at z 158: walking north attaches.
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    col.build();
    game::World w(std::move(col), anims, {{-23835, -3600, 300}, glm::radians(90.0f)});
    bool climbed = false;
    for (int t = 0; t < 60 && !climbed; ++t) {
        w.tick(forward());
        climbed = w.player().state().state == State::Climb;
    }
    ASSERT_TRUE(climbed) << "the hold wall is climbable";
    for (int t = 0; t < 120; ++t) w.tick({});
    const float z0 = w.player().state().pos.z;
    for (int t = 0; t < 180; ++t) w.tick(forward());
    EXPECT_EQ(w.player().state().state, State::Climb);
    EXPECT_GT(w.player().state().pos.z, z0 + 300) << "climbs up through the hold field";
    // l1_baldrsstairs_a: falling past hold 44 (-5294, -6605, 1860) with forward held catches the rock.
    auto stairs = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l1_baldrsstairs_a.drm"))));
    stairs.build();
    game::World air(std::move(stairs), anims, {{-5294, -6455, 2160}, glm::radians(-90.0f)});
    climbed = false;
    for (int t = 0; t < 120 && !climbed; ++t) {
        air.tick(forward());
        climbed = air.player().state().state == State::Climb;
    }
    EXPECT_TRUE(climbed) << "air attach onto the hold field";
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

// Wall run [I]: roped to a wall anchor from a ledge beside its wall, a sideways stick toward the anchor runs onto the
// wall (ng_ground_to_wallrun_*), then along it (grapple_rappel_right) in a pendulum arc over the pit, onto the far
// ledge, where she stands.
TEST(Game, GrappleWallRunFromGround) {
    CollisionMesh m;
    addFloor(m, -1000, 1000, 500, 2000, 0);   // start ledge
    addFloor(m, -1000, -2000, 500, -1000, 0); // far ledge
    addFloor(m, -1000, -1000, 500, 1000, -3000); // pit
    addBox(m, {500, -3000, -3000}, {800, 3000, 3000}); // wall face x = 500, facing -X
    auto w = makeWorld(std::move(m), {400, 1500, 0}, 0); // facing the wall; stick right runs along -Y
    w->attachGrapple({480, 0, 1500}, 1, game::GrappleKind::Wall);
    Actions run;
    run.move.x = 1;
    for (int i = 0; i < 90 && w->player().state().state != State::GrappleRappel; ++i) w->tick(run);
    ASSERT_EQ(w->player().state().state, State::GrappleRappel);
    EXPECT_EQ(w->player().state().clip, "ng_ground_to_wallrun_right");
    float minZ = 0;
    bool ran = false;
    for (int i = 0; i < 240 && w->player().state().state == State::GrappleRappel; ++i) {
        w->tick(run);
        const auto& s = w->player().state();
        if (s.state != State::GrappleRappel) break;
        ran |= s.clip == "grapple_rappel_right";
        EXPECT_NEAR(s.pos.x, 400.0f, 2.0f) << "feet on the wall, tick " << i;
        EXPECT_NEAR(std::cos(s.yaw), 1.0f, 1e-3f) << "facing the wall";
        EXPECT_LE(glm::distance(s.pos + vec3(0, 0, 400), s.grappleAnchor), s.ropeLength + 1.0f);
        minZ = std::min(minZ, s.pos.z);
    }
    EXPECT_TRUE(ran);
    EXPECT_LT(minZ, -300.0f) << "swung down through the arc";
    EXPECT_LT(w->player().state().pos.y, -1000.0f) << "reached the far ledge";
    EXPECT_NEAR(w->player().state().pos.z, 0.0f, 1.0f);
    EXPECT_NE(w->player().state().state, State::GrappleRappel);
    EXPECT_EQ(w->player().state().grappleUid, 0) << "stands on the far ledge";
}

// The same at l5_valaskjalf's wall anchor 6750 (tests/replays/valaskjalf_wallrun.txt): from the ledge 1600 to its side
// over the pit below it, onto the ledge 1600 to its other side.
TEST(GameReal, ValaskjalfWallRun) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l5_valaskjalf", game::loadLaraAnims(bf), game::Spawn{{6981, -91555, -11378}, -1.5707963f});
    w.script().setBigfile(&bf);
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/valaskjalf_wallrun.txt");
    bool started = false, ran = false;
    float minX = 1e9f, minZ = 0;
    for (uint64_t i = 0; i < 6 * 60; ++i) {
        if (i == 66) { // just before the throw (1.1 s)
            const auto t = w.grappleTarget();
            ASSERT_TRUE(t);
            EXPECT_EQ(t->uid, 6750);
            EXPECT_EQ(t->kind, game::GrappleKind::Wall);
        }
        w.tick(replay.at(i));
        const auto& s = w.player().state();
        if (std::getenv("OU_GRAPPLE_TRACE") && i % 10 == 0)
            std::printf("tick %3llu %s %s pos %.0f %.0f %.0f vel %.0f %.0f %.0f rope %.0f\n", (unsigned long long)i,
                        std::string(game::stateName(s.state)).c_str(), s.clip.c_str(), s.pos.x, s.pos.y, s.pos.z, s.vel.x,
                        s.vel.y, s.vel.z, s.ropeLength);
        started |= s.clip == "ng_ground_to_wallrun_right";
        ran |= s.clip == "grapple_rappel_right";
        EXPECT_NE(w.anims().find(s.clip), nullptr) << s.clip << " not loaded (bind pose)";
        if (s.grappleUid) minX = std::min(minX, s.pos.x), minZ = std::min(minZ, s.pos.z);
    }
    const auto& s = w.player().state();
    std::printf("valaskjalf wall run: min x %.0f min z %.0f, ends %s at %.0f %.0f %.0f\n", minX, minZ,
                std::string(game::stateName(s.state)).c_str(), s.pos.x, s.pos.y, s.pos.z);
    EXPECT_TRUE(started && ran);
    EXPECT_LT(minZ, -11378.0f - 300.0f) << "down through the arc";
    EXPECT_LT(minX, 4200.0f) << "across the pit (the far ledge ends at x 4150)";
    EXPECT_EQ(s.grappleUid, 0) << "let go, standing on the far ledge";
    EXPECT_NEAR(s.pos.z, -11378.0f, 5.0f);
    EXPECT_LT(s.pos.x, 4200.0f);
}

// Uber grapple targets (script type 3565's subclasses), tests/replays/grapple_pull.txt from a spot facing each: look
// up, throw, hold E for a second, and the target's pull handler runs (World::lastPulled); Lara lets go standing.
TEST(GameReal, GrapplePullsUberTargets) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/grapple_pull.txt");
    struct Case { const char* unit; int32_t uid; vec3 spawn; float yawDeg; } cases[] = {
        {"l4_trailstart", 6695, {61161, -42611, 4948}, -11}, // ca_ei_lever_grapple_post
        {"l2_norsehall", 2457, {381, -1901, -1279}, 180},     // sa_ei_grapple_wall_naga_statue
        {"l2_puzzleroom", 2982, {-23353, 42001, 1149}, -67},  // sa_ei_grapple_shiva
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.unit);
        game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), c.unit,
                      anims, game::Spawn{c.spawn, glm::radians(c.yawDeg)});
        w.script().setBigfile(&bf);
        bool braced = false;
        for (uint64_t i = 0; i < 5 * 60; ++i) {
            if (i == 72) { // just before the throw (1.2 s)
                const auto t = w.grappleTarget();
                ASSERT_TRUE(t);
                EXPECT_EQ(t->uid, c.uid);
                EXPECT_EQ(t->kind, game::GrappleKind::Mounted);
            }
            w.tick(replay.at(i));
            braced |= w.player().state().state == State::GrapplePull && w.player().state().grappleUid == c.uid;
        }
        EXPECT_TRUE(braced);
        EXPECT_EQ(w.lastPulled(), c.uid);
        EXPECT_EQ(w.player().state().grappleUid, 0);
        EXPECT_EQ(w.player().state().state, State::Idle);
    }
}

// l1_kraken_room wall anchor 326: jump-on from the ledge east of it, swing onto its wall (grapple_rappel_wall_land*),
// rappel down and back up with the stick, kick off (jump) into a swing. tests/replays/kraken_rappel.txt plays it.
TEST(GameReal, KrakenRoomRappel) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l1_kraken_room", game::loadLaraAnims(bf), game::Spawn{{-2204, -15106, 2212}, 3.1415927f});
    w.script().setBigfile(&bf);
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/kraken_rappel.txt");
    bool landed = false, down = false, up = false;
    float zLanded = 0, zLow = 1e9f, zHigh = -1e9f;
    for (uint64_t i = 0; i < 8 * 60; ++i) {
        w.tick(replay.at(i));
        const auto& s = w.player().state();
        if (std::getenv("OU_GRAPPLE_TRACE") && i % 15 == 0)
            std::printf("tick %3llu %s %s pos %.0f %.0f %.0f uid %d rope %.0f\n", (unsigned long long)i,
                        std::string(game::stateName(s.state)).c_str(), s.clip.c_str(), s.pos.x, s.pos.y, s.pos.z, s.grappleUid, s.ropeLength);
        if (s.state != State::GrappleRappel) continue;
        EXPECT_EQ(s.grappleUid, 326);
        if (!landed) landed = true, zLanded = s.pos.z;
        down |= s.clip.starts_with("grapple_rappel_down");
        up |= s.clip.starts_with("grapple_rappel_up");
        EXPECT_NE(w.anims().find(s.clip), nullptr) << s.clip;
        zLow = std::min(zLow, s.pos.z), zHigh = std::max(zHigh, s.pos.z);
        EXPECT_NEAR(std::abs(std::cos(s.yaw)), 1.0f, 0.05f) << "facing the wall (x = -3161)";
        EXPECT_LE(glm::distance(s.pos + vec3(0, 0, 400), s.grappleAnchor), s.ropeLength + 1.0f);
    }
    std::printf("kraken rappel: landed z %.0f, low %.0f, high %.0f, ends %s\n", zLanded, zLow, zHigh,
                std::string(game::stateName(w.player().state().state)).c_str());
    EXPECT_TRUE(landed && down && up);
    EXPECT_LT(zLow, zLanded - 200.0f);
    EXPECT_GT(zHigh, zLow + 200.0f);
    EXPECT_EQ(w.player().state().state, State::GrappleSwing) << "kicked off the wall";
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
    for (int i = 0; i < 30; ++i) w.tick({}); // section 0 settles in its rest state 1 (0 -> 1 is automatic)
    ASSERT_TRUE(w.sendUberCommand(6045, 1)); // command 1 (trigger 16, 1): 1 -> 6, raise
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
    for (int i = 0; i < 60; ++i) w.tick({}); // startup runs state 0: the elevator waits for its switch
    EXPECT_EQ(it->transform, initial);
    Actions use;
    use.interact = true;
    for (int i = 0; i < 30; ++i) w.tick(use);
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_NE(it->transform, initial);
}

// l7_yggdrasil_end natla_end_mover 8404: intro +0x68 path (200 Bezier keys, loop from the start, 0x5BA280). Its world
// position is the path value + Level+0x80 at the current frame (30 per second); the movers' scripts call the spline
// natives without a stop.
TEST(GameReal, SplineMoverFollowsItsPath) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l7_yggdrasil_end");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l7_yggdrasil_end", game::loadLaraAnims(bf), unit.spawn);
    w.script().setBigfile(&bf);
    w.tick({});
    const auto* mover = w.script().find(8404);
    ASSERT_TRUE(mover && mover->spline);
    EXPECT_EQ(mover->spline->mode, 2); // +0x64 = 2: loop
    EXPECT_TRUE(mover->spline->playing);
    const auto worldPos = [&](const script::Object& o) {
        const glm::mat4 intro = glm::translate(glm::mat4(1), o.pos) * glm::rotate(glm::mat4(1), o.rot.x, {1, 0, 0}) *
                                glm::rotate(glm::mat4(1), o.rot.y, {0, 1, 0}) * glm::rotate(glm::mat4(1), o.rot.z, {0, 0, 1});
        return vec3((intro * o.body)[3]);
    };
    const vec3 start = worldPos(*mover);
    const float t0 = mover->spline->t;
    for (int i = 0; i < 60; ++i) w.tick({});
    mover = w.script().find(8404);
    const auto& path = *mover->spline->path;
    EXPECT_NEAR(mover->spline->t - t0, 30.0f, 1e-3f); // 60 ticks at 60 Hz = 30 frames
    const auto v = formats::evalSpline(*path.position, mover->spline->t, false);
    const vec3 expected = vec3(v[0], v[1], v[2]) + vec3(path.origin[0], path.origin[1], path.origin[2]);
    EXPECT_LT(glm::length(worldPos(*mover) - expected), 0.5f);
    EXPECT_GT(glm::length(worldPos(*mover) - start), 500.0f);
    // Between keys the Bezier segment interpolates (not a key value).
    const auto k0 = formats::evalSpline(*path.position, 31.0f, false), k1 = formats::evalSpline(*path.position, 32.0f, false);
    const auto mid = formats::evalSpline(*path.position, 31.5f, false);
    EXPECT_GT(glm::length(vec3(mid[0], mid[1], mid[2]) - vec3(k0[0], k0[1], k0[2])), 1.0f);
    EXPECT_GT(glm::length(vec3(mid[0], mid[1], mid[2]) - vec3(k1[0], k1[1], k1[2])), 1.0f);
    for (const auto& [key, count] : w.script().failures())
        EXPECT_EQ(key.second.find("native spline"), std::string::npos) << key.first << " " << key.second;
}

// No shipped spline mover has solid collision (natla_end_mover and fish_medsea carry render meshes only), so this puts
// mover 8404's real path on a real solid object (ar_rw_shard, l7_yggdrasil_end): its collision follows the path and
// carries Lara standing on it.
TEST(GameReal, SplineMoverCarriesLara) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto unit = game::loadUnitData(bf, "l7_yggdrasil_end");
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l7_yggdrasil_end", game::loadLaraAnims(bf), unit.spawn);
    w.script().setBigfile(&bf);
    w.tick({});
    const auto* mover = w.script().find(8404);
    ASSERT_TRUE(mover && mover->spline);
    script::Object rock;
    rock.uid = 990001, rock.unit = "l7_yggdrasil_end", rock.name = "ar_rw_shard", rock.kind = script::Kind::Scripted;
    rock.spline = mover->spline;
    rock.spline->t = 0, rock.spline->playing = false;
    const auto& path = *rock.spline->path;
    const auto p0 = formats::evalSpline(*path.position, 0, false);
    rock.pos = vec3(p0[0], p0[1], p0[2]) + vec3(path.origin[0], path.origin[1], path.origin[2]);
    w.script().addObject(std::move(rock), false);
    w.tick({});
    const auto& col = w.collision();
    const auto d = std::find_if(col.dynamics.begin(), col.dynamics.end(), [](const auto& x) { return x.id == 990001; });
    ASSERT_NE(d, col.dynamics.end()) << "ar_rw_shard solid shape";
    const auto top = col.groundProbe(vec3(d->transform[3]) + vec3(0, 0, 3000), 6000);
    ASSERT_TRUE(top && top->dynamic == int(d - col.dynamics.begin()));
    game::PlayerState stand;
    stand.pos = top->point + vec3(0, 0, 1);
    w.restore(stand, 0, 0);
    for (int i = 0; i < 10; ++i) w.tick({}); // settle on the stone
    const size_t index = size_t(top->dynamic);
    const vec3 feet = w.player().state().pos, stone = vec3(w.collision().dynamics[index].transform[3]);
    const_cast<script::Object*>(w.script().find(990001))->spline->playing = true; // Play (spline fn 5)
    for (int i = 0; i < 20; ++i) w.tick({});
    const vec3 moved = vec3(w.collision().dynamics[index].transform[3]) - stone;
    EXPECT_GT(glm::length(moved), 200.0f);
    EXPECT_LT(glm::length((w.player().state().pos - feet) - moved), 0.1f * glm::length(moved));
    const auto under = w.collision().groundProbe(w.player().state().pos + vec3(0, 0, 20), 60);
    EXPECT_TRUE(under && under->dynamic == int(index)) << "Lara is still on the moving stone";
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
        const char* only = std::getenv("OU_SCRIPT_SURVEY_UNIT"); // optional substring filter
        if (bf.read("pc-w\\" + name + ".drm") && (!only || name.find(only) != std::string::npos)) units.push_back(name);
    }
    const auto implemented = [](uint8_t op) {
        return op <= 0x15 || (op >= 0x1D && op <= 0x22 && op != 0x1F) || (op >= 0x2E && op <= 0x31) || op == 0x34 ||
               (op >= 0x3D && op <= 0x6C) || (op >= 0x77 && op <= 0x7E) || op == 0x80;
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

// Reveal camera (docs/GAMEPLAY.md "Cameras"): l1_kraken_room plane 37 calls reveal_object 53, which plays camera spline
// 9 (autosplinecamera 0, camera switch gamebase 53). Its flag +42 = 1 (member default) skips the larabase 56 control
// lock other reveals take. The camera starts on the spline's first key, and when it ends the dispatchers' done
// callbacks run and the orbit camera returns.
TEST(GameReal, RevealCameraPlaysUnitSpline) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), "l1_kraken_room",
                  game::loadLaraAnims(bf));
    w.script().setBigfile(&bf);
    w.tick({});
    const auto* t = w.script().find(37);
    ASSERT_TRUE(t && t->kind == script::Kind::Plane);
    const glm::mat4 rot = glm::rotate(glm::mat4(1), t->rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), t->rot.y, {0, 1, 0}) *
                          glm::rotate(glm::mat4(1), t->rot.z, {0, 0, 1});
    const vec3 n = glm::normalize(vec3(rot * glm::vec4(0, 1, 0, 0)));
    const vec3 base = t->pos - vec3(0, 0, script::kTestPointHeight);
    game::PlayerState s = w.player().state();
    int started = -1;
    for (const vec3 p : {base + n * 300.0f, base - n * 300.0f}) {
        s.pos = p;
        w.restore(s, 0, 0);
        w.tick({});
    }
    for (int i = 0; i < 120 && started < 0; ++i, w.tick({}))
        if (w.splineCameraActive()) started = i;
    ASSERT_GE(started, 0) << "no spline camera";
    EXPECT_EQ(w.cameraUnit(), "l1_kraken_room"); // the spline's unit: the renderer's portal walk starts there
    const auto& spline = w.currentUnit().data.cameraSplines.at(9);
    const auto& k0 = spline.camera.keys.front();
    EXPECT_LT(glm::distance(w.camera().position, vec3(k0.pos[0], k0.pos[1], k0.pos[2]) + w.currentUnit().offset), 200.0f);
    int ticks = 0;
    while (w.splineCameraActive() && ticks < 60 * 30) w.tick({}), ++ticks;
    EXPECT_FALSE(w.splineCameraActive());
    // Camera track 9 lasts its last key's time plus the last key's hold events.
    EXPECT_GE(ticks, int(spline.camera.keys.back().time * 60) - 2);
    for (int i = 0; i < 60; ++i) w.tick({});
    EXPECT_TRUE(w.script().playerControl()) << "the done callback did not give control back";
    for (const auto& [key, count] : w.script().failures()) EXPECT_NE(key.first, 53) << key.second;
    std::printf("reveal: plane 37 base (%.0f %.0f %.0f) normal (%.2f %.2f %.2f) started +%d ticks, ended after %d\n",
                base.x, base.y, base.z, n.x, n.y, n.z, started, ticks);
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
    // Coverage: inside a measured source's column only; unmeasured sources cover the unit.
    const auto& val = weather.at("l5_valaskjalf");
    EXPECT_EQ(val.sources.size(), 8u);
    game::UnitWeather w = val;
    for (auto& s : w.sources) s.radius = 3161, s.depth = 8861;
    const glm::vec3 a = w.sources[0].position;
    EXPECT_FLOAT_EQ(game::weatherCoverage(w, a - glm::vec3(0, 0, 1000)), 1.0f);
    EXPECT_FLOAT_EQ(game::weatherCoverage(w, a + glm::vec3(0, 0, 20000)), 0.0f);      // above every emitter
    EXPECT_FLOAT_EQ(game::weatherCoverage(w, a + glm::vec3(1e6f, 0, 0)), 0.0f);      // outside every column
    EXPECT_FLOAT_EQ(game::weatherCoverage(val, a + glm::vec3(1e6f, 0, 0)), 1.0f);    // unmeasured: whole unit
    std::string got;
    for (const auto& [u, w] : weather)
        got += u + (w.kind == game::Precipitation::Snow ? " snow " : " rain ") + std::to_string(int(w.intensity * 100)) + "\n";
    EXPECT_EQ(got, "l5_startsnow snow 100\nl5_valaskjalf snow 55\nl5_valgrind snow 55\nl6_ship_exterior rain 80\n"
                   "l7_himalaya_end snow 100\nl7_monastery snow 55\nl7_thin_ice snow 100\n");
}

// ---- render-rate independence of real puzzles (docs/GAMEPLAY.md "Timing") ----------------------------------------
namespace {
using MakeWorld = std::function<std::unique_ptr<game::World>()>;

// `ticks` ticks of make()'s world fed `input` (indexed by tick since the start), paced by frames of frameDt(f) seconds
// through the FrameClock as in ou-game, with ou-game's render-path reads (interpolation, pose, grapple target,
// flashlight) between frames. Returns World::stateHash after every tick.
std::vector<uint64_t> digestsAtRate(const MakeWorld& make, const game::InputScript& input, int ticks,
                                    const std::function<double(int)>& frameDt, std::unique_ptr<game::World>* owner = nullptr) {
    auto w = make();
    const uint64_t start = w->ticks();
    engine::FrameClock clock(game::kTickSeconds);
    std::vector<uint64_t> out;
    for (int f = 0; int(out.size()) < ticks; ++f) {
        for (int n = clock.advance(frameDt(f)); n > 0 && int(out.size()) < ticks; --n) {
            w->tick(input.at(w->ticks() - start));
            out.push_back(w->stateHash());
        }
        const double a = clock.alpha();
        (void)w->interpolated(a), (void)w->interpolatedCamera(a), (void)w->pose(a), (void)w->grappleTarget(), (void)w->flashlightPose(a);
        size_t k = 0; // enemy skinning is culled by the view (ou-game EnemyMeshes::pose): a frame-dependent subset
        for (const auto& e : w->enemies().all())
            if ((size_t(f) + k++) % 3 == 0) (void)e->interpolated(a), (void)e->pose(a);
    }
    if (owner) *owner = std::move(w);
    return out;
}

// The digest after every tick is bit-identical at render rates 24..1000 Hz and at irregular frame times with hitches
// (dropped time); returns the 60 Hz world for the caller's puzzle checks.
std::unique_ptr<game::World> expectRateIndependent(const MakeWorld& make, const game::InputScript& input, int ticks) {
    std::unique_ptr<game::World> ref;
    const auto expected = digestsAtRate(make, input, ticks, [](int) { return game::kTickSeconds; }, &ref);
    EXPECT_GT(std::set<uint64_t>(expected.begin(), expected.end()).size(), size_t(ticks / 2)) << "the state changes";
    auto check = [&](const std::string& label, const std::function<double(int)>& dt) {
        const auto got = digestsAtRate(make, input, ticks, dt);
        const auto bad = std::mismatch(expected.begin(), expected.end(), got.begin());
        EXPECT_TRUE(bad.first == expected.end()) << label << ": diverges at tick " << (bad.first - expected.begin());
    };
    for (const double hz : {24.0, 30.0, 75.0, 144.0, 240.0, 1000.0})
        check(std::to_string(int(hz)) + " Hz", [hz](int) { return 1.0 / hz; });
    check("irregular", [](int f) { // 1..50 ms frames, every 37th a 250 ms hitch (more than 8 ticks: time dropped)
        uint32_t x = uint32_t(f) * 2654435761u;
        x ^= x >> 15;
        return f % 37 == 36 ? 0.25 : 0.001 + double(x % 1000) * 0.049 / 1000.0;
    });
    return ref;
}

MakeWorld worldMaker(const cdc::BigfileSet& bf, std::string unit, std::optional<game::Spawn> spawn, bool enemies = false) {
    return [&bf, unit, spawn, enemies] {
        auto w = std::make_unique<game::World>(game::UnitLoader([&bf](const std::string& n) { return game::loadUnitData(bf, n); }),
                                               unit, game::loadLaraAnims(bf), spawn);
        w->script().setBigfile(&bf);
        if (enemies) w->enemies().setBigfile(&bf);
        return w;
    };
}
} // namespace

// l4_ballcourt: pole lever 6502 drives door 6505's hinge (multibody natives), Lara runs through the passage.
TEST(GameReal, BallcourtLeverIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto input = game::InputScript::parse("0.3 interact 1\n0.4 interact 0\n0.6 forward 1\n1.75 forward 0\n1.8 right 1\n5.0 right 0\n");
    const auto w = expectRateIndependent(worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}), input, 360);
    ASSERT_EQ(w->physics().hinges.size(), 2u);
    EXPECT_GT(std::abs(w->physics().angle(1)), 0.1f);
    EXPECT_GT(w->player().state().pos.x, -18520.0f);
}

// The same with l4_ballcourt's enemies live, as in ou-game (poacher fire and hit reactions interrupt the run): AI,
// shots and hit capsules stay tick-based while their skinning is culled per frame by the view.
TEST(GameReal, BallcourtWithEnemiesIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto input = game::InputScript::parse("0.3 interact 1\n0.4 interact 0\n0.6 forward 1\n1.75 forward 0\n1.8 right 1\n5.0 right 0\n");
    const auto w = expectRateIndependent(worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}, true), input, 360);
    EXPECT_FALSE(w->enemies().all().empty());
}

// l1_baldrs_tomb press plate 219 (prismatic joint, Objects.cpp tickPlates): Lara stands on it, it sinks to its pressed
// range (vt 34 -> vt 54; door 223: ProgressionReal.CriticalGates), she walks off and it rises back.
TEST(GameReal, PressurePlateIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    vec3 at(0);
    for (const auto& in : formats::parseUnit(*game::loadUnitData(bf, "l1_baldrs_tomb").drm).intros)
        if (in.uniqueId == 219) at = {in.position[0], in.position[1], in.position[2] + 30};
    const auto input = game::InputScript::parse("1.5 forward 1\n2.5 forward 0\n");
    const auto w = expectRateIndependent(worldMaker(bf, "l1_baldrs_tomb", game::Spawn{at, 0}), input, 300);
    const auto* plate = w->script().find(219);
    ASSERT_TRUE(plate && plate->joint);
    EXPECT_EQ(plate->joint->position, 0.0f) << "the plate rose after Lara left it";
}

// l4_trailstart grapple lever post 6695 (ca_ei_lever_grapple_post, uber joints): Lara pulls it with the grapple, its
// script's command runs section 0's motor (158 units/s), the post slides out on its prismatic joint and drives section
// 1's joint through their 0.75 link; that joint entering its watched range [158, 1580) sends trigger 14 (section 1:
// 0 -> 1) from the simulation, not a timer.
TEST(GameReal, GrappleLeverJointIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/grapple_pull.txt");
    const auto w = expectRateIndependent(worldMaker(bf, "l4_trailstart", game::Spawn{{61161, -42611, 4948}, glm::radians(-11.0f)}), replay, 420);
    EXPECT_EQ(w->lastPulled(), 6695);
    const auto* post = w->uberJoint(6695, 0);
    const auto* linked = w->uberJoint(6695, 1);
    ASSERT_TRUE(post && linked);
    EXPECT_GT(post->x, 211.0f) << "pulled out";
    EXPECT_FLOAT_EQ(linked->x, 0.75f * post->x);
    ASSERT_TRUE(w->uberState(6695));
    EXPECT_EQ((*w->uberState(6695))[1], 1u);
}

// l2_puzzleroom: switch 2961 starts elevator 2973's uber command chain; the platform's collision moves.
TEST(GameReal, ElevatorUberIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    vec3 at(0);
    for (const auto& in : formats::parseUnit(*game::loadUnitData(bf, "l2_puzzleroom").drm).intros)
        if (in.uniqueId == 2961) at = {in.position[0], in.position[1], in.position[2]};
    const auto input = game::InputScript::parse("1.0 interact 1\n1.5 interact 0\n");
    const auto w = expectRateIndependent(worldMaker(bf, "l2_puzzleroom", game::Spawn{at, 0}), input, 300);
    const auto& d = w->collision().dynamics;
    const auto it = std::find_if(d.begin(), d.end(), [](const auto& x) { return x.id == 2973; });
    ASSERT_NE(it, d.end());
    const auto fresh = worldMaker(bf, "l2_puzzleroom", game::Spawn{at, 0})();
    fresh->tick({});
    const auto& d0 = fresh->collision().dynamics;
    const auto it0 = std::find_if(d0.begin(), d0.end(), [](const auto& x) { return x.id == 2973; });
    ASSERT_NE(it0, d0.end());
    EXPECT_NE(it->transform, it0->transform) << "the elevator moved";
}

// l4_traprooms push block 5884: Lara grips it and pushes it along the floor.
TEST(GameReal, PushBlockIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = game::loadUnitData(bf, "l4_traprooms");
    const auto g = std::find_if(unit.grapples.begin(), unit.grapples.end(), [](const auto& t) { return t.uid == 5884; });
    ASSERT_NE(g, unit.grapples.end());
    const auto base = worldMaker(bf, "l4_traprooms", game::Spawn{g->pos + vec3(-1000, 0, 0), 0});
    const MakeWorld make = [&] {
        auto w = base();
        w->tick({});
        const auto* block = w->script().find(5884);
        game::PlayerState s;
        s.pos = block->pos + block->offset - vec3(block->half.x + 60, 0, 0); // west face, facing +X
        if (const auto floor = w->collision().groundProbe(s.pos + vec3(0, 0, 200), 600, 0x0F)) s.pos.z = floor->point.z;
        w->restore(s, 0, -0.25f);
        return w;
    };
    const auto input = game::InputScript::parse("0.2 interact 1\n0.3 forward 1\n2.5 forward 0\n2.6 interact 0\n");
    const auto w = expectRateIndependent(make, input, 180);
    const auto* block = w->script().find(5884);
    ASSERT_TRUE(block);
    EXPECT_GT(glm::length(block->offset), 100.0f) << "the block was pushed";
}

// l7_yggdrasil_end: spline mover 8404's path on a solid stone (as GameReal.SplineMoverCarriesLara), Lara riding it.
TEST(GameReal, SplineMoverIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto base = worldMaker(bf, "l7_yggdrasil_end", game::loadUnitData(bf, "l7_yggdrasil_end").spawn);
    const MakeWorld make = [&] {
        auto w = base();
        w->tick({});
        const auto* mover = w->script().find(8404);
        script::Object rock;
        rock.uid = 990001, rock.unit = "l7_yggdrasil_end", rock.name = "ar_rw_shard", rock.kind = script::Kind::Scripted;
        rock.spline = mover->spline;
        rock.spline->t = 0, rock.spline->playing = true;
        const auto& path = *rock.spline->path;
        const auto p0 = formats::evalSpline(*path.position, 0, false);
        rock.pos = vec3(p0[0], p0[1], p0[2]) + vec3(path.origin[0], path.origin[1], path.origin[2]);
        w->script().addObject(std::move(rock), false);
        w->tick({});
        const auto& col = w->collision();
        const auto d = std::find_if(col.dynamics.begin(), col.dynamics.end(), [](const auto& x) { return x.id == 990001; });
        game::PlayerState stand;
        if (const auto top = col.groundProbe(vec3(d->transform[3]) + vec3(0, 0, 3000), 6000)) stand.pos = top->point + vec3(0, 0, 1);
        w->restore(stand, 0, -0.25f);
        return w;
    };
    const auto input = game::InputScript::parse("0.5 forward 0.4\n1.0 forward 0\n1.5 lookx 1\n2.0 lookx 0\n");
    const auto w = expectRateIndependent(make, input, 180);
    EXPECT_GT(w->script().find(990001)->spline->t, 60.0f) << "the stone travelled";
}

// Stairs (user report: Lara could not walk up the prologue stairs). tru.exe, measured under Wine in l3_main_bedroom
// (docs/GAMEPLAY.md Numbers "Step up / down"): she runs up the staircase's 97..100-unit risers, the root snapping
// onto each step as it crosses the riser line, and the 121-unit hearth edge stops her.
namespace {
struct Riser {
    vec3 front; // floor point 200 in front of the riser
    float yaw;  // facing it
    float top;
};
// Every wide (>= 300) vertical riser of 40..115 with a floor in front, a >= 230-deep tread on top and no wall
// behind, at most one per 200-unit cell.
std::vector<Riser> findRisers(const collision::CollisionMesh& col) {
    std::vector<Riser> out;
    std::set<std::pair<int, int>> seen;
    for (const auto& f : col.faces) {
        if (!collision::queryable(f, 0xFF)) continue;
        const vec3 p0 = col.vertices[f.v[0]], p1 = col.vertices[f.v[1]], p2 = col.vertices[f.v[2]];
        vec3 n = glm::cross(p1 - p0, p2 - p0);
        if (glm::length(n) < 1e-3f) continue;
        n = glm::normalize(n);
        const float lo = std::min({p0.z, p1.z, p2.z}), hi = std::max({p0.z, p1.z, p2.z});
        if (std::abs(n.z) > 0.05f || hi - lo < 40 || hi - lo > 115) continue;
        const float w = std::max({glm::length(glm::vec2(p1 - p0)), glm::length(glm::vec2(p2 - p1)), glm::length(glm::vec2(p0 - p2))});
        if (w < 300) continue;
        const vec3 mid = (p0 + p1 + p2) / 3.0f, o = glm::normalize(vec3(n.x, n.y, 0));
        if (!seen.insert({int(mid.x / 200), int(mid.y / 200)}).second) continue;
        const auto front = col.groundProbe(vec3(mid.x, mid.y, lo) + o * 200.0f + vec3(0, 0, 20), 40);
        const auto top = col.groundProbe(vec3(mid.x, mid.y, hi) - o * 40.0f + vec3(0, 0, 20), 40);
        const auto top2 = col.groundProbe(vec3(mid.x, mid.y, hi) - o * 230.0f + vec3(0, 0, 300), 330);
        if (!front || !top || !top2 || front->normal.z < 0.9f || top->normal.z < 0.9f || std::abs(front->point.z - lo) > 3 ||
            std::abs(top->point.z - hi) > 3 || top2->point.z < hi - 3 || top2->point.z > hi + 115)
            continue;
        if (col.raycast(vec3(mid.x, mid.y, hi + 150) + o * 200.0f, -o, 450)) continue; // wall on the tread
        out.push_back({front->point, std::atan2(-o.y, -o.x), hi});
    }
    return out;
}
} // namespace

TEST(GameReal, StairsWalkAndRunUpAndDown) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto unit = [&](const char* name) { return collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + name + ".drm"))); };
    // Every riser of these staircases (Croft Manor hall and chapel, the Mictlan staircase, Baldr's tomb, the kraken
    // stairs, the pool) is climbed walking and running, without a fall.
    size_t risers = 0;
    for (const char* name : {"l3_main_hall", "l3_chapel", "l4_staircase_mictlan", "l1_baldrs_tomb", "l1_krakenstairs_a",
                             "l1_krakenstairs_b", "l4_pool"}) {
        const auto col = unit(name);
        for (const auto& r : findRisers(col)) {
            ++risers;
            for (const float stick : {0.3f, 1.0f}) {
                game::World w(collision::CollisionMesh(col), game::AnimLibrary(anims), {r.front, r.yaw});
                bool up = false;
                for (int t = 0; t < 60 && !up; ++t) {
                    w.tick(forward(stick));
                    ASSERT_NE(w.player().state().state, State::Fall) << name << " riser at " << r.front.x << "," << r.front.y;
                    up = w.player().state().pos.z > r.top - 3;
                }
                EXPECT_TRUE(up) << name << " riser at " << r.front.x << "," << r.front.y << " stick " << stick;
            }
        }
    }
    EXPECT_GE(risers, 100u);
    // l3_main_bedroom's staircase (risers 97..100, treads 186): up and down the whole flight, walking and running.
    const auto bedroom = unit("l3_main_bedroom");
    for (const float stick : {0.3f, 1.0f}) {
        game::World w(collision::CollisionMesh(bedroom), game::AnimLibrary(anims), {{-8000, -5500, 900}, glm::radians(-90.0f)});
        for (int t = 0; t < 600 && w.player().state().pos.y > -7600; ++t) {
            w.tick(forward(stick));
            ASSERT_NE(w.player().state().state, State::Fall) << "up, stick " << stick << " tick " << t;
        }
        EXPECT_NEAR(w.player().state().pos.z, 1766, 10) << "stick " << stick;
        game::World d(collision::CollisionMesh(bedroom), game::AnimLibrary(anims), {{-8000, -7600, 1800}, glm::radians(90.0f)});
        for (int t = 0; t < 600 && d.player().state().pos.y < -5650; ++t) {
            d.tick(forward(stick));
            ASSERT_NE(d.player().state().state, State::Fall) << "down, stick " << stick << " tick " << t;
        }
        EXPECT_NEAR(d.player().state().pos.z, 870, 30) << "stick " << stick; // the landing below: 802..899
    }
    // The hearth beside the bedroom start is 121 high: blocked, as in tru.exe, where she stops 38..72 short of its edge
    // (Wine trace t2: pushed to and fro); ours stops one capsule radius (79.01) short.
    game::World h(collision::CollisionMesh(bedroom), game::AnimLibrary(anims), {{-4264, 5958, 90}, glm::radians(180.0f)});
    for (int t = 0; t < 120; ++t) h.tick(forward());
    EXPECT_GT(h.player().state().pos.x, -4853 + 30);
    EXPECT_LT(h.player().state().pos.x, -4853 + 85);
    EXPECT_NEAR(h.player().state().pos.z, 87, 1);
}
