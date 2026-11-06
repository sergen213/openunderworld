// Pad rumble and camera shake (game/Feedback.hpp).
#include "game/Feedback.hpp"
#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <cstdlib>
#include <cstring>

using namespace ou::game;

namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

// One sequence: channel 0 linear 0 -> 1 over 1 s, channel 1 step 0.5 until 0.5 s then 0.25 until 1 s.
Padshock ramp(bool loop = false) {
    Padshock p;
    p.name = "ramp", p.loop = loop, p.loopStart = 0, p.loopEnd = 0.5f;
    p.channels.push_back({1, 0, 1, {{0, 0}, {1, 1}}});
    p.channels.push_back({2, 0, 1, {{0, 0.5f}, {0.5f, 0.25f}, {1, 0}}});
    return p;
}
} // namespace

// 0x481CC0 / 0x481CF0: motor 0 on / off, motor 1 level / 255, for duration / 4096 frames of 30 Hz; counted down by
// the tick, the value is added while the time left is positive.
TEST(Rumble, ShocksHoldForTheirFrames) {
    Rumble r;
    r.shock0(100, 3 << 12); // effect command 0xE {100, 3 frames}: 0.1 s
    r.shock1(51, 6 << 12);  // 0xF {51, 6 frames}: 0.2 s at 0.2
    int ticks0 = 0, ticks1 = 0;
    for (int i = 0; i < 30; ++i) {
        r.tick(1 / 60.0f);
        ticks0 += r.motors()[0] > 0, ticks1 += r.motors()[1] > 0;
        EXPECT_TRUE(r.motors()[0] == 0 || r.motors()[0] == 1.0f);
        EXPECT_TRUE(r.motors()[1] == 0 || std::abs(r.motors()[1] - 0.2f) < 1e-6f);
    }
    EXPECT_NEAR(ticks0, 5.5, 0.6); // 0.1 s: the 6th tick reaches 0 (float rounding decides that tick)
    EXPECT_NEAR(ticks1, 11.5, 0.6);
    EXPECT_EQ(r.motors(), (std::array<float, 2>{0, 0}));
}

// 0x481340 / 0x4811E0: channel n drives motor n; linear keys interpolate (scaled, clamped 0..1, times the play scale),
// step keys hold the last key's value; past the last key the slot frees. Handles count 1, 3, 5.
TEST(Rumble, SequenceCurvesDriveTheMotors) {
    Rumble r;
    r.setLibrary({ramp()});
    EXPECT_EQ(r.play("nope"), 0u);
    const uint32_t h = r.play("RAMP", 1, 0.5f); // case-insensitive (__stricmp)
    EXPECT_EQ(h, 1u);
    EXPECT_TRUE(r.playing(h));
    EXPECT_FALSE(r.looping(h));
    for (int i = 0; i < 15; ++i) r.tick(1 / 60.0f); // 0.25 s
    EXPECT_NEAR(r.motors()[0], 0.125f, 1e-4f);
    EXPECT_NEAR(r.motors()[1], 0.25f, 1e-4f);
    for (int i = 0; i < 30; ++i) r.tick(1 / 60.0f); // 0.75 s
    EXPECT_NEAR(r.motors()[0], 0.375f, 1e-4f);
    EXPECT_NEAR(r.motors()[1], 0.125f, 1e-4f);
    r.setRate(h, 2);
    for (int i = 0; i < 10; ++i) r.tick(1 / 60.0f); // past 1 s: done
    EXPECT_FALSE(r.playing(h));
    EXPECT_EQ(r.motors(), (std::array<float, 2>{0, 0}));
    EXPECT_EQ(r.play("ramp"), 3u);
}

// Looping sequences wrap between their loop points until released (0x483130 with release), then run out; stop without
// release clears at once. Sums clamp at 1.
TEST(Rumble, LoopsWrapUntilReleased) {
    Rumble r;
    r.setLibrary({ramp(true)});
    const uint32_t a = r.play("ramp"), b = r.play("ramp", 1, 4);
    EXPECT_TRUE(r.looping(a));
    for (int i = 0; i < 600; ++i) r.tick(1 / 60.0f);
    EXPECT_TRUE(r.playing(a));
    EXPECT_LE(r.motors()[0], 1.0f);
    r.stop(b, false);
    EXPECT_FALSE(r.playing(b));
    r.stop(a, true);
    for (int i = 0; i < 60; ++i) r.tick(1 / 60.0f);
    EXPECT_FALSE(r.playing(a));
}

namespace {
ShakeData flatShake() {
    ShakeData d;
    for (auto& c : d.channels) c.envelope.fill(255), c.amplitude = 1, c.period = 1;
    d.duration = 1;
    return d;
}
} // namespace

// 0x5F3190: amplitude x envelope x sin(2 pi t / period); translation x 3.1605 units, rotation and FOV in degrees; the
// shake ends after its duration. Render offsets interpolate between the last two ticks.
TEST(CameraShake, ChannelsFollowTheirSine) {
    CameraShake s;
    s.setLibrary({{7, flatShake()}});
    EXPECT_FALSE(s.start(8, 1));
    ASSERT_TRUE(s.start(7, 0.5f));
    for (int i = 0; i < 16; ++i) s.tick(1 / 60.0f); // the 16th tick evaluates t = 0.25 s: sin = 1
    const auto o = s.offset(1);
    EXPECT_NEAR(o.move.x, 3.1605f, 1e-3f);
    EXPECT_NEAR(o.turn.z, 0.0174533f, 1e-5f);
    EXPECT_NEAR(o.fov, 0.0174533f, 1e-5f);
    const auto half = s.offset(0.5f), before = s.offset(0);
    EXPECT_NEAR(half.move.x, (before.move.x + o.move.x) / 2, 1e-5f);
    for (int i = 0; i < 20; ++i) s.tick(1 / 60.0f);
    EXPECT_FALSE(s.active());
    EXPECT_EQ(s.offset(1).move, glm::vec3(0));
    ASSERT_TRUE(s.start(7, 10));
    s.tick(1 / 60.0f);
    s.stop(7);
    EXPECT_FALSE(s.active());
}

// Real data: padshocklib.tfb's 32 sequences and camerashakes.drm's 20 shakes decode [O].
TEST(FeedbackReal, LibrariesDecode) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto tfb = bf.read("pc-w\\padshock\\padshocklib.tfb");
    ASSERT_TRUE(tfb);
    const auto lib = parsePadshockLib(*tfb);
    ASSERT_EQ(lib.size(), 32u);
    EXPECT_EQ(lib[18].name, "pistol_fire");
    EXPECT_EQ(lib[14].name, "loop_steady");
    EXPECT_TRUE(lib[14].loop);
    Rumble r;
    r.setLibrary(lib);
    const uint32_t h = r.play("pistol_fire");
    float peak = 0;
    for (int i = 0; i < 30; ++i) r.tick(1 / 60.0f), peak = std::max(peak, r.motors()[0]);
    EXPECT_GT(peak, 0.7f);        // its kick: 0.952 at 0.193 s
    EXPECT_FALSE(r.playing(h));   // its last keys are at 0.4 s
    const auto drm = bf.read("pc-w\\camerashakes.drm");
    ASSERT_TRUE(drm);
    const auto shakes = parseCameraShakes(*drm);
    ASSERT_EQ(shakes.size(), 20u);
    EXPECT_FLOAT_EQ(shakes.at(2549).channels[2].amplitude, 100.0f);
    EXPECT_FLOAT_EQ(shakes.at(968).duration, 0.2f);
    EXPECT_TRUE(shakes.at(2561).positional);
}
