// Group B chart conditions / actions (src/game/chart/Ledge.cpp): ledge, hang, shimmy lookahead, clearance classes,
// hurdles. Synthetic one-transition charts evaluate a condition through the runtime (S0 -> S1 when it holds); the
// real-data tests read lara.drm's player data (OU_GAME_DIR).
#include "cdc/Cdc.hpp"
#include "game/Chart.hpp"

#include <glm/gtc/constants.hpp>
#include <gtest/gtest.h>

#include <bit>

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace ou::game::chart;

namespace {

// A ledge context: a straight two-segment ledge along +x (0..200..400 at z 100), Lara hanging at `param` facing +y
// (into the wall), a world whose only obstacle is `wallX` (rays crossing x = wallX hit).
struct LedgeContext : ChartContext {
    std::vector<glm::vec3> pts{{0, 0, 100}, {200, 0, 100}, {400, 0, 100}};
    float param = 0.5f, stickLen = 0;
    glm::vec2 stickDir{0};
    bool attached = true;
    uint32_t flags = 0x40000000; // ledge
    std::optional<float> wallX, wallY;
    int clearance = 2;
    uint32_t ms = 0;
    Action action(int) const override { return {}; }
    float stickLevel() const override { return stickLen; }
    glm::vec2 stick() const override { return stickDir * stickLen; }
    MarkupSlot attachedMarkup() const override {
        if (!attached) return {};
        MarkupSlot s;
        s.markup = 0, s.flags = flags, s.param = param;
        const int seg = std::min(int(param), 1);
        s.point = pts[size_t(seg)] + (pts[size_t(seg) + 1] - pts[size_t(seg)]) * (param - float(seg));
        s.normal = {0, -1, 0};
        return s;
    }
    std::optional<std::pair<glm::vec3, glm::vec3>> markupSegment(int m, int seg) const override {
        if (m != 0 || seg < 0) return {};
        const size_t i = std::min(size_t(seg), pts.size() - 2); // 0x5C3AA0 clamps past the last segment
        return std::pair{pts[i], pts[i + 1]};
    }
    int markupSegmentCount(int m) const override { return m == 0 ? int(pts.size()) - 1 : 0; }
    std::optional<float> playerRay(glm::vec3 a, glm::vec3 b) const override {
        if (wallY && (a.y - *wallY) * (b.y - *wallY) <= 0 && a.y != b.y) return std::abs(*wallY - a.y) / std::abs(b.y - a.y) * glm::length(b - a);
        if (!wallX || (a.x - *wallX) * (b.x - *wallX) > 0 || a.x == b.x) return {};
        return std::abs(*wallX - a.x) / std::abs(b.x - a.x) * glm::length(b - a);
    }
    bool hasCollision() const override { return true; }
    glm::vec3 position() const override { return attachedMarkup().point + glm::vec3(0, -50, -500); }
    float heading() const override { return glm::half_pi<float>(); } // facing +y
    float controlYaw() const override { return glm::half_pi<float>(); }
    int ledgeClearanceClass() const override { return clearance; }
    uint32_t gameMs() const override { return ms; }
};

// S0 (root, props) -> S1 when every condition holds; returns whether the transition fired at start.
struct OneShot {
    Charts c;
    Registry reg;
    std::unique_ptr<ChartRuntime> rt;
    explicit OneShot(std::vector<Cond> conds, std::vector<Property> props = {}) {
        registerFamilies(reg, reg);
        c.player.root = 0;
        c.player.states.resize(2);
        c.player.states[1].parent = 0;
        c.player.states[0].props = std::move(props);
        c.player.transitions.push_back({1, {}, std::move(conds)});
        c.player.states[0].transitions.push_back(0);
        c.bytes.resize(0x2000);
        c.variables = {{4, std::bit_cast<uint32_t>(40.0f)}};
    }
    void pd16(uint32_t off, int16_t v) { std::memcpy(c.bytes.data() + off, &v, 2); }
    bool fires(ChartContext& ctx) {
        rt = std::make_unique<ChartRuntime>(c, c.player, reg);
        rt->start(ctx);
        return rt->state() == 1;
    }
};
Cond cond(int16_t id, int16_t value, bool neg = false, int16_t group = 600) { return {group, id, value, uint8_t(neg ? 1 : 0)}; }
Property intProp(uint16_t id, int v) { return {id, 0x10, uint32_t(v), -1}; }

} // namespace

TEST(ChartLedge, ClearanceClassValuesMapPlayerEDC) {
    LedgeContext ctx;
    ctx.playerInt(0xEDC) = 2;
    EXPECT_TRUE(OneShot({cond(0x28A, 650)}).fires(ctx));  // 650: class 2 (climb up)
    EXPECT_FALSE(OneShot({cond(0x28A, 649)}).fires(ctx)); // 649: class 11
    ctx.playerInt(0xEDC) = 11;
    EXPECT_TRUE(OneShot({cond(0x28A, 649)}).fires(ctx));
    ctx.playerInt(0xEDC) = 8;
    EXPECT_TRUE(OneShot({cond(0x28A, 657)}).fires(ctx));  // 657: class 8 (wall-hug shelf)
    EXPECT_TRUE(OneShot({cond(0x28A, 1)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x28A, 700)}).fires(ctx)); // unknown value: false
}

TEST(ChartLedge, AttachInitStoresTheClearanceClass) {
    LedgeContext ctx;
    ctx.clearance = 5;
    struct Probe { static void run(ChartContext& c, uint16_t) { markupAttachInit(c); } };
    OneShot s({cond(0x28A, 651)}); // class 5
    s.c.player.states[0].actions = {9999};
    s.reg.action(9999, {Probe::run, nullptr, nullptr, nullptr, nullptr, nullptr}, "attach", "test");
    EXPECT_TRUE(s.fires(ctx));
    EXPECT_EQ(ctx.playerInt(0xEDC), 5);
}

TEST(ChartLedge, StickDirectionClassesAgainstHerFacing) {
    LedgeContext ctx; // hanging, facing +y
    ctx.stickLen = 1;
    ctx.stickDir = {-1, 0}; // world -x = her left
    EXPECT_TRUE(OneShot({cond(0x195, 452, false, 400)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x195, 453, false, 400)}).fires(ctx));
    ctx.stickDir = {1, 0};
    EXPECT_TRUE(OneShot({cond(0x195, 453, false, 400)}).fires(ctx));
    ctx.stickDir = {0, 1};
    EXPECT_TRUE(OneShot({cond(0x195, 450, false, 400)}).fires(ctx));
    ctx.stickDir = {0, -1};
    EXPECT_TRUE(OneShot({cond(0x195, 451, false, 400)}).fires(ctx));
    ctx.stickLen = 0;
    EXPECT_TRUE(OneShot({cond(0x195, 454, false, 400)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x195, 411, true, 400)}).fires(ctx)); // other values: false even negated
}

TEST(ChartLedge, AttachedMarkupClassAndBits) {
    LedgeContext ctx;
    EXPECT_TRUE(OneShot({cond(0x263, 610)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x263, 625)}).fires(ctx));
    EXPECT_TRUE(OneShot({cond(0x263, 1)}).fires(ctx));
    EXPECT_TRUE(OneShot({cond(0x26C, 691, false, 620)}).fires(ctx)); // +0xED4 == 0
    ctx.playerInt(0xED4) = 2;
    EXPECT_FALSE(OneShot({cond(0x26C, 691, false, 620)}).fires(ctx));
    EXPECT_TRUE(OneShot({cond(0x26C, 692, false, 620)}).fires(ctx)); // +0xED4 != +0xED8
    ctx.attached = false;
    EXPECT_TRUE(OneShot({cond(0x263, 0)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x26C, 694, false, 620)}).fires(ctx));
}

TEST(ChartLedge, NegationQuirks) {
    LedgeContext ctx;
    // 0x2B4: the negation is applied twice and cancels (segment 200 long)
    EXPECT_FALSE(OneShot({cond(0x2B4, 100, false, 20)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x2B4, 100, true, 20)}).fires(ctx));
    EXPECT_TRUE(OneShot({cond(0x2B4, 250, true, 20)}).fires(ctx));
    // 0x281: without a markup flagged 0x0A010000 false whatever the negation
    EXPECT_FALSE(OneShot({cond(0x281, 30, true, 20)}).fires(ctx));
    // 0x960: always true, even negated; records the play
    EXPECT_TRUE(OneShot({cond(0x960, 20, true, 2400)}).fires(ctx));
}

TEST(ChartLedge, VerticalClearanceRay) {
    LedgeContext ctx; // no obstacle: a ray up is clear
    EXPECT_TRUE(OneShot({cond(0x20, 0, false, 1000)}).fires(ctx)); // variable 0 = 40 cm
}

// The ledge lookahead 0x735970: shimmying right (limb class 4, property 499 = 453) toward the ledge's end at x 400
// within Lara data +0x6DC sets bit 4 (cond 0x280 value 642); a wall across the walk sets bit 1 (640).
TEST(ChartLedge, LedgeLookaheadEndAndBlock) {
    LedgeContext ctx;
    ctx.param = 1.8f; // x 360: 40 short of the end
    const auto setup = [](OneShot& s) {
        s.pd16(0x6F2, 120); // walk distance
        s.pd16(0x6DC, 60);  // end reach
        s.pd16(0x6F6, 20), s.pd16(0x6F8, 300), s.pd16(0x708, 30), s.pd16(0x70A, 300), s.pd16(0x250, -160);
        s.c.player.states[0].actions = {404};
    };
    OneShot end({cond(0x280, 642, false, 640)}, {intProp(499, 453)});
    setup(end);
    EXPECT_TRUE(end.fires(ctx));
    OneShot left({cond(0x280, 642, false, 640)}, {intProp(499, 452)});
    setup(left);
    EXPECT_FALSE(left.fires(ctx)); // walking left: no end within reach
    ctx.wallX = 300.0f;
    ctx.param = 0.2f; // x 40, walking right 120 stays short of the wall
    OneShot clear({cond(0x280, 640, false, 640)}, {intProp(499, 453)});
    setup(clear);
    EXPECT_FALSE(clear.fires(ctx));
    ctx.param = 0.95f; // x 190: the walk reaches x 310
    OneShot blocked({cond(0x280, 640, false, 640)}, {intProp(499, 453)});
    setup(blocked);
    EXPECT_TRUE(blocked.fires(ctx));
}

TEST(ChartLedge, HurdleTypeReadsPlayer6C) {
    LedgeContext ctx;
    ctx.playerInt(0x6C) = 2;
    EXPECT_TRUE(OneShot({cond(0x1D1, 2, false, 470)}).fires(ctx));
    EXPECT_FALSE(OneShot({cond(0x1D1, 2, false, 600)}).fires(ctx)); // only group 470
}

// lara.drm player data constants the ports read (docs/GAMEPLAY.md hang clearance: +0x6E8 100, +0x6EA 139, +0x6F0
// 252; catalog: +0x70A 300, +0x250 -160, +0x6E2 15).
TEST(ChartLedgeReal, PlayerDataConstants) {
    const char* dir = std::getenv("OU_GAME_DIR");
    if (!dir) GTEST_SKIP() << "OU_GAME_DIR not set";
    const ou::cdc::BigfileSet bf(dir);
    const auto c = loadCharts(bf);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->data<int16_t>(0x6E2), 15);
    EXPECT_EQ(c->data<int16_t>(0x6E8), 100);
    EXPECT_EQ(c->data<int16_t>(0x6EA), 139);
    EXPECT_EQ(c->data<int16_t>(0x6F0), 252);
    EXPECT_EQ(c->data<int16_t>(0x70A), 300);
    EXPECT_EQ(c->data<int16_t>(0x250), -160);
    for (uint32_t o : {0x6DC, 0x6F2, 0x6F4, 0x6F6, 0x6F8, 0x6FC, 0x6FE, 0x700, 0x702, 0x706, 0x708})
        std::printf("lara data +0x%X = %d\n", o, c->data<int16_t>(o));
}

// Every B-owned condition id the chart uses is registered (0x264 excepted: the type-27 markup query is not ported).
TEST(ChartLedge, OwnedConditionsRegistered) {
    Registry r;
    registerFamilies(r, r);
    for (int16_t id : {0x14, 0x15, 0x20, 0x67, 0x6E, 0x9E, 0xD0, 0xD1, 0xD6, 0xD7, 0xD8, 0x195, 0x199, 0x19B, 0x19C, 0x1D1, 0x1FF,
                       0x200, 0x201, 0x213, 0x263, 0x26C, 0x280, 0x281, 0x284, 0x28A, 0x2B4, 0x3EB, 0x960})
        EXPECT_NE(r.condition(id), nullptr) << std::hex << id;
    for (uint16_t id : {205, 213, 232, 301, 303, 404, 408, 506, 513, 514, 516, 606, 613, 2003, 2004})
        EXPECT_NE(r.action(id), nullptr) << id;
}

// Action 230's attach (0x74ED30 -> 0x6FDBE0 / 0x707B90 -> 0x707730): the chart-side +0xC94 takes the jump target, the
// old markup goes to +0xEA0 (index + 1), and the jump target is handed on (+0xA64 = 0).
TEST(ChartLedge, ChartSideAttachHandsTheJumpTargetOn) {
    LedgeContext ctx;
    ChartContext::MarkupSlot t;
    t.markup = 7, t.flags = 0x40000000, t.param = 0.25f;
    attachChartMarkup(ctx, t); // the context keeps no chart-side slot: nothing happens
    EXPECT_FALSE(ctx.chartAttached);
    ctx.chartAttached = ChartContext::MarkupSlot{};
    ctx.chartTarget.target = t;
    attachChartMarkup(ctx, t);
    ASSERT_TRUE(ctx.chartAttached && *ctx.chartAttached);
    EXPECT_EQ(ctx.chartAttached->markup, 7);
    EXPECT_FALSE(ctx.chartTarget.target);
    EXPECT_EQ(ctx.handedTarget, 7);
    detachChartMarkup(ctx);
    EXPECT_FALSE(*ctx.chartAttached);
    EXPECT_EQ(ctx.playerInt(0xEA0), 8);
    EXPECT_TRUE(attachTargetClass(610) && attachTargetClass(627) && !attachTargetClass(611) && !attachTargetClass(628));
}

// 0x74F480 / 0x744E90: a wall under the hang (the ray from 90 out to Lara data +0x6E6 behind the ledge line, in the
// segment frame) makes the attach surface class 0 (braced), none 1 (free hang, chapel S447).
TEST(ChartLedge, LedgeBraceWallUnderTheHang) {
    LedgeContext ctx;
    OneShot s({cond(0x1, 1, false, 500)});
    s.pd16(0x6E4, 200), s.pd16(0x6E6, 20), s.pd16(0x252, 498), s.pd16(0x704, 100);
    ASSERT_TRUE(s.fires(ctx)); // the runtime is current for the property / data reads
    const auto a = ctx.attachedMarkup();
    EXPECT_EQ(ledgeBrace(ctx, a), std::optional<bool>(false));
    ctx.wallY = 10.0f; // a wall face just behind the ledge line (the ledge runs +x: its frame's outward normal is -y)
    EXPECT_EQ(ledgeBrace(ctx, a), std::optional<bool>(true));
    ctx.wallY = 40.0f; // beyond +0x6E6 = 20
    EXPECT_EQ(ledgeBrace(ctx, a), std::optional<bool>(false));
}
