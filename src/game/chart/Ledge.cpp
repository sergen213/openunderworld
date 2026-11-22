// Group B chart conditions and actions: ledge grab, hang, shimmy, corners, pull-up / clearance classes, drop to
// hang, hurdles, saving grab, and the beam controller actions the catalog gives this group
// (docs/re/PLAYER_CHART.md "Conditions" / "Actions"; ownership openunderworld-work/chart/*_owner.json by_group.B).
// Each condition returns the raw truth (the runtime applies the negation); the catalogued exceptions that ignore or
// double the negation return `forced(c, v)`, which makes the final truth v whatever the negation. Actions record their
// effects in the chart-side stores of ChartContext (playerInt / playerFloat / agentFloat / animBlend / ...), never on
// the live Player. Evidence: [O] read in tru.exe, [I] inferred, [U] open.
//
// Already registered by Generic.cpp (owned by B in the catalog, same functions): 0x14 (0x783B70), 0x67 / 0x6E
// (0x760A70 / 0x760B30 -> 0x7607C0), 0x9E (0x760C40).
#include "game/Chart.hpp"
#include "game/Game.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <cstring>

namespace ou::game::chart {

namespace {

constexpr float kPi = glm::pi<float>(), kTwoPi = glm::two_pi<float>(), kHalfPi = glm::half_pi<float>();

// tru.exe 0x781FF0: the mode a condition reads from its value; group 1000 makes it 1000 (no mode case matches).
int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; }
// The final truth is v whatever the negation (the functions that do not apply it, or apply it twice).
bool forced(const Cond& c, bool v) { return v != c.negate(); }
// 0x782B40: the condition's value (a variable's for group 1000) against x: mode 1 value <= x, 2 value >= x, else ==.
bool cmpInt(ChartContext& ctx, const Cond& c, int x, int m) {
    const int v = ctx.chart().value(c);
    return m == 1 ? v <= x : m == 2 ? v >= x : v == x;
}
// 0x783BD0: the same on floats (0x782060), mode 0 within 1e-8.
bool cmpFloat(ChartContext& ctx, const Cond& c, float x, int m) {
    const float v = ctx.chart().valueF(c);
    return m == 1 ? v <= x : m == 2 ? v >= x : std::abs(x - v) <= 1e-8f;
}
// 0x814D20: x87 round to nearest (ties to even, the FPU default).
int roundI(float x) { return int(std::nearbyint(x)); }
// 0x431D70: an angle wrapped to (-pi, pi].
float wrapPi(float a) {
    a = std::remainder(a, kTwoPi);
    return a <= -kPi ? a + kTwoPi : a;
}
float yawOf(glm::vec2 d) { return std::atan2(d.y, d.x); } // ours: forward = (cos, sin)
int16_t pd(ChartContext& ctx, uint32_t off) { return ctx.chart().charts().data<int16_t>(off); }
float propF(ChartContext& ctx, uint16_t id, float def = 0) { return ctx.chart().propFloat(id).value_or(def); }
bool propB(ChartContext& ctx, uint16_t id, bool def = false) { return ctx.chart().propBool(id).value_or(def); }
int propI(ChartContext& ctx, uint16_t id, int def = 0) { return ctx.chart().propInt(id).value_or(def); }
// A slot of an action nobody could port: counted like a missing action.
void unportedSlot(ChartContext& ctx, uint16_t id) { ++ctx.chart().stats.unportedActions[id]; }

// 0x782D70 (SCCondTest_DeferStatusCheck): a bool recomputed once per update (+0x50C) into the condition cache as
// (previous & 1) << 1 | now; the first evaluation takes the previous as now. Mode 0 !now, 1 now, 2 just turned off,
// 3 just turned on; anything else is a debug print and false.
bool edge(ChartContext& ctx, const Cond& c, bool now) {
    auto& k = ctx.chart().cache(c.id);
    if (k.frame != ctx.chart().frame()) {
        k.value = k.frame == ~0u ? (now ? 3 : 0) : ((k.value & 1) << 1) | int(now);
        k.frame = ctx.chart().frame();
    }
    const int bits = k.value & 3;
    switch (mode(c)) {
    case 0: return !(bits & 1);
    case 1: return bits & 1;
    case 2: return bits == 2;
    case 3: return bits == 1;
    default: return forced(c, false);
    }
}

// 0x785FB0: the MoveDir sector of an angle. Bounds = state float properties 501..504 (degrees, wrapped), the last
// sector (agent +0x70, 0 = 1) widened by property 507 on both sides; mirrored with property 207 negates the angle.
// Sectors: 1 B4..B1 (front), 4 B1..B2 (+angle side), 8 B3..B4 (-angle side), else 2 (back); stored to +0x70.
// [I] property 508 (bounds from the gear slot-0 instance's message 0x35) needs a drawn gear item: not modelled.
int moveDirSector(ChartContext& ctx, float a) {
    const auto bound = [&](uint16_t id) { return wrapPi(glm::radians(propF(ctx, id))); };
    float b1 = bound(501), b2 = bound(502), b3 = bound(503), b4 = bound(504);
    const float h = glm::radians(propF(ctx, 507));
    int& last = ctx.chart().field(0x70);
    const int z0 = last ? last : 1;
    if (z0 & 1) b4 -= h, b1 += h;
    if (z0 & 2) b2 -= h, b3 += h;
    if (z0 & 4) b1 -= h, b2 += h;
    if (z0 & 8) b3 -= h, b4 += h;
    if (ctx.chart().mirrored() && propB(ctx, 207)) a = -a;
    const int z = b4 <= a && a <= b1 ? 1 : b1 <= a && a <= b2 ? 4 : b3 <= a && a <= b4 ? 8 : 2;
    last = z;
    return z;
}
// MoveDir values to sectors (0x19A.. aliases): 410 / 414 front, 411 / 415 back, 412 / 416 +side, 413 / 417 -side.
int sectorOf(int v) {
    switch (v) {
    case 410: case 414: return 1;
    case 411: case 415: return 2;
    case 412: case 416: return 4;
    case 413: case 417: return 8;
    default: return 0;
    }
}
// 0x7868F0 (PSCCF_MoveDirLeftOrRight), cached per update: sector 4 when the angle is >= 0 (widened below by property
// 507 while 4 was last), else 8 when <= 0 (widened above while 8 was last). tru.exe writes the other bounds as 180 /
// -180 against the angle in radians, so they never bind [O quirk]. Mirror as 0x785FB0; stored to +0x70.
int leftRightSector(ChartContext& ctx, const Cond& c, float a) {
    auto& k = ctx.chart().cache(c.id);
    if (k.frame == ctx.chart().frame()) return k.value;
    const float h = glm::radians(propF(ctx, 507));
    int& last = ctx.chart().field(0x70);
    if (ctx.chart().mirrored() && propB(ctx, 207)) a = -a;
    const int z = a >= ((last & 4) ? -h : 0.0f) ? 4 : a <= ((last & 8) ? h : 0.0f) ? 8 : 0;
    last = z;
    k = {ctx.chart().frame(), z};
    return z;
}

// 0x5C3AA0 on a slot: the segment its param lies on (rounded down, as the callers floor / round it as noted).
std::optional<std::pair<glm::vec3, glm::vec3>> slotSegment(ChartContext& ctx, const ChartContext::MarkupSlot& s, int seg) {
    return s ? ctx.markupSegment(s.markup, seg) : std::nullopt;
}

// 0x782810: markup class test on a slot's flags F (+0x28; 0 without a valid markup). 0 none, 1 any, 600.. classes.
// 601 / 603 / 628 also take per-segment attribute tables (0x741720 / 0x733F60 via markup data +0x180 / +0x184) or the
// wall-hug shelf (0x751230) for the flag-0x8000000 markups and ledges.
bool markupClass(ChartContext& ctx, const Cond& c, const ChartContext::MarkupSlot& s) {
    const auto f = int32_t(s.flags);
    const uint32_t u = s.flags;
    switch (mode(c)) {
    case 0: return f == 0 || f < 0;
    case 1: return f != 0 && f >= 0;
    case 600: return u & 0x10;
    case 601: if (u & 0x4000000) return true; return (u & 0x8000000) ? ctx.chart().unported(c) : false;
    case 602: // 0x735000: her root above the markup point minus 101.136
        return (u & 0x400000) && ctx.position().z > s.point.z - 101.136f;
    case 603: if (u & 0x2000000) return true; return (u & 0x8000000) ? ctx.chart().unported(c) : false;
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
    case 628: {
        if (!(u & 0x40000000)) return false;
        const auto shelf = ctx.ledgeShelf(s.markup, s.param);
        return shelf ? *shelf : ctx.chart().unported(c);
    }
    default: return forced(c, false);
    }
}

// ---- conditions ----

// 0x764BE0 (0x15): round(agent float +0x374) >= the value (0x782B40 mode 1). [U] its writer (beam / hurdle).
bool agentFloat374(ChartContext& ctx, const Cond& c) { return cmpInt(ctx, c, roundI(ctx.agentFloat(0x374)), 1); }

// 0x76F630 (0x20): the segment from her root bone (x, y; z of the instance) straight up `value` cm (3.1605 units
// each; group 20 a literal, 1000 a variable, else no length) is clear (0x493330, ray flags 0x2C19E7). Attached to a
// markup, the start moves a third of the way to the grab point +0xCA0 horizontally.
bool verticalClearance(ChartContext& ctx, const Cond& c) {
    if (!ctx.hasCollision()) return ctx.chart().unported(c);
    glm::vec3 p0 = ctx.rootBone();
    p0.z = ctx.position().z;
    if (const auto a = ctx.attachedMarkup()) p0 += glm::vec3((glm::vec2(a.point) - glm::vec2(p0)) / 3.0f, 0);
    float cm = 0;
    if (c.fromVariable()) cm = ctx.chart().valueF(c);
    else if (c.group == 20) cm = float(c.value);
    if (cm == 0) return true;
    return !ctx.playerRay(p0, p0 + glm::vec3(0, 0, 3.1605f * cm));
}

// 0x7840B0: the played clip's 2D blend parameters (anim section +0x8C / +0x90). 0xD0 |+0x8C| >= value, 0xD1 |+0x90|
// >= value (0x783BD0 mode 1).
bool blendAbs(ChartContext& ctx, const Cond& c) {
    return cmpFloat(ctx, c, std::abs(c.id == 0xD0 ? ctx.animBlend.x : ctx.animBlend.y), 1);
}

// 0x783950: the clip's time t = round(time / 100) (30 Hz frames; ours the chart's clip clock). 0xD6 t <= value,
// 0xD7 round(length) - t <= value (frames left), 0xD8 floor(time / length x 100) >= value; the percent is computed
// only on a fresh cache, so a second evaluation in the same update reads 0 [O quirk].
bool animTime(ChartContext& ctx, const Cond& c) {
    const ChartRuntime& rt = ctx.chart();
    const float len = rt.animLengthFrames();
    const int t = roundI(rt.animFrames());
    switch (c.id) {
    case 0xD6: return cmpInt(ctx, c, t, 2);
    case 0xD7:
        if (len <= 0) return ctx.chart().unported(c);
        return cmpInt(ctx, c, roundI(len) - t, 2);
    default: {
        auto& k = ctx.chart().cache(c.id);
        int pct = 0;
        if (k.frame != rt.frame()) {
            pct = len > 0 ? int(std::floor(rt.animFrames() / len * 100.0f)) : 0;
            k = {rt.frame(), pct};
        }
        return cmpInt(ctx, c, pct, 1);
    }
    }
}

// The input controller's direction class (0xFADFB0 +0xB0 +0x24, set by 0x6F89A0): the pad (x right, y down) against
// a frame: u along its forward axis (+0x100), w along its side axis (+0x110); class 3 / 4 (w < 0 / >= 0) when
// |u| <= |w|, else 1 / 2 (u >= 0 / < 0) [O: decomp, the tie going to the side]; 0 under the dead zone +0xD8.
// On a markup the record runs mode 2 (+0xB0 = 2, +0xB1 = +0xB2 = 1, +0xDC = 1, +0xB4 = 30, +0xD8 = 0.05) [O-runtime:
// Wine stick dumps, chapel ch_hang_left_hold and bedroom hang_shimmy_right / hang_grab], and 0x6F8050 builds the
// frame from two references in her frame (+0x120 rows: her right, her forward, up): forward +0x70 = (0, 0.707,
// 0.707), side +0x80 = (1, 0, 0). With the camera looking up (+0x196: +0xDC and the look row's z cz.z > 0) the forward's y is
// scaled by 1 - cz.z, and the side's x by 1 - cz.z sqrt|l| and its z set to 2 l cz.z, l = (R0.s, -R1.s, R2.s) . cz;
// both normalized. Each goes to camera space (0x6F7A10: the 0x5E7870 matrix, the inverse of g_camera +0x10; x =
// camera right, y = camera down: the screen plane), z (depth) dropped, normalized. When the two axes come within +0xB4
// degrees (acos |f . s|), 0x6F89A0 rebuilds
// from +0xB8 = (0, 1, 0) / +0xC8 = (1, 0, 0) (+0x195): the side's x scaled by 1 - |l|, its z = sign(l) cz.z [O: decomp,
// the formula checked against the dumps' +0x100 / +0x110 with their camera matrices]. Ours runs it on the live
// camera (ChartContext::cameraAxes: right, down, look as tru.exe's rows). Off a markup: the camera control
// yaw [I]; dead zone 0.10 (the move-speed zone 1 threshold); no re-classing hysteresis (+0x174) [I].
std::optional<std::pair<glm::vec2, glm::vec2>> markupFrame(const ChartContext::CameraAxes& cam, float heading) {
    const glm::vec3 R[3] = {{std::sin(heading), -std::cos(heading), 0}, {std::cos(heading), std::sin(heading), 0}, {0, 0, 1}};
    const auto world = [&](glm::vec3 r) { return r.x * R[0] + r.y * R[1] + r.z * R[2]; };
    const auto lOf = [&](glm::vec3 r) { return glm::dot(glm::vec3(glm::dot(R[0], r), -glm::dot(R[1], r), glm::dot(R[2], r)), cam.look); };
    const auto unit = [](glm::vec3 v) { const float n = glm::length(v); return n > 0 ? v / n : v; };
    const auto flat = [&](glm::vec3 v) -> std::optional<glm::vec2> {
        const glm::vec2 c(glm::dot(v, cam.right), glm::dot(v, cam.down));
        const float n = glm::length(c);
        if (n < 1e-6f) return std::nullopt;
        return c / n;
    };
    const float z = cam.look.z;
    const bool tilt = z > 0;
    const auto build = [&](bool alt) -> std::optional<std::pair<glm::vec2, glm::vec2>> {
        glm::vec3 f = alt ? glm::vec3(0, 1, 0) : glm::vec3(0, 0.70710677f, 0.70710677f), s(1, 0, 0);
        if (tilt) {
            const float l = lOf(s);
            if (!alt) {
                const float l50 = lOf(f);
                if (l != 0) {
                    s.x *= 1.0f - z * std::sqrt(std::abs(l));
                    if (s.z == 0) s.z = 2.0f * l * z;
                }
                s = unit(s);
                f.y *= 1.0f - z;
                if (f.z == 0) f.z = l50 * -2.0f * z;
                f = unit(f);
            } else {
                if (l != 0) s.x *= 1.0f - std::abs(l);
                if (s.z == 0) s.z = (l < 0 ? -1.0f : 1.0f) * z;
                s = unit(s);
            }
        }
        const auto f2 = flat(world(f)), s2 = flat(world(s));
        if (!f2 || !s2) return std::nullopt;
        return std::pair{*f2, *s2};
    };
    auto fr = build(false);
    if (fr && std::acos(std::min(1.0f, std::abs(glm::dot(fr->first, fr->second)))) < 30.0f * 0.017453292f) fr = build(true);
    return fr;
}
int stickDirClass(ChartContext& ctx) {
    if (ctx.attachedMarkup()) {
        const auto cam = ctx.cameraAxes();
        const auto pad = ctx.pad();
        if (cam && pad) {
            if (ctx.stickLevel() <= 0.05f) return 0;
            const auto fr = markupFrame(*cam, ctx.heading());
            if (fr) {
                const float u = glm::dot(fr->first, *pad), w = glm::dot(fr->second, *pad);
                if (std::abs(u) <= std::abs(w)) return w >= 0 ? 4 : 3;
                return u >= 0 ? 1 : 2;
            }
        }
    }
    if (ctx.stickLevel() < 0.10f) return 0;
    const glm::vec2 s = ctx.stick();
    const float yaw = ctx.controlYaw();
    const glm::vec2 fwd(std::cos(yaw), std::sin(yaw)), right(std::sin(yaw), -std::cos(yaw));
    const float u = glm::dot(s, fwd), v = glm::dot(s, right);
    if (std::abs(u) <= std::abs(v)) return v >= 0 ? 4 : 3;
    return u >= 0 ? 1 : 2;
}
// 0x764270 (0x195): values 450..453 test classes 1..4, 454 class 0; other values are false whatever the negation.
bool stickDir(ChartContext& ctx, const Cond& c) {
    auto& k = ctx.chart().cache(c.id);
    if (k.frame != ctx.chart().frame()) k = {ctx.chart().frame(), stickDirClass(ctx)};
    switch (mode(c)) {
    case 450: return k.value == 1;
    case 451: return k.value == 2;
    case 452: return k.value == 3;
    case 453: return k.value == 4;
    case 454: return k.value == 0;
    default: return forced(c, false);
    }
}

// 0x761780 (0x199): the unbalance angle (agent +0x390, 0 taken as -0.01): 410 / 411 by 0x785FB0, 412 / 413 by
// 0x7868F0; other values are a debug print and false.
bool moveDirUnbalanced(ChartContext& ctx, const Cond& c) {
    float a = wrapPi(ctx.agentFloat(0x390));
    if (a == 0) a = -0.01f;
    const int m = mode(c);
    if (m == 410 || m == 411) return moveDirSector(ctx, a) == sectorOf(m);
    if (m == 412 || m == 413) return leftRightSector(ctx, c, a) == sectorOf(m);
    return forced(c, false);
}

// 0x761840 (0x19B): the jump target point (+0x9F0) seen from her against the control yaw (0x6EBA90), 0x785FB0.
// Without a jump target it returns the negation bit.
bool moveDirJumpTarget(ChartContext& ctx, const Cond& c) {
    const auto t = ctx.jumpTarget();
    if (!t) return false;
    const glm::vec2 d = glm::vec2(t.point) - glm::vec2(ctx.position());
    const float a = wrapPi(ctx.controlYaw() - yawOf(d));
    return moveDirSector(ctx, a) == sectorOf(mode(c));
}

// 0x76EC80 -> 0x76EB10 (0x19C): the stick's world yaw (0xFADFB0 +0xB0 +8) against the jump target's segment at
// round(+0xA68), folded to +-90 degrees (either way along the edge) unless the value is 413; 0x785FB0. Without a
// jump target it returns the negation bit; a degenerate segment takes the stick yaw itself.
bool markupAngleJumpTarget(ChartContext& ctx, const Cond& c) {
    const auto t = ctx.jumpTarget();
    if (!t) return false;
    const glm::vec2 s = ctx.stick();
    const float yaw = glm::length(s) > 0 ? yawOf(s) : ctx.controlYaw(); // [I] a centred stick keeps the control yaw
    float edgeYaw = yaw;
    if (const auto seg = slotSegment(ctx, t, roundI(t.param)); seg && glm::length(glm::vec2(seg->second - seg->first)) > 1e-4f)
        edgeYaw = yawOf(glm::vec2(seg->second - seg->first));
    float d = wrapPi(yaw - edgeYaw);
    if (mode(c) != 413 && std::abs(d) > kPi / 2) d = wrapPi(d + kPi);
    return moveDirSector(ctx, d) == sectorOf(mode(c));
}

// 0x72F1C0 (0x1D1): group 470 and the value equals the hurdle class player +0x6C (action 513).
bool hurdleType(ChartContext& ctx, const Cond& c) { return c.group == 470 && c.value == ctx.playerInt(0x6C); }

// 0x764D40 (0x1FF): agent byte +0x3E4 (set on attaching to a hurdle, 0x76D010 -> 0x73E610), inverted for value 0.
bool flag3E4(ChartContext& ctx, const Cond& c) {
    bool b = ctx.chart().field(0x3E4) != 0;
    if (c.value == 0) b = !b;
    return b;
}
// 0x764C30 (0x200): agent byte +0x3BD: 0 clear, 1 set, other false.
bool flag3BD(ChartContext& ctx, const Cond& c) {
    const bool b = ctx.chart().field(0x3BD) != 0;
    switch (mode(c)) {
    case 0: return !b;
    case 1: return b;
    default: return forced(c, false);
    }
}

// 0x763030 (0x201): on ground = agent +0x8C == 0, or a support instance (player +0x610), or player byte +0x11CC.
// [U] the writers of agent +0x8C / player +0x610 / +0x11CC (read from the chart-side stores).
bool onGround(ChartContext& ctx, const Cond& c) {
    const int s = ctx.chart().field(0x8C);
    const bool g = s == 0 || ctx.playerInt(0x610) != 0 || ctx.playerInt(0x11CC) != 0;
    switch (mode(c)) {
    case 0: return !g;
    case 1: return g;
    case 2: return !g && s == 1;
    default: return forced(c, false);
    }
}

// 0x765F40 (0x213): bool property 1705 (callback 0x765F10: 0x6EEF00(0x6A9, &b, 2), absent = true), edge-coded.
// 0x6EEDA0 mode 2 [O]: Lara's chart (player +0xF10) answers when its current state record's +0x8 word is nonzero
// (State::word8; 0 in every lara.drm state), else the upper-body chart (+0xF14) does, falling back to Lara's when it
// lacks the property or holds it with type 0x2B (property +0x2: the upper chart's root S0 declares most properties so,
// 1705 itself as bool 0). 0x6E5C10 = the property in force on that chart [I: same lookup as ours].
std::optional<bool> fullBodyProp(ChartContext& ctx, uint16_t id) {
    const ChartRuntime& rt = ctx.chart();
    const State* st = rt.data().state(rt.state());
    const ChartRuntime* up = ctx.upperChart();
    if ((!st || st->word8 == 0) && up)
        if (const Property* p = up->property(id); p && p->type != 0x2B) return up->charts().propBool(*p);
    return rt.propBool(id);
}
bool contextualAllowed(ChartContext& ctx, const Cond& c) { return edge(ctx, c, fullBodyProp(ctx, 0x6A9).value_or(true)); }

// 0x762250 (0x263): the class of the attached markup (+0xC94) by 0x782810.
bool attachedClass(ChartContext& ctx, const Cond& c) { return markupClass(ctx, c, ctx.attachedMarkup()); }

// 0x7623E0 -> 0x7622D0 (0x26C): bits of the attached markup: 690 it has an owning object, 691 player +0xED4 == 0
// (no attach surface class), 692 +0xED4 != +0xED8 (the class changed), 693 the object's class flags & 0x100, 694 a
// valid markup. Other values are a debug print and false. [I] ours: object markup = markup that moves with an
// instance; its class flags are unknown (693 unported while there is an object).
bool attachedBits(ChartContext& ctx, const Cond& c) {
    const auto a = ctx.attachedMarkup();
    switch (mode(c)) {
    case 690: return a && a.live;
    case 691: return ctx.playerInt(0xED4) == 0;
    case 692: return ctx.playerInt(0xED4) != ctx.playerInt(0xED8);
    case 693: return a && a.live ? ctx.chart().unported(c) : false;
    case 694: return bool(a);
    default: return forced(c, false);
    }
}

// 0x7626B0 (0x280): the lookahead bits agent +0xF0 (0x770BA0): 640..644 bits 0..4, 645 bit 3 or 4, 0 none of the low
// six, 1 any of them; other values are a debug print and false.
bool lookahead(ChartContext& ctx, const Cond& c) {
    const int b = ctx.chart().field(0xF0);
    const int m = mode(c);
    if (m >= 640 && m <= 644) return b & (1 << (m - 640));
    if (m == 645) return b & 0x18;
    if (m == 0) return (b & 0x3F) == 0;
    if (m == 1) return (b & 0x3F) != 0;
    return forced(c, false);
}

// 0x762880 (0x281) / 0x762920 (0x284): round(agent float +0xF4 / +0xF8) >= the value (0x782B40 mode 1), only with an
// attached markup flagged 0x0A010000; otherwise false whatever the negation.
bool markupDistance(ChartContext& ctx, const Cond& c) {
    const auto a = ctx.attachedMarkup();
    if (!a || !(a.flags & 0x0A010000)) return forced(c, false);
    return cmpInt(ctx, c, roundI(ctx.agentFloat(c.id == 0x281 ? 0xF4 : 0xF8)), 1);
}

// 0x7629C0 (0x28A): the hang clearance class player +0xEDC (0x74E290 at attach): 0 none, 1 any, 648..659 the classes
// 12, 11, 2, 5, 1, 3, 4, 6, 7, 8, 9, 10; other values are a debug print and false.
bool clearanceClass(ChartContext& ctx, const Cond& c) {
    static constexpr int kClass[] = {12, 11, 2, 5, 1, 3, 4, 6, 7, 8, 9, 10};
    const int r = ctx.playerInt(0xEDC);
    const int m = mode(c);
    if (m == 0) return r == 0;
    if (m == 1) return r != 0;
    if (m >= 648 && m <= 659) return r == kClass[m - 648];
    return forced(c, false);
}

// 0x7640A0 (0x2B4): the attached markup's segment at round(+0xC98) is at most `value` long (0x782B40 mode 2), then
// XORed with the negation a second time, so the negation cancels [O quirk]. No markup: length 0.
bool segmentShort(ChartContext& ctx, const Cond& c) {
    int len = 0;
    if (const auto a = ctx.attachedMarkup())
        if (const auto s = slotSegment(ctx, a, roundI(a.param))) len = roundI(glm::length(s->second - s->first));
    return forced(c, cmpInt(ctx, c, len, 2));
}

// 0x765380 -> 0x7652B0 -> 0x786E50 (0x3EB): bool property 1003 of the current state (absent = false), edge-coded.
bool stateFlag1003(ChartContext& ctx, const Cond& c) { return edge(ctx, c, propB(ctx, 1003)); }

// 0x766420 (0x960): at most every `value` 30 Hz frames (player +0x85C, game ms), plays request lara data +0x8B8
// ({1, 1479}) on her secondary anim layer; always true whatever the negation. Ours records the play count in player
// +0x85C's neighbour field playerInt(-0x85C) (shadow: not played).
bool throttledEffect(ChartContext& ctx, const Cond& c) {
    const uint32_t now = ctx.gameMs();
    const auto last = uint32_t(ctx.playerInt(0x85C));
    if (float(last) + float(ctx.chart().value(c)) * 33.333f < float(now)) {
        ctx.playerInt(0x85C) = int(now);
        ++ctx.playerInt(-0x85C);
    }
    return forced(c, true);
}

// ---- 0x770BA0: the markup contact / lookahead probe (agent +0xF0 low six bits, agent float +0xF4) ----

// 0x787EF0: the limb class of state property 499 (410 / 414 1, 411 / 415 2, 412 / 416 / 452 3, 413 / 417 / 453 4,
// 450 5, 451 6, else 0).
int limbClass(ChartContext& ctx) {
    switch (propI(ctx, 499)) {
    case 410: case 414: return 1;
    case 411: case 415: return 2;
    case 412: case 416: case 452: return 3;
    case 413: case 417: case 453: return 4;
    case 450: return 5;
    case 451: return 6;
    default: return 0;
    }
}

// 0x735970 (from 0x7364D0) on a ledge: walk the attached markup from her param toward the limb class's side (3: -1,
// 4: +1; else nowhere) for the lookahead distance (Lara data +0x6F2, +0x6F4 when player +0xED4 != 0), meeting
// 0x5D0090's events: a vertex turning more than 30 degrees (0x5C9170: the 2D cross of the two segment directions,
// type 3 one way, 2 the other) and the markup's end (type 1, at the walked distance). Bits: 4 the end within Lara data
// +0x6DC; 8 a type-2 corner within +0x700 / +0x702 (scaled by +0x706 with bool property 0x21A) sharper than 90 -
// +0x708 degrees, which also sets the corner angle agent +0x9C / +0xA0; 0x10 the same for type 3 within +0x6FC /
// +0x6FE; then 1 when a ray from her root + (0, 0, +0x6F8) along the walk (to the vertex + +0x6F6 and on along the
// next segment when the walk crosses one) hits (0x493330 flags 0x2C19E7). +0xF4 = the walked distance left to the
// event; +0xA0 is cleared without a corner bit. [I] 0x5D0090's type 5 / 7 (connected markup) and 6 (a link: bit
// 0x40) do not occur in ours (no markup links); the corner's angle test reads the turn between the two segments.
int ledgeLookahead(ChartContext& ctx, const ChartContext::MarkupSlot& a) {
    const int lc = limbClass(ctx);
    const float dir = lc == 3 ? -1.0f : lc == 4 ? 1.0f : 0.0f;
    const bool noClass = ctx.playerInt(0xED4) == 0;
    const float reach = float(pd(ctx, noClass ? 0x6F2 : 0x6F4));
    const float walk = reach * dir;
    float scale = 1;
    if (const Property* p = ctx.chart().property(0x21A); p && p->type == 2 && propB(ctx, 0x21A)) scale = float(pd(ctx, 0x706));
    const float convexReach = float(pd(ctx, noClass ? 0x6FC : 0x6FE)) * scale; // type 3
    const float concaveReach = float(pd(ctx, noClass ? 0x700 : 0x702)) * scale; // type 2
    const float endReach = float(pd(ctx, 0x6DC));
    const float sharp = std::sin(glm::radians(90.0f - float(pd(ctx, 0x708))));
    int bits = 0;
    float left = ctx.agentFloat(0xF4);
    const int n = ctx.markupSegmentCount(a.markup);
    if (n > 0 && dir != 0) {
        int seg = std::clamp(int(std::floor(a.param)), 0, n - 1);
        float f = a.param - float(seg);
        float walked = 0;
        for (int guard = 0; guard < n + 1; ++guard) {
            const auto s = ctx.markupSegment(a.markup, seg);
            if (!s) break;
            const float len = glm::length(s->second - s->first);
            const float toVertex = (dir > 0 ? 1.0f - f : f) * len;
            if (walked + toVertex >= std::abs(walk)) break; // the walk ends on this segment: no event
            walked += toVertex;
            const int next = seg + int(dir);
            if (next < 0 || next >= n) { // type 1: the markup ends
                if (walked <= endReach) bits |= 4, left = dir * endReach - dir * walked;
                break;
            }
            const auto s2 = ctx.markupSegment(a.markup, next);
            if (!s2) break;
            glm::vec2 d1 = glm::vec2(s->second - s->first) * dir, d2 = glm::vec2(s2->second - s2->first) * dir;
            if (glm::length(d1) > 0) d1 = glm::normalize(d1);
            if (glm::length(d2) > 0) d2 = glm::normalize(d2);
            const float cross = d1.x * d2.y - d1.y * d2.x;
            if ((a.flags & 0x4B010000) && std::abs(cross) > 0.5f) {
                const bool concave = cross < -0.5f; // type 2 [I sign]
                const float r = concave ? concaveReach : convexReach;
                if (walked <= r) {
                    if (concave) {
                        float ang = glm::degrees(std::acos(std::clamp(glm::dot(d1, d2), -1.0f, 1.0f)));
                        if (dir < 0) ang = 180.0f - ang;
                        ctx.agentFloat(0x9C) = ang;
                        ctx.chart().field(0xA0) = 1;
                    }
                    if (std::abs(glm::dot(d1, d2)) < sharp) {
                        bits |= concave ? 8 : 0x10;
                        left = dir * r - dir * walked;
                        break;
                    }
                }
            }
            seg = next;
            f = dir > 0 ? 0.0f : 1.0f;
        }
    }
    ctx.agentFloat(0xF4) = left;
    // the rays
    if (n > 0 && ctx.hasCollision()) {
        const int seg = std::clamp(int(std::floor(a.param)), 0, n - 1);
        const float f = a.param - float(seg);
        if (const auto s = ctx.markupSegment(a.markup, seg)) {
            const glm::vec3 d = s->second - s->first;
            const float len = glm::length(d);
            const glm::vec3 u = len > 0 ? d / len : glm::vec3(0);
            float toEnd = dir > 0 ? (1.0f - f) * len : f * len;
            if (toEnd < 0.1f) {
                const glm::vec3 vtx = dir > 0 ? s->second : s->first;
                toEnd = std::min(std::abs(glm::dot(ctx.position() - vtx, u)), std::abs(walk) - 1.0f);
            }
            const glm::vec3 p0 = ctx.position() + glm::vec3(0, 0, float(pd(ctx, 0x6F8)));
            const int next = seg + (dir >= 0 ? 1 : -1);
            bool hit = false;
            if (next < 0 || next >= n || std::abs(walk) <= 0 || std::abs(walk) <= toEnd) {
                hit = bool(ctx.playerRay(p0, p0 + u * walk));
            } else {
                const glm::vec3 p1 = p0 + u * (dir * (float(pd(ctx, 0x6F6)) + toEnd));
                hit = bool(ctx.playerRay(p0, p1));
                if (const auto s2 = ctx.markupSegment(a.markup, next)) {
                    const glm::vec3 d2 = s2->second - s2->first;
                    const float l2 = glm::length(d2);
                    // along the next segment: `walk` units of it, or all of it (signed) when it is shorter
                    const glm::vec3 v = l2 <= 0 ? glm::vec3(0) : std::abs(walk) <= l2 ? d2 / l2 * walk : d2 * (walk > 0 ? 1.0f : -1.0f);
                    hit = hit || bool(ctx.playerRay(p1, p1 + v));
                }
            }
            if (hit) bits |= 1;
        }
    }
    if (!(bits & 0x18)) ctx.chart().field(0xA0) = 0;
    return bits;
}

} // namespace

// 0x770BA0 (action 404's update, its entry 0x770EB0 clears the bits first): with an attached markup, a ray 300 (Lara
// data +0x70A) long from 160 (-(+0x250)) out of the markup frame (agent +0xB0) along its up axis sets bit 2 when it
// hits; with a limb class (property 499) the markup's lookahead adds its bits: ledges 0x735970 (above; 0x74BCF0 for
// the wall-hug shelf when property 497 is 628), poles 0x7327F0 / 0x7337F0, hurdles 0x7327F0, ladders 0x744920,
// pillars 0x753D30, holds 0x74B640, boxes 0x72AD70. [I] the frame ours builds: X along the segment, Y the outward
// normal, Z up at the grab point. Only the ledge lookahead is ported; the others count action 404 as unported.
// 0x753D30: the pillar lookahead (markup flag 0x10), limb class 5 (up, property 499 = 450) / 6 (down, 451) [O].
// The pillar's z extent: 0x6F94F0 / 0x7347C0 = the higher / lower z of its first and last point, less / plus the
// markup-class record's +0x98 / +0x9A (0x5D5930 -> 0x5D57A0: flag 0x10 = Lara data +0x590's record; 22 / 284) and
// 0.3164. Up: the grab point (+0xCA8) over the top -> 4; down: at or under the bottom -> 4. Then 0x70B5A0 casts in her
// root bone's frame (instance +0x90), moved by -row1 x (s - Lara data +0x7E0) (0x753390), from local z +0x7F6 to
// +0x7F6 + +0x7F2 (up, s = +0x7F4) or +0x7FC to +0x7FC - +0x7F8 (down, s = +0x7FA): a hit -> 2 (up) / 1 (down).
// 0x7538D0 (the hit's ledge point into player +0xEC0..+0xECC) is not ported (counted as action 404's).
// [I] row1 of the bone frame = backward (-forward): her hands are at local -y (AnimLibrary::hangHandOffset).
int pillarLookahead(ChartContext& ctx, const ChartContext::MarkupSlot& a, int lc) {
    if (lc != 5 && lc != 6) return 0;
    const int count = ctx.markupSegmentCount(a.markup);
    const auto first = ctx.markupSegment(a.markup, 0), last = ctx.markupSegment(a.markup, std::max(count - 1, 0));
    if (!first || !last) return 0;
    const auto& d = ctx.chart().charts();
    int16_t r98 = 0, r9A = 0;
    if (const auto r = d.pointer(d.dataOffset + 0x590); r && size_t(*r) + 0x9C <= d.bytes.size())
        std::memcpy(&r98, d.bytes.data() + *r + 0x98, 2), std::memcpy(&r9A, d.bytes.data() + *r + 0x9A, 2);
    const float z0 = first->first.z, z1 = last->second.z;
    const float top = std::max(z0, z1) - float(r98) - 0.31640562f, bottom = std::min(z0, z1) + float(r9A) + 0.31640562f;
    const float grabZ = a.point.z;
    int bits = 0;
    if (lc == 5 ? top < grabZ : grabZ <= bottom) bits = 4;
    const float s = float(pd(ctx, lc == 5 ? 0x7F4 : 0x7FA)), from = float(pd(ctx, lc == 5 ? 0x7F6 : 0x7FC));
    const float reach = lc == 5 ? float(pd(ctx, 0x7F2)) : -float(pd(ctx, 0x7F8));
    if (reach != 0 && ctx.hasCollision()) {
        const float h = ctx.heading();
        const glm::vec3 back(-std::cos(h), -std::sin(h), 0);
        const glm::vec3 o = ctx.rootBone() - back * (s - float(pd(ctx, 0x7E0)));
        if (ctx.playerRay(o + glm::vec3(0, 0, from), o + glm::vec3(0, 0, from + reach))) {
            bits |= lc == 5 ? 2 : 1;
            ++ctx.chart().stats.unportedActions[404]; // 0x7538D0
        }
    }
    return bits;
}

// 0x744920: the ladder lookahead (markup flag 0x400000), limb class 5 (up) / 6 (down) [O]. The rung she holds
// (0x72B8C0: her root's depth under the top point less Lara data float +0x784, over the spacing +0x770, rounded) and
// the rung count (0x734B70: trunc(trunc(the first segment's length) / spacing) + 1), as Traverse.cpp places her on
// them; up: rung < 1 -> 4; down: rung >= count - byte +0x774 -> 4. Then the pillar's cast (0x70B5A0 in her root
// bone's frame, moved by -row1 x s): up s = +0x78C from local z +0x78E to +0x78E + +0x78A, down s = +0x792 from
// +0x794 to +0x794 - +0x790; a hit -> 2 (up) / 1 (down).
int ladderLookahead(ChartContext& ctx, const ChartContext::MarkupSlot& a, int lc) {
    if (lc != 5 && lc != 6) return 0;
    const auto seg = ctx.markupSegment(a.markup, 0);
    if (!seg) return 0;
    const auto& d = ctx.chart().charts();
    const float spacing = d.data<float>(0x770);
    if (!(spacing > 0)) return 0;
    const int rung = int(std::floor((seg->first.z - ctx.position().z - d.data<float>(0x784)) / spacing + 0.5f));
    const int count = int(std::trunc(std::trunc(glm::length(seg->second - seg->first)) / spacing)) + 1;
    int bits = 0;
    if (lc == 5 ? rung < 1 : count - int(d.data<uint8_t>(0x774)) <= rung) bits = 4;
    const float s = float(pd(ctx, lc == 5 ? 0x78C : 0x792)), from = float(pd(ctx, lc == 5 ? 0x78E : 0x794));
    const float reach = lc == 5 ? float(pd(ctx, 0x78A)) : -float(pd(ctx, 0x790));
    if (reach != 0 && ctx.hasCollision()) {
        const float h = ctx.heading();
        const glm::vec3 back(-std::cos(h), -std::sin(h), 0);
        const glm::vec3 o = ctx.rootBone() - back * s;
        if (ctx.playerRay(o + glm::vec3(0, 0, from), o + glm::vec3(0, 0, from + reach))) bits |= lc == 5 ? 2 : 1;
    }
    return bits;
}

void markupContactProbe(ChartContext& ctx) {
    int& out = ctx.chart().field(0xF0);
    const auto a = ctx.attachedMarkup();
    if (!a) return;
    int bits = 0;
    if (ctx.hasCollision()) {
        const glm::vec3 start = a.point + a.normal * float(-pd(ctx, 0x250));
        const float len = float(pd(ctx, 0x70A));
        if (ctx.playerRay(start, start + glm::vec3(0, 0, len))) bits |= 2;
    }
    const uint32_t m = a.flags;
    if (limbClass(ctx) != 0) {
        if ((m & 0x40000000) != 0) {
            const auto shelf = ctx.ledgeShelf(a.markup, a.param);
            if (propI(ctx, 0x1F1) == 628 && shelf.value_or(false)) ++ctx.chart().stats.unportedActions[404]; // 0x74BCF0
            else bits |= ledgeLookahead(ctx, a);
        } else if ((m & 0x400000) != 0) {
            bits |= ladderLookahead(ctx, a, limbClass(ctx));
        } else if ((m & (0x1000000 | 0x10000)) != 0) {
            ++ctx.chart().stats.unportedActions[404]; // 0x7327F0 / 0x7337F0
        } else if (m & 0x10) {
            bits |= pillarLookahead(ctx, a, limbClass(ctx));
        } else if (m & 0x8000000) {
            ++ctx.chart().stats.unportedActions[404]; // 0x741720 -> 0x74B640
        }
    }
    if (m & 0x8000) ++ctx.chart().stats.unportedActions[404]; // 0x72AD70
    out = (out & ~0x3F) | bits;
}

// 0x76D010: per-class attach init. Ledges (0x40000000) and boxes (0x8000): the hang clearance class 0x74E290 ->
// player +0xEDC (ours: ChartContext::ledgeClearanceClass). Ladders 0x7352A0, holds 0x7352A0 + 0x741740, hurdles
// (0x10000: agent +0x3E4 = 0x73E610) and poles 0x7343F0 are not ported (counted as action 420's).
void markupAttachInit(ChartContext& ctx) {
    const auto a = ctx.attachedMarkup();
    if (!a) return;
    ctx.playerInt(0xEDC) = 0;
    if (a.flags & (0x40000000 | 0x8000)) {
        ctx.playerInt(0xEDC) = ctx.ledgeClearanceClass();
        return;
    }
    if (a.flags & (0x400000 | 0x10000 | 0x2000000 | 0x8000000)) ++ctx.chart().stats.unportedActions[420];
}

void detachChartMarkup(ChartContext& ctx) {
    if (!ctx.chartAttached) return;
    if (const auto& a = *ctx.chartAttached) ctx.playerInt(0xEA0) = a.markup + 1, ctx.playerFloat(0xEA4) = a.param; // ours: index + 1
    ctx.chartAttached = ChartContext::MarkupSlot{};
}

void attachChartMarkup(ChartContext& ctx, const ChartContext::MarkupSlot& slot) {
    if (!ctx.chartAttached || !slot) return;
    detachChartMarkup(ctx);
    ctx.chartAttached = slot;
    // 0x707730 / 0x707530 then copy the search block and clear the jump target (+0xA64 = 0): the chart's own target
    // goes, and the game's target (the same markup, until the game drops it) counts as handed on.
    ctx.chartTarget = {};
    ctx.handedTarget = slot.markup;
}

// 0x744E90: one ray in the frame `rows` (x along the edge, y the edge's outward normal, z up, origin) from (lat, 90, h)
// to (lat, -Lara data +0x6E6, h), h = +0x6E4 - +0x252 (0x4935F0, ray flags 0x2C19E7): a hit = a wall under the hang.
namespace {
bool braceRay(ChartContext& ctx, glm::vec3 e, glm::vec3 n, glm::vec3 o, float lat) {
    const float h = float(pd(ctx, 0x6E4)) - float(pd(ctx, 0x252));
    const glm::vec3 up = glm::cross(e, n);
    const glm::vec3 a = o + e * lat + n * 90.0f + up * h, b = o + e * lat - n * float(pd(ctx, 0x6E6)) + up * h;
    return bool(ctx.playerRay(a, b));
}
// The frame 0x74F480 builds on a segment: n = the horizontal edge direction turned -90 degrees (0x49C8B0), e =
// cross(n, up), up = cross(e, n).
void braceFrame(glm::vec3 d, glm::vec3& e, glm::vec3& n) {
    glm::vec2 h(d.x, d.y);
    h = glm::length(h) > 0 ? glm::normalize(h) : glm::vec2(1, 0);
    n = glm::vec3(h.y, -h.x, 0);
    e = glm::cross(n, glm::vec3(0, 0, 1));
}
} // namespace

std::optional<bool> ledgeBrace(ChartContext& ctx, const ChartContext::MarkupSlot& m) {
    const int count = ctx.markupSegmentCount(m.markup);
    if (count <= 0 || !ctx.hasCollision()) return std::nullopt;
    const bool closed = m.flags & 4; // markup +0x28 bit 2: the polyline wraps
    const int seg = std::clamp(int(std::floor(m.param)), 0, count - 1);
    const float f = m.param - std::floor(m.param);
    int other = seg;
    if (f > 0.99f) other = seg < count - 1 ? seg + 1 : closed ? 0 : seg;
    else if (f < 0.01f) other = seg >= 1 ? seg - 1 : closed ? count - 1 : seg;
    const auto s = ctx.markupSegment(m.markup, seg);
    if (!s) return std::nullopt;
    const glm::vec3 p = s->first + (s->second - s->first) * f;
    glm::vec3 e, n;
    braceFrame(s->second - s->first, e, n);
    if (other != seg) { // at a vertex: 40 into this segment ("segment"), else 40 into the other one ("otherSegment")
        const bool forward = other == seg + 1 || (closed && seg == count - 1 && other == 0);
        const glm::vec3 back = glm::normalize(s->first - s->second) * 40.0f;
        if (braceRay(ctx, e, n, forward ? p + back : p - back, 0)) return true;
        const auto so = ctx.markupSegment(m.markup, other);
        if (!so) return std::nullopt;
        glm::vec3 e2, n2;
        braceFrame(so->second - so->first, e2, n2);
        const glm::vec3 back2 = glm::normalize(so->first - so->second) * 40.0f;
        return braceRay(ctx, e2, n2, forward ? p - back2 : p + back2, 0);
    }
    // limb class 4 (right) on the last segment / 3 (left) on the first: the ray moves up to Lara data +0x704 toward the
    // end, at most 0.8 of what is left of the segment
    float lat = 0;
    const float len = glm::length(s->second - s->first);
    const int lc = limbClass(ctx);
    if (lc == 4 && seg == count - 1) lat = -std::min(float(pd(ctx, 0x704)), (1.0f - f) * 0.8f * len);
    else if (lc == 3 && seg == 0) lat = std::min(float(pd(ctx, 0x704)), f * 0.8f * len);
    return braceRay(ctx, e, n, p, lat);
}

// 0x74C8D0 [O: decomp + disasm 0x74C9D6..0x74D38B]. With alt (0x1A4) it re-attaches +0xC94 at +0xC98 and returns
// before the VM byte and the mirror (0x74D282): nothing here changes. Else:
//  - d = the jump target point (+0x9F0) - her root, flattened, when a markup was attached (then detached, 0x6FDBE0);
//    0x707B90 attaches the jump target (attachChartMarkup).
//  - 0x733690: f = her facing (model -Y, flattened), s = the first segment's flattened direction; dot = s.f (1 when f
//    is degenerate); along = |dot| >= cos(Lara data float +0x740 degrees). d degenerate (0x49AFA0) -> d = f.
//  - properties 430 / 431 (0x6EEF00 mode 2, false when absent): +0x344 = !431 && (430 || !along).
//  - +0x344 = 0: n = the markup normal (+0xCB0), its xy negated when n.d >= 0 (+0xED0 = 1, else 2); +0x345 = 0.
//    +0x344 = 1: n = +0xCC0 (0x6FE7E0: a horizontal pole's cross(segment, up), 0x72B4A0), negated when n.d > 0;
//    +0x345 = 0 when (stick magnitude < Lara data float +0x748 and player +0xEE8 == 0) or the jump query profile
//    (+0xA84, 0x6F1950) has +0x2C flag 0x100 or 0x80, else 1.
//  - side = cross(up, n).d: > 0.15 mirror on; < -0.15 off (for +0x344 = 0; for 1 only when not > 0.15 and < -0.15:
//    off), else the mirror stays. [I] the pole branch of 0x6FE7E0 for flag 0x8000000 takes 0x72B4A0 when 0x733F60
//    accepts it (pitch within *(0xE7EE50)+0x184 [U]); horizontal poles assumed accepted.
// Not ported (counted as action 230's): the heading targets +0xCDC / agent +0x244 / +0x234, the +0x1250 / +0x1260
// frames, 0x74BF90's helper object, +0xCC0 for markups other than horizontal poles.
void poleAttachSetter(ChartContext& ctx, bool alt) {
    ChartRuntime& rt = ctx.chart();
    if (alt) return;
    const auto prev = ctx.attachedMarkup();
    const auto t = ctx.jumpTarget();
    glm::vec2 d(0);
    if (prev && t) d = glm::vec2(t.point - ctx.position());
    if (t) attachChartMarkup(ctx, t);
    const auto a = ctx.attachedMarkup();
    if (!a) return;
    ++rt.stats.unportedActions[230];
    const float h = ctx.heading();
    const glm::vec2 f(std::cos(h), std::sin(h));
    float dot = 1;
    bool along = true;
    if (const auto seg = ctx.markupSegment(a.markup, 0); seg && glm::dot(f, f) >= 0.0001f) {
        glm::vec2 sd(seg->second - seg->first);
        sd = glm::length(sd) > 0 ? glm::normalize(sd) : glm::vec2(0);
        dot = glm::dot(sd, glm::normalize(f));
        along = std::abs(dot) >= std::cos(glm::radians(rt.charts().data<float>(0x740)));
    }
    if (std::abs(d.x) <= 1e-6f && std::abs(d.y) <= 1e-6f) d = f;
    const bool p430 = fullBodyProp(ctx, 430).value_or(false), p431 = fullBodyProp(ctx, 431).value_or(false);
    const bool vertical = !p431 && (p430 || !along); // agent +0x344
    glm::vec2 n;
    if (!vertical) {
        n = glm::vec2(a.normal);
        ctx.playerInt(0xED0) = glm::dot(n, d) < 0 ? 2 : 1;
        if (ctx.playerInt(0xED0) == 1) n = -n;
        ctx.vmPoleSide = 0;
    } else {
        ctx.playerInt(0xED0) = 0;
        if (!(a.flags & 0x8000000) && !(a.flags & 0x2000000)) return; // +0xCC0 of other markups: not ported
        const auto seg = ctx.markupSegment(a.markup, 0);
        if (!seg) return;
        const glm::vec3 sd = seg->second - seg->first;
        n = glm::length(glm::vec2(sd.y, -sd.x)) > 0 ? glm::normalize(glm::vec2(sd.y, -sd.x)) : glm::vec2(0);
        if (glm::dot(n, d) > 0) n = -n;
        // 0x74CC8A..0x74CCDC: the stick test first; the profile record is read only when it does not decide
        if (ctx.stickLevel() < rt.charts().data<float>(0x748) && ctx.playerInt(0xEE8) == 0) ctx.vmPoleSide = 0;
        else {
            const PlayerState* ps = ctx.player();
            const auto prof = ps ? decodeProfile(rt.charts(), int(int16_t(ps->jumpQuery))) : std::nullopt;
            if (!prof) ctx.vmPoleSide = std::nullopt;
            else ctx.vmPoleSide = (prof->flags & 0x180) ? 0 : 1;
        }
    }
    const float side = -n.y * d.x + n.x * d.y;
    if (side > 0.15f) rt.setMirror(true);
    else if (side < -0.15f) rt.setMirror(false);
}

// 602 / 611 (0x74DCD0) attach only a ladder target (MUD 0x400000): markupAttachTarget checks it.
bool attachTargetClass(int cls) {
    switch (cls) {
    case 600: case 601: case 602: case 603: case 605: case 606: case 610: case 611: case 614: case 621: case 624: case 625:
    case 626: case 627: return true;
    default: return false;
    }
}

namespace {

// ---- actions ----

// 0x749980 / 0x749A10 / 0x749AD0: the clamped blend axes.
float span01(float v, float a, float b) { return b == a ? 0.0f : std::clamp((v - a) / (b - a), 0.0f, 1.0f); }
float spanSigned(float v, float a, float lo, float hi) { return hi < lo ? 0.0f : hi == lo ? 0.0f : std::clamp((v - a) / (hi - lo), -1.0f, 1.0f); }

// Action 213 preenter / prereenter 0x749CD0: the hurdle-over clip's blend from the attached grab point (+0xCA0) to the
// jump target point (+0x9F0): x by their horizontal distance (properties 230 / 231, or signed with 229 > 0), y by the
// target's height over her (233 / 234) [O].
void hurdleOverBlend(ChartContext& ctx, uint16_t) {
    const float p229 = propF(ctx, 229), p230 = propF(ctx, 230), p231 = propF(ctx, 231), p233 = propF(ctx, 233), p234 = propF(ctx, 234);
    const glm::vec3 a = ctx.attachedMarkup().point, b = ctx.jumpTarget().point;
    const float d = glm::length(glm::vec2(b - a));
    ctx.animBlend.x = p229 <= 0 ? span01(d, p230, p231) : spanSigned(d, p230, p229, p231);
    ctx.animBlend.y = span01(b.z - ctx.position().z - p233, 0, p234 - p233);
}

// Action 232 preenter / prereenter 0x749E90 (with a jump target and an attached markup): blend x as 0x749A10 over
// properties 230 / 229 / 231, then detach (0x6FDBE0: player +0xEA0 / +0xEA4 the markup and param left) and attach the
// jump target (0x707B90: +0xC94 / +0xC98 / +0xCA0), chart-side (attachChartMarkup).
void hurdleReattach(ChartContext& ctx, uint16_t) {
    const auto a = ctx.attachedMarkup();
    const auto t = ctx.jumpTarget();
    if (!a || !t) return;
    ctx.animBlend.x = spanSigned(glm::length(glm::vec2(t.point - a.point)), propF(ctx, 230), propF(ctx, 229), propF(ctx, 231));
    ctx.playerInt(0xEA0) = a.markup + 1, ctx.playerFloat(0xEA4) = a.param;
    attachChartMarkup(ctx, t);
}

// 0x72A5F0: the beam controller (agent +0x358): which way she faces along the attached segment floor(+0xC98)
// (player +0xED0 1 along / 2 against, +0xCDC 0 / pi), the beam angle +0x35C = wrap(segment yaw + +0xCDC), segment
// +0x360. When the angle changes: +0x37C wobble delay = Lara data +0xEC8 (property 912 present), +0x368 / +0x36C =
// random in [+0xEA4, +0xEA6] / [+0xEA8, +0xEAA], +0x384 wobble rate random in [+0xE9C, +0xEA0] degrees with a random
// sign (+0x3BC), +0x388 = 0. [I] the random draws come from our own generator.
bool beamFacing(ChartContext& ctx) {
    const auto a = ctx.attachedMarkup();
    if (!a) return false;
    const int seg = int(std::floor(a.param));
    const auto s = ctx.markupSegment(a.markup, seg);
    if (!s) return false;
    const glm::vec2 d = glm::vec2(s->second - s->first);
    const glm::vec2 h(std::cos(ctx.heading()), std::sin(ctx.heading()));
    const bool along = glm::dot(glm::length(d) > 0 ? glm::normalize(d) : d, h) >= 0;
    ctx.playerInt(0xED0) = along ? 1 : 2;
    ctx.playerFloat(0xCDC) = along ? 0.0f : kPi;
    const float old = ctx.agentFloat(0x35C);
    ctx.chart().field(0x360) = seg;
    const float ang = wrapPi(yawOf(d) + ctx.playerFloat(0xCDC));
    ctx.agentFloat(0x35C) = ang;
    if (old == ang) return false;
    static std::minstd_rand rng(0x72A5F0);
    const auto rnd = [&](float lo, float hi) { return lo + (hi - lo) * std::uniform_real_distribution<float>(0, 1)(rng); };
    const bool wobble = propI(ctx, 912, 912) != 912;
    ctx.agentFloat(0x37C) = wobble ? float(pd(ctx, 0xEC8)) : 0.0f;
    ctx.agentFloat(0x368) = rnd(pd(ctx, 0xEA4), pd(ctx, 0xEA6));
    ctx.agentFloat(0x36C) = rnd(pd(ctx, 0xEA8), pd(ctx, 0xEAA));
    float rate = wobble ? rnd(glm::radians(ctx.chart().charts().data<float>(0xE9C)), glm::radians(ctx.chart().charts().data<float>(0xEA0))) : 0.0f;
    const bool neg = rnd(0, 1) < 0.5f;
    ctx.chart().field(0x3BC) = neg;
    ctx.agentFloat(0x384) = neg ? -rate : rate;
    ctx.agentFloat(0x388) = 0;
    return true;
}

// 0x743900 (thiscall on the beam controller agent +0x358; disassembled 0x743900..0x74430D [O]): the per-tick balance
// steer. Fields: +0x358 the beam heading in [0, 2pi) after a turn-around, +0x35C the beam angle, +0x3A0 the lean,
// +0x3A4 the walk amount, +0x3A8 the target off the beam angle, +0x3B4 the held heading; the layer (instance +0x280 ->
// +0x14) mirror bit +0xF4 0x20 (ChartRuntime mirror) and blend parameters +0x8C / +0x90.
//  1. want = the mirror bit; prop 0x227 (bool) p227; idle = Lara data byte +0xE94 & 0x10 and the stick magnitude
//     (0xFADFB0 -> +0xB0 -> +0x18) == 0.
//  2. init: +0x3A0 = +0x3A4 = 0, the blend 0 / 0, +0x3B4 = her heading; dh = wrap(heading - beam angle):
//     dh < -0.1 -> want on, dh > 0.1 -> off.
//  3. +0x3B4 = the stick's world heading (+0x8) while the stick is off zero; target = p227 || idle ? wrap(beam angle)
//     : +0x3B4 -> agent +0x28 (the heading target); +0x3A8 = wrap(target - beam angle); Wb = wrap(beam angle),
//     d = wrap(wrap(target) - Wb); with combat aim flags (player +0x504 -> +0x1FC bit 9 / 10) the target is the aim
//     point player +0x800 instead (0x49F030 [I: ours the heading from her to it]).
//  4. rate = 0.1 x timeMult, or prop 0xD1 x timeMult (at most 1) when prop 0xD1 > 0; prev = +0x3A4;
//     e = wrap(heading - Wb).
//  5. 1.5808 < |d| < 2.0944, (1 - rate) <= |prev|, |prev| > 0.5 and |e| < 0.01: turn around: heading = beam angle + pi
//     in [0, 2pi) (also +0x358), +0x35C -= +0xCDC, +0xED0 1 <-> 2 with +0xCDC pi / 0, +0x35C += +0xCDC; d =
//     wrap(Wt - new angle); prev -= 2 sign(prev), clamped to [-1 - rate, 1 + rate]; want = !mirror; player +0xF3C
//     1 <-> 2 (else 0).
//     Else |d| > pi/2: d = sign(d) pi/2 (clamped).
//  6. amount = d / (pi/2), outside init clamped to prev +- rate. Unless clamped with |d| > pi/2 and |prev| < 1 - rate:
//     amount zeroed when its sign is against want (want off: amount < 0; on: amount > 0).
//  7. No attached markup: the blend 0 / 0, done. Else the lean = -asin(n.z x (flat n . f)) / (pi/6) clamped to [-1, 1]
//     (n the attached segment's unit direction, flat n its unit horizontal, f her facing), outside init within
//     +0x3A0 +- 1.
//  8. Layer type 2: +0x3A4 = amount, +0x8C = |amount|, the mirror = want. 7: +0x3A4 = amount, +0x3A0 = lean, +0x8C =
//     lean, +0x90 = |amount|. 5: as 7, and the mirror = want.
// prop 0x261 is read (into a byte nothing tests).
void beamBalance(ChartContext& ctx, bool init) {
    ChartRuntime& rt = ctx.chart();
    const auto& cd = rt.charts();
    bool want = rt.mirrored();
    const bool mirror = want;
    const bool p227 = propB(ctx, 0x227);
    const float stick = ctx.stickLevel();
    const bool idle = (cd.data<uint8_t>(0xE94) & 0x10) && stick == 0;
    float& beamHeading = ctx.agentFloat(0x358);
    float& angle = ctx.agentFloat(0x35C);
    float& lean = ctx.agentFloat(0x3A0);
    float& walk = ctx.agentFloat(0x3A4);
    float& held = ctx.agentFloat(0x3B4);
    if (init) {
        lean = walk = 0;
        ctx.setLayerBlend(0, 0);
        held = ctx.heading();
        const float dh = wrapPi(held - angle);
        if (dh < -0.1f) want = true;
        else if (dh > 0.1f) want = false;
    }
    if (stick > 0) held = yawOf(ctx.stick()); // [I] ours: the stick's world direction
    float target = held;
    if (p227 || idle) target = wrapPi(angle);
    ctx.agentFloat(0x28) = target;
    ctx.agentFloat(0x3A8) = wrapPi(target - angle);
    const float wb = wrapPi(angle);
    float wt = wrapPi(target);
    float d = wrapPi(wt - wb);
    float rate = ctx.timeMult * 0.1f;
    const float prev0 = walk;
    float prev = prev0;
    if (const float p = propF(ctx, 0xD1, 1.0f); p > 0) rate = std::min(p * ctx.timeMult, 1.0f);
    if (ctx.combatFlag(9).value_or(false) || ctx.combatFlag(10).value_or(false)) {
        const glm::vec2 aim(ctx.playerFloat(0x800), ctx.playerFloat(0x804));
        wt = wrapPi(yawOf(aim - glm::vec2(ctx.position())));
        d = wrapPi(wt - wb);
    }
    float e = wrapPi(ctx.heading() - wb);
    (void)propB(ctx, 0x261);
    bool clamped = false;
    const float ad = std::abs(d);
    if (ad > 1.5807964f && ad < 2.0943952f && 1.0f - rate <= std::abs(prev) && std::abs(prev) > 0.5f && std::abs(e) < 0.01f) {
        const float v = (angle + kPi) / kTwoPi;
        const float turned = (v - std::floor(v)) * kTwoPi;
        beamHeading = turned;
        ctx.setHeading(turned);
        angle -= ctx.playerFloat(0xCDC);
        if (ctx.playerInt(0xED0) == 1) ctx.playerInt(0xED0) = 2, ctx.playerFloat(0xCDC) = kPi;
        else ctx.playerInt(0xED0) = 1, ctx.playerFloat(0xCDC) = 0;
        angle += ctx.playerFloat(0xCDC);
        d = wrapPi(wt - angle);
        prev -= 2.0f * (prev >= 0 ? 1.0f : -1.0f);
        prev = std::clamp(prev, -1.0f - rate, 1.0f + rate);
        want = !mirror;
        int& f3c = ctx.playerInt(0xF3C);
        f3c = f3c == 1 ? 2 : f3c == 2 ? 1 : 0;
    } else if (std::abs(d) > kHalfPi) {
        d = (d >= 0 ? 1.0f : -1.0f) * kHalfPi;
        clamped = true;
    }
    float amount = d / kHalfPi;
    if (!init) amount = std::clamp(amount, prev - rate, prev + rate);
    const bool keep = clamped && std::abs(d) > kHalfPi && std::abs(prev) < 1.0f - rate;
    if (!keep && (want ? amount > 0 : amount < 0)) amount = 0;
    const auto a = ctx.attachedMarkup();
    if (!a) {
        ctx.setLayerBlend(0, 0);
        return;
    }
    float l = 0;
    if (const auto s = ctx.markupSegment(a.markup, roundI(std::floor(a.param)))) {
        const glm::vec3 n = glm::length(s->second - s->first) > 0 ? glm::normalize(s->second - s->first) : glm::vec3(0);
        glm::vec2 fl(n);
        fl = glm::length(fl) > 0 ? glm::normalize(fl) : fl;
        const glm::vec2 f(std::cos(ctx.heading()), std::sin(ctx.heading()));
        l = std::clamp(-std::asin(std::clamp(n.z * glm::dot(fl, f), -1.0f, 1.0f)) / 0.52359879f, -1.0f, 1.0f);
    }
    if (!init) l = std::clamp(l, lean - 1.0f, lean + 1.0f);
    const auto type = ctx.layerBlendType();
    if (!type) {
        ++rt.stats.unportedActions[205]; // no anim layer blend to drive
        walk = amount;
        return;
    }
    if (*type == 2) {
        walk = amount;
        ctx.setLayerBlend(std::abs(amount), 0);
        if (want != mirror) rt.setMirror(want);
    } else if (*type == 7 || *type == 5) {
        walk = amount, lean = l;
        ctx.setLayerBlend(l, std::abs(amount));
        if (*type == 5 && want != mirror) rt.setMirror(want);
    }
}

// Action 205 enter 0x75D600: 0x72A5F0, then 0x743900 with init (beamBalance). Update: 0x743900 (beamBalance). Exit
// 0x754F60 clears the blend.
void beamSteerEnter(ChartContext& ctx, uint16_t) {
    beamFacing(ctx);
    ctx.animBlend = glm::vec2(0);
    beamBalance(ctx, true);
}
void beamSteerUpdate(ChartContext& ctx, uint16_t) { beamBalance(ctx, false); }
void clearBlend(ChartContext& ctx, uint16_t) { ctx.animBlend = glm::vec2(0); }

// Action 303 update 0x75D630 -> 0x72A8F0: on a markup riding a live instance, 0x72A5F0; when the beam angle changed,
// the segment's horizontal direction -> player +0xCB0.
void beamTrack(ChartContext& ctx, uint16_t) {
    const auto a = ctx.attachedMarkup();
    if (!a || !a.live || !beamFacing(ctx)) return;
    if (const auto s = ctx.markupSegment(a.markup, int(std::floor(a.param)))) {
        glm::vec2 d = glm::vec2(s->second - s->first);
        if (glm::length(d) > 0) d = glm::normalize(d);
        ctx.playerFloat(0xCB0) = d.x, ctx.playerFloat(0xCB4) = d.y, ctx.playerFloat(0xCB8) = 0;
    }
}

// Action 404: entry 0x770EB0 clears agent +0xF0's low six bits and probes, update 0x770BA0 probes, exit 0x75D9C0
// clears them.
void contactEnter(ChartContext& ctx, uint16_t) {
    ctx.chart().field(0xF0) &= ~0x3F;
    markupContactProbe(ctx);
}
void contactUpdate(ChartContext& ctx, uint16_t) { markupContactProbe(ctx); }
void contactExit(ChartContext& ctx, uint16_t) { ctx.chart().field(0xF0) &= ~0x3F; }

// Action 408 (0x6457A0 in every slot): the stripped debug print.
void noop(ChartContext&, uint16_t) {}

// Action 506 update 0x753110: the layers' rate matching property 2000 (linear speed) or 2001 (turn speed, degrees)
// to the clip's root motion, clamped to 0.1..10; neither: 1. [U] the clip's root motion is not visible to the chart:
// with either property set the action counts as unported.
void matchRate(ChartContext& ctx, uint16_t id) {
    if (propF(ctx, 2000) != 0 || propF(ctx, 2001) != 0) return unportedSlot(ctx, id);
    ctx.animRate = 1;
}

// Action 513 entry 0x73DFF0: what lies past the hurdle, from the attached point along her way over (-normal), into
// player +0x6C (-1 first).
void hurdleFarSide(ChartContext& ctx, uint16_t id) {
    ctx.playerInt(0x6C) = -1;
    const auto a = ctx.attachedMarkup();
    if (!a) return;
    const glm::vec2 f = glm::length(glm::vec2(a.normal)) > 0 ? -glm::normalize(glm::vec2(a.normal)) : glm::vec2(0);
    if (const auto cls = ctx.hurdlePastClass(a.point, f)) ctx.playerInt(0x6C) = *cls;
    else unportedSlot(ctx, id);
}

// Action 514 preenter 0x7715E0 -> 0x6FD900: the secondary jump-target search block (player +0xB10, 0x180 bytes)
// becomes the primary one (+0x980).
void promoteSearch(ChartContext& ctx, uint16_t) {
    for (int off = 0; off < 0x180; off += 4) {
        ctx.playerInt(0x980 + off) = ctx.playerInt(0xB10 + off);
        ctx.playerFloat(0x980 + off) = ctx.playerFloat(0xB10 + off);
    }
}

// Action 516: entry player +0xA90 = the ms timer (0xE7F15C), exit +0xA98 = it.
void jumpStart(ChartContext& ctx, uint16_t) { ctx.playerInt(0xA90) = int(ctx.gameMs()); }
void jumpEnd(ChartContext& ctx, uint16_t) { ctx.playerInt(0xA98) = int(ctx.gameMs()); }

// The turn system's mode change 0x6ED910 (player +0x238; +0x80 1 for modes 2 / 6 / 8; mode 1 / 13 zero the rate
// +0x240 and keep the heading in +0x234).
void turnMode(ChartContext& ctx, int m) {
    ctx.playerInt(0x23C) = 0;
    if (ctx.playerInt(0x238) == m) return;
    ctx.playerInt(0x80) = (m == 2 || m == 6 || m == 8) ? 1 : 0;
    if (m == 1 || m == 13) ctx.playerFloat(0x240) = 0, ctx.playerFloat(0x234) = ctx.heading();
    ctx.playerInt(0x238) = m;
}
// 0x75B330: the turn rate cap: property 601 (exact = true) or the lesser of 601 and property 605 (default: the
// global tuning +0x234, [U] read as 0 here).
float turnRate(ChartContext& ctx, bool exact) {
    const float p605 = propF(ctx, 605), p601 = propF(ctx, 601);
    if (exact) return p601;
    return p601 > 0 && p601 < p605 ? p601 : p605;
}
// 0x76AB70: a timed turn to heading h: frames = (blend frames with 602) + (frames of the clip with 603) - 2, at least
// 1; agent +0x28 = h, delta +0x248 = wrap(h - heading - clip root yaw) (608: the way round toward point +0x800), rate
// +0x240 = |delta| / frames / 2pi x 4096 capped by property 601; mode 10. [I] the clip's root yaw is not visible to
// the chart (0); its blend frames are the request's fade.
void timedTurn(ChartContext& ctx, float h, bool blend, bool clip, bool toward) {
    const ChartRuntime& rt = ctx.chart();
    float frames = 0;
    const int r = rt.request();
    if (blend && r >= 0 && size_t(r) < rt.charts().requests.size()) frames += float(rt.charts().requests[size_t(r)].blend);
    if (clip) frames += rt.animLengthFrames();
    frames = std::max(1.0f, frames - 2.0f);
    ctx.agentFloat(0x28) = h;
    float delta = wrapPi(h - ctx.heading());
    if (toward) {
        const glm::vec2 p(ctx.playerFloat(0x800), ctx.playerFloat(0x804));
        const float side = wrapPi(yawOf(p - glm::vec2(ctx.position())) - ctx.heading());
        if (side < 0 && delta > 0) delta -= kTwoPi;
        else if (side > 0 && delta < 0) delta += kTwoPi;
    }
    float rate = std::abs(delta / frames) / kTwoPi * 4096.0f;
    const float cap = turnRate(ctx, true);
    if (cap > 0 && cap < rate) rate = cap;
    ctx.playerFloat(0x240) = rate;
    ctx.playerFloat(0x248) = delta;
    turnMode(ctx, 10);
}

// Action 606 entry 0x76B210: without property 602 (default on) and with a clip turning on its own [U: 0x433D10, read
// as false]: delta pi, mode 0; else a timed turn to the jump heading player +0xA60 + property 607 degrees (602 / 603
// / 608). Reentry 0x76B350 only when +0xA60 moved off agent +0x28. Exit 0x75B780 snaps her heading to agent +0x28
// (recorded: player float -0x38). Update 0x76B380 steps toward +0xA7C + 607 (0x76A860, recorded as the target).
void turnToJumpEnter(ChartContext& ctx, uint16_t) {
    const bool p602 = propB(ctx, 602, true), p603 = propB(ctx, 603, true), p608 = propB(ctx, 608);
    timedTurn(ctx, wrapPi(ctx.playerFloat(0xA60) + glm::radians(propF(ctx, 607))), p602, p603, p608);
}
void turnToJumpReenter(ChartContext& ctx, uint16_t id) {
    if (ctx.playerFloat(0xA60) != ctx.agentFloat(0x28)) turnToJumpEnter(ctx, id);
}
void turnToJumpExit(ChartContext& ctx, uint16_t) {
    if (ctx.agentFloat(0x28) != ctx.heading()) ctx.playerFloat(-0x38) = ctx.agentFloat(0x28);
}
void turnToJumpUpdate(ChartContext& ctx, uint16_t id) {
    const bool p602 = propB(ctx, 602, true), p603 = propB(ctx, 603, true);
    constexpr bool rootTurns = false; // [U] 0x433D10
    if ((!p602 && rootTurns) || (!p603 && !rootTurns)) return;
    if (!p602 && p603 && ctx.playerFloat(0x248) != 0 && ctx.playerInt(0x238) == 0) return turnToJumpEnter(ctx, id);
    ctx.agentFloat(0x2C) = wrapPi(ctx.playerFloat(0xA7C) + glm::radians(propF(ctx, 607))); // 0x76A860's target
}

// Action 613 entry 0x75B720 (and every update before 0x76A860 steps toward the stick's control yaw): no attach
// surface class (player +0xED4 == 0): mode 0; else start yaw +0x244 = heading, mode 14, rate +0x240 = 0x75B330.
// [U] +0x23C (0x75B3E0) is not ported.
void turnToStickEnter(ChartContext& ctx, uint16_t) {
    if (ctx.playerInt(0xED4) == 0) return turnMode(ctx, 0);
    ctx.playerFloat(0x244) = ctx.heading();
    turnMode(ctx, 14);
    ctx.playerFloat(0x240) = turnRate(ctx, false);
}
void turnToStickUpdate(ChartContext& ctx, uint16_t id) {
    turnToStickEnter(ctx, id);
    ctx.agentFloat(0x2C) = ctx.controlYaw();
}

// Actions 2003 / 2004: CameraManager 0 vfunc +0xB0 (4 / 5) on entry, +0xB4 on exit.
void camera4(ChartContext& ctx, uint16_t) { ctx.cameraMode = 4; }
void camera5(ChartContext& ctx, uint16_t) { ctx.cameraMode = 5; }
void cameraPop(ChartContext& ctx, uint16_t) { ctx.cameraMode = -1; }

} // namespace

void beamBalanceTick(ChartContext& ctx, bool init) { beamBalance(ctx, init); }

void registerLedge(Registry& player, Registry&) {
    auto& r = player;
    const std::string f = "ledge";
    r.condition(0x15, agentFloat374, "PSCCF_AgentFloat374AtLeast", f);
    r.condition(0x20, verticalClearance, "PSCCF_VerticalClearance", f);
    r.condition(0xD0, blendAbs, "SCCond_AnimLayerAbs8C", f);
    r.condition(0xD1, blendAbs, "SCCF_AnimSectionAbs90AtMost", f);
    r.condition(0xD6, animTime, "SCCond_AnimTimeLE", f);
    r.condition(0xD7, animTime, "SCCondFunction_AnimFramesRemaining_AtMost", f);
    r.condition(0xD8, animTime, "SCCF_AnimProgressPercentAtLeast", f);
    r.condition(0x195, stickDir, "PSCCF_StickDirClass", f);
    r.condition(0x199, moveDirUnbalanced, "PSCCF_MoveDirUnbalanced", f);
    r.condition(0x19B, moveDirJumpTarget, "PSCCF_MoveDirJumpTarget", f);
    r.condition(0x19C, markupAngleJumpTarget, "PSCCF_MarkupAngle (jump target)", f);
    r.condition(0x1D1, hurdleType, "PSCCF_HurdleType", f);
    r.condition(0x1FF, flag3E4, "PSCCF_Flag3E4", f);
    r.condition(0x200, flag3BD, "PSCCF_Flag3BD", f);
    r.condition(0x201, onGround, "PSCCF_OnGround", f);
    r.condition(0x213, contextualAllowed, "PSCCF_ScriptContextualAnimAllowed", f);
    r.condition(0x263, attachedClass, "PSCCF_Markup_Modifier", f);
    r.condition(0x26C, attachedBits, "PSCCF_Markup_MoveDir (attached)", f);
    r.condition(0x280, lookahead, "PSCCF_Markup_LookaheadResult", f);
    r.condition(0x281, markupDistance, "PSCCF_MarkupClearance_AtLeast", f);
    r.condition(0x284, markupDistance, "PSCCF_Markup_ClearanceResult (+0xF8)", f);
    r.condition(0x28A, clearanceClass, "PSCCF_Markup_ClearanceResult", f);
    r.condition(0x2B4, segmentShort, "PSCCF_Markup_SegmentLengthLessThan", f);
    r.condition(0x3EB, stateFlag1003, "PSCCF_StateFlag1003", f);
    r.condition(0x960, throttledEffect, "PSCCF_ThrottledEffectTrigger", f);
    // 0x264 (0x7641D0, the type-27 drop-down markup query 0x7296C0) is not ported: no pure markup-query port yet.

    r.action(205, {beamSteerEnter, nullptr, nullptr, nullptr, clearBlend, beamSteerUpdate}, "PSCAF_BeamBalanceSteerBlend", f);
    r.action(213, {nullptr, nullptr, hurdleOverBlend, hurdleOverBlend, nullptr, nullptr}, "PSCAF_HurdleOverBlendParams", f);
    r.action(232, {nullptr, nullptr, hurdleReattach, hurdleReattach, nullptr, nullptr}, "PSCAF_HurdleReattachToJumpTarget", f);
    r.action(301, {unportedSlot, unportedSlot, nullptr, nullptr, unportedSlot, unportedSlot}, "PSCAF_BalanceWobble", f);
    r.action(303, {nullptr, nullptr, nullptr, nullptr, nullptr, beamTrack}, "PSCAF_BeamDirectionTrack", f);
    r.action(404, {contactEnter, nullptr, nullptr, nullptr, contactExit, contactUpdate}, "PSCAF_MarkupContactProbeFlags", f);
    r.action(408, {noop, nullptr, nullptr, nullptr, noop, noop}, "PSCAF_Noop408", f);
    r.action(506, {nullptr, nullptr, nullptr, nullptr, nullptr, matchRate}, "PSCAF_MatchAnimRateToSpeed", f);
    r.action(513, {hurdleFarSide, nullptr, nullptr, nullptr, nullptr, nullptr}, "PSCAF_HurdleFarSideProbe", f);
    r.action(514, {nullptr, nullptr, promoteSearch, nullptr, nullptr, nullptr}, "PSCAF_PromoteSecondarySearch", f);
    r.action(516, {jumpStart, nullptr, nullptr, nullptr, jumpEnd, nullptr}, "PSCAF_JumpTimestamps", f);
    r.action(606, {turnToJumpEnter, turnToJumpReenter, nullptr, nullptr, turnToJumpExit, turnToJumpUpdate}, "PSCAF_TurnToJumpHeading", f);
    r.action(613, {turnToStickEnter, nullptr, nullptr, nullptr, nullptr, turnToStickUpdate}, "PSCAF_TurnToStickOnAttach", f);
    r.action(2003, {camera4, nullptr, nullptr, nullptr, cameraPop, noop}, "PSCAF_CameraMode4", f);
    r.action(2004, {camera5, nullptr, nullptr, nullptr, cameraPop, noop}, "PSCAF_CameraMode5", f);
}

} // namespace ou::game::chart
