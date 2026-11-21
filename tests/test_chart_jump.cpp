// Chart group J (src/game/chart/Jump.cpp): jump / air conditions and actions on lara.drm's player data (OU_GAME_DIR)
// and synthetic charts.
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"
#include "game/Chart.hpp"

#include <gtest/gtest.h>

#include <cstdlib>

using namespace ou::game::chart;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

struct JumpContext : ChartContext {
    std::optional<bool> floorless = false, carried = false;
    std::optional<Water> wet = Water{};
    std::optional<glm::vec3> vel = glm::vec3(0);
    MarkupSlot target;
    std::array<int, size_t(JEffect::kCount)> effects{};
    Action action(int) const override { return {}; }
    float stickLevel() const override { return 0; }
    std::optional<bool> noFloor() const override { return floorless; }
    std::optional<bool> carrying() const override { return carried; }
    std::optional<Water> water() const override { return wet; }
    std::optional<glm::vec3> velocity() const override { return vel; }
    MarkupSlot jumpTarget() const override { return target; }
    void effect(JEffect e, float) override { ++effects[size_t(e)]; }
};

// One condition through Lara's registry (the runtime applies the negation; this is the raw truth).
bool cond(JumpContext& ctx, int16_t id, int16_t value, int16_t group = 500) {
    const auto* e = Registry::player().condition(id);
    EXPECT_NE(e, nullptr) << std::hex << id;
    return e && e->fn(ctx, Cond{group, id, value, 0});
}

// A chart whose root S0 has no transitions: start() only binds the context to the runtime.
Charts idleCharts() {
    Charts c;
    c.player.states.resize(3);
    c.player.states[1].parent = 0, c.player.states[2].parent = 0;
    return c;
}

} // namespace

TEST(ChartJump, MarkupClassOfTheJumpTarget) {
    Charts c = idleCharts();
    ChartRuntime rt(c, c.player, Registry::player());
    JumpContext ctx;
    rt.start(ctx);
    EXPECT_TRUE(cond(ctx, 0x262, 0, 600));   // no target: class 0
    EXPECT_FALSE(cond(ctx, 0x262, 1, 600));
    EXPECT_FALSE(cond(ctx, 0x262, 610, 610));
    ctx.target.markup = 7;
    ctx.target.flags = ou::collision::markupFlags(1); // ledgegrab
    rt.tick(ctx, 0.5f);                                // a new update (the class is cached per update)
    EXPECT_TRUE(cond(ctx, 0x262, 1, 600));
    EXPECT_TRUE(cond(ctx, 0x262, 610, 610));
    EXPECT_FALSE(cond(ctx, 0x262, 625, 600)); // hurdle
    EXPECT_TRUE(cond(ctx, 0x26D, 694, 620));  // target valid
    EXPECT_TRUE(cond(ctx, 0x26D, 691, 620));  // player +0xED4 == 0
}

TEST(ChartJump, FallingVelocityAndCarryEdges) {
    Charts c = idleCharts();
    ChartRuntime rt(c, c.player, Registry::player());
    JumpContext ctx;
    rt.start(ctx);
    EXPECT_TRUE(cond(ctx, 0x1F8, 0));
    EXPECT_FALSE(cond(ctx, 0x1F8, 1));
    EXPECT_FALSE(cond(ctx, 0x1F8, 2)); // unhandled value
    ctx.floorless = true;
    EXPECT_TRUE(cond(ctx, 0x1F8, 1));
    EXPECT_FALSE(cond(ctx, 0x20C, 1)); // standing still: no velocity
    ctx.vel = glm::vec3(10, 0, -3);
    EXPECT_TRUE(cond(ctx, 0x20C, 1));
    EXPECT_FALSE(cond(ctx, 0x20A, 1)); // agent +0x3EC stays clear
    // 0x5DD edge cache: rising edge (value 3) only on the update it became true.
    EXPECT_TRUE(cond(ctx, 0x5DD, 0));
    ctx.carried = true;
    rt.tick(ctx, 0.5f);
    EXPECT_TRUE(cond(ctx, 0x5DD, 3));
    EXPECT_TRUE(cond(ctx, 0x5DD, 1));
    rt.tick(ctx, 0.5f);
    EXPECT_FALSE(cond(ctx, 0x5DD, 3));
    EXPECT_TRUE(cond(ctx, 0x5DD, 1));
    ctx.carried.reset(); // the context cannot answer: false for every value (the runtime fails the transition)
    rt.tick(ctx, 0.5f);
    EXPECT_FALSE(cond(ctx, 0x5DD, 0));
    EXPECT_FALSE(cond(ctx, 0x5DD, 1));
}

TEST(ChartJump, WaterDepthClassesFromPlayerData) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    auto real = loadCharts(bf);
    ASSERT_TRUE(real);
    // lara.drm player data +0xAC / +0xAE / +0xB0 / +0xB2 (docs/GAMEPLAY.md: 154 / 391 / 391 / 948).
    EXPECT_EQ(real->data<int16_t>(0xAC), 154);
    EXPECT_EQ(real->data<int16_t>(0xB0), 391);
    EXPECT_EQ(real->data<int16_t>(0xB2), 948);
    Charts c = idleCharts();
    c.bytes = real->bytes, c.dataOffset = real->dataOffset;
    ChartRuntime rt(c, c.player, Registry::player());
    JumpContext ctx;
    rt.start(ctx);
    // Dry: class 800 (counted as 807 by 0x322).
    EXPECT_FALSE(cond(ctx, 0x322, 803, 800));
    EXPECT_FALSE(cond(ctx, 0x323, 802, 800));
    ctx.wet = ChartContext::Water{true, 200};
    rt.tick(ctx, 0.5f);
    EXPECT_TRUE(cond(ctx, 0x323, 802, 800));  // 200 >= 154: 802
    EXPECT_FALSE(cond(ctx, 0x323, 803, 800));
    EXPECT_TRUE(cond(ctx, 0x322, 803, 800));
    EXPECT_TRUE(cond(ctx, 0x325, 802, 800));
    ctx.wet = ChartContext::Water{true, 1000};
    rt.tick(ctx, 0.5f);
    EXPECT_TRUE(cond(ctx, 0x323, 805, 800));
}

TEST(ChartJump, ChainCounterAndStateMessage) {
    Charts c = idleCharts();
    c.player.states[1].actions = {917, 1800};
    c.player.states[1].props = {{917, 2, 1}, {2100, 0x10, 191}, {2101, 0x10, 0}, {2102, 0x10, 1}};
    // S0 -> S1 on carrying (0x5DD 1), S1 -> S2 when not.
    c.player.transitions.push_back({1, {}, {Cond{1500, 0x5DD, 1, 0}}});
    c.player.transitions.push_back({2, {}, {Cond{1500, 0x5DD, 0, 0}}});
    c.player.states[0].transitions = {0};
    c.player.states[1].transitions = {1};
    ChartRuntime rt(c, c.player, Registry::player());
    JumpContext ctx;
    rt.start(ctx);
    ctx.carried = true;
    rt.tick(ctx, 0.5f);
    ASSERT_EQ(rt.state(), 1);
    EXPECT_EQ(rt.field(0x4EC), 1);                                       // prop 917 true: +1
    EXPECT_EQ(ctx.effects[size_t(ChartContext::JEffect::StateMessage)], 1); // prop 2101 = 0: sent on entry
    ctx.carried = false;
    rt.tick(ctx, 0.5f);
    ASSERT_EQ(rt.state(), 2);
    EXPECT_EQ(rt.field(0x4EC), 0); // S2's chain lacks 917: its exit cleared the counter
}
