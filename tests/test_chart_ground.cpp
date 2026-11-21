// Group A chart families (src/game/chart/Ground.cpp): real lara.drm values (OU_GAME_DIR) and the conditions on
// synthetic charts (docs/re/PLAYER_CHART.md "Conditions").
#include "cdc/Cdc.hpp"
#include "game/Chart.hpp"
#include "game/Game.hpp"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdlib>

using namespace ou::game::chart;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

struct GroundContext : ChartContext {
    ou::game::PlayerState s;
    glm::vec2 stickV{0};
    std::map<int, Action> actions;
    Action action(int id) const override {
        const auto it = actions.find(id);
        return it == actions.end() ? Action{} : it->second;
    }
    float stickLevel() const override { return glm::length(stickV); }
    glm::vec2 stick() const override { return stickV; }
    const ou::game::PlayerState* player() const override { return &s; }
};

Property floatProp(uint16_t id, float v) { return {id, 0xC, std::bit_cast<uint32_t>(v), -1}; }

// S0 root with MoveDir sector properties 501..504 = 45 / 135 / -135 / -45 degrees (sectors: 1 forward [-45, 45], 4 left
// [45, 135], 8 right [-135, -45], 2 back) and hysteresis 507 = 10; transitions from S1 to S2..S5 on the four stick
// sectors while the stick is off rest; S2..S5 return to S1 when the stick rests.
Charts moveDirCharts() {
    Charts c;
    auto& ch = c.player;
    ch.root = 0;
    ch.states.resize(6);
    for (size_t i = 1; i < 6; ++i) ch.states[i].parent = 0;
    ch.states[0].props = {floatProp(0x1F5, 45), floatProp(0x1F6, 135), floatProp(0x1F7, -135), floatProp(0x1F8, -45), floatProp(0x1FB, 10),
                          floatProp(0x20A, 0.1f), floatProp(0x20B, 0.7f), floatProp(0x20C, 0.91f), floatProp(0x20D, 0.02f),
                          floatProp(0x20E, 0.02f), floatProp(0x20F, 0.02f)};
    const auto add = [&](int from, int16_t to, std::vector<Cond> conds) {
        ch.transitions.push_back({to, {}, std::move(conds)});
        if (from >= 0) ch.states[size_t(from)].transitions.push_back(int16_t(ch.transitions.size() - 1));
    };
    add(0, 1, {{500, 1, 1, 0}});
    for (int16_t k = 0; k < 4; ++k) add(1, int16_t(2 + k), {{400, 0x191, 401, 1}, {400, 0x192, int16_t(410 + k), 0}});
    for (int16_t k = 0; k < 4; ++k) add(2 + k, 1, {{400, 0x191, 401, 0}});
    return c;
}

} // namespace

TEST(ChartGroundReal, PlayerDataValues) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    // Action 207's markup length bounds (17.5 / 27.5 cm in units of 3.1605 per cm).
    EXPECT_NEAR(c->data<float>(0x800), 55.31f, 0.01f);
    EXPECT_NEAR(c->data<float>(0x804), 86.92f, 0.01f);
    // S0 T25 -> S1 (ng_idle) on PSCCF_IsInCheatMode = 1: the start state's first transition.
    const State& s0 = c->player.states[0];
    ASSERT_FALSE(s0.transitions.empty());
    const Transition& t = c->player.transitions[size_t(s0.transitions[0])];
    EXPECT_EQ(t.target, 1);
    ASSERT_EQ(t.conds.size(), 1u);
    EXPECT_EQ(t.conds[0].id, 0x207);
    EXPECT_EQ(t.conds[0].value, 1);
    // S64 (standing) property 911 = 10 frames: the crouch button's long hold (condition 0x9B).
    const State& s64 = c->player.states[64];
    const auto p911 = std::ranges::find_if(s64.props, [](const Property& p) { return p.id == 911; });
    ASSERT_NE(p911, s64.props.end());
    EXPECT_FLOAT_EQ(c->propFloat(*p911), 10.0f);
}

TEST(ChartGroundReal, StartStateStaysWithoutCheatMode) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    ChartRuntime rt(*c, c->player, Registry::player());
    GroundContext ctx;
    rt.start(ctx);
    // T25 (0x207 = 1) never holds: ours has no cheat mode; the S0 -> S1 gate is closed.
    EXPECT_NE(rt.state(), 1);
}

TEST(ChartGround, MoveDirSectorsAndHysteresis) {
    Charts c = moveDirCharts();
    ChartRuntime rt(c, c.player, Registry::player());
    GroundContext ctx;
    ctx.s.yaw = 0; // facing +x; + angles are her left (+y)
    rt.start(ctx);
    ASSERT_EQ(rt.state(), 1);
    ctx.stickV = {1, 0};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 2); // forward: 410
    ctx.stickV = {};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 1);
    ctx.stickV = {0, -1};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 5); // right (-90 degrees): sector 8 = [C, D] = 413
    ctx.stickV = {};
    rt.tick(ctx, 0.5f);
    ctx.stickV = {0, 1};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 4); // left: sector 4 = [A, B] = 412
    ctx.stickV = {};
    rt.tick(ctx, 0.5f);
    ctx.stickV = {-1, 0};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 3); // back: 411
    ctx.stickV = {};
    rt.tick(ctx, 0.5f);
    // Hysteresis: last sector 2 (back) widened by 10 degrees; 140 degrees left stays "back" (bound 135 - 10 = 125).
    const float a = glm::radians(140.0f);
    ctx.stickV = {std::cos(a), std::sin(a)};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 3);
}

TEST(ChartGround, EdgeCodedCheatModeAndSlopeCounter) {
    Charts c;
    c.player.states.resize(4);
    for (size_t i = 1; i < 4; ++i) c.player.states[i].parent = 0;
    c.player.transitions = {{1, {}, {{500, 0x207, 0, 0}}}, {2, {}, {{500, 0x1FD, 1, 0}}}, {3, {}, {{500, 0x1FD, 0, 0}}}};
    c.player.states[0].transitions = {0};
    c.player.states[1].transitions = {1};
    c.player.states[2].transitions = {2};
    ChartRuntime rt(c, c.player, Registry::player());
    GroundContext ctx;
    rt.start(ctx);
    EXPECT_EQ(rt.state(), 1); // not in cheat mode (0x207 = 0)
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 1); // not sliding: counter >= 0
    ctx.s.state = ou::game::State::Slide;
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 2);
    EXPECT_EQ(rt.field(0x3C4), -4);
    ctx.s.state = ou::game::State::Idle;
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 3);
    EXPECT_EQ(rt.field(0x3C4), 0);
}

TEST(ChartGround, StickRateMarksTimeForCondition16) {
    // S1 runs action 501 at band 403 (property 521); condition 0x16 (frames since the marker >= 3, negated in Lara's
    // chart) then measures from the stamp.
    Charts c;
    c.player.states.resize(3);
    c.player.states[1].parent = 0, c.player.states[2].parent = 0;
    c.player.states[1].actions = {501};
    c.player.states[1].props = {{0x209, 0x10, 0x193, -1}, floatProp(0x202, 1.0f), floatProp(0x203, 1.5f), floatProp(0x20B, 0.7f),
                                floatProp(0x20C, 0.91f), floatProp(0x20E, 0.02f), floatProp(0x20F, 0.02f)};
    c.player.transitions = {{1, {}, {{500, 1, 1, 0}}}, {2, {}, {{20, 0x16, 3, 0}}}};
    c.player.states[0].transitions = {0};
    c.player.states[1].transitions = {1};
    ChartRuntime rt(c, c.player, Registry::player());
    GroundContext ctx;
    ctx.stickV = {1, 0};
    rt.start(ctx);
    ASSERT_EQ(rt.state(), 1);
    for (int i = 0; i < 20; ++i) rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 1); // the marker is restamped every frame at band 403
    EXPECT_FLOAT_EQ(ctx.anim.rate, 1.5f); // m = 1 at the top of the band: lerp(p514, p515) = 1.5
}
