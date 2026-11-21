// Group J of Lara's chart: the jump / air conditions, the jump-target markup class, the combat / carry / grapple /
// water-depth edges of the ground states and the jump / air / landing / camera actions (docs/re/PLAYER_CHART.md
// "Conditions", "Actions"; ownership openunderworld-work/chart/conditions_owner.json / actions_owner.json, by_group J).
// Behaviour after tru.exe; addresses cited per function, evidence [O] read in the exe / data, [I] inferred. Conditions
// return the raw truth (the runtime applies the negation) except the side-effect conditions 0x8FC / 0x8FE / 0x900,
// which tru.exe makes true whatever their negation (handled at registration: their negate bit is never set in
// lara.drm, catalog usage). A condition whose game state the context cannot give returns unported().
//
// Not registered (they stay counted unported in ChartRuntime::stats), with the reason:
//   0x2BD (grapple component item type), 0x2C4 (global 0xFAE440), 0x2D0 (needs the ray hit's normal), 0x2D4 / 0x2D6 /
//   0x2D7 / 0x2FE (grapple rope geometry ours does not keep), 0x2AF (markup move vector +0x6F0), 0x32F (anim root
//   offset), 0x591 / 0x60F (engagement context +0x854), 0x5DE / 0x5DF (portable data +0x18), 0x7D3 / 0x7D7 / 0x7D8 /
//   0x7DA / 0x7DB (targeting slots 5 / 6), 0x8A0 (tru.exe inventory slot index). Registered but answered by the
//   context, nullopt in shadow mode: 0x217 (the wall probe), 0x385 / 0x386 (combat object +0x50), 0x403 (combat flag
//   bit 10), 0x401 / 0x404 / 0x408 (the upper chart is not run beside Lara's).
#include "game/Chart.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ou::game::chart {

namespace {

using JEffect = ChartContext::JEffect;
constexpr float kPi = std::numbers::pi_v<float>;

int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; } // 0x781FF0

// 0x783BD0: float compare of the condition's value (a variable's for group 1000) against x: mode 1 value <= x, mode 2
// value >= x, else |x - value| <= 1e-8.
bool cmpF(ChartContext& ctx, const Cond& c, float x, int m) {
    const float v = ctx.chart().valueF(c);
    if (m == 1) return v <= x;
    if (m == 2) return v >= x;
    return std::fabs(x - v) <= 1e-8f;
}
// 0x782B40: int compare: mode 1 value <= x, mode 2 x <= value, else ==.
bool cmpI(ChartContext& ctx, const Cond& c, int x, int m) {
    const int v = ctx.chart().value(c);
    if (m == 1) return v <= x;
    if (m == 2) return x <= v;
    return x == v;
}
// 0/1 boolean test of the PSCCF_* family: value 0 -> !b, 1 -> b, any other ("unhandled condition") -> false.
bool flag01(const Cond& c, bool b) {
    switch (mode(c)) {
    case 0: return !b;
    case 1: return b;
    default: return false;
    }
}

// 0x782D70 (SCCondTest_DeferStatusCheck): the predicate once per chart update, kept with the previous update's value:
// cache = (old & 1) << 1 | now. Value 0 !now, 1 now, 2 falling edge, 3 rising edge; group 1000 / other -> false.
template <class F>
bool edge(ChartContext& ctx, const Cond& c, F pred) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const std::optional<bool> now = pred();
        if (!now) return rt.unported(c);
        cache = {rt.frame(), ((cache.value & 1) << 1) | (*now ? 1 : 0)};
    }
    const int bits = cache.value & 3;
    switch (mode(c)) {
    case 0: return !(bits & 1);
    case 1: return bits & 1;
    case 2: return bits == 2;
    case 3: return bits == 1;
    default: return false;
    }
}

// ---- conditions ----

// 0x1F (0x76F5D0): bone 0 z of the current matrices (instance +0x90 +0x38) minus the previous ones' (+0x94) >= value
// (0x783BD0 mode 1; all uses are variables 378..381).
bool boneZDelta(ChartContext& ctx, const Cond& c) {
    const auto d = ctx.boneRise();
    if (!d) return ctx.chart().unported(c);
    return cmpF(ctx, c, *d, 1);
}

// 0x1F8 (0x762C60, PSCCF_IsFalling; 0x1F6 shares it): instance +0x14 bit 19, which the ground check 0x5DCE20 clears and
// sets when it finds no floor (posting message 0x1CC).
bool isFalling(ChartContext& ctx, const Cond& c) {
    const auto b = ctx.noFloor();
    if (!b) return ctx.chart().unported(c);
    return flag01(c, *b);
}

// 0x20A (0x762FC0): agent byte +0x3EC. Cleared when the chart starts (0x768B60); set by message 0x1CC's handler
// 0x759900 when the ground check lost the floor with velocity z under player data short +0x256 (then player state
// 0x13), and by 0x71FFE0. Ours posts no 0x1CC: it stays 0 [O read; ours never sets it].
bool fastFall(ChartContext& ctx, const Cond& c) { return flag01(c, ctx.chart().field(0x3EC) != 0); }

// 0x20C (0x763910): instance velocity +0x2A0 not ~0 (0x49AFA0) and pointing down.
bool velocityDown(ChartContext& ctx, const Cond& c) {
    const auto v = ctx.velocity();
    if (!v) return ctx.chart().unported(c);
    const bool b = glm::dot(*v, *v) > 1e-12f && glm::normalize(*v).z < 0;
    return flag01(c, b);
}

// 0x217 (0x763490 -> 0x759800): the wall probe 0x7594C0 answered 1 (a wall met that is no wall jump), cached per update.
bool wallJumpCollide(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto r = ctx.wallJumpProbe();
        if (!r) return rt.unported(c);
        cache = {rt.frame(), *r == 1};
    }
    return flag01(c, cache.value != 0);
}

// 0x20B (0x763330, PSCCF_WallJumpAnticipate; group A's id, handed over): cached per update, 0x759260 = false when the
// jump query in effect (+0xA84) has +0x2C flag 0x80, else the wall rays with player data byte +0x820 frames' lookahead.
bool wallJumpAnticipate(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto r = ctx.wallJumpAnticipate(float(rt.charts().data<uint8_t>(0x820)));
        if (!r) return rt.unported(c);
        cache = {rt.frame(), *r ? 1 : 0};
    }
    return flag01(c, cache.value != 0);
}

// 0x21A (0x765FB0 -> 0x782D70, cb 0x765FA0 -> 0x6E66C0): (player +0x504) +0x1FC bit 22, the head-shot latch.
bool headShotLatch(ChartContext& ctx, const Cond& c) { return edge(ctx, c, [&] { return ctx.combatFlag(22); }); }
// 0x403 (0x765530, cb 0x765510): bit 10 of the same flags.
bool combatFlag10(ChartContext& ctx, const Cond& c) { return edge(ctx, c, [&] { return ctx.combatFlag(10); }); }
// 0x417 (0x765620, cb 0x765600): bit 27 (focused fire / adrenaline mode).
bool combatFlag27(ChartContext& ctx, const Cond& c) { return edge(ctx, c, [&] { return ctx.combatFlag(27); }); }
// 0x385 (0x763BA0, cb 0x763B80): (player +0x504) +0x50 != 0. 0x386 (0x763BE0, cb 0x763BC0): == 4.
bool combatMode(ChartContext& ctx, const Cond& c) {
    return edge(ctx, c, [&]() -> std::optional<bool> {
        const auto m = ctx.combatObjectMode();
        if (!m) return std::nullopt;
        return c.id == 0x385 ? *m != 0 : *m == 4;
    });
}
// 0x401 / 0x404 / 0x408 (0x765420 / 0x7654F0 / 0x765470): the upper-body chart's current state's bool property of the
// same id (0x6EEF00 on player +0xF14), false when absent.
bool upperProp(ChartContext& ctx, const Cond& c) { return edge(ctx, c, [&] { return ctx.upperProp(uint16_t(c.id)); }); }

// 0x782810 (via 0x782AD0 / 0x763EB0 for 0x262): the class test of markup flags F (+0x28; 0 without a valid markup).
bool markupClass(ChartContext& ctx, const Cond& c, const ChartContext::MarkupSlot& m) {
    const int32_t f = m ? int32_t(m.flags) : 0;
    const uint32_t u = uint32_t(f);
    switch (mode(c)) {
    case 0: return f == 0 || f < 0;
    case 1: return f != 0 && f >= 0;
    case 600: return u & 0x10;
    case 601: // F & 0x4000000, or F & 0x8000000 and 0x741720(M) (a sub-query of the markup's record, not ported)
        if (u & 0x4000000) return true;
        return (u & 0x8000000) ? ctx.chart().unported(c) : false;
    case 602: { // F & 0x400000 and 0x735000: her z above markup point 0's z - 101.136
        if (!(u & 0x400000)) return false;
        const auto s = ctx.markupSegment(m.markup, 0);
        if (!s) return ctx.chart().unported(c);
        return s->first.z - 101.136f < ctx.position().z;
    }
    case 603: // F & 0x2000000, or F & 0x8000000 and 0x733F60(M) (not ported)
        if (u & 0x2000000) return true;
        return (u & 0x8000000) ? ctx.chart().unported(c) : false;
    case 610: return u & 0x40000000;
    case 611: return u & 0x400000;
    case 612: return u & 0x8000000;
    case 613: return u & 0x80000;
    case 614: return u & 0x1000000;
    case 615: case 626: return u & 0x800000;
    case 616: return u & 0x20200000;
    case 617: return u & 0x100000;
    case 618: return u & 0x8000;
    case 620: return u & 0x4F48FC10;
    case 623: return u & 0x40000;
    case 624: return u & 0x20000;
    case 625: return u & 0x10000;
    case 628: // F & 0x40000000 and the beam test 0x751230 (not ported)
        return (u & 0x40000000) ? ctx.chart().unported(c) : false;
    default: return false;
    }
}
// 0x262 (0x763EB0): the class of the jump-target markup (player +0xA64 / +0xA68). Registry name
// "JumpTargetSameAsAttached" belongs to a neighbour; this is the class test.
bool jumpTargetClass(ChartContext& ctx, const Cond& c) { return markupClass(ctx, c, ctx.jumpTarget()); }

// 0x26D (0x762410 -> 0x7622D0 on M = player +0xA64): bits {0 M has an owning object (M +0xC -> +0x10), 1 player +0xED4
// == 0, 2 +0xED4 != +0xED8, 3 the owner's class flags & 0x100, 4 M valid}; value 690..694 tests bit value - 690.
bool jumpTargetBits(ChartContext& ctx, const Cond& c) {
    const auto m = ctx.jumpTarget();
    switch (mode(c)) {
    case 690: return m && m.live; // [I] ours: markup moved by an object = owned by one
    case 691: return ctx.playerInt(0xED4) == 0;
    case 692: return ctx.playerInt(0xED4) != ctx.playerInt(0xED8);
    case 693: return m ? ctx.chart().unported(c) : false; // owner class flags not in ours
    case 694: return bool(m);
    default: return false;
    }
}

// 0x294 (0x7649A0): group 661 -> player +0xA80 (jump target type) == value; else by value: 660..663 agent +0x340 ==
// 0..3, 673 / 674 / 675 +0xA80 == 2 / 1 / 4, 670 / 671 / 672 / 676 the segment-junction probe 0x737040 (not ported).
bool markupGeometry(ChartContext& ctx, const Cond& c) {
    const int a80 = ctx.jumpTargetType().value_or(ctx.jumpTarget() ? ctx.jumpTarget().type : 0);
    if (c.group == 0x295) return a80 == c.value;
    switch (mode(c)) {
    case 660: case 661: case 662: case 663: return ctx.chart().field(0x340) == mode(c) - 660;
    case 673: return a80 == 2;
    case 674: return a80 == 1;
    case 675: return a80 == 4;
    case 670: case 671: case 672: case 676: return ctx.chart().unported(c);
    default: return false;
    }
}

// 0x2A2 / 0x2A3 (0x76ED20): inclination (degrees from horizontal) of the attached markup's segment round(+0xC98), else
// the jump target's round(+0xA68); no markup -> false. Cached rounded to int (0x814D20); 0x2A2 value <= angle (mode 1),
// 0x2A3 value >= angle (mode 2).
bool segmentPitch(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        auto m = ctx.attachedMarkup();
        if (!m) m = ctx.jumpTarget();
        if (!m) return false;
        const auto s = ctx.markupSegment(m.markup, int(std::lround(m.param)));
        if (!s) return rt.unported(c);
        const glm::vec3 d = s->second - s->first;
        const float h = glm::length(glm::vec2(d)), l = glm::length(d);
        const float deg = l > 0 ? glm::degrees(std::acos(std::clamp(h / l, -1.0f, 1.0f))) : 0.0f;
        cache = {rt.frame(), int(std::lround(deg))};
    }
    return cmpF(ctx, c, float(cache.value), c.id == 0x2A2 ? 1 : c.id == 0x2A3 ? 2 : 0);
}

// 0x2BA (0x76F760): bit `value` of the u16 array at player +0x980 +0x140 = +0xAC0, the jump-target query's result
// flags (cleared with the target by 0x707500): the context's, else the chart-side store.
bool queryFlag(ChartContext& ctx, const Cond& c) {
    const int v = ctx.chart().value(c);
    if (const auto f = ctx.jumpQueryFlags(); f && v >= 0 && v < 32) return (*f >> v) & 1;
    const int word = ctx.playerInt(0xAC0 + (v >> 4) * 2);
    return (word >> (v & 15)) & 1;
}

// 0x2BE (0x72CEC0): grapple component (query 0x175) answering query 0x17C (the rope attached).
bool grappleAttached(ChartContext& ctx, const Cond& c) {
    const auto g = ctx.grapple();
    if (!g) return ctx.chart().unported(c);
    return flag01(c, g->component && g->attached);
}
// 0x2C6 (0x739C80, PlayerSC_Grapple::IsBelowAttachPoint): attached and the attach point z (0x4D8D90 +0x88) above her
// z ((instance +0x90) +0x78, bone 1).
bool belowAttach(ChartContext& ctx, const Cond& c) {
    const auto g = ctx.grapple();
    if (!g) return ctx.chart().unported(c);
    return flag01(c, g->attached && g->anchor.z > ctx.rootBone().z);
}
// 0x2C7 (0x72D2E0): rope length (component +0xCC) under its limit (+0xC8).
bool ropeShort(ChartContext& ctx, const Cond& c) {
    const auto g = ctx.grapple();
    if (!g) return ctx.chart().unported(c);
    if (!g->component) return flag01(c, false);
    if (!g->attached) return ctx.chart().unported(c);
    return flag01(c, g->rope < g->ropeMax);
}

// 0x322 / 0x323 (0x764340): water depth class: no water block 800; depth >= data +0xB2 (948) 805, >= +0xB0 (391) 804,
// >= +0xAE (391) 803, >= +0xAC (154) 802, > 0 801, else 800. 0x321 equal, 0x322 class <= value (800 counts as 807),
// 0x323 value <= class.
int depthClass(const Charts& d, float depth) {
    if (depth >= float(d.data<int16_t>(0xB2))) return 805;
    if (depth >= float(d.data<int16_t>(0xB0))) return 804;
    if (depth >= float(d.data<int16_t>(0xAE))) return 803;
    if (depth >= float(d.data<int16_t>(0xAC))) return 802;
    return depth > 0 ? 801 : 800;
}
bool waterDepth(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(0x321); // one node for the three ids (shared fn)
    if (cache.frame != rt.frame()) {
        const auto w = ctx.water();
        if (!w) return rt.unported(c);
        cache = {rt.frame(), w->inWater ? depthClass(rt.charts(), w->depth) : 800};
    }
    int k = cache.value;
    if (c.id == 0x322) return cmpI(ctx, c, k == 800 ? 807 : k, 2);
    if (c.id == 0x323) return cmpI(ctx, c, k, 1);
    return cmpI(ctx, c, k, 0);
}
// 0x325 (0x764550): swim depth class: player +0x918 & 0x400 -> 800; 805 needs -player +0x910 >= 948 as well as the
// depth; else as 0x764340. Class <= value (mode 2). [I] ours has no +0x910 head height: 805 is not decided.
bool swimDepth(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const auto w = ctx.water();
    if (!w) return rt.unported(c);
    int k = w->inWater ? depthClass(rt.charts(), w->depth) : 800;
    if (k == 805) return rt.unported(c);
    return cmpI(ctx, c, k, 2);
}

// 0x5DD (0x7301C0, cb 0x730120): player +0x858 (the carried portable) != 0.
bool carrying(ChartContext& ctx, const Cond& c) { return edge(ctx, c, [&] { return ctx.carrying(); }); }
// 0x5E0 (0x730250, cb 0x730140): bool property 1501 of the current state (0x6EEF00 mode 2), false when absent.
bool prop1501(ChartContext& ctx, const Cond& c) {
    return edge(ctx, c, [&]() -> std::optional<bool> { return ctx.chart().propBool(1501).value_or(false); });
}
// 0x7D1 (0x765290, cb 0x765270 -> 0x786E50): bool property 1002 (melee allowed) of the current state.
bool meleeAllowed(ChartContext& ctx, const Cond& c) {
    return edge(ctx, c, [&]() -> std::optional<bool> { return ctx.chart().propBool(1002).value_or(false); });
}

// 0x8FC (0x766340): posts 0x156 to the carried portable if any; true.
bool postCarried(ChartContext& ctx, const Cond&) {
    if (ctx.carrying().value_or(false)) ctx.effect(JEffect::CarriedPost, 0x156);
    return true;
}
// 0x8FE (0x7663D0): (player +0x504) +0x1FC |= 0x10000000, 0xFAE8D8 = 1, HUD flag 1 (0x77DA80); true.
bool setWeaponFlag(ChartContext& ctx, const Cond&) {
    ctx.effect(JEffect::WeaponFlag, 28);
    return true;
}
// 0x900 (0x766400): dispatches the current clip's anim events 0x38 (0x718780); true.
bool fireEvent38(ChartContext& ctx, const Cond&) {
    ctx.effect(JEffect::AnimEvent, 0x38);
    return true;
}

// ---- actions ----

// Process functions step by gameTracker_timeMult (ctx.timeMult, 30 Hz frames), as tru.exe's.

// 504 (enter 0x76BD30, update 0x76BF00, exit 0x75BD00): take-off. Chart-side: agent +0x98 air time from 0, collision
// mask 0xFADA74 = 0x49; jump type +0xA84 == 0x12 forces the end counter +0x24 = 1 (not representable: recorded).
// [U] the take-off heading (0x76B210 / 0x76B380) needs the anim root-rotation flag: not ported.
void takeoffEnter(ChartContext& ctx, uint16_t) {
    ctx.agentFloat(0x98) = 0;
    ctx.effect(JEffect::CollisionMask, 0x49);
}
void takeoffUpdate(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x98) += ctx.timeMult; }

// 510 (enter / reenter 0x7522A0, update 0x752410, exit 0x72FB90): airborne. Enter: detach the attached markup
// (0x6FDBE0: +0xEA0 keeps it), agent +0x3E8 = 0, mask 0xC9 (0x81 rising); falling with a jump target of flags
// 0x20800000 clears it (0x707500). Update: +0x3E8 += timeMult, mask &~0x48 rising else |= 0x48; the target cleared when
// falling onto flags 0x20830000 & 0x20800000.
void airEnter(ChartContext& ctx, uint16_t) {
    if (const auto a = ctx.attachedMarkup()) {
        ctx.playerInt(0xEA0) = a.markup + 1; // non-zero = kept (ours: index + 1)
        ctx.effect(JEffect::DetachMarkup, float(a.markup));
    } else {
        ctx.playerInt(0xEA0) = 0;
    }
    ctx.agentFloat(0x3E8) = 0;
    const auto v = ctx.velocity();
    const bool rising = v && v->z > 0;
    ctx.effect(JEffect::CollisionMask, rising ? 0x81 : 0xC9);
    if (const auto t = ctx.jumpTarget(); t && !rising && (t.flags & 0x20800000)) ctx.effect(JEffect::ClearJumpTarget);
}
void airUpdate(ChartContext& ctx, uint16_t) {
    ctx.agentFloat(0x3E8) += ctx.timeMult;
    const auto v = ctx.velocity();
    const bool rising = v && v->z > 0;
    ctx.effect(JEffect::CollisionMask, rising ? -0x48 : 0x48);
    if (const auto t = ctx.jumpTarget(); t && !rising && (t.flags & 0x20830000 & 0x20800000)) ctx.effect(JEffect::ClearJumpTarget);
}

// 511 (enter 0x73ECE0, update 0x750040): free-jump steering. Enter keeps the stick heading (agent +0x448 / +0x424 /
// +0x420), the take-off velocity (+0x430..), the heading (+0x44C), steer time +0x440 = 0, +0x444 = -1, +0x450 = the
// jump target. Update (only without a jump target) advances +0x440; the brake itself is Player::airBrake's.
void steerEnter(ChartContext& ctx, uint16_t) {
    const glm::vec2 st = ctx.stick();
    const float h = glm::dot(st, st) > 0 ? std::atan2(st.y, st.x) : ctx.heading();
    ctx.agentFloat(0x448) = h, ctx.agentFloat(0x424) = h, ctx.agentFloat(0x420) = h;
    const glm::vec3 v = ctx.velocity().value_or(glm::vec3(0));
    ctx.agentFloat(0x430) = v.x, ctx.agentFloat(0x434) = v.y, ctx.agentFloat(0x438) = v.z;
    float y = ctx.heading();
    while (y > kPi) y -= 2 * kPi;
    while (y < -kPi) y += 2 * kPi;
    ctx.agentFloat(0x44C) = y;
    ctx.agentFloat(0x440) = 0;
    ctx.chart().field(0x444) = -1;
    ctx.chart().field(0x450) = ctx.jumpTarget().markup + 1;
}
void steerUpdate(ChartContext& ctx, uint16_t) {
    if (ctx.jumpTarget()) return;
    ctx.agentFloat(0x440) += ctx.timeMult;
}

// 572 (enter 0x76DCD0, exit 0x75E7F0): pushes a yaw limit (prop 993 degrees, default player data short +0x74C) onto the
// look constraint stack for the frame chosen by prop 481; exit pops it.
void yawLimitEnter(ChartContext& ctx, uint16_t) {
    const float lim = ctx.chart().propFloat(993).value_or(float(ctx.chart().charts().data<int16_t>(0x74C)));
    ctx.effect(JEffect::YawLimit, lim);
}
void yawLimitExit(ChartContext& ctx, uint16_t) { ctx.effect(JEffect::YawLimit, -1); }

// 802 (enter 0x75E2F0, reenter 0x75E300, exit 0x75E310, update 0x75E320): the air meter player +0x920. Enter: full
// (player data float +0xF8). Update: prop 802 true drains timeMult x 1 a frame, every -data short +0xFE (20) costs data
// short +0xFC % health (recorded); else it refills max x dt / data short +0xFA.
void airMeterEnter(ChartContext& ctx, uint16_t) {
    ctx.playerFloat(0x920) = ctx.chart().charts().data<float>(0xF8);
}
void airMeterReenter(ChartContext& ctx, uint16_t) {
    if (ctx.playerFloat(0x920) < 0) ctx.playerFloat(0x920) = 0;
}
void airMeterUpdate(ChartContext& ctx, uint16_t) {
    const Charts& d = ctx.chart().charts();
    const float dt = ctx.timeMult;
    const float max = d.data<float>(0xF8);
    float& air = ctx.playerFloat(0x920);
    if (ctx.chart().propBool(802).value_or(false)) {
        air -= dt;
        if (air <= -float(d.data<int16_t>(0xFE))) {
            air += float(d.data<int16_t>(0xFE));
            ctx.effect(JEffect::AirMeter, float(d.data<int16_t>(0xFC)));
        }
    } else if (const int16_t fill = d.data<int16_t>(0xFA); fill > 0) {
        air = std::min(max, air + max * dt / float(fill));
    }
}

// 917 (enter / reenter 0x75E350, exit 0x75E390): chain counter agent +0x4EC (wall jumps in a row) += 1 when prop 917.
void chainEnter(ChartContext& ctx, uint16_t) {
    if (ctx.chart().propBool(917).value_or(false)) ++ctx.chart().field(0x4EC);
}
void chainExit(ChartContext& ctx, uint16_t) { ctx.chart().field(0x4EC) = 0; }

// 1002 (enter 0x72FA00, reenter 0x72FA70, exit 0x72FAE0): acrobatic combo. Prop 1010 (type 0xE) not 0 / 1 appends to
// agent +0x46C[agent +0x484] (max 6) and 0x72F920 matches the buffer against player data +0xFE0[+0xFDC] (0x14 bytes:
// float mult, int enabled, int length, -> s16 move ids, stride 0x18): a match raises the meter to mult x max.
void comboMatch(ChartContext& ctx) {
    ChartRuntime& rt = ctx.chart();
    const Charts& d = rt.charts();
    const uint32_t n = d.data<uint32_t>(0xFDC);
    const auto table = d.pointer(d.dataOffset + 0xFE0);
    if (!table) return;
    const int len = rt.field(0x484);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = *table - d.dataOffset + i * 0x14;
        if (d.data<int32_t>(e + 4) == 0 || d.data<int32_t>(e + 8) != len) continue;
        const auto seq = d.pointer(d.dataOffset + e + 0xC);
        bool same = true;
        for (int k = 0; k < len && same; ++k)
            same = seq && d.data<int16_t>(*seq - d.dataOffset + uint32_t(k) * 0x18) == rt.field(0x46C + k * 4);
        if (same) ctx.effect(JEffect::ComboMeter, d.data<float>(e));
    }
}
void comboAppend(ChartContext& ctx, int v) {
    ChartRuntime& rt = ctx.chart();
    if (rt.field(0x484) < 6) rt.field(0x46C + rt.field(0x484) * 4) = v, ++rt.field(0x484);
    comboMatch(ctx);
}
void comboEnter(ChartContext& ctx, uint16_t) {
    ChartRuntime& rt = ctx.chart();
    rt.field(0x484) = 0;
    const Property* p = rt.property(0x3F2);
    if (p && p->type == 0xE && p->raw != 0 && p->raw != 1) comboAppend(ctx, int(p->raw));
}
void comboReenter(ChartContext& ctx, uint16_t) {
    ChartRuntime& rt = ctx.chart();
    const Property* p = rt.property(0x3F2);
    if (!p || p->type != 0xE || p->raw == 0) rt.field(0x484) = 0;
    else if (p->raw != 1) comboAppend(ctx, int(p->raw));
}
void comboExit(ChartContext& ctx, uint16_t) { ctx.chart().field(0x484) = 0; }

// 1011 (enter / reenter 0x75E100): prop 1023 selects player +0x588 = data byte +0x27C (8) or +0x58C = byte +0x294 /
// +0x295 / +0x296 (9 / 10 / 11).
void tuningValue(ChartContext& ctx, uint16_t) {
    const Property* p = ctx.chart().property(1023);
    if (!p) return;
    const Charts& d = ctx.chart().charts();
    switch (p->raw & 0xFFFF) {
    case 8: ctx.playerFloat(0x588) = float(d.data<uint8_t>(0x27C)); break;
    case 9: ctx.playerFloat(0x58C) = float(d.data<uint8_t>(0x294)); break;
    case 10: ctx.playerFloat(0x58C) = float(d.data<uint8_t>(0x295)); break;
    case 11: ctx.playerFloat(0x58C) = float(d.data<uint8_t>(0x296)); break;
    default: break;
    }
}

// 1053 (enter 0x75E750, exit 0x75E7D0): births the helper object data +0x70C over her (0x70CC50); exit releases it
// (0x70CDE0) and clears player +0x74C.
void wrestleEnter(ChartContext& ctx, uint16_t) { ctx.effect(JEffect::WrestleHelper, float(ctx.chart().charts().data<int16_t>(0x70C))); }
void wrestleExit(ChartContext& ctx, uint16_t) {
    ctx.effect(JEffect::WrestleHelper, -1);
    ctx.playerInt(0x74C) = 0;
}

// 1304 (enter 0x75D230, update 0x75D290, exit 0x75D2F0): landing damage. agent +0x3EE = applied; prop 911 (seconds,
// >= 0.0001) defers it until the state time passes it; exit applies it if still pending. The damage (0x75D080) is
// recorded with the fall height player +0x6DC + +0x6E0 - z.
void landDamage(ChartContext& ctx) {
    ctx.effect(JEffect::LandDamage, ctx.fallDistance() + ctx.playerFloat(0x6E0));
    ctx.chart().field(0x3EE) = 1;
}
void landEnter(ChartContext& ctx, uint16_t) {
    ctx.chart().field(0x3EE) = 0;
    if (const auto d = ctx.chart().propFloat(911); d && *d >= 0.0001f) return;
    landDamage(ctx);
}
void landUpdate(ChartContext& ctx, uint16_t) {
    const auto d = ctx.chart().propFloat(911);
    if (d && *d < ctx.chart().stateTime() && !ctx.chart().field(0x3EE)) landDamage(ctx);
}
void landExit(ChartContext& ctx, uint16_t) {
    if (!ctx.chart().field(0x3EE)) landDamage(ctx);
    ctx.chart().field(0x3EE) = 0;
    ctx.playerInt(0x70) = 0;
}

// 1800 (enter 0x75F910, update 0x75F960, exit 0x75F9B0): state message prop 2100 (queued when prop 2102 == 1). Prop
// 2101: 0 / 1 send on entry, 1 also every frame while player +0x11C8 == 0, 2 on exit.
void stateMessage(ChartContext& ctx) {
    const int msg = ctx.chart().propInt(2100).value_or(0);
    ctx.effect(JEffect::StateMessage, float(ctx.chart().propInt(2102).value_or(0) == 1 ? -msg : msg));
}
void messageEnter(ChartContext& ctx, uint16_t) {
    const int m = ctx.chart().propInt(2101).value_or(0);
    if (m == 0 || m == 1) stateMessage(ctx);
}
void messageUpdate(ChartContext& ctx, uint16_t) {
    if (ctx.chart().propInt(2101).value_or(0) == 1 && ctx.playerInt(0x11C8) == 0) stateMessage(ctx);
}
void messageExit(ChartContext& ctx, uint16_t) {
    if (ctx.chart().propInt(2101).value_or(0) == 2) stateMessage(ctx);
}

// 230 PreAnimEntry / PreAnimReentry 0x772CA0 (PSCPF_MarkUpAttach_SetAnimTarget; group A's id, handed over). The target
// class: prop 0x1A4 (type 0x23, "alt") else prop 0x19A. With bool prop 0x199 and no jump target (player +0xA64) it
// promotes the candidate slot +0x3A0, or when +0x3E0 is filled too the one whose direction (+0x20) lies more along her
// heading (0x4320A0 dot of 0x49F5D0(yaw)), into the target (+0xA64 markup, +0xA68 param, +0x9F0 point; +0xA6C =
// 0x6EC440(markup), +0xA74 = 2.0). Chart-side: stored in playerInt(0xA64) (markup + 1) / playerFloat(0xA68); the live
// target stays the controller's in shadow mode. Prop 400 / 401 (0x75F2D0) and the per-class anim-target functions
// (0x74FDF0, 0x74ED30 ...) belong to the attach families: recorded with the class, not run. [O] prop 0x199 is true only
// in the hang attach states S542..S544, so no free flight promotes a candidate.
void markupAttachTarget(ChartContext& ctx, uint16_t) {
    ChartRuntime& rt = ctx.chart();
    int cls = 0;
    if (const Property* p = rt.property(0x1A4); p && p->type == 0x23) cls = int(p->raw);
    else if (const Property* q = rt.property(0x19A); q && q->type == 0x23) cls = int(q->raw);
    const bool promote = rt.propBool(0x199).value_or(false);
    if (promote && !ctx.jumpTarget() && ctx.playerInt(0xA64) == 0) {
        if (const auto cand = ctx.jumpCandidates(); cand && cand->a) {
            const ChartContext::MarkupSlot* pick = &cand->a;
            if (cand->b) {
                const glm::vec2 h(std::cos(ctx.heading()), std::sin(ctx.heading()));
                if (glm::dot(h, glm::vec2(cand->a.normal)) < glm::dot(h, glm::vec2(cand->b.normal))) pick = &cand->b;
            }
            ctx.playerInt(0xA64) = pick->markup + 1;
            ctx.playerFloat(0xA68) = pick->param;
            ctx.playerFloat(0xA74) = 2.0f;
            ctx.effect(JEffect::TargetPromote, float(pick->markup));
        }
    }
    if (cls) ctx.effect(JEffect::AttachTarget, float(cls)); // the class handler that would run (not ported here)
}

// 2001 (0x75EF70: camera vfunc +0xB0 (0)), 2002 (0x75EF90: +0xB0 (1)), 2005 (enter and exit 0x75EF50: +0xB4); 2001 /
// 2002 exit 0x75EF50.
void cameraHold(ChartContext& ctx, uint16_t) { ctx.effect(JEffect::CameraPush, 0); }
void cameraMode1(ChartContext& ctx, uint16_t) { ctx.effect(JEffect::CameraPush, 1); }
void cameraRelease(ChartContext& ctx, uint16_t) { ctx.effect(JEffect::CameraPop); }

// 2101 (enter 0x75EED0): plays resource prop 1112 (type 5) through pool 0xE80020 (0x5EE7F0).
void playResource(ChartContext& ctx, uint16_t) {
    const Property* p = ctx.chart().property(1112);
    if (p && p->type == 5) ctx.effect(JEffect::PlayResource, float(p->raw));
}

} // namespace

void registerJump(Registry& player, Registry&) {
    const char* fam = "jump";
    player.condition(0x1F, boneZDelta, "BoneZDelta", fam);
    player.condition(0x1F8, isFalling, "IsFalling", fam);
    player.condition(0x20A, fastFall, "IsFalling(+0x3EC)", fam);
    player.condition(0x20C, velocityDown, "VelocityDownward", fam);
    player.condition(0x20B, wallJumpAnticipate, "WallJumpAnticipate", fam);
    player.condition(0x217, wallJumpCollide, "WallJumpCollide", fam);
    player.condition(0x21A, headShotLatch, "CombatFlag22", fam);
    player.condition(0x262, jumpTargetClass, "Markup_JumpTargetClass", fam);
    player.condition(0x26D, jumpTargetBits, "Markup_JumpTargetBits", fam);
    player.condition(0x294, markupGeometry, "MarkupGeometry", fam);
    player.condition(0x2A2, segmentPitch, "MarkupPitchAtLeast", fam);
    player.condition(0x2A3, segmentPitch, "MarkupPitchAtMost", fam);
    player.condition(0x2BA, queryFlag, "MarkupQueryFlag", fam);
    player.condition(0x2BE, grappleAttached, "GrappleAttached", fam);
    player.condition(0x2C6, belowAttach, "Grapple_IsBelowAttachPoint", fam);
    player.condition(0x2C7, ropeShort, "GrappleRopeLength", fam);
    player.condition(0x322, waterDepth, "WaterDepthClass_AtMost", fam);
    player.condition(0x323, waterDepth, "WaterDepthClass_AtLeast", fam);
    player.condition(0x325, swimDepth, "SwimDepthClass_AtMost", fam);
    player.condition(0x385, combatMode, "InCombatMode", fam);
    player.condition(0x386, combatMode, "CombatMode4", fam);
    for (const int16_t id : {0x401, 0x404, 0x408}) player.condition(id, upperProp, "UpperStateProp", fam);
    player.condition(0x403, combatFlag10, "CombatFlag10", fam);
    player.condition(0x417, combatFlag27, "CombatFlag27", fam);
    player.condition(0x5DD, carrying, "HasCarriedObject", fam);
    player.condition(0x5E0, prop1501, "Prop1501", fam);
    player.condition(0x7D1, meleeAllowed, "MeleeAllowed", fam);
    player.condition(0x8FC, postCarried, "Do_PostPortable156", fam);
    player.condition(0x8FE, setWeaponFlag, "SetWeaponFlag", fam);
    player.condition(0x900, fireEvent38, "Do_FireAnimEvent38", fam);

    player.action(230, {nullptr, nullptr, markupAttachTarget, markupAttachTarget, nullptr, nullptr}, "MarkUpAttach_SetAnimTarget", fam);
    player.action(504, {takeoffEnter, nullptr, nullptr, nullptr, nullptr, takeoffUpdate}, "JumpTakeoffHeading", fam);
    player.action(510, {airEnter, airEnter, nullptr, nullptr, nullptr, airUpdate}, "Airborne", fam);
    player.action(511, {steerEnter, nullptr, nullptr, nullptr, nullptr, steerUpdate}, "AirSteer", fam);
    player.action(572, {yawLimitEnter, nullptr, nullptr, nullptr, yawLimitExit, nullptr}, "YawConstraintFrame", fam);
    player.action(802, {airMeterEnter, airMeterReenter, nullptr, nullptr, airMeterReenter, airMeterUpdate}, "AirMeter", fam);
    player.action(917, {chainEnter, chainEnter, nullptr, nullptr, chainExit, nullptr}, "CountChain917", fam);
    player.action(1002, {comboEnter, comboReenter, nullptr, nullptr, comboExit, nullptr}, "AcrobaticCombo", fam);
    player.action(1011, {tuningValue, tuningValue, nullptr, nullptr, nullptr, nullptr}, "SetTuningValue1023", fam);
    player.action(1053, {wrestleEnter, nullptr, nullptr, nullptr, wrestleExit, nullptr}, "SpawnWrestleHelper", fam);
    player.action(1304, {landEnter, nullptr, nullptr, nullptr, landExit, landUpdate}, "ApplyLandingFallDamage", fam);
    player.action(1800, {messageEnter, nullptr, nullptr, nullptr, messageExit, messageUpdate}, "StateMessage", fam);
    player.action(2001, {cameraHold, nullptr, nullptr, nullptr, cameraRelease, nullptr}, "CameraSuspendResume", fam);
    player.action(2002, {cameraMode1, nullptr, nullptr, nullptr, cameraRelease, nullptr}, "CameraMode1", fam);
    player.action(2005, {cameraRelease, nullptr, nullptr, nullptr, cameraRelease, nullptr}, "CameraModeRestore", fam);
    player.action(2101, {playResource, nullptr, nullptr, nullptr, nullptr, nullptr}, "PlayEffectResource", fam);
}

} // namespace ou::game::chart
