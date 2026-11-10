#include "game/Game.hpp"
#include "game/Weather.hpp"
#include "config/Config.hpp"

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
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <unistd.h>

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
    a.clips["sprint_loop"] = rootClip(15, 64.2f);          // ~1927 units/s
    a.clips["ng_sprint_to_exhaust"] = rootClip(17, 49.3f); // 0.567 s
    a.clips["ng_run_exhaust"] = rootClip(20, 42.47f);      // 0.667 s
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
    a.clips["swim_surface_exhausted"] = rootClip(41, 0); // 1.367 s
    // off balance on a beam: the about-to-fall loop (no root motion) and the falls off it (lara.drm: 172 to her left /
    // 193 to her right, 486 / 491 down in 0.33 s; the no-ledge ones 458 down)
    a.clips["bbeam_aboutfall_center"] = rootClip(18, 0);
    a.clips["bbeam_aboutfall_left"] = rootClip(18, 0);
    a.clips["bbeam_aboutfall_right"] = rootClip(18, 0);
    for (const auto& [name, x, z] : {std::tuple{"bbeam_fall_left", 172.2f, -486.0f}, {"bbeam_fall_right", -192.7f, -490.9f},
                                     {"bbeam_fall_left_no_ledge", 172.2f, -458.3f}, {"bbeam_fall_right_no_ledge", -192.7f, -458.3f}}) {
        anim::Clip f = rootClip(5, 0);
        f.tracks[0][anim::TransY].clear();
        for (int i = 0; i < 5; ++i) {
            const float u = float(i) / 4;
            f.tracks[0][anim::TransX].push_back(x * u);
            f.tracks[0][anim::TransZ].push_back(z * u);
        }
        a.clips[name] = f;
    }
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

// The same pool with sides too high to climb out of (their tops 550 over the water; the climb-out reaches 400).
CollisionMesh tallPoolMesh() {
    CollisionMesh m;
    addBox(m, {-3000, -3000, -3000}, {0, 3000, 400});
    addBox(m, {2000, -3000, -3000}, {5000, 3000, 400});
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

// Stick input as if the camera still looked along `yaw0`: the orbit camera drags after Lara (World::updateOrbitCamera),
// so a held sideways stick circles her round it; tests that mean a world direction convert through this.
Actions fixedCamera(const game::World& w, Actions a, float yaw0) {
    const float d = w.camera().yaw - yaw0, c = std::cos(d), s = std::sin(d);
    a.move = {a.move.x * c + a.move.y * s, a.move.y * c - a.move.x * s};
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
    for (int i = 0; i < 180; ++i) w->tick(fixedCamera(*w, a, 0));
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

// tru.exe camera (Wine, l3_main_bedroom start; camst.sh / campitch traces): pitch -0.0225, 491.1 above her feet; at
// rest a looked-down pitch holds; running forward after a look down to -1.05 it flattens (-0.47 / -0.25 / -0.10 at
// 1.0 / 1.6 / 2.3 s) as the camera drags after her. Its distance is the camera settings' 1264.22 (DTP 0x14 +0x60;
// l1_seatop / l2_cliffclimb at rest; the bedroom's 1260.8 behind her).
TEST(Game, CameraRestAndPitchReturn) {
    CollisionMesh m;
    addFloor(m, -20000, -20000, 20000, 20000, 0);
    auto w = makeWorld(std::move(m), {0, 0, 0});
    for (int i = 0; i < 10; ++i) w->tick({});
    const auto& c = w->camera();
    EXPECT_NEAR(glm::length(glm::vec2(c.position)), 1263.9f, 2);
    EXPECT_NEAR(c.position.z, 491.1f, 2);
    EXPECT_NEAR(c.position.x, -1263.9f, 2) << "behind her";
    w->restore(w->player().state(), c.yaw, -1.05f);
    for (int i = 0; i < 60; ++i) w->tick({});
    EXPECT_NEAR(c.pitch, -1.05f, 1e-3);
    for (int i = 0; i < 60; ++i) w->tick(forward());
    EXPECT_NEAR(c.pitch, -0.47f, 0.08f);
    for (int i = 0; i < 36; ++i) w->tick(forward());
    EXPECT_NEAR(c.pitch, -0.25f, 0.08f);
    for (int i = 0; i < 42; ++i) w->tick(forward());
    EXPECT_NEAR(c.pitch, -0.10f, 0.08f);
}

namespace {
// One tick, and how much of its camera yaw turn the forces made: the yaw it ended at minus the yaw read back from
// where it was (the orbit's desired point = last tick's camera position, caught up on open ground) toward this
// tick's pivot (World.cpp updateOrbitCamera).
struct CamTick {
    float force, off; // the forces' turn; the read-back yaw off her heading (0 = behind her)
    vec3 move;        // her move this tick
    float readback;
};
CamTick camTick(game::World& w, const Actions& a) {
    const vec3 before = w.camera().position, from = w.player().state().pos;
    w.tick(a);
    const auto& c = w.camera();
    const float rb = std::atan2(c.target.y - before.y, c.target.x - before.x);
    return {std::remainder(c.yaw - rb, 6.2831853f), std::remainder(rb - w.player().state().yaw, 6.2831853f),
            w.player().state().pos - from, rb};
}
// tru.exe default yaw force set 11 (generalbank DTP 0x1C): 180 deg/s x curve(|off| / 180 deg) x min(1, 0.02 x |her
// move per frame across the view|), toward behind her.
float yawSet11(const CamTick& t, float weight) {
    static constexpr uint8_t kCurve[64] = {0,   0,   0,   0,   0,   0,   0,   3,   9,   14,  20,  27,  34,  41,  48,  55,
                                           62,  69,  76,  83,  89,  95,  101, 108, 115, 122, 128, 134, 140, 145, 148, 151,
                                           152, 151, 149, 144, 139, 132, 125, 117, 109, 101, 94,  88,  82,  75,  68,  61,
                                           54,  47,  40,  33,  26,  20,  14,  8,   3,   0,   0,   0,   0,   0,   0,   0};
    const float x = std::abs(t.off) / glm::pi<float>() * 63, k = std::floor(x);
    const int i = int(k), j = std::min(63, i + 1);
    const float curve = std::round(float(kCurve[j] - kCurve[i]) * (x - k) + float(kCurve[i])) / 255;
    const vec3 camX(std::sin(t.readback), -std::cos(t.readback), 0);
    const float rate = glm::radians(180.0f) * curve * std::min(1.0f, 0.02f * std::abs(glm::dot(t.move, camX))) * weight / 60;
    return std::clamp(-t.off, -rate, rate);
}
} // namespace

// tru.exe camera forces (LaraCameraRewrite 0x5FFB00 -> 0x608B60, weight 0x5FFA90): the default yaw set 11 swings the
// camera round behind her while she runs across the view, only after 5 s without look input (full at 7 s), never
// while she stands or while the camera is being turned.
TEST(Game, CameraSwingsBehindRunningLara) {
    const auto world = [] {
        CollisionMesh m;
        addFloor(m, -30000, -30000, 30000, 30000, 0);
        return makeWorld(std::move(m), {0, 0, 0});
    };
    Actions right;
    right.move.x = 1; // running to the camera's right: across the view
    auto w = world();
    float maxForce = 0;
    for (int i = 1; i <= 12 * 60; ++i) {
        const CamTick t = camTick(*w, right);
        const float weight = std::clamp((float(i) / 60 - 5) / 2, 0.0f, 1.0f);
        if (i <= 5 * 60) ASSERT_NEAR(t.force, 0, 1e-4) << "tick " << i << ": no force within 5 s of the last look";
        else ASSERT_NEAR(t.force, yawSet11(t, weight), 2e-4) << "tick " << i << " off " << t.off;
        if (i > 5 * 60) EXPECT_LE(t.force * t.off, 1e-6f) << "toward behind her";
        maxForce = std::max(maxForce, std::abs(t.force));
    }
    EXPECT_GT(maxForce * 60, glm::radians(20.0f)) << "it swings";
    // Look input held (a slow pitch turn): the clock stays at 0, no swing.
    // Look input held (a slow pitch turn): the clock stays at 0, no swing: she runs round the camera, it turns slower.
    const auto turned = [](game::World& v, const Actions& a) {
        float sum = 0;
        for (int i = 0; i < 12 * 60; ++i) {
            const float y = v.camera().yaw;
            v.tick(a);
            if (i >= 5 * 60) sum += std::remainder(v.camera().yaw - y, 6.2831853f);
        }
        return std::abs(sum);
    };
    Actions lookRight = right;
    lookRight.look.y = -0.01f;
    const float swung = turned(*world(), right), dragged = turned(*world(), lookRight);
    EXPECT_GT(swung, dragged + 0.5f) << "the swing turns the camera after her (" << swung << " vs " << dragged << " rad)";
    const auto off = [](const game::World& v) { return std::remainder(v.camera().yaw - v.player().state().yaw, 6.2831853f); };
    // Standing with the camera turned to her side: it stays there.
    auto still = world();
    Actions turn;
    turn.look.x = 2;
    for (int i = 0; i < 45; ++i) still->tick(turn);
    const float side = off(*still);
    ASSERT_GT(std::abs(side), 1.0f);
    for (int i = 0; i < 12 * 60; ++i) still->tick({});
    EXPECT_NEAR(off(*still), side, 1e-3) << "no swing while she stands";
}

// Surface swimming (camera mode 20: no command 0x10, so the default yaw set 11; no pitch set) swings like on foot.
// tru.exe (Wine, l1_seatop sea, stick held right after a mouse nudge): circling her at 60 deg/s for 5 s, 101 deg/s
// from 7 s on; the yaw controller (camera +0x8F8) holds set 11 (2 forces, 180 deg/s) and her move 21.97 a frame.
TEST(Game, CameraSwingsBehindSwimmingLara) {
    auto w = makeWorld(tallPoolMesh(), {1000, -2000, 0}, glm::pi<float>() / 2); // sides she cannot climb out over
    for (int i = 0; i < 120; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::SwimTread);
    Actions right;
    right.move.x = 1;
    float maxForce = 0;
    for (int i = 0; i < 9 * 60; ++i) {
        const CamTick t = camTick(*w, right);
        if (w->player().state().state != State::SwimSurface || i < 7 * 60) continue;
        ASSERT_NEAR(t.force, yawSet11(t, 1), 2e-4) << "tick " << i << " off " << t.off;
        maxForce = std::max(maxForce, std::abs(t.force));
    }
    EXPECT_GT(maxForce * 60, glm::radians(10.0f));
}

// Hanging (camera mode 11, yaw set 15): toward 60 deg off behind her at 10 deg/s x min(1, 20 x her mean move), on the
// side she shimmies along the ledge (dir 3: the sign of her move along it; [I] the camera looks the way she goes).
TEST(Game, CameraHangYawForceFollowsShimmy) {
    const game::AnimLibrary anims = syntheticAnims();
    const auto tuning = game::defaultTuning();
    const float ledgeZ = anims.hangHandOffset.z + tuning.jumpVz * tuning.jumpVz / (2 * tuning.gravity) * 0.6f;
    CollisionMesh m;
    addFloor(m, -3000, -6000, 300, 6000, 0);
    addBox(m, {300, -6000, 0}, {1000, 6000, ledgeZ});
    m.markup.push_back({1, 7, {{300, 5800, ledgeZ}, {300, -5800, ledgeZ}}}); // hanging to its right (-X), as retail ledges
    auto w = makeWorld(std::move(m), {300 + anims.hangHandOffset.y - 10, 4000, 0});
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 120 && w->player().state().state != State::Hang; ++i) w->tick(i < 2 ? jump : Actions{});
    ASSERT_EQ(w->player().state().state, State::Hang);
    // The stick along the ledge (-Y, her right) under the current camera, so she keeps shimmying as the camera turns.
    const auto along = [&] {
        const float y = w->camera().yaw;
        Actions a;
        a.move = {glm::dot(glm::vec2(0, -1), glm::vec2(std::sin(y), -std::cos(y))), glm::dot(glm::vec2(0, -1), glm::vec2(std::cos(y), std::sin(y)))};
        return a;
    };
    int full = 0;
    for (int k = 0; k < 12 * 60; ++k) {
        const CamTick t = camTick(*w, along());
        const int i = int(w->ticks()); // the look clock runs from the first tick
        if (k >= 60) ASSERT_EQ(w->player().state().state, State::Shimmy) << "tick " << i;
        const float gap = std::remainder(glm::radians(-60.0f) - t.off, 6.2831853f);
        // 10 deg/s x min(1, 20 x her mean move: 294 units/s shimmy = 4.9 a frame) x the look-idle weight
        const float rate = glm::radians(10.0f) / 60 * std::clamp((float(i) / 60 - 5) / 2, 0.0f, 1.0f);
        ASSERT_NEAR(t.force, std::clamp(gap, -rate, rate), 3e-4) << "tick " << i << ": toward 60 deg to her right";
        full += std::abs(t.force) > rate * 0.9f && rate > 0;
        if (i >= 9 * 60) ASSERT_NEAR(gap, 0, 0.03f) << "tick " << i << ": held 60 deg to the side she shimmies to";
    }
    EXPECT_GT(full, 30) << "it turned at the full rate";
}

TEST(Game, CameraForcesIndependentOfRenderRate) {
    const auto script = game::InputScript::parse("0 right 1\n0.3 lookx 2\n0.6 lookx 0\n4 forward 1\n6 forward 0\n");
    auto run = [&](double renderHz) {
        CollisionMesh m;
        addFloor(m, -30000, -30000, 30000, 30000, 0);
        auto w = makeWorld(std::move(m), {0, 0, 0});
        engine::FrameClock clock(game::kTickSeconds);
        while (w->ticks() < 660) // 11 s
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w->ticks() < 660; --n) w->tick(script.at(w->ticks()));
        return w->camera();
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.yaw, o.yaw);
        EXPECT_EQ(a.pitch, o.pitch);
        EXPECT_EQ(a.position, o.position);
    }
}

// tru.exe sprint (Wine, l4_trailstart; screens/controls s_spr / s_rec / s_r4): Shift held while running sprints at
// ~1860..1930 units/s for 9.5..9.9 s, then runs exhausted; Shift still held: ~2.2 s running, 0.9 s sprinting; 4 s
// without Shift after exhaustion refill 4.1 s of sprint.
TEST(Game, SprintStaminaMatchesTru) {
    CollisionMesh m;
    addQuad(m, {-60000, -60000, 0}, {60000, -60000, 0}, {60000, 60000, 0}, {-60000, 60000, 0});
    auto w = makeWorld(std::move(m), {-55000, 0, 0});
    const auto& s = w->player().state();
    Actions sprint = forward();
    sprint.sprint = true;
    const auto sprintSeconds = [&](const Actions& a, int ticks) {
        int n = 0;
        for (int i = 0; i < ticks; ++i) w->tick(a), n += s.clip == "sprint_loop";
        return n / 60.0f;
    };
    w->tick(forward());
    for (int i = 0; i < 30; ++i) w->tick(sprint);
    ASSERT_EQ(s.clip, "sprint_loop");
    const vec3 p0 = s.pos;
    w->tick(sprint);
    EXPECT_NEAR(glm::length(s.pos - p0) * 60, 1927, 40);
    EXPECT_NEAR(sprintSeconds(sprint, 11 * 60) + 0.5f, 9.7f, 0.1f) << "one full sprint";
    EXPECT_NE(s.clip, "sprint_loop");
    EXPECT_NEAR(sprintSeconds(sprint, 6 * 60), 2 * 0.9f, 0.2f) << "held after exhaustion: short bursts";
    for (int i = 0; i < 600 && s.clip != "ng_sprint_to_exhaust"; ++i) w->tick(sprint);
    ASSERT_EQ(s.clip, "ng_sprint_to_exhaust");
    for (int i = 0; i < 72; ++i) w->tick(sprint);      // tru: Shift released 1.2 s after exhaustion,
    for (int i = 0; i < 240; ++i) w->tick(forward()); // 4 s of plain running,
    EXPECT_NEAR(sprintSeconds(sprint, 6 * 60), 4.1f, 0.2f); // then 4.1 s of sprint
    Actions walk = forward();
    walk.walk = true;
    for (int i = 0; i < 60; ++i) w->tick(walk);
    EXPECT_EQ(s.state, State::Walk) << "Alt walks";
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
    EXPECT_EQ(s.clip, "swim_surface_fast_forward") << "tru.exe's surface stroke (docs/GAMEPLAY.md SwimSurface)";
    const vec3 p0 = s.pos;
    for (int i = 0; i < 30; ++i) w->tick(go);
    EXPECT_NEAR(glm::length(s.pos - p0) / 0.5f, 44.95f * 30, 5);
    for (int i = 0; i < 12; ++i) w->tick({});
    EXPECT_NEAR(glm::length(s.vel), 21.6f * 30, 150) << "0.2 s after the release";
}

// tru.exe surface swim turning (Wine, l1_seatop sea, camfollow-wine/turn1.txt): with the stick swung round she turns at
// 117.5 deg/s at full swim speed (2765 / speed rad/s), and her velocity is steered after her heading as a vector at the
// swim acceleration, so it lags the heading in a turn and dips in speed.
TEST(Game, SwimTurnLagsLikeTru) {
    auto w = makeWorld(poolMesh(), {1000, -2500, 0}, glm::pi<float>() / 2);
    const auto& s = w->player().state();
    for (int i = 0; i < 120; ++i) w->tick({});
    for (int i = 0; i < 90; ++i) w->tick(forward());
    ASSERT_EQ(s.state, State::SwimSurface);
    ASSERT_NEAR(glm::length(s.vel), 1348.6f, 1);
    float lag = 0, slowest = 1e9f;
    for (int i = 0; i < 60; ++i) {
        Actions turn; // the stick toward world +x under the current camera
        const float cy = w->camera().yaw;
        turn.move = {std::sin(cy), std::cos(cy)};
        const float y0 = s.yaw, v0 = glm::length(glm::vec2(s.vel));
        w->tick(turn);
        const float turned = std::abs(std::remainder(s.yaw - y0, 6.2831853f)) * 60;
        if (std::abs(std::remainder(s.yaw, 6.2831853f)) > 0.05f) EXPECT_NEAR(turned, 2765.0f / v0, 1e-3f) << "tick " << i;
        lag = std::max(lag, std::abs(std::remainder(std::atan2(s.vel.y, s.vel.x) - s.yaw, 6.2831853f)));
        slowest = std::min(slowest, glm::length(glm::vec2(s.vel)));
    }
    EXPECT_GT(lag, glm::radians(6.0f)) << "the velocity lags her heading";
    EXPECT_LT(slowest, 1340.0f) << "and dips in speed through the turn";
    EXPECT_NEAR(std::remainder(s.yaw, 6.2831853f), 0, 1e-3f) << "facing +x";
}

// tru.exe swim sprint (Wine, l2_cliffclimb sea, player +0x2A0 velocity; openunderworld-work/scratch/swimsprint): Shift
// held swims 53.94 units per 30 Hz frame at the surface and under water (1.2 x 44.95) for ~9.95 s, then normal speed;
// Shift still held: 2.13 s at normal speed, 0.95 s sprinting.
TEST(Game, SwimSprintMatchesTru) {
    CollisionMesh m;
    // deep: after the dive the stick swims along the camera's pitch (down), and the speed is measured along her path
    addQuad(m, {-60000, -60000, -60000}, {60000, -60000, -60000}, {60000, 60000, -60000}, {-60000, 60000, -60000});
    m.markup.push_back({10, 1, {{-60000, -60000, -100150}, {60000, 60000, -150}}});
    auto w = makeWorld(std::move(m), {-55000, 0, 0});
    const auto& s = w->player().state();
    for (int i = 0; i < 120; ++i) w->tick({});
    ASSERT_EQ(s.state, State::SwimTread);
    const auto speed = [&](const Actions& a) {
        const vec3 p0 = s.pos;
        for (int i = 0; i < 30; ++i) w->tick(a);
        return glm::length(s.pos - p0) / 0.5f;
    };
    for (int i = 0; i < 60; ++i) w->tick(forward());
    EXPECT_NEAR(speed(forward()), 44.95f * 30, 5);
    Actions sprint = forward();
    sprint.sprint = true;
    for (int i = 0; i < 30; ++i) w->tick(sprint);
    EXPECT_EQ(s.clip, "swim_surface_sprint_forward");
    EXPECT_NEAR(speed(sprint), 53.94f * 30, 5) << "1 s into the sprint";
    int n = 0;
    for (; n < 15 * 60 && s.clip == "swim_surface_sprint_forward"; ++n) w->tick(sprint);
    EXPECT_NEAR(n / 60.0f + 1.0f, 9.7f, 0.1f) << "one full sprint";
    EXPECT_EQ(s.clip, "swim_surface_exhausted");
    for (int i = 0; i < 15; ++i) w->tick(sprint);
    EXPECT_NEAR(speed(sprint), 44.95f * 30, 5) << "exhausted: normal speed";
    int off = 45, on = 0;
    for (; off < 5 * 60 && s.clip != "swim_surface_sprint_forward"; ++off) w->tick(sprint);
    for (; on < 5 * 60 && s.clip == "swim_surface_sprint_forward"; ++on) w->tick(sprint);
    EXPECT_NEAR(off / 60.0f, 2.13f, 0.1f) << "held after exhaustion";
    EXPECT_NEAR(on / 60.0f, 0.95f, 0.1f);
    for (int i = 0; i < 600; ++i) w->tick({}); // refilled
    Actions drop;
    drop.drop = true;
    for (int i = 0; i < 60; ++i) w->tick(drop); // dive, swim down
    ASSERT_EQ(s.state, State::SwimUnder);
    for (int i = 0; i < 60; ++i) w->tick(sprint);
    EXPECT_EQ(s.clip, "swim_submerged_sprint");
    EXPECT_NEAR(speed(sprint), 53.94f * 30, 5) << "under water";
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
    EXPECT_FALSE(platform::parseInput("Pad leftx")); // sticks bind through move_stick / look_stick
    EXPECT_FALSE(platform::parseInput("Mouse Wheel"));

    platform::DeviceState d;
    const auto press = [&](const char* name, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, e.key.down = down;
        e.key.scancode = SDL_Scancode(platform::parseInput(name)->code);
        d.apply(e);
    };
    const game::Controls standard;
    press("Space", true);
    EXPECT_TRUE(game::readDevices(standard, d).jump);
    press("Space", false);
    SDL_Event click{};
    click.type = SDL_EVENT_MOUSE_BUTTON_DOWN, click.button.button = SDL_BUTTON_RIGHT, click.button.down = true;
    d.apply(click);
    EXPECT_TRUE(game::readDevices(standard, d).aim);
    click.type = SDL_EVENT_MOUSE_BUTTON_UP, click.button.down = false;
    d.apply(click);
    EXPECT_FALSE(game::readDevices(standard, d).aim);
    press("W", true);
    EXPECT_EQ(game::readDevices(standard, d).move.y, 1);
    press("Down", true); // forward and back cancel
    EXPECT_EQ(game::readDevices(standard, d).move.y, 0);
    press("W", false), press("Down", false);
    press("F5", true); // each action maps to its own Actions field (enum order == bindableActions())
    auto a = game::readDevices(standard, d);
    EXPECT_TRUE(a.save);
    EXPECT_FALSE(a.load || a.grapple || a.heal || a.jump);
    press("F5", false);
    // tru.exe defaults (0x487710): Shift sprints, Alt walks, Q grapples, V heals, F light, Caps Lock grenade
    for (const auto& [key, field] : {std::pair{"Left Shift", &game::Actions::sprint}, std::pair{"Left Alt", &game::Actions::walk},
                                     std::pair{"Q", &game::Actions::grapple}, std::pair{"V", &game::Actions::heal},
                                     std::pair{"F", &game::Actions::light}, std::pair{"CapsLock", &game::Actions::grenade},
                                     std::pair{"R", &game::Actions::weapon}, std::pair{"G", &game::Actions::aim},
                                     std::pair{"H", &game::Actions::fire}, std::pair{"Left Ctrl", &game::Actions::drop}}) {
        press(key, true);
        a = game::readDevices(standard, d);
        EXPECT_TRUE(a.*field) << key;
        EXPECT_FALSE(key != std::string_view("Left Shift") && a.sprint) << key;
        EXPECT_FALSE(key != std::string_view("Left Alt") && a.walk) << key;
        press(key, false);
    }

    config::InputConfig custom;
    custom.bindings["jump"] = {"J"};
    custom.bindings["grapple"] = {"Space"};
    const game::Controls rebound(custom);
    press("Space", true);
    a = game::readDevices(rebound, d);
    EXPECT_FALSE(a.jump);
    EXPECT_TRUE(a.grapple);
    custom.bindings["heal"] = {"Hyperspace"};
    EXPECT_THROW(game::Controls{custom}, std::runtime_error);
}

namespace {
SDL_Event padEvent(Uint32 type, SDL_JoystickID pad, int what = 0, int value = 0) {
    SDL_Event e{};
    e.type = type;
    if (type == SDL_EVENT_GAMEPAD_AXIS_MOTION) e.gaxis.which = pad, e.gaxis.axis = uint8_t(what), e.gaxis.value = int16_t(value);
    else if (type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || type == SDL_EVENT_GAMEPAD_BUTTON_UP)
        e.gbutton.which = pad, e.gbutton.button = uint8_t(what), e.gbutton.down = type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    else e.gdevice.which = pad;
    return e;
}
} // namespace

// Sticks (controls.move_stick / look_stick, invert_x / invert_y) and the menu keys (menu_*) rebind; the in-game BUTTON
// CONFIGURATION capture (rebind / unmap) replaces one device's inputs; Steam Deck / Elite back paddles and the positional
// face buttons are bindable names.
TEST(Game, SticksAndMenuNavigationRebind) {
    platform::DeviceState d;
    d.apply(padEvent(SDL_EVENT_GAMEPAD_ADDED, 7));
    d.apply(padEvent(SDL_EVENT_GAMEPAD_AXIS_MOTION, 7, SDL_GAMEPAD_AXIS_LEFTY, -32768)); // left stick fully up
    d.apply(padEvent(SDL_EVENT_GAMEPAD_AXIS_MOTION, 7, SDL_GAMEPAD_AXIS_RIGHTX, 32767)); // right stick fully right
    const game::Controls standard;
    auto a = game::readDevices(standard, d);
    EXPECT_NEAR(a.move.y, 1, 1e-4f);
    EXPECT_NEAR(a.look.x, -2.5f, 1e-3f); // turns the camera right
    config::InputConfig southpaw;
    southpaw.moveStick = "right", southpaw.lookStick = "left", southpaw.invertX = true;
    a = game::readDevices(game::Controls(southpaw), d);
    EXPECT_NEAR(a.move.x, 1, 1e-4f);
    EXPECT_NEAR(a.move.y, 0, 1e-4f);
    EXPECT_NEAR(a.look.y, 2.5f, 1e-3f); // left stick up looks up
    EXPECT_NEAR(a.look.x, 0, 1e-4f);
    southpaw.moveStick = "none";
    EXPECT_EQ(game::readDevices(game::Controls(southpaw), d).move, glm::vec2(0));
    config::InputConfig inverted;
    inverted.invertX = true;
    EXPECT_NEAR(game::readDevices(game::Controls(inverted), d).look.x, 2.5f, 1e-3f);

    // Menus: the defaults are ou-game's former fixed keys; a rebound menu key navigates, the old one no longer does.
    using K = platform::InputRef::Kind;
    const auto key = [](const char* n) { return *platform::parseInput(n); };
    EXPECT_EQ(standard.menuNav(key("Up")), 0);
    EXPECT_EQ(standard.menuNav(key("S")), 1);
    EXPECT_EQ(standard.menuNav(key("Return")), 4);
    EXPECT_EQ(standard.menuNav(key("Escape")), 5);
    EXPECT_EQ(standard.menuNav(key("Backspace")), 6);
    EXPECT_EQ(standard.menuNav(key("E")), 7);
    EXPECT_EQ(standard.menuNav({K::PadButton, SDL_GAMEPAD_BUTTON_SOUTH}), 4);
    EXPECT_EQ(standard.menuNav({K::PadButton, SDL_GAMEPAD_BUTTON_EAST}), 5);
    EXPECT_EQ(standard.menuNav({K::PadButton, SDL_GAMEPAD_BUTTON_START}), 5);
    EXPECT_TRUE(standard.bound(game::Controls::Pause, key("Escape")));
    EXPECT_EQ(standard.menuNav(key("K")), -1);
    config::InputConfig menus;
    game::rebind(menus, "menu_accept", key("K"));
    EXPECT_EQ(menus.bindings["menu_accept"], (std::vector<std::string>{"Pad a", "K"})); // the pad's stays
    const game::Controls reboundMenus(menus);
    EXPECT_EQ(reboundMenus.menuNav(key("K")), 4);
    EXPECT_EQ(reboundMenus.menuNav(key("Return")), -1);
    game::unmap(menus, "menu_accept", true);
    EXPECT_EQ(menus.bindings["menu_accept"], std::vector<std::string>{"K"});
    game::rebind(menus, "fire", {K::PadButton, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1});
    EXPECT_EQ(menus.bindings["fire"].back(), platform::inputName({K::PadButton, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1}));
    EXPECT_EQ(platform::parseInput(menus.bindings["fire"].back())->code, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1);
    EXPECT_EQ(menus.bindings["fire"].size(), 3u); // keyboard + mouse kept
    // Steam Deck back buttons (L4 / R4 / L5 / R5 = SDL paddles) and the misc button are bindable; a / b are positional.
    for (const char* n : {"Pad paddle1", "Pad paddle2", "Pad paddle3", "Pad paddle4", "Pad misc1"}) EXPECT_TRUE(platform::parseInput(n)) << n;
    EXPECT_EQ(platform::parseInput("Pad a")->code, SDL_GAMEPAD_BUTTON_SOUTH);
    EXPECT_EQ(platform::parseInput("Pad b")->code, SDL_GAMEPAD_BUTTON_EAST);
    // Every in-game row names a bindable action; every bindable action has a row.
    EXPECT_EQ(game::kControlRows.size(), config::bindableActions().size());
    for (const auto& r : game::kControlRows) EXPECT_TRUE(std::ranges::count(config::bindableActions(), std::string(r.action)) == 1) << r.action;
}

// Recorded device sessions (--record-input / --replay-input): every event round-trips through its line, and replaying the
// lines through DeviceState gives readDevices the same actions per tick as the live events did. Gamepad hotplug: the
// first pad added is used, others are ignored; once it is removed the next one to press takes over.
TEST(Game, ReplayedDeviceSessions) {
    struct Timed { uint64_t tick; SDL_Event e; };
    const auto key = [](SDL_Scancode s, bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, e.key.scancode = s, e.key.down = down;
        return e;
    };
    SDL_Event motion{}, button{};
    motion.type = SDL_EVENT_MOUSE_MOTION, motion.motion.xrel = 12.5f, motion.motion.yrel = -3;
    button.type = SDL_EVENT_MOUSE_BUTTON_DOWN, button.button.button = SDL_BUTTON_LEFT, button.button.down = true;
    SDL_Event release = button;
    release.type = SDL_EVENT_MOUSE_BUTTON_UP, release.button.down = false;
    const std::vector<Timed> keyboardMouse = {{10, key(SDL_SCANCODE_W, true)}, {12, motion}, {20, button}, {25, release},
                                              {30, key(SDL_SCANCODE_W, false)}, {30, key(SDL_SCANCODE_SPACE, true)},
                                              {32, key(SDL_SCANCODE_SPACE, false)}};
    const std::vector<Timed> gamepad = {
        {0, padEvent(SDL_EVENT_GAMEPAD_ADDED, 3)},
        {1, padEvent(SDL_EVENT_GAMEPAD_ADDED, 4)},
        {5, padEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, 4, SDL_GAMEPAD_BUTTON_SOUTH)}, // not the pad in use
        {6, padEvent(SDL_EVENT_GAMEPAD_AXIS_MOTION, 3, SDL_GAMEPAD_AXIS_LEFTY, -32768)},
        {8, padEvent(SDL_EVENT_GAMEPAD_AXIS_MOTION, 3, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767)},
        {12, padEvent(SDL_EVENT_GAMEPAD_REMOVED, 3)}, // unplugged mid-walk: its stick and trigger let go
        {14, padEvent(SDL_EVENT_GAMEPAD_BUTTON_UP, 4, SDL_GAMEPAD_BUTTON_SOUTH)},
        {15, padEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, 4, SDL_GAMEPAD_BUTTON_NORTH)}, // the other pad takes over
        {18, padEvent(SDL_EVENT_GAMEPAD_BUTTON_UP, 4, SDL_GAMEPAD_BUTTON_NORTH)}};
    const game::Controls controls;
    for (const auto* session : {&keyboardMouse, &gamepad}) {
        // Live: events applied before their tick, actions read every tick; recorded to text on the way.
        platform::DeviceState live;
        std::string text = "# session\n";
        std::vector<std::string> lines;
        std::vector<game::Actions> want;
        size_t next = 0;
        for (uint64_t t = 0; t < 40; ++t) {
            for (; next < session->size() && (*session)[next].tick <= t; ++next) {
                const auto& ev = (*session)[next];
                live.apply(ev.e);
                const std::string line = platform::eventLine(ev.tick, ev.e);
                ASSERT_FALSE(line.empty());
                text += line + "\n", lines.push_back(line);
            }
            want.push_back(game::readDevices(controls, live));
        }
        auto log = platform::EventLog::parse(text);
        EXPECT_EQ(log.size(), session->size());
        platform::DeviceState replayed;
        size_t replayedEvents = 0;
        for (uint64_t t = 0; t < 40; ++t) {
            while (const auto e = log.next(t)) {
                EXPECT_EQ(platform::eventLine(t, *e), lines[replayedEvents++]); // lossless
                replayed.apply(*e);
            }
            const auto a = game::readDevices(controls, replayed), &b = want[t];
            EXPECT_EQ(a.move, b.move) << t;
            EXPECT_EQ(a.look, b.look) << t;
            EXPECT_EQ(std::tuple(a.jump, a.fire, a.interact), std::tuple(b.jump, b.fire, b.interact)) << t;
        }
        EXPECT_TRUE(log.done());
        if (session == &keyboardMouse) {
            EXPECT_EQ(want[11].move.y, 1);
            EXPECT_TRUE(want[22].fire); // mouse left
            EXPECT_TRUE(want[31].jump);
            EXPECT_EQ(want[31].move.y, 0);
        } else {
            EXPECT_EQ(want[5].jump, false);     // pad 4's A while pad 3 is in use
            EXPECT_NEAR(want[7].move.y, 1, 1e-4f);
            EXPECT_TRUE(want[9].fire);          // right trigger past half
            EXPECT_EQ(want[12].move, glm::vec2(0));  // pad 3 gone
            EXPECT_FALSE(want[12].fire);
            EXPECT_TRUE(want[16].interact);     // pad 4's Y: it is the pad in use now
            EXPECT_EQ(replayed.pad, SDL_JoystickID(4));
        }
    }
    // A pad's A press is a menu Accept once it is the pad in use (ou-game: platform::pressedBy -> Controls::menuNav).
    EXPECT_EQ(controls.menuNav(*platform::pressedBy(padEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, 4, SDL_GAMEPAD_BUTTON_SOUTH))), 4);
    EXPECT_THROW(platform::EventLog::parse("5 key 26 1\n4 key 26 0\n"), std::runtime_error); // ticks go backwards
    EXPECT_THROW(platform::EventLog::parse("5 wiggle 1\n"), std::runtime_error);
}

// ---- real data (OU_GAME_DIR) ------------------------------------------------------------------
namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }
}

// tru.exe 0x5C7CA0 (docs/RENDERING.md "Neighbour units"): portal weights by plane / edge distance, and the blend.
TEST(Game, NeighbourUnitBlendWeights) {
    formats::Portal p{};
    p.radius = 2500;
    const float quad[4][3] = {{0, 0, 0}, {1000, 0, 0}, {1000, 0, 1000}, {0, 0, 1000}};
    std::copy(&quad[0][0], &quad[0][0] + 12, &p.quad[0][0]);
    p.max[0] = p.max[2] = 1000, p.normal[1] = 1;
    EXPECT_NEAR(game::portalBlendWeight(p, vec3(0), {500, 1000, 500}), 0.6f, 1e-6f);   // over the quad: its plane
    EXPECT_NEAR(game::portalBlendWeight(p, vec3(0), {1500, 0, 500}), 0.8f, 1e-6f);    // beside it: the nearest edge
    EXPECT_NEAR(game::portalBlendWeight(p, vec3(100, 0, 0), {1500, 0, 500}), 0.84f, 1e-6f); // moved with its unit
    EXPECT_EQ(game::portalBlendWeight(p, vec3(0), {500, 2600, 500}), 0.0f);            // outside the grown box
    scene::Environment a, b, out;
    a.valid = b.valid = true, a.farPlane = 100, b.farPlane = 200, a.ambient[1] = 0.3f;
    const std::pair<const scene::Environment*, float> o[] = {{&b, 1.0f}, {nullptr, 1.0f}};
    game::blendUnitLook(out, a, o);
    EXPECT_TRUE(out.valid);
    EXPECT_FLOAT_EQ(out.farPlane, 150.0f);
    EXPECT_FLOAT_EQ(out.ambient[1], 0.15f);
}

// The same blend at the l4_ballcourt start, against tru.exe read under Wine (Lara at 33313.004, 12734.549, -12879.648
// in its frame, + Level+0x80 in ours): stream trackers +0xA4 = 0.703324 (ballcourt) / 0.296676 (l4_trailtube) =
// 1 / (1 + w), w / (1 + w); view near / far 16 / 113569.9; fog DB6A54 near 9376.6, DBC690 colour 63.263, 71.747,
// 78.120; ambient DBA290 14.474, 16.755, 20.887 (x 1/255 = device +0xFAD4, g_Ambient).
TEST(GameReal, NeighbourUnitBlendMatchesOriginal) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unit = [&](const char* n) { return formats::parseUnit(cdc::parseDrm(std::move(*bf.read(std::string("pc-w\\") + n + ".drm")))); };
    const auto court = unit("l4_ballcourt"), tube = unit("l4_trailtube");
    const vec3 lara = vec3(33313.004f, 12734.549f, -12879.648f) + vec3(-5881.4f, 10787.2f, 15435.9f);
    std::vector<std::pair<const scene::Environment*, float>> others;
    for (const auto& p : court.portals) {
        const float w = game::portalBlendWeight(p, vec3(0), lara);
        if (p.toUnit != "l4_trailtube") {
            EXPECT_EQ(w, 0.0f) << p.toUnit;
            continue;
        }
        EXPECT_NEAR(w, 0.296676f / 0.703324f, 1e-4f);
        others.push_back({&tube.environment, w});
    }
    scene::Environment look;
    game::blendUnitLook(look, court.environment, others);
    EXPECT_NEAR(look.farPlane, 113569.9f, 10.0f);
    EXPECT_NEAR(look.fogFar, 113569.9f, 10.0f);
    EXPECT_NEAR(look.fogNear, 9376.6f, 2.0f);
    const float fog[3] = {63.263f, 71.747f, 78.120f}, ambient[3] = {14.474f, 16.755f, 20.887f};
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(look.fogColor[k] * 255, fog[k], 0.01f);
        EXPECT_NEAR(look.ambient[k] * 255, ambient[k], 0.01f);
    }
    // l2_junglehub start: trackers 0.68145 / 0.31855 (l2_cliffhall_b); the view far 105585.445 and the PSSM split depths
    // (OriginalShading.PssmSplitsMatchOriginal: nearest 2986.066) are the blend's far plane x its fractions.
    const auto hub = unit("l2_junglehub"), hall = unit("l2_cliffhall_b");
    const std::pair<const scene::Environment*, float> h[] = {{&hall.environment, 0.31855f / 0.68145f}};
    game::blendUnitLook(look, hub.environment, h);
    EXPECT_NEAR(look.farPlane, 105585.445f, 5.0f);
    EXPECT_NEAR(look.farPlane * look.pssmSplits[0], 2986.066f, 1.0f);
}

TEST(GameReal, LaraClipsAndRootMotion) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    size_t weaponClips = 0;
    for (int i = 0; i < game::kWeaponCount; ++i) weaponClips += game::weaponClipNames(game::Weapon(i)).size();
    // + grenade throw + the five switch use clips (use:2441 / 2605 / 2671 / 2608 / 4129)
    EXPECT_EQ(anims.clips.size(), game::kLaraClipNames.size() + weaponClips + game::kBikeClipNames.size() + 1 + 5);
    EXPECT_TRUE(anims.find("use:2605")) << "obj_pull_lever";
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

// l4_pool ledge markup id 1253 (rock ledge at z 3562, lower floor 523 below, rock top walkable; the line sits up to ~180
// back from the rock face): the ledge class test measures the floor 94.8 out from the ledge point (tru.exe 0x728650),
// which is the rock top here, so it is no hurdle (pulling her up 523 squared to the line sank her into the rock): a
// standing jump grabs it and a second jump climbs onto the top.
TEST(GameReal, PoolRockGrabNotHurdle) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto bytes = bf.read("pc-w\\l4_pool.drm");
    ASSERT_TRUE(bytes);
    game::World w(collision::loadLevelCollision(cdc::parseDrm(std::move(*bytes))), game::loadLaraAnims(bf),
                  {{-4291, 4225.6f, 3100}, glm::radians(-157.9f)});
    EXPECT_NEAR(w.player().state().pos.z, 3039.5f, 1);
    Actions jump;
    jump.jump = true;
    const auto& s = w.player().state();
    std::vector<std::string> clips;
    for (int i = 0; i < 200; ++i) {
        w.tick(i == 5 || i == 90 ? jump : Actions{});
        if (clips.empty() || clips.back() != s.clip) clips.push_back(s.clip);
    }
    EXPECT_TRUE(std::find(clips.begin(), clips.end(), "ledgeb_attach_M") != clips.end()) << ::testing::PrintToString(clips);
    EXPECT_TRUE(std::find(clips.begin(), clips.end(), "ledgeb_climbup_fast") != clips.end()) << ::testing::PrintToString(clips);
    EXPECT_TRUE(std::none_of(clips.begin(), clips.end(), [](const std::string& c) { return c.starts_with("hurdle_"); }));
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 3571, 1); // rock top
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
        w->tick(forward()); // the stick alone: at the far side she climbs out by herself (no button)
        splashed |= s.state == State::SwimSplash;
        if (s.state == State::SwimTread || s.state == State::SwimSurface) {
            swam |= s.state == State::SwimSurface;
            ASSERT_NEAR(s.pos.z, -150 - w->player().tuning().swimSurfaceDepth, 1e-3) << i; // root under the surface
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
    // Out of the water she dries (tru.exe under Wine: 0.006 per second, 0.0002 per 30 Hz frame).
    EXPECT_EQ(w->wetness().water, 0);
    const float wet = w->wetness().region[6];
    for (int i = 0; i < 600; ++i) w->tick({});
    EXPECT_NEAR(w->wetness().region[6], wet - 0.06f, 1e-3f);
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
    const auto& t = w->player().tuning();
    EXPECT_NEAR(s.pos.z, -150 - t.swimSurfaceDepth, 1e-3);
    EXPECT_NEAR(ticks / 60.0, (-150 - t.swimSurfaceDepth - z) / t.swimSpeed + t.swimSpeed / (2 * t.swimAccel), 0.4); // + pitch-up time
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

// Off balance [I]: the stick across the beam while walking blends in bbeam_unbalance_walk_<side> (request 192's blend
// space; same root motion as bbeam_walk), standing she wobbles (bbeam_idle_wobble_fwd); straight on she walks plainly.
TEST(Game, BeamUnbalanceAndWobble) {
    CollisionMesh m;
    addFloor(m, -2000, -1000, 3000, 1000, -2000);
    m.markup.push_back({14, 9, {{0, 0, 0}, {2400, 0, 0}}});
    auto w = makeWorld(std::move(m), {600, 0, 400});
    const auto& s = w->player().state();
    for (int i = 0; i < 120 && s.state != State::Beam; ++i) w->tick({});
    ASSERT_EQ(s.state, State::Beam);
    for (int i = 0; i < 120; ++i) w->tick({}); // the landing clip
    const auto pushed = [&](game::vec2 stick) { // camera fixed along +X: stick up = +X, right = -Y
        Actions a;
        a.move = stick;
        w->tick(fixedCamera(*w, a, 0));
    };
    pushed({0.5f, 1}); // along +X, a little across
    EXPECT_EQ(s.clip, "bbeam_walk");
    EXPECT_FALSE(s.clipMix.empty()) << "off balance";
    EXPECT_EQ(s.mix, 1.0f);
    const float x0 = s.pos.x;
    for (int i = 0; i < 60; ++i) pushed({0.5f, 1});
    EXPECT_NEAR(s.pos.x - x0, 4.67f * 30 * 1, 10) << "bbeam_walk's root speed";
    pushed({0, 1});
    EXPECT_TRUE(s.clipMix.empty());
    pushed({1, 0}); // straight across
    EXPECT_EQ(s.clip, "bbeam_idle_wobble_fwd");
    EXPECT_EQ(s.state, State::Beam);
    pushed({0, 0});
    EXPECT_EQ(s.clip, "bbeam_idle");
}

namespace {
// A beam along +X over a pit (floor 2000 down), optionally with a ledge along its left (+Y) edge 31.6 off the line as
// on l3_chapel's scaffolds; Lara landed on it facing +X.
std::unique_ptr<game::World> beamOverPit(bool ledge) {
    CollisionMesh m;
    addFloor(m, -2000, -1000, 3000, 1000, -2000);
    m.markup.push_back({14, 9, {{0, 0, 0}, {2400, 0, 0}}});
    if (ledge) m.markup.push_back({1, 10, {{2400, 31.6f, 0}, {0, 31.6f, 0}}}); // hangs to its right: +Y
    auto w = makeWorld(std::move(m), {600, 0, 400});
    for (int i = 0; i < 120 && w->player().state().state != State::Beam; ++i) w->tick({});
    for (int i = 0; i < 120; ++i) w->tick({}); // the landing clip
    return w;
}
void pushStick(game::World& w, game::vec2 stick) { // camera fixed along +X: stick up = +X, right = -Y
    Actions a;
    a.move = stick;
    w.tick(fixedCamera(w, a, 0));
}
} // namespace

// The stick held across the beam tips her into the about-to-fall state after 45 frames (90 ticks: fixed 60 Hz ticks of
// tru.exe's 30 Hz frames); held there, the balance controller (lara.drm objdata +0xF88) leans her fully that way after
// its 20-frame grace and drops her off: with a ledge on that side she falls onto its hang (bbeam_fall_left, then the
// hang idle facing the beam, hands on the ledge).
TEST(Game, BeamOffBalanceEscalatesToFallAndCatchesLedge) {
    auto w = beamOverPit(true);
    const auto& s = w->player().state();
    ASSERT_EQ(s.state, State::Beam);
    const vec3 on = s.pos;
    int tip = -1, fall = -1;
    for (int i = 0; i < 600 && s.state == State::Beam && fall < 0; ++i) {
        pushStick(*w, {-1, 0}); // to her left (+Y)
        if (tip < 0 && s.balance.aboutFall) tip = i + 1;
        if (fall < 0 && s.clip == "bbeam_fall_left") fall = i + 1;
        if (fall < 0) {
            ASSERT_EQ(glm::length(game::vec2(s.pos - on)), 0) << i << ": pinned to the beam";
        }
    }
    EXPECT_EQ(tip, 90) << "45 frames held across";
    ASSERT_GT(fall, 0);
    EXPECT_GE(fall - tip, 2 * 20 + 2 * 10) << "the grace, then a full lean held 10 frames";
    EXPECT_LE(fall - tip, 2 * (20 + 46)) << "the unbalanced limit";
    EXPECT_FALSE(s.balance.aboutFall);
    for (int i = 0; i < 60 && s.state == State::Beam; ++i) w->tick({});
    ASSERT_EQ(s.state, State::Hang);
    EXPECT_EQ(s.ledge, 1);
    EXPECT_EQ(s.clip, "ledgeb_idle");
    EXPECT_NEAR(std::sin(s.yaw), -1, 1e-4) << "facing the beam";
    EXPECT_GT(s.pos.y, 31.6f) << "hanging off its left side";
}

// Counter-stick: back along her heading (within the 15-degree tolerance) or over to the other side (the side lock) for
// more than 2 frames recovers her; she stays on the beam.
TEST(Game, BeamOffBalanceCounterStickRecovers) {
    for (const game::vec2 counter : {game::vec2(0, 1), game::vec2(1, 0)}) {
        auto w = beamOverPit(false);
        const auto& s = w->player().state();
        for (int i = 0; i < 90; ++i) pushStick(*w, {-1, 0});
        ASSERT_TRUE(s.balance.aboutFall);
        for (int i = 0; i < 50; ++i) pushStick(*w, {-1, 0}); // past the grace, leaning left
        ASSERT_TRUE(s.balance.aboutFall);
        EXPECT_EQ(s.balance.side, 1);
        EXPECT_LT(s.balance.blend, 0) << "leaning to her left";
        EXPECT_EQ(s.clip, "bbeam_aboutfall_center");
        EXPECT_EQ(s.clipMix, "bbeam_aboutfall_left");
        int recovered = -1;
        for (int i = 0; i < 20 && recovered < 0; ++i) {
            pushStick(*w, counter);
            if (!s.balance.aboutFall) recovered = i + 1;
        }
        EXPECT_EQ(recovered, 5) << counter.x << ": balanced for more than 2 frames (2.5 frames = 5 ticks)";
        EXPECT_EQ(s.state, State::Beam);
        for (int i = 0; i < 30; ++i) w->tick({});
        EXPECT_EQ(s.state, State::Beam);
        EXPECT_EQ(s.clip, "bbeam_idle");
    }
}

// Without a ledge to catch she falls off with bbeam_fall_right_no_ledge (to her right) and drops to the floor; the
// fall height counts from the fall state's entry after that clip (tru.exe player +0x6DC, set on FallState entry).
TEST(Game, BeamFallWithoutLedgeDrops) {
    auto w = beamOverPit(true); // the ledge is on her left: falling right finds none
    const auto& s = w->player().state();
    bool noLedge = false;
    float fell = 0;
    for (int i = 0; i < 900 && !noLedge; ++i) {
        pushStick(*w, {1, 0});
        noLedge = s.clip == "bbeam_fall_right_no_ledge";
    }
    ASSERT_TRUE(noLedge);
    for (int i = 0; i < 600 && s.state != State::Idle && s.state != State::Land && s.state != State::Dead; ++i) {
        w->tick({});
        if (s.fallHeight > 0) fell = s.fallHeight;
    }
    EXPECT_LT(s.pos.y, -150) << "off its right side";
    EXPECT_NEAR(s.pos.z, -2000, 1);
    EXPECT_GT(fell, 2000 - 458.3f) << "the clip's drop is not counted";
    EXPECT_LT(fell, 2000 - 300);
}

// The escalation runs in fixed 60 Hz ticks: rendered at 30, 60 or 144 Hz the same input tips, leans and drops her off on
// the same tick, onto the same hang.
TEST(Game, BeamFallIndependentOfRenderRate) {
    auto run = [](double renderHz) {
        auto w = beamOverPit(true);
        engine::FrameClock clock(game::kTickSeconds);
        const uint64_t t0 = w->ticks();
        uint64_t fellAt = 0;
        while (w->ticks() < t0 + 400)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w->ticks() < t0 + 400; --n) {
                pushStick(*w, {-1, 0});
                if (!fellAt && w->player().state().clip == "bbeam_fall_left") fellAt = w->ticks() - t0;
            }
        return std::tuple{w->player().state().pos, w->player().state().state, fellAt};
    };
    const auto a = run(30), b = run(60), c = run(144);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_GT(std::get<2>(a), 0u);
    EXPECT_EQ(std::get<1>(a), State::Hang);
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

// Targeted jumps from a hang: up to the ledge above, right to the next one, back across to the far wall, then left
// along it (ledgeb_jump_right mirrored: lara.drm request 979). Each catches where its jump query (q10, q7 / q8, q9)
// first meets the ledge: the right leap's arc (70.80 aside + 34.77 up a frame from the hands) comes back to the
// ledge's height 860 along, the back leap keeps that line, the left one meets the far ledge's inset end (79 in).
TEST(Game, HangJumpsUpRightAndBack) {
    CollisionMesh m;
    addBox(m, {0, -3000, -100}, {1000, 3000, 3000});     // wall, face x = 0
    addBox(m, {-1700, -3000, -100}, {-700, 3000, 3000}); // wall behind, face x = -700
    addFloor(m, -700, -3000, 0, 3000, -100);
    m.markup.push_back({1, 1, {{0, 300, 500}, {0, -300, 500}}});
    m.markup.push_back({1, 2, {{0, 300, 900}, {0, -300, 900}}});      // 400 above
    m.markup.push_back({1, 3, {{0, -600, 900}, {0, -1100, 900}}});    // right of it (facing +X, right = -Y)
    m.markup.push_back({1, 4, {{-700, -1100, 700}, {-700, -500, 700}}}); // behind, on the far wall
    m.markup.push_back({1, 5, {{-700, -1700, 700}, {-700, -1300, 700}}}); // left of it (facing -X, left = -Y)
    auto w = makeWorld(std::move(m), {-300, 0, -100});
    game::PlayerState h;
    h.state = State::Hang, h.ledge = 0, h.ledgeT = 300, h.ledgeSide = -1, h.clip = "ledgeb_idle";
    w->restore(h, 0, -0.25f);
    w->tick({});
    const auto& s = w->player().state();
    int lefts = 0;
    auto jump = [&](float right, float fwd, int ledge, vec3 at) {
        Actions a;
        a.jump = true, a.move = {right, fwd};
        const vec3 from = handsOf(*w, w->anims().hangHandOffset);
        const bool left = glm::dot(glm::vec2(at - from), glm::vec2(std::sin(s.yaw), -std::cos(s.yaw))) < -100; // to her left
        lefts += left;
        w->tick(fixedCamera(*w, a, 0));
        ASSERT_EQ(s.state, State::HangJump);
        EXPECT_EQ(s.ledge, ledge);
        EXPECT_EQ(s.mirror, left) << s.clip;
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
    jump(1, 0, 2, {0, -860.074f, 900});
    jump(0, -1, 3, {-700, -860.074f, 700});
    EXPECT_NEAR(std::cos(s.yaw), -1, 1e-4); // turned round to face the far wall
    jump(1, 0, 4, {-700, -1621, 700}); // stick right on screen (-Y): her left, facing -X
    EXPECT_EQ(lefts, 1) << "the last jump goes to her left";
}

// The hang jump queries by their class records (lara.drm 0x2AA4C, 0x2A9EC): sideways (q7 / q8) the hit must lie 553
// (+0x28) or more from the attach point, so a ledge whose near half ends 521 aside is no target while one running on to
// 1000 is caught where the arc comes back to its height; back (q9), a ledge whose floor 94.8 out of its face is
// 312..474 down is class 6, which entry 2 refuses, while the same ledge over an 800 drop is caught.
TEST(Game, HangJumpRecordsReachAndClass) {
    const auto leap = [](float sideEnd, float backFloor, Actions a) {
        CollisionMesh m;
        addBox(m, {0, -3000, -100}, {1000, 3000, 900});      // wall, face x = 0
        addBox(m, {-1700, -3000, -100}, {-700, 3000, 700});  // far wall, face x = -700, top 700
        addFloor(m, -700, -3000, 0, 3000, backFloor);
        m.markup.push_back({1, 1, {{0, 300, 900}, {0, -300, 900}}});
        m.markup.push_back({1, 2, {{0, -300, 900}, {0, sideEnd, 900}}});
        m.markup.push_back({1, 3, {{-700, -300, 700}, {-700, 300, 700}}});
        auto w = makeWorld(std::move(m), {-300, 0, backFloor});
        game::PlayerState h;
        h.state = State::Hang, h.ledge = 0, h.ledgeT = 300, h.ledgeSide = -1, h.clip = "ledgeb_idle";
        w->restore(h, 0, -0.25f);
        w->tick({});
        a.jump = true;
        w->tick(fixedCamera(*w, a, 0));
        const auto& s = w->player().state();
        if (s.state != State::HangJump) return -1;
        for (int i = 0; i < 150 && s.state == State::HangJump; ++i) w->tick({});
        return s.state == State::Hang ? s.ledge : -2;
    };
    Actions right;
    right.move = {1, 0};
    EXPECT_EQ(leap(-600, -100, right), -1) << "the near ledge ends 521 aside: under q8's 553";
    EXPECT_EQ(leap(-1000, -100, right), 1);
    Actions back;
    back.move = {0, -1};
    EXPECT_EQ(leap(-600, 300, back), -1) << "class 6: its floor 400 down";
    EXPECT_EQ(leap(-600, -100, back), 2);
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
    EXPECT_EQ(s.clip, "hpole_perp_fling");
    int onPole = 1;
    for (; onPole < 120 && s.state == State::PoleHang; ++onPole) w->tick({});
    ASSERT_EQ(s.state, State::HangJump);
    EXPECT_NEAR(onPole / 60.0, 0.64, 0.04) << "tru.exe: she leaves the pole 0.64 s after the press";
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
    EXPECT_NEAR(s.pos.z, surf - w.player().tuning().swimSurfaceDepth, 1e-3);
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
    // Back west; with the stick held she climbs out at the first wall with a top 50..400 above the goo (no button).
    ASSERT_TRUE(runUntil([&](int) { return forward(-1); }, inState(State::SwimClimbOut), 900))
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
    for (int i = 0; i < 300; ++i) w.tick(fixedCamera(w, forward(0.3f), 0)), onBeam(i);
    EXPECT_NEAR(s.pos.x - x0, 140 * 5, 60); // bbeam_walk: 501 units per 3.57 s cycle
    Actions jump = forward(0.3f);
    jump.jump = true;
    w.tick(fixedCamera(w, jump, 0));
    EXPECT_EQ(s.state, State::JumpPrep);
    const float x1 = s.pos.x;
    for (int i = 0; i < 120 && s.state != State::Beam; ++i) w.tick(fixedCamera(w, forward(0.3f), 0));
    ASSERT_EQ(s.state, State::Beam);
    EXPECT_EQ(s.clip, "bbeam_jumpland");
    EXPECT_GT(s.pos.x - x1, 300);
    for (int i = 0; i < 900; ++i) w.tick(fixedCamera(w, forward(0.3f), 0)), onBeam(i);
    EXPECT_NEAR(s.pos.x, -3986, 1); // stopped at the far end
}

// l3_chapel's scaffold: balance beam 1031 (63 wide, x 4708.6) has ledgegrab loop 1009 round its top (x 4740.6 /
// 4677.4). Hanging from the ledge, jump: the top 139 over the ledge is air (tru.exe 0x74E290 clearance class 1, no
// ledgeb_climbup_fast, which used to carry her 102 past the line and off the far side), the beam is the jump target:
// bbeam_ledgeu_climbup puts her on the beam, then the beam idle (chart S439 T698 -> S882 -> S885).
TEST(GameReal, ChapelHangClimbsOntoBalanceBeam) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l3_chapel", {{4800, 4500, -6700}, 0});
    const auto& s = w.player().state();
    const int ledge = markupIndex(w, 1009), beam = markupIndex(w, 1031);
    ASSERT_GE(ledge, 0);
    ASSERT_GE(beam, 0);
    for (const int seg : {0, 2}) { // the +x and -x sides of the beam
        bool hung = false;
        for (const float side : {1.0f, -1.0f}) {
            game::PlayerState h;
            h.state = State::Hang, h.ledge = ledge, h.ledgeSeg = seg, h.ledgeSide = side, h.ledgeT = 400, h.clip = "ledgeb_idle";
            w.restore(h, 0, -0.25f);
            w.tick({});
            if (s.state != State::Hang || std::cos(s.yaw) * (seg == 0 ? -1.0f : 1.0f) < 0.7f) continue; // outside, facing the beam
            hung = true;
            break;
        }
        ASSERT_TRUE(hung) << seg;
        w.restore(s, s.yaw, -0.25f);
        w.tick({}); // restore holds the buttons: no press edge on its first tick
        ASSERT_EQ(s.state, State::Hang);
        const float hangY = s.pos.y;
        Actions jump;
        jump.jump = true;
        w.tick(jump);
        ASSERT_EQ(s.state, State::Beam) << seg << " " << game::stateName(s.state);
        EXPECT_EQ(s.ledge, beam);
        EXPECT_EQ(s.clip, "bbeam_ledgeu_climbup");
        for (int i = 0; i < 120; ++i) {
            w.tick({});
            ASSERT_EQ(s.state, State::Beam) << seg << " tick " << i;
            ASSERT_NEAR(s.pos.x, 4708.6f, 1) << i;
            ASSERT_NEAR(s.pos.z, -6163.0f, 1) << i;
        }
        EXPECT_EQ(s.clip, "bbeam_idle");
        EXPECT_NEAR(s.pos.y, hangY, 150); // up where she hung (her hands 105 behind her root along the beam)
    }
}

// Falling off l3_chapel's scaffold beam 1031 (ledge loop 1009 round it, 31.6 each side): the stick held to one side
// tips her into about to fall, then off that side onto the loop's ledge there, hanging facing the beam
// (bbeam_fall_left / _right); l4_temple's beam 1079 likewise onto ledge 1085 along its right side.
TEST(GameReal, BeamFallsCatchScaffoldLedge) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l3_chapel", {{4800, 4500, -6700}, 0});
    const auto& s = w.player().state();
    const int ledge = markupIndex(w, 1009), beam = markupIndex(w, 1031);
    ASSERT_GE(ledge, 0);
    ASSERT_GE(beam, 0);
    const float facing = -1.5707963f; // along the beam's points: -y
    for (const auto& [stickX, clip, seg, wallX] : {std::tuple{-1.0f, "bbeam_fall_left", 0, 4740.6f}, {1.0f, "bbeam_fall_right", 2, 4677.4f}}) {
        game::PlayerState b;
        b.state = State::Beam, b.ledge = beam, b.ledgeSeg = 0, b.ledgeT = 400, b.ledgeSide = 1, b.clip = "bbeam_idle";
        b.pos = game::vec3(4708.6f, 4994.8f - 400, -6163.0f), b.yaw = facing;
        w.restore(b, facing, -0.25f);
        bool fell = false;
        for (int i = 0; i < 600 && s.state == State::Beam; ++i) {
            Actions a;
            a.move = {stickX, 0};
            w.tick(fixedCamera(w, a, facing));
            fell |= s.clip == clip;
        }
        EXPECT_TRUE(fell) << clip;
        ASSERT_EQ(s.state, State::Hang) << clip << " " << game::stateName(s.state);
        EXPECT_EQ(s.ledge, ledge);
        EXPECT_EQ(s.ledgeSeg, seg);
        EXPECT_GT(std::cos(s.yaw) * (seg == 0 ? -1.0f : 1.0f), 0.99f) << "facing the beam";
        EXPECT_GT((s.pos.x - wallX) * (seg == 0 ? 1.0f : -1.0f), 100) << "hanging outside the scaffold";
    }
    auto t = unitWorld(bf, "l4_temple", {{-5500, -8099, 3100}, 0});
    const auto& ts = t.player().state();
    t.tick({});
    ASSERT_EQ(ts.state, State::Beam);
    for (int i = 0; i < 120; ++i) t.tick({});
    bool fell = false;
    for (int i = 0; i < 600 && ts.state == State::Beam; ++i) {
        Actions a;
        a.move = {1, 0};
        t.tick(fixedCamera(t, a, 0));
        fell |= ts.clip == "bbeam_fall_right";
    }
    EXPECT_TRUE(fell);
    ASSERT_EQ(ts.state, State::Hang) << game::stateName(ts.state);
    EXPECT_EQ(t.collision().markup[size_t(ts.ledge)].id, 1085);
    EXPECT_GT(std::sin(ts.yaw), 0.99f) << "facing the beam (+y)";
    EXPECT_LT(ts.pos.y, -8099 - 100) << "off its right side (-y)";
}

// Ledge to ledge (l4_temple): grab 1187 from a standing jump, jump up 528 to 1162; hanging on 1207, jump back across
// the 948 gap to 1208 at the same height, turning round. (1158 -> 1159, 585 behind and 324 lower, is no target: the
// floor 94.8 out from 1159 is 312..474 down, ledge class 6, which q9's record 0x2A9EC refuses.)
TEST(GameReal, TempleLedgeJumpsUpAndBack) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions jump;
    jump.jump = true;
    auto run = [&](game::Spawn spawn, uint16_t first, Actions a, uint16_t second, int hangSeg = -1) {
        auto w = unitWorld(bf, "l4_temple", spawn);
        const auto& s = w.player().state();
        if (hangSeg >= 0) { // hang there (from its middle), camera behind her
            game::PlayerState h;
            h.state = State::Hang, h.ledge = markupIndex(w, first), h.ledgeSeg = hangSeg, h.ledgeSide = -1, h.clip = "ledgeb_idle";
            ASSERT_GE(h.ledge, 0);
            const auto& m = w.collision().markup[size_t(h.ledge)];
            h.ledgeT = 0.5f * glm::length(glm::vec2(m.points[size_t(hangSeg) + 1] - m.points[size_t(hangSeg)]));
            w.restore(h, 0, -0.25f);
            w.tick({});
            w.restore(s, s.yaw, -0.25f);
        }
        for (int i = 0; i < 90; ++i) w.tick(i == 5 && hangSeg < 0 ? jump : Actions{});
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
    run({{-3300, -10900, -2800}, glm::radians(90.0f)}, 1207, forward(-1), 1208, 3);
}

// l4_pool rock ledge 1253 (hands z 3562; PoolRockGrabNotHurdle's spawn): grabbed and climbed onto the rock top, drop
// (crouch button) goes back down into the hang on 1253.
TEST(GameReal, PoolDropToHang) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const game::Spawn spawn{{-4291, 4225.6f, 3100}, glm::radians(-157.9f)};
    Actions jump;
    jump.jump = true;
    auto w = unitWorld(bf, "l4_pool", spawn);
    const auto& s = w.player().state();
    for (int i = 0; i < 200; ++i) w.tick(i == 5 || i == 90 ? jump : Actions{}); // grab, climb onto the top
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

// l2_cliffclimb's basic obstacles (the shore plateau west of the start boat, floor ~3000-3200): walking into the
// 169-high step (ledge 941) hurdles onto it; a standing jump at the 474-high block (939) pulls her up onto it
// (hurdle_ground_attach_mid_150 -> hurdle_onto); the 614-high wall (911) is caught from a standing jump with the
// stick and from a running jump (pressed square against its face: hands 63 over the ledge line), then climbed.
TEST(GameReal, CliffclimbObstaclesAndLedgeGrabs) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto unitBytes = *bf.read("pc-w\\l2_cliffclimb.drm");
    const auto anims = game::loadLaraAnims(bf);
    // Ticks `input` from the spawn; returns the clips played (in order) and the ledge ids she was attached to.
    struct Run { std::vector<std::string> clips; std::set<int> ledges; game::PlayerState end; float snap = -1; };
    const auto run = [&](vec3 pos, float yawDeg, int ticks, auto input) {
        game::World w(collision::loadLevelCollision(cdc::parseDrm(std::vector<uint8_t>(unitBytes))), anims, {pos, glm::radians(yawDeg)});
        Run r;
        float lastClimbZ = 0;
        for (int t = 0; t < ticks; ++t) {
            w.tick(input(t, w.player().state()));
            const auto& s = w.player().state();
            if (r.clips.empty() || r.clips.back() != s.clip) r.clips.push_back(s.clip);
            if (s.ledge >= 0) r.ledges.insert(w.collision().markup[size_t(s.ledge)].id);
            if (s.state == State::ClimbUp) lastClimbZ = s.pos.z;
            else if (r.snap < 0 && lastClimbZ != 0) r.snap = std::abs(s.pos.z - lastClimbZ);
        }
        r.end = w.player().state();
        return r;
    };
    const auto has = [](const Run& r, const char* clip) { return std::find(r.clips.begin(), r.clips.end(), clip) != r.clips.end(); };
    // Waist-high step: walk / run into it.
    Run r = run({-28152, -1550, 3100}, 90, 60, [](int, const game::PlayerState&) { return forward(); });
    EXPECT_TRUE(has(r, "hurdle_low_onto"));
    EXPECT_TRUE(r.ledges.count(941));
    EXPECT_GT(r.end.pos.z, 3160); // on the step (floor 3010), running on over its sloping top
    EXPECT_TRUE(r.end.state == State::Run || r.end.state == State::Idle);
    // Chest-high block: a standing jump in front of it.
    r = run({-28029, -650, 3300}, 90, 120, [](int t, const game::PlayerState&) { Actions a; a.jump = t == 5; return a; });
    // 474 high: the top of request 128 (mid_100 / mid_150) or the bottom of 129 (high_150 / high_200).
    EXPECT_TRUE((has(r, "hurdle_ground_attach_mid_100") || has(r, "hurdle_ground_attach_high_150")) && has(r, "hurdle_onto")) << r.clips.size();
    EXPECT_TRUE(r.ledges.count(939));
    EXPECT_EQ(r.end.state, State::Idle);
    EXPECT_NEAR(r.end.pos.z, 3638, 5);
    EXPECT_LT(r.snap, 10);
    // Head-high wall: catch it, hang, pull up (jump again once hanging).
    const auto grabAndClimb = [&](const Run& g) {
        EXPECT_TRUE(has(g, "ledgeb_attach_M"));
        EXPECT_TRUE(has(g, "ledgeb_climbup_fast"));
        EXPECT_TRUE(g.ledges.count(911));
        EXPECT_EQ(g.end.state, State::Idle);
        EXPECT_NEAR(g.end.pos.z, 3807, 5);
        EXPECT_LT(g.snap, 10); // ledgeb_climbup_fast starts where its root motion ends on the ledge
    };
    const auto pullUp = [](int t, const game::PlayerState& s) { Actions a; a.jump = s.state == State::Hang && s.clip == "ledgeb_idle" && t % 2; return a; };
    grabAndClimb(run({-29439, -900, 3300}, 110, 240, [&](int t, const game::PlayerState& s) {
        Actions a = t >= 10 && t < 40 ? forward() : Actions{};
        a.jump = t == 10 || pullUp(t, s).jump;
        return a;
    }));
    grabAndClimb(run({-28734, -1318, 3300}, 107.3f, 240, [&](int t, const game::PlayerState& s) {
        Actions a = s.state == State::Run || t < 25 ? forward() : Actions{};
        a.jump = t == 25 || pullUp(t, s).jump;
        return a;
    }));
}

// Ticks `input` in `unit` from the spawn; the clips played in order and the state on the tick the obstacle move ended.
static std::pair<std::vector<std::string>, game::PlayerState> obstacleRun(const char* unit, vec3 pos, float yawDeg, int ticks, Actions in) {
    const cdc::BigfileSet bf(gameDir());
    game::World w(collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + unit + ".drm"))), game::loadLaraAnims(bf),
                  {pos, glm::radians(yawDeg)});
    std::vector<std::string> clips;
    std::optional<game::PlayerState> after;
    bool climbing = false;
    for (int t = 0; t < ticks; ++t) {
        w.tick(in);
        const auto& s = w.player().state();
        if (clips.empty() || clips.back() != s.clip) clips.push_back(s.clip);
        if (climbing && s.state != State::ClimbUp && !after) after = s;
        climbing = s.state == State::ClimbUp;
    }
    return {clips, after.value_or(w.player().state())};
}

// l2_junglehub ledge 2: a 273-high, 240-thick wall (ledges both sides, the floor behind it ~3975): running into it the
// ledge class is 5 (low) and 0x73DFF0's class 0: hurdle_low_over straight off the run (chart S958 -> S971), landing
// on the floor behind it at her own height.
TEST(GameReal, JunglehubVaultOver) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto [clips, end] = obstacleRun("l2_junglehub", {-9422.8f, 18174.8f, 4100}, -90, 120, forward());
    EXPECT_TRUE(std::find(clips.begin(), clips.end(), "hurdle_low_over") != clips.end() ||
                std::find(clips.begin(), clips.end(), "hurdle_over_low_long_50") != clips.end())
        << ::testing::PrintToString(clips);
    EXPECT_LT(end.pos.y, 17774.8f - 100); // past the wall (its far ledge 3)
    EXPECT_NE(end.state, State::Fall);   // on the floor behind it (3975), not falling
    EXPECT_NEAR(end.pos.z, 3975, 40);
}

// l4_pool rock top (ledge 50, floor 195 below past it): running off the edge plays hurdle_down_low_50 (ledge class 8,
// chart S724 -> S996) and ends on the lower floor.
TEST(GameReal, PoolRockStepDown) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto [clips, end] = obstacleRun("l4_pool", {-3993.2f, 4255.0f, 3100}, -47.7f, 90, forward());
    EXPECT_TRUE(std::find(clips.begin(), clips.end(), "hurdle_down_low_50") != clips.end()) << ::testing::PrintToString(clips);
    EXPECT_LT(end.pos.z, 3069.5f - 150);
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

// l3_main_bedroom, the new game's start (tru.exe = ours + (9622, 333, -2977)): tru.exe under Wine (`wmemt trace`,
// openunderworld-work/jumpfid) leaves the floor at 48.04 up (standing), 49.97 up + 40.00 along the stick, and 42.53
// up + 65.83 forward (running), units per 30 Hz frame: rise 201.6 / 218.1 / 157.1 (exact for g 5.72443), air time
// 0.56 / 0.58 / 0.50 s, running 981 from take-off to touch-down (samples every ~17 ms).
TEST(GameReal, BedroomJumpsMatchTruTraces) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    struct Flight { vec3 v; float rise, dist, air; };
    const auto jump = [&](int mode) { // 0 standing, 1 with the stick, 2 running
        auto w = unitWorld(bf, "l3_main_bedroom", {{-4264.2f, 5958.0f, 86.6f}, glm::radians(-90.0f)});
        const auto& s = w.player().state();
        for (int i = 0; i < 30; ++i) w.tick({});
        const float yaw0 = w.camera().yaw;
        for (int i = 0; i < (mode == 2 ? 54 : 0); ++i) w.tick(fixedCamera(w, forward(), yaw0));
        Actions a = mode ? fixedCamera(w, forward(), yaw0) : Actions{};
        a.jump = true;
        Flight f{};
        int t0 = -1;
        vec3 p0(0);
        for (int i = 0; i < 200; ++i) {
            const bool air = s.state == State::Jump;
            w.tick(a);
            if (s.state == State::Jump && !air) t0 = i, p0 = s.pos, f.v = s.vel / 30.0f;
            if (t0 >= 0) f.rise = std::max(f.rise, s.pos.z - p0.z);
            if (t0 >= 0 && s.state != State::Jump) {
                f.dist = glm::length(glm::vec2(s.pos - p0)), f.air = float(i - t0) / 60;
                break;
            }
        }
        return f;
    };
    Flight f = jump(0);
    EXPECT_NEAR(f.v.z, 48.04f, 0.01f);
    EXPECT_NEAR(f.rise, 201.6f, 2);
    EXPECT_NEAR(f.air, 0.56f, 0.02f);
    f = jump(1);
    EXPECT_NEAR(f.v.z, 49.97f, 0.01f);
    EXPECT_NEAR(glm::length(glm::vec2(f.v)), 40.0f, 0.01f);
    EXPECT_NEAR(f.rise, 218.1f, 2);
    EXPECT_NEAR(f.air, 0.58f, 0.02f);
    f = jump(2);
    EXPECT_NEAR(f.v.z, 42.53f, 0.01f);
    EXPECT_NEAR(glm::length(glm::vec2(f.v)), 65.83f, 0.01f);
    EXPECT_NEAR(f.rise, 157.1f, 2);
    EXPECT_NEAR(f.air, 0.50f, 0.02f);
    EXPECT_NEAR(f.dist, 981, 35);
}

// Free leaps off a hang (nothing to jump to), l3_main_bedroom's ledge 973 (the prologue wall, caught from a standing
// jump as in tru.exe). tru.exe under Wine: back, ledgeb_jump_back first turns her round carrying the root 61.6 out and
// 32.2 up, then she leaves at 53.73 out + 34.77 up a frame: rise 105.6, touch-down on the corridor floor (258.4
// below) 0.578 s and 932 later; right, ledgeb_jump_right carries her ~122 aside and ~100 up, turning her a quarter
// round, then 70.80 aside + 34.77 up.
TEST(GameReal, HangLeapsMatchTruTraces) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    struct Leap { vec3 takeoff, v; float rise, dist, air; };
    const auto leap = [&](Actions stick, const char* clip) {
        Leap l{};
        auto w = unitWorld(bf, "l3_main_bedroom", {{-8484.3f, -4330, 244}, glm::radians(-90.0f)});
        const auto& s = w.player().state();
        Actions jump;
        jump.jump = true;
        for (int i = 0; i < 120 && !(s.state == State::Hang && s.clip == "ledgeb_idle"); ++i) w.tick(i == 5 ? jump : Actions{});
        EXPECT_EQ(s.state, State::Hang);
        if (s.state != State::Hang) return l;
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 973);
        w.restore(s, s.yaw, -0.25f); // camera behind her
        w.tick({});
        const vec3 hang = s.pos;
        const glm::vec2 out = -glm::vec2(std::cos(s.yaw), std::sin(s.yaw)), right(std::sin(s.yaw), -std::cos(s.yaw));
        stick.jump = true;
        w.tick(stick);
        EXPECT_EQ(s.state, State::Jump);
        EXPECT_EQ(s.clip, clip);
        vec3 p0 = s.pos, v0 = s.vel; // the launch: the last tick of the take-off clip (the next one flies)
        for (int i = 0; i < 60 && s.clip == clip; ++i) p0 = s.pos, v0 = s.vel, w.tick({});
        const vec3 d = p0 - hang;
        l.takeoff = vec3(glm::dot(glm::vec2(d), out), glm::dot(glm::vec2(d), right), d.z);
        l.v = vec3(glm::dot(glm::vec2(v0), out), glm::dot(glm::vec2(v0), right), v0.z) / 30.0f;
        int t = 1;
        l.rise = s.pos.z - p0.z;
        for (; t < 120 && s.state == State::Jump; ++t) w.tick({}), l.rise = std::max(l.rise, s.pos.z - p0.z);
        l.dist = glm::length(glm::vec2(s.pos - p0)), l.air = float(t) / 60;
        return l;
    };
    Leap l = leap(forward(-1), "ledgeb_jump_back");
    EXPECT_NEAR(l.takeoff.x, 61.6f, 3);
    EXPECT_NEAR(l.takeoff.z, 32.2f, 3);
    EXPECT_NEAR(l.v.x, 53.73f, 0.01f);
    EXPECT_NEAR(l.v.z, 34.77f, 0.01f);
    EXPECT_NEAR(l.rise, 105.6f, 2);
    EXPECT_NEAR(l.air, 0.578f, 0.02f);
    EXPECT_NEAR(l.dist, 932, 35);
    Actions right;
    right.move.x = 1;
    l = leap(right, "ledgeb_jump_right");
    EXPECT_NEAR(l.takeoff.y, 122, 20);
    EXPECT_NEAR(l.takeoff.z, 100, 15);
    EXPECT_NEAR(l.v.y, 70.80f, 0.01f);
    EXPECT_NEAR(l.v.z, 34.77f, 0.01f);
}

// Leaning into hang jumps (l3_main_bedroom ledge 973). tru.exe under Wine: the stick held up keeps her in the up-lean
// state S434 (no climb, though the top is climbable), back in S456, right at the ledge's end S457 -> S449, until released.
// Ours: ledgeb_idle_to_jump_<way>_antic, its loop while held, _antic_to_idle when released; jump from the up lean climbs.
TEST(GameReal, HangJumpLeans) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l3_main_bedroom", {{-8484.3f, -4330, 244}, glm::radians(-90.0f)});
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 120 && !(s.state == State::Hang && s.clip == "ledgeb_idle"); ++i) w.tick(i == 5 ? jump : Actions{});
    ASSERT_EQ(s.clip, "ledgeb_idle");
    w.restore(s, s.yaw, -0.25f); // camera behind her
    const vec3 hang = s.pos;
    for (const auto& [stick, way] : {std::pair{forward(), "up"}, std::pair{forward(-1), "back"}}) {
        w.tick(stick);
        EXPECT_EQ(s.clip, "ledgeb_idle_to_jump_" + std::string(way) + "_antic");
        for (int i = 0; i < 90; ++i) w.tick(stick);
        EXPECT_EQ(s.clip, "ledgeb_jump_" + std::string(way) + "_antic") << "the lean loops while held";
        EXPECT_EQ(s.state, State::Hang);
        EXPECT_LT(glm::distance(s.pos, hang), 1.0f);
        w.tick({});
        EXPECT_EQ(s.clip, "ledgeb_jump_" + std::string(way) + "_antic_to_idle");
        for (int i = 0; i < 60; ++i) w.tick({});
        EXPECT_EQ(s.clip, "ledgeb_idle");
    }
    for (int i = 0; i < 60; ++i) w.tick(forward());
    Actions upJump = forward();
    upJump.jump = true;
    w.tick(upJump);
    EXPECT_EQ(s.state, State::ClimbUp) << "the climbable top: jump from the up lean climbs";
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
    for (int i = 0; i < 60 && s.state == State::PoleHang; ++i) w.tick({});
    ASSERT_EQ(s.state, State::HangJump);
    // tru.exe under Wine (1336 -> 1337): 23.5 frames in the air, the vertical take-off of query q33 (52.10 a frame)
    EXPECT_NEAR(s.flight, 23.5f / 30.0f, 0.02f);
    const float z0 = s.pos.z, vz0 = s.vel.z;
    float top = z0;
    int air = 0;
    for (; air < 120 && s.state == State::HangJump; ++air) {
        w.tick({});
        top = std::max(top, s.pos.z);
    }
    EXPECT_NEAR(air / 60.0, 23.5 / 30.0, 0.03);
    EXPECT_GT(top, z0 + 100) << "a high arc, not ours' old flat 60 (vz " << vz0 << ")";
    ASSERT_EQ(s.state, State::PoleHang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1337);
    EXPECT_NEAR(handsOf(w, w.anims().poleHandOffset).y, -19591, 1);
}

// l3_main_hall pole 948 (axis z 1861) -> ledge 946, the edge of a platform 1052 ahead and 152 under the axis (floor
// 1708.6). tru.exe under Wine (openunderworld-work/polefid2/mhsw.txt): the fling lands her on that edge 23.0 frames
// after she leaves, (35.9 forward, 74.1 up) a frame; q33 as it is (52.10 up) tops out ~230 short of the platform.
TEST(GameReal, MainHallPoleFlingLandsOnPlatform) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l3_main_hall", {{1042.6f, -3911.3f, 1300}, 0});
    game::PlayerState h;
    h.state = State::PoleHang, h.ledge = markupIndex(w, 948), h.ledgeT = 233.7f, h.ledgeSide = 1, h.clip = "HPole_idleHangPerp";
    ASSERT_GE(h.ledge, 0);
    w.restore(h, 0, -0.25f); // facing +x, camera behind her
    const auto& s = w.player().state();
    for (int i = 0; i < 10; ++i) w.tick({});
    Actions fling = forward();
    fling.jump = true;
    w.tick(fling);
    for (int i = 0; i < 60 && s.state == State::PoleHang; ++i) w.tick({});
    ASSERT_EQ(s.state, State::Jump);
    int air = 0;
    for (; air < 120 && s.state == State::Jump; ++air) w.tick({});
    EXPECT_NEAR(air / 60.0, 23.5 / 30.0, 0.04);
    for (int i = 0; i < 30; ++i) w.tick({});
    EXPECT_NE(s.state, State::Fall);
    EXPECT_GT(s.pos.x, 2094.6f); // on the platform, past its edge
    EXPECT_NEAR(s.pos.z, 1740, 40);
}

// l2_junglehub ledge 851 (z 7972): the top of a platform across a gap, 353 over the floor the run-up ends on (y ~9850).
// A running jump taking off ~1180 short of it falls into the gap at its plain 65.83 + 42.54 a frame; q26's arc (90.25 +
// 42.54 from 300 over the root) meets the ledge and the flight is re-aimed onto its hang (within the 1233 reach tru.exe
// showed under Wine on l4_temple ledge 1204; from the old start, 1554 short, the box catch is out of reach).
TEST(GameReal, JunglehubRunningJumpReachesHigherLedge) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l2_junglehub", {{-23937.1f, 9560.0f, 7629.2f}, glm::radians(90.0f)});
    game::PlayerState st = w.player().state();
    st.state = State::Run, st.clip = "ng_run";
    w.restore(st, st.yaw, -0.25f);
    const auto& s = w.player().state();
    Actions go = forward();
    go.jump = true;
    w.tick(forward());
    for (int i = 0; i < 150 && s.state != State::Hang && s.pos.z > 7000; ++i) w.tick(go);
    ASSERT_EQ(s.state, State::Hang) << game::stateName(s.state) << " at y " << s.pos.y << " z " << s.pos.z;
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 851);
}

// tru.exe under Wine (openunderworld-work/ledgecatch, runjump; Lara at rest facing the ledge, then jump): l4_temple
// ledge 1155's middle segment, 830 over the floor (line x -165.9 ours, hanging toward -x), is caught by a standing jump
// from 100 and 424 out, straight onto its hang in 11 ticks; from 425 out she only jumps (a plain 201.6 rise). Ledge
// 1166, 907 over the floor (line y -10429.3, hanging toward -y): caught from 100, not from 125 (over q2's box only the
// capsule round the hands' apex reaches it).
TEST(GameReal, StandingJumpCatchesHighLedgeWithinReach) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions jump;
    jump.jump = true;
    struct Case { uint16_t id; vec3 at; float yaw; bool caught; };
    const auto at1155 = [](float d) { return vec3(-165.9f - d, -11850.2f, 1750); };
    const auto at1166 = [](float d) { return vec3(-4178.7f, -10429.3f - d, -2780); };
    for (const Case& c : {Case{1155, at1155(100), 0, true}, Case{1155, at1155(424), 0, true}, Case{1155, at1155(425), 0, false},
                          Case{1166, at1166(100), glm::half_pi<float>(), true}, Case{1166, at1166(125), glm::half_pi<float>(), false}}) {
        auto w = unitWorld(bf, "l4_temple", {c.at, c.yaw});
        const auto& s = w.player().state();
        int leap = 0;
        for (int i = 0; i < 150; ++i) {
            w.tick(i == 20 ? jump : Actions{});
            leap += s.state == State::HangJump;
        }
        if (!c.caught) {
            EXPECT_NE(s.state, State::Hang) << c.id << " " << c.at.x << " " << c.at.y;
            continue;
        }
        ASSERT_EQ(s.state, State::Hang) << c.id << " " << c.at.x << " " << c.at.y;
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, c.id);
        EXPECT_NEAR(leap, 11, 1) << c.id; // 60 Hz ticks of the straight leap
    }
}

// tru.exe under Wine (openunderworld-work/runjump2: l4_temple ledge 1204, 782 over the floor, run along +y with the
// camera pinned behind her, jump at varied times): from a take-off 1222.9 short of the line she is caught by a re-aimed
// arc, 10.5 frames (21 ticks) in the air for take-offs 781..1218; from 1243.4 she only jumps (lands 285..300 short);
// from 614 and closer the straight rush (11 ticks at a constant speed onto the hang; ours counts the attach tick too).
TEST(GameReal, RunningJumpReachAndRush) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    constexpr float line = -10808.9f;
    struct Case { float takeoff; bool caught; int air; };
    for (const Case c : {Case{1200, true, 21}, Case{900, true, 21}, Case{500, true, 12}, Case{1280, false, 0}}) {
        auto w = unitWorld(bf, "l4_temple", {{-4901.6f, line - 2400, -2804.9f}, glm::half_pi<float>()});
        game::PlayerState st = w.player().state();
        st.state = State::Run, st.clip = "ng_run";
        w.restore(st, st.yaw, -0.25f);
        const auto& s = w.player().state();
        float take = 0;
        int air = 0;
        bool pressed = false;
        for (int i = 0; i < 400 && s.state != State::Hang; ++i) {
            Actions a = forward();
            // the take-off comes ~45 units after the press (the take-off clip), so press that much sooner
            if (!pressed && line - s.pos.y <= c.takeoff + 45) pressed = true, a.jump = true;
            const State before = s.state;
            w.tick(pressed && s.state != State::Run && s.state != State::JumpPrep ? Actions{} : a);
            if (before == State::JumpPrep && s.state != State::JumpPrep) take = line - s.pos.y;
            air += s.state == State::Jump || s.state == State::HangJump;
        }
        EXPECT_NEAR(take, c.takeoff, 45) << c.takeoff;
        if (!c.caught) {
            EXPECT_NE(s.state, State::Hang) << c.takeoff;
            continue;
        }
        ASSERT_EQ(s.state, State::Hang) << c.takeoff;
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1204);
        EXPECT_NEAR(air, c.air, 1) << c.takeoff;
    }
}

// tru.exe under Wine (openunderworld-work/jumpaim): a standing jump on the far side of l3_main_hall ledge 944 (it hangs
// toward -x; Lara 113 behind its line on +x, facing it) rises its plain 201.6 and catches nothing.
TEST(GameReal, StandingJumpBehindLedgeCatchesNothing) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l3_main_hall", {{1771.0f, -1182.2f, 0}, glm::radians(180.0f)});
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    float top = -1e9f;
    for (int i = 0; i < 120; ++i) {
        w.tick(i == 20 ? jump : Actions{});
        top = std::max(top, s.pos.z);
    }
    EXPECT_NE(s.state, State::Hang);
    EXPECT_NE(s.state, State::HangJump);
    EXPECT_NEAR(top, -37.6f + 201.6f, 25);
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
    auto w = makeWorld(tallPoolMesh(), {1000, 0, 400}); // she swims against the far side without climbing out
    for (int i = 0; i < 90; ++i) w->tick({});
    const float ox = w->water().originX(), oy = w->water().originY();
    float peak = 0;
    for (int i = 0; i < 300; ++i) w->tick(forward()), peak = std::max(peak, maxRipple(w->water()));
    EXPECT_GT(peak, 0.5f) << "surface swimming leaves a wake";
    EXPECT_TRUE(w->water().originX() != ox || w->water().originY() != oy) << "the field follows Lara";
    EXPECT_NEAR(std::remainder(w->water().originX(), game::WaveField::kCell), 0.0f, 1e-2f);
    EXPECT_TRUE(w->water().field().wall({-5, 0})) << "the pool side is a wall";
    EXPECT_FALSE(w->water().field().wall({1000, 0}));
}

// ---- the wave field itself (game::WaveField): Joan of Arc's tests/engine/water_waves_test.cpp in TRU units ----
namespace {
using glm::vec2;
constexpr float S = game::kWaterScale, kDt = game::WaveField::kStep;
using Wet = game::WaveField::Wet;

game::WaveField waveField(const Wet& wet = [](vec2) { return true; }) {
    game::WaveField f;
    f.follow({0, 0}, wet);
    return f;
}
void runWaves(game::WaveField& f, float seconds) {
    for (int i = 0; i < int(seconds / kDt + 0.5f); ++i) f.step(kDt);
}
float waveEnergy(const game::WaveField& f, const std::function<bool(vec2)>& in) {
    const auto h = f.heights();
    constexpr int n = game::WaveField::kN;
    float e = 0;
    for (int i = 0; i < n * n; ++i)
        if (in(f.origin() + (vec2(i % n, i / n) + 0.5f) * game::WaveField::kCell)) e += h[size_t(i)] * h[size_t(i)];
    return e;
}
float wavePeak(const game::WaveField& f) {
    float m = 0;
    for (float h : f.heights()) m = std::max(m, std::abs(h));
    return m;
}
} // namespace

// Two splashes 400 Joan units apart: the field is linear, so at the midpoint both together are exactly the sum of each
// alone; equal kicks double there (constructive), opposite ones cancel (destructive).
TEST(WaveField, Superposition) {
    const vec2 p1 = vec2(-198, 2) * S, p2 = vec2(202, 2) * S, mid = vec2(2, 2) * S; // mirror images about a cell centre
    auto a = waveField(), b = waveField(), ab = waveField(), anti = waveField();
    a.kick(p1, 6 * S, 24 * S), b.kick(p2, 6 * S, 24 * S);
    ab.kick(p1, 6 * S, 24 * S), ab.kick(p2, 6 * S, 24 * S);
    anti.kick(p1, 6 * S, 24 * S), anti.kick(p2, -6 * S, 24 * S);
    float worst = 0, single = 0, both = 0, cancel = 0;
    for (int i = 0; i < 240; ++i) {
        for (auto* f : {&a, &b, &ab, &anti}) f->step(kDt);
        worst = std::max(worst, std::abs(ab.height(mid) - a.height(mid) - b.height(mid)));
        single = std::max(single, std::abs(a.height(mid)));
        both = std::max(both, std::abs(ab.height(mid)));
        cancel = std::max(cancel, std::abs(anti.height(mid)));
    }
    EXPECT_GT(single, 0.02f * S);
    EXPECT_LT(worst, 1e-4f * S);
    EXPECT_NEAR(both, 2 * single, 1e-3f * S);
    EXPECT_LT(cancel, 1e-4f * S);
}

// A shore: a splash near it keeps its energy on the water side (it reflects) where open water lets it run on; wall
// cells never move.
TEST(WaveField, ShoreReflects) {
    const auto water = [](vec2 p) { return p.x < 160 * S; };
    auto shore = waveField(water), open = waveField();
    for (auto* f : {&shore, &open}) f->kick(vec2(96, 4) * S, 6 * S, 24 * S);
    runWaves(shore, 2), runWaves(open, 2);
    const float kept = waveEnergy(shore, water), lost = waveEnergy(open, water);
    EXPECT_GT(kept, 1.5f * lost);
    EXPECT_EQ(waveEnergy(shore, [&](vec2 p) { return !water(p); }), 0.0f);
    EXPECT_TRUE(shore.wall(vec2(200, 0) * S));
    EXPECT_FALSE(shore.wall(vec2(100, 0) * S));
}

// A body is a wall for the step after it is placed: no water moves inside it. A kick at its feet, clipped by the body
// and a shore, still moves no water in or out (the pool level stays put).
TEST(WaveField, BodiesAreObstacles) {
    auto f = waveField([](vec2 p) { return p.x < 40 * S; });
    f.wade({0, 0}, {0, 0}, kDt);
    f.kick({0, 0}, 6 * S, 22 * S);
    float volume = 0;
    for (float h : f.heights()) volume += h;
    f.step(kDt);
    EXPECT_LT(std::abs(volume), 1e-3f * S * S);
    EXPECT_EQ(f.height({0, 0}), 0.0f);
    EXPECT_NE(f.height(vec2(-40, 0) * S), 0.0f);
}

// Damping: a full splash is gone after a few seconds, and the empty span tells the renderer the water is at rest.
TEST(WaveField, DampsToRest) {
    auto f = waveField();
    f.kick({0, 0}, 6 * S, 24 * S);
    const float start = wavePeak(f);
    runWaves(f, 1);
    const float one = wavePeak(f);
    runWaves(f, 3);
    const float four = wavePeak(f);
    runWaves(f, 6);
    EXPECT_GT(start, 5 * S);
    EXPECT_LT(one, 1.5f * S);
    EXPECT_LT(four, 0.05f * S);
    EXPECT_TRUE(f.heights().empty());
}

// Scrolling the window by whole cells keeps every wave at its world position.
TEST(WaveField, ScrollKeepsWaves) {
    auto still = waveField(), moved = waveField();
    for (auto* f : {&still, &moved}) f->kick(vec2(40, 40) * S, 6 * S, 24 * S), runWaves(*f, 1);
    moved.follow(vec2(37 * 8 + 3, -21 * 8) * S, [](vec2) { return true; });
    EXPECT_NE(moved.origin(), still.origin());
    runWaves(still, 0.5f), runWaves(moved, 0.5f);
    float worst = 0, seen = 0;
    for (float x = -160; x <= 240; x += 8)
        for (float y = -160; y <= 240; y += 8) {
            const vec2 p = vec2(x, y) * S;
            worst = std::max(worst, std::abs(still.height(p) - moved.height(p)));
            seen = std::max(seen, std::abs(still.height(p)));
        }
    EXPECT_GT(seen, 0.1f * S);
    EXPECT_EQ(worst, 0.0f);
}

// A body wading slower than the waves (40 / 75 Joan units/s; they run at 80) at 240 fps leaves ~2-unit (x S) wake
// waves. One outrunning them is neither a wall nor a wake source (its wake would be a Mach cone).
TEST(WaveField, WakeHeight) {
    for (const float speed : {40 * S, 75 * S}) {
        auto f = waveField();
        float top = 0;
        vec2 p = vec2(-250, 0) * S;
        for (int i = 0; i < 360; ++i) {
            const float dt = 1.0f / 240.0f;
            f.step(dt);
            p.x += speed * dt;
            f.wade(p, {speed, 0}, dt);
            top = std::max(top, wavePeak(f));
        }
        EXPECT_GT(top, 0.5f * S) << speed;
        EXPECT_LT(top, 2.5f * S) << speed;
    }
    auto f = waveField();
    f.kick(vec2(-40, 0) * S, 6 * S, 22 * S);
    f.wade({0, 0}, {game::WaveField::kSpeed, 0}, kDt);
    f.step(kDt);
    EXPECT_NE(f.height({0, 0}), 0.0f) << "a fast body is no wall";
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
    for (int i = 0; i < 30; ++i) w->tick({}); // treading: the head is out (root swimSurfaceDepth under the surface)
    EXPECT_GT(w->air(), low) << "surfacing refills the meter";
    // tru.exe 0x755230: 4000 frames of breath (133.3 s), then 10 % of max health 20 frames after it ran out
    for (int i = 0; i < 300 && w->air() < 1.0f; ++i) w->tick({}); // full again in 60 frames
    ASSERT_EQ(w->air(), 1.0f);
    int ticks = 0;
    for (; ticks < 60 * 200 && w->health().value() >= w->health().max(); ++ticks) w->tick(dive);
    EXPECT_EQ(w->air(), 0.0f);
    EXPECT_NEAR(w->health().value(), 0.9f * w->health().max(), 1e-3f) << "an empty meter drowns Lara";
    EXPECT_GT(ticks / 60.0f, 133.0f);
    EXPECT_LT(ticks / 60.0f, 136.5f); // + the dive back down and the 0.67 s period
}

// ---- free climbing (src/game/Climb) ----
namespace {
// A rock wall at x = 400 (z 0..1200, facing -x) with a floor on top. `holds`: a column of free-climb holds
// (markup type 16 triangles, 20 off the rock) at y = 0 from z 450 to the top, as the levels place them.
// A rock wall at x = 400 (top 1200, a ledge along its edge) with a column of free-climb holds 158 apart (the hold
// grid every DICE transition's root motion steps), laid out as the levels' are: point 0 low in the middle, 1 high left,
// 2 high right (Lara faces +X: her left is +Y).
CollisionMesh climbMesh(bool holds, std::vector<uint8_t> types = {16, 16, 16, 16, 16}) {
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 1200}, {400, -2000, 1200});
    addQuad(m, {400, -2000, 1200}, {2000, -2000, 1200}, {2000, 2000, 1200}, {400, 2000, 1200});
    m.markup.push_back({1, 1, {{400, 2000, 1200}, {400, -2000, 1200}}}); // hanging to its right (-X), as retail ledges
    if (holds)
        for (size_t i = 0; i < types.size(); ++i) {
            const float z = 350 + 158.0f * float(i);
            m.markup.push_back({types[i], uint16_t(z), {{380, 0, z}, {380, 70, z + 144}, {380, -70, z + 174}}});
        }
    return m;
}
// The free-climb tests need lara.drm's DICE clips (their root motion is the step between holds).
std::unique_ptr<game::World> climbWorld(CollisionMesh m, vec3 spawn) {
    static const auto anims = [] {
        const cdc::BigfileSet bf(gameDir());
        return game::loadLaraAnims(bf);
    }();
    m.build();
    return std::make_unique<game::World>(std::move(m), anims, game::Spawn{spawn, 0});
}
} // namespace

// Pushing into the rock attaches at a hold (root at point 0 - 158.025, tru.exe 0x707C60); each push up steps one hold
// with DICE_B_to_B_Up's root motion; no hold above but the ledge on the up-jump query's arc (q31): pushing up leans into
// the jump (DICE_B_antic_Up), jump leaps (DICE_B_jump_up) onto it, hang, climb up.
TEST(GameReal, ClimbAttachClimbUpTopOut) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    auto w = climbWorld(climbMesh(true), {300, 0, 0});
    w->tick(forward()); // pushing into the rock attaches
    ASSERT_EQ(w->player().state().state, State::Climb);
    for (int i = 0; i < 120; ++i) w->tick({}); // the attach clip finishes
    EXPECT_EQ(w->player().state().clip, "DICE_B_Idle");
    EXPECT_NEAR(glm::distance(w->player().state().pos, vec3(380, 0, 350 - 158.025f)), 0, 1e-2f) << "at hold 0";
    const float z0 = w->player().state().pos.z;
    bool up = false;
    for (int i = 0; i < 62; ++i) {
        w->tick(forward()); // into the wall = climb up
        up |= w->player().state().clip == "DICE_B_to_B_Up";
    }
    EXPECT_TRUE(up);
    EXPECT_NEAR(w->player().state().pos.z, z0 + 158.0f, 1.0f) << "one hold up";
    EXPECT_NEAR(w->player().state().pos.x, 380.0f, 1e-2f);
    bool lean = false;
    for (int i = 0; i < 400 && !lean; ++i) {
        w->tick(forward());
        lean = w->player().state().clip == "DICE_B_antic_Up";
    }
    ASSERT_TRUE(lean) << "no hold above, the ledge in the jump's reach: the lean";
    for (int i = 0; i < 30; ++i) w->tick(forward());
    EXPECT_EQ(w->player().state().state, State::Climb) << "no jump without the button";
    Actions upJump = forward();
    upJump.jump = true;
    w->tick(upJump);
    EXPECT_EQ(w->player().state().clip, "DICE_B_jump_up");
    EXPECT_EQ(w->player().state().state, State::HangJump);
    for (int i = 0; i < 300 && w->player().state().state != State::Hang; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::Hang);
    for (int i = 0; i < 60; ++i) w->tick({}); // ledgeb_attach_U
    Actions jump;
    jump.jump = true;
    w->tick(jump); // pull up
    for (int i = 0; i < 300 && w->player().state().state != State::Idle; ++i) w->tick({});
    EXPECT_EQ(w->player().state().state, State::Idle);
    EXPECT_NEAR(w->player().state().pos.z, 1200.0f, 1.0f) << "climbed onto the wall's top";
}

TEST(GameReal, ClimbJumpOff) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    auto w = climbWorld(climbMesh(true), {300, 0, 0});
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

TEST(GameReal, ClimbNeedsHolds) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    auto w = climbWorld(climbMesh(false), {300, 0, 0});
    w->tick(forward());
    EXPECT_NE(w->player().state().state, State::Climb) << "rock without holds is not climbable";
    auto far = climbWorld(climbMesh(true), {300, 1500, 0}); // same wall, 1500 from the hold column
    for (int i = 0; i < 10; ++i) far->tick(forward());
    EXPECT_NE(far->player().state().state, State::Climb) << "only the rock around the holds is climbable";
}

// Pushing toward no hold reaches for one (DICE_B_Reach_Right) without moving.
TEST(GameReal, ClimbStaysWithinHolds) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    auto w = climbWorld(climbMesh(true), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    for (int i = 0; i < 120; ++i) w->tick({});
    const vec3 p0 = w->player().state().pos;
    Actions right;
    right.move.x = 1;
    for (int i = 0; i < 120; ++i) w->tick(right);
    EXPECT_EQ(w->player().state().state, State::Climb);
    EXPECT_EQ(w->player().state().clip, "DICE_B_Reach_Right");
    EXPECT_EQ(w->player().state().pos, p0) << "stays at the hold";
    w->tick({});
    EXPECT_EQ(w->player().state().clip, "DICE_B_Idle");
}

// Hand stance from the hold's markup type (16..20 = B / L / LW / R / RW); each step plays that pair's
// DICE_<from>_to_<to>_<dir>, then the new stance's idle when the stick is released.
TEST(GameReal, ClimbStanceFollowsHolds) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    auto w = climbWorld(climbMesh(true, {19, 17, 19, 18, 20, 16}), {300, 0, 0});
    w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    EXPECT_EQ(w->player().state().climbStance, 1) << "hold 1 (type 17): DICE_L_ground_attach's rise 317 reaches it";
    EXPECT_EQ(w->player().state().clip, "DICE_L_ground_attach");
    for (int i = 0; i < 120; ++i) w->tick({});
    EXPECT_EQ(w->player().state().clip, "DICE_L_Idle");
    std::vector<std::string> clips;
    std::vector<int> stances{1};
    for (int i = 0; i < 400 && stances.size() < 5; ++i) {
        w->tick(forward());
        const auto& s = w->player().state();
        if (clips.empty() || clips.back() != s.clip) clips.push_back(s.clip);
        if (stances.back() != s.climbStance) stances.push_back(s.climbStance);
    }
    for (int i = 0; i < 300 && w->player().state().climbWarp; ++i) w->tick({}); // the last step finishes
    EXPECT_EQ(stances, (std::vector<int>{1, 3, 2, 4, 0})) << "L, R, LW, RW, B going up the column";
    for (const char* c : {"DICE_L_to_R_UpL", "DICE_R_to_LW_UpR", "DICE_Lw_to_Rw_UpL", "DICE_Rw_to_B_UpR"})
        EXPECT_NE(std::find(clips.begin(), clips.end(), c), clips.end()) << c;
    EXPECT_EQ(w->player().state().clip, "DICE_B_Idle");
}

// Every hold-to-hold clip lara.drm has, spelled as it spells them (Lw/LW, Rw/RW).
TEST(Game, DiceTransitionNames) {
    int names = 0;
    for (const auto n : game::kLaraClipNames) names += n.find("_to_") != std::string_view::npos && n.substr(0, 4) == "DICE";
    EXPECT_EQ(names, 181);
}

// l2_cliffclimb's hold wall (the ClimbOnlyAtHolds wall), its whole route: 18 holds through all stances but RW, each
// step a lara.drm transition clip, then the leap up onto the ledge. The root moves smoothly (no per-tick
// snapping: before 2026-10-31 she slid at a fixed 240 units/s and was re-pinned to each rock face, its heading
// jumping with the face normals) and is exactly at a hold (point 0 - 158.025) whenever a step ends.
TEST(GameReal, CliffclimbStances) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    col.build();
    game::World w(std::move(col), anims, {{-23835, -3600, 300}, glm::radians(90.0f)});
    for (int t = 0; t < 60 && w.player().state().state != State::Climb; ++t) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Climb);
    std::set<int> stances, holds;
    std::vector<std::string> transitions;
    // Smoothness of what is seen: the chest (spine bone 14) in the world. Transition clips move the root in ramps their
    // pose compensates, so the root alone is not the measure.
    const auto chest = [&] {
        const auto& s = w.player().state();
        return vec3(w.player().modelMatrix(s.pos, s.yaw) * w.pose(1.0)[14][3]);
    };
    float maxStep = 0, maxChange = 0, maxTurn = 0;
    vec3 prev = chest(), prevStep(0);
    float prevYaw = w.player().state().yaw;
    // The wall's route, hold by hold (markup indices): up the left column, right around the corner, up the corner column,
    // left along the top row to hold 5 under ledge 45. The stick points at the next hold as seen on the rock.
    const std::vector<int> plan = {21, 11, 38, 12, 7, 19, 17, 13, 18, 14, 16, 15, 6, 4, 40, 8, 3, 9, 5};
    size_t next = 1;
    std::vector<int> route{w.player().state().climbHold};
    float maxRootChange = 0;
    vec3 prevRoot = w.player().state().pos, prevRootStep(0);
    const float z0 = w.player().state().pos.z;
    float minCamera = 1e9f; // orbit camera distance from its pivot (bone 108 + 79.01 climbing, ~95 off the rock)
    for (int t = 0; t < 3600 && w.player().state().state == State::Climb; ++t) {
        const auto& s = w.player().state();
        if (next < plan.size() && s.climbHold == plan[next] && !s.climbWarp) ++next;
        Actions a;
        if (next < plan.size()) {
            const vec3 d = w.collision().markup[size_t(plan[next])].points[0] - w.collision().markup[size_t(s.climbHold)].points[0];
            const vec3 n = s.climbNormal, right = glm::normalize(glm::cross(-n, vec3(0, 0, 1)));
            a.move = glm::normalize(vec2(glm::dot(d, right), d.z));
        } else {
            a.move = {0, 1}; // up from the last hold: the lean toward ledge 45, then the jump
            a.jump = s.clip == "DICE_B_antic_Up";
        }
        w.tick(a);
        if (s.climbHold != route.back() && s.climbHold >= 0) route.push_back(s.climbHold);
        const vec3 c = chest(), step = c - prev, rootStep = s.pos - prevRoot;
        if (t > 0 && s.state == State::Climb) { // (the first tick starts the attach crossfade from the standing pose)
            maxStep = std::max(maxStep, glm::length(step));
            maxChange = std::max(maxChange, glm::length(step - prevStep));
            maxRootChange = std::max(maxRootChange, glm::length(rootStep - prevRootStep));
        }
        prevRoot = s.pos, prevRootStep = rootStep;
        maxTurn = std::max(maxTurn, std::abs(std::remainder(s.yaw - prevYaw, 6.2831853f)));
        prev = c, prevStep = step, prevYaw = s.yaw;
        stances.insert(s.climbStance);
        holds.insert(s.climbHold);
        EXPECT_NE(anims.find(s.clip), nullptr) << s.clip;
        minCamera = std::min(minCamera, glm::distance(w.camera().position, w.camera().target));
        if (!s.climbWarp && s.state == State::Climb)
            EXPECT_LT(glm::distance(s.pos, w.collision().markup[size_t(s.climbHold)].points[0] - vec3(0, 0, 158.025f)), 1e-2f);
        if (s.clip.find("_to_") != std::string::npos && (transitions.empty() || transitions.back() != s.clip)) transitions.push_back(s.clip);
    }
    const float rise = w.player().state().pos.z - z0;
    int leap = 0; // tru.exe under Wine: up + jump at hold 5 rushes straight onto ledge 45's hang in 11 ticks
    for (int t = 0; t < 120 && w.player().state().state == State::HangJump; ++t) w.tick({}), ++leap;
    EXPECT_NEAR(leap, 12, 1) << "the straight rush: 11 ticks of flight, then the attach tick";
    std::printf("cliffclimb: %zu holds, %zu stances, rise %.0f, chest max step %.2f/tick, max step change %.2f, "
                "root max step change %.2f, max turn %.4f rad/tick, camera min %.0f; transitions:",
                holds.size(), stances.size(), rise, maxStep, maxChange, maxRootChange, maxTurn, minCamera);
    for (const auto& c : transitions) std::printf(" %s", c.c_str());
    std::printf("\nroute:");
    for (const int h : route) std::printf(" %d", h);
    std::printf(" -> %s\n", std::string(game::stateName(w.player().state().state)).c_str());
    EXPECT_EQ(route, plan) << "every step of the route has a transition clip";
    EXPECT_EQ(w.player().state().state, State::Hang) << "DICE_B_jump_up onto ledge 45";
    EXPECT_EQ(w.player().state().ledge, 45);
    EXPECT_GE(stances.size(), 4u);
    EXPECT_GT(rise, 1800.0f);
    EXPECT_LT(maxStep, 25.0f) << "no snapping (the attach clip's leap moves the chest up to ~19 units a tick)";
    EXPECT_LT(maxChange, 16.0f) << "no pops (a settle clip after a step crossfaded the chest by up to 45 units a tick)";
    EXPECT_LT(maxRootChange, 10.0f) << "the root follows the clips' root motion";
    EXPECT_LT(maxTurn, 0.02f) << "the heading turns smoothly (re-pinned to each face it jumped by up to 0.067 rad)";
    EXPECT_GT(minCamera, 1000.0f) << "the camera stays back (its probe started touching the rock and collapsed onto the "
                                     "pivot: position = target, a NaN view, the screen white)";
}

// The same hold-to-hold route at 30 / 60 / 144 Hz render rates: the fixed 60 Hz simulation steps identically, so the
// transitions, the route and the final state come out bit for bit, ending on ledge 45 (DICE_B_jump_up, ledgeb_attach_U).
TEST(GameReal, CliffclimbIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    auto base = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    base.build();
    const std::vector<int> plan = {21, 11, 38, 12, 7, 19, 17, 13, 18, 14, 16, 15, 6, 4, 40, 8, 3, 9, 5};
    struct Run { game::PlayerState end; std::vector<std::string> clips; };
    const auto run = [&](double renderHz) {
        game::World w(collision::CollisionMesh(base), anims, {{-23835, -3600, 300}, glm::radians(90.0f)});
        engine::FrameClock clock(game::kTickSeconds);
        size_t next = 1;
        Run r;
        const auto done = [&] { return w.ticks() >= 4000 || (w.player().state().state == State::Hang && w.player().state().clip == "ledgeb_idle"); };
        while (!done())
            for (int n = clock.advance(1.0 / renderHz); n > 0 && !done(); --n) {
                const auto& s = w.player().state();
                Actions a;
                if (s.state != State::Climb) a = w.ticks() < 60 ? forward() : Actions{};
                else {
                    if (next < plan.size() && s.climbHold == plan[next] && !s.climbWarp) ++next;
                    if (next < plan.size()) {
                        const vec3 d = w.collision().markup[size_t(plan[next])].points[0] - w.collision().markup[size_t(s.climbHold)].points[0];
                        const vec3 right = glm::normalize(glm::cross(-s.climbNormal, vec3(0, 0, 1)));
                        a.move = glm::normalize(vec2(glm::dot(d, right), d.z));
                    } else {
                        a.move = {0, 1};
                        a.jump = s.clip == "DICE_B_antic_Up";
                    }
                }
                w.tick(a);
                if (r.clips.empty() || r.clips.back() != w.player().state().clip) r.clips.push_back(w.player().state().clip);
            }
        r.end = w.player().state();
        return r;
    };
    const Run a = run(30), b = run(60), c = run(144);
    EXPECT_EQ(a.end.state, State::Hang);
    EXPECT_EQ(a.end.ledge, 45);
    EXPECT_TRUE(std::ranges::find(a.clips, "DICE_B_jump_up") != a.clips.end());
    EXPECT_GT(std::ranges::count_if(a.clips, [](const std::string& n) { return n.find("_to_") != std::string::npos; }), 15);
    for (const Run* o : {&b, &c}) {
        EXPECT_EQ(o->clips, a.clips);
        EXPECT_EQ(o->end.pos, a.end.pos);
        EXPECT_EQ(o->end.yaw, a.end.yaw);
        EXPECT_EQ(o->end.clipTime, a.end.clipTime);
    }
}

// Caught falling too fast for a firm catch (faster than grabMinVz, up to twice: as a ledge's saving grab [I]) the
// stance's bad attach holds: DICE_B_air_attach_bad_start onto the hold, its loop for the saving-grab time; jump pulls
// her back up (DICE_B_air_attach_bad_end, then DICE_B_Idle), else she lets go. A slow fall catches firmly.
TEST(GameReal, ClimbBadAttach) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto mesh = [](float z) {
        CollisionMesh m;
        addFloor(m, -2000, -2000, 2000, 2000, 0);
        addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 3000}, {400, -2000, 3000});
        m.markup.push_back({16, 350, {{380, 0, z}, {380, 70, z + 144}, {380, -70, z + 174}}});
        return m;
    };
    const vec3 anchor(380, 0, 350 - 158.025f);
    for (const bool pull : {true, false}) {
        auto w = climbWorld(mesh(350), {300, 0, 1400});
        const auto& s = w->player().state();
        float vz = 0;
        for (int i = 0; i < 120 && s.state != State::Climb; ++i) vz = s.vel.z, w->tick(forward());
        ASSERT_EQ(s.state, State::Climb);
        EXPECT_LT(vz, w->player().tuning().grabMinVz);
        EXPECT_EQ(s.clip, "DICE_B_air_attach_bad_start");
        EXPECT_GT(s.savingGrab, 0);
        for (int i = 0; i < 45; ++i) w->tick({}); // the start clip (0.57 s) carries her onto the hold
        EXPECT_EQ(s.clip, "DICE_B_air_attach_bad_loop");
        EXPECT_NEAR(glm::distance(s.pos, anchor), 0, 1e-2f);
        if (pull) {
            Actions jump;
            jump.jump = true;
            w->tick(jump);
            EXPECT_EQ(s.clip, "DICE_B_air_attach_bad_end");
            for (int i = 0; i < 60; ++i) w->tick({});
            EXPECT_EQ(s.state, State::Climb);
            EXPECT_EQ(s.clip, "DICE_B_Idle");
            EXPECT_EQ(s.savingGrab, 0);
            EXPECT_NEAR(glm::distance(s.pos, anchor), 0, 1e-2f);
        } else {
            for (int i = 0; i < 150 && s.state == State::Climb; ++i) w->tick({});
            EXPECT_NE(s.state, State::Climb) << "let go when the saving-grab time ran out";
        }
    }
    auto w = climbWorld(mesh(1350), {300, 0, 1450}); // a short fall onto a high hold
    for (int i = 0; i < 120 && w->player().state().state != State::Climb; ++i) w->tick(forward());
    ASSERT_EQ(w->player().state().state, State::Climb);
    EXPECT_EQ(w->player().state().clip, "DICE_B_air_attach_M");
    EXPECT_EQ(w->player().state().savingGrab, 0);
}

// l2_cliffclimb's hold wall carries a level camera record (type 3, 2 Hermite points, 11 holds, 300 deg/s, force 5 x
// 0.2): holds list in the record's order (hold id 900 second, 901 fourth: tru.exe under Wine put the camera point at
// u = 0.1 / 0.3 there). Idle on the wall, after 5 s without look input the camera turns 1.0 deg/s toward the record's
// point and its distance grows 3.16 units/s toward the point's (Wine: 1.0 deg/s at hold 908).
TEST(GameReal, CliffclimbCameraRecord) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    col.build();
    ASSERT_TRUE(col.markup[11].camera);
    const auto& r = *col.markup[11].camera;
    EXPECT_EQ(col.markup[11].id, 900);
    EXPECT_EQ(r.type, 3u);
    EXPECT_EQ(r.markups, (std::vector<uint16_t>{910, 900, 927, 901, 896, 908, 906, 902, 907, 903, 905}));
    ASSERT_EQ(r.pos.size(), 2u);
    EXPECT_NEAR(r.pos[0].x, -22592.1f, 0.1f);
    EXPECT_NEAR(r.angleSpeed, 300.0f, 1e-3f);
    EXPECT_TRUE(r.distance);
    game::World w(std::move(col), game::loadLaraAnims(bf), {{-23835, -3600, 300}, glm::radians(90.0f)});
    for (int t = 0; t < 60 && w.player().state().state != State::Climb; ++t) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Climb);
    const auto view = [&] {
        const vec3 d = w.camera().position - w.camera().target;
        return std::pair{glm::degrees(std::atan2(d.y, d.x)), glm::length(d)};
    };
    for (int t = 0; t < 9 * 60; ++t) w.tick({});
    const auto [yaw0, dist0] = view();
    for (int t = 0; t < 10 * 60; ++t) w.tick({});
    const auto [yaw1, dist1] = view();
    EXPECT_NEAR(yaw1 - yaw0, 10.0f, 0.5f) << "toward the record's point at 1 deg/s";
    EXPECT_NEAR(dist1 - dist0, 31.6f, 2.0f);
}

// DICE jumps on the same wall: pushing at a hold out of a move's reach leans toward it (DICE_<s>_antic_<dir>); jump
// plays DICE_<s>_jump_<dir> (its root motion carries her off the rock), flies to a catch point 168 out and 161 below
// the hold (DICE_attach_M's root motion; tru.exe: 167 / 163) and the air attach carries her onto it. From hold 18 the
// corner column has a 475 gap up to hold 10 (lara.drm q31's arc rises 480); from 10 hold 1 is 905 to the right (q29).
TEST(GameReal, CliffclimbDiceJumps) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    auto col = collision::loadLevelCollision(cdc::parseDrm(std::move(*bf.read("pc-w\\l2_cliffclimb.drm"))));
    col.build();
    game::World w(std::move(col), anims, {{-23835, -3600, 300}, glm::radians(90.0f)});
    for (int t = 0; t < 60 && w.player().state().state != State::Climb; ++t) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Climb);
    const std::vector<int> plan = {21, 11, 38, 12, 7, 19, 17, 13, 18};
    size_t next = 1;
    for (int t = 0; t < 3600 && next < plan.size(); ++t) {
        const auto& s = w.player().state();
        if (s.climbHold == plan[next] && !s.climbWarp) { ++next; continue; }
        const vec3 d = w.collision().markup[size_t(plan[next])].points[0] - w.collision().markup[size_t(s.climbHold)].points[0];
        const vec3 right = glm::normalize(glm::cross(-s.climbNormal, vec3(0, 0, 1)));
        Actions a;
        a.move = glm::normalize(vec2(glm::dot(d, right), d.z));
        w.tick(a);
    }
    for (int t = 0; t < 120; ++t) w.tick({});
    ASSERT_EQ(w.player().state().climbHold, 18);
    // Jumps to `hold` pushing `move`: antic while pushing, then the launch, flight, catch, air attach, idle at the hold.
    const auto jumpTo = [&](vec2 move, int hold, const char* antic, const char* launch, const char* attach) {
        Actions push;
        push.move = move;
        const int from = w.player().state().climbHold;
        for (int t = 0; t < 10; ++t) w.tick(push);
        EXPECT_EQ(w.player().state().clip, antic);
        EXPECT_EQ(w.player().state().climbHold, from) << "out of a move's reach";
        push.jump = true;
        w.tick(push);
        EXPECT_EQ(w.player().state().clip, launch);
        EXPECT_EQ(w.player().state().climbHold, hold);
        const vec3 p0 = w.player().state().pos;
        const auto* c = anims.find(launch);
        ASSERT_NE(c, nullptr);
        for (int t = 0; t < int(c->durationSeconds() * 60) - 1; ++t) w.tick({});
        EXPECT_EQ(w.player().state().clip, launch);
        EXPECT_GT(glm::distance(w.player().state().pos, p0), 60.0f) << "the launch clip's root motion moves her";
        for (int t = 0; t < 120 && w.player().state().clip != std::string(attach); ++t) w.tick({});
        EXPECT_EQ(w.player().state().clip, attach);
        const vec3 anchor = w.collision().markup[size_t(hold)].points[0] - vec3(0, 0, 158.025f);
        EXPECT_NEAR(w.player().state().pos.z, anchor.z - 161, 15.0f) << "caught below the hold";
        for (int t = 0; t < 120 && w.player().state().climbWarp; ++t) w.tick({});
        EXPECT_LT(glm::distance(w.player().state().pos, anchor), 1e-2f);
        w.tick({});
        EXPECT_EQ(w.player().state().state, State::Climb);
        EXPECT_NE(w.player().state().clip.find("Idle"), std::string::npos) << w.player().state().clip;
    };
    jumpTo({0, 1}, 10, "DICE_B_antic_Up", "DICE_B_jump_up", "DICE_R_air_attach_mid");
    jumpTo({1, 0}, 1, "DICE_R_antic_right", "DICE_R_jump_right", "DICE_R_air_attach_mid");
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
    Actions upRight; // up alone stops at hold 11 (hold 38 is up and right), as in tru.exe
    upRight.move = {0.7071f, 0.7071f};
    for (int t = 0; t < 180; ++t) w.tick(t < 60 ? forward() : upRight);
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

// The rope bends round geometry as tru.exe's (Player::wrapRope, 0x6C5490): swinging under a beam below the anchor, the
// rope catches on its corner, gets a bend 7.9 off the surface there and swings her on it (the hand never farther from
// the bend than the rope's free part; the total length kept); swinging back out past the corner the straight rope is
// clear again and the bend slides off and goes.
TEST(Game, GrappleRopeBendsRoundGeometry) {
    CollisionMesh m;
    addBox(m, {150, -2000, 1950}, {200, 2000, 2000}); // beam, its top outer corner at x 200, z 2000
    auto w = makeWorld(std::move(m), {1800, 0, 600});
    w->attachGrapple({0, 0, 3000}, 1);
    for (int i = 0; i < 300 && w->player().state().state == State::GrappleThrow; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::GrappleSwing);
    const float len = w->player().state().ropeLength;
    bool bent = false, straightAgain = false;
    for (int i = 0; i < 600 && !straightAgain; ++i) {
        w->tick({});
        const auto& s = w->player().state();
        ASSERT_EQ(s.state, State::GrappleSwing) << "tick " << i;
        EXPECT_FLOAT_EQ(s.ropeLength, len);
        const vec3 hand = s.pos + vec3(0, 0, 400);
        if (std::getenv("OU_GRAPPLE_TRACE") && i % 5 == 0)
            std::printf("tick %3d hand %.0f %.0f bends %d pivot %.1f %.1f\n", i, hand.x, hand.z, s.ropeBends,
                        game::ropePivot(s).x, game::ropePivot(s).z);
        EXPECT_LE(glm::distance(hand, game::ropePivot(s)), s.ropeLength - game::ropeBentLength(s) + 1.0f) << "tick " << i;
        if (s.ropeBends && !bent) {
            bent = true;
            EXPECT_NEAR(s.ropeBend[0].x, 200.0f, 12.0f);
            EXPECT_NEAR(s.ropeBend[0].z, 2000.0f, 12.0f);
            EXPECT_GT(glm::distance(s.ropeBend[0], vec3(200, s.ropeBend[0].y, 2000)), 5.0f) << "lifted off the surface";
            EXPECT_LT(hand.x, 600.0f) << "caught as the rope swings under the corner";
        }
        straightAgain = bent && !s.ropeBends;
    }
    EXPECT_TRUE(bent) << "the rope caught on the beam";
    EXPECT_TRUE(straightAgain) << "and came off it swinging back";
    EXPECT_GT(w->player().state().pos.x, 200.0f) << "back out past the corner";
}

// The grapple takes only *_ei_grapple_* anchors (World::grappleTarget): an enemy in front and in reach is no target,
// as in TRU (no enemy has a grapple clip or reaction); the anchor beside it is.
TEST(Game, GrappleIgnoresEnemies) {
    CollisionMesh m;
    addFloor(m, -3000, -3000, 3000, 3000, 0);
    auto w = makeWorld(std::move(m), {0, 0, 0});
    game::DummyTarget enemy;
    enemy.pos = {800, 0, 0};
    w->targets.push_back(&enemy);
    EXPECT_FALSE(w->grappleTarget());
    w->targets.clear();
}

// g_grapple's data as GrappleTuning's defaults [O]: range 4424.78 (14 m), rope 553.1..4424.78, 8 bends, stowed at
// Lara's marker 9 (+0xB5).
TEST(GameReal, GrappleTuningFromData) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto t = game::loadGrappleTuning(cdc::BigfileSet(gameDir()));
    ASSERT_TRUE(t);
    const game::GrappleTuning d;
    EXPECT_FLOAT_EQ(t->range, d.range);
    EXPECT_FLOAT_EQ(t->maxRope, d.maxRope);
    EXPECT_FLOAT_EQ(t->minRope, d.minRope);
    EXPECT_EQ(t->bends, d.bends);
    EXPECT_EQ(t->stowMarker, d.stowMarker);
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

// Grapple combat as TRU's: hanging on the rope the pistols come out one-handed (wp_handgun's
// combat_pistol_grapple_rappel_free_* clips) and the right gun alone fires.
TEST(GameReal, GrappleShootsOnTheRope) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), "l4_pool",
                  game::loadLaraAnims(bf), game::Spawn{{4362, -400, 2000}, 0});
    w.attachGrapple({4362, -70, 2607}, 5511);
    for (int i = 0; i < 90; ++i) w.tick({});
    ASSERT_EQ(w.player().state().state, State::GrappleSwing);
    Actions draw;
    draw.draw = true;
    w.tick(draw);
    w.tick({});
    EXPECT_EQ(w.combatLayerClip().first, "combat_pistol_grapple_rappel_free_equip");
    Actions fire;
    fire.fire = true;
    int leftGun = 0;
    for (int i = 0; i < 120; ++i) {
        w.tick(fire);
        for (const auto& s : w.combat().shots) leftGun += s.hand != 0;
    }
    EXPECT_EQ(w.player().state().state, State::GrappleSwing);
    EXPECT_TRUE(w.combat().drawn);
    EXPECT_GE(w.combat().shotsFired, 3u);
    EXPECT_EQ(leftGun, 0);
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
    for (int i = 0; i < 90 && w->player().state().state != State::GrappleRappel; ++i) w->tick(fixedCamera(*w, run, 0));
    ASSERT_EQ(w->player().state().state, State::GrappleRappel);
    EXPECT_EQ(w->player().state().clip, "ng_ground_to_wallrun_right");
    float minZ = 0;
    bool ran = false;
    for (int i = 0; i < 240 && w->player().state().state == State::GrappleRappel; ++i) {
        w->tick(fixedCamera(*w, run, 0));
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
        w.tick(fixedCamera(w, replay.at(i), -1.5707963f));
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

// Uber grapple pull targets (their uber state machines watch the pulled joint), tests/replays/grapple_pull.txt from a
// spot facing each: look up, throw, hold E for a second, and the target's pull handler runs (World::lastPulled); Lara
// lets go standing. (The naga statue 2457 is a swivelling grapple handle, an anchor: ProgressionReal.GrappleHandlesSwivel.)
TEST(GameReal, GrapplePullsUberTargets) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/grapple_pull.txt");
    struct Case { const char* unit; int32_t uid; vec3 spawn; float yawDeg; } cases[] = {
        {"l4_trailstart", 6695, {61161, -42611, 4948}, -11}, // ca_ei_lever_grapple_post
        {"l2_puzzleroom", 2982, {-23353, 42001, 1149}, -67},  // sa_ei_grapple_shiva
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.unit);
        game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), c.unit,
                      anims, game::Spawn{c.spawn, glm::radians(c.yawDeg)});
        w.script().setBigfile(&bf);
        // The shiva arms spawn in state 0 with uber action 0x1A (no grapple target). Walking into trigger volume 3023 runs
        // its script, which commands both arms into the state with action 0x19: offered. Lara walks in, then back.
        if (c.uid == 2982) {
            w.tick({}); // uber objects start their sections at spawn
            EXPECT_TRUE(w.uberGrappleOff(2982));
            const auto* volume = w.script().find(3023);
            ASSERT_TRUE(volume);
            game::PlayerState in;
            in.pos = volume->pos - vec3(0, 0, script::kTestPointHeight), in.yaw = glm::radians(c.yawDeg);
            w.restore(in, in.yaw, -0.25f);
            w.script().playerJumped();
            for (int i = 0; i < 30; ++i) w.tick({});
            EXPECT_FALSE(w.uberGrappleOff(2982)) << "volume 3023 offers arm 2982";
            EXPECT_FALSE(w.uberGrappleOff(2983)) << "and arm 2983";
            game::PlayerState back;
            back.pos = c.spawn, back.yaw = glm::radians(c.yawDeg);
            w.restore(back, back.yaw, -0.25f);
            w.script().playerJumped();
        }
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

// ---- ladders / pillars / perches / wall hug (src/game/Traverse) ----
namespace {
// The stick that pushes toward world direction `dir` under the World's current camera (World::tick's mapping).
Actions worldStick(const game::World& w, vec2 dir, Actions a = {}) {
    const float y = w.camera().yaw;
    a.move = {glm::dot(dir, vec2(std::sin(y), -std::cos(y))), glm::dot(dir, vec2(std::cos(y), std::sin(y)))};
    return a;
}
const collision::Markup* markupById(const game::World& w, int type, int id) {
    for (const auto& m : w.collision().markup)
        if (m.type == type && m.id == id) return &m;
    return nullptr;
}
} // namespace

// Rung alignment (tru.exe LadderRungAlignAnimTarget 0x74DCD0, player data +0x770 / +0x784 / +0x778 / +0x774): every
// climb clip ends with the root exactly on a rung, kRungTop + r * 110.619 below the top; off the top rung
// ladder_detach_ground_top's root motion (216.4 forward, 575.4 up) sets her on the floor above.
TEST(GameReal, LadderRungsClimbOffTop) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    CollisionMesh m;
    addFloor(m, -2000, -2000, 400, 2000, 0);
    addFloor(m, 400, -2000, 2000, 2000, 1200);
    m.markup.push_back({9, 1, {{400, 79, 1200}, {400, 79, 0}, {400, -79, 0}}}); // 158 wide across Y: the wall faces -X, toward Lara
    auto w = climbWorld(std::move(m), {300, 0, 0});
    const auto& s = w->player().state();
    w->tick(forward());
    ASSERT_EQ(s.state, State::Ladder);
    EXPECT_EQ(s.clip, "ladder_attach_ground_bottom");
    const auto rungZ = [](int r) { return 1200.0f - 575.221f - 110.619f * float(r); };
    for (int i = 0; i < 120 && s.clip != "ladder_idle"; ++i) w->tick({});
    ASSERT_EQ(s.clip, "ladder_idle");
    EXPECT_NEAR(s.pos.z, rungZ(5), 1e-3) << "lowest rung: trunc(1200 / 110.619) + 1 - 6";
    EXPECT_NEAR(s.pos.x, 400 - 110.619f, 1e-3) << "110.619 out from the wall";
    EXPECT_NEAR(s.pos.y, 0, 1e-3) << "on the middle of its width";
    std::set<int> rungs; // where each climb clip started: the rung the one before ended on
    for (int i = 0; i < 900 && s.state == State::Ladder; ++i) {
        w->tick(forward());
        if (s.clip == "ladder_climb_up" || s.clip == "ladder_detach_ground_top") {
            const float r = (1200.0f - 575.221f - s.climbFrom.z) / 110.619f;
            EXPECT_NEAR(r, std::round(r), 1e-3) << "between rungs at z " << s.climbFrom.z;
            rungs.insert(int(std::lround(r)));
        }
    }
    EXPECT_EQ(rungs, (std::set<int>{0, 1, 2, 3, 4, 5}));
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 1200.0f, 1.0f);
    EXPECT_NEAR(s.pos.x, 400 - 110.619f + 216.4f, 1.0f);
}

// Ladder jump up (chart T1871, q23 80.15 up a frame): the top blocked by the wall going on up (the 297 sweep over the top
// hits it), a ledge 500 over the top: on the top rung the stick up leans (ladder_idle_to_jump_up_prepare) instead of
// climbing off, jump leaps up the wall onto the ledge (ladder_jump_up, ledgeb_attach_U).
TEST(GameReal, LadderJumpUpToLedge) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    CollisionMesh m;
    addFloor(m, -2000, -2000, 400, 2000, 0);
    addQuad(m, {400, -2000, 0}, {400, 2000, 0}, {400, 2000, 3000}, {400, -2000, 3000}); // the wall, facing -X
    m.markup.push_back({9, 1, {{400, 79, 1200}, {400, 79, 0}, {400, -79, 0}}});
    m.markup.push_back({1, 2, {{400, 1000, 1700}, {400, -1000, 1700}}}); // hanging to its right (-X), as retail ledges
    auto w = climbWorld(std::move(m), {300, 0, 0});
    const auto& s = w->player().state();
    w->tick(forward());
    ASSERT_EQ(s.state, State::Ladder);
    for (int i = 0; i < 900 && s.clip.find("prepare") == std::string::npos; ++i) w->tick(forward());
    EXPECT_EQ(s.state, State::Ladder) << "blocked top: no climbing off";
    EXPECT_NE(s.clip.find("jump_up_prepare"), std::string::npos);
    Actions upJump = forward();
    upJump.jump = true;
    w->tick(upJump);
    EXPECT_EQ(s.clip, "ladder_jump_up");
    EXPECT_EQ(s.state, State::HangJump);
    for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w->tick({});
    EXPECT_EQ(s.state, State::Hang);
    EXPECT_EQ(s.clip, "ledgeb_attach_U");
    EXPECT_EQ(w->collision().markup[size_t(s.ledge)].id, 2);
}

// Ladders (camera mode 10: pitch set 16): stopped at a blocked top after climbing up with the stick still up, the
// camera pitches up toward 45 deg at 5 deg/s x curve(|stick| = 1) = 5 deg/s.
TEST(GameReal, CameraLadderPitchesUpAfterClimbingUp) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    CollisionMesh m;
    addFloor(m, -4000, -4000, 400, 4000, 0);
    addQuad(m, {400, -4000, 0}, {400, 4000, 0}, {400, 4000, 3000}, {400, -4000, 3000});
    m.markup.push_back({9, 1, {{400, 79, 1200}, {400, 79, 0}, {400, -79, 0}}});
    auto w = climbWorld(std::move(m), {300, 0, 0});
    const auto& s = w->player().state();
    w->tick(forward());
    ASSERT_EQ(s.state, State::Ladder);
    vec3 last = s.pos;
    int still = 0;
    for (int i = 0; i < 1200 && still < 60; ++i) w->tick(forward()), still = s.pos == last ? still + 1 : 0, last = s.pos;
    ASSERT_EQ(still, 60) << "stopped at the top";
    ASSERT_EQ(s.state, State::Ladder);
    EXPECT_GT(w->camera().pitch, 0.3f) << "pitched up toward 45 deg on the way up";
    Actions down = forward(); // look down a little: the look clock restarts
    down.look.y = -2;
    for (int i = 0; i < 20; ++i) w->tick(down);
    const float p0 = w->camera().pitch;
    for (int i = 0; i < 5 * 60; ++i) w->tick(forward());
    EXPECT_NEAR(w->camera().pitch, p0, 3e-3) << "no force within 5 s of the last look";
    for (int i = 0; i < int(2.5f * 60); ++i) w->tick(forward()); // past the 7 s ramp
    const float p1 = w->camera().pitch;
    ASSERT_LT(p1, glm::radians(45.0f) - 0.1f);
    for (int i = 0; i < 30; ++i) w->tick(forward());
    EXPECT_EQ(s.pos, last) << "she held still";
    EXPECT_NEAR(w->camera().pitch - p1, glm::radians(5.0f) / 2, 2e-3) << "5 deg/s toward 45 deg up";
}

// tru.exe (Wine, l1_seatop sea, camfollow-wine/swim_right.txt): swimming in a circle with the stick held right, the
// camera that dropped in at 37 deg above her levels out (29.5 / 23.1 / 18.5 / 12.5 deg at 2 / 3 / 4 / 6 s) to the
// surface-swim limit -10 deg (camera mode 20, command 8: -40 +- 30) and stays within -70..-10: her velocity lags her
// heading through the turn, so she drifts away from the camera. It used to steepen toward -70 deg (her velocity
// turned with her heading at once and the pivot, 43 ahead of her root, swung toward the camera). At 30 / 60 / 144 Hz
// rendering the camera is the same.
TEST(GameReal, SeaCircleCameraLevelsOut) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto run = [&](double renderHz, std::vector<float>* pitches) {
        game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), "l1_seatop", anims,
                      game::Spawn{{-15000, -3775, 2000}, glm::radians(-40.0f)});
        engine::FrameClock clock(game::kTickSeconds);
        Actions right;
        right.move.x = 1;
        while (w.ticks() < 480 + 12 * 60) // drop in and settle (8 s: look clock past 7 s), then 12 s round
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w.ticks() < 480 + 12 * 60; --n) {
                w.tick(w.ticks() < 480 ? Actions{} : right);
                if (pitches && w.ticks() > 480) pitches->push_back(w.camera().pitch);
            }
        EXPECT_EQ(w.player().state().state, State::SwimSurface);
        return w.camera();
    };
    std::vector<float> p;
    const auto a = run(60, &p);
    ASSERT_LT(p.front(), glm::radians(-35.0f)) << "dropped in looking down";
    for (size_t i = 0; i < p.size(); ++i) {
        EXPECT_GE(p[i], glm::radians(-70.0f) - 1e-4f) << "tick " << i;
        EXPECT_LE(p[i], glm::radians(-10.0f) + 1e-4f) << "tick " << i;
    }
    EXPECT_GT(p[4 * 60], glm::radians(-24.0f)) << "levelled out within 4 s (tru.exe 18.5 deg)";
    EXPECT_GT(*std::min_element(p.begin() + 7 * 60, p.end()), glm::radians(-22.0f)) << "and stays near the -10 deg limit";
    for (const double hz : {30.0, 144.0}) {
        const auto o = run(hz, nullptr);
        EXPECT_EQ(a.pitch, o.pitch) << hz;
        EXPECT_EQ(a.yaw, o.yaw) << hz;
        EXPECT_EQ(a.position, o.position) << hz;
    }
}

// Ladder jumps (player chart ladder group: jump + stick left / right T1869 / T1870, neutral T1868, all query q22: 47.41
// that way + 60.05 up a 30 Hz frame). Two ladders 900 apart on one wall: jump right takes off with ladder_jump_right
// (root motion, quarter turn), flies onto the other ladder's rung and attaches (ladder_attach_M, idle exactly on a rung);
// the stick held toward a ladder 300 aside reaches across (ladder_reach_right, ladder_settle_right); jump with the stick
// neutral leaps back at q22's speeds; on the lowest rung with no floor under it the stick down leans back
// (ladder_idle_to_jump_back_antic) instead of stepping off.
TEST(GameReal, LadderSideJumpsReachAndBack) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto rungZ = [](int r) { return 1200.0f - 575.221f - 110.619f * float(r); };
    const auto twoLadders = [](float apart, bool floor) {
        CollisionMesh m;
        if (floor) addFloor(m, -2000, -2000, 400, 2000, 0);
        else addFloor(m, -3000, -2000, -2000, 2000, -2000); // nothing under the ladders: a pit
        m.markup.push_back({9, 1, {{400, 79, 1200}, {400, 79, 0}, {400, -79, 0}}});
        m.markup.push_back({9, 2, {{400, 79 - apart, 1200}, {400, 79 - apart, 0}, {400, -79 - apart, 0}}});
        return m;
    };
    {
        auto w = climbWorld(twoLadders(900, true), {300, 0, 0});
        const auto& s = w->player().state();
        w->tick(forward());
        ASSERT_EQ(s.state, State::Ladder);
        for (int i = 0; i < 120 && s.clip != "ladder_idle"; ++i) w->tick({});
        Actions jump = worldStick(*w, {0, -1});
        jump.jump = true;
        w->tick(jump);
        EXPECT_EQ(s.clip, "ladder_jump_right");
        EXPECT_EQ(s.state, State::HangJump);
        EXPECT_EQ(w->collision().markup[size_t(s.ledge)].id, 2);
        for (int i = 0; i < 20 && s.clip == "ladder_jump_right"; ++i) w->tick({});
        EXPECT_NEAR(std::remainder(s.yaw + 1.5708f, 6.2831853f), 0, 0.05f) << "the take-off's quarter turn to the right";
        for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w->tick({});
        ASSERT_EQ(s.state, State::Ladder);
        EXPECT_EQ(s.clip, "ladder_attach_M");
        for (int i = 0; i < 120 && s.clip != "ladder_idle"; ++i) w->tick({});
        ASSERT_EQ(s.clip, "ladder_idle");
        EXPECT_NEAR(s.pos.y, -900, 1e-2);
        EXPECT_NEAR(s.pos.x, 400 - 110.619f, 1e-2);
        const float r = (1200.0f - 575.221f - s.pos.z) / 110.619f;
        EXPECT_NEAR(r, std::round(r), 1e-3) << "on a rung";
        EXPECT_NEAR(std::remainder(s.yaw, 6.2831853f), 0, 1e-3) << "facing the ladder again";
    }
    {
        auto w = climbWorld(twoLadders(300, true), {300, 0, 0});
        const auto& s = w->player().state();
        w->tick(forward());
        for (int i = 0; i < 120 && s.clip != "ladder_idle"; ++i) w->tick({});
        const float z0 = s.pos.z;
        w->tick(worldStick(*w, {0, -1}));
        EXPECT_EQ(s.clip, "ladder_reach_right");
        bool settled = false;
        for (int i = 0; i < 180 && s.clip != "ladder_idle"; ++i) {
            w->tick({});
            settled |= s.clip == "ladder_settle_right";
        }
        EXPECT_TRUE(settled);
        EXPECT_EQ(s.state, State::Ladder);
        EXPECT_EQ(w->collision().markup[size_t(s.ledge)].id, 2);
        EXPECT_NEAR(s.pos.y, -300, 1e-2);
        EXPECT_NEAR(s.pos.z, z0, 1e-2);
        w->tick(worldStick(*w, {0, 1})); // back left onto the first ladder, 300 the other way
        EXPECT_EQ(s.clip, "ladder_reach_left");
    }
    {
        auto w = climbWorld(twoLadders(5000, false), {400 - 110.619f, 0, rungZ(5)});
        const auto& s = w->player().state();
        w->tick(forward()); // in the air at the lowest rung: air attach
        for (int i = 0; i < 120 && s.clip != "ladder_idle"; ++i) w->tick({});
        ASSERT_EQ(s.state, State::Ladder);
        ASSERT_NEAR(s.pos.z, rungZ(5), 1e-2);
        w->tick(forward(-1));
        EXPECT_EQ(s.clip, "ladder_idle_to_jump_back_antic") << "no floor under the bottom: lean back, not step off";
        for (int i = 0; i < 60; ++i) w->tick(forward(-1));
        EXPECT_EQ(s.clip, "ladder_jump_back_prepare");
        w->tick({});
        EXPECT_EQ(s.clip, "ladder_jump_back_antic_to_idle");
        for (int i = 0; i < 60 && s.clip != "ladder_idle"; ++i) w->tick({});
        Actions jump;
        jump.jump = true;
        w->tick(jump);
        EXPECT_EQ(s.clip, "ladder_jump_back");
        EXPECT_EQ(s.state, State::Jump);
        EXPECT_NEAR(s.vel.x, -47.41f * 30, 1) << "q22 out from the ladder";
        EXPECT_NEAR(s.vel.z, 60.05f * 30, 90);
    }
}

// l7_arctic_sea ladder 2044 (wall y = -47421 facing +Y, 158 wide toward -X) hangs over a pit with balance beam 2042
// crossing 853 out from its wall at z -18899: from its middle rungs a jump back (ladder_jump_back, q22) leaps onto the
// beam behind her.
TEST(GameReal, ArcticSeaLadderJumpBackOntoBeam) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto rungZ = [](int r) { return -17318.5f - 575.221f - 110.619f * float(r); };
    for (const int rung : {11}) {
        auto w = unitWorld(bf, "l7_arctic_sea", {{16513.6f - 79, -47420.7f + 110.619f, rungZ(rung)}, glm::radians(-90.0f)});
        const auto& s = w.player().state();
        for (int i = 0; i < 5 && s.state != State::Ladder; ++i) w.tick(forward());
        ASSERT_EQ(s.state, State::Ladder);
        ASSERT_EQ(w.collision().markup[size_t(s.ledge)].id, 2044);
        const auto rungOf = [&] { return int(std::lround((-17318.5f - 575.221f - s.pos.z) / 110.619f)); };
        for (int i = 0; i < 3000 && !(s.clip == "ladder_idle" && rungOf() == rung); ++i)
            w.tick(s.clip != "ladder_idle" ? Actions{} : rungOf() > rung ? forward() : forward(-1));
        ASSERT_NEAR(s.pos.z, rungZ(rung), 0.1) << "float precision at z -19000";
        EXPECT_NEAR(s.pos.y, -47420.7f + 110.619f, 1.0f) << "110.619 out from the wall";
        Actions jump;
        jump.jump = true;
        w.tick(jump);
        EXPECT_EQ(s.clip, "ladder_jump_back") << rung;
        for (int i = 0; i < 180 && s.state != State::Beam && s.state != State::Fall; ++i) w.tick({});
        EXPECT_EQ(s.state, State::Beam);
        const auto* beam = markupById(w, 14, 2042);
        ASSERT_TRUE(beam);
        EXPECT_NEAR(s.pos.y, beam->points[0].y, 30);
        EXPECT_NEAR(s.pos.z, beam->points[0].z, 1);
    }
}

// Ledge corners (a polyline joint turning 90 degrees): shimmying right onto the joint plays ledgeb_<convex|concave>_
// right_phase1 into the corner pose (hands on the joint, facing between the walls), the corner idle while the stick
// lets go, the up / back leans (ledgeb_<kind>_jump_up_antic / _jump_back_antic) while it points that way, and on to the
// right phase2 round onto the next wall, the hang idle there facing it.
TEST(GameReal, HangShimmyRoundCorners) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    for (const bool concave : {false, true}) {
        const std::string kind = concave ? "concave" : "convex";
        CollisionMesh m;
        addFloor(m, -3000, -3000, 3000, 3000, -2000);
        // her side: x < 400 along the first segment (side -1); the next turns away from her (convex) or round to her
        m.markup.push_back({1, 1, {{400, 1000, 1200}, {400, 0, 1200}, {concave ? -600.0f : 1400.0f, 0, 1200}}});
        auto w = climbWorld(std::move(m), {0, 500, 0});
        game::PlayerState st = w->player().state();
        st.state = State::Hang, st.ledge = 0, st.ledgeSeg = 0, st.ledgeT = 700, st.ledgeSide = -1, st.vel = vec3(0);
        st.clip = "ledgeb_idle", st.loop = true, st.yaw = 0;
        w->restore(st, 0, -0.25f);
        const auto& s = w->player().state();
        w->tick({});
        ASSERT_EQ(s.state, State::Hang);
        ASSERT_NEAR(s.yaw, 0, 1e-3) << "facing the first wall (+x)";
        for (int i = 0; i < 400 && !s.clip.ends_with("phase1"); ++i) w->tick(worldStick(*w, {0, -1}));
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_right_phase1") << kind;
        for (int i = 0; i < 60 && s.clip.ends_with("phase1"); ++i) w->tick({});
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_idle") << kind;
        EXPECT_NEAR(std::remainder(s.yaw - (concave ? -0.785398f : 0.785398f), 6.2831853f), 0, 1e-3) << kind << " between the walls";
        const vec2 fwd(std::cos(s.yaw), std::sin(s.yaw));
        w->tick(worldStick(*w, fwd));
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_jump_up_antic") << kind;
        w->tick(worldStick(*w, -fwd));
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_jump_back_antic") << kind;
        w->tick({});
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_idle") << kind;
        const vec2 right(std::sin(s.yaw), -std::cos(s.yaw));
        w->tick(worldStick(*w, right));
        EXPECT_EQ(s.clip, "ledgeb_" + kind + "_right_phase2") << kind;
        for (int i = 0; i < 60 && s.clip.ends_with("phase2"); ++i) w->tick({});
        EXPECT_EQ(s.clip, "ledgeb_idle") << kind;
        EXPECT_EQ(s.ledgeSeg, 1) << kind;
        EXPECT_GT(s.ledgeT, 0) << kind;
        EXPECT_NEAR(std::remainder(s.yaw - (concave ? -1.5707963f : 1.5707963f), 6.2831853f), 0, 1e-3) << kind << " facing the next wall";
        EXPECT_EQ(s.state, State::Hang);
    }
}

// Pillar (markup 15, two points): from the floor pillar_attach_ground puts her 199 up the shaft (player data +0x7EE),
// pillar_climb_right takes her round it (left: mirrored) facing the axis, pillar_climb_up climbs, the slide
// (pillar_slide_down_start / _down) brings her down to pillar_detach_ground at its foot.
TEST(GameReal, PillarAttachRoundUpSlideOff) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    CollisionMesh m;
    addFloor(m, -2000, -2000, 2000, 2000, 0);
    m.markup.push_back({15, 1, {{400, 0, 1600}, {400, 0, 0}}});
    auto w = climbWorld(std::move(m), {300, 0, 0});
    const auto& s = w->player().state();
    w->tick(forward());
    ASSERT_EQ(s.state, State::Pillar);
    for (int i = 0; i < 120 && s.clip != "pillar_idle"; ++i) w->tick({});
    ASSERT_EQ(s.clip, "pillar_idle");
    EXPECT_NEAR(s.pos.z, 199, 1e-3);
    const auto fromAxis = [&] { return glm::length(vec2(s.pos) - vec2(400, 0)); };
    EXPECT_NEAR(fromAxis(), 0, 1e-2) << "tru.exe: Lara's instance on the axis";
    const float yaw0 = s.yaw;
    Actions right;
    right.move.x = 1;
    w->tick(fixedCamera(*w, right, 0)); // one pillar_climb_right: its root turns 1.31 from frame 0, it starts at frame 3
    EXPECT_EQ(s.clip, "pillar_climb_right");
    for (int i = 0; i < 60 && s.clip != "pillar_idle"; ++i) w->tick({});
    EXPECT_NEAR(std::remainder(s.yaw - yaw0, 6.2831853f), 1.19f, 0.05f) << "the clip's root turn";
    EXPECT_NEAR(fromAxis(), 0, 1e-2);
    const float z0 = s.pos.z;
    for (int i = 0; i < 120; ++i) w->tick(forward()); // stick up climbs, whichever way she faces (screen-relative)
    EXPECT_NEAR((s.pos.z - z0) / 2.0f, 312.0f, 40.0f) << "tru.exe: a steady 312 units/s";
    bool slid = false;
    std::vector<float> loop; // the slide loop's drop a tick
    for (int i = 0; i < 600 && s.state == State::Pillar; ++i) {
        const float zp = s.pos.z;
        w->tick(forward(-1));
        slid |= s.clip == "pillar_slide_down";
        if (s.clip == "pillar_slide_down" && s.pos.z > 193.5f) loop.push_back(zp - s.pos.z);
    }
    EXPECT_TRUE(slid);
    ASSERT_GE(loop.size(), 6u);
    // tru.exe under Wine (l4_temple 1194): 10 a 60 Hz frame for three frames, then a steady 15 (900 units/s)
    for (size_t i = 0; i < loop.size(); ++i) EXPECT_NEAR(loop[i], i < 3 ? 10.0f : 15.0f, 0.01f) << i;
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 0, 0.1);
}

// Pillar jumps (state S1213): the stick's angle from straight out picks the clip, she leaves along the stick at query
// q48's 55.63 out + 28.45 up a 30 Hz frame; no stick: pillar_jump_back straight out.
TEST(GameReal, PillarJumpsByStickAngle) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    struct Case { float deg; const char* clip; bool mirror; };
    for (const Case c : {Case{0, "pillar_jump_forward", false}, {50, "pillar_jump_forward", true}, {-50, "pillar_jump_forward", false},
                         {95, "pillar_jump_right", true}, {-95, "pillar_jump_right", false}, {-130, "pillar_jump_back_45R", false},
                         {999, "pillar_jump_back", false}}) {
        CollisionMesh m;
        addFloor(m, -2000, -2000, 2000, 2000, 0);
        m.markup.push_back({15, 1, {{400, 0, 1600}, {400, 0, 0}}});
        auto w = climbWorld(std::move(m), {300, 0, 0});
        const auto& s = w->player().state();
        w->tick(forward());
        for (int i = 0; i < 120 && s.clip != "pillar_idle"; ++i) w->tick({});
        ASSERT_EQ(s.clip, "pillar_idle");
        const float a = glm::radians(c.deg);
        const vec2 out(-1, 0), dir(std::cos(a) * out.x - std::sin(a) * out.y, std::sin(a) * out.x + std::cos(a) * out.y);
        Actions j = c.deg > 900 ? Actions{} : worldStick(*w, dir);
        j.jump = true;
        w->tick(j);
        EXPECT_EQ(s.state, State::Jump) << c.deg;
        EXPECT_EQ(s.clip, c.clip) << c.deg;
        EXPECT_EQ(s.mirror, c.mirror) << c.deg;
        EXPECT_NEAR(glm::length(vec2(s.vel)), 55.63f * 30, 1) << c.deg;
        EXPECT_NEAR(glm::dot(glm::normalize(vec2(s.vel)), c.deg > 900 ? out : dir), 1, 1e-3) << c.deg;
    }
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

// lara.drm's chart: the jump on a perch with the stick is perch_jump_forward on query q37 (74.32 out + 42.54 up a
// 30 Hz frame); without it perch_jump_up on q36 (63.21 straight up), back onto the perch. From a perch 500 over the
// floor q37 carries her ~1680 out before she lands (the old placeholder 600 + 1200 up: ~440).
TEST(Game, PerchJumpsByTheirQueries) {
    const auto perched = [] {
        CollisionMesh m;
        addFloor(m, -3000, -3000, 3000, 3000, 0);
        m.markup.push_back({13, 1, {{400, -50, 500}, {400, 50, 500}}});
        auto w = makeWorld(std::move(m), {350, 0, 400});
        Actions jump;
        jump.jump = true;
        w->tick(jump);
        for (int i = 0; i < 60; ++i) w->tick({});
        return w;
    };
    {
        auto w = perched();
        const auto& s = w->player().state();
        ASSERT_EQ(s.state, State::Perch);
        const vec2 f(std::cos(s.yaw), std::sin(s.yaw));
        const vec3 from = s.pos;
        Actions go = worldStick(*w, f);
        go.jump = true;
        w->tick(go);
        ASSERT_EQ(s.state, State::Jump);
        EXPECT_EQ(s.clip, "perch_jump_forward");
        EXPECT_NEAR(glm::dot(vec2(s.vel), f), 74.32f * 30, 1);
        EXPECT_NEAR(s.vel.z, 42.54f * 30, 1);
        for (int i = 0; i < 120 && s.state == State::Jump; ++i) w->tick(worldStick(*w, f));
        EXPECT_NEAR(glm::dot(vec2(s.pos - from), f), 1680, 60) << game::stateName(s.state);
        EXPECT_NEAR(s.pos.z, 0, 1);
    }
    auto w = perched();
    const auto& s = w->player().state();
    Actions jump;
    jump.jump = true;
    w->tick(jump);
    ASSERT_EQ(s.state, State::Jump);
    EXPECT_EQ(s.clip, "perch_jump_up");
    EXPECT_NEAR(glm::length(vec2(s.vel)), 0, 1e-3);
    EXPECT_NEAR(s.vel.z, 63.21f * 30, 1);
    for (int i = 0; i < 120 && s.state == State::Jump; ++i) w->tick({});
    EXPECT_EQ(s.state, State::Perch) << "back on the perch";
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
    // Ladder 234 lies in the valve wall's plane (y = 29, open side -Y), 158 wide from x 1401: she comes onto it out of
    // the air in front of it (the shaft has no floor), on the middle of its width, 110.619 out from the wall, facing it.
    auto w = unitWorld(bf, "l1_ship_sinking_hall", {{1480, -120, 1100}, glm::radians(90.0f)});
    for (int i = 0; i < 20 && w.player().state().state != State::Ladder; ++i) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Ladder);
    EXPECT_EQ(w.collision().markup[size_t(w.player().state().ledge)].id, 234);
    for (int i = 0; i < 120 && w.player().state().clip != "ladder_idle"; ++i) w.tick({});
    EXPECT_NEAR(w.player().state().pos.x, 1401 + 79, 1.0f);
    EXPECT_NEAR(w.player().state().pos.y, 29 - 110.619f, 1.0f);
    EXPECT_NEAR(glm::degrees(w.player().state().yaw), 90.0f, 1.0f) << "facing the wall (+Y)";
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
    const vec3 before = plane->pos - normal * 1000.0f, after = plane->pos + normal * 1000.0f; // toward local +Y
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
    // Command 1 moves the ramps by their sections, not their frame: section 0 (model 1) drops on prismatic joint 0 to
    // its lower limit (states 6 -> 3 -> 2), section 1 (model 2) plays its raising clip (transition 1, condition list);
    // model 0 (section 2) is the frame and stays.
    const auto collision = [&] {
        std::vector<glm::mat4> out;
        for (const auto& d : w.collision().dynamics)
            if (d.id == 6045) out.push_back(d.transform);
        return out;
    };
    const auto initial = collision();
    ASSERT_FALSE(initial.empty());
    for (int i = 0; i < 30; ++i) w.tick({}); // section 0 settles in its rest state 1 (0 -> 1 is automatic)
    ASSERT_TRUE(w.uberJoint(6045, 0));
    const float rest = w.uberJoint(6045, 0)->x;
    ASSERT_TRUE(w.sendUberCommand(6045, 1)); // command 1 (trigger 16, 1): 1 -> 6
    bool raising = false;
    for (int i = 0; i < 120; ++i) {
        w.tick({});
        for (const auto& c : w.objectClips()) raising |= c.uid == 6045 && c.section == 1 && c.clip == "raising";
    }
    EXPECT_LT(w.uberJoint(6045, 0)->x, rest - 900.0f) << "section 0 dropped on its joint";
    EXPECT_TRUE(raising) << "section 1 plays its clip";
    const auto after = collision(); // each section's collision follows it (World::syncObjects); the frame stays
    ASSERT_EQ(after.size(), initial.size());
    size_t moved = 0;
    for (size_t i = 0; i < after.size(); ++i) moved += after[i] != initial[i];
    EXPECT_GT(moved, 0u) << "the lowered section's collision moved";
    EXPECT_LT(moved, after.size()) << "the frame's collision stayed";
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

// l4_ballcourt pole lever 6502 is a receptacle (tru.exe: only a kind-5 engagement record, needing ca_pt_pole_a): a use
// next to it leaves door 6505 shut (ProgressionReal.BallcourtPoleLeverOpensDoor inserts the pole).
TEST(GameReal, BallcourtLeverIgnoresAUse) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_ballcourt", game::loadLaraAnims(bf), game::Spawn{{-20066, 14150, 3540}, 0});
    w.script().setBigfile(&bf);
    Actions use;
    use.interact = true;
    for (int i = 0; i < 30; ++i) w.tick(use);
    for (int i = 0; i < 240; ++i) w.tick({});
    EXPECT_TRUE(w.physics().hinges.empty()) << "the lever and door 6505 both slide on cylindrical joints";
    const auto* door = w.script().find(6505);
    ASSERT_TRUE(door);
    EXPECT_LT(std::abs(door->body[3].z), 5.0f) << "shut (its joint limit at rest)";
}

// l4_ballcourt: pole 6503 inserted into pole lever 6502 and hung on slides the lever down; its loop passes the lever's
// joint value (multibody 78 limit 1: the cylindrical joint's linear position, units) x 2 to door 6505's limit 1 (vt 54,
// multibody 77: linear on the door's cylindrical joint, whose marker Z points down): the door rises twice the drop.
TEST(GameReal, BallcourtLeverDrivesDoorJoint) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }),
                  "l4_ballcourt", game::loadLaraAnims(bf), game::Spawn{{-20584, 15040, 3484}, -glm::half_pi<float>()});
    w.script().setBigfile(&bf);
    Actions use;
    use.interact = true;
    for (int i = 0; i < 20; ++i) w.tick({});
    w.tick(use), w.tick({});
    ASSERT_EQ(w.heldPortable(), 6503);
    const auto* lever = w.script().find(6502);
    ASSERT_TRUE(lever);
    for (const vec3 side : {vec3(0, 250, 0), vec3(0, -250, 0), vec3(250, 0, 0), vec3(-250, 0, 0)}) {
        if (w.insertedIn(6503) == 6502) break;
        game::PlayerState s;
        s.pos = lever->pos + side, s.yaw = std::atan2(-side.y, -side.x);
        w.restore(s, s.yaw, -0.25f);
        for (int i = 0; i < 10; ++i) w.tick({});
        const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt");
        const auto names = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        for (const auto& u : w.units()) {
            const auto unitf = formats::parseUnit(*u.data.drm);
            for (const auto& in : unitf.intros) {
                const auto n = names.find(in.objectId);
                const std::string nm = n == names.end() ? "?" : n->second;
                if (nm.find("grap") != std::string::npos || nm.find("ring") != std::string::npos || nm.find("pole") != std::string::npos || nm.find("bling") != std::string::npos)
                    std::printf("  intro %s uid %d %s at %.0f %.0f %.0f script %d\n", u.data.name.c_str(), in.uniqueId, nm.c_str(),
                                in.position[0] + u.offset.x, in.position[1] + u.offset.y, in.position[2] + u.offset.z, int(in.scriptId));
            }
        }
        w.tick(use), w.tick({});
    }
    ASSERT_EQ(w.insertedIn(6503), 6502);
    const collision::Markup* m = nullptr;
    for (const auto& x : w.collision().markup)
        if (x.type == 6 && x.points.size() == 2 && glm::distance(x.points[0], lever->pos) < 1500) m = &x;
    ASSERT_TRUE(m);
    const vec3 mid = (m->points[0] + m->points[1]) * 0.5f, along = glm::normalize(m->points[1] - m->points[0]);
    game::PlayerState s;
    s.pos = mid + vec3(-along.y, along.x, 0) * 17.0f + vec3(0, 0, -594.0f), s.yaw = std::atan2(along.x, -along.y);
    w.restore(s, s.yaw, -0.25f);
    for (int i = 0; i < 300; ++i) w.tick({});
    EXPECT_EQ(w.player().state().state, game::State::PoleHang);
    lever = w.script().find(6502);
    ASSERT_TRUE(lever && lever->joint);
    const float drop = lever->joint->position;
    EXPECT_LE(drop, -350.0f) << "into its watch";
    const auto* door = w.script().find(6505);
    ASSERT_TRUE(door);
    EXPECT_NEAR(door->body[3].z, -2 * drop, 2) << "door 6505 risen twice the lever's drop";
    EXPECT_NEAR(glm::length(glm::vec2(door->body[3])), 0, 1) << "straight up its joint";
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
        for (const vec3 p : t->kind == script::Kind::Plane ? std::vector<vec3>{base - n * 300.0f, base + n * 300.0f}
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
    // up through the plane (its local +Y); the first step twice: the room's opening cinematic moves her after one tick
    for (const vec3 p : {base - n * 300.0f, base - n * 300.0f, base + n * 300.0f}) {
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
    const float yaw0 = w->camera().yaw;
    engine::FrameClock clock(game::kTickSeconds);
    std::vector<uint64_t> out;
    for (int f = 0; int(out.size()) < ticks; ++f) {
        for (int n = clock.advance(frameDt(f)); n > 0 && int(out.size()) < ticks; --n) {
            w->tick(fixedCamera(*w, input.at(w->ticks() - start), yaw0));
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

// l4_ballcourt: Lara by pole lever 6502 (a receptacle now: the door needs its pole, ProgressionReal.BallcourtPoleLeverOpensDoor)
// and door 6505 (on its cylindrical joint), interacting and running about.
TEST(GameReal, BallcourtLeverIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto input = game::InputScript::parse("0.3 interact 1\n0.4 interact 0\n1.7 forward 1\n2.85 forward 0\n2.9 right 1\n6.1 right 0\n");
    const auto w = expectRateIndependent(worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}), input, 430);
    ASSERT_TRUE(w->script().find(6505)) << "door 6505";
    EXPECT_LT(std::abs(w->script().find(6505)->body[3].z), 5.0f) << "shut: no pole in the lever";
}

// The same with l4_ballcourt's enemies live, as in ou-game (poacher fire and hit reactions interrupt the run): plane
// 6543 has dispatcher 6536 birth poachers 6537..6539 and panther 6532 ("combat 4 starting"). AI, shots and hit
// capsules stay tick-based while their skinning is culled per frame by the view.
TEST(GameReal, BallcourtWithEnemiesIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto input = game::InputScript::parse("0.3 interact 1\n0.4 interact 0\n1.7 forward 1\n2.85 forward 0\n2.9 right 1\n6.1 right 0\n");
    const auto base = worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}, true);
    const auto w = expectRateIndependent([&] {
        auto x = base();
        x->script().call(6543, 54); // the crossing
        return x;
    }, input, 430);
    EXPECT_FALSE(w->enemies().all().empty());
}

// Ragdolls (docs/formats/PHYSICS.md "Ragdolls"): l4_ballcourt's dead poachers 6540..6542 are dormant intros; plane 6546
// (crossed on the way in from l4_trailtube) has dispatcher 6535 birth them (vt 63 -> state 1: instanceref Birth) and
// their constructors kill them (health 1); each plays its death clip, then its ragdoll (poacher.drm's 15 bones) takes
// the body. 6540 lies on the edge of a ledge: it slides off, falls about 500 units onto the floor below and settles.
// Every tick is bit-identical at any render rate.
TEST(GameReal, KilledPoacherRagdollIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto base = worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}, true);
    const auto w = expectRateIndependent([&] {
        auto x = base();
        x->script().call(6546, 54); // the crossing
        return x;
    }, game::InputScript::parse(""), 600);
    const game::ai::Enemy* poacher = nullptr;
    for (const auto& e : w->enemies().all())
        if (e->uid() == 6540) poacher = e.get();
    ASSERT_TRUE(poacher);
    const auto& s = poacher->state();
    EXPECT_EQ(s.mode, game::ai::Mode::Dead);
    ASSERT_TRUE(poacher->type().ragdoll);
    EXPECT_EQ(poacher->type().ragdoll->bones.size(), 15u);
    EXPECT_EQ(s.ragdollPhase, 3) << "settled";
    ASSERT_EQ(s.ragdoll.size(), 15u);
    const glm::mat4 model = game::ai::Enemy::modelMatrix(s.pos, s.yaw, 0, s.tilt);
    const vec3 pelvis(model * s.ragdoll[0][3]);
    EXPECT_LT(pelvis.z, s.pos.z - 300) << "fell off the ledge";
    const auto floor = w->collision().groundProbe(pelvis + vec3(0, 0, 100), 1000, 0x0F);
    ASSERT_TRUE(floor);
    EXPECT_LT(pelvis.z - floor->point.z, 80.0f) << "lying on the floor";
    EXPECT_GT(pelvis.z - floor->point.z, -10.0f) << "not through it";
}

// Dormant intros (+0x72 & 0x80) do not exist at unit load (tru.exe 0x5C2360): none of l4_ballcourt's panthers 6531..6534
// and poachers 6537..6542 is an enemy or born, nor do triggerplanes 6545 / 6547 exist (their scripts do not run).
// Crossing plane 6546 has dispatcher 6535 birth 6540..6542 (instanceref Birth, 0x4AC0D0): they exist from then on and
// their constructors kill them; the others stay dormant (saves: StreamReal.SaveContinuesBitIdentically).
TEST(GameReal, BallcourtDormantIntrosBornByScripts) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto make = worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0}, true);
    const auto w = make();
    const auto enemy = [](const game::World& x, int32_t uid) -> const game::ai::Enemy* {
        for (const auto& e : x.enemies().all())
            if (e->uid() == uid) return e.get();
        return nullptr;
    };
    const int32_t dormant[] = {6531, 6532, 6533, 6534, 6537, 6538, 6539, 6540, 6541, 6542};
    for (int i = 0; i < 30; ++i) w->tick({});
    for (const int32_t uid : dormant) EXPECT_FALSE(enemy(*w, uid) || w->script().born(uid)) << uid;
    EXPECT_FALSE(w->script().find(6545) || w->script().find(6547));
    EXPECT_TRUE(w->script().find(6535) && w->script().find(6546));
    w->script().call(6546, 54);
    w->tick({}), w->tick({}); // births at the end of the first tick; constructors SLEEP once, then health 1 (10000)
    for (const int32_t uid : {6540, 6541, 6542}) {
        EXPECT_TRUE(w->script().born(uid)) << uid;
        ASSERT_TRUE(enemy(*w, uid)) << uid;
        EXPECT_FALSE(enemy(*w, uid)->alive()) << uid; // killed by its constructor
    }
    for (const int32_t uid : {6531, 6532, 6533, 6534, 6537, 6538, 6539}) EXPECT_FALSE(enemy(*w, uid)) << uid;
}

// Lara's death ragdoll (AnimLibrary::ragdoll, 55 kg, 15 bones): killed on l4_ballcourt, her death clip plays, then
// her ragdoll takes the last pose and collapses; bit-identical at any render rate up to just before the respawn.
TEST(GameReal, LaraRagdollIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto base = worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0});
    const auto make = [&] {
        auto w = base();
        // an absolute (fall) death: death_fall_normal (1.07 s) ends well inside the 3 s death fade; death_toknee (4.7 s)
        // would still be playing at the respawn
        w->damageLara(1e6f, w->player().state().pos, game::kDamageAbsolute);
        return w;
    };
    float clip = 0;
    {
        const auto w = make();
        ASSERT_TRUE(w->anims().ragdoll);
        EXPECT_EQ(w->anims().ragdoll->bones.size(), 15u);
        EXPECT_FLOAT_EQ(w->anims().ragdoll->mass, 55.0f);
        w->tick({});
        ASSERT_EQ(w->player().state().state, game::State::Dead);
        clip = w->anims().find(w->player().state().clip)->durationSeconds();
    }
    const int ticks = int(std::min(clip + 0.9f, 2.95f) / game::kTickSeconds); // before the respawn (3 s after the death)
    const auto w = expectRateIndependent(make, game::InputScript::parse(""), ticks);
    ASSERT_EQ(w->player().state().state, game::State::Dead);
    const auto& c = w->combat();
    EXPECT_GE(c.ragdollPhase, 2);
    ASSERT_EQ(c.ragdoll.size(), 15u);
    const auto& s = w->player().state();
    const glm::mat4 model = w->player().modelMatrix(s.pos, s.yaw, s.pitch);
    const float pelvis = (model * c.ragdoll[0][3]).z, head = (model * c.ragdoll[2][3]).z;
    EXPECT_LT(head - s.pos.z, 400.0f) << "collapsed";
    EXPECT_GT(pelvis - s.pos.z, -60.0f) << "not through the floor";
}

// A Mjolnir bolt kills l7_yggdrasil_hall01's viking thrall 8833: the death timer breaks it into
// viking_thrall_chunkdoll's 14 pieces (loose bodies at its pose, pushed along the bolt), which scatter and come to rest
// on the hall's floor.
TEST(GameReal, KilledThrallBreaksIntoChunks) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto w = worldMaker(bf, "l7_yggdrasil_hall01", game::Spawn{{1000, 0, 0}, glm::half_pi<float>()}, true)();
    w->tick({});
    game::ai::Enemy* thrall = nullptr;
    for (const auto& e : w->enemies().all())
        if (e->uid() == 8833) thrall = e.get();
    ASSERT_TRUE(thrall);
    ASSERT_EQ(thrall->type().name, "viking_thrall");
    const auto uid = thrall->uid();
    const game::vec3 at = thrall->position();
    game::Hit bolt{1000, at + game::vec3(0, 0, 200), {1, 0, 0}, game::Team::Player};
    bolt.damageClass = 13, bolt.damageType = 3;
    thrall->onHit(bolt);
    ASSERT_EQ(thrall->state().mode, game::ai::Mode::Dead);
    const auto find = [&]() -> const game::ai::Enemy* {
        for (const auto& e : w->enemies().all())
            if (e->uid() == uid) return e.get();
        return nullptr;
    };
    for (int i = 0; i < 120 && find()->state().pieces.empty(); ++i) w->tick({});
    ASSERT_EQ(find()->state().ragdollPhase, 4);
    ASSERT_EQ(find()->state().pieces.size(), 14u);
    const auto where = [&] {
        const auto& s = find()->state();
        const glm::mat4 model = game::ai::Enemy::modelMatrix(s.pos, s.yaw, 0, s.tilt);
        std::vector<game::vec3> out;
        for (const auto& p : s.pieces) out.emplace_back(model * p[3]);
        return out;
    };
    for (int i = 0; i < 570; ++i) w->tick({});
    const auto before = where();
    for (int i = 0; i < 30; ++i) w->tick({});
    const auto after = where();
    EXPECT_EQ(find()->state().ragdollPhase, 4);
    ASSERT_EQ(after.size(), 14u);
    float spread = 0;
    for (size_t k = 0; k < after.size(); ++k) {
        const game::vec3 q = after[k];
        spread = std::max(spread, glm::length(game::vec2(q - at)));
        EXPECT_LT(std::abs(q.z - before[k].z), 5.0f) << "piece " << k << " landed"; // the head may still roll
        const auto floor = w->collision().groundProbe(q + game::vec3(0, 0, 100), 1000, 0x0F);
        ASSERT_TRUE(floor) << "piece " << k << " above a floor";
        EXPECT_LT(q.z - floor->point.z, 150.0f) << "piece " << k << " lying on it";
        EXPECT_GT(q.z - floor->point.z, -20.0f) << "piece " << k << " not through it";
    }
    EXPECT_GT(spread, 60.0f) << "scattered";
}

// Interactions (docs/GAMEPLAY.md "Interactions"): l3_main_bedroom wall switch 4525 — Lara stands in front of it and plays
// its own Lara clip (obj_pull_lever from the switch DRM) with the input locked while the switch's use handler still runs
// at the switch clip's end; l4_ballcourt push block 6504 — grab, push clip, Lara carried with the block, release.
TEST(GameReal, LaraPlaysInteractionClips) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::Actions use;
    use.interact = true;
    {
        const auto w = worldMaker(bf, "l3_main_bedroom", game::Spawn{{-8680, -8279, 1767}, 0})();
        w->tick({});
        ASSERT_TRUE(w->script().find(4525));
        w->tick(use);
        const auto* sw = w->script().find(4525);
        const auto& s = w->player().state();
        EXPECT_EQ(s.state, game::State::Use);
        EXPECT_EQ(s.clip, "use:" + std::to_string(sw->laraClip));
        EXPECT_EQ(s.clip, "use:2605") << "obj_pull_lever";
        EXPECT_NEAR(glm::length(glm::vec2(s.pos - sw->pos)), sw->useDistance, 1.0f);
        bool fired = false;
        for (int i = 0; i < 200 && !fired; ++i) {
            w->tick(i % 2 ? use : game::Actions{}); // presses are ignored while the clip plays
            for (const auto& e : w->script().events()) fired |= e.kind == script::EventKind::Use && e.source == 4525;
            if (!fired) EXPECT_EQ(w->player().state().state, game::State::Use) << "tick " << i;
        }
        EXPECT_TRUE(fired) << "the switch's use handler";
        for (int i = 0; i < 60; ++i) w->tick({});
        EXPECT_EQ(w->player().state().state, game::State::Idle);
    }
    {
        const auto w = worldMaker(bf, "l4_ballcourt", game::Spawn{{-20066, 14150, 3540}, 0})();
        w->tick({});
        const auto block = [&] { return w->script().find(6504); };
        ASSERT_TRUE(block());
        bool pushed = false;
        for (const float a : {0.0f, glm::half_pi<float>(), glm::pi<float>(), -glm::half_pi<float>()}) { // each face in turn
            const float yaw = block()->rot.z + a;
            const vec3 dir(std::cos(yaw), std::sin(yaw), 0);
            vec3 at = block()->pos + block()->offset - dir * 160.0f;
            if (const auto floor = w->collision().groundProbe(at + vec3(0, 0, 200), 600, 0x0F)) at.z = floor->point.z;
            game::PlayerState p;
            p.pos = at, p.yaw = yaw;
            w->restore(p, yaw, -0.25f);
            w->tick(use);
            ASSERT_EQ(w->player().state().state, game::State::Block);
            EXPECT_EQ(w->player().state().clip, "obj_block_grab");
            const vec3 block0 = block()->offset, lara0 = w->player().state().pos;
            game::Actions push = use;
            push.move.y = 1;
            for (int i = 0; i < 150; ++i) w->tick(push);
            const vec3 moved = block()->offset - block0;
            if (glm::length(moved) < 50) continue;
            EXPECT_EQ(w->player().state().clip, "obj_block_front");
            const vec3 lara = w->player().state().pos - lara0;
            EXPECT_LT(glm::length(lara - moved), 1.0f) << "Lara moves with the block: " << lara.x << " " << lara.y << " " << lara.z
                                                       << " block " << moved.x << " " << moved.y << " " << moved.z;
            w->tick({});
            EXPECT_EQ(w->player().state().clip, "obj_block_release");
            pushed = true;
            break;
        }
        EXPECT_TRUE(pushed);
    }
}

// l1_baldrs_tomb press plate 219 (prismatic joint, Objects.cpp tickJoints): Lara stands on it, it sinks to its pressed
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
    EXPECT_NEAR(linked->x, 0.75f * post->x, 0.5f) << "the 0.75 link"; // a Jolt constraint row: holds within its drift fix
    ASSERT_TRUE(w->uberState(6695));
    EXPECT_EQ((*w->uberState(6695))[1], 1u);
}

// Joint sounds (World::jointSounds, AUDIO.md §9.16): l1_baldrs_tomb press plate 219 (class 34, ms_rb_press_plate: its
// joint record's sound) sinks under Lara, reaching its pressed limit (an impact) and goes quiet 0.5 s after it rests;
// l4_trailstart grapple lever post 6695 (uber joints, ca_ei_lever_grapple_post) slides out while pulled.
TEST(GameReal, JointSoundsFollowMovingJoints) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto run = [](game::World& w, const game::InputScript& input, int ticks, const std::function<void(const game::JointSound&)>& f) {
        const uint64_t start = w.ticks();
        const float yaw0 = w.camera().yaw;
        for (int t = 0; t < ticks; ++t) {
            w.tick(fixedCamera(w, input.at(w.ticks() - start), yaw0));
            for (const auto& s : w.jointSounds()) f(s);
        }
    };
    vec3 at(0);
    for (const auto& in : formats::parseUnit(*game::loadUnitData(bf, "l1_baldrs_tomb").drm).intros)
        if (in.uniqueId == 219) at = {in.position[0], in.position[1], in.position[2] + 30};
    auto plate = worldMaker(bf, "l1_baldrs_tomb", game::Spawn{at, 0})();
    bool sinking = false, impact = false;
    run(*plate, game::InputScript::parse("10 forward 0\n"), 120, [&](const game::JointSound& s) {
        if (s.uid != 219) return;
        EXPECT_EQ(s.joint, -1);
        EXPECT_EQ(s.object, "ms_rb_press_plate");
        sinking |= s.linear < -1.0f;
        impact |= s.impact.has_value();
    });
    EXPECT_TRUE(sinking) << "Lara's weight presses it down";
    EXPECT_TRUE(impact) << "its pressed limit";
    bool heard = false;
    run(*plate, game::InputScript::parse("10 forward 0\n"), 60, [&](const game::JointSound& s) { heard |= s.uid == 219; });
    EXPECT_FALSE(heard) << "asleep: at rest under her for more than 0.5 s";

    auto lever = worldMaker(bf, "l4_trailstart", game::Spawn{{61161, -42611, 4948}, glm::radians(-11.0f)})();
    bool slides = false, early = false;
    run(*lever, game::InputScript::load(OU_SOURCE_DIR "/tests/replays/grapple_pull.txt"), 900, [&](const game::JointSound& s) {
        if (s.uid != 6695) return;
        early |= lever->ticks() < 120; // the grapple is thrown at 1.2 s, pulled from 2.5 s
        if (s.joint == 0 && std::abs(s.linear) > 10.0f) slides = true;
    });
    EXPECT_FALSE(early) << "at rest since spawn: asleep, no voice";
    EXPECT_TRUE(slides) << "the post's prismatic joint, pulled out at its motor speed";
    heard = false;
    run(*lever, game::InputScript::parse("10 forward 0\n"), 60, [&](const game::JointSound& s) { heard |= s.uid == 6695; });
    EXPECT_FALSE(heard) << "settled at its limit: asleep again";
}

// l2_puzzleroom: switch 2961 starts elevator 2973's uber command chain; the platform's collision moves.
TEST(GameReal, ElevatorUberIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    vec3 at(0);
    for (const auto& in : formats::parseUnit(*game::loadUnitData(bf, "l2_puzzleroom").drm).intros)
        if (in.uniqueId == 2961) at = {in.position[0], in.position[1], in.position[2]};
    const auto input = game::InputScript::parse("1.0 interact 1\n1.5 interact 0\n");
    // 200 ticks: mid-way through the elevator's rumble transition (its section clip; the pole in hole 2969 still holds
    // it, so the switch alone does not drop it).
    const auto w = expectRateIndependent(worldMaker(bf, "l2_puzzleroom", game::Spawn{at, 0}), input, 200);
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

// Wetness (tru.exe 0x6FF200 / 0x6ED130): soaked at once in water; out of it the regions dry at 0.0002 per 30 Hz frame
// (1 -> 0 in 5000 frames, ~167 s). The head chain's first bone follows its region; the feet, last down the chains, stay
// wet until the bones above them dried (water runs down).
TEST(Game, WetnessSoaksAndDries) {
    ou::game::Wetness w;
    w.water = 0x7F;
    w.tick(1);
    for (const float r : w.region) EXPECT_EQ(r, 1.0f);
    EXPECT_EQ(w.bone[17], 1.0f);  // head
    EXPECT_EQ(w.bone[95], 1.0f);  // a hand bone
    EXPECT_EQ(w.bone[0], 0.0f);   // root: in no chain
    w.water = 0;
    for (int i = 0; i < 2500; ++i) w.tick(1);
    EXPECT_NEAR(w.region[6], 0.5f, 1e-3f);
    EXPECT_NEAR(w.bone[17], 0.5f, 1e-3f);
    EXPECT_EQ(w.bone[8], 1.0f);   // foot: not yet
    for (int i = 0; i < 12000; ++i) w.tick(1);
    EXPECT_EQ(w.region[1], 0.0f);
    EXPECT_EQ(w.bone[8], 0.0f);
    EXPECT_EQ(w.bone[95], 0.0f);
}

// Drying timeline after leaving the water, tru.exe 1.1 under Wine (l1_seatop yacht stern, *0xFADF98 sampled every
// 0.75 s): head first, then down the spine and arms, the feet last. Seconds from leaving the water until each bone
// starts drying / is dry; World ticks at 60 Hz with timeMult 0.5.
TEST(Game, WetnessDryingMatchesOriginalTimeline) {
    ou::game::Wetness w;
    w.water = 0x7F;
    w.tick(1);
    w.water = 0;
    struct Bone { int bone; float start, dry; };
    const Bone tru[] = {{17, 0.0f, 166.4f},  {51, 33.0f, 199.3f},  {16, 36.7f, 203.0f}, {14, 66.7f, 232.5f},
                        {1, 77.5f, 244.1f},  {3, 100.2f, 266.4f},  {4, 116.1f, 283.0f}, {5, 133.3f, 299.5f},
                        {6, 144.5f, 311.1f}, {8, 166.4f, 332.7f},  {11, 144.5f, 311.1f}, {13, 166.4f, 332.7f},
                        {53, 37.5f, 203.9f}, {76, 66.7f, 232.5f},  {95, 100.2f, 266.4f}};
    float start[160], dry[160];
    std::fill(std::begin(start), std::end(start), -1.0f), std::fill(std::begin(dry), std::end(dry), -1.0f);
    for (int t = 1; t <= 60 * 400; ++t) {
        w.tick(0.5f);
        for (const auto& b : tru) {
            if (start[b.bone] < 0 && w.bone[size_t(b.bone)] < 0.999f) start[b.bone] = float(t) / 60;
            if (dry[b.bone] < 0 && w.bone[size_t(b.bone)] <= 0.0f) dry[b.bone] = float(t) / 60;
        }
    }
    for (const auto& b : tru) {
        SCOPED_TRACE(b.bone);
        EXPECT_NEAR(start[b.bone], b.start, 1.5f); // the samples are 0.75 s apart
        EXPECT_NEAR(dry[b.bone], b.dry, 1.5f);
    }
}

// bend_* plants (game/Bend.hpp). A steady wind (no frequency, no wavelength: the phase stays 0, so no |sin| pulse)
// settles a critically damped bone at x = F / w^2; its bend vector is b = linear x - quadratic x^2 / 2 for a box
// symmetric about the trunk. The wind ramps to strength x 8 over the retarget countdown (seconds x 60 ticks).
TEST(Game, BendSettlesUnderSteadyWind) {
    using namespace ou;
    formats::Unit::Wind rec;
    rec.enabled = true, rec.retarget[0] = rec.retarget[1] = 1, rec.strength[0] = rec.strength[1] = 0.1f;
    game::Wind wind({rec});
    wind.tick();
    EXPECT_NEAR(wind.at(vec3(0)).x, 0.8f / 60, 1e-6f);
    auto type = std::make_shared<game::BendType>();
    game::BendType::Bone b; // bend_kelp_a's bone [O]
    b.segment = 1, b.mass = 5, b.linear = 0.8f, b.quadratic = 0.6f, b.omega = 3, b.damping = 1, b.wind = 5;
    b.lo = {-407.7f, -348.2f, -34.9f}, b.hi = {407.7f, 348.2f, 3188.4f}, b.inv2Size = {1 / 1630.9f, 1 / 1392.6f};
    type->bones = {b}, type->segments = 2;
    const std::vector<game::BendIntro> intros{{7, vec3(0), vec3(0, 0, 3.14159265f / 2), type}}; // turned 90 degrees
    const std::vector<formats::Unit::Wind> winds{rec};
    const game::Bends::Unit unit{"u", vec3(0), &intros, &winds};
    game::Bends bends;
    bends.sync({&unit, 1});
    const vec3 far(1e5f);
    for (int i = 0; i < 1200; ++i) bends.tick(far, far);
    const auto out = bends.bend(7);
    ASSERT_EQ(out.size(), 2u);
    const float x = 5 * 0.8f / 9; // F = w^2 x (tru.exe does not scale the spring by mass); wind +X = the plant's local -Y
    EXPECT_NEAR(out[1].x, 0, 1e-6f);
    EXPECT_NEAR(out[1].y, -x * x * 0.6f / (b.hi.y - b.lo.y), 1e-6f);
    EXPECT_NEAR(out[1].w, 0.8f * -x + 0.3f * x * x, 1e-4f);
    EXPECT_EQ(out[0], glm::vec4(0));
}

// Without wind, Lara swimming past a distance-pushed plant bends it away from her.
TEST(Game, BendGivesWayToLara) {
    using namespace ou;
    auto type = std::make_shared<game::BendType>();
    game::BendType::Bone b;
    b.mass = 5, b.linear = 0.8f, b.quadratic = 0.6f, b.omega = 3, b.damping = 1, b.push = 25, b.pushRadius = 300, b.pushPad = 0;
    b.centre = {0, 0, 1500}, b.lo = vec3(-400, -400, 0), b.hi = vec3(400, 400, 3000), b.inv2Size = vec2(1 / 1600.0f);
    type->bones = {b}, type->segments = 1;
    const std::vector<game::BendIntro> intros{{1, vec3(0), vec3(0), type}};
    const game::Bends::Unit unit{"u", vec3(0), &intros, nullptr};
    game::Bends bends;
    bends.sync({&unit, 1});
    for (int i = 0; i < 30; ++i) { // 10 units per tick toward +Y, 200 to the plant's -X side of its centre
        const vec3 at(-200, -150 + 10.0f * i, 1500);
        bends.tick(at, at - vec3(0, 10, 0));
    }
    EXPECT_GT(bends.bend(1)[0].z, 0.01f); // b.x = linear x + ...: bent toward +X, away from Lara
}

TEST(GameReal, SeafloorKelpSwaysInTheUnitWind) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto u = game::loadUnitData(bf, "l1_seafloor");
    ASSERT_EQ(u.wind.size(), 1u); // [O] settings +0x2A4: retarget 1..5 s, strength 0..0.1, heading 0..80, wavelength 6..10, 0.1 Hz
    EXPECT_TRUE(u.wind[0].enabled);
    EXPECT_EQ(u.wind[0].heading[1], 80.0f);
    EXPECT_NEAR(u.wind[0].frequency, 0.1f, 1e-6f);
    const auto kelp = std::find_if(u.bends.begin(), u.bends.end(), [](const game::BendIntro& b) { return b.uid == 886; });
    ASSERT_NE(kelp, u.bends.end());
    ASSERT_EQ(kelp->type->bones.size(), 1u);
    const auto& bone = kelp->type->bones[0]; // tru.exe's state after setup (Wine, l1_seafloor instance 886) [O]
    EXPECT_EQ(bone.segment, 1);
    EXPECT_NEAR(bone.lo.x, -407.733f, 1e-2f);
    EXPECT_NEAR(bone.hi.z, 3188.367f, 1e-2f);
    EXPECT_NEAR(bone.inv2Size.x, 0.000613146f, 1e-8f);
    EXPECT_GE(u.bends.size(), 50u);
    // kelp_b 873 in tru.exe peaked at x ~0.12 within 20 s; ours stays in that range and moves.
    const std::vector<game::Bends::Unit> units{{u.name, vec3(0), &u.bends, &u.wind}};
    game::Bends bends;
    bends.sync(units);
    float lo = 1, hi = -1;
    for (int i = 0; i < 60 * 30; ++i) {
        bends.tick(vec3(1e6f), vec3(1e6f));
        const float bx = bends.bend(873)[1].z;
        lo = std::min(lo, bx), hi = std::max(hi, bx);
    }
    EXPECT_GT(hi - lo, 0.01f);
    EXPECT_LT(std::max(std::abs(lo), std::abs(hi)), 0.3f);
}

// Recenter camera (virtual button 8): the press turns the orbit camera behind Lara at the rest pitch.
TEST(Game, RecenterTurnsCameraBehindLara) {
    auto w = makeWorld(poolMesh(), {-1500, 0, 0}, 0.7f);
    w->tick({});
    Actions look;
    look.look = {2.0f, 0.5f};
    for (int i = 0; i < 40; ++i) w->tick(look);
    const float yaw = w->player().state().yaw;
    ASSERT_GT(std::abs(std::remainder(w->camera().yaw - yaw, 6.2831853f)), 0.5f);
    Actions r;
    r.recenter = true;
    w->tick(r);
    EXPECT_NEAR(std::remainder(w->camera().yaw - yaw, 6.2831853f), 0.0f, 0.02f);
    EXPECT_NEAR(w->camera().pitch, game::CameraState{}.pitch, 0.02f);
    for (int i = 0; i < 60; ++i) w->tick(r); // held: no new press; the camera swings round behind her and stays
    EXPECT_NEAR(std::remainder(w->camera().yaw - yaw, 6.2831853f), 0.0f, 0.05f);
    const vec3 toLara = w->player().state().pos - w->camera().position; // the camera behind her
    EXPECT_GT(glm::dot(glm::normalize(vec2(toLara)), vec2(std::cos(yaw), std::sin(yaw))), 0.95f);
}

// Q is both the grapple (button 1) and Recenter Camera (button 8) in tru.exe's keyboard defaults (0x487710: key 0x90).
// With a grapple target in reach the press throws the grapple and leaves the camera where it is; with none it recenters.
TEST(GameReal, SharedGrappleKeyThrowsWithoutRecentering) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& name) { return game::loadUnitData(bf, name); }), "l4_pool",
                  game::loadLaraAnims(bf), game::Spawn{{4362, -400, 2000}, 0});
    w.restore(w.player().state(), 1.5707963f, 0.7f); // looking up at the ceiling anchor 5511, Lara facing +x
    w.tick({}); // no press edge on the first tick
    ASSERT_TRUE(w.grappleTarget());
    const float camYaw = w.camera().yaw;
    Actions q;
    q.grapple = q.recenter = true;
    w.tick(q);
    EXPECT_EQ(w.player().state().state, State::GrappleThrow);
    EXPECT_EQ(w.player().state().grappleUid, 5511);
    EXPECT_GT(std::abs(std::remainder(w.camera().yaw - w.player().state().yaw, 6.2831853f)), 1.0f) << "not recentered";
    EXPECT_NEAR(std::remainder(w.camera().yaw - camYaw, 6.2831853f), 0.0f, 0.2f);
    for (int i = 0; i < 90; ++i) w.tick({});
    ASSERT_EQ(w.player().state().state, State::GrappleSwing);
    w.tick(q); // the same key lets go of the rope, still without recentering
    EXPECT_EQ(w.player().state().grappleUid, 0);
}

TEST(Game, SharedGrappleKeyRecentersWithoutTarget) {
    auto w = makeWorld(poolMesh(), {-1500, 0, 0}, 0.7f);
    w->tick({});
    Actions look;
    look.look = {2.0f, 0.5f};
    for (int i = 0; i < 40; ++i) w->tick(look);
    ASSERT_FALSE(w->grappleTarget());
    Actions q;
    q.grapple = q.recenter = true;
    w->tick(q);
    EXPECT_NEAR(std::remainder(w->camera().yaw - w->player().state().yaw, 6.2831853f), 0.0f, 0.02f);
}

// Field camera (virtual button 13): the press switches to her eye view, she holds still, forward zooms in; the next
// press ends it.
TEST(Game, FieldCameraTogglesEyeView) {
    auto w = makeWorld(poolMesh(), {-1500, 0, 0}, 0.0f);
    for (int i = 0; i < 30; ++i) w->tick({}); // landed from the spawn
    Actions c;
    c.fieldCamera = true;
    w->tick(c);
    ASSERT_TRUE(w->fieldCamera()) << game::stateName(w->player().state().state);
    const vec3 pos = w->player().state().pos;
    EXPECT_NEAR(w->camera().position.z - pos.z, w->targeting.eyeHeight, 1.0f);
    EXPECT_LT(glm::distance(vec2(w->camera().position), vec2(pos)), 40.0f);
    Actions zoom = forward();
    zoom.fieldCamera = true; // still held
    for (int i = 0; i < 60; ++i) w->tick(zoom); // zooms, does not walk
    EXPECT_NEAR(w->fieldZoom(), 1.0f, 1e-3f);
    EXPECT_LT(glm::distance(w->player().state().pos, pos), 1.0f);
    EXPECT_LT(w->camera().fovDeg, 10.0f);
    w->tick(c); // held: no new press
    EXPECT_TRUE(w->fieldCamera());
    w->tick({});
    w->tick(c);
    EXPECT_FALSE(w->fieldCamera());
}

// tru.exe 0x6ACB60: a sink-only current pulls toward its marker at the strength (units per 30 Hz frame), falling off as
// (radius / d)^2 beyond the radius; flag bit 0 keeps it level.
TEST(Game, WaterFlowPullsTowardTheSink) {
    collision::Markup m{10, 1, std::vector<vec3>(game::kWaterIds + 1, vec3(0))};
    m.points[1] = vec3(1000, 1000, 500);                     // surface 500
    m.points[game::kWaterSink] = vec3(1000, 0, 0);
    m.points[game::kWaterFlow] = vec3(47.41f, 0, 0);         // l7_yggdrasil_end's volumes
    m.points[game::kWaterIds] = vec3(0, 1, 0);               // source off, sink marker 1
    const vec3 f = game::waterFlow(m, vec3(0, 0, 0));
    EXPECT_NEAR(f.x, 47.41f * 30, 0.1f);
    EXPECT_NEAR(f.y, 0, 1e-3f);
    m.points[game::kWaterFlow].y = 250; // radius: (250 / 1000)^2
    EXPECT_NEAR(game::waterFlow(m, vec3(0)).x, 47.41f * 30 / 16, 0.1f);
    m.points[game::kWaterIds] = vec3(0, 0, 0); // no markers: still
    EXPECT_EQ(game::waterFlow(m, vec3(0)), vec3(0));
}

// Decoded values pinned against the data: Lara's breath and water-depth classes (lara.drm ObjectBase +0xC8 player
// data, every outfit the same) and the l7 currents (watervolume intro data +0x38..+0x40).
TEST(GameReal, SwimWaterDataDecoded) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto drm = cdc::parseDrm(std::move(*bf.read("pc-w\\lara.drm")));
    const auto obj = std::find_if(drm.sections.begin(), drm.sections.end(), [](const cdc::Section& s) { return s.type == cdc::SectionType::Object; });
    ASSERT_NE(obj, drm.sections.end());
    uint32_t base;
    std::memcpy(&base, obj->data.data(), 4);
    for (uint32_t i = 0; i < drm.sections.size(); ++i) {
        if (drm.sections[i].type != cdc::SectionType::DtpData || drm.sections[i].id != base) continue;
        const auto p = cdc::sectionPointers(drm, i).at(0xC8);
        const auto s16 = [&](uint32_t o) { int16_t v; std::memcpy(&v, drm.sections[p.section].data.data() + p.offset + o, 2); return v; };
        EXPECT_EQ(s16(0xF8), 4000); // World::kAirSeconds = 4000 / 30
        EXPECT_EQ(s16(0xFA), 60);
        EXPECT_EQ(s16(0xFC), 10);
        EXPECT_EQ(s16(0xFE), 20);
        EXPECT_EQ(s16(0xAC), 154);
        EXPECT_EQ(float(s16(0xAE)), game::defaultTuning().swimDepth);
        EXPECT_EQ(s16(0xB0), 391);
        EXPECT_EQ(s16(0xB2), 948);
    }
    const auto ygg = game::findWater(bf, cdc::parseDrm(std::move(*bf.read("pc-w\\l7_yggdrasil_end.drm"))));
    const auto v = std::find_if(ygg.begin(), ygg.end(), [](const collision::Markup& m) { return m.id == 8209; });
    ASSERT_NE(v, ygg.end());
    EXPECT_NEAR(v->points[game::kWaterFlow].x, 47.41f, 0.01f);
    EXPECT_EQ(v->points[game::kWaterIds], vec3(0, 1, 0));
    EXPECT_GT(glm::length(game::waterFlow(*v, v->points[2])), 0.0f);
}

// Wading (tru.exe 0x5D3860 class 0x100: floor 154..391 under the surface): wading_idle standing, wading_walk moving;
// shallower water keeps the on-foot clips.
TEST(Game, WadingClipsAtTheDecodedDepth) {
    for (const float depth : {250.0f, 100.0f}) {
        CollisionMesh m;
        addFloor(m, -5000, -5000, 5000, 5000, 0);
        m.markup.push_back({10, 1, {{-5000, -5000, -2000}, {5000, 5000, depth}}});
        auto anims = syntheticAnims();
        anims.clips["wading_idle"] = rootClip(30, 0);
        anims.clips["wading_walk"] = rootClip(30, 8.0f);
        m.build();
        game::World w(std::move(m), std::move(anims), game::Spawn{{0, 0, 0}, 0});
        for (int i = 0; i < 30; ++i) w.tick({});
        EXPECT_EQ(w.player().state().clip, depth == 250 ? "wading_idle" : "ng_idle") << depth;
        for (int i = 0; i < 30; ++i) w.tick(forward());
        EXPECT_EQ(w.player().state().clip, depth == 250 ? "wading_walk" : "ng_run") << depth;
        EXPECT_LT(w.player().state().water, 0) << "walked, not swum";
    }
}

// Scripted water (watervolumebase natives, tru.exe 0x6AC340 / 0x6C10D0): the water scripts of l7_yggdrasil_end (8309
// SetLevelToMarker on load, 8209 / l4_pool 5608 GetDepth, larabase 3) run their watervolumebase and larabase 3 calls
// without stopping. 8309's instance
// argument is null in our runtime (its instanceref is unbound) and the exe's thunk 0x79E0E0 skips a null one, so its
// level stays: the motion itself is GameReal.ScriptedWaterLevelMoves.
TEST(GameReal, ScriptedWaterLevelsRun) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    for (const std::string unit : {"l7_yggdrasil_end", "l4_pool"}) {
        std::map<std::string, game::UnitData> cache;
        game::World w(game::UnitLoader([&](const std::string& name) {
                          auto& d = cache[name];
                          if (!d.drm) d = game::loadUnitData(bf, name);
                          return d;
                      }),
                      unit, game::loadLaraAnims(bf), game::Spawn{{0, 0, 0}, 0});
        w.script().setBigfile(&bf);
        for (int i = 0; i < 600; ++i) w.tick({});
        for (const auto& [key, count] : w.script().failures())
            EXPECT_TRUE(key.second.find("watervolumebase") == std::string::npos && key.second.find("larabase fn 3") == std::string::npos)
                << unit << " " << key.first << " " << key.second;
    }
}

// SetLevel (watervolumebase 1, 0x7782D0 -> 0x1B4 mode 2): depth and speed in script lengths (x 3.1605 + 0.5, ints): 100
// -> 316 over the volume's bottom, 10 -> 32 units per 30 Hz frame; the surface moves there (0x6C10D0), GetDepth reads
// it back x 0.3164, and a save carries the moving level on bit for bit.
TEST(GameReal, ScriptedWaterLevelMoves) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    std::map<std::string, game::UnitData> cache;
    const auto loader = game::UnitLoader([&](const std::string& name) {
        auto& d = cache[name];
        if (!d.drm) d = game::loadUnitData(bf, name);
        return d;
    });
    game::World w(loader, "l7_yggdrasil_end", game::loadLaraAnims(bf), game::Spawn{{0, 0, 0}, 0});
    const auto surface = [](const game::World& v) {
        for (const auto& m : v.collision().markup)
            if (m.type == 10 && m.id == 8309) return std::pair{m.points[0].z, m.points[1].z};
        return std::pair{0.0f, 0.0f};
    };
    const auto [bottom, top] = surface(w);
    ASSERT_GT(top - bottom, 316 + 32 * 10.0f);
    uint32_t r = 0;
    const int32_t set[] = {100, 10};
    ASSERT_TRUE(w.waterNative(8309, "watervolumebase", 1, set, r));
    for (int i = 0; i < 20; ++i) w.tick({});
    EXPECT_NEAR(top - surface(w).second, 32 * 10.0f, 0.5f) << "32 units per 30 Hz frame for 10 frames";
    const auto saved = w.serializeRuntime();
    for (int i = 0; i < 1200; ++i) w.tick({});
    EXPECT_NEAR(surface(w).second - surface(w).first, 316, 1e-2f) << "landed on the target (streaming may move the unit)";
    ASSERT_TRUE(w.waterNative(8309, "watervolumebase", 4, {}, r));
    EXPECT_NEAR(std::bit_cast<float>(r), 316 * 0.31640562f, 1e-3f);
    game::World l(loader, "l7_yggdrasil_end", game::loadLaraAnims(bf), game::Spawn{{0, 0, 0}, 0});
    l.restoreRuntime(saved);
    EXPECT_NEAR(top - surface(l).second, 32 * 10.0f, 0.5f) << "the level is saved";
}


// ---- poles: parallel hang, hand over hand, turns, swing stops (Player::pole) ----
namespace {
// l4_traprooms pole 1336 (x 14222..15219 at y -20855, z 7133): a standing jump from the floor south of it catches it
// facing +y (GameReal.TrapRoomsPoleFling); the attach clip played out.
game::World trapRooms(const cdc::BigfileSet& bf) { return unitWorld(bf, "l4_traprooms", {{14300, -21200, 6400}, glm::radians(90.0f)}); }
void hangOnPole(game::World& w) {
    const auto& s = w.player().state();
    Actions go = forward();
    go.jump = true;
    for (int i = 0; i < 120 && s.state != State::PoleHang; ++i) w.tick(i < 20 ? go : Actions{});
    for (int i = 0; i < 120 && s.clip != "HPole_idleHangPerp"; ++i) w.tick({});
}
} // namespace

// Stick along the pole: Hpole_PerptoPara turns her parallel (root turn +90), HPole_ParaForward_trans / HPole_ParaForward
// move her hand over hand along it (hands on the axis), letting go stops her (_to_idle_*_hand), the stick back turns her
// round (Hpole_Paraturn180), across turns her back square to it (Hpole_ParatoPerp, mirrored for that side).
TEST(GameReal, TrapRoomsPoleParallelHandOverHandAndTurns) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = trapRooms(bf);
    hangOnPole(w);
    const auto& s = w.player().state();
    ASSERT_EQ(s.state, State::PoleHang);
    ASSERT_EQ(s.clip, "HPole_idleHangPerp");
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1336);
    const float x0 = handsOf(w, w.anims().poleHandOffset).x;
    bool turned = false, moved = false;
    for (int i = 0; i < 150 && !moved; ++i) {
        w.tick(worldStick(w, {1, 0}));
        turned |= s.clip == "Hpole_PerptoPara";
        moved |= s.clip == "HPole_ParaForward";
    }
    EXPECT_TRUE(turned && moved);
    EXPECT_TRUE(s.polePara);
    EXPECT_NEAR(std::cos(s.yaw), 1, 1e-3) << "facing along +x";
    const auto& pole = w.collision().markup[size_t(s.ledge)];
    for (int i = 0; i < 120; ++i) {
        w.tick(worldStick(w, {1, 0}));
        const vec3 h = handsOf(w, w.anims().poleParaHandOffset);
        ASSERT_NEAR(h.y, pole.points[0].y, 1e-2);
        ASSERT_NEAR(h.z, pole.points[0].z, 1e-2);
    }
    const float x1 = handsOf(w, w.anims().poleParaHandOffset).x;
    EXPECT_GT(x1, x0 + 250) << "HPole_ParaForward: 281.7 per 1.37 s";
    bool stopped = false, round = false;
    for (int i = 0; i < 240 && !(round && s.clip == "hpole_IdleHangPara"); ++i) {
        w.tick(worldStick(w, {-1, 0}));
        stopped |= s.clip.starts_with("HPole_ParaForward_to_idle");
        round |= s.clip == "Hpole_Paraturn180";
    }
    EXPECT_TRUE(stopped && round);
    EXPECT_NEAR(std::cos(s.yaw), -1, 1e-3) << "facing -x";
    EXPECT_EQ(s.ledgeSide, -1);
    bool square = false;
    for (int i = 0; i < 150 && !(square && s.clip == "HPole_idleHangPerp"); ++i) {
        w.tick(worldStick(w, {0, -1})); // across, to the side she jumped from
        square |= s.clip == "Hpole_ParatoPerp";
    }
    EXPECT_TRUE(square);
    EXPECT_FALSE(s.polePara);
    EXPECT_NEAR(std::sin(s.yaw), -1, 1e-3) << "facing -y";
    EXPECT_NEAR(handsOf(w, w.anims().poleHandOffset).z, pole.points[0].z, 1e-2);
}

// Swinging and letting go: the swing loop ends at its next phase boundary in the stop clip that starts there
// (Hpole_swingstopA / B / C), then the perpendicular idle.
// tru.exe under Wine: the catch hands over to the idle hang 0.62-0.85 s after it; pulling back across the pole does
// nothing; the turn along it takes 0.30 s.
TEST(GameReal, TrapRoomsPoleAttachBackAndTurnTimes) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = trapRooms(bf);
    const auto& s = w.player().state();
    Actions go = forward();
    go.jump = true;
    int t = 0;
    for (; t < 120 && s.state != State::PoleHang; ++t) w.tick(t < 20 ? go : Actions{});
    ASSERT_EQ(s.state, State::PoleHang);
    int idle = 0;
    for (; idle < 120 && s.clip != "HPole_idleHangPerp"; ++idle) w.tick({});
    EXPECT_NEAR(idle / 60.0, 0.7, 0.05);
    const float yaw0 = s.yaw;
    for (int i = 0; i < 60; ++i) w.tick(worldStick(w, {0, -1}));
    EXPECT_EQ(s.clip, "HPole_idleHangPerp") << "back does nothing";
    EXPECT_NEAR(s.yaw, yaw0, 1e-4);
    w.tick(worldStick(w, {1, 0}));
    ASSERT_EQ(s.clip, "Hpole_PerptoPara");
    int turn = 1;
    for (; turn < 120 && s.clip == "Hpole_PerptoPara"; ++turn) w.tick(worldStick(w, {1, 0}));
    EXPECT_NEAR(turn / 60.0, 0.30, 0.04);
    EXPECT_TRUE(s.polePara);
}

// A jump at a pole in reach flies her root in a straight line, no gravity, onto the hang in 11 frames at 60 Hz
// (tru.exe under Wine on 1336 from this spot: 0.17-0.20 s in the air, 1718 units/s for 315 out) [O].
TEST(GameReal, TrapRoomsJumpToPoleStraightLine) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = trapRooms(bf);
    const auto& s = w.player().state();
    Actions go = forward();
    go.jump = true;
    std::vector<vec3> path;
    for (int t = 0; t < 120 && s.state != State::PoleHang; ++t) {
        w.tick(t < 20 ? go : Actions{});
        if (s.state == State::HangJump) path.push_back(s.pos);
    }
    ASSERT_EQ(s.state, State::PoleHang);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1336);
    ASSERT_GE(path.size(), 2u);
    EXPECT_NEAR(path.size() / 60.0, 0.183, 0.02) << "in the air";
    const vec3 a = path.front(), b = path.back();
    for (const vec3& p : path) EXPECT_LT(glm::length(glm::cross(p - a, glm::normalize(b - a))), 0.5f) << "off the line";
}

// Onto the top of a pole and off it (lara.drm's state chart, Player::poleTop): jump from the idle hang with no stick
// climbs up (hpole_to_perch, root 19 over the axis; Wine 19.9), drop lowers her back onto the hang, jump alone leaps
// straight up (q35 53.73 a frame) and lands back on top, the stick along it leaps that way (q69 / q70: 45 out + 70 up),
// the stick across leans in (_Antic) and jump leaps to the next pole (q33, as the fling).
TEST(GameReal, TrapRoomsPoleTop) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions jump;
    jump.jump = true;
    const auto climbUp = [&](game::World& w) {
        const auto& s = w.player().state();
        w.tick(jump);
        EXPECT_EQ(s.state, State::Perch);
        EXPECT_EQ(s.clip, "hpole_to_perch");
        for (int i = 0; i < 200 && s.clip == "hpole_to_perch"; ++i) w.tick({});
        EXPECT_EQ(s.clip, "hpole_perch_ready");
        EXPECT_NEAR(s.pos.z, 7133 + 19, 2) << "on top: 19 over the axis";
    };
    {
        auto w = trapRooms(bf);
        hangOnPole(w);
        const auto& s = w.player().state();
        ASSERT_EQ(s.clip, "HPole_idleHangPerp");
        const float hangYaw = s.yaw;
        climbUp(w);
        Actions drop;
        drop.drop = true;
        w.tick(drop);
        EXPECT_EQ(s.clip, "hpole_perch_fall_to_hpole");
        for (int i = 0; i < 200 && s.state == State::Perch; ++i) w.tick({});
        ASSERT_EQ(s.state, State::PoleHang);
        EXPECT_EQ(s.clip, "HPole_idleHangPerp");
        EXPECT_NEAR(handsOf(w, w.anims().poleHandOffset).z, 7133, 1);
        EXPECT_NEAR(std::remainder(s.yaw - hangYaw, 6.2831853f), 0, 1e-3);
        for (int i = 0; i < 5; ++i) w.tick({});
        climbUp(w);
        w.tick(jump);
        EXPECT_EQ(s.state, State::Jump);
        EXPECT_EQ(s.clip, "hpole_perch_Jump_up");
        EXPECT_NEAR(s.vel.z, 53.73f * 30, 30) << "q35";
        int air = 1;
        for (; air < 120 && s.state == State::Jump; ++air) w.tick({});
        ASSERT_EQ(s.state, State::Perch) << "back on top";
        EXPECT_NEAR(air / 60.0, 2 * 53.73 / 5.72443 / 30, 0.05);
        EXPECT_NEAR(s.pos.z, 7133 + 19, 1);
        for (int i = 0; i < 30; ++i) w.tick(worldStick(w, {0, 1}));
        EXPECT_EQ(s.clip, "hpole_perch_Jump_forward_Antic");
        Actions go = worldStick(w, {0, 1});
        go.jump = true;
        w.tick(go);
        ASSERT_EQ(s.state, State::HangJump);
        EXPECT_EQ(s.clip, "hpole_perch_Jump_forward");
        for (int i = 0; i < 120 && s.state == State::HangJump; ++i) w.tick({});
        ASSERT_EQ(s.state, State::PoleHang);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1337);
    }
    auto w = trapRooms(bf);
    hangOnPole(w);
    const auto& s = w.player().state();
    climbUp(w);
    Actions side = worldStick(w, {1, 0});
    side.jump = true;
    w.tick(side);
    ASSERT_EQ(s.state, State::Jump);
    EXPECT_TRUE(s.clip == "hpole_perch_Jump_left" || s.clip == "hpole_perch_Jump_right") << s.clip;
    EXPECT_NEAR(glm::length(vec2(s.vel)), 45.0f * 30, 1) << "q69 / q70";
    EXPECT_NEAR(s.vel.x, 45.0f * 30, 1) << "along the stick";
}

TEST(GameReal, TrapRoomsPoleSwingStops) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    for (int hold : {150, 160, 175, 190}) {
        auto w = trapRooms(bf);
    hangOnPole(w);
        const auto& s = w.player().state();
        ASSERT_EQ(s.clip, "HPole_idleHangPerp");
        for (int i = 0; i < hold; ++i) w.tick(worldStick(w, {0, 1}));
        EXPECT_EQ(s.clip, "HPole_swing");
        std::string stop;
        for (int i = 0; i < 240 && s.clip != "HPole_idleHangPerp"; ++i) {
            w.tick({});
            if (s.clip.starts_with("Hpole_swingstop")) stop = s.clip;
        }
        EXPECT_FALSE(stop.empty()) << hold;
        EXPECT_EQ(s.clip, "HPole_idleHangPerp") << hold;
        EXPECT_EQ(s.state, State::PoleHang);
    }
}

// l4_temple perch 1190 on pillar 1191's top: standing on the top, a jump gets on it (perch_attach_ground), turned on the spot left 90 (perch_turn_right_90
// mirrored: the root turns +90) and right 45, down onto the pillar (pillar_attach_perch), back up onto the perch
// (pillar_detach_perch), down again and off at its foot (slide, pillar_detach_ground).
TEST(GameReal, TemplePerchTurnsAndPillarTop) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto w = unitWorld(bf, "l4_temple", {{-6620, -12345, -1700}, 0});
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    for (int i = 0; i < 10 && s.state != State::Perch; ++i) w.tick(i % 2 ? Actions{} : jump);
    ASSERT_EQ(s.state, State::Perch) << game::stateName(s.state) << " at " << s.pos.x << " " << s.pos.y << " " << s.pos.z;
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1190);
    for (int i = 0; i < 120 && s.clip != "perch_idle"; ++i) w.tick({});
    const float yaw0 = s.yaw;
    const vec2 f(std::cos(yaw0), std::sin(yaw0)), l(-f.y, f.x);
    w.tick(worldStick(w, l));
    EXPECT_EQ(s.clip, "perch_turn_right_90");
    EXPECT_TRUE(s.mirror);
    for (int i = 0; i < 90 && s.clip != "perch_idle"; ++i) w.tick({});
    EXPECT_NEAR(std::remainder(s.yaw - yaw0, 6.2831853f), 1.5708f, 0.02f);
    const float yaw1 = s.yaw;
    const vec2 f1(std::cos(yaw1), std::sin(yaw1)), r1(f1.y, -f1.x);
    w.tick(worldStick(w, glm::normalize(f1 + r1)));
    EXPECT_EQ(s.clip, "perch_turn_right_45");
    EXPECT_FALSE(s.mirror);
    for (int i = 0; i < 90 && s.clip != "perch_idle"; ++i) w.tick({});
    EXPECT_NEAR(std::remainder(s.yaw - yaw1, 6.2831853f), -0.754f, 0.02f); // -45 less the 3 skipped start frames
    Actions drop;
    drop.drop = true;
    w.tick(drop);
    ASSERT_EQ(s.state, State::Pillar);
    EXPECT_EQ(s.clip, "pillar_attach_perch");
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1191);
    for (int i = 0; i < 120 && s.clip != "pillar_idle"; ++i) w.tick({});
    EXPECT_NEAR(s.pos.z, -1878 - 315.9f, 1.0f);
    bool top = false;
    for (int i = 0; i < 120 && s.state == State::Pillar; ++i) {
        w.tick(forward());
        top |= s.clip == "pillar_detach_perch";
    }
    EXPECT_TRUE(top);
    ASSERT_EQ(s.state, State::Perch);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 1190);
    w.tick(drop);
    ASSERT_EQ(s.state, State::Pillar);
    for (int i = 0; i < 120 && s.clip != "pillar_idle"; ++i) w.tick({});
    for (int i = 0; i < 300 && s.state == State::Pillar; ++i) w.tick(forward(-1));
    EXPECT_EQ(s.state, State::Idle);
}

// Wall hug on l2_puzzleroom ledge 476 (a 88.5-139 deep shelf against a wall: tru.exe 0x74B920's test passes along
// most of it): walking along the floor onto its first end, bbeam_walk_turn180 turns her back to the wall
// (Wall_Hug_Idle), Wall_Hug_Shimmy_Left (mirrored the other way) carries her along it, its end walks her off.
TEST(GameReal, PuzzleRoomWallHug) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto probe = unitWorld(bf, "l2_puzzleroom", {{0, 0, 0}, 0});
    const auto* m = markupById(probe, 1, 476);
    ASSERT_TRUE(m);
    const vec3 p0 = m->points[0], p1 = m->points[1];
    const vec2 d = glm::normalize(vec2(p1 - p0)), shelf(-d.y, d.x);
    const vec3 start = vec3(vec2(p0) - d * 150.0f + shelf * 88.496f, p0.z + 20);
    auto w = unitWorld(bf, "l2_puzzleroom", {start, std::atan2(d.y, d.x)});
    const auto& s = w.player().state();
    for (int i = 0; i < 120 && s.state != State::WallHug; ++i) w.tick(worldStick(w, d));
    const bool entered = s.clip == "bbeam_walk_turn180";
    for (int i = 0; i < 120 && s.clip != "Wall_Hug_Idle"; ++i) w.tick({});
    ASSERT_EQ(s.state, State::WallHug) << game::stateName(s.state) << " at " << s.pos.x << " " << s.pos.y << " " << s.pos.z;
    EXPECT_TRUE(entered);
    EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 476);
    EXPECT_NEAR(glm::dot(vec2(std::cos(s.yaw), std::sin(s.yaw)), -shelf), 1, 1e-3) << "back to the wall";
    const float t0 = s.ledgeT;
    bool shimmied = false;
    for (int i = 0; i < 90; ++i) {
        w.tick(worldStick(w, d));
        shimmied |= s.clip == "Wall_Hug_Shimmy_Left";
    }
    EXPECT_TRUE(shimmied);
    EXPECT_GT(s.ledgeT + float(s.ledgeSeg) * 1e4f, t0 + 100) << "along the shelf";
    bool off = false;
    for (int i = 0; i < 3000 && s.state == State::WallHug; ++i) {
        w.tick(worldStick(w, -d));
        off |= s.clip == "Wall_Hug_Walk_Off";
    }
    EXPECT_TRUE(off) << "walked off where the shelf ends";
    EXPECT_NE(s.state, State::WallHug);
}

namespace {
constexpr float kHugDepth = 88.496f; // tru.exe player data +0x71C: she stands this far in from the ledge line [I]
// Lara placed in the wall hug on markup `li`, segment `seg`, `t` along it (Wall_Hug_Idle, back to the wall).
void placeHug(game::World& w, int li, int seg, float t) {
    const auto& m = w.collision().markup[size_t(li)];
    const vec3 a = m.points[size_t(seg)], b = m.points[size_t(seg) + 1];
    const vec2 d = glm::normalize(vec2(b - a)), shelf(-d.y, d.x);
    game::PlayerState st = w.player().state();
    st.state = State::WallHug, st.ledge = li, st.ledgeSeg = seg, st.ledgeT = t, st.vel = vec3(0);
    st.pos = glm::mix(a, b, t / glm::length(vec2(b - a))) + vec3(shelf * kHugDepth, 0);
    st.yaw = std::atan2(-shelf.y, -shelf.x), st.clip = "Wall_Hug_Idle", st.loop = true, st.clipTime = 0;
    w.restore(st, st.yaw, -0.25f);
}
vec2 segDirOf(const game::World& w, int li, int seg) {
    const auto& m = w.collision().markup[size_t(li)];
    return glm::normalize(vec2(m.points[size_t(seg) + 1] - m.points[size_t(seg)]));
}
// Shimmying toward a joint with the stick along `way`: the corner clip that starts there and where it leaves her.
struct CornerRun { std::string clip; bool mirror = false; int seg = -1; float yawTurn = 0; float offLine = 1e9f; };
CornerRun shimmyRound(game::World& w, vec2 way) {
    const auto& s = w.player().state();
    const float yaw0 = s.yaw;
    CornerRun r;
    for (int i = 0; i < 900 && s.state == State::WallHug && !s.clip.starts_with("Wall_Hug_Shimmy_to_"); ++i) w.tick(worldStick(w, way));
    r.clip = s.clip, r.mirror = s.mirror;
    for (int i = 0; i < 120 && s.clip.starts_with("Wall_Hug_Shimmy_to_"); ++i) w.tick(worldStick(w, way));
    r.seg = s.ledgeSeg;
    r.yawTurn = glm::degrees(std::remainder(s.yaw - yaw0, 6.2831853f));
    const auto& m = w.collision().markup[size_t(s.ledge)];
    const vec3 a = m.points[size_t(s.ledgeSeg)], b = m.points[size_t(s.ledgeSeg) + 1];
    const vec2 d = glm::normalize(vec2(b - a)), shelf(-d.y, d.x);
    r.offLine = std::abs(glm::dot(vec2(s.pos - a), shelf) - kHugDepth);
    return r;
}
} // namespace

// Wall-hug corners (lara.drm chart S1236 / S1237: corner angle 70..110 -> Wall_Hug_Shimmy_to_Concave_left_*, 250..290
// -> _Convex_left_*, the right-hand shimmy mirrored): l7_arctic_sea shelf 2054 turns 90 degrees into its wall (an
// inside corner), l5_valaskjalf 1494 runs round the outside of a block. Shimmying onto the joint plays the corner
// clip, which carries her onto the next segment's stand line turned a quarter round; back the other way the same
// clip mirrored.
TEST(GameReal, WallHugRoundsConcaveAndConvexCorners) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    {
        auto w = unitWorld(bf, "l7_arctic_sea", {{0, 0, 0}, 0});
        const int li = markupIndex(w, 2054);
        ASSERT_GE(li, 0);
        const float len0 = glm::length(vec2(w.collision().markup[size_t(li)].points[1] - w.collision().markup[size_t(li)].points[0]));
        placeHug(w, li, 0, len0 - 150);
        const CornerRun in = shimmyRound(w, segDirOf(w, li, 0));
        EXPECT_EQ(in.clip, "Wall_Hug_Shimmy_to_Concave_left_90");
        EXPECT_FALSE(in.mirror);
        EXPECT_EQ(in.seg, 1);
        EXPECT_NEAR(in.yawTurn, -90, 0.5) << "turned a quarter round, away from the wall that now runs across";
        EXPECT_LT(in.offLine, 1.0f) << "on the next segment's stand line";
        const auto& s = w.player().state();
        EXPECT_EQ(s.state, State::WallHug);
        const float t1 = s.ledgeT;
        for (int i = 0; i < 60; ++i) w.tick(worldStick(w, segDirOf(w, li, 1)));
        EXPECT_GT(s.ledgeT, t1 + 50) << "shuffling on along the new segment";
        placeHug(w, li, 1, 150); // back the other way: the same corner mirrored
        const CornerRun back = shimmyRound(w, -segDirOf(w, li, 1));
        EXPECT_EQ(back.clip, "Wall_Hug_Shimmy_to_Concave_left_90");
        EXPECT_TRUE(back.mirror);
        EXPECT_EQ(back.seg, 0);
        EXPECT_NEAR(back.yawTurn, 90, 0.5);
        EXPECT_LT(back.offLine, 1.0f);
    }
    {
        auto w = unitWorld(bf, "l5_valaskjalf", {{0, 0, 0}, 0});
        const int li = markupIndex(w, 1494);
        ASSERT_GE(li, 0);
        const auto& m = w.collision().markup[size_t(li)];
        placeHug(w, li, 0, glm::length(vec2(m.points[1] - m.points[0])) - 400);
        const CornerRun out = shimmyRound(w, segDirOf(w, li, 0));
        EXPECT_EQ(out.clip, "Wall_Hug_Shimmy_to_Convex_left_90");
        EXPECT_FALSE(out.mirror);
        EXPECT_EQ(out.seg, 1);
        EXPECT_NEAR(out.yawTurn, 90, 0.5) << "round the outside of the block";
        EXPECT_LT(out.offLine, 1.0f);
        EXPECT_EQ(w.player().state().state, State::WallHug);
        placeHug(w, li, 2, 400);
        const CornerRun back = shimmyRound(w, -segDirOf(w, li, 2));
        EXPECT_EQ(back.clip, "Wall_Hug_Shimmy_to_Convex_left_90");
        EXPECT_TRUE(back.mirror);
        EXPECT_EQ(back.seg, 1);
        EXPECT_NEAR(back.yawTurn, -90, 0.5);
    }
}

namespace {
// A shelf against a wall: ledge 0 along +x at z 0 (its top, the left side, is the floor before a wall 95 in), the
// floor everywhere at z 0 but `lipOnly` (only a strip 60..95 in: nothing under the point 47 in front of her). Real
// lara.drm clips.
game::World hugWorld(const cdc::BigfileSet& bf, std::vector<collision::Markup> extra, bool lipOnly = false,
                     std::vector<std::pair<vec3, vec3>> boxes = {}) {
    CollisionMesh m;
    if (lipOnly) addFloor(m, -1200, 60, 1200, 95, 0);
    else addFloor(m, -4000, -4000, 4000, 4000, 0);
    addBox(m, {-1200, 95, 0}, {1200, 600, 600});
    for (const auto& [lo, hi] : boxes) addBox(m, lo, hi);
    m.markup.push_back({1, 9000, {{-1000, 0, 0}, {0, 0, 0}}});
    for (auto& e : extra) m.markup.push_back(std::move(e));
    m.build();
    return game::World(std::move(m), game::loadLaraAnims(bf), {{-500, 88.5f, 0}, 0});
}
} // namespace

// Jumps off the wall hug by their decoded queries (lara.drm S275's q68: 74.32 that way + 42.54 up a 30 Hz frame from
// 63.21 out of the attach point; S1261's q55: 50.82 straight up, frames 1..17, coming down; both class record
// 0x2A9EC): to the side onto a ledge q68 meets (Wall_Hug_jump_left, mirrored to the right, its take-off, then the
// flight onto the hang), the twist up onto the ledge on top of her wall (Wall_hug_Jump_Up_Twist), and with nothing
// above the somersault on the spot back into the hug. Forward with nothing in reach: the free leap at q68's launch.
TEST(GameReal, WallHugSideAndTwistJumpsByTheirQueries) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions jump;
    jump.jump = true;
    {   // a block 700..1700 along x, its near edge (x 700, top 300) a ledge hanging toward her
        auto w = hugWorld(bf, {{1, 9001, {{700, 95, 300}, {700, -400, 300}}}}, false, {{{700, -400, 0}, {1700, 95, 300}}});
        placeHug(w, 0, 0, 700);
        const auto& s = w.player().state();
        w.tick({});
        const vec2 left(1, 0); // she faces -y: her left is +x
        w.tick(worldStick(w, left, jump));
        EXPECT_EQ(s.clip, "Wall_Hug_jump_left");
        EXPECT_FALSE(s.mirror);
        ASSERT_EQ(s.state, State::HangJump);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 9001) << "the ledge q68 met";
        for (int i = 0; i < 240 && s.state == State::HangJump; ++i) w.tick({});
        EXPECT_EQ(s.state, State::Hang);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 9001);
        EXPECT_TRUE(s.pos.x > 450 && s.pos.x < 700) << s.pos.x << ": hanging in front of the block's face (x 700)";
    }
    {   // the same to the right: nothing there, the free leap at q68's launch after the mirrored take-off
        auto w = hugWorld(bf, {});
        placeHug(w, 0, 0, 500);
        const auto& s = w.player().state();
        w.tick({});
        w.tick(worldStick(w, {-1, 0}, jump));
        EXPECT_EQ(s.clip, "Wall_Hug_jump_left");
        EXPECT_TRUE(s.mirror);
        EXPECT_EQ(s.state, State::Jump);
        for (int i = 0; i < 60 && s.clip == "Wall_Hug_jump_left"; ++i) w.tick({});
        w.tick({});
        EXPECT_NEAR(s.vel.x, -74.32f * 30, 1) << "q68's sideways launch";
    }
    {   // the twist up onto the ledge along the top of her wall (z 600, hanging toward her)
        auto w = hugWorld(bf, {{1, 9002, {{-1200, 95, 600}, {1200, 95, 600}}}});
        placeHug(w, 0, 0, 500);
        const auto& s = w.player().state();
        w.tick({});
        w.tick(jump);
        EXPECT_EQ(s.clip, "Wall_hug_Jump_Up_Twist");
        ASSERT_EQ(s.state, State::HangJump);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 9002);
        for (int i = 0; i < 240 && s.state == State::HangJump; ++i) w.tick({});
        EXPECT_EQ(s.state, State::Hang);
        EXPECT_EQ(w.collision().markup[size_t(s.ledge)].id, 9002);
        EXPECT_NEAR(glm::dot(vec2(std::cos(s.yaw), std::sin(s.yaw)), vec2(0, 1)), 1, 0.05) << "turned round to the wall";
    }
    {   // nothing above: the somersault on the spot, back to Wall_Hug_Idle
        auto w = hugWorld(bf, {});
        placeHug(w, 0, 0, 500);
        const auto& s = w.player().state();
        const vec3 at = s.pos;
        w.tick({});
        w.tick(jump);
        EXPECT_EQ(s.clip, "bbeam_somersault_forward");
        for (int i = 0; i < 120 && s.clip != "Wall_Hug_Idle"; ++i) w.tick({});
        EXPECT_EQ(s.state, State::WallHug);
        EXPECT_EQ(s.clip, "Wall_Hug_Idle");
        EXPECT_LT(glm::distance(s.pos, at), 1.0f);
    }
}

// Drop to water (chart T2069 -> S1270 Wall_Hug_Drop_to_Water; tru.exe 0x7420D0: the ray from 15 over her root and
// 47.41 in front of it down 632.11 must meet water first): over a lip with water 300 below she drops in; where the
// ray meets the shelf's floor the drop does nothing.
TEST(GameReal, WallHugDropToWater) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    Actions drop;
    drop.drop = true;
    {
        auto w = hugWorld(bf, {{10, 9100, {{-3000, -3000, -2000}, {3000, 3000, -300}}}}, true);
        placeHug(w, 0, 0, 500);
        const auto& s = w.player().state();
        w.tick({});
        w.tick(drop);
        EXPECT_EQ(s.clip, "Wall_Hug_Drop_to_Water");
        const auto swimming = [&] { return s.state >= State::SwimSplash && s.state <= State::SwimClimbOut; };
        for (int i = 0; i < 300 && !swimming(); ++i) w.tick({});
        EXPECT_TRUE(swimming()) << game::stateName(s.state);
    }
    {
        auto w = hugWorld(bf, {{10, 9100, {{-3000, -3000, -2000}, {3000, 3000, -300}}}});
        placeHug(w, 0, 0, 500);
        const auto& s = w.player().state();
        w.tick({});
        w.tick(drop);
        EXPECT_EQ(s.state, State::WallHug);
        EXPECT_NE(s.clip, "Wall_Hug_Drop_to_Water");
    }
}

// The wall-hug corner and twist at 30 / 60 / 144 Hz render rates end in the same state.
TEST(GameReal, WallHugIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    auto run = [&](double renderHz) {
        auto w = unitWorld(bf, "l5_valaskjalf", {{0, 0, 0}, 0});
        const int li = markupIndex(w, 1494);
        const auto& m = w.collision().markup[size_t(li)];
        placeHug(w, li, 0, glm::length(vec2(m.points[1] - m.points[0])) - 400);
        const vec2 d0 = segDirOf(w, li, 0);
        engine::FrameClock clock(game::kTickSeconds);
        const uint64_t t0 = w.ticks();
        while (w.ticks() < t0 + 400)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w.ticks() < t0 + 400; --n) {
                const uint64_t t = w.ticks() - t0;
                Actions a = t < 250 ? worldStick(w, d0) : Actions{};
                a.jump = t == 300;
                w.tick(a);
            }
        return std::pair{w.player().state(), w.stateHash()};
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.first.pos, o.first.pos);
        EXPECT_EQ(a.first.clip, o.first.clip);
        EXPECT_EQ(a.second, o.second);
    }
    EXPECT_EQ(a.first.ledgeSeg, 1);
}

// The new traversal at 30 / 60 / 144 Hz render rates ends in the same state (pole: parallel turn, hand over hand,
// swing and stop; then a fling).
TEST(GameReal, PoleTraversalIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto input = [](const game::World& w, uint64_t t) {
        if (t < 20) { Actions a = forward(); a.jump = true; return a; }
        if (t < 200) return Actions{};
        if (t < 400) return worldStick(w, {1, 0});
        if (t < 520) return worldStick(w, {0, 1});
        if (t < 600) return worldStick(w, {0, -1});
        if (t < 700) return worldStick(w, {0, 1});
        return Actions{};
    };
    auto run = [&](double renderHz) {
        auto w = unitWorld(bf, "l4_traprooms", {{14300, -21200, 6400}, glm::radians(90.0f)});
        engine::FrameClock clock(game::kTickSeconds);
        while (w.ticks() < 800)
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w.ticks() < 800; --n) w.tick(input(w, w.ticks()));
        return std::pair{w.player().state(), w.stateHash()};
    };
    const auto a = run(30), b = run(60), c = run(144);
    for (const auto& o : {b, c}) {
        EXPECT_EQ(a.first.pos, o.first.pos);
        EXPECT_EQ(a.first.clip, o.first.clip);
        EXPECT_EQ(a.second, o.second);
    }
    EXPECT_EQ(a.first.state, State::PoleHang);
}

// tru.exe 0x4E76A0: a joint limit's impact fires once, on the step that finds the joint at it after being inside, with
// the speed it came at; a joint created at a limit or resting there fires nothing more. Bodies the game moves sleep
// 0.5 s after they stop [I].
TEST(JointSound, LimitImpactAndWake) {
    game::LimitHit l;
    EXPECT_FALSE(l.update(10, 0, 100, 0.5f, 5));
    EXPECT_EQ(l.update(99.6f, 0, 100, 0.5f, 5), std::optional<float>(5.0f)) << "within the tolerance counts as at the limit";
    l = {};
    EXPECT_FALSE(l.update(50, 0, 100, 0.5f, 30));
    EXPECT_EQ(l.update(100, 0, 100, 0.5f, 30), std::optional<float>(30.0f)) << "the upper limit";
    EXPECT_FALSE(l.update(100, 0, 100, 0.5f, 30)) << "once";
    EXPECT_FALSE(l.update(50, 0, 100, 0.5f, -12)) << "back inside: re-armed";
    EXPECT_EQ(l.update(0, 0, 100, 0.5f, -12), std::optional<float>(-12.0f)) << "the lower limit, its signed speed";
    EXPECT_FALSE(l.update(0, 0, 100, 0.5f, 0)) << "resting there";
    game::WakeTimer w;
    EXPECT_FALSE(w.update(false, 1.0f / 60)) << "never moved";
    EXPECT_TRUE(w.update(true, 1.0f / 60));
    int awake = 0;
    while (w.update(false, 1.0f / 60)) ++awake;
    EXPECT_NEAR(awake, 29, 1) << "0.5 s at 60 Hz";
    EXPECT_FLOAT_EQ(game::jointAngularSpeed(glm::radians(1.0f) / 30.0f), 1.0f);
    EXPECT_FLOAT_EQ(game::jointLinearSpeed(2.0f), 60.0f);
}


// l3_caverns_start: the pillar 963 the training text sends her down to ("drop down and grab the pillar"), its top perch
// 962 (z -4899), and across the cave the upper floor (z ~-4870) with ledge 959 along its edge, ~1200-1400 away. Off the
// perch q37 (74.32 out + 42.54 up a frame) carries her there; the old placeholder (600 out + 1200 up, ~280 across)
// dropped her onto the cave floor 1340 below.
TEST(GameReal, CavernsPerchJumpReaches) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const vec3 perch(8561, -1264, -4899);
    const auto jump = [&](float deg) {
        auto w = unitWorld(bf, "l3_caverns_start", {{7881, -5980, -7244}, 0});
        const float a = glm::radians(deg);
        game::PlayerState st = w.player().state();
        st.state = State::Perch, st.ledge = markupIndex(w, 962), st.ledgeSeg = 0, st.ledgeT = 28, st.pos = perch, st.yaw = a;
        st.clip = "perch_idle", st.vel = vec3(0);
        w.restore(st, a, -0.25f);
        w.tick({});
        const Actions stick = worldStick(w, {std::cos(a), std::sin(a)});
        Actions go = stick;
        go.jump = true;
        w.tick(go);
        EXPECT_EQ(w.player().state().clip, "perch_jump_forward");
        for (int t = 0; t < 240 && (w.player().state().state == State::Jump || w.player().state().state == State::Fall); ++t) w.tick(stick);
        for (int i = 0; i < 30; ++i) w.tick({});
        return std::pair{w.player().state(), w.player().state().ledge >= 0 ? int(w.collision().markup[size_t(w.player().state().ledge)].id) : -1};
    };
    const auto [onFloor, none] = jump(45);
    EXPECT_GT(onFloor.pos.z, -4950) << "on the upper floor, not the cave floor at -6237";
    EXPECT_GT(glm::length(vec2(onFloor.pos - perch)), 1000);
    const auto [hang, ledge] = jump(90);
    EXPECT_EQ(hang.state, State::Hang);
    EXPECT_EQ(ledge, 959);
}

// l3_chapel ring 4744 (ma_ei_grapple_ceiling, its own range 4424.78 in its intro data's 0x404 block) hangs 116 under the
// barrel vault, 6440 over the floor, so the vault and the walls hide it from the floor. The timed run to the chapel doors
// throws at it from the top of the column by the stone angel (ledge 1008 at x -5226; checkpoint 4738 is on it): within
// ~330 of that edge her eye is in range and sees it. A throw from there swings her out under the ring.
TEST(GameReal, ChapelRingFromTheColumnTop) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const float west = 3.1415927f;
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l3_chapel",
                  game::loadLaraAnims(bf), game::Spawn{{-5126, 1900, -3141}, west});
    w.script().setBigfile(&bf);
    const auto place = [&](vec3 pos) {
        game::PlayerState s;
        s.pos = pos, s.yaw = west;
        w.restore(s, west, -0.25f);
        for (int i = 0; i < 5; ++i) w.tick({});
    };
    const auto floor = w.collision().groundProbe({-6700, 1900, -6500}, 1000);
    ASSERT_TRUE(floor);
    place(floor->point);
    EXPECT_FALSE(w.grappleTarget()) << "under the vault, from the chapel floor";
    place({-5126, 1900, -3141});
    const auto t = w.grappleTarget();
    ASSERT_TRUE(t) << "from the column top";
    EXPECT_EQ(t->uid, 4744);
    EXPECT_NEAR(t->range, 4424.78f, 0.01f);
    Actions throwIt;
    throwIt.grapple = true;
    w.tick(throwIt);
    EXPECT_EQ(w.player().state().grappleUid, 4744);
    float minX = 1e9f;
    Actions swing;
    swing.move = {0, 1};
    for (int i = 0; i < 4 * 60; ++i) {
        w.tick(fixedCamera(w, swing, west));
        minX = std::min(minX, w.player().state().pos.x);
    }
    EXPECT_EQ(w.player().state().grappleUid, 4744) << "still on the rope";
    EXPECT_LT(minX, -7000.0f) << "swung out under the ring (x -8697)";
}

// The shared Q (tru.exe's defaults bind Grapple and Recenter Camera both to key 0x90 = 0x80 + DIK_Q; a user config
// writes it as grapple [Q, Pad x], recenter [Q, Pad leftstick]) through the device path: readDevices sets both actions,
// and the World throws at the chapel ring from the column top without swinging the camera; from the chapel floor,
// where nothing is offered (as in the original), the same press recenters.
TEST(GameReal, SharedQThrowsTheGrappleNotTheCamera) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const auto dir = std::filesystem::temp_directory_path() / ("ou_sharedq_" + std::to_string(::getpid()));
    std::filesystem::create_directories(dir);
    {
        std::ofstream(dir / "config.json") << R"({"controls": {"bindings": {"grapple": ["Q", "Pad x"],
                                                "recenter": ["Q", "Pad leftstick"]}}})";
    }
    const config::Config cfg = config::load(dir / "config.json");
    std::filesystem::remove_all(dir);
    EXPECT_EQ(cfg.controls.bindings.at("recenter"), (std::vector<std::string>{"Q", "Pad leftstick"})) << "loaded as written";
    const game::Controls controls(cfg.controls);
    platform::DeviceState d;
    const auto key = [&](bool down) {
        SDL_Event e{};
        e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP, e.key.down = down;
        e.key.scancode = SDL_Scancode(platform::parseInput("Q")->code);
        d.apply(e);
        return game::readDevices(controls, d);
    };
    const Actions q = key(true), none = key(false);
    ASSERT_TRUE(q.grapple && q.recenter);

    const cdc::BigfileSet bf(gameDir());
    const float west = 3.1415927f;
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l3_chapel",
                  game::loadLaraAnims(bf), game::Spawn{{-5126, 1900, -3141}, west});
    w.script().setBigfile(&bf);
    const auto place = [&](vec3 pos, float yaw) {
        game::PlayerState s;
        s.pos = pos, s.yaw = yaw;
        w.restore(s, west, -0.25f);
        for (int i = 0; i < 5; ++i) w.tick(none);
    };
    // The floor: no target in range (6440 under the ring), so Q is Recenter Camera.
    const auto floor = w.collision().groundProbe({-6700, 1900, -6500}, 1000);
    ASSERT_TRUE(floor);
    place(floor->point, 0);
    w.tick(q);
    EXPECT_EQ(w.grapplePress().kind, game::World::GrapplePress::NoTarget);
    EXPECT_TRUE(w.grapplePress().recentered);
    EXPECT_NEAR(w.camera().yaw, 0, 1e-4f) << "swung behind her";
    w.tick(none);
    // The column top: the ring is offered, the press throws and the camera stays where the player had it.
    place({-5126, 1900, -3141}, west);
    const float yaw = w.camera().yaw;
    w.tick(q);
    EXPECT_EQ(w.grapplePress().kind, game::World::GrapplePress::Throw);
    EXPECT_EQ(w.grapplePress().uid, 4744);
    EXPECT_TRUE(w.grapplePress().attached);
    EXPECT_FALSE(w.grapplePress().recentered);
    EXPECT_EQ(w.player().state().grappleUid, 4744);
    EXPECT_NEAR(w.camera().yaw, yaw, 0.05f) << "not recentered";
    w.tick(none);
    w.tick(q); // Q again lets go, still no recenter
    EXPECT_EQ(w.grapplePress().kind, game::World::GrapplePress::Release);
    EXPECT_FALSE(w.grapplePress().recentered);
    EXPECT_EQ(w.player().state().grappleUid, 0);
}

// l3_main_hall ring 4089 over the burning staircase, its own range 3792.67 (0x404 block): thrown at from the end of a
// balcony (z 1709, ~2900 from her eye), as the prologue does; not from the stairs' foot ~4030 away (in g_grapple's
// 4424.78, out of its own).
TEST(GameReal, MainHallRingByItsOwnRange) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l3_main_hall",
                  game::loadLaraAnims(bf), game::Spawn{{2441, 3912, 1709}, 3.1415927f});
    w.script().setBigfile(&bf);
    const auto place = [&](vec3 pos, float yaw) {
        game::PlayerState s;
        s.pos = pos, s.yaw = yaw;
        w.restore(s, yaw, -0.25f);
        for (int i = 0; i < 5; ++i) w.tick({});
    };
    place({2441, 3912, 1709}, 3.1415927f);
    const auto t = w.grappleTarget();
    ASSERT_TRUE(t) << "from the balcony";
    EXPECT_EQ(t->uid, 4089);
    EXPECT_NEAR(t->range, 3792.67f, 0.01f);
    EXPECT_NEAR(t->rope, 3792.67f, 0.01f);
    Actions throwIt;
    throwIt.grapple = true;
    w.tick(throwIt);
    EXPECT_EQ(w.player().state().grappleUid, 4089);
    EXPECT_NEAR(w.player().maxRope(), 3792.67f, 0.01f) << "the rope pays out to the target's own longest";
    w.releaseGrapple(false);
    place({-159, 1712, -38}, 1.5707963f);
    EXPECT_FALSE(w.grappleTarget()) << "~4030 away: out of the ring's own range";
}

// Chimney jumps (src/game/WallJump.cpp, docs/GAMEPLAY.md "Chimney jumps"). The shaft: two walls facing each other, A at
// x = 400 and B at x = -300, over a floor.
namespace {
CollisionMesh shaftMesh() {
    CollisionMesh m;
    addFloor(m, -300, -1000, 400, 1000, 0);
    addBox(m, {400, -1000, -100}, {1000, 1000, 6000});
    addBox(m, {-900, -1000, -100}, {-300, 1000, 6000});
    return m;
}
// A jump with the stick forward (+X, toward A) from the floor; ticks until she is on a wall. False if she never is.
bool jumpAtWall(game::World& w) {
    Actions a = forward();
    a.jump = true;
    w.tick(a);
    for (int i = 0; i < 180 && w.player().state().state != State::WallJump; ++i) w.tick(forward());
    return w.player().state().state == State::WallJump;
}
// One jump press on the wall, then ticks until she is on a wall again (true), or anywhere else.
bool rebound(game::World& w, float* maxStep = nullptr) {
    const auto& s = w.player().state();
    Actions jump;
    jump.jump = true;
    vec3 last = s.pos;
    const auto step = [&](const Actions& a) {
        w.tick(a);
        if (maxStep) *maxStep = std::max(*maxStep, glm::distance(last, s.pos));
        last = s.pos;
    };
    step(jump);
    for (int i = 0; i < 4 && s.state == State::WallJump; ++i) step({}); // the turn on the wall
    for (int i = 0; i < 180 && s.state == State::Jump; ++i) step({});
    return s.state == State::WallJump;
}
} // namespace

// A jump at a wall plants her feet on it 110 out, facing it, at the height she reached; each jump press there leaps
// back off it to the wall behind, higher: from wall to wall 700 apart the jump back (46.81 out + 77.27 up a 30 Hz
// frame) is caught 3 frames short of the far wall, about 490 higher. No step of it is a teleport.
TEST(Game, ChimneyJumpGainsHeightEachRebound) {
    auto w = makeWorld(shaftMesh(), {0, 0, 0});
    const auto& s = w->player().state();
    const auto& t = w->player().tuning();
    ASSERT_TRUE(jumpAtWall(*w)) << game::stateName(s.state);
    for (int i = 0; i < 6; ++i) w->tick({});
    EXPECT_NEAR(s.pos.x, 400 - t.wallJumpStandoff, 0.01f);
    EXPECT_NEAR(std::cos(s.yaw), 1, 1e-4f) << "facing wall A";
    EXPECT_GT(s.pos.z, 100);
    EXPECT_LT(s.pos.z, 320) << "a standing jump's height";
    EXPECT_EQ(s.vel, vec3(0));
    float z = s.pos.z, maxStep = 0;
    for (int k = 1; k <= 4; ++k) {
        ASSERT_TRUE(rebound(*w, &maxStep)) << "rebound " << k << ": " << game::stateName(s.state);
        for (int i = 0; i < 6; ++i) w->tick({});
        const bool onB = k % 2 == 1;
        EXPECT_NEAR(s.pos.x, onB ? -300 + t.wallJumpStandoff : 400 - t.wallJumpStandoff, 0.01f) << "rebound " << k;
        EXPECT_NEAR(std::cos(s.yaw), onB ? -1 : 1, 1e-4f) << "facing the wall she is on";
        EXPECT_NEAR(s.pos.z - z, 486.5f, 45.0f) << "rebound " << k << " rose " << s.pos.z - z;
        EXPECT_EQ(s.wallJumps, k);
        z = s.pos.z;
    }
    EXPECT_GT(z, 1900);
    EXPECT_LT(maxStep, 60) << "flight and the two-frame draw onto the wall: no jump in position";
}

// Without the press she holds on for wall_jump_attach_air's two frames and wall_jump_idle's twelve, then lets go and
// falls to the floor; a press after that does nothing. The press counts from the moment she touches the wall.
TEST(Game, ChimneyJumpNeedsThePress) {
    auto w = makeWorld(shaftMesh(), {0, 0, 0});
    const auto& s = w->player().state();
    ASSERT_TRUE(jumpAtWall(*w));
    const float z = s.pos.z;
    int held = 1;
    for (; held < 200 && s.state == State::WallJump; ++held) {
        w->tick({});
        EXPECT_LE(s.pos.z, z + 0.01f) << "nothing lifts her on the wall";
    }
    EXPECT_NEAR(held, 28, 2) << "14 frames at 60 ticks a second";
    EXPECT_EQ(s.state, State::Fall);
    Actions jump;
    jump.jump = true;
    w->tick(jump);
    EXPECT_EQ(s.state, State::Fall) << "too late";
    for (int i = 0; i < 240 && s.state == State::Fall; ++i) w->tick({});
    for (int i = 0; i < 60; ++i) w->tick({});
    EXPECT_EQ(s.state, State::Idle);
    EXPECT_NEAR(s.pos.z, 0, 0.01f);
    EXPECT_EQ(s.wallJumps, 0);
    // pressed on the very first tick on the wall: the leap
    auto w2 = makeWorld(shaftMesh(), {0, 0, 0});
    ASSERT_TRUE(jumpAtWall(*w2));
    const float z2 = w2->player().state().pos.z;
    ASSERT_TRUE(rebound(*w2));
    EXPECT_GT(w2->player().state().pos.z, z2 + 400);
}

// tru.exe allows five jumps back in a row (player data byte +0x843): on the sixth wall the press does nothing and she
// drops; standing again, the count starts over.
TEST(Game, ChimneyJumpFallsAfterFiveInARow) {
    auto w = makeWorld(shaftMesh(), {0, 0, 0});
    const auto& s = w->player().state();
    ASSERT_TRUE(jumpAtWall(*w));
    for (int k = 1; k <= 5; ++k) ASSERT_TRUE(rebound(*w)) << "rebound " << k;
    EXPECT_EQ(s.wallJumps, 5);
    const float top = s.pos.z;
    EXPECT_GT(top, 2400);
    Actions jump;
    jump.jump = true;
    w->tick({});
    for (int i = 0; i < 60 && s.state == State::WallJump; ++i) {
        w->tick(i % 2 ? jump : Actions{}); // pressing all the while
        EXPECT_LE(s.pos.z, top + 0.01f);
    }
    EXPECT_EQ(s.state, State::Fall);
    EXPECT_EQ(s.wallJumps, 5);
    for (int i = 0; i < 600 && s.state == State::Fall; ++i) w->tick({});
    EXPECT_LT(s.pos.z, 1);
    for (int i = 0; i < 90; ++i) w->tick({});
    EXPECT_EQ(s.wallJumps, 0);
}

// The last jump back flies onto the ledge its jump query (q12) meets: she ends hanging from it, hands on its line.
TEST(Game, ChimneyJumpTopsOutOntoLedge) {
    CollisionMesh m = shaftMesh();
    m.markup.push_back({1, 7, {{400, 600, 1500}, {400, -600, 1500}}}); // on wall A, hanging to its right (-X)
    auto w = makeWorld(std::move(m), {0, 0, 0});
    const auto& s = w->player().state();
    ASSERT_TRUE(jumpAtWall(*w));
    int presses = 0;
    while (presses < 5 && rebound(*w)) ++presses;
    ++presses;
    EXPECT_EQ(presses, 2) << "onto B, then from B onto the ledge";
    for (int i = 0; i < 180 && s.state == State::HangJump; ++i) w->tick({});
    ASSERT_EQ(s.state, State::Hang) << game::stateName(s.state);
    EXPECT_EQ(w->collision().markup[size_t(s.ledge)].id, 7);
    const vec3 hands = handsOf(*w, w->anims().hangHandOffset);
    EXPECT_NEAR(hands.x, 400, 0.01f);
    EXPECT_NEAR(hands.z, 1500, 0.01f);
}

// A wall face tru.exe's test refuses (terrain client flag 0x80, 0x5D3BA0) is no wall to jump off: she never holds on.
TEST(Game, ChimneyJumpRefusesFlaggedWall) {
    CollisionMesh m = shaftMesh();
    for (auto& f : m.faces) f.clientFlags |= 0x80;
    auto w = makeWorld(std::move(m), {0, 0, 0});
    EXPECT_FALSE(jumpAtWall(*w));
    EXPECT_NE(w->player().state().state, State::WallJump);
}

// Saved on the wall mid-chimney and restored into a fresh world, the climb goes on tick for tick as it does unsaved.
TEST(Game, ChimneyJumpSaveLoadContinuesIdentically) {
    auto w = makeWorld(shaftMesh(), {0, 0, 0});
    const auto& s = w->player().state();
    ASSERT_TRUE(jumpAtWall(*w));
    ASSERT_TRUE(rebound(*w));
    w->tick({});
    ASSERT_EQ(s.state, State::WallJump);
    EXPECT_EQ(s.wallJumps, 1);
    auto l = makeWorld(shaftMesh(), {0, 0, 0});
    l->restore(s, w->camera().yaw, w->camera().pitch);
    const auto& r = l->player().state();
    Actions jump;
    jump.jump = true;
    int walls = 0;
    for (int i = 0; i < 400; ++i) {
        // released, then a press on every wall for the first 240 ticks; none after: the fall
        const bool press = i < 240 && s.state == State::WallJump && i % 4 == 1;
        w->tick(press ? jump : Actions{}), l->tick(press ? jump : Actions{});
        ASSERT_EQ(s.state, r.state) << "tick " << i;
        ASSERT_EQ(s.pos, r.pos) << "tick " << i;
        ASSERT_EQ(s.yaw, r.yaw) << "tick " << i;
        ASSERT_EQ(s.vel, r.vel) << "tick " << i;
        ASSERT_EQ(s.clip, r.clip) << "tick " << i;
        ASSERT_EQ(s.clipTime, r.clipTime) << "tick " << i;
        ASSERT_EQ(s.wallJumps, r.wallJumps) << "tick " << i;
        ASSERT_EQ(s.wallTime, r.wallTime) << "tick " << i;
        ASSERT_EQ(s.jumpTime, r.jumpTime) << "tick " << i;
        ASSERT_EQ(s.jumpStart, r.jumpStart) << "tick " << i;
        walls = std::max<int>(walls, s.wallJumps);
    }
    EXPECT_GE(walls, 3) << "the restored climb went on up the shaft";
    EXPECT_EQ(s.state, State::Idle);
}

namespace {
// A chimney climb in a real unit: hanging on ledge `id`'s segment `seg` (its right side, as retail ledges hang), the
// stick away from the wall + jump leaps back; then a jump press on every wall she holds on to, until she hangs again.
struct ChimneyRun {
    int ledge = -1, jumps = 0; // the ledge she ends on (markup id), the jump presses it took
    State end = State::Idle;
    float rise = 0, maxStep = 0;  // her root's gain, the longest step of a tick
    std::vector<vec3> walls;      // where she held on, wall by wall (z relative to the first hang)
    bool airborneOnly = true;     // between the hangs she was only ever jumping or on a wall
};
ChimneyRun climbChimney(const cdc::BigfileSet& bf, const game::AnimLibrary& anims, const char* unit, int id, int seg) {
    ChimneyRun r;
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), unit, anims);
    w.script().setBigfile(&bf);
    const auto& mk = w.collision().markup;
    int li = -1;
    for (size_t i = 0; i < mk.size(); ++i)
        if (mk[i].type == 1 && mk[i].id == id) li = int(i);
    if (li < 0 || mk[size_t(li)].points.size() < size_t(seg) + 2) return r;
    const vec3 a = mk[size_t(li)].points[size_t(seg)], b = mk[size_t(li)].points[size_t(seg) + 1];
    const vec2 d = glm::normalize(vec2(b - a)), out(d.y, -d.x); // her side: the right of the ledge's direction
    game::PlayerState st = w.player().state();
    st.state = State::Hang, st.ledge = li, st.ledgeSeg = seg, st.ledgeT = 0.5f * glm::length(vec2(b - a)), st.ledgeSide = -1;
    st.clip = "ledgeb_idle", st.loop = true, st.clipTime = 0, st.vel = vec3(0);
    w.restore(st, std::atan2(-out.y, -out.x), -0.25f);
    const auto& s = w.player().state();
    for (int i = 0; i < 30; ++i) w.tick({});
    if (s.state != State::Hang) return r;
    const vec3 start = s.pos;
    vec3 last = s.pos;
    const auto tick = [&](const Actions& in) {
        w.tick(in);
        r.maxStep = std::max(r.maxStep, glm::distance(last, s.pos));
        last = s.pos;
    };
    Actions jump;
    jump.jump = true;
    tick(worldStick(w, out, jump));
    r.jumps = 1;
    for (int i = 0; i < 1500 && s.state != State::Hang; ++i) {
        if (s.state == State::WallJump && s.clip == "wall_jump_idle" && s.clipTime > 0.1) {
            r.walls.push_back(vec3(vec2(s.pos), s.pos.z - start.z));
            tick(jump);
            ++r.jumps;
        } else {
            tick({});
        }
        if (s.state != State::Jump && s.state != State::WallJump && s.state != State::HangJump && s.state != State::Hang) {
            r.airborneOnly = false;
            break;
        }
    }
    for (int i = 0; i < 90 && s.state == State::Hang; ++i) w.tick({});
    r.end = s.state;
    r.ledge = s.state == State::Hang && s.ledge >= 0 ? int(mk[size_t(s.ledge)].id) : -1;
    r.rise = s.pos.z - start.z;
    return r;
}
} // namespace

// l2_junglehub (Coastal Thailand, the streambed's square columns): from the hang on ledge 796's east side (column A,
// x = -12659) the higher ledge 797 on the same column, 1248 up, is out of a jump's reach; column B's face is 638 behind
// her. The jump back off the hang plants her on B; jump presses there carry her to A, to B and then from B onto 797:
// four jumps in all, as the original's players count them (Stella's walkthrough, "Climbing Out of the Streambed").
TEST(GameReal, JunglehubChimneyJumpClimbsColumns) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const ChimneyRun r = climbChimney(bf, game::loadLaraAnims(bf), "l2_junglehub", 796, 1);
    ASSERT_EQ(r.end, State::Hang) << game::stateName(r.end);
    EXPECT_TRUE(r.airborneOnly);
    EXPECT_EQ(r.ledge, 797);
    EXPECT_EQ(r.jumps, 4) << "the leap off the hang and three off the walls";
    ASSERT_EQ(r.walls.size(), 3u);
    for (size_t i = 0; i < 3; ++i) // 110 out of column B's face (x ~ -12021) and of column A's (-12659)
        EXPECT_NEAR(r.walls[i].x, i % 2 ? -12659 + 110 : -12021 - 110, 6.0f) << "wall " << i;
    EXPECT_NEAR(r.walls[0].z, 138, 15) << "on B, a little above the hang";
    EXPECT_NEAR(r.walls[1].z - r.walls[0].z, 435, 20) << "a rebound across the 418 between the holds";
    EXPECT_NEAR(r.walls[2].z - r.walls[1].z, 435, 20);
    EXPECT_NEAR(r.rise, 7597 - 6349, 2.0f);
    EXPECT_LT(r.maxStep, 80) << "no teleport on the way up";
}

// l5_valaskjalf (Jan Mayen): ledge 1512 to ledge 1510 on the same wall, 1893 up, the facing wall 822 away: the leap
// back and five wall jumps, all that tru.exe allows in a row (player data byte +0x843), each ~520 higher. And
// l2_towerroom: from ledge 418 a single kick off the wall 603 behind her reaches ledge 417, 553 up.
TEST(GameReal, ValaskjalfChimneyJumpTakesAllFive) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const ChimneyRun r = climbChimney(bf, anims, "l5_valaskjalf", 1512, 1);
    ASSERT_EQ(r.end, State::Hang) << game::stateName(r.end);
    EXPECT_TRUE(r.airborneOnly);
    EXPECT_EQ(r.ledge, 1510);
    EXPECT_EQ(r.jumps, 6);
    ASSERT_EQ(r.walls.size(), 5u);
    for (size_t i = 1; i < 5; ++i) EXPECT_GT(r.walls[i].z, r.walls[i - 1].z + 380) << "wall " << i;
    EXPECT_NEAR(r.rise, 1893, 3.0f);
    EXPECT_LT(r.maxStep, 80);
    const ChimneyRun t = climbChimney(bf, anims, "l2_towerroom", 418, 0);
    ASSERT_EQ(t.end, State::Hang) << game::stateName(t.end);
    EXPECT_EQ(t.ledge, 417);
    EXPECT_EQ(t.jumps, 2);
    EXPECT_NEAR(t.rise, 553, 3.0f);
}

// ---- water gameplay (GpWater) ----
namespace {
// A streamed World of `unit` (scripts, enemies, water volumes) with Lara at `spawn`.
struct UnitGame {
    std::map<std::string, game::UnitData> cache;
    game::UnitLoader loader;
    game::World w;
    UnitGame(const cdc::BigfileSet& bf, const char* unit, game::Spawn spawn, bool enemies = false)
        : loader([this, &bf](const std::string& name) {
              auto& d = cache[name];
              if (!d.drm) d = game::loadUnitData(bf, name);
              return d;
          }),
          w(loader, unit, game::loadLaraAnims(bf), spawn) {
        w.script().setBigfile(&bf);
        if (enemies) w.enemies().setBigfile(&bf);
    }
};
} // namespace

namespace {
// A sea 60 000 deep with real clips: Lara starts treading at the origin.
std::unique_ptr<game::World> deepSeaWorld(const cdc::BigfileSet& bf) {
    CollisionMesh m;
    addQuad(m, {-60000, -60000, -60000}, {60000, -60000, -60000}, {60000, 60000, -60000}, {-60000, 60000, -60000});
    m.markup.push_back({10, 1, {{-60000, -60000, -100150}, {60000, 60000, -150}}});
    m.build();
    auto w = std::make_unique<game::World>(std::move(m), game::loadLaraAnims(bf), game::Spawn{{0, 0, -150}, 0});
    for (int i = 0; i < 90; ++i) w->tick({});
    return w;
}
Actions held(bool drop, bool jump = false, bool aim = false) {
    Actions a;
    a.drop = drop, a.jump = jump, a.aim = aim;
    return a;
}
} // namespace

// Scuba (tru.exe 0x6EEF50 / 0x755230 / 0x7572F0): with the tanks in use the breath does not drain under water and the
// breath meter stays away; without them it drains and the meter takes the adrenaline meter's place while she is under.
TEST(Game, SwimScubaKeepsTheBreath) {
    auto w = makeWorld(poolMesh(), {1000, 0, 400});
    w->outfit = "lara_wetsuit_warm";
    for (int i = 0; i < 90; ++i) w->tick({});
    ASSERT_EQ(w->player().state().state, State::SwimTread);
    EXPECT_TRUE(w->scuba());
    for (int i = 0; i < 60 * 60; ++i) w->tick(held(true));
    ASSERT_TRUE(w->submerged());
    EXPECT_EQ(w->air(), 1.0f) << "a minute under water on the tanks";
    EXPECT_FALSE(w->breathShown());
    EXPECT_EQ(w->health().value(), w->health().max());
    w->outfit = "lara_jungle_a"; // no tanks: the same dive drains it
    EXPECT_FALSE(w->scuba());
    for (int i = 0; i < 60; ++i) w->tick(held(true));
    EXPECT_NEAR(w->air(), 1.0f - 30.0f / 4000.0f, 1e-4f) << "one 30 Hz frame of the 4000 per frame";
    EXPECT_TRUE(w->breathShown());
    for (int i = 0; i < 600 && w->player().state().state != State::SwimTread; ++i) w->tick(held(false, true));
    for (int i = 0; i < 150; ++i) w->tick({});
    EXPECT_EQ(w->air(), 1.0f);
    EXPECT_FALSE(w->breathShown()) << "at the surface with a full meter the adrenaline meter is back";
}

// The surface dive needs water of depth class 0x325 (player data +0xB2 = 948 deep, chart T887): over a floor 800 under
// the surface the crouch button does nothing; over one 1000 under it she dives.
TEST(Game, SwimNoSurfaceDiveInShallowWater) {
    for (const float depth : {800.0f, 1000.0f}) {
        CollisionMesh m;
        addFloor(m, -3000, -3000, 3000, 3000, -150 - depth);
        m.markup.push_back({10, 1, {{-10000, -10000, -100150}, {10000, 10000, -150}}});
        auto w = makeWorld(std::move(m), {0, 0, 200}); // drops in (a spawn within 1000 of a floor is put on it)
        for (int i = 0; i < 180; ++i) w->tick({});
        ASSERT_EQ(w->player().state().state, State::SwimTread) << depth;
        w->tick(held(true));
        EXPECT_EQ(w->player().state().state, depth >= 948 ? State::SwimDive : State::SwimTread) << depth;
    }
}

// Under water the stick's forward share swims along the camera's view line: looking down while swimming forward takes
// her down, looking up takes her up; the stick sideways swims level.
// tru.exe under Wine (scratch/swimsprint/under1.txt: a dive, then forward held): with the camera looking down at her
// (pitch p) her velocity's pitch is p + 9.2 deg below p = -14 (-28.8 at -36.8, -13.4 at -22.5), the lead falling to
// nothing at +8.5: she goes on down (1034 units), levels out within ~3.5 s as the camera trails behind her and ends
// rising gently (velocity pitch 8.2 at camera pitch 7.5).
TEST(Game, SwimFollowsTheCameraPitchUnderWater) {
    CollisionMesh m;
    addQuad(m, {-60000, -60000, -60000}, {60000, -60000, -60000}, {60000, 60000, -60000}, {-60000, 60000, -60000});
    m.markup.push_back({10, 1, {{-60000, -60000, -100150}, {60000, 60000, -150}}});
    auto w = makeWorld(std::move(m), {0, 0, -150});
    const auto& s = w->player().state();
    for (int i = 0; i < 90; ++i) w->tick({});
    // down until the camera looks at her from 37 deg above, as after tru.exe's dive (the synthetic dive clip does not move)
    for (int i = 0; i < 120 && w->camera().pitch > glm::radians(-29.0f); ++i) w->tick(held(true));
    for (int i = 0; i < 90; ++i) w->tick({});
    ASSERT_EQ(s.state, State::SwimUnder);
    ASSERT_NEAR(glm::degrees(w->camera().pitch), -36.8f, 6.0f);
    const auto lead = [](float p) { // degrees
        return 9.2f * std::sqrt(std::clamp((8.5f - p) / 22.5f, 0.0f, 1.0f));
    };
    const float z0 = s.pos.z;
    float lowest = z0;
    int level = -1;
    for (int i = 0; i < 10 * 60; ++i) {
        const float p = glm::degrees(w->camera().pitch);
        w->tick(forward());
        lowest = std::min(lowest, s.pos.z);
        if (i >= 30) ASSERT_NEAR(glm::degrees(s.pitch), p + lead(p), 3.0f) << i; // her pitch follows the view line
        if (i == 30) EXPECT_NEAR(glm::degrees(s.pitch), -36.8f + 9.2f, 8.0f) << "tru.exe: -28.8 at -36.8";
        if (level < 0 && i > 30 && s.pitch > 0) level = i;
    }
    EXPECT_NEAR(z0 - lowest, 1034.0f, 350.0f) << "she goes on down first (tru.exe: 1034 at the sprint's speed)";
    ASSERT_GT(level, 0) << "then levels out";
    EXPECT_NEAR(level / 60.0f, 3.5f, 1.5f);
    EXPECT_NEAR(glm::degrees(w->camera().pitch), 8.5f, 2.5f) << "and ends rising gently, the camera right behind her";
    EXPECT_NEAR(glm::degrees(s.pitch), 8.5f, 2.5f);
    // looking down takes her down again, looking up up
    Actions down = forward();
    down.look.y = -1.0f;
    for (int i = 0; i < 40; ++i) w->tick(down);
    float z = s.pos.z;
    for (int i = 0; i < 30; ++i) w->tick(forward());
    EXPECT_LT(s.pos.z, z - 150);
    EXPECT_LT(s.pitch, -0.15f);
    for (int i = 0; i < 180; ++i) w->tick(down); // well under again
    Actions up = forward();
    up.look.y = 1.0f;
    for (int i = 0; i < 150 && w->camera().pitch < 0.5f; ++i) w->tick(up);
    ASSERT_GT(w->camera().pitch, 0.45f);
    z = s.pos.z;
    for (int i = 0; i < 30; ++i) w->tick(forward());
    EXPECT_GT(s.pos.z, z + 250);
    EXPECT_GT(s.pitch, 0.4f);
    Actions side;
    side.move.x = 1;
    for (int i = 0; i < 120; ++i) w->tick(side);
    EXPECT_NEAR(s.pitch, 0.0f, 0.05f) << "the stick sideways swims level whatever the camera's pitch";
}

// Sprinting under water spends the breath 1.5 times as fast (lara.drm chart property 914 of the sprint state S159).
TEST(GameReal, SwimSprintSpendsBreathFaster) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto world = deepSeaWorld(bf);
    auto& w = *world;
    const auto& s = w.player().state();
    for (int i = 0; i < 300; ++i) w.tick(held(true));
    ASSERT_EQ(s.state, State::SwimUnder);
    for (int i = 0; i < 60; ++i) w.tick(forward());
    float a0 = w.air();
    for (int i = 0; i < 120; ++i) w.tick(forward());
    const float swim = a0 - w.air();
    EXPECT_NEAR(swim, 60.0f / 4000.0f, 1e-5f) << "2 s = 60 frames of 4000";
    Actions sprint = forward();
    sprint.sprint = true;
    for (int i = 0; i < 30; ++i) w.tick(sprint);
    ASSERT_EQ(s.clip, "swim_submerged_sprint");
    a0 = w.air();
    for (int i = 0; i < 120; ++i) w.tick(sprint);
    EXPECT_NEAR((a0 - w.air()) / swim, 1.5f, 1e-3f);
}

// Under-water dodges (target lock + crouch or jump): each plays its clip and moves her by its root motion [O clips]:
// no stick or stick up 883.4 up in 30 frames, down 881.2 down in 20, a side 791.6 sideways in 30; side-on to the
// camera the sides are 468.5 ahead (26 frames) / 449.9 back.
TEST(GameReal, SwimDodgesMoveAsTheirClips) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto world = deepSeaWorld(bf);
    auto& w = *world;
    const auto& s = w.player().state();
    for (int i = 0; i < 360; ++i) w.tick(held(true));
    for (int i = 0; i < 60; ++i) w.tick({});
    ASSERT_EQ(s.state, State::SwimUnder);
    const auto dodge = [&](vec2 stick, bool jump, std::string_view clip) {
        const vec3 p0 = s.pos;
        Actions a = held(!jump, jump, true);
        a.move = stick;
        w.tick(held(false, false, true)); // lock held, buttons up: the press edge is the next tick
        w.tick(a);
        EXPECT_EQ(s.state, State::SwimDodge) << clip;
        EXPECT_EQ(s.clip, clip);
        int n = 1;
        for (; n < 200 && s.state == State::SwimDodge; ++n) w.tick(held(false, false, true));
        EXPECT_EQ(s.state, State::SwimUnder) << clip;
        EXPECT_EQ(s.clip, "swim_submerged_idle");
        return std::pair{s.pos - p0, n};
    };
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0), right(std::sin(s.yaw), -std::cos(s.yaw), 0);
    auto [up, upTicks] = dodge({0, 0}, false, "swim_submerged_dodge_up");
    EXPECT_NEAR(up.z, 883.4f, 1.0f);
    EXPECT_NEAR(glm::length(vec2(up)), 0, 1.0f);
    EXPECT_NEAR(upTicks, 60, 2) << "30 frames";
    auto [down, downTicks] = dodge({0, -1}, false, "swim_submerged_dodge_down");
    EXPECT_NEAR(down.z, -881.2f, 1.0f);
    EXPECT_NEAR(downTicks, 40, 2);
    auto [jumpUp, jt] = dodge({0, 1}, true, "swim_submerged_dodge_up"); // the jump button dodges too
    EXPECT_NEAR(jumpUp.z, 883.4f, 1.0f);
    dodge({0, -1}, false, "swim_submerged_dodge_down");
    auto [r, rt] = dodge({1, 0}, false, "swim_submerged_dodge_side");
    EXPECT_NEAR(glm::dot(r, right), 791.6f, 8.0f) << "to her right";
    EXPECT_NEAR(r.z, 7.3f, 1.0f);
    const auto behind = [&] { // the camera back behind her (the dodges read the stick as on screen)
        Actions c = held(false);
        c.recenter = true;
        w.tick(c);
        for (int i = 0; i < 240 && std::abs(std::remainder(w.camera().yaw - s.yaw, 6.2831853f)) > 0.05f; ++i) w.tick({});
    };
    behind();
    ASSERT_LT(std::abs(std::remainder(w.camera().yaw - s.yaw, 6.2831853f)), 0.3f);
    auto [l, lt] = dodge({-1, 0}, false, "swim_submerged_dodge_side");
    EXPECT_NEAR(glm::dot(l, right), -791.6f, 8.0f) << "mirrored to her left";
    // side-on: the camera turned a quarter round her
    Actions turn = held(false, false, false);
    turn.look.x = 2;
    for (int i = 0; i < 200 && std::abs(std::sin(w.camera().yaw - s.yaw)) < 0.95f; ++i) w.tick(turn);
    ASSERT_GT(std::abs(std::sin(w.camera().yaw - s.yaw)), 0.9f);
    const float rel = std::remainder(s.yaw - w.camera().yaw, 6.2831853f); // her heading left (+) or right (-) of the view line
    const vec2 ahead(rel < 0 ? 1.0f : -1.0f, 0);                           // the stick side that runs along her heading
    auto [f, ft] = dodge(ahead, false, "swim_submerged_dodge_forward");
    EXPECT_NEAR(glm::dot(f, fwd), 468.5f, 1.0f);
    EXPECT_NEAR(ft, 52, 2);
    auto [b, bt] = dodge(-ahead, false, "swim_submerged_dodge_backward");
    EXPECT_NEAR(glm::dot(b, fwd), -449.9f, 1.0f);
    // no dodge without the lock: crouch just swims down
    w.tick({});
    w.tick(held(true));
    EXPECT_EQ(s.state, State::SwimUnder);
}

// Swan dive: a running jump, crouch while it still rises. Into deep water from any height: swim_swan_land carries her
// 235 on and 346 down (its root motion from the request's start frame), unhurt. Onto the ground she rolls out of it (ng_swan_land, 591 ahead) and the fall counts as
// any other: a short drop is free, a long one kills (death_fall_swan).
TEST(GameReal, SwanDiveIntoWaterAndOntoLand) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto run = [&](float ground, bool water) { // a platform at z 0 ending at x 0; ground / water beyond
        CollisionMesh m;
        addFloor(m, -4000, -1000, 0, 1000, 0);
        addFloor(m, 0, -1000, 9000, 1000, ground);
        if (water) m.markup.push_back({10, 1, {{0, -1000, ground}, {9000, 1000, -6000}}});
        m.build();
        auto w = std::make_unique<game::World>(std::move(m), game::loadLaraAnims(bf), game::Spawn{{-1500, 0, 0}, 0});
        const auto& s = w->player().state();
        for (int i = 0; i < 300 && s.pos.x < -260; ++i) w->tick(forward());
        Actions jump = forward();
        jump.jump = true;
        w->tick(jump);
        for (int i = 0; i < 60 && s.state != State::Jump; ++i) w->tick(forward());
        EXPECT_EQ(s.state, State::Jump);
        Actions crouch = forward();
        crouch.drop = true;
        w->tick(crouch);
        EXPECT_EQ(s.clip, "ng_inair_swan_stage1");
        return w;
    };
    { // 6000 down into deep water
        auto w = run(-20000, true);
        const auto& s = w->player().state();
        bool stage3 = false;
        for (int i = 0; i < 600 && s.state == State::Jump; ++i) w->tick({}), stage3 |= s.clip == "ng_inair_swan_stage3";
        EXPECT_TRUE(stage3) << "stage 1 (25 frames), stage 2 (6), then the stage 3 hold";
        ASSERT_EQ(s.state, State::SwimSplash);
        EXPECT_EQ(s.clip, "swim_swan_land");
        const vec3 in = s.pos;
        int n = 0;
        for (; n < 200 && s.state == State::SwimSplash; ++n) w->tick({});
        EXPECT_EQ(s.state, State::SwimUnder);
        // the clip's root motion (268.5 on, 437.7 down over 31 frames) from its request's start frame 4
        EXPECT_NEAR(s.pos.x - in.x, 235.0f, 2.0f);
        EXPECT_NEAR(s.pos.z - in.z, -346.1f, 2.0f);
        EXPECT_NEAR(n, 54, 2) << "27 frames";
        EXPECT_EQ(w->health().value(), w->health().max()) << "no fall damage into deep water";
    }
    { // 250 down onto the ground: the roll
        auto w = run(-250, false);
        const auto& s = w->player().state();
        for (int i = 0; i < 600 && s.state == State::Jump; ++i) w->tick({});
        ASSERT_EQ(s.state, State::Roll);
        EXPECT_EQ(s.clip, "ng_swan_land");
        const float x = s.pos.x;
        for (int i = 0; i < 60 && s.state == State::Roll; ++i) w->tick({});
        EXPECT_EQ(s.state, State::Idle);
        EXPECT_NEAR(s.pos.x - x, 591.0f * (9.0f / 11.0f), 60.0f) << "the roll's root motion from its start frame 2";
        EXPECT_EQ(w->health().value(), w->health().max());
    }
    { // 6000 down onto the ground: dead
        auto w = run(-6000, false);
        const auto& s = w->player().state();
        for (int i = 0; i < 600 && s.state == State::Jump; ++i) w->tick({});
        for (int i = 0; i < 5; ++i) w->tick({});
        EXPECT_TRUE(w->health().dead());
        EXPECT_EQ(s.state, State::Dead);
        EXPECT_EQ(s.clip, "death_fall_swan");
    }
}

// At the surface, treading: the stick to her side plays swim_surface_idle_turn_90 (a quarter turn in 15 frames, mirrored
// to the left), behind her swim_surface_idle_turn_180; then she swims off that way.
TEST(GameReal, SwimSurfaceIdleTurns) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto turn = [&](vec2 stick, std::string_view clip, float by) {
        const auto world = deepSeaWorld(bf);
        auto& w = *world;
        const auto& s = w.player().state();
        EXPECT_EQ(s.state, State::SwimTread);
        const float yaw0 = s.yaw;
        const vec3 p0 = s.pos;
        Actions a;
        a.move = stick;
        w.tick(fixedCamera(w, a, yaw0));
        EXPECT_EQ(s.clip, clip);
        int n = 1;
        for (; n < 120 && s.clip == clip; ++n) w.tick(fixedCamera(w, a, yaw0));
        EXPECT_NEAR(n, 30, 3) << clip << ": 15 frames";
        EXPECT_NEAR(std::remainder(s.yaw - yaw0 - by, 6.2831853f), 0, 0.12f) << clip;
        EXPECT_LT(glm::distance(vec2(s.pos), vec2(p0)), 60.0f) << "turned on the spot";
    };
    turn({1, 0}, "swim_surface_idle_turn_90", -1.5708f);
    turn({-1, 0}, "swim_surface_idle_turn_90", 1.5708f);
    turn({0, -1}, "swim_surface_idle_turn_180", 3.14159f);
}

// ---- real units ----

// l1_seafloor (Mediterranean dive, lara_wetsuit_warm): a 150 s swim under water on the tanks leaves the breath full and
// the meter hidden; a dodge in the open sea moves her as its clip.
TEST(GameReal, SeafloorScubaSwimKeepsTheBreath) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l1_seafloor", {{24142, -6344, -1500}, 0});
    auto& w = g.w;
    const auto& s = w.player().state();
    EXPECT_EQ(w.outfit, "lara_wetsuit_warm");
    ASSERT_TRUE(w.scuba());
    for (int i = 0; i < 30; ++i) w.tick({});
    ASSERT_EQ(s.state, State::SwimUnder);
    const float z = s.pos.z;
    w.tick(held(false, false, true));
    w.tick(held(true, false, true));
    ASSERT_EQ(s.state, State::SwimDodge);
    for (int i = 0; i < 100 && s.state == State::SwimDodge; ++i) w.tick(held(false, false, true));
    EXPECT_NEAR(s.pos.z - z, 883.4f, 1.0f) << "swim_submerged_dodge_up in the real sea";
    for (int i = 0; i < 60; ++i) w.tick(held(true)); // back down
    int under = 0;
    for (int i = 0; i < 150 * 60; ++i) {
        Actions a;
        a.move.x = (i / 600) % 2 ? 1.0f : 0.0f; // idling and circling, to stay in open water
        w.tick(a);
        under += w.submerged();
        ASSERT_FALSE(w.breathShown()) << i;
    }
    EXPECT_GT(under, 149 * 60);
    EXPECT_EQ(w.air(), 1.0f) << "4000 frames (133.3 s) would have emptied it";
    EXPECT_NE(w.health().lastType(), game::kDamagePeriodic);
}

// l2_cliffclimb (Coastal Thailand, lara_jungle_a: no tanks): under the river she holds her breath 4000 frames; from then
// the meter asks for 10 % of her health every 20 frames, every other one of which falls in the health's 30-frame
// invulnerability window (0x6F5030), so she loses 10 % every 40 frames and drowns (death_drown) 146 s after going under.
TEST(GameReal, ThailandDiveDrownsAtTheBreathTime) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l2_cliffclimb", {{-21800, 10762, -2000}, 0});
    auto& w = g.w;
    const auto& s = w.player().state();
    EXPECT_EQ(w.outfit, "lara_jungle_a");
    ASSERT_FALSE(w.scuba());
    for (int i = 0; i < 30; ++i) w.tick({});
    ASSERT_EQ(s.state, State::SwimUnder);
    ASSERT_TRUE(w.submerged());
    EXPECT_TRUE(w.breathShown());
    int firstHurt = -1, dead = -1;
    for (int i = 0; i < 150 * 60 && dead < 0; ++i) {
        w.tick({});
        if (firstHurt < 0 && w.health().value() < w.health().max()) firstHurt = i;
        if (w.health().dead()) dead = i;
    }
    ASSERT_GE(firstHurt, 0);
    EXPECT_NEAR((firstHurt + 30) / 60.0f, 4020 / 30.0f, 0.1f) << "4000 frames of breath + the first 20-frame period";
    ASSERT_GE(dead, 0);
    EXPECT_NEAR((dead - firstHurt) / 60.0f, 9 * 40 / 30.0f, 0.1f) << "ten 10 % losses 40 frames apart";
    for (int i = 0; i < 3; ++i) w.tick({});
    EXPECT_EQ(s.state, State::Dead);
    EXPECT_EQ(s.clip, "death_drown");
}

// l1_seafloor: a blue shark comes for the swimming Lara; the speargun's spears fly to it (pr_spear, 550 a hit under
// water) and kill it.
TEST(GameReal, SeafloorSpearKillsShark) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame probe(bf, "l1_seafloor", {{24142, -6344, -1500}, 0}, true);
    probe.w.tick({});
    vec3 at(0);
    int32_t uid = 0;
    for (const auto& e : probe.w.enemies().all())
        if (e->type().name.starts_with("shark_") && !uid) uid = e->uid(), at = e->state().pos;
    ASSERT_NE(uid, 0) << "no shark placed";
    UnitGame g(bf, "l1_seafloor", {at + vec3(1500, 0, 0), glm::pi<float>()}, true);
    auto& w = g.w;
    w.giveWeapon(game::Weapon::Speargun, 15);
    const auto shark = [&]() -> const game::ai::Enemy* {
        for (const auto& e : w.enemies().all())
            if (e->uid() == uid) return e.get();
        return nullptr;
    };
    for (int i = 0; i < 10; ++i) w.tick({});
    ASSERT_TRUE(shark());
    const float hp = shark()->state().health;
    ASSERT_GT(hp, 0);
    Actions sw;
    sw.weapon = true;
    w.tick(sw);
    Actions fire;
    fire.fire = fire.aim = true;
    int flights = 0, t = 0;
    bool hurtAtLaunch = false;
    std::vector<float> losses;
    for (; t < 60 * 60 && shark() && shark()->state().mode != game::ai::Mode::Dead; ++t) {
        const size_t before = w.combat().projectiles.size();
        const float h0 = shark()->state().health;
        w.tick(fire);
        const float h1 = shark()->state().health;
        if (w.combat().projectiles.size() > before) ++flights, hurtAtLaunch |= h1 < h0;
        if (h1 < h0) losses.push_back(h0 - h1);
        if (w.health().dead()) break;
    }
    ASSERT_TRUE(shark());
    EXPECT_EQ(w.combat().weapon, game::Weapon::Speargun);
    EXPECT_EQ(shark()->state().mode, game::ai::Mode::Dead) << "after " << t << " ticks, " << flights << " spears";
    EXPECT_GE(flights, 1);
    EXPECT_FALSE(hurtAtLaunch) << "a spear hurts when it arrives, not the tick it is fired";
    ASSERT_FALSE(losses.empty());
    for (size_t i = 0; i + 1 < losses.size(); ++i) EXPECT_EQ(losses[i], 550.0f) << "pr_spear's under-water record";
    EXPECT_LE(losses.back(), 550.0f) << "the last one takes what was left";
    EXPECT_LE(flights, int(hp / 550.0f) + 2) << "it took the spears its health asks for";
}

// l2_cliffclimb: from the cliff top 5950 over the sea a running jump and crouch is a swan dive; she enters the sea on
// swim_swan_land, unhurt.
TEST(GameReal, CliffSwanDiveIntoTheSea) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l2_cliffclimb", {{-29956 + 900, 2361, 6100}, glm::pi<float>()});
    auto& w = g.w;
    const auto& s = w.player().state();
    for (int i = 0; i < 60; ++i) w.tick({});
    ASSERT_EQ(s.state, State::Idle) << "standing on the cliff top";
    const float top = s.pos.z;
    EXPECT_GT(top, 5900.0f);
    const auto& col = w.collision();
    int i = 0;
    for (; i < 600; ++i) { // run west until the ground ahead ends
        w.tick(forward());
        if (s.state == State::Run && !col.groundProbe(s.pos + vec3(-420, 0, 100), 400)) break;
    }
    ASSERT_LT(i, 600);
    Actions jump = forward();
    jump.jump = true;
    w.tick(jump);
    int prep = 0;
    for (; prep < 60 && s.state != State::Jump; ++prep) w.tick(forward());
    ASSERT_EQ(s.state, State::Jump);
    Actions crouch = forward();
    crouch.drop = true;
    w.tick(crouch);
    ASSERT_EQ(s.clip, "ng_inair_swan_stage1");
    for (int k = 0; k < 900 && s.state == State::Jump; ++k) w.tick({});
    ASSERT_EQ(s.state, State::SwimSplash) << "at " << s.pos.x << " " << s.pos.y << " " << s.pos.z;
    EXPECT_EQ(s.clip, "swim_swan_land");
    EXPECT_GT(top - s.pos.z, 5500.0f);
    for (int k = 0; k < 200 && s.state == State::SwimSplash; ++k) w.tick({});
    EXPECT_TRUE(s.state == State::SwimUnder || s.state == State::SwimTread);
    EXPECT_EQ(w.health().value(), w.health().max()) << "a 5900-unit fall into the sea costs nothing";
}

// l7_yggdrasil_end volume 8309: SetLevel (watervolumebase 1) lowers the surface by script; Lara treading on it goes
// down with it, 16 units per 30 Hz frame, and stays afloat.
TEST(GameReal, ScriptedWaterLevelCarriesLara) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l7_yggdrasil_end", {{-9473, -9100, 2869}, 0});
    auto& w = g.w;
    const auto& s = w.player().state();
    const auto surface = [&] {
        for (const auto& m : w.collision().markup)
            if (m.type == 10 && m.id == 8309) return std::pair{m.points[0].z, m.points[1].z};
        return std::pair{0.0f, 0.0f};
    };
    for (int i = 0; i < 120; ++i) w.tick({});
    ASSERT_EQ(s.state, State::SwimTread);
    const auto [bottom, top] = surface();
    EXPECT_NEAR(s.pos.z, top - w.player().tuning().swimSurfaceDepth, 0.5f);
    uint32_t r = 0;
    const float target = top - 1000;
    const int32_t set[] = {int32_t((target - bottom) / 3.1605f), 5}; // script lengths: depth over the bottom, speed
    ASSERT_TRUE(w.waterNative(8309, "watervolumebase", 1, set, r));
    for (int i = 0; i < 60; ++i) {
        w.tick({});
        ASSERT_EQ(s.state, State::SwimTread) << i;
        // within one 60 Hz tick of the level's motion (the player ticks before the volume moves)
        ASSERT_NEAR(s.pos.z, surface().second - w.player().tuning().swimSurfaceDepth, 8.5f) << i;
    }
    EXPECT_NEAR(top - surface().second, 16 * 30.0f, 1.0f) << "16 units per 30 Hz frame for a second";
    for (int i = 0; i < 240; ++i) w.tick({});
    EXPECT_NEAR(surface().second, bottom + float(set[0]) * 3.1605f, 2.0f);
    EXPECT_EQ(s.state, State::SwimTread);
    EXPECT_NEAR(s.pos.z, surface().second - w.player().tuning().swimSurfaceDepth, 0.5f) << "she floats on the new level";
}

// l1_seafloor jellyfish 817 (jellyfish_a, a class-79 uber object; script type 5594): three player sensors on its HInfo
// markers 10 / 20 / 30 (radii 278.1 / 208.6 / 192.8 around the bell and down the tentacles). While Lara is inside one
// its script runs the shock command and takes 100 health, again and again; away from it nothing happens; shots do
// nothing to it (its section takes no damage). A save carries the sensors on.
TEST(GameReal, JellyfishStingOnTouch) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    {
        UnitGame far(bf, "l1_seafloor", {{-4680, 8992, 1243 - 900}, 0});
        for (int i = 0; i < 600; ++i) far.w.tick({});
        EXPECT_EQ(far.w.health().value(), far.w.health().max()) << "900 under it: out of every sensor";
    }
    UnitGame g(bf, "l1_seafloor", {{-4680, 8850, 1532}, 0}); // 160 from the middle marker
    auto& w = g.w;
    std::vector<int> hurtAt;
    float h = w.health().value();
    nlohmann::json saved;
    for (int i = 0; i < 600; ++i) {
        w.tick({});
        if (w.health().value() < h - 50) {
            hurtAt.push_back(i);
            EXPECT_NEAR(h - w.health().value(), 100.0f, 30.0f) << i;
        }
        h = w.health().value();
        if (i == 30) saved = w.serializeRuntime();
    }
    ASSERT_GE(hurtAt.size(), 3u) << "stung again and again";
    EXPECT_LT(hurtAt[0], 5) << "at once";
    EXPECT_FALSE(w.health().dead());
    for (const auto& [key, count] : w.script().failures()) EXPECT_NE(key.first, 817) << key.second;
    // swimming away ends it
    const float left = w.health().value();
    Actions away;
    away.move.y = -1;
    for (int i = 0; i < 240; ++i) w.tick(away);
    const float after = w.health().value();
    for (int i = 0; i < 300; ++i) w.tick({});
    EXPECT_GE(w.health().value(), after) << "out of reach: no more stings";
    EXPECT_GT(glm::distance(w.player().state().pos, vec3(-4675, 9008, 1532)), 400.0f);
    (void)left;
    // a loaded game goes on being stung at the same ticks
    UnitGame l(bf, "l1_seafloor", {{-4680, 8850, 1532}, 0});
    for (int i = 0; i <= 30; ++i) l.w.tick({});
    l.w.restoreRuntime(saved);
    std::vector<int> again;
    float lh = l.w.health().value();
    for (int i = 31; i < 600; ++i) {
        l.w.tick({});
        if (l.w.health().value() < lh - 50) again.push_back(i);
        lh = l.w.health().value();
    }
    std::vector<int> expect;
    for (const int t : hurtAt)
        if (t > 30) expect.push_back(t);
    EXPECT_EQ(again, expect);
}

// ---- class-30 breakables, the prologue's collapsing floor (docs/GAMEPLAY.md "Breakables") ----------------------

// Breakable records [O data]: ar_rb_falling_floor_straight breaks 23 frames after Lara stands on it (condition flag 8),
// the ledges after 105 / 120 frames of hanging (0x40), the pole after 176 on it (0x20); the temple floor takes 3 hits of
// damage class 6.
TEST(GameReal, BreakableRecords) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto load = [&](const char* name) { return game::loadBreakable(cdc::parseDrm(*bf.read(std::string("pc-w\\") + name + ".drm"))); };
    const auto floor = load("ar_rb_falling_floor_straight");
    ASSERT_TRUE(floor);
    EXPECT_EQ(floor->hits, 3);
    ASSERT_EQ(floor->pieces.size(), 4u);
    ASSERT_EQ(floor->conditions.size(), 1u);
    EXPECT_EQ(floor->conditions[0].flags, 0x08u);
    ASSERT_EQ(floor->conditions[0].action, 0);
    EXPECT_EQ(floor->actions[0].type, 2);
    EXPECT_FLOAT_EQ(floor->actions[0].delay, 23);
    EXPECT_EQ(floor->actions[0].shake, vec3(4, 4, 10));
    for (const auto& p : floor->pieces) EXPECT_FALSE(p.physics.prims.empty());
    const auto ledge = load("ar_ei_break_braced_ledge");
    ASSERT_TRUE(ledge);
    EXPECT_EQ(ledge->conditions[0].flags, 0x40u);
    EXPECT_FLOAT_EQ(ledge->actions[0].delay, 105);
    EXPECT_TRUE(ledge->actions[0].flags & 0x10);
    size_t markup = 0;
    for (const auto& p : ledge->pieces) markup += p.physics.markup.size();
    EXPECT_EQ(markup, 1u) << "the ledge she hangs from is a piece's";
    const auto pole = load("ar_ei_breakable_hpole");
    ASSERT_TRUE(pole);
    EXPECT_EQ(pole->conditions[0].flags, 0x20u);
    EXPECT_FLOAT_EQ(pole->actions[0].delay, 176);
    const auto temple = load("ca_rb_breakable_temple_floor");
    ASSERT_TRUE(temple);
    EXPECT_EQ(temple->conditions[0].flags, 0x200u);
    EXPECT_EQ(temple->conditions[0].classes, std::vector<int32_t>{6});
    EXPECT_FALSE(load("ma_uo_bedroom_door")) << "an uber object";
    // The barrel's squib: 800 damage within 948, thrown at 500 .. 250; the bedroom door's: 5 .. 2 within 1896.
    const auto squib = game::loadExplosion(cdc::parseDrm(*bf.read("pc-w\\squib_exploding_barrel.drm")));
    ASSERT_TRUE(squib);
    EXPECT_FLOAT_EQ(squib->damage, 800);
    EXPECT_FLOAT_EQ(squib->edgeDamage, 800);
    EXPECT_FLOAT_EQ(squib->radius, 948);
    EXPECT_FLOAT_EQ(squib->knock, 500);
    EXPECT_FLOAT_EQ(squib->knockEdge, 250);
    EXPECT_FLOAT_EQ(squib->angle, 30);
    const auto door = game::loadExplosion(cdc::parseDrm(*bf.read("pc-w\\squib_explode_door.drm")));
    ASSERT_TRUE(door);
    EXPECT_FLOAT_EQ(door->damage, 5);
    EXPECT_FLOAT_EQ(door->radius, 1896);
    EXPECT_FLOAT_EQ(door->knock, 1000);
    EXPECT_FALSE(game::loadExplosion(cdc::parseDrm(*bf.read("pc-w\\squib_basic_explode.drm")))) << "no damage: effects only";
}

// Every unit's class-30 intros (the table in docs/GAMEPLAY.md "Breakables").
TEST(GameReal, BreakableSurvey) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    ASSERT_TRUE(list);
    const std::string text(list->begin(), list->end());
    std::map<std::string, std::map<std::string, std::vector<int32_t>>> found; // unit -> object -> uids
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::string name = line.substr(line.rfind('\\') + 1);
        if (name.empty() || !bf.read("pc-w\\" + name + ".drm")) continue;
        game::UnitData u;
        try { u = game::loadUnitData(bf, name); } catch (const std::exception&) { continue; }
        for (const auto& b : u.breakables) found[name][b.object].push_back(b.uid);
    }
    if (std::getenv("OU_BREAKABLE_SURVEY"))
        for (const auto& [unit, objects] : found)
            for (const auto& [object, uids] : objects) {
                std::printf("%s %s:", unit.c_str(), object.c_str());
                for (const int32_t id : uids) std::printf(" %d", id);
                std::printf("\n");
            }
    ASSERT_EQ(found.size(), 3u) << "l4_pool, l4_temple, l5_valaskjalf";
    EXPECT_EQ(found["l4_pool"]["ca_uo_ledgebreak"], (std::vector<int32_t>{5615, 5616}));
    EXPECT_EQ(found["l4_pool"]["ca_uo_ledgebreak_u"], (std::vector<int32_t>{5617, 5618, 5619}));
    EXPECT_EQ(found["l4_pool"]["ca_uo_balancebreakb"], (std::vector<int32_t>{5614}));
    EXPECT_EQ(found["l4_temple"]["ca_rb_breakable_temple_floor"], (std::vector<int32_t>{5234}));
    EXPECT_EQ(found["l5_valaskjalf"]["ar_rb_falling_floor_straight"], (std::vector<int32_t>{6975}));
    EXPECT_EQ(found["l5_valaskjalf"]["ar_ei_break_braced_ledge"], (std::vector<int32_t>{6960, 6961}));
    EXPECT_EQ(found["l5_valaskjalf"]["ar_ei_break_braced_corner"], (std::vector<int32_t>{6959}));
    EXPECT_EQ(found["l5_valaskjalf"]["ar_ei_breakable_hpole"], (std::vector<int32_t>{6962}));
}

// l5_valaskjalf's falling floor 6975 bridges a pit between ledges 1549 (south) and 1551 (north); the route goes north
// (the tilted one-sided kill plane 7034 under it cuts the walkway south of it for anyone walking back). Lara runs onto
// it: it holds for its 23 frames, then its four pieces fall as rigid bodies and she falls with them, catching the far
// edge. With a jump from its start she is over before it goes.
TEST(GameReal, FallingFloorBreaksUnderLara) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const game::Spawn south{{3476, -73450, -11300}, glm::radians(90.0f)};
    UnitGame g(bf, "l5_valaskjalf", south);
    auto& w = g.w;
    w.tick({}), w.tick({});
    ASSERT_EQ(w.breakablePhase(6975), game::World::BreakPhase::Whole);
    const auto on = w.collision().groundProbe(vec3(3476, -72698, -11300), 400);
    ASSERT_TRUE(on && on->dynamic >= 0) << "the floor is its pieces' collision";
    EXPECT_EQ(w.collision().dynamics[size_t(on->dynamic)].id, 6975);
    EXPECT_NEAR(on->point.z, -11378.4f, 1.0f);
    int stepped = -1, broke = -1;
    for (int i = 0; i < 200 && broke < 0; ++i) {
        w.tick(forward());
        if (stepped < 0 && w.breakablePhase(6975) == game::World::BreakPhase::Waiting) stepped = i;
        if (!w.brokenNow().empty()) broke = i;
    }
    ASSERT_GE(stepped, 0) << "her weight starts it";
    ASSERT_GE(broke, 0);
    EXPECT_EQ(broke - stepped, 46) << "23 frames (record action +0x1C)";
    EXPECT_EQ(w.brokenNow(), std::vector<int32_t>{6975});
    w.tick(forward());
    EXPECT_EQ(w.player().state().state, State::Fall) << "nothing under her";
    EXPECT_FALSE(w.collision().groundProbe(vec3(3476, -72698, -11300), 400)) << "its collision is gone";
    bool caught = false;
    for (int i = 0; i < 180; ++i) {
        w.tick(forward());
        caught |= w.player().state().state == State::Hang;
    }
    ASSERT_TRUE(caught) << "the far edge";
    EXPECT_EQ(w.collision().markup[size_t(w.player().state().ledge)].id, 1551);
    EXPECT_FALSE(w.health().dead());
    int fallen = 0;
    for (const auto& p : w.breakablePieces())
        if (p.uid == 6975) {
            EXPECT_EQ(p.phase, game::World::BreakPhase::Broken);
            fallen += p.world[3].z < -13000.0f;
        }
    EXPECT_EQ(fallen, 4) << "the pieces lie in the pit";
    // it stays broken across a save
    const auto saved = w.serializeObjects();
    UnitGame l(bf, "l5_valaskjalf", south);
    l.w.restoreObjects(saved);
    l.w.tick({}), l.w.tick({});
    EXPECT_EQ(l.w.breakablePhase(6975), game::World::BreakPhase::Broken);
    EXPECT_FALSE(l.w.collision().groundProbe(vec3(3476, -72698, -11300), 400));
    EXPECT_EQ(l.w.breakablePhase(6960), game::World::BreakPhase::Whole);
    // a running jump from its near edge clears it
    UnitGame j(bf, "l5_valaskjalf", south);
    j.w.tick({}), j.w.tick({});
    for (int i = 0; i < 12; ++i) j.w.tick(forward());
    Actions jump = forward();
    jump.jump = true;
    for (int i = 0; i < 4; ++i) j.w.tick(jump);
    for (int i = 0; i < 150; ++i) j.w.tick(forward());
    EXPECT_GT(j.w.player().state().pos.y, -72000);
    EXPECT_NEAR(j.w.player().state().pos.z, -11377.8f, 2.0f);
    EXPECT_FALSE(j.w.health().dead());
}

// Prologue, l3_bedroom_hall -> l3_main_hall (walkthrough: "the floor collapses ... take a running jump across"):
// one-sided plane 4375 fires as she runs toward the door (local +Y), holds her for a second (larabase 56) while floor
// 4359 plays `collapse` (from `idle`, its graph's start state, into `idle_collapsed`) and sends door 4376 its command:
// the door leaves its slide (uber state 5: joint removed, squib_explode_door born) and is thrown into the pit with its
// ring. A running jump then reaches the far edge (ledge 969, a saving grab), and she climbs up and walks into the hall.
TEST(GameReal, PrologueFloorCollapsesAndTheDoorIsBlownOff) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l3_bedroom_hall", {{-3300, -3640, 1900}, 0});
    auto& w = g.w;
    w.tick({}), w.tick({});
    const auto clip = [&] {
        const auto* o = w.script().find(4359);
        return o && o->graph && o->graph->clip >= 0 ? o->graph->set->names[size_t(o->graph->clip)] : std::string();
    };
    EXPECT_EQ(clip(), "idle") << "the floor is whole";
    ASSERT_TRUE(w.uberState(4376));
    EXPECT_EQ((*w.uberState(4376))[1], 0u);
    int crossed = -1;
    for (int i = 0; i < 90 && crossed < 0; ++i) {
        w.tick(forward());
        for (const auto& e : w.script().events())
            if (e.kind == script::EventKind::Cross && e.source == 4375) crossed = i;
    }
    ASSERT_GE(crossed, 0) << "toward local +Y";
    EXPECT_NEAR(w.player().state().pos.x, -2886, 60);
    bool collapsing = false, exploded = false;
    float farthest = -1e9f;
    for (int i = 0; i < 400 && (i < 150 || clip() == "collapse"); ++i) {
        w.tick(i < 55 ? forward() : Actions{}); // the stick does nothing for the first second
        collapsing |= clip() == "collapse";
        exploded |= !w.explosions().empty();
        if (i < 55) farthest = std::max(farthest, w.player().state().pos.x);
    }
    EXPECT_LT(farthest, -2600) << "held while it collapses";
    EXPECT_TRUE(collapsing);
    EXPECT_EQ(clip(), "idle_collapsed");
    EXPECT_EQ((*w.uberState(4376))[1], 5u);
    EXPECT_TRUE(exploded) << "squib_explode_door";
    for (const auto& d : w.collision().dynamics)
        if (d.id == 4376) EXPECT_LT(d.transform[3].z, 600) << "door and ring lie in the pit";
    EXPECT_FALSE(w.collision().raycast(vec3(-1400, -3675, 1995), vec3(1, 0, 0), 1000)) << "the doorway is open";
    // the run-up and the jump over the pit (1250 wide, 1700 deep)
    for (int i = 0; i < 120 && w.player().state().pos.x < -2440; ++i) w.tick(forward());
    ASSERT_GE(w.player().state().pos.x, -2440);
    Actions jump = forward();
    jump.jump = true;
    for (int i = 0; i < 4; ++i) w.tick(jump);
    for (int i = 0; i < 120 && w.player().state().state != State::Hang; ++i) w.tick(forward());
    ASSERT_EQ(w.player().state().state, State::Hang);
    EXPECT_EQ(w.collision().markup[size_t(w.player().state().ledge)].id, 969);
    Actions press;
    press.jump = true;
    w.tick({}), w.tick(press), w.tick({}); // the saving grab's recovery
    for (int i = 0; i < 150 && w.player().state().clip != "ledgeb_idle"; ++i) w.tick({});
    ASSERT_EQ(w.player().state().clip, "ledgeb_idle");
    for (int i = 0; i < 6; ++i) w.tick(forward());
    for (int i = 0; i < 6; ++i) w.tick(jump);
    for (int i = 0; i < 300 && w.currentUnit().data.name != "l3_main_hall"; ++i) w.tick(forward());
    EXPECT_EQ(w.currentUnit().data.name, "l3_main_hall");
    EXPECT_GT(w.player().state().pos.z, 1700);
    EXPECT_FALSE(w.health().dead());
}

// ---- the ship (docs/GAMEPLAY.md "Breakables" explosions, PROGRESSION.md chapter 1) ---------------------------------

// Boarding (walkthrough: from the boat to the anchor, up the chain, onto the deck): the anchor and the chain are stacked
// ledges on the hull's collision (the anchor's 189 wide at z 2393, the chain's links 64 wide every 237 up to 4275, two
// wider ones at 4517 / 4733 and the deck's edge 369 at 5426). A running jump off the yacht's bow catches the anchor,
// jumps up from hang to hang carry her to the rail and over it onto the deck.
TEST(GameReal, ShipBoardingByTheAnchorChain) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l1_ship_exterior", {{6928.8f, 22415.0f, 1900}, glm::radians(-168.2f)}); // the unit's player intro
    auto& w = g.w;
    const auto& s = w.player().state();
    for (int i = 0; i < 120 && s.state != State::Idle; ++i) w.tick({});
    ASSERT_NEAR(s.pos.z, 1745.1f, 2.0f) << "on the yacht's deck";
    for (int i = 0; i < 48; ++i) w.tick(forward());
    Actions jump = forward();
    jump.jump = true;
    for (int i = 0; i < 4; ++i) w.tick(jump);
    for (int i = 0; i < 90 && s.state != State::Hang; ++i) w.tick(forward());
    ASSERT_EQ(s.state, State::Hang) << "the anchor";
    const auto ledgeZ = [&] { return w.collision().markup[size_t(s.ledge)].points[0].z; };
    EXPECT_NEAR(w.collision().markup[size_t(s.ledge)].points[0].x, 5400, 20);
    EXPECT_LE(ledgeZ(), 2620) << "the anchor (2393) or the chain's first link (2616)";
    float top = ledgeZ();
    int jumps = 0;
    for (; jumps < 12 && s.pos.x > 5300; ++jumps) { // stick up + jump: the next hang, at last over the rail
        for (int i = 0; i < 4; ++i) w.tick(jump);
        for (int i = 0; i < 56; ++i) w.tick(forward());
        if (s.state == State::Hang) {
            EXPECT_GE(ledgeZ(), top) << "never down";
            top = ledgeZ();
        }
        EXPECT_NE(s.state, State::Dead);
    }
    EXPECT_LE(jumps, 8);
    for (int i = 0; i < 240 && !(s.state == State::Run && s.pos.z > 5000 && s.pos.z < 5100); ++i) w.tick(forward());
    EXPECT_NEAR(s.pos.z, 5075.9f, 6.0f) << "on the ship's deck";
    EXPECT_LT(s.pos.x, 5200);
    EXPECT_FALSE(w.health().dead());
}

// Fuel tanks (sh_ei_exploading_barrel, health 50, uber trigger 18): two pistol hits blow one up; its squib
// (squib_exploding_barrel: 800 damage within 948, damage class 4) hurts Lara and a mercenary standing by, and sets off
// the tank next to it.
TEST(GameReal, FuelTankExplosionHurtsLaraAndAMercenary) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    UnitGame g(bf, "l1_ship_exterior", {{4800, 18144.2f, 3900}, 0}, true); // tanks 1603 / 1604 stand at x 5496.6, 240 apart
    auto& w = g.w;
    w.tick({}), w.tick({});
    ASSERT_TRUE(w.uberState(1603) && w.uberState(1604));
    const auto type = game::ai::loadEnemyType(bf, "merc");
    auto& merc = w.enemies().add(type, 990001, vec3(5496.6f, 18700, 3809.2f), 0); // 556 from tank 1603
    const float mercHealth = merc.health();
    ASSERT_GT(mercHealth, 0);
    Actions look;
    look.look.y = -0.5f; // the free-aim ray is the camera's: tip it down onto the tank
    for (int i = 0; i < 30; ++i) w.tick(look);
    const float before = w.health().value();
    Actions fire;
    fire.fire = true;
    int exploded = 0, first = -1;
    std::vector<vec3> at;
    for (int i = 0; i < 300 && exploded < 2; ++i) {
        w.tick(first < 0 ? fire : Actions{});
        for (const vec3& e : w.explosions()) at.push_back(e), ++exploded;
        if (exploded && first < 0) first = i;
    }
    ASSERT_EQ(exploded, 2) << "the second tank goes off from the first";
    EXPECT_NEAR(at[0].y, 18144.2f, 1.0f);
    EXPECT_NEAR(at[1].y, 17903.7f, 1.0f);
    EXPECT_LT(w.health().value(), before - 500) << "696 from the first tank: 800 a blast";
    EXPECT_LT(merc.health(), mercHealth) << "the mercenary beside it";
    for (int i = 0; i < 240; ++i) w.tick({});
    EXPECT_TRUE(w.removedObjects().contains(1603));
    EXPECT_TRUE(w.removedObjects().contains(1604));
}
