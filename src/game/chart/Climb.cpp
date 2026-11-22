// Group CD: Lara's climb / ladder / pillar / perch / pole / beam / wall hug / slide / climb-out conditions and actions,
// and the shared gates every ground exit tests (docs/re/PLAYER_CHART.md "Conditions", "Actions"; ownership
// openunderworld-work/chart/{conditions,actions}_owner.json by_group.CD). Behaviour after tru.exe, one function per
// registry entry, the decompiled tests in their order. A condition returns the raw truth (the runtime applies the
// negation; the exceptions are noted per function and folded back here); where the game cannot answer it returns
// ctx.chart().unported(c). Actions record their effect chart-side (ChartContext player / agent stores); shadow mode
// never applies them to the live Player.
#include "game/Chart.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>

namespace ou::game::chart {

namespace {

// 0x781FF0: a condition's mode; group 1000 makes it 1000 (no case matches it).
int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; }

// A function that applies the negation itself and returns a value the runtime must not negate again: hand back what
// the runtime's negation turns into `result`.
bool raw(const Cond& c, bool result) { return c.negate() ? !result : result; }

constexpr float kPi = glm::pi<float>(), kTwoPi = glm::two_pi<float>();

// 0x782B40: the value (a variable's for group 1000) against x: mode 1 value <= x, 2 value >= x, else ==.
bool cmpInt(ChartContext& ctx, const Cond& c, int x, int m) {
    const int v = ctx.chart().value(c);
    return m == 1 ? v <= x : m == 2 ? v >= x : v == x;
}
// 0x783BD0: the same on floats (0x782060), mode 0 within 1e-8.
bool cmpFloat(ChartContext& ctx, const Cond& c, float x, int m) {
    const float v = ctx.chart().valueF(c);
    return m == 1 ? v <= x : m == 2 ? v >= x : std::abs(x - v) <= 1e-8f;
}
// 0x431D70: an angle wrapped to (-pi, pi].
float wrapPi(float a) {
    a = std::remainder(a, kTwoPi);
    return a <= -kPi ? a + kTwoPi : a;
}
float yawOf(glm::vec2 d) { return std::atan2(d.y, d.x); } // ours: forward = (cos, sin)
float propF(ChartContext& ctx, uint16_t id, float def = 0) { return ctx.chart().propFloat(id).value_or(def); }
bool propB(ChartContext& ctx, uint16_t id, bool def = false) { return ctx.chart().propBool(id).value_or(def); }
void unportedSlot(ChartContext& ctx, uint16_t id) { ++ctx.chart().stats.unportedActions[id]; }

// 0x782D70 / the edge-coded bools: cache = (previous & 1) << 1 | now, once per update (+0x50C). 0 !now, 1 now,
// 2 just turned off, 3 just turned on; others false. [I] a fresh cache takes the previous as 0 (0x7660D0 keeps
// whatever the node held; the node starts zeroed).
bool edge(ChartContext& ctx, const Cond& c, bool now) {
    auto& k = ctx.chart().cache(c.id);
    if (k.frame != ctx.chart().frame()) {
        k.value = k.frame == ~0u ? int(now) : ((k.value & 1) << 1) | int(now);
        k.frame = ctx.chart().frame();
    }
    const int bits = k.value & 3;
    switch (mode(c)) {
    case 0: return !(bits & 1);
    case 1: return bits & 1;
    case 2: return bits == 2;
    case 3: return bits == 1;
    default: return raw(c, false);
    }
}

// 0x785FB0: the MoveDir sector of an angle: bounds = float properties 501..504 (degrees, wrapped), the last sector
// (agent +0x70, 0 = 1) widened by property 507; mirrored with property 207 negates the angle. 1 front, 4 +side, 8
// -side, else 2; stored to +0x70. [I] property 508 (gear-instance bounds) not modelled.
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
int sectorOf(int v) { return v == 410 ? 1 : v == 411 ? 2 : v == 412 ? 4 : v == 413 ? 8 : 0; }

// The stick's direction relative to her heading, radians (+ = her left; 0 at rest).
float stickAngle(ChartContext& ctx) {
    const glm::vec2 v = ctx.stick();
    if (glm::dot(v, v) < 1e-12f) return 0.0f;
    return wrapPi(yawOf(v) - ctx.heading());
}

// ---- gates ----------------------------------------------------------------------------------------------------------

// 0xCA (0x7606A0, SCCondFunction_AnimationCriticalSection), cached per update: bit 0 = player +0x78 (the chart clip's
// critical section, anim events 0x13 / 0x14), bit 1 = agent +0x6C (+0x78 as the last chart tick left it, 0x768C50).
// 0 !bit0, 1 bit0, 2 bit1 && !bit0 (just left), 3 bit0 && !bit1 (just entered); others false ("unhandled").
bool criticalSection(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    if (!ctx.tracksCriticalSection()) return rt.unported(c);
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) cache = {rt.frame(), (ctx.playerInt(0x78) != 0 ? 1 : 0) | (rt.field(0x6C) != 0 ? 2 : 0)};
    const int b = cache.value;
    switch (mode(c)) {
    case 0: return !(b & 1);
    case 1: return b & 1;
    case 2: return (b & 3) == 2;
    case 3: return (b & 3) == 1;
    default: return false;
    }
}

// 0x1F5 (0x7824F0, SCCondFunction_InWater, base table), cached per update: f = owner message 9 (player +0x8EC). f == 0:
// false whatever the negation. 0: f & 0x400 (dry); 1: !(f & 0x400) (in water); 2: f & 0x3000; 3: f & 0x4800; others
// false whatever the negation. [O: the catalog's "1 = f & 0x400" reads the branch the wrong way round; 0x782585 returns
// !negate when case 1 finds 0x400 clear.]
bool inWater(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto f = ctx.waterFlags();
        if (!f) return rt.unported(c);
        cache = {rt.frame(), int(*f)};
    }
    const uint32_t f = uint32_t(cache.value);
    if (f == 0) return raw(c, false);
    switch (mode(c)) {
    case 0: return f & 0x400;
    case 1: return !(f & 0x400);
    case 2: return f & 0x3000;
    case 3: return f & 0x4800;
    default: return raw(c, false);
    }
}

// 0x204 (0x763540 -> 0x7370A0(agent +0x3F0), registry string "PSCCF_JustLanded" misattributed): a ledge to mantle onto
// ahead, cached per update. lara.drm player data: +0xDAC low ray height (100), +0xDA8 its length (150), +0xDB0 high ray
// height (400), +0xD9C..+0xDA4 a vector whose length (130) the high ray adds, +0xDB4 the steepest top (35 degrees),
// +0xDB8 accepted hit kinds (bit 0 static, bit 1 instance; 0 in lara.drm: only an instance's still physics body).
// On success +0x400 = the top point, +0x3F0 = it pulled back along her heading by |+0xD9C| (0x432190 on that vector).
// Ray flags 0x442C11E3 (0x42C11AB with 0xE7F1AC, 0x2C19E7 with 0xE7F14D, neither set on this path [I]).
std::optional<bool> mantleProbe(ChartContext& ctx) {
    const Charts& d = ctx.chart().charts();
    constexpr uint32_t kFlags = 0x442C11E3;
    const glm::vec3 pos = ctx.position();
    const float h = ctx.heading();
    const glm::vec3 fwd(std::cos(h), std::sin(h), 0.0f); // 0x49F5D0 / 0x49D270 of instance +0x38
    const float lowZ = d.data<float>(0xDAC), highZ = d.data<float>(0xDB0);
    const glm::vec3 a1 = pos + glm::vec3(0, 0, lowZ), b1 = a1 + d.data<float>(0xDA8) * fwd;
    const auto r1 = ctx.rayHit(a1, b1, kFlags);
    if (!r1) return false;
    float reach = 0;
    const glm::vec3 to = r1->point - a1;
    if (glm::dot(to, to) > 1e-12f) reach = glm::length(to); // 0x49AFA0: a zero vector keeps 0
    const glm::vec3 v(d.data<float>(0xD9C), d.data<float>(0xDA0), d.data<float>(0xDA4));
    const float back = glm::length(v);
    reach += back;
    const glm::vec3 a2 = pos + glm::vec3(0, 0, highZ), b2 = a2 + reach * fwd;
    if (ctx.rayHit(a2, b2, kFlags)) return false;
    const glm::vec3 b3 = b2 - glm::vec3(0, 0, highZ - lowZ);
    const auto r3 = ctx.rayHit(b2, b3, kFlags);
    const int kind = r3 ? r3->kind : 0;
    const uint16_t accept = d.data<uint16_t>(0xDB8);
    bool ok = (accept & 1) && kind == 1;
    bool skip = false; // the instance branch's goto past the acceptance test (0x7374FF)
    if (!(accept & 2)) {
        if (kind == 2) {
            if (!r3->stillBody) return std::nullopt;
            skip = *r3->stillBody;
        }
    } else {
        ok = ok || kind == 2;
    }
    if (!skip && !ok) return false;
    if (kind == 2) {
        if (!r3->flag8000000) return std::nullopt;
        if (*r3->flag8000000) return false;
    }
    if (!(std::cos(d.data<float>(0xDB4) * 0.017453292f) < r3->normal.z)) return false;
    ctx.agentFloat(0x400) = r3->point.x, ctx.agentFloat(0x404) = r3->point.y, ctx.agentFloat(0x408) = r3->point.z;
    const glm::vec3 p = r3->point - back * fwd;
    ctx.agentFloat(0x3F0) = p.x, ctx.agentFloat(0x3F4) = p.y, ctx.agentFloat(0x3F8) = p.z;
    return true;
}
bool mantleLedgeAhead(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        if (!ctx.hasCollision()) return rt.unported(c);
        const auto b = mantleProbe(ctx);
        if (!b) return rt.unported(c);
        cache = {rt.frame(), int(*b)};
    }
    switch (mode(c)) {
    case 0: return !cache.value;
    case 1: return cache.value;
    default: return false;
    }
}

// 0x836 (0x765D20): (player +0x500) +0x134 == the value (0x7820E0: group 1000 reads the variable).
bool gearSlotIs(ChartContext& ctx, const Cond& c) {
    const auto slot = ctx.gearSlot();
    if (!slot) return ctx.chart().unported(c);
    return *slot == ctx.chart().value(c);
}

// ---- animation section ----------------------------------------------------------------------------------------------

// 0xCB (0x7821E0 -> 0x433D10), cached: the chart's anim section is crossfading into its new clip (+0x58 bit 9, blend
// record +0x70 / +0x7C set). [I] ours: the state's pattern has a blend and the state is younger than it (the clip
// starts on the change, 0x783470). 0 !t, 1 t; others false. Without a pattern (none, or kind 2) it cannot say.
bool animTransitioning(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const Request* p = rt.pattern();
    if (!p) return rt.unported(c);
    const bool t = p->blend > 0 && rt.stateTime() < float(p->blend);
    switch (mode(c)) {
    case 0: return !t;
    case 1: return t;
    default: return raw(c, false);
    }
}

// 0xCE (0x782420): the chart's anim section plays mirrored (+0xF4 bit 0x20: transition flags 2 / 5). 0 / 1.
bool animMirrored(ChartContext& ctx, const Cond& c) {
    const bool m = ctx.chart().mirrored();
    switch (mode(c)) {
    case 0: return !m;
    case 1: return m;
    default: return false;
    }
}

// ---- markup ---------------------------------------------------------------------------------------------------------

// 0x196 (0x7642C0), cached: c = 0 without an attached markup (+0xC94); with one whose flags +0x28 have 0x1000000 or
// pass 0x733F60 (markupLevel: flags & 0xA000000, first segment within 40 degrees of level), 0x733210: param +0xC98 clamped
// under the segment count, the segment at round(param) s (normalised), heading hv; dn = -n.hv, ds = s.hv; |dn| > |ds|:
// 1 (dn >= 0) or 2, else 4 (ds >= 0) or 3. 450..454 test 1, 2, 3, 4, 0; others false.
bool markupFacing(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& k = rt.cache(c.id);
    if (k.frame != rt.frame()) {
        const auto m = ctx.attachedMarkup();
        int f = 0;
        if (m) {
            if (!(m.flags & 0x1000000) && !markupLevel(ctx, m)) { // 0x733F60 (Chart.cpp markupLevel)
                k = {rt.frame(), 0};
                return mode(c) == 454;
            }
            const int n = ctx.markupSegmentCount(m.markup);
            float param = m.param;
            if (float(n) <= param) param = float(n) - 0.0001f;
            const auto seg = ctx.markupSegment(m.markup, int(std::nearbyint(param)));
            if (!seg) return rt.unported(c);
            glm::vec2 s = glm::vec2(seg->second - seg->first);
            if (glm::length(s) > 0) s = glm::normalize(s);
            const glm::vec2 hv(std::cos(ctx.heading()), std::sin(ctx.heading()));
            const float dn = -glm::dot(glm::vec2(m.normal), hv), ds = glm::dot(s, hv);
            f = std::abs(dn) <= std::abs(ds) ? (ds >= 0 ? 4 : 3) : (dn >= 0 ? 1 : 2);
        }
        k = {rt.frame(), f};
    }
    static constexpr int kWant[] = {1, 2, 3, 4, 0};
    const int v = mode(c) - 450;
    return v >= 0 && v < 5 && k.value == kWant[v];
}

// 0x76EB10 with her heading (instance +0x38): the slot's segment at round(param) against the heading, folded to
// +-90 degrees unless the value is 413, then 0x785FB0. Without the slot it returns the negation bit (false after the
// runtime's negation, raw true when negated... i.e. the final truth is the negation bit).
bool markupAngle(ChartContext& ctx, const Cond& c, const ChartContext::MarkupSlot& m) {
    if (!m) return raw(c, c.negate());
    const float h = ctx.heading();
    float edgeYaw = h;
    if (const auto seg = ctx.markupSegment(m.markup, int(std::nearbyint(m.param))); seg && glm::length(glm::vec2(seg->second - seg->first)) > 1e-4f)
        edgeYaw = yawOf(glm::vec2(seg->second - seg->first));
    float d = wrapPi(h - edgeYaw);
    if (mode(c) != 413 && std::abs(d) > kPi / 2) d = wrapPi(d + kPi);
    return moveDirSector(ctx, d) == sectorOf(mode(c));
}
// 0x1A8 (0x76ECC0): the jump target (+0xA64, +0xA68).
bool markupAngleJumpTarget(ChartContext& ctx, const Cond& c) { return markupAngle(ctx, c, chartJumpTarget(ctx)); }
// 0x1AC (0x76ECF0): the attached markup (+0xC94, +0xC98).
bool markupAngleAttached(ChartContext& ctx, const Cond& c) { return markupAngle(ctx, c, ctx.attachedMarkup()); }

// 0x1A9 / 0x1AA (0x761C50): the stick angle in degrees against the value: 0x1A9 value <= a, 0x1AA value >= a.
bool stickAngleRange(ChartContext& ctx, const Cond& c) {
    const float a = stickAngle(ctx) * 57.29578f;
    return cmpFloat(ctx, c, a, c.id == 0x1A9 ? 1 : 2);
}

// 0x1F6 (0x762C60, the same function as 0x1F8): instance +0x14 bit 19 (0x5DCE20 found no floor). 0 / 1.
bool isFalling(ChartContext& ctx, const Cond& c) {
    const auto b = ctx.noFloor();
    if (!b) return ctx.chart().unported(c);
    switch (mode(c)) {
    case 0: return !*b;
    case 1: return *b;
    default: return false;
    }
}

// 0x210 (0x765C90): agent byte +0x3ED (0x76E790's head ray, mask 0x3E11E7) [U its caller]: value 0 -> c == 0, else c;
// the negation is not applied.
bool agentFlag3ED(ChartContext& ctx, const Cond& c) {
    const int b = ctx.chart().field(0x3ED);
    return raw(c, mode(c) == 0 ? b == 0 : b != 0);
}

// 0x219 (0x762E60): float property 915 (0x393, 0x6EEED0 mode 2) <= agent float +0x4B4 (action 573's timer); value and
// negation ignored. [I] the property in force of Lara's chart (0x6EEED0 picks the upper chart when hers has no request).
bool prop915Elapsed(ChartContext& ctx, const Cond& c) { return raw(c, propF(ctx, 0x393) <= ctx.agentFloat(0x4B4)); }

// 0x21C (0x7660D0): agent u32 +0x4EC (wall jumps done) < lara.drm u8 +0x843 (5), edge-coded.
bool wallJumpAllowed(ChartContext& ctx, const Cond& c) {
    return edge(ctx, c, uint32_t(ctx.chart().field(0x4EC)) < ctx.chart().charts().data<uint8_t>(0x843));
}

// 0x282 / 0x283 (0x7627E0 / 0x762830): with agent byte +0xA0 set, agent float +0x9C (the corner angle, degrees)
// against the value: 0x282 value <= x, 0x283 value >= x; +0xA0 clear: false whatever the negation.
bool cornerAngle(ChartContext& ctx, const Cond& c) {
    if (!ctx.chart().field(0xA0)) return raw(c, false);
    return cmpFloat(ctx, c, ctx.agentFloat(0x9C), c.id == 0x282 ? 1 : 2);
}

// 0x29F / 0x2A0 (0x764AA0 -> 0x733E00), cached: the angle between her heading and (root bone - attach point +0xCA0),
// 2pi - it when that vector points down, degrees; 0x29F value <= a, 0x2A0 value >= a. [I] heading vector horizontal.
// [U] the same-update cached path compares the rounded int; ours compares the float each time.
bool attachAngle(ChartContext& ctx, const Cond& c) {
    const auto m = ctx.attachedMarkup();
    const glm::vec3 v = ctx.rootBone() - m.point;
    const glm::vec3 hv(std::cos(ctx.heading()), std::sin(ctx.heading()), 0);
    float a = 0;
    if (glm::length(v) > 0) a = std::acos(std::clamp(glm::dot(hv, glm::normalize(v)), -1.0f, 1.0f));
    if (v.z < 0) a = kTwoPi - a;
    return cmpFloat(ctx, c, a * 57.29578f, c.id == 0x29F ? 1 : c.id == 0x2A0 ? 2 : 0);
}

// 0x2B0 (0x764D80): a jump target and player float +0x77C (its time to go) <= timeMult * 0.5. 0 / 1.
bool jumpTargetReached(ChartContext& ctx, const Cond& c) {
    const bool b = chartJumpTarget(ctx) && ctx.playerFloat(0x77C) <= ctx.timeMult * 0.5f;
    switch (mode(c)) {
    case 0: return !b;
    case 1: return b;
    default: return false;
    }
}

// 0x2B1 (0x765CE0): an attached markup riding a live instance (0x5BE2D0). Value ignored.
bool markupLinked(ChartContext& ctx, const Cond&) {
    const auto m = ctx.attachedMarkup();
    return m && m.live;
}

// 0x2B2 (0x763F10): jump target (+0xA64) == attached markup (+0xC94) (both absent counts as equal), edge-coded.
bool jumpTargetIsAttached(ChartContext& ctx, const Cond& c) {
    return edge(ctx, c, chartJumpTarget(ctx).markup == ctx.attachedMarkup().markup);
}

// 0x2B5 (0x762D20 -> 0x731EA0), cached: an attached markup and its param within 0.05 of round(param) or round + 1.
bool atMarkupVertex(ChartContext& ctx, const Cond&) {
    const auto m = ctx.attachedMarkup();
    if (!m) return false;
    const float r = std::nearbyint(m.param);
    return std::abs(m.param - r) <= 0.05f || std::abs(m.param - (r + 1)) <= 0.05f;
}

// ---- water ----------------------------------------------------------------------------------------------------------

// 0x764340 / 0x764550 depth classes: depth = -(player +0x914) + waves (ChartContext::water); 805 >= +0xB2 (948), 804 >=
// +0xB0, 803 >= +0xAE (391), 802 >= +0xAC (154), 801 > 0, else 800.
int depthClass(const Charts& d, float depth) {
    if (depth >= float(d.data<int16_t>(0xB2))) return 805;
    if (depth >= float(d.data<int16_t>(0xB0))) return 804;
    if (depth >= float(d.data<int16_t>(0xAE))) return 803;
    if (depth >= float(d.data<int16_t>(0xAC))) return 802;
    return depth > 0 ? 801 : 800;
}
// 0x321 (0x764340, id 0x321): class == value (mode 0).
bool waterDepthIs(ChartContext& ctx, const Cond& c) {
    const auto w = ctx.water();
    if (!w) return ctx.chart().unported(c);
    return cmpInt(ctx, c, w->inWater ? depthClass(ctx.chart().charts(), w->depth) : 800, 0);
}
// 0x324 / 0x326 (0x764550): player +0x918 & 0x400 -> 800 (ours: no water block); 805 also needs -(player +0x910) >=
// +0xB2; 0x324 class == value, 0x326 value <= class. Bool property 1901 (0x76D) with value 805, mode 0 / 1 and agent byte
// +0x3ED set: true. The class is only asked for as far as the compare needs it (805 vs 804 needs +0x910).
bool swimDepthClass(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const int m = c.id == 0x324 ? 0 : 1;
    if (propB(ctx, 0x76D) && rt.value(c) == 805 && rt.field(0x3ED)) return raw(c, true);
    const auto w = ctx.water();
    if (!w) return rt.unported(c);
    int k = w->inWater ? depthClass(rt.charts(), w->depth) : 800;
    if (k == 805) {
        const auto s = ctx.waterFloorDepth();
        if (s) k = *s >= float(rt.charts().data<int16_t>(0xB2)) ? 805 : 804;
        else if (!(m == 1 && rt.value(c) <= 804)) return rt.unported(c);
        else k = 804; // 804 or 805: both pass value <= 804
    }
    return cmpInt(ctx, c, k, m);
}

// 0x32E (0x7644F0): agent byte +0x4A6 (action 209's beam-side blend, 0x756700). Value unused.
bool agentFlag4A6(ChartContext& ctx, const Cond&) { return ctx.chart().field(0x4A6) != 0; }

// 0x415 (0x765550): the camera looks against her: dot(her forward, camera direction) < 0; value and negation ignored.
// [I] ours: the horizontal control yaw (0x7566B0 / 0x703730's override direction not modelled).
bool cameraBehind(ChartContext& ctx, const Cond& c) {
    const float d = std::cos(ctx.heading() - ctx.controlYaw());
    return raw(c, d < 0);
}

// 0x5AE (0x72CBF0): (float) the deadline 0x8B083C (action 1409) < game time (0xE7F158; ours gameMs). 0 / 1.
bool deadlinePast(ChartContext& ctx, const Cond& c) {
    const bool b = ctx.deadline8B083C < float(ctx.gameMs());
    switch (mode(c)) {
    case 0: return !b;
    case 1: return b;
    default: return false;
    }
}

// ---- actions --------------------------------------------------------------------------------------------------------

// 212 (0x75CC90 / 0x75CCA0): enter zeroes the anim section's blend parameters +0x8C / +0x90. The update steers them by
// the raw input axes 0 / 2 / 3 (0x754BF0), which ours does not have: counted unported.
void beamStickBlendEnter(ChartContext& ctx, uint16_t) { ctx.animBlend = glm::vec2(0); }
// 214 (0x755100 / 0x7550D0): the second blend slot +0x3AC / +0x3B0 cleared on entry and exit, = +0x8C / +0x90 each tick.
void latchBlendClear(ChartContext& ctx, uint16_t) { ctx.blendLatch = {0, 0}; }
void latchBlendUpdate(ChartContext& ctx, uint16_t) { ctx.blendLatch = ctx.animBlend; }
// 235 (0x75CA60) exit: agent +0x4A0 = 0.
void clearAgent4A0(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x4A0) = 0; }
// 405 (0x6457A0): an empty debug stub.
void nop(ChartContext&, uint16_t) {}
// 401 (0x770AD0) exit: the attach frame agent +0xB0..+0xEC = identity [not kept chart-side], player +0xED8 = +0xED4,
// +0xED4 = 2. Its preEntry / update (0x75D6C0 classifier: ladder side, wall rays 0x733F80, 0x741720 / 0x733F60 tables)
// are not ported.
void climbModeExit(ChartContext& ctx, uint16_t) {
    ctx.playerInt(0xED8) = ctx.playerInt(0xED4);
    ctx.playerInt(0xED4) = 2;
}
// 414 (0x730BB0) enter: agent +0x3E0 (slide speed) = 0. The update (0x740A30) moves her along the markup.
void slideEnter(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x3E0) = 0; }
// 427 (0x758AD0) exit: the instance handle player +0x420 cleared, +0x428 = 0.
void clearWallJumpTarget(ChartContext& ctx, uint16_t) { ctx.playerInt(0x420) = 0, ctx.playerInt(0x428) = 0; }
// 512 exit (0x73E6C0): agent +0x464 = now (game ms x 0x841D80 [U: the constant; ours seconds x 30 frames]).
void balanceTimerExit(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x464) = float(ctx.gameMs()) * 0.03f; }
// 573 (0x75CBB0 / 0x75CBE0): agent +0x4B4 = float property 915; then, while 0 < +0x4B4 < it, += timeMult / 30 (s).
void prop915TimerEnter(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x4B4) = propF(ctx, 0x393); }
void prop915TimerUpdate(ChartContext& ctx, uint16_t) {
    const float v = propF(ctx, 0x393);
    float& t = ctx.agentFloat(0x4B4);
    if (0 < t && t < v) t += ctx.timeMult * 33.333f * 0.001f;
}
// 803 (0x75E330 / 0x75E340): the audio flag 0xCCDEA4 set on entry, cleared on exit.
void audioFadeOn(ChartContext& ctx, uint16_t) { ctx.audioFade = true; }
void audioFadeOff(ChartContext& ctx, uint16_t) { ctx.audioFade = false; }
// 1409 (0x72C970) enter: 0x8B083C = game time (0xE7F158) + 3500 ms.
void stampDeadline(ChartContext& ctx, uint16_t) { ctx.deadline8B083C = float(ctx.gameMs()) + 3500.0f; }

// 239 (0x75F3D0 -> 0x756ED0) enter: the climb-out target, player +0x1260 = the jump target's point + its outward normal
// x lara.drm s16 +0x112 (63), z + s16 +0x114 (-151), no higher than the water surface (player +0x8DC volume +0x58 [U]);
// +0x1270 the facing quaternion (from (0, -1, 0) to -n) is kept as the yaw in +0x1270. Without a jump target nothing.
void climbOutTarget(ChartContext& ctx, uint16_t id) {
    const auto t = chartJumpTarget(ctx);
    if (!t) return;
    const Charts& d = ctx.chart().charts();
    glm::vec3 p = t.point + glm::vec3(glm::vec2(t.normal), 0) * float(d.data<int16_t>(0x112));
    p.z += float(d.data<int16_t>(0x114));
    if (const auto w = ctx.water(); w && w->inWater) unportedSlot(ctx, id); // the surface clamp: volume +0x58 unknown
    ctx.playerFloat(0x1260) = p.x, ctx.playerFloat(0x1264) = p.y, ctx.playerFloat(0x1268) = p.z;
    ctx.playerFloat(0x1270) = yawOf(-glm::vec2(t.normal));
}

// A slot nobody could port (the game state it changes is not chart-side).
void unported(ChartContext& ctx, uint16_t id) { unportedSlot(ctx, id); }

} // namespace

// Group CD ids not ported (the game state they read is not modelled; unregistered, they count as unported):
//   0xDA the chart section's anim root offset (0x72C230, bone lara.drm +0x214) against the jump-target point;
//   0x1FE agent +0x3C8 (0x787E60 counts it up, message 0x98 to 0x76FC30 resets it to -4 / -2: proxy support);
//   0x202 the agent's event-time list (+0x530 / +0x538, event 0x1C7); 0x265 the markup slot player +0xBF4 / +0xBF8;
//   0x2A5 the markup owner's bone tilt; 0x2B3 the request's root offset (0x435EC0); 0x2B6 / 0x2B7 the vertex-turn
//   threshold (0x813680 of an ST0 value Ghidra lost); 0x2D3 the grapple support probe (0x72CC50 / 0x739240 records);
//   0x32D the water-material line query (hit byte +0x9D == 2); 0x5AA / 0x5AC / 0x5AD the engagement record player
//   +0x854. 0x1E is Generic.cpp's (same function 0x764B40).
// Actions not ported (their slots count as unported): 209 (beam-side blend from the camera, 0x756700), 237 / 238 (the
// climb-out align / attach: anim root offset, 0x707B90 attach), 242 (0x6EB3A0 absent from the decomp), 302 (0x732C40
// not decompiled), 416 (jump-target search 0x7296C0), 520 / 521 (bone-matrix push-out rays), 608 / 609 / 615 / 617 /
// 740 (the heading system player +0x234..+0x248), 804 (weapon slot validity 0x6F5D00), 1012 (pole wrestle align),
// 1301 / 1302 (water surface player +0x91C), 1306 (an unrecovered carrier vector), 1407 (engagement record).
void registerClimb(Registry& player, Registry&) {
    player.condition(0xCA, criticalSection, "AnimationCriticalSection", "gate");
    player.condition(0x1F5, inWater, "InWater", "gate");
    player.condition(0x204, mantleLedgeAhead, "MantleLedgeAhead", "gate");
    player.condition(0x836, gearSlotIs, "GearSlotIs", "gate");
    const std::string f = "climb";
    player.condition(0xCB, animTransitioning, "AnimationTransitioning", f);
    player.condition(0xCE, animMirrored, "AnimationMirrored", f);
    player.condition(0x196, markupFacing, "MarkupFacing", f);
    player.condition(0x1A8, markupAngleJumpTarget, "MarkupAngleJumpTarget", f);
    player.condition(0x1A9, stickAngleRange, "StickAngleAtLeast", f);
    player.condition(0x1AA, stickAngleRange, "StickAngleAtMost", f);
    player.condition(0x1AC, markupAngleAttached, "MarkupAngle", f);
    player.condition(0x1F6, isFalling, "IsFalling", f);
    player.condition(0x210, agentFlag3ED, "AgentFlag3ED", f);
    player.condition(0x219, prop915Elapsed, "Prop915Elapsed", f);
    player.condition(0x21C, wallJumpAllowed, "WallJumpAllowed", f);
    player.condition(0x282, cornerAngle, "CornerAngleAtLeast", f);
    player.condition(0x283, cornerAngle, "CornerAngleAtMost", f);
    player.condition(0x29F, attachAngle, "AttachAngleAtLeast", f);
    player.condition(0x2A0, attachAngle, "AttachAngleAtMost", f);
    player.condition(0x2B0, jumpTargetReached, "JumpTargetReached", f);
    player.condition(0x2B1, markupLinked, "MarkupHasLinkedObject", f);
    player.condition(0x2B2, jumpTargetIsAttached, "JumpTargetSameAsAttached", f);
    player.condition(0x2B5, atMarkupVertex, "AtMarkupVertex", f);
    player.condition(0x321, waterDepthIs, "WaterDepthClassIs", f);
    player.condition(0x324, swimDepthClass, "SwimDepthClassIs", f);
    player.condition(0x326, swimDepthClass, "SwimDepthClassAtLeast", f);
    player.condition(0x32E, agentFlag4A6, "AgentFlag4A6", f);
    player.condition(0x415, cameraBehind, "CameraBehindFacing", f);
    player.condition(0x5AE, deadlinePast, "DeadlinePast", f);

    // {entry, reentry, preEntry, preReentry, exit, process}
    player.action(212, {beamStickBlendEnter, nullptr, nullptr, nullptr, nullptr, unported}, "BeamStickBlend", f);
    player.action(214, {latchBlendClear, nullptr, nullptr, nullptr, latchBlendClear, latchBlendUpdate}, "LatchBlendParams", f);
    player.action(235, {nullptr, nullptr, nullptr, nullptr, clearAgent4A0, nullptr}, "ClearAgent4A0", f);
    player.action(239, {climbOutTarget, nullptr, nullptr, nullptr, nullptr, nullptr}, "ClimbOutComputeTarget", f);
    player.action(401, {nullptr, nullptr, unported, nullptr, climbModeExit, unported}, "AttachedMarkupClimbMode", f);
    player.action(405, {nop, nullptr, nullptr, nullptr, nullptr, nop}, "Nop405", f);
    player.action(414, {slideEnter, nullptr, nullptr, nullptr, nop, unported}, "SlideAlongMarkup", f);
    player.action(427, {nullptr, nullptr, nullptr, nullptr, clearWallJumpTarget, nullptr}, "ClearWallJumpTarget", f);
    player.action(512, {unported, nullptr, nullptr, nullptr, balanceTimerExit, unported}, "BalanceTimer", f);
    player.action(573, {prop915TimerEnter, nullptr, nullptr, nullptr, nullptr, prop915TimerUpdate}, "Prop915Timer", f);
    player.action(803, {audioFadeOn, nullptr, nullptr, nullptr, audioFadeOff, nullptr}, "AudioFlagCCDEA4", f);
    player.action(1409, {stampDeadline, nullptr, nullptr, nullptr, nullptr, nullptr}, "StampDeadline3500ms", f);
}

} // namespace ou::game::chart
