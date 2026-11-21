// Lara's state chart: loader on lara.drm (OU_GAME_DIR) and the runtime's evaluation order on synthetic charts
// (docs/re/PLAYER_CHART.md "Runtime").
#include "cdc/Cdc.hpp"
#include "game/Chart.hpp"
#include "game/Game.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <set>
#include <sstream>
#include <tuple>

using namespace ou::game::chart;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

// A context whose inputs the test sets; condition 0x7000 + n is "flag n is set".
struct TestContext : ChartContext {
    std::set<int> flags;
    std::map<int, Action> actions;
    float stick = 0;
    Action action(int id) const override {
        const auto it = actions.find(id);
        return it == actions.end() ? Action{} : it->second;
    }
    float stickLevel() const override { return stick; }
    std::optional<bool> sprint;
    std::optional<bool> canSprint() const override { return sprint; }
};
bool flagCond(ChartContext& ctx, const Cond& c) { return static_cast<TestContext&>(ctx).flags.contains(c.value); }
Cond flag(int16_t n, bool neg = false) { return {0, 0x7000, n, uint8_t(neg ? 1 : 0)}; }

std::vector<std::string> trail; // actions run, in order
void recEntry(ChartContext&, uint16_t a) { trail.push_back("entry" + std::to_string(a)); }
void recReentry(ChartContext&, uint16_t a) { trail.push_back("reentry" + std::to_string(a)); }
void recPre(ChartContext&, uint16_t a) { trail.push_back("pre" + std::to_string(a)); }
void recExit(ChartContext&, uint16_t a) { trail.push_back("exit" + std::to_string(a)); }

Registry testRegistry() {
    Registry r;
    registerFamilies(r, r);
    r.condition(0x7000, flagCond, "Flag", "test");
    for (uint16_t a : {1, 2, 3}) r.action(a, {recEntry, recReentry, recPre, recPre, recExit, nullptr}, "rec", "test");
    return r;
}

// S0 root (parent of S1, S2), S1 ground, S2 other, S3 child of S1.
Charts testCharts() {
    Charts c;
    auto& ch = c.player;
    ch.root = 0;
    ch.states.resize(4);
    ch.states[1].parent = 0, ch.states[2].parent = 0, ch.states[3].parent = 1;
    ch.states[0].actions = {1};
    ch.states[1].actions = {2};
    ch.states[3].actions = {3};
    c.variables = {{1, 7}, {3, 0x40400000u /* 3.0f */}};
    return c;
}
int addTransition(Charts& c, int from, int16_t to, std::vector<Cond> conds, std::vector<int16_t> flags = {}) {
    c.player.transitions.push_back({to, std::move(flags), std::move(conds)});
    const int t = int(c.player.transitions.size()) - 1;
    if (from >= 0) c.player.states[size_t(from)].transitions.push_back(int16_t(t));
    return t;
}

} // namespace

TEST(ChartReal, LoadsBothChartsAndVariables) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->player.states.size(), 1445u);
    EXPECT_EQ(c->player.transitions.size(), 2374u);
    EXPECT_EQ(c->player.events.size(), 69u);
    EXPECT_EQ(c->player.root, 0);
    EXPECT_EQ(c->upper.states.size(), 103u);
    EXPECT_EQ(c->upper.transitions.size(), 203u);
    EXPECT_EQ(c->upper.events.size(), 14u);
    EXPECT_EQ(c->upper.agent, 1);
    ASSERT_EQ(c->variables.size(), 400u);
    EXPECT_NEAR(c->variables[344].asFloat(), 632.111f, 0.01f); // the stumble landing's fall (docs/GAMEPLAY.md)
    EXPECT_EQ(c->variables[347].asInt(), 4);                    // float 4.0 truncated
    EXPECT_EQ(c->variables[193].asInt(), 10);
    EXPECT_EQ(c->data<int16_t>(0x6E2), 15); // Lara data +0x6E2: a held button's long-hold threshold
    EXPECT_FALSE(c->requests.empty());
}

TEST(ChartReal, StandingStateTransitionOrder) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    const State& s = c->player.states[64];
    EXPECT_EQ(s.parent, 10);
    ASSERT_GE(s.transitions.size(), 5u);
    EXPECT_EQ(std::vector<int16_t>(s.transitions.begin(), s.transitions.begin() + 5), (std::vector<int16_t>{69, 82, 81, 83, 84}));
    const auto t56 = std::ranges::find(s.transitions, int16_t(56));
    ASSERT_NE(t56, s.transitions.end());
    EXPECT_LT(std::ranges::find(s.transitions, int16_t(89)), t56); // T56 comes after T89 in S64's list
    const Transition& t = c->player.transitions[56];
    EXPECT_EQ(t.target, 245);
    ASSERT_EQ(t.conds.size(), 5u);
    // !state time >= 3, Jump pressed (0x65 mode 3), 0x836 / -1, !stick at full (0x191 404), !stick back (0x192 411).
    EXPECT_EQ(t.conds[0].id, 0x14); EXPECT_EQ(t.conds[0].value, 3); EXPECT_TRUE(t.conds[0].negate());
    EXPECT_EQ(t.conds[1].id, 0x65); EXPECT_EQ(t.conds[1].value, 3); EXPECT_FALSE(t.conds[1].negate());
    EXPECT_EQ(t.conds[2].id, 0x836); EXPECT_EQ(t.conds[2].value, -1);
    EXPECT_EQ(t.conds[3].id, 0x191); EXPECT_EQ(t.conds[3].value, 404); EXPECT_TRUE(t.conds[3].negate());
    EXPECT_EQ(t.conds[4].id, 0x192); EXPECT_EQ(t.conds[4].value, 411); EXPECT_TRUE(t.conds[4].negate());
}

// The request's clip starts at (start - blend) frames (0x4428A0 -> 0x441FB0): ng_standing_jump (request 52, blend 6,
// start 6, 10 frames) plays all 10 frames, so S248 leaves (S278 T256, AnimationEnded) 19 ticks after the tick that
// entered it, as under Wine (W/chart/diff/J: press tick 10, flight tick 29); ng_run_start (716, 4 / 6, 27 frames) from
// its frame 2: S63 -> S65 (T246, ended + full stick) 25 frames on. enter() stands for the entering tick minus its own
// clock step, hence one tick more here.
TEST(ChartReal, ClipClockStartsAtStartMinusBlend) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    const Registry& reg = Registry::player();
    TestContext ctx;
    for (const int a : {52, 50, 51, 42, 99, 32, 112, 34, 25, 28, 79}) ctx.actions[a] = {true, false, false, 1.0f};
    const auto leaves = [&](int state, float stick, int& ticks) {
        ChartRuntime rt(*c, c->player, reg);
        ctx.stick = stick;
        rt.enter(ctx, state);
        ticks = 0;
        while (rt.state() == state && ticks < 200) rt.tick(ctx, 0.5f), ++ticks;
        return rt.lastTransition();
    };
    int ticks = 0;
    EXPECT_EQ(leaves(248, 0.0f, ticks), 256);
    EXPECT_EQ(ticks, 20);
    EXPECT_EQ(leaves(63, 1.0f, ticks), 246);
    EXPECT_EQ(ticks, 50);
    // Kind-2 requests name an anim graph node (0x443720 -> 0x43E250), not a pattern: the node's program picks the pattern.
    // S245's node 213 plays 680 ng_planted_jump_forward (start 1, blend 4, 5 frames) whatever played before; Play queues
    // it for 0x4428A0, whose lead is cut to 0: 5 frames here [U: Wine's S245 lasts 15 ticks; the queue's end callback
    // (0x43E0C0) and 0x4428A0's crossfade branch are not modelled].
    ChartRuntime rt(*c, c->player, reg);
    rt.enter(ctx, 245);
    EXPECT_EQ(rt.playingPattern(), 680);
    ASSERT_NE(rt.pattern(), nullptr);
    EXPECT_EQ(rt.pattern()->clip, "ng_planted_jump_forward");
    EXPECT_EQ(leaves(245, 0.0f, ticks), 256);
    EXPECT_EQ(ticks, 10);
    rt.enter(ctx, 248);
    EXPECT_EQ(rt.playingPattern(), 52);
    rt.enter(ctx, 245); // IsPlaying(52) branch
    EXPECT_EQ(rt.playingPattern(), 680);
    // S40's node 46 plays 685 or 691 by module 1's native 76 (0x6EA1E0 = 0x74FD70 on the sprint stamina, ctx.canSprint());
    // unknown stamina leaves it unresolved (no clip clock, never a guess).
    rt.enter(ctx, 40);
    EXPECT_EQ(rt.pattern(), nullptr);
    // Stamina left: ng_idle / ng_run_start; spent: ng_idle_break_hot / ng_run_exhaust (node 99 asks 76 first).
    for (const auto& [sprint, idle, run] : {std::tuple{true, 685, 716}, std::tuple{false, 691, 717}}) {
        ctx.sprint = sprint;
        rt.enter(ctx, 245), rt.enter(ctx, 40);
        EXPECT_EQ(rt.playingPattern(), idle);
        rt.enter(ctx, 65);
        EXPECT_EQ(rt.playingPattern(), run);
    }
}

TEST(ChartRuntime, TickSetsContextTimeMult) {
    Charts c = testCharts();
    const Registry reg = testRegistry();
    ChartRuntime rt(c, c.player, reg);
    TestContext ctx;
    rt.start(ctx);
    rt.tick(ctx, 0.25f);
    EXPECT_EQ(ctx.timeMult, 0.25f);
    EXPECT_EQ(rt.stateTime(), 0.25f);
}

TEST(ChartReal, DumpAndCoverageRun) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(gameDir());
    std::ostringstream o;
    EXPECT_EQ(dump(bf, 64, true, false, nullptr, o), 0);
    EXPECT_NE(o.str().find("S64 "), std::string::npos);
    EXPECT_NE(o.str().find("summary: conditions"), std::string::npos);
}

TEST(ChartRuntime, OwnTransitionsFirstThenParentFirstMatchWins) {
    Charts c = testCharts();
    addTransition(c, 1, 2, {flag(1)}); // own, first
    addTransition(c, 1, 3, {flag(1)}); // own, second: never taken while the first holds
    addTransition(c, 0, 2, {flag(2)}); // inherited from the root
    c.player.states[0].transitions.clear();
    const int toGround = addTransition(c, 0, 1, {flag(9)});
    (void)toGround;
    addTransition(c, 0, 2, {flag(2)});
    const Registry reg = testRegistry();
    ChartRuntime rt(c, c.player, reg);
    TestContext ctx;
    ctx.flags = {9};
    rt.start(ctx); // root -> S1 on its first evaluation
    EXPECT_EQ(rt.state(), 1);
    ctx.flags = {1, 2};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 2); // S1's own T0 before its second and before the root's
    ChartRuntime rt2(c, c.player, reg);
    ctx.flags = {9};
    rt2.start(ctx);
    ctx.flags = {2};
    rt2.tick(ctx, 0.5f);
    EXPECT_EQ(rt2.state(), 2); // nothing of S1's holds: the parent's
}

TEST(ChartRuntime, StateTimeCountsThirtyHertzFramesAndResets) {
    Charts c = testCharts();
    addTransition(c, 0, 1, {flag(9)});
    addTransition(c, 1, 2, {{20, 0x14, 3, 0}}); // state time >= 3
    const Registry reg = testRegistry();
    ChartRuntime rt(c, c.player, reg);
    TestContext ctx;
    ctx.flags = {9};
    rt.start(ctx);
    ASSERT_EQ(rt.state(), 1);
    EXPECT_EQ(rt.stateTime(), 0.0f);
    int ticks = 0;
    while (rt.state() == 1 && ticks < 20) rt.tick(ctx, 0.5f), ++ticks; // 60 Hz: timeMult 0.5
    EXPECT_EQ(ticks, 6);  // 6 x 0.5 = 3 frames: int(3.0) >= 3 on the sixth tick
    EXPECT_EQ(rt.state(), 2);
    EXPECT_EQ(rt.stateTime(), 0.0f); // reset on the change
    // The variable form (group 1000): variable 1 = 3.0 -> 3.
    EXPECT_EQ(c.value(Cond{1000, 0x14, 1, 0}), 3);
    EXPECT_EQ(c.value(Cond{1000, 0x14, 0, 0}), 7);
}

TEST(ChartRuntime, ActionsExitEnterAndReentryParentFirst) {
    Charts c = testCharts();
    addTransition(c, 0, 1, {flag(9)});
    addTransition(c, 1, 3, {flag(1)});
    addTransition(c, 3, 2, {flag(2)});
    const Registry reg = testRegistry();
    ChartRuntime rt(c, c.player, reg);
    TestContext ctx;
    trail.clear();
    ctx.flags = {9};
    rt.start(ctx);
    EXPECT_EQ(trail, (std::vector<std::string>{"entry1", "pre1", "pre2", "reentry1", "entry2"}));
    trail.clear();
    ctx.flags = {1};
    rt.tick(ctx, 0.5f); // S1 -> S3 (child): 1 and 2 stay (re-entry), 3 is new
    EXPECT_EQ(trail, (std::vector<std::string>{"pre1", "pre2", "pre3", "reentry1", "reentry2", "entry3"}));
    trail.clear();
    ctx.flags = {2};
    rt.tick(ctx, 0.5f); // S3 -> S2: exits 2 and 3 (parent first), 1 stays
    EXPECT_EQ(trail, (std::vector<std::string>{"exit2", "exit3", "pre1", "reentry1"}));
}

TEST(ChartRuntime, AnimEndTransitionsWaitAndUnportedConditionsFail) {
    Charts c = testCharts();
    addTransition(c, 0, 1, {flag(9)});
    addTransition(c, 1, 2, {flag(1)}, {1});            // flag 1: only on the animation-end tick
    addTransition(c, 2, 3, {{0, 0x5555, 0, 1}});       // unported (negated: still false)
    const Registry reg = testRegistry();
    ChartRuntime rt(c, c.player, reg);
    TestContext ctx;
    ctx.flags = {9};
    rt.start(ctx);
    ctx.flags = {1};
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 1);
    EXPECT_EQ(rt.queued(), 1); // held back
    rt.animEnded(ctx);         // counter -> 1: the queued transition fires
    EXPECT_EQ(rt.state(), 2);
    rt.tick(ctx, 0.5f);
    EXPECT_EQ(rt.state(), 2);
    EXPECT_GT(rt.stats.unportedConds.at(0x5555), 0u);
}

TEST(ChartRuntime, ButtonTimingValues) {
    EXPECT_EQ(actionValue({true, true, true, 0}), 1);
    EXPECT_EQ(actionValue({true, true, false, 0.01f}), 2);  // at least one frame held
    EXPECT_EQ(actionValue({true, true, false, 0.5f}), 16);
    EXPECT_EQ(actionValue({true, false, true, 0}), -1);
    EXPECT_EQ(actionValue({true, false, false, 0.5f}), -16);
}

// --input-script's chart-contract form: [[tick, mask], ...] and a Wine case object shifted by its hold.
TEST(InputScriptMask, ContractMasks) {
    const auto s = ou::game::InputScript::parse("[[0,0],[10,1],[40,17],[46,1],[70,0]]");
    EXPECT_EQ(s.at(9).move.y, 0.0f);
    EXPECT_EQ(s.at(10).move.y, 1.0f);
    EXPECT_FALSE(s.at(39).jump);
    EXPECT_TRUE(s.at(40).jump);
    EXPECT_FALSE(s.at(46).jump);
    EXPECT_EQ(s.at(70).move.y, 0.0f);
    const auto m = ou::game::InputScript::parse("[[0,2],[5,12],[6,960]]");
    EXPECT_EQ(m.at(0).move.y, -1.0f);
    EXPECT_EQ(m.at(5).move.x, 0.0f); // left + right cancel
    const auto b = m.at(6);
    EXPECT_TRUE(b.sprint && b.walk && b.grapple && b.drop);
    EXPECT_FALSE(b.jump || b.interact);
    const auto c = ou::game::InputScript::parse(R"({"spawn":{"hold_ticks":60},"input":[[0,0],[10,1]]})");
    EXPECT_EQ(c.at(69).move.y, 0.0f);
    EXPECT_EQ(c.at(70).move.y, 1.0f);
    EXPECT_THROW(ou::game::InputScript::parse("[[1]]"), std::runtime_error);
}

