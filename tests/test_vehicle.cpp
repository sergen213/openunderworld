// Lara's motorcycle (src/game/Vehicle.*): intros, decoded tuning, mount / ride / dismount replay, saves.
#include "game/Game.hpp"
#include "save/Save.hpp"
#include "engine/FrameClock.hpp"
#include "app/game/Autopilot.hpp"

#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <tuple>

using namespace ou;
using game::State;

namespace {
const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

game::UnitLoader loader(const cdc::BigfileSet& bf) {
    return [&bf](const std::string& name) { return game::loadUnitData(bf, name); };
}
// Lara at the mount spot of l4_trailstart's bike 5482, facing it (tests/replays/bike_ride.txt).
const game::Spawn kMountSpot{{37981, -51758, 1463}, -126.7f * 3.14159265f / 180.0f};
} // namespace

// [V-data] motorcycle intros of the chapter-start units, the vehicle data record and the riding clips.
TEST(VehicleReal, BikeIntrosTuningAndClips) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto trail = game::loadUnitData(bf, "l4_trailstart");
    ASSERT_EQ(trail.vehicles.size(), 1u);
    EXPECT_EQ(trail.vehicles[0].uid, 5482);
    EXPECT_NEAR(trail.vehicles[0].pos.x, 38051, 1);
    EXPECT_NEAR(trail.vehicles[0].pos.y, -51966, 1);
    EXPECT_NEAR(trail.vehicles[0].yaw, 0.93f - 3.14159265f / 2, 0.01f);
    EXPECT_EQ(game::loadUnitData(bf, "l5_startsnow").vehicles.size(), 1u);
    EXPECT_EQ(game::loadUnitData(bf, "l5_valgrind_tunnel").vehicles.size(), 1u);
    EXPECT_TRUE(game::loadUnitData(bf, "l4_pool").vehicles.empty());

    const auto t = game::loadBikeTuning(cdc::parseDrm(*bf.read("pc-w\\motorcycle.drm")));
    EXPECT_NEAR(t.maxSpeed, 40 * 280 / 2.237f, 1);           // 40 mph
    EXPECT_NEAR(t.steerLow, 30 * 3.14159265f / 180, 1e-4f);  // degrees in the record
    EXPECT_NEAR(t.steerHigh, 10 * 3.14159265f / 180, 1e-4f);
    EXPECT_NEAR(t.steerRateLow, 95 * 3.14159265f / 180, 1e-4f);
    EXPECT_NEAR(t.steerRateHigh, 75 * 3.14159265f / 180, 1e-4f);
    EXPECT_FLOAT_EQ(t.steerFraction, 0.3f);
    EXPECT_NEAR(t.fallMin, 3792.67f, 0.01f);
    EXPECT_NEAR(t.fallMax, 4977.88f, 0.01f);
    EXPECT_FLOAT_EQ(t.damageLethal, 1650);
    EXPECT_NEAR(t.frontAxle.y, -206.6f, 0.5f); // bones 5 / 8: front / rear wheel
    EXPECT_NEAR(t.rearAxle.y, 256.8f, 0.5f);
    EXPECT_NEAR(t.wheelbase(), 463.4f, 1);
    // Havok vehicle data [V-data]: the wheel definitions (vehicle data +0x90) on the model's wheel spheres / markers,
    // the engine and suspension fields of the record.
    ASSERT_EQ(t.wheels.size(), 2u);
    const auto& front = t.wheels[0];
    const auto& rear = t.wheels[1];
    EXPECT_EQ(front.flags & 1, 1u); // steered
    EXPECT_EQ(rear.flags & 1, 0u);
    EXPECT_NEAR(front.attach.y, -206, 0.5f);
    EXPECT_NEAR(front.attach.z, 258.9f, 0.5f); // marker 30
    EXPECT_NEAR(rear.attach.y, 260, 0.5f);
    EXPECT_NEAR(rear.attach.z, 220.6f, 0.5f);  // marker 31
    EXPECT_FLOAT_EQ(front.rest, 170);
    EXPECT_FLOAT_EQ(rear.rest, 150);
    EXPECT_FLOAT_EQ(front.radius, 103);
    EXPECT_FLOAT_EQ(rear.radius, 97);
    EXPECT_NEAR(front.drive, 0.4f, 1e-6f);
    EXPECT_NEAR(rear.drive, 0.6f, 1e-6f);
    EXPECT_FLOAT_EQ(t.rpm[2], 18000);
    EXPECT_FLOAT_EQ(t.torque[0], 30);
    EXPECT_FLOAT_EQ(t.reverseTorque, 20);
    EXPECT_NEAR(t.springHz, 1.8f, 1e-5f);
    EXPECT_NEAR(t.springDamping, 0.35f, 1e-5f);
    EXPECT_NEAR(t.maxTilt, 42.5f * 3.14159265f / 180, 1e-4f);
    EXPECT_NEAR(t.stopSpeed, 2 * 280 / 2.237f, 0.1f);
    EXPECT_NEAR(t.vehicle.ratio, 37.42f, 0.01f); // top rpm at 40 mph (0x78F860)
    EXPECT_NEAR(physics::massProps(*t.body).mass, 265, 1e-3f); // the chassis: the wheel spheres are the wheel rays

    const auto anims = game::loadLaraAnims(bf);
    for (const auto n : game::kBikeClipNames) EXPECT_NE(anims.find("motorcycle:" + std::string(n)), nullptr) << n;
}

// tests/replays/bike_ride.txt: mount, accelerate up the trail, steer, brake and dismount, in l4_trailstart.
TEST(VehicleReal, TrailstartRideReplay) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    ASSERT_TRUE(w.bikes().contains(5482));
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/bike_ride.txt");
    const game::vec3 start = w.bikes().at(5482).pos;
    bool mounted = false, rode = false, flew = false;
    float topSpeed = 0, minYaw = 10, maxYaw = -10, farthest = 0;
    for (uint64_t i = 0; i < 10 * 60; ++i) {
        w.tick(replay.at(i));
        const auto* b = w.ridden();
        if (b && std::getenv("OU_VEHICLE_TRACE") && i % 15 == 0)
            std::printf("tick %3llu %s pos %.0f %.0f %.0f yaw %.2f pitch %.2f speed %.0f%s clip %s\n", (unsigned long long)i,
                        std::string(game::bikePhaseName(b->phase)).c_str(), b->pos.x, b->pos.y, b->pos.z, b->yaw, b->pitch, b->speed,
                        b->air ? " air" : "", w.player().state().clip.c_str());
        if (!b) continue;
        mounted = true;
        rode |= b->phase == game::BikePhase::Ride;
        flew |= b->air;
        topSpeed = std::max(topSpeed, b->speed);
        farthest = std::max(farthest, glm::length(b->pos - start));
        minYaw = std::min(minYaw, b->yaw), maxYaw = std::max(maxYaw, b->yaw);
        EXPECT_EQ(w.player().state().state, State::Vehicle);
        if (b->phase == game::BikePhase::Ride) EXPECT_NEAR(glm::length(w.player().state().pos - game::vec3((*w.bikeMatrix(b->uid, 1))[3])), 0, 1);
    }
    const auto& bike = w.bikes().at(5482);
    std::printf("bike ride: top speed %.0f, yaw %.2f..%.2f, parked at %.0f %.0f %.0f (%s), lara %s\n", topSpeed, minYaw, maxYaw,
                bike.pos.x, bike.pos.y, bike.pos.z, std::string(game::bikePhaseName(bike.phase)).c_str(),
                std::string(game::stateName(w.player().state().state)).c_str());
    EXPECT_TRUE(mounted && rode && flew);
    EXPECT_GT(topSpeed, 3000);
    EXPECT_GT(maxYaw - minYaw, 0.5f);
    EXPECT_GT(farthest, 5500); // up to the steps' top
    EXPECT_EQ(bike.phase, game::BikePhase::Parked);
    EXPECT_EQ(w.ridden(), nullptr);
    EXPECT_EQ(w.player().state().state, State::Idle); // on her feet beside the bike
    EXPECT_LT(glm::length(game::vec2(w.player().state().pos - game::vec3((*w.bikeMatrix(5482, 1))[3]))), 400);
    EXPECT_FLOAT_EQ(w.health().value(), w.health().max()); // no fall damage on this route
}

// An enemy standing on the replay's route is rammed once at speed (ramDamage per unit/s) and the bike slows down.
TEST(VehicleReal, RamsEnemyOnTheTrail) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    game::DummyTarget dummy;
    dummy.pos = {40634, -53906, 1161}; // where the bike passes at tick 180 (3394 units/s)
    dummy.hp = 700;
    w.targets.push_back(&dummy);
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/bike_ride.txt");
    float before = 0, after = 0;
    for (uint64_t i = 0; i < 200; ++i) {
        const int hits = dummy.hits;
        const float speed = w.ridden() ? w.ridden()->speed : 0.0f;
        w.tick(replay.at(i));
        if (dummy.hits != hits) before = speed, after = w.ridden()->speed;
    }
    EXPECT_EQ(dummy.hits, 1);
    EXPECT_LE(dummy.hp, 0);
    EXPECT_GT(before, w.anims().bike.ramSpeed);
    EXPECT_LT(after, before * 0.75f);
    w.targets.clear();
}

// The ride replay with a pistol drawn once riding and fire held: the right gun alone shoots the target ahead on the
// trail through the hitscan path, the bike still steers, and the gun goes away for the dismount.
TEST(VehicleReal, ShootsFromTheBike) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    game::DummyTarget dummy;
    dummy.pos = {40634, -53906, 1161}; // on the route (RamsEnemyOnTheTrail)
    dummy.hp = 1e6f;                   // survives the ram: only shots are counted
    w.targets.push_back(&dummy);
    std::string text;
    {
        std::FILE* f = std::fopen(OU_SOURCE_DIR "/tests/replays/bike_ride.txt", "rb");
        ASSERT_TRUE(f);
        for (int ch; (ch = std::fgetc(f)) != EOF;) text += char(ch);
        std::fclose(f);
    }
    const auto script = game::InputScript::parse(text + "1.5 draw 1\n1.6 draw 0\n1.7 fire 1\n6.0 fire 0\n");
    int onTarget = 0, leftGun = 0;
    float shotDamage = 0; // the ram's hit is not a shot
    float minYaw = 10, maxYaw = -10;
    bool drawnRiding = false;
    for (uint64_t i = 0; i < 10 * 60; ++i) {
        w.tick(script.at(i));
        for (const auto& s : w.combat().shots) onTarget += s.target == &dummy, leftGun += s.hand != 0, shotDamage += s.target == &dummy ? s.damage : 0;
        if (const auto* b = w.ridden(); b && b->phase == game::BikePhase::Ride) {
            minYaw = std::min(minYaw, b->yaw), maxYaw = std::max(maxYaw, b->yaw);
            drawnRiding |= w.combat().drawn && w.combat().drawT >= 1;
        }
    }
    std::printf("bike shooting: %d shots on target (%llu fired), %.0f damage, yaw %.2f..%.2f\n", onTarget,
                (unsigned long long)w.combat().shotsFired, shotDamage, minYaw, maxYaw);
    EXPECT_TRUE(drawnRiding);
    EXPECT_GE(onTarget, 3); // three refires (10/30 s) while it is ahead, then it is behind
    EXPECT_GE(shotDamage, 3 * w.anims().weapons[0].damage[2]);
    EXPECT_EQ(leftGun, 0);
    EXPECT_GT(maxYaw - minYaw, 0.5f); // the replay's right curve
    EXPECT_EQ(w.ridden(), nullptr);   // dismounted
    EXPECT_FALSE(w.combat().drawn);
    w.targets.clear();
}

// A save taken mid-ride restores the ridden bike (its Jolt body, wheels and contacts: objects.physics) and Lara on it;
// both worlds then drive bit for bit the same.
TEST(VehicleReal, SaveRestoreMidRide) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/bike_ride.txt");
    game::World a(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    uint64_t i = 0;
    for (; i < 150; ++i) a.tick(replay.at(i));
    ASSERT_NE(a.ridden(), nullptr);
    const auto save = save::parse(save::serialize(save::capture(a)));
    game::World b(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    save::apply(save, b);
    ASSERT_NE(b.ridden(), nullptr);
    EXPECT_EQ(b.ridden()->uid, 5482);
    EXPECT_EQ(b.player().state().state, State::Vehicle);
    for (; i < 210; ++i) a.tick(replay.at(i)), b.tick(replay.at(i));
    ASSERT_EQ(a.currentUnit().data.name, b.currentUnit().data.name);
    const auto pa = a.player().state().pos - a.currentUnit().offset, pb = b.player().state().pos - b.currentUnit().offset;
    EXPECT_EQ(pa, pb);
    EXPECT_EQ(a.ridden()->speed, b.ridden()->speed);
    EXPECT_EQ(a.ridden()->yaw, b.ridden()->yaw);
    EXPECT_GT(a.ridden()->speed, 1000); // still riding hard
}

// The ride replay at 30, 60 and 144 Hz render rates: the fixed tick drives the Jolt bike, so the bike and Lara end up
// bit for bit at the same place.
TEST(VehicleReal, RideIndependentOfRenderRate) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/bike_ride.txt");
    const auto anims = game::loadLaraAnims(bf);
    auto run = [&](double renderHz) {
        game::World w(loader(bf), "l4_trailstart", anims, kMountSpot);
        engine::FrameClock clock(game::kTickSeconds);
        while (w.ticks() < 300) // through the curve
            for (int n = clock.advance(1.0 / renderHz); n > 0 && w.ticks() < 300; --n) w.tick(replay.at(w.ticks()));
        const auto& b = w.bikes().at(5482);
        return std::tuple(b.pos, b.yaw, b.speed, w.player().state().pos);
    };
    const auto a = run(30), b = run(60), c = run(144);
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, c);
    EXPECT_GT(glm::length(std::get<0>(a) - game::loadUnitData(bf, "l4_trailstart").vehicles[0].pos), 3000); // it rode
}

namespace {
// Mounts bike 5482 (interact at 0.3 s; riding from about 1.2 s), then `rest` of an input script; per-tick callback.
// The autopilot plan of tests/selftests/bike_ride.json (after its mount step); `upTo`: only its first plan steps.
std::string firstRidePlan(int upTo = 99) {
    std::string plan = "wait 0.3\npress interact\nwait 3\n";
    std::ifstream f(OU_SOURCE_DIR "/tests/selftests/bike_ride.json");
    int n = 0;
    for (const auto& step : nlohmann::json::parse(f).at("steps")) {
        if (!step.contains("plan") || n++ >= upTo) continue;
        for (const auto& line : step["plan"]) plan += line.get<std::string>() + "\n";
    }
    return plan;
}

// The bike ridden by the scenario's plan onto the level floor of the stone tunnel (z -686) and stopped there with the
// Vehicle Brake; then `input` (times from 0) for `seconds`, `each(tick, bike, world)` per tick.
template <class F> void inTunnel(const cdc::BigfileSet& bf, const game::AnimLibrary& anims, const std::string& input, double seconds, F&& each) {
    game::World w(loader(bf), "l4_trailstart", anims, kMountSpot);
    auto pilot = app::Autopilot::parse(firstRidePlan(1) + "hold jump 3\n");
    uint64_t i = 0;
    for (; i < 60 * 60 && !pilot.done(); ++i) w.tick(pilot.step(i, w));
    ASSERT_FALSE(pilot.failed()) << pilot.failure();
    ASSERT_NE(w.ridden(), nullptr);
    ASSERT_LT(std::abs(w.ridden()->speed), 5);
    const auto script = game::InputScript::parse(input);
    for (uint64_t k = 0; k < uint64_t(seconds * 60) && w.ridden(); ++k) {
        w.tick(script.at(k));
        if (std::getenv("OU_VEHICLE_TRACE") && k % 3 == 0)
            std::printf("t %.2f v %6.0f air %d x %.0f z %.0f stunt %d\n", double(k) / 60, w.ridden()->speed, w.ridden()->air, w.player().state().pos.x, w.player().state().pos.z, int(w.ridden()->stunt));
        each(k, *w.ridden(), w);
    }
}

template <class F> void ride(const cdc::BigfileSet& bf, const game::AnimLibrary& anims, const std::string& rest, double seconds, F&& each) {
    game::World w(loader(bf), "l4_trailstart", anims, kMountSpot);
    const auto script = game::InputScript::parse("0.3 interact 1\n0.4 interact 0\n" + rest);
    for (uint64_t i = 0; i < uint64_t(seconds * 60); ++i) {
        w.tick(script.at(i));
        if (const auto* b = w.ridden(); b && b->phase == game::BikePhase::Ride) {
            if (std::getenv("OU_VEHICLE_TRACE"))
                std::printf("tick %3llu stunt %d air %d v %5.0f yaw %6.3f roll %6.3f lean %6.3f pos %.0f %.0f %.0f\n", (unsigned long long)i,
                            int(b->stunt), b->air, b->speed, b->yaw, b->roll, b->drive.lean, b->pos.x, b->pos.y, b->pos.z);
            each(i, *b, w);
        }
    }
}
} // namespace

// Lean (0x793680): at speed the bike rolls into the steering (+ = left side down) and back the other way, up to the
// record's 30 degrees; Lara is rolled with it.
TEST(VehicleReal, LeanFollowsSteeringAtSpeed) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    EXPECT_NEAR(anims.bike.leanMax, 30 * 3.14159265f / 180, 1e-4f);
    EXPECT_NEAR(anims.bike.airYaw, 300, 1e-3f);
    EXPECT_NEAR(anims.bike.airSide, 2600, 1e-3f);
    float left = 0, right = 0, straight = 1, maxRoll = 0;
    ride(bf, anims, "1.3 forward 1\n2.4 right -1\n3.0 right 1\n3.6 right 0\n", 3.6, [&](uint64_t i, const game::Bike& b, const game::World& w) {
        if (i == 140) {
            straight = b.roll;
            EXPECT_GT(b.speed, 1500);
        }
        if (i == 177) left = b.roll;
        if (i == 213) right = b.roll;
        maxRoll = std::max(maxRoll, std::abs(b.roll));
        EXPECT_EQ(w.player().state().roll, b.roll);
    });
    std::printf("bike lean: straight %.3f, left %.3f, right %.3f, max %.3f rad\n", straight, left, right, maxRoll);
    EXPECT_LT(std::abs(straight), 0.05f);
    EXPECT_GT(left, 0.15f);
    EXPECT_LT(right, -0.15f);
    EXPECT_LT(maxRoll, anims.bike.leanMax + 0.1f);
}

// Air control (0x793680, no wheel down): steering while the bike flies off the tunnel's ramp over the pit yaws it towards
// the stick, the same each run. (Rewritten: it used the Space hop, which the original does not have.)
TEST(VehicleReal, AirControlTurnsMidJump) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto jump = [&](float steer) {
        game::World w(loader(bf), "l4_trailstart", anims, kMountSpot);
        auto pilot = app::Autopilot::parse(firstRidePlan(1) + "ride 64000 -55680 300\nride 65700 -55680 200\n"); // to the ramp
        uint64_t i = 0;
        for (; i < 60 * 60 && !pilot.done(); ++i) w.tick(pilot.step(i, w));
        EXPECT_FALSE(pilot.failed()) << pilot.failure();
        game::Actions a;
        a.move = {0, 1};
        int air = 0;
        float yaw0 = 0, yaw = 0;
        for (int k = 0; k < 60 && w.ridden(); ++k) { // straight on off the lip, then the stick while no wheel is down
            const game::Bike& b = *w.ridden();
            if (w.player().state().pos.x > 67600) break; // still over the pit
            if (b.air && air++ == 0) yaw0 = b.yaw;
            a.move.x = b.air ? steer : 0.0f;
            yaw = b.yaw;
            w.tick(a);
        }
        EXPECT_GE(air, 8);
        return std::remainder(yaw - yaw0, 2 * 3.14159265f);
    };
    const float none = jump(0), left = jump(-1), again = jump(-1), right = jump(1);
    std::printf("bike air control over the pit: yaw %+.4f none, %+.4f left, %+.4f right over the pit\n", none, left, right);
    EXPECT_GT(left - none, 0.01f);
    EXPECT_LT(right - none, -0.01f);
    EXPECT_EQ(left, again);
}

// Stunts (0x790E40): throttle + hand brake from a standstill with the stick over is a donut (the bike spins round its
// front wheel, leaning in); hand brake + steering at speed is a power slide.
TEST(VehicleReal, DonutAndPowerSlide) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    float turned = 0, prevYaw = 0, roll = 0;
    int donut = 0;
    game::vec3 start{0}, farthest{0};
    ride(bf, anims, "1.5 forward 1\n1.5 jump 1\n1.5 right -1\n", 4.5, [&](uint64_t i, const game::Bike& b, const game::World& w) {
        if (i == 89) start = b.pos, prevYaw = b.yaw;
        if (i < 90) return;
        turned += std::remainder(b.yaw - prevYaw, 2 * 3.14159265f), prevYaw = b.yaw;
        donut += b.stunt == game::BikeStunt::Donut;
        if (glm::length(b.pos - start) > glm::length(farthest)) farthest = b.pos - start;
        if (b.stunt == game::BikeStunt::Donut) {
            roll = b.roll;
            EXPECT_EQ(w.player().state().clip, "motorcycle:motorcycle_donut_left");
        }
    });
    std::printf("bike donut: %d ticks, turned %.2f rad, farthest %.0f, roll %.3f\n", donut, turned, glm::length(farthest), roll);
    EXPECT_GT(donut, 90);                     // until a bump on the trail's slope throws it
    EXPECT_GT(turned, 4.0f);                  // well past half a turn
    EXPECT_LT(glm::length(farthest), 1000);  // about two bike lengths: round its front wheel, not away
    EXPECT_GT(roll, 0.05f);

    int slide = 0;
    float slideRoll = 0;
    ride(bf, anims, "1.3 forward 1\n2.6 forward 0\n2.6 jump 1\n2.6 right -1\n", 3.4, [&](uint64_t, const game::Bike& b, const game::World&) {
        if (b.stunt == game::BikeStunt::PowerSlide) ++slide, slideRoll = b.roll;
    });
    std::printf("bike power slide: %d ticks, roll %.3f\n", slide, slideRoll);
    EXPECT_GT(slide, 20);
    EXPECT_GT(slideRoll, 0.05f);
}

// Launch (tru.exe 0x793680 flag bits 23 / 22, 0x791D10; record +0x0C 10, +0x1C0 0.1 s, +0x1C4 0.3 s): revving against
// the hand brake and letting it go with the throttle held shoves the bike off with ten times the engine torque for a
// tenth of a second; without the throttle the charge is dropped.
TEST(VehicleReal, LaunchBoostAfterRevvingAgainstTheBrake) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    const auto speedAt = [&](const std::string& input, uint64_t tick) {
        float v = 0, held = 0;
        inTunnel(bf, anims, input, 1.5, [&](uint64_t i, const game::Bike& b, const game::World&) {
            if (i == 25) held = std::abs(b.speed); // revving: the brake holds it
            if (i == tick) v = b.speed;
        });
        EXPECT_LT(held, 60);
        return v;
    };
    const uint64_t at = 30 + 12; // 0.2 s after the brake is let go at 0.5 s
    const float plain = speedAt("0.5 forward 1\n", at);
    const float launched = speedAt("0.0 forward 1\n0.0 jump 1\n0.5 jump 0\n", at);
    const float dropped = speedAt("0.0 forward 1\n0.0 jump 1\n0.4 forward 0\n0.5 jump 0\n0.5 forward 1\n", at);
    std::printf("bike launch: %.0f units/s 0.2 s after the launch, %.0f from a plain start, %.0f after a dropped charge\n", launched, plain, dropped);
    EXPECT_GT(launched, plain + 1200); // the burst: about 0.1 s x 9 x the engine's push more
    EXPECT_NEAR(dropped, plain, 150);
}

// Reverse (0x790E40 state 3 -> 2, record +0x1F8 6 frames; +0x74 / +0x6C: top rpm at 10 mph): standing, the brake has to
// be held for 7 frames before the reverse gear takes; it then backs up to about 10 mph and no faster on level ground.
TEST(VehicleReal, ReverseAfterTheBrakeIsHeld) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    bool early = false;
    float back = 0;
    inTunnel(bf, anims, "0.0 forward -1\n", 3.0, [&](uint64_t i, const game::Bike& b, const game::World&) {
        if (i <= 13) early |= b.drive.reverse; // 7 frames at 30 Hz = 14 ticks
        if (i == 16) EXPECT_TRUE(b.drive.reverse);
        back = std::min(back, b.speed);
    });
    std::printf("bike reverse: fastest %.0f units/s\n", back);
    EXPECT_FALSE(early);
    EXPECT_NEAR(back, -10.0f * 125.2f, 0.12f * 1252); // 10 mph on the level
}

// The first ride of Southern Mexico by autopilot input (the plan lines of the self-test scenario
// tests/selftests/bike_ride.json): from where the chapter leaves her bike on the trail, over the stair hump between the two
// pillars, down the valley, into the stone tunnel and off its ramp over the pit, to the checkpoint beyond.
TEST(VehicleReal, FirstRideOverThePitByAutopilot) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    auto pilot = app::Autopilot::parse(firstRidePlan());
    float takeOff = 0, landX = 0, airStartX = 0, top = 0;
    int airTicks = 0;
    bool overPit = false;
    uint64_t i = 0;
    for (; i < 60 * 60 && !pilot.done(); ++i) {
        w.tick(pilot.step(i, w));
        const auto* b = w.ridden();
        if (!b) continue;
        const game::vec3 p = w.player().state().pos;
        top = std::max(top, b->speed);
        // the pit: its near edge (the ramp's lip) at x 66100, the far edge at x 67700, 6000 deep
        if (b->air && !overPit && p.x > 65900 && p.x < 67200) overPit = true, takeOff = b->speed, airStartX = p.x;
        if (overPit && landX == 0) {
            if (b->air) ++airTicks;
            else landX = p.x;
        }
        if (std::getenv("OU_VEHICLE_TRACE") && i % 6 == 0)
            std::printf("tick %4llu v %5.0f air %d pos %.0f %.0f %.0f pitch %.2f\n", (unsigned long long)i, b->speed, b->air, p.x, p.y, p.z, b->pitch);
    }
    std::printf("first ride: %s after %.1f s, top %.0f units/s; pit: take-off %.0f units/s at x %.0f, %d ticks in the air, down at x %.0f\n",
                pilot.failed() ? pilot.failure().c_str() : "done", double(i) / 60, top, takeOff, airStartX, airTicks, landX);
    EXPECT_FALSE(pilot.failed()) << pilot.failure();
    ASSERT_NE(w.ridden(), nullptr);
    EXPECT_TRUE(overPit);
    EXPECT_GT(landX, 67700);                 // beyond the far edge
    EXPECT_GT(w.player().state().pos.z, -1200); // not in the pit
    EXPECT_FLOAT_EQ(w.health().value(), w.health().max());
    EXPECT_LT(glm::length(game::vec2(w.player().state().pos) - game::vec2(69586, -55062)), 700); // checkpoint 6620
}
