// Lara's motorcycle (src/game/Vehicle.*): intros, decoded tuning, mount / ride / hop / dismount replay, saves.
#include "game/Game.hpp"
#include "save/Save.hpp"

#include "cdc/Cdc.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    const auto anims = game::loadLaraAnims(bf);
    for (const auto n : game::kBikeClipNames) EXPECT_NE(anims.find("motorcycle:" + std::string(n)), nullptr) << n;
}

// tests/replays/bike_ride.txt: mount, accelerate up the trail, steer, hop, brake and dismount, in l4_trailstart.
TEST(VehicleReal, TrailstartRideReplay) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    game::World w(loader(bf), "l4_trailstart", game::loadLaraAnims(bf), kMountSpot);
    ASSERT_TRUE(w.bikes().contains(5482));
    const auto replay = game::InputScript::load(OU_SOURCE_DIR "/tests/replays/bike_ride.txt");
    const game::vec3 start = w.bikes().at(5482).pos;
    bool mounted = false, rode = false, flew = false;
    float topSpeed = 0, minYaw = 10, maxYaw = -10;
    for (uint64_t i = 0; i < 9 * 60; ++i) {
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
    EXPECT_GT(glm::length(bike.pos - start), 4000);
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

// A save taken mid-ride restores the ridden bike and Lara on it; both worlds then drive the same.
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
    EXPECT_LT(glm::length(pa - pb), 1.0f);
    EXPECT_NEAR(a.ridden()->speed, b.ridden()->speed, 1.0f);
}
