// Group CD chart conditions and actions (src/game/chart/Climb.cpp) on lara.drm (OU_GAME_DIR): the shared gates
// (0xCA critical section, 0x1F5 water flags, 0x204 mantle probe, 0x836 gear slot) and the climb / beam families.
#include "cdc/Cdc.hpp"
#include "game/Chart.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <functional>

using namespace ou::game::chart;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

struct CDContext : ChartContext {
    std::optional<uint32_t> flags8EC;
    std::optional<int> gear;
    bool tracks = false, world = false;
    glm::vec3 pos{0};
    float yaw = 0;
    std::function<std::optional<RayHit>(glm::vec3, glm::vec3)> ray;
    glm::vec2 stickDir{0};
    std::optional<Water> pool;
    std::optional<float> floorDepth;
    uint32_t ms = 0;
    Action action(int) const override { return {}; }
    float stickLevel() const override { return glm::length(stickDir); }
    glm::vec2 stick() const override { return stickDir; }
    std::optional<Water> water() const override { return pool; }
    std::optional<float> waterFloorDepth() const override { return floorDepth; }
    uint32_t gameMs() const override { return ms; }
    std::optional<uint32_t> waterFlags() const override { return flags8EC; }
    std::optional<int> gearSlot() const override { return gear; }
    bool tracksCriticalSection() const override { return tracks; }
    bool hasCollision() const override { return world; }
    glm::vec3 position() const override { return pos; }
    float heading() const override { return yaw; }
    std::optional<RayHit> rayHit(glm::vec3 a, glm::vec3 b, uint32_t) const override { return ray ? ray(a, b) : std::nullopt; }
};

// The real chart and registry with one evaluation of condition `c` (runtime negation included) in Lara's root state.
struct Rig {
    Charts charts;
    std::unique_ptr<ChartRuntime> rt;
    CDContext ctx;
    int probe = -1; // an added state whose one transition (to S40) carries the condition under test
    explicit Rig(Charts c) : charts(std::move(c)) {
        probe = int(charts.player.states.size());
        charts.player.states.emplace_back();
        charts.player.states.back().transitions = {int16_t(charts.player.transitions.size())};
        charts.player.transitions.emplace_back();
        rt = std::make_unique<ChartRuntime>(charts, charts.player, Registry::player());
    }
    // Condition `c` through the runtime (0x785920; the negation and an unported answer as it treats them).
    bool eval(Cond c) {
        charts.player.transitions.back() = Transition{40, {}, {c}};
        rt->enter(ctx, probe);
        rt->tick(ctx, 0.5f);
        return rt->state() == 40;
    }
};

std::optional<Charts> real() {
    if (!gameDir()) return std::nullopt;
    const ou::cdc::BigfileSet bf(gameDir());
    return loadCharts(bf);
}

TEST(ChartCD, InWaterReadsTheDryBit) {
    auto c = real();
    if (!c) GTEST_SKIP() << "OU_GAME_DIR not set";
    Rig r(std::move(*c));
    r.ctx.flags8EC = 0x400; // dry
    EXPECT_TRUE(r.eval({500, 0x1F5, 0, 0}));
    EXPECT_FALSE(r.eval({500, 0x1F5, 1, 0}));
    EXPECT_TRUE(r.eval({500, 0x1F5, 1, 1})); // !in water: every S0 exit
    r.ctx.flags8EC = 0x100;                 // wading
    EXPECT_TRUE(r.eval({500, 0x1F5, 1, 0}));
    EXPECT_FALSE(r.eval({500, 0x1F5, 0, 0}));
    r.ctx.flags8EC = 0; // no handler: false whatever the negation
    EXPECT_FALSE(r.eval({500, 0x1F5, 1, 1}));
    EXPECT_FALSE(r.eval({500, 0x1F5, 0, 0}));
    r.ctx.flags8EC = 0x400; // unhandled mode: false whatever the negation
    EXPECT_FALSE(r.eval({500, 0x1F5, 7, 1}));
}

TEST(ChartCD, GearSlotAndCriticalSection) {
    auto c = real();
    if (!c) GTEST_SKIP() << "OU_GAME_DIR not set";
    Rig r(std::move(*c));
    r.ctx.gear = -1;
    EXPECT_TRUE(r.eval({2100, 0x836, -1, 0}));
    EXPECT_FALSE(r.eval({2100, 0x836, 3, 0}));
    r.ctx.gear = std::nullopt;
    EXPECT_FALSE(r.eval({2100, 0x836, -1, 1})); // unknown: unported, never true
    EXPECT_FALSE(r.eval({200, 0xCA, 0, 0}));    // nobody tracks the critical section
    r.ctx.tracks = true;
    EXPECT_TRUE(r.eval({200, 0xCA, 0, 0}));
    r.ctx.playerInt(0x78) = 1;
    EXPECT_TRUE(r.eval({200, 0xCA, 1, 0}));
    EXPECT_TRUE(r.eval({200, 0xCA, 3, 0})); // just entered (agent +0x6C still 0)
    r.rt->field(0x6C) = 1;
    EXPECT_FALSE(r.eval({200, 0xCA, 3, 0}));
    r.ctx.playerInt(0x78) = 0;
    EXPECT_TRUE(r.eval({200, 0xCA, 2, 0})); // just left
}

// 0x204 on lara.drm's numbers: low ray (z +100, 150 long) hits a wall 100 ahead, the high ray (z +400, 100 + 130 long)
// is clear, the down ray finds a top. lara.drm +0xDB8 = 0 accepts only an instance's still body.
TEST(ChartCD, MantleProbeAcceptsOnlyStillInstances) {
    auto c = real();
    if (!c) GTEST_SKIP() << "OU_GAME_DIR not set";
    Rig r(std::move(*c));
    r.ctx.world = true;
    int kind = 1;
    std::optional<bool> still;
    r.ctx.ray = [&](glm::vec3 a, glm::vec3 b) -> std::optional<ChartContext::RayHit> {
        if (a.z == 100 && b.z == 100) return ChartContext::RayHit{1, {100, 0, 100}, {-1, 0, 0}, {}, {}};
        if (a.z == 400 && b.z == 400) {
            EXPECT_NEAR(b.x, 230, 1e-3);
            return std::nullopt;
        }
        if (b.z == 100) return ChartContext::RayHit{kind, {230, 0, 300}, {0, 0, 1}, still, false};
        return std::nullopt;
    };
    EXPECT_FALSE(r.eval({500, 0x204, 1, 0})); // static top: rejected by +0xDB8 = 0
    kind = 2;
    EXPECT_FALSE(r.eval({500, 0x204, 1, 1})); // body unknown: unported
    still = true;
    EXPECT_TRUE(r.eval({500, 0x204, 1, 0}));
    EXPECT_NEAR(r.ctx.agentFloat(0x400), 230, 1e-3);
    EXPECT_NEAR(r.ctx.agentFloat(0x3F0), 100, 1e-3); // pulled back by |(+0xD9C, +0xDA0, +0xDA4)| = 130
    still = false;
    EXPECT_FALSE(r.eval({500, 0x204, 1, 0}));
}

} // namespace

namespace {

// Pillar jump arcs (0x1A9 / 0x1AA, degrees; + = her left), the wall jump count (0x21C, lara.drm +0x843 = 5), the
// swim depth classes (0x326 class >= value; 805 needs -(player +0x910)), action 1409's deadline (0x5AE).
TEST(ChartCD, StickArcsWallJumpsDepthAndDeadline) {
    auto c = real();
    if (!c) GTEST_SKIP() << "OU_GAME_DIR not set";
    Rig r(std::move(*c));
    r.ctx.stickDir = {0, 1}; // her heading 0 = +x: the stick to her left, +90 degrees
    EXPECT_TRUE(r.eval({20, 0x1A9, 78, 0}));
    EXPECT_TRUE(r.eval({20, 0x1AA, 112, 0}));
    EXPECT_FALSE(r.eval({20, 0x1AA, 78, 0}));
    r.rt->field(0x4EC) = 4;
    EXPECT_TRUE(r.eval({500, 0x21C, 1, 0}));
    r.rt->field(0x4EC) = 5;
    EXPECT_FALSE(r.eval({500, 0x21C, 1, 0}));
    r.ctx.pool = ChartContext::Water{true, 500};
    EXPECT_TRUE(r.eval({800, 0x326, 802, 0}));
    EXPECT_FALSE(r.eval({800, 0x326, 805, 0}));
    r.ctx.pool = ChartContext::Water{true, 1000};
    EXPECT_FALSE(r.eval({800, 0x326, 805, 0})); // 804 or 805: +0x910 unknown, unported
    EXPECT_TRUE(r.eval({800, 0x326, 804, 0}));  // either passes
    r.ctx.floorDepth = 1200;
    EXPECT_TRUE(r.eval({800, 0x326, 805, 0}));
    r.ctx.pool = ChartContext::Water{false, 0};
    EXPECT_TRUE(r.eval({800, 0x321, 800, 0}));
    r.ctx.ms = 10000;
    EXPECT_TRUE(r.eval({1450, 0x5AE, 0, 1})); // deadline 0 is past
    r.ctx.deadline8B083C = 10000 + 3500;
    EXPECT_TRUE(r.eval({1450, 0x5AE, 0, 0}));
}

} // namespace

#include "game/Game.hpp"

namespace {

// A Wine case's camera_follow pins the stick's world yaw (Forward = (dx, dy)); a note string pins nothing.
TEST(InputScriptCase, CameraFollowPinsStickYaw) {
    const auto s = ou::game::InputScript::parse(R"({"spawn":{"hold_ticks":0},"camera_follow":"0,1,1264,300","input":[[0,0]]})");
    ASSERT_TRUE(s.cameraYaw());
    EXPECT_NEAR(*s.cameraYaw(), 1.5707964f, 1e-6f);
    const auto n = ou::game::InputScript::parse(R"({"camera_follow":"attach, then Up held","input":[[0,0]]})");
    EXPECT_FALSE(n.cameraYaw());
    EXPECT_FALSE(ou::game::InputScript::parse("[[0,1]]").cameraYaw());
}

} // namespace
