// Group A: Lara's ground locomotion and landing conditions and actions (docs/re/PLAYER_CHART.md "Conditions",
// "Actions"; ownership openunderworld-work/chart/{conditions,actions}_owner.json by_group.A). Behaviour after tru.exe,
// one function per registry entry, the decompiled tests in their order. A condition returns the raw truth (the runtime
// applies the negation); where the game cannot answer it returns ctx.chart().unported(c). Actions record their effect
// on Lara chart-side (ChartContext player / agent stores, ChartContext::anim); shadow mode never applies them to the
// live Player. A branch an action cannot run here (a markup system, a spawned helper object, a search not ported) bumps
// stats.unportedActions for that action, so the summary still shows it.
//
// Group A ids ported elsewhere: 0x1, 0x65, 0x66, 0x68, 0x69, 0x6A, 0x6B, 0x96, 0x98, 0xC9, 0x191 (Generic.cpp).
// Group A ids not ported (the game state they read is not modelled; they stay unregistered and count as unported):
//   0x12E / 0x131 foot-lock controllers (+0xF20.., their lock bytes +0x10A / +0x10B set through 0x7000F0 by anim events),
//   0x193 / 0x19A the player +0x800 target point, 0x20B the wall-jump anticipation rays (0x759260, n = 0 frames), 0x20D
//   the combat component flags +0x1FC bit 11.
#include "game/Chart.hpp"
#include "game/Game.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>

namespace ou::game::chart {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kDegToRad = 0.017453292f;

// Her heading, instance +0x38 (ours: forward = (cos yaw, sin yaw)).
float playerYaw(const PlayerState& s) { return s.yaw; }

// 0x781FF0: a condition's mode; group 1000 makes it 1000 (no case matches it).
int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; }

// 0x431D70: an angle wrapped into (-pi, pi] (x / 2pi minus its floor, times 2pi, then down by 2pi above pi).
float wrapPi(float a) {
    const float t = a / 6.2831855f;
    float r = (t - std::floor(t)) * 6.2831854820251465f;
    if (r > 3.1415927410125732f) r -= 6.2831854820251465f;
    return r;
}

// 0x814D20: x87 round to nearest (even).
int roundX87(float x) { return int(std::nearbyint(x)); }

// 0x784BF0: a float property in force, the default when the chain never set it.
float propF(ChartRuntime& rt, uint16_t id, float def = 0) { return rt.propFloat(id).value_or(def); }

void unportedPart(ChartContext& ctx, uint16_t action) { ++ctx.chart().stats.unportedActions[action]; }

// The stick's direction relative to her heading, *(*(0xFADFB0 + 0xB0) + 0x10) [radians, + = her left, 0 with the
// stick at rest]: the context's composed angle when it has one (ChartContext::stickRelative), else from the world-space
// stick. nullopt without a player.
std::optional<float> stickAngle(ChartContext& ctx) {
    const PlayerState* s = ctx.player();
    if (!s) return std::nullopt;
    if (const auto r = ctx.stickRelative()) return wrapPi(*r);
    const glm::vec2 v = ctx.stick();
    if (glm::dot(v, v) < 1e-12f) return 0.0f;
    return wrapPi(std::atan2(v.y, v.x) - playerYaw(*s));
}

// The edge-coded bool of 0x763830 / 0x763C50 / 0x782D70 (and 0x762B40 with prevIsCur): cache bit 0 = this update's
// value, bit 1 = the previous update's; recomputed once per agent update stamp. A fresh cache holds 0, except for
// 0x762B40, whose first evaluation takes the previous value equal to the current one.
bool edge(ChartContext& ctx, const Cond& c, bool cur, bool prevIsCur = false) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const bool fresh = cache.frame == ~0u;
        const int prev = fresh && prevIsCur ? int(cur) : (cache.value & 1);
        cache = {rt.frame(), (prev << 1) | int(cur)};
    }
    const int bits = cache.value & 3;
    switch (mode(c)) {
    case 0: return !(bits & 1);
    case 1: return bits & 1;
    case 2: return bits == 2; // just turned off
    case 3: return bits == 1; // just turned on
    default: return false;    // "--- ... unhandled condition"
    }
}

// ---- conditions ---------------------------------------------------------------------------------------------------

// 0xA (0x762B10 -> 0x782B40 mode 0): value == (s16) player +0xA84, the jump query in effect (docs/GAMEPLAY.md
// "Saving grab": +0xA84 is the query a state last ran) [O]; ours PlayerState::jumpQuery [I: the queries ours models].
bool jumpQueryIs(ChartContext& ctx, const Cond& c) {
    const PlayerState* s = ctx.player();
    if (!s) return ctx.chart().unported(c);
    return ctx.chart().value(c) == int(int16_t(s->jumpQuery));
}

// 0x16 (0x764B80 -> 0x782B40 mode 1): round(agent +0x504 - agent +0x78) >= value; +0x78 = the total time action 501
// last stamped (0x75BD30 at 0x75C079).
bool timeSinceMarker(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    return rt.value(c) <= roundX87(rt.totalTime() - ctx.agentFloat(0x78));
}

// 0x9B (0x760D20): frames the button has been held (now - press time, ms x 30 / 1000, + 1; 0 when up) > the state's
// float property 911 (0x38F). Input disabled counts as not held.
bool heldLongerThanProperty(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const ChartContext::Action a = ctx.action(rt.value(c));
    if (!a.known) return rt.unported(c);
    const auto threshold = rt.propFloat(0x38F);
    if (!threshold) return rt.unported(c);
    const int held = a.down ? int(a.seconds * 1000.0f * 30.0f / 1000.0f) + 1 : 0;
    return *threshold < float(held);
}

// 0x9C (0x760E60, id 0x9C): frames since the button's last press (ms x 30 / 1000) < the state's float property 991
// (0x3DF).
bool pressedWithin(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const int id = rt.value(c);
    const auto since = ctx.sincePress(id);
    const auto prop = rt.propFloat(0x3DF);
    if (!since || !prop) return rt.unported(c);
    // never pressed: our timer holds 1e9 s, whose frame count would overflow int (UB; x87 gives INT_MIN, which made
    // every "pressed within" true: a landing took the running jump S280 without Jump) -> clamp first
    const double frames = std::min(double(*since) * 30.0, 1e9);
    return float(int(frames)) < *prop;
}

// 0xD5 (0x783950, id 0xD5 -> 0x782B40 mode 1): round(anim time / 100) >= value. The anim time runs 100 a 30 Hz frame;
// ours is the runtime's clip clock in frames.
bool animFrameAtLeast(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) cache = {rt.frame(), roundX87(rt.animFrames())};
    return rt.value(c) <= cache.value;
}

// 0x785FB0, the MoveDir quadrant classifier: sector bounds A..D = wrapPi(rad(float properties 501..504)), hysteresis
// H = rad(property 507) widening the last sector (agent +0x70, 0 = 1) on both sides; the angle flips sign when the
// chart's anim section plays mirrored and bool property 207 is set. Sector 1 = [D, A], 4 = [A, B], 8 = [C, D], else 2.
// Property 508 with the gear slot-0 instance takes the bounds from that instance (message 0x35): not modelled.
std::optional<int> moveDirZone(ChartContext& ctx, float angle) {
    ChartRuntime& rt = ctx.chart();
    const bool mirrorFlip = rt.propBool(0xCF).value_or(false);
    if (rt.propBool(0x1FC).value_or(false)) return std::nullopt; // the gear instance's bounds [U]
    const auto p501 = rt.propFloat(0x1F5), p502 = rt.propFloat(0x1F6), p503 = rt.propFloat(0x1F7), p504 = rt.propFloat(0x1F8),
               p507 = rt.propFloat(0x1FB);
    if (!p501 || !p502 || !p503 || !p504 || !p507) return std::nullopt; // the debug print path reads garbage
    float a = wrapPi(*p501 * kDegToRad), b = wrapPi(*p502 * kDegToRad), cc = wrapPi(*p503 * kDegToRad), d = wrapPi(*p504 * kDegToRad);
    const float h = *p507 * kDegToRad;
    int& last = rt.field(0x70);
    const int z = last == 0 ? 1 : last;
    if (z & 2) b -= h, cc += h;
    if (z & 1) d -= h, a += h;
    if (z & 4) a -= h, b += h;
    if (z & 8) cc -= h, d += h;
    if (rt.mirrored() && mirrorFlip) angle = -angle;
    int zone;
    if (!(a < angle) && !(d > angle)) zone = 1;
    else if (!(b < angle) && !(a > angle)) zone = 4;
    else if (!(cc > angle) && !(d < angle)) zone = 8;
    else zone = 2;
    last = zone;
    return zone;
}

// The MoveDir values: 410 / 414 / 418 / 423 / 425 / 429 sector 1, 411 / 415 / 419 / 422 / 426 / 430 sector 2,
// 412 / 416 / 420 / 427 / 431 sector 4, 413 / 417 / 421 / 428 / 432 sector 8 (0x785FB0's switch).
int zoneBit(int v) {
    switch (v) {
    case 0x19A: case 0x19E: case 0x1A2: case 0x1A7: case 0x1A9: case 0x1AD: return 1;
    case 0x19B: case 0x19F: case 0x1A3: case 0x1A6: case 0x1AA: case 0x1AE: return 2;
    case 0x19C: case 0x1A0: case 0x1A4: case 0x1AB: case 0x1AF: return 4;
    case 0x19D: case 0x1A1: case 0x1A5: case 0x1AC: case 0x1B0: return 8;
    default: return 0;
    }
}

// 0x192 (0x7612D0): the angle by value, then 0x785FB0. 410..413, 422, 423: the stick against her heading. 414..417
// (the player +0x800 point), 418..421 (the wall / slide normal 0x730790), 424 (stick field +8 with 0x782E60) and
// 429..432 (stick field +0x14) read state ours does not model. 425..428: 0x6EBB40 = the stick's world yaw (stick record
// +0x8) minus the yaw of the attached markup's reversed normal (player +0xCC0, latched into the controller's +0x160 while
// attached and +0x170 is clear) [O decomp]; ours: +0x8 = the stick against her heading plus her heading [I: the
// controller's latch is the current normal; a centred stick reads along her heading]. No attached markup: unported.
bool moveDir(ChartContext& ctx, const Cond& c) {
    const int v = mode(c);
    const bool vsNormal = v >= 0x1A9 && v <= 0x1AC;
    if (!((v >= 0x19A && v <= 0x19D) || v == 0x1A6 || v == 0x1A7 || vsNormal)) return ctx.chart().unported(c);
    auto angle = stickAngle(ctx);
    if (!angle) return ctx.chart().unported(c);
    if (vsNormal) {
        const auto a = ctx.attachedMarkup();
        if (!a || glm::length(glm::vec2(a.normal)) < 1e-6f) return ctx.chart().unported(c);
        angle = wrapPi(*angle + ctx.heading() - std::atan2(-a.normal.y, -a.normal.x));
    }
    const auto zone = moveDirZone(ctx, *angle);
    if (!zone) return ctx.chart().unported(c);
    return (*zone & zoneBit(v)) != 0;
}

// 0x194 (0x761520 -> 0x7868F0, PSCCF_MoveDirLeftOrRight): the stick angle on her left (sector 4) or right (8),
// cached per update. Bounds [0, 180] / [-180, 0] as literal numbers against a radian angle, widened by rad(property
// 507) on the side of the last sector (agent +0x70; 0 keeps none); mirrored by property 207 like 0x785FB0.
bool moveDirLeftRight(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto angle = stickAngle(ctx);
        const auto p507 = rt.propFloat(0x1FB);
        if (!angle || !p507) return rt.unported(c);
        const bool mirrorFlip = rt.propBool(0xCF).value_or(false);
        const float h = *p507 * kDegToRad;
        float lo4 = 0, hi4 = 180, lo8 = -180, hi8 = 0;
        int& last = rt.field(0x70);
        if (last & 4) lo4 = 0.0f - h, hi4 = h + 180.0f;
        if (last & 8) lo8 = -180.0f - h, hi8 = h + 0.0f;
        float a = *angle;
        if (rt.mirrored() && mirrorFlip) a = -a;
        int zone = 0;
        if (!(hi4 < a) && !(lo4 > a)) zone = 4;
        else if (!(lo8 > a) && a <= hi8) zone = 8;
        cache = {rt.frame(), zone};
        last = zone;
    }
    switch (mode(c)) {
    case 0x19C: return cache.value & 4;
    case 0x19D: return cache.value & 8;
    default: return false; // "--- PSCCF_MoveDirLeftOrRight unhandled condition"
    }
}

// 0x197 (0x761560): the direction of her velocity (instance +0x2A0 plus the carrier's) in her own frame, through
// 0x785FB0 (values 410..413). Ours: PlayerState::vel [I: its ground velocity]; 414..417 (against the player +0x800
// point) are not modelled.
bool moveDirMomentum(ChartContext& ctx, const Cond& c) {
    const int v = mode(c);
    const PlayerState* s = ctx.player();
    if (v < 0x19A || v > 0x19D || !s) return ctx.chart().unported(c);
    const float angle = wrapPi(std::atan2(s->vel.y, s->vel.x) - playerYaw(*s));
    const auto zone = moveDirZone(ctx, angle);
    if (!zone) return ctx.chart().unported(c);
    return (*zone & zoneBit(v)) != 0;
}

// 0x198 (0x761660, PSCCF_MoveDirDownhill): 0x7308B0's downhill vector through 0x785FB0 as 0x197 (values 410..413).
// 0x7308B0: an attached markup flagged 0x800000 gives player +0xCB0 (not modelled); else d = (Z x n) x n =
// (nx nz, ny nz, -(nx^2 + ny^2)) from +0x5F0 (the last too-steep face's normal, written only on message 0x1C9 /
// 0x1CA by 0x76FC30, so it outlives the slope) when its x / y are not both 0, else from +0x600 (the last support
// normal, 0x71B910) when its x / y are not both 0 [O: decomp]; the heading is that of (nx, ny) when nz > 0. Both flat:
// (0, 0, owner +0x38), whose heading through 0x70B7C0 / 0x49EFC0 is not decoded [U]; 414..417 (against the player
// +0x800 point) are not modelled.
bool moveDirDownhill(ChartContext& ctx, const Cond& c) {
    const int v = mode(c);
    const PlayerState* s = ctx.player();
    if (v < 0x19A || v > 0x19D || !s) return ctx.chart().unported(c);
    if (const auto am = ctx.attachedMarkup(); am.markup >= 0 && (am.flags & 0x800000)) return ctx.chart().unported(c);
    const auto steep = ctx.steepFaceNormal(), support = ctx.supportFaceNormal();
    if (!steep || !support) return ctx.chart().unported(c);
    const auto sloped = [](const glm::vec3& n) { return n.x != 0.0f || n.y != 0.0f; };
    const glm::vec3* n = sloped(*steep) ? &*steep : sloped(*support) ? &*support : nullptr;
    if (!n) return ctx.chart().unported(c);
    const glm::vec3 d(n->x * n->z, n->y * n->z, -(n->x * n->x + n->y * n->y));
    const float angle = wrapPi(std::atan2(d.y, d.x) - playerYaw(*s));
    const auto zone = moveDirZone(ctx, angle);
    if (!zone) return ctx.chart().unported(c);
    return (*zone & zoneBit(v)) != 0;
}

// 0x19E (0x7619B0, PSCCF_MoveDirSpherical): without a jump target (player +0xA64 == 0) the function returns the negate
// bit itself, i.e. a raw false. With one, the target's yaw / pitch against her (0x786490) is not ported.
bool moveDirSpherical(ChartContext& ctx, const Cond& c) {
    if (!ctx.jumpTarget()) return false;
    return ctx.chart().unported(c);
}

// 0x1F7 (0x762B40, PSCCF_LowCeiling): cur = !0x7145D0, edge-coded, the first evaluation taking the previous value equal
// to the current one. 0x7145D0: 0x7144B0 -> 0x70B5A0 casts the player line query (0x493330, flags 0x2C19E7) from
// her origin (instance +0x20) + 0.1 h up to + h, h = player data +0x10 (the standing height, 512.01); the clearance
// is h unless it hits, and cur is clearance != h, so any hit is a low ceiling [O: decomp]. Without a world to ask, the
// context's own answer (ChartContext::lowCeiling).
bool lowCeiling(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    if (rt.cache(c.id).frame == rt.frame()) return edge(ctx, c, false, true); // cached: cur is not read again
    std::optional<bool> low;
    if (ctx.hasCollision()) {
        const float h = rt.charts().data<float>(0x10);
        const glm::vec3 o = ctx.position();
        low = ctx.playerRay(o + glm::vec3(0, 0, 0.1f * h), o + glm::vec3(0, 0, h)).has_value();
    } else {
        low = ctx.lowCeiling();
    }
    if (!low) return rt.unported(c);
    return edge(ctx, c, *low, true);
}

// 0x1FA (0x763C50, PSCCF_InCombatMode): cur = 0x6F2800, edge-coded.
bool inCombatMode(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    if (rt.cache(c.id).frame == rt.frame()) return edge(ctx, c, false);
    const auto combat = ctx.combatMode();
    if (!combat) return rt.unported(c);
    return edge(ctx, c, *combat);
}

// 0x1FB (0x763D70, PSCCF_UsingGear): cur = the int at (player +0x500) +0x134 is non-zero and 0x6E8E30(slot) holds: the
// slot's instance handle ((player +0x500) + slot * 0x10 + 4) is live and its message 0x36 (0x5B36F0) answers 0;
// edge-coded. With empty hands the slot byte is -1 and 0x6E8E30 returns 0, so cur is false [O]; a held item's message
// 0x36 is not modelled [U], nor the int's upper bytes for slot 0, so those stay unported. The first evaluation takes
// the previous value equal to the current one (uVar6 = cur ? 2 : 0).
bool usingGear(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    if (rt.cache(c.id).frame == rt.frame()) return edge(ctx, c, false, true);
    const auto slot = ctx.gearSlot();
    if (!slot || *slot != -1) return rt.unported(c);
    return edge(ctx, c, false, true);
}

// 0x1E0 (0x72FAF0 -> 0x72F730 with ecx = agent +0x46C, the combo history action 1002 keeps: Jump.cpp comboAppend,
// count +0x484) [O: decomp + asm]. No history: false. Else per entry of Lara data +0xFE0 (count +0xFDC, 0x14 bytes:
// +4 enabled, +8 record count, +0xC records of 0x18 bytes) the records before the history's last position must equal
// the history (s16 +0); the record at that position must have +2 == the condition's raw value, +0 == the current
// state's int property 0x3F2 (type 0xE, else 0) and +0xC < the state time (+0x508) <= +0x10; then for each of its +4
// alternatives (s16 input action ids at +8): the action is active (input table 0xA02B68 +0x18 flags 0x10 or 2, while
// DAT_008AB4E6 is set (default 1), 0xE7F144 bit 0 clear and 0xE7F143 != 4: [I] ours = held) and 0x72F6D0 holds (the
// stick angle, player globals +0xB0 +0x10, through 0x785FB0 with the record's s16 +0x14) -> true. The entry's scan ends
// at that record either way.
bool comboMove(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    const int len = rt.field(0x484);
    if (len <= 0) return false;
    const Charts& d = rt.charts();
    const auto table = d.pointer(d.dataOffset + 0xFE0);
    if (!table) return false;
    const Property* p = rt.property(0x3F2);
    const int combo = p && p->type == 0xE ? int(p->raw) : 0;
    const float t = rt.stateTime();
    const uint32_t n = d.data<uint32_t>(0xFDC);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = *table - d.dataOffset + i * 0x14;
        if (d.data<int32_t>(e + 4) == 0) continue;
        const uint32_t count = d.data<uint32_t>(e + 8);
        const auto seq = d.pointer(d.dataOffset + e + 0xC);
        if (!seq) continue;
        for (uint32_t k = 0; k < count; ++k) {
            const uint32_t r = *seq - d.dataOffset + k * 0x18;
            if (int(k) >= len - 1) {
                const uint32_t alts = d.data<uint32_t>(r + 4);
                if (d.data<int16_t>(r + 2) == c.value && d.data<int16_t>(r) == combo && d.data<float>(r + 0xC) < t &&
                    t <= d.data<float>(r + 0x10) && alts != 0) {
                    const auto list = d.pointer(d.dataOffset + r + 8);
                    if (!list) return rt.unported(c);
                    const auto angle = stickAngle(ctx);
                    const auto zone = angle ? moveDirZone(ctx, *angle) : std::nullopt;
                    for (uint32_t j = 0; j < alts; ++j) {
                        const ChartContext::Action a = ctx.action(d.data<int16_t>(*list - d.dataOffset + j * 2));
                        if (!a.known) return rt.unported(c);
                        if (!a.down) continue;
                        if (!zone) return rt.unported(c);
                        if (*zone & zoneBit(d.data<int16_t>(r + 0x14))) return true;
                    }
                }
                break;
            }
            if (d.data<int16_t>(r) != rt.field(0x46C + int(k) * 4)) break;
        }
    }
    return false;
}

// 0x1EA (0x764FD0) / 0x1EF (0x764E00) / 0x1E5 (0x7651E0): an s16 of the move sweep's record (player +0x31C, ChartJ's
// moveSweepRecord: Lara data +0x630's 0x84-byte records), read only when the record is set and, for these ids, byte
// +0x320 is (sweepFresh: this update's sweep stored it; the sibling ids 0x1EB / 0x1F0 / 0x1E6 skip that byte), else
// 0. offs are by state int property 532 (0x214) i = 1 / 0 / 2 (Jump.cpp sweepSlot, the same layout). nullopt: the
// context cannot say.
std::optional<int16_t> freshSweepField(ChartContext& ctx, uint16_t off) {
    const auto rec = ctx.moveSweepRecord();
    const auto fresh = ctx.sweepFresh();
    if (!rec || !fresh) return std::nullopt;
    if (*rec < 0 || !*fresh) return int16_t(0);
    const Charts& d = ctx.chart().charts();
    const auto base = d.pointer(d.dataOffset + 0x630);
    if (!base) return std::nullopt;
    int16_t v = 0;
    const size_t at = size_t(*base) + size_t(*rec) * 0x84 + off;
    if (at + 2 <= d.bytes.size()) std::memcpy(&v, d.bytes.data() + at, 2);
    return v;
}
std::optional<int16_t> freshSweepSlot(ChartContext& ctx, const std::array<uint16_t, 3>& offs) {
    const int i = ctx.chart().propInt(0x214).value_or(0);
    if (i < 0 || i > 2) { // no field for an index outside 0..2 (as Jump.cpp sweepSlot)
        if (!ctx.moveSweepRecord() || !ctx.sweepFresh()) return std::nullopt;
        return int16_t(0);
    }
    return freshSweepField(ctx, offs[size_t(i == 1 ? 0 : i == 0 ? 1 : 2)]);
}
// 0x1EA: grapple {+0x5C, +0x5A, +0x58}; else by byte +0x2C5 (sweepCandidates) and combat (0x6F2800):
// {+0x3C, +0x3A, +0x38} / {+0x4C, +0x4A, +0x48} with a candidate, {+0x44, +0x42, +0x40} / {+0x54, +0x52, +0x50}
// without. Equal to the value [O].
bool envProbeClass(ChartContext& ctx, const Cond& c) {
    const auto g = ctx.grapple();
    const auto w = ctx.combatMode();
    const auto cv = ctx.sweepCandidates();
    if (!g || !w || !cv) return ctx.chart().unported(c);
    using Offs = std::array<uint16_t, 3>;
    const Offs offs = g->attached ? Offs{0x5C, 0x5A, 0x58}
                      : *cv       ? (*w ? Offs{0x4C, 0x4A, 0x48} : Offs{0x3C, 0x3A, 0x38})
                                  : (*w ? Offs{0x54, 0x52, 0x50} : Offs{0x44, 0x42, 0x40});
    const auto s = freshSweepSlot(ctx, offs);
    if (!s) return ctx.chart().unported(c);
    return *s == ctx.chart().value(c);
}
// 0x1EF: grapple {+0x74, +0x72, +0x70}, combat {+0x6C, +0x6A, +0x68}, else {+0x64, +0x62, +0x60}; then unless 0x759850
// holds for either candidate slot (+0x3A0 / +0x3E0; ours a candidate now, as Jump.cpp 0x1F0 [I]) 2 -> 0 and 9 -> 7.
bool envProbeClassB(ChartContext& ctx, const Cond& c) {
    const auto g = ctx.grapple();
    const auto w = ctx.combatMode();
    if (!g || !w) return ctx.chart().unported(c);
    using Offs = std::array<uint16_t, 3>;
    const Offs offs = g->attached ? Offs{0x74, 0x72, 0x70} : *w ? Offs{0x6C, 0x6A, 0x68} : Offs{0x64, 0x62, 0x60};
    auto s = freshSweepSlot(ctx, offs);
    if (!s) return ctx.chart().unported(c);
    if (*s == 2 || *s == 9) {
        const auto cand = ctx.jumpCandidates();
        if (!cand) return ctx.chart().unported(c);
        if (!cand->a && !cand->b) s = int16_t(*s == 2 ? 0 : 7);
    }
    return *s == ctx.chart().value(c);
}
// 0x1E5: the record's s16 +0x32 (one field, no slot index). Equal to the value [O].
bool envProbeClass32(ChartContext& ctx, const Cond& c) {
    const auto s = freshSweepField(ctx, 0x32);
    if (!s) return ctx.chart().unported(c);
    return *s == ctx.chart().value(c);
}

// 0x208 (0x7635F0, PSCCF_OnStairs): byte Lara +0x11D4 (ChartContext::onStairs: the ground check 0x5DCE20's floor hit
// through 0x5D3AF0(hit, 0x100, 0x20), a terrain face's client flags 0x0100); 0 -> clear, 1 -> set, other -> false [O].
bool onStairs(ChartContext& ctx, const Cond& c) {
    const auto s = ctx.onStairs();
    if (!s) return ctx.chart().unported(c);
    switch (mode(c)) {
    case 0: return !*s;
    case 1: return *s;
    default: return false;
    }
}

// 0x1FC (0x764940): byte agent +0x345; 0 -> clear, 1 -> set.
bool agentFlag345(ChartContext& ctx, const Cond& c) {
    const bool f = ctx.chart().field(0x345) != 0;
    switch (mode(c)) {
    case 0: return !f;
    case 1: return f;
    default: return false;
    }
}

// 0x1FD (0x763670, PSCCF_OnSlopeSlide): counter agent +0x3C4; on a sliding slope (player +0x11D0 > 0 and byte
// +0x11CF) a counter >= 0 becomes -4, off it a negative counter becomes 0. 0: c >= 0, 1: c < 0, 2: c == 0, 3: c == -4.
// Ours: on a sliding slope = the floor under her is a slide face when the context can tell (drive mode,
// ChartContext::onSlideFloor), else State::Slide (shadow) [I].
bool onSlopeSlide(ChartContext& ctx, const Cond& c) {
    const PlayerState* s = ctx.player();
    const auto floor = ctx.onSlideFloor();
    if (!s && !floor) return ctx.chart().unported(c);
    int& counter = ctx.chart().field(0x3C4);
    if (floor ? *floor : s->state == ou::game::State::Slide) {
        if (counter >= 0) counter = -4;
    } else if (counter < 0) {
        counter = 0;
    }
    switch (mode(c)) {
    case 0: return counter >= 0;
    case 1: return counter < 0;
    case 2: return counter == 0;
    case 3: return counter == -4;
    default: return false;
    }
}

// 0x203 (0x7633E0, PSCCF_WallJumpAttach): cur = 0x7594C0 == 2 (cached). 0 -> !cur, 1 -> cur.
bool wallJumpAttach(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto w = ctx.wallJumpAttach();
        if (!w) return rt.unported(c);
        cache = {rt.frame(), int(*w)};
    }
    switch (mode(c)) {
    case 0: return !cache.value;
    case 1: return cache.value;
    default: return false;
    }
}

// 0x12D (0x760FB0, PSCCF_FootSupport): f = player +0xF3C (0x6EB630), cached per update; 301 -> f == 1, 302 -> f == 2,
// 305 -> f == 0, else "unhandled", false.
bool footSupport(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    if (cache.frame != rt.frame()) {
        const auto f = ctx.supportFoot();
        if (!f) return rt.unported(c);
        cache = {rt.frame(), *f};
    }
    switch (mode(c)) {
    case 301: return cache.value == 1;
    case 302: return cache.value == 2;
    case 305: return cache.value == 0;
    default: return false;
    }
}

// 0x206 (0x763740, PSCCF_IsWallSlide): now = agent +0x90 != 0 and (0xE7F15C - +0x90) <= 99 ms, +0x90 being stamped by
// message 0x1C9 (too-steep ground, 0x76FC30); edge-coded (a fresh cache holds 0).
bool isWallSlide(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    if (rt.cache(c.id).frame == rt.frame()) return edge(ctx, c, false);
    const auto recent = ctx.wallSlideRecent();
    if (!recent) return rt.unported(c);
    return edge(ctx, c, *recent);
}

// 0x205 (0x752220, CanSprint): 0x74FD70 on the sprint stamina block agent +0x460. 0 -> !cur, 1 -> cur.
bool canSprint(ChartContext& ctx, const Cond& c) {
    const auto can = ctx.canSprint();
    if (!can) return ctx.chart().unported(c);
    switch (mode(c)) {
    case 0: return !*can;
    case 1: return *can;
    default: return false;
    }
}

// 0x207 (0x763830, PSCCF_IsInCheatMode): cur = player +0x68 bit 30, edge-coded (a fresh cache holds 0).
bool isInCheatMode(ChartContext& ctx, const Cond& c) { return edge(ctx, c, ctx.cheatMode()); }

// 0x214 (0x762D00 -> 0x782D70, callback 0x762CE0): bool property 2200 (0x898, 0x6EEF00 mode 2, default false),
// edge-coded.
bool stateFlag898(ChartContext& ctx, const Cond& c) { return edge(ctx, c, ctx.chart().propBool(0x898).value_or(false)); }

// ---- actions ------------------------------------------------------------------------------------------------------

// 0x6457A0: the stripped debug print (actions 7, 409, and the empty slots of others).
void noop(ChartContext&, uint16_t) {}

// Action 201 (PSCAF_LocomotionLeanBlend). Entry 0x75CCB0: agent +0x40 / +0x50 = her position (previous-position
// samples), the blend parameters +0x8C / +0x90 = 0. Exit 0x754F60: +0x8C / +0x90 = 0. Process 0x76C0A0 projects the
// desired move (player +0x600, which action 502 / 0x755A00 writes) onto her axes: not ported.
void leanBlendEntry(ChartContext& ctx, uint16_t) {
    if (const PlayerState* s = ctx.player()) {
        for (int i = 0; i < 3; ++i) ctx.agentFloat(0x40 + 4 * i) = ctx.agentFloat(0x50 + 4 * i) = s->pos[i];
        ctx.agentFloat(0x4C) = ctx.agentFloat(0x5C) = 1.0f; // the w of the vec4 0x4D9C20 copies [I]
    }
    ctx.animBlend = glm::vec2(0);
}
void leanBlendExit(ChartContext& ctx, uint16_t) { ctx.animBlend = glm::vec2(0); }
void leanBlendProcess(ChartContext& ctx, uint16_t id) { unportedPart(ctx, id); }

// Action 207 (PSCAF_MarkupLengthBlend) Process 0x744FD0: segment 0 of the attached markup, L its length; +0x8C =
// clamp01((L - pd +0x800) / (pd +0x804 - pd +0x800)) (0 when they are equal), +0x90 = 1 when the markup rides a live
// instance (0x5BE2D0) else 0.
void markupLengthBlend(ChartContext& ctx, uint16_t id) {
    const auto m = ctx.attachedMarkup();
    const auto seg = m ? ctx.markupSegment(m.markup, 0) : std::nullopt;
    if (!seg) return unportedPart(ctx, id); // tru.exe reads a null markup here
    const float lo = ctx.chart().charts().data<float>(0x800), hi = ctx.chart().charts().data<float>(0x804);
    const float len = glm::length(seg->second - seg->first);
    ctx.animBlend.x = hi == lo ? 0.0f : std::clamp((len - lo) / (hi - lo), 0.0f, 1.0f);
    ctx.animBlend.y = m.live ? 1.0f : 0.0f;
}

// Action 210 (PSCAF_HurdleHeightBlendAndSpeed). PreAnimEntry / PreAnimReentry 0x749B80: point A = her position,
// B = the jump target's point (+0x9F0) when there is one, else the attached markup's (+0xCA0), else nothing; target
// types 8..10 lower A by a type-specific height (+0xAE8 / +0xE88, not modelled); +0x8C = clamp01((B.z - A.z - p230) /
// (p231 - p230)) via 0x749980 (0 when p231 < p230). Entry / Process 0x72F140: s = property 238 (default 1); s >= 1:
// rate = (1 - +0x8C) (s - 1) + 1.
void hurdleHeightBlend(ChartContext& ctx, uint16_t id) {
    const PlayerState* s = ctx.player();
    const auto jt = ctx.jumpTarget();
    const auto at = jt ? jt : ctx.attachedMarkup();
    if (!at) return;
    if (!s || (at.type >= 8 && at.type <= 10)) return unportedPart(ctx, id);
    ChartRuntime& rt = ctx.chart();
    const float p230 = propF(rt, 0xE6), p231 = propF(rt, 0xE7);
    float x = 0;
    if (!(p231 < p230)) {
        const float d = p231 - p230;
        x = d > 0 ? std::clamp((at.point.z - s->pos.z - p230) / d, 0.0f, 1.0f) : 0.0f; // [I] 0x749980 on d == 0
    }
    ctx.animBlend.x = x;
}
void hurdleSpeed(ChartContext& ctx, uint16_t) {
    const float sp = propF(ctx.chart(), 0xEE, 1.0f);
    if (sp >= 1.0f) ctx.animRate = (1.0f - ctx.animBlend.x) * (sp - 1.0f) + 1.0f;
}

// Action 221 (PSCAF_RunLeanToStick). Entry 0x75CDA0: agent +0x7C = 0. Process 0x75CDB0: max = property 217 (deg),
// gain = 218, rate = clamp01(219 x timeMult); target = clamp(wrapPi(stick angle) x gain, +-rad(max)) when max and gain
// are positive, else 0; lean += rate (target - lean) into +0x7C and the lean controller (+0xF18 -> +0x40). Exit
// 0x75CF30: +0x7C = 0, the controller's lean 0.
void runLeanEntry(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x7C) = 0; }
void runLeanProcess(ChartContext& ctx, uint16_t id) {
    ChartRuntime& rt = ctx.chart();
    float maxDeg = propF(rt, 0xD9), gain = propF(rt, 0xDA);
    const float rate = std::clamp(ctx.timeMult * propF(rt, 0xDB), 0.0f, 1.0f);
    float target = 0;
    if (maxDeg > 0 && gain > 0) {
        const auto angle = stickAngle(ctx);
        if (!angle) return unportedPart(ctx, id);
        const float lim = maxDeg * kDegToRad;
        target = std::clamp(wrapPi(*angle) * gain, -lim, lim);
    }
    float& lean = ctx.agentFloat(0x7C);
    lean += rate * (target - lean);
    ctx.anim.lean = lean;
}
void runLeanExit(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x7C) = 0, ctx.anim.lean = 0; }

// Action 222 (PSCAF_TorsoTwistTowardHeading). Entry 0x75CF60: agent +0x88 = 0 (and the spine controllers of type 14
// removed). Process 0x76C9B0: max = property 220 (deg), gain 221, rate clamp01(222 x timeMult), source 223 (default
// 203; 204 = the yaw of the player +0x1270 target rotation against hers, not modelled; else the stick angle);
// target = clamp(wrapPi(e) x gain, +-rad(max)); twist = wrapPi(wrapPi(target - twist) x rate + twist); |twist| <= 0.01:
// twist 0, player +0x7D8 = wrapPi(yaw + twist) and +0x7DC = 0; else +0x7D4 = wrapPi(yaw + twist) the first time
// (+0x7DC set) and +0x7D8 = wrapPi(yaw + target). Exit 0x75CFD0: +0x88 = 0, +0x7D8 = her yaw, +0x7DC = 0.
void twistEntry(ChartContext& ctx, uint16_t) { ctx.agentFloat(0x88) = 0; }
void twistProcess(ChartContext& ctx, uint16_t id) {
    ChartRuntime& rt = ctx.chart();
    const PlayerState* s = ctx.player();
    if (!s) return unportedPart(ctx, id);
    const float maxDeg = propF(rt, 0xDC), gain = propF(rt, 0xDD);
    const float rate = std::clamp(ctx.timeMult * propF(rt, 0xDE), 0.0f, 1.0f);
    const int source = rt.propInt(0xDF).value_or(0xCB);
    float e = 0;
    if (source == 0xCC) return unportedPart(ctx, id); // the +0x1270 target rotation
    const auto angle = stickAngle(ctx);
    if (!angle) return unportedPart(ctx, id);
    e = *angle;
    float target = 0;
    if (maxDeg > 0 && gain > 0) {
        const float lim = maxDeg * kDegToRad;
        target = std::clamp(wrapPi(e) * gain, -lim, lim);
    }
    const float yaw = playerYaw(*s);
    float twist = ctx.agentFloat(0x88);
    twist = wrapPi(wrapPi(target - twist) * rate + twist);
    const float toTarget = wrapPi(yaw + target), toTwist = wrapPi(yaw + twist);
    if (std::abs(twist) <= 0.01f) {
        ctx.playerFloat(0x7D8) = toTwist;
        twist = 0;
        ctx.playerInt(0x7DC) = 0;
    } else {
        if (!ctx.playerInt(0x7DC)) ctx.playerFloat(0x7D4) = wrapPi(toTwist), ctx.playerInt(0x7DC) = 1;
        ctx.playerFloat(0x7D8) = toTarget;
    }
    ctx.agentFloat(0x88) = twist;
}
void twistExit(ChartContext& ctx, uint16_t) {
    ctx.agentFloat(0x88) = 0;
    if (const PlayerState* s = ctx.player()) ctx.playerFloat(0x7D8) = playerYaw(*s);
    ctx.playerInt(0x7DC) = 0;
}

// Action 231 (PSCAF_AlignToTargetOverAnim) Entry 0x75F550: the root blends onto player +0x1260 (position) / +0x1270
// (rotation) over property 283 frames (type 0x23 value 225: the current anim's length) x 100, or the anim's length
// when under one frame; bool property 285 = position only (mode 2) else position and rotation (mode 3).
void alignToTarget(ChartContext& ctx, uint16_t) {
    ChartRuntime& rt = ctx.chart();
    float frames = 0;
    if (const Property* p = rt.property(0x11B); p && p->type == 0x23 && rt.charts().propInt(*p) == 225)
        frames = float(roundX87(rt.animLengthFrames()));
    else
        frames = float(rt.propInt(0x11B).value_or(0));
    auto& al = ctx.anim.align;
    al.active = true;
    al.positionOnly = rt.propBool(0x11D).value_or(false);
    al.duration = frames >= 1 ? frames : rt.animLengthFrames();
    al.pos = {ctx.playerFloat(0x1260), ctx.playerFloat(0x1264), ctx.playerFloat(0x1268)};
}

// Action 407 (PSCAF_DetachMarkupOnExit) Exit 0x75DC00: with an attached markup, its linked objects get NtMarkup
// OnDetach (game side, not in shadow mode), player +0xEA0 = the markup, +0xEA4 = its param, +0xEA8 = 0x13, +0xC94 = 0,
// +0xEAC = 0.
void detachMarkupExit(ChartContext& ctx, uint16_t) {
    const auto m = ctx.attachedMarkup();
    if (!m) return;
    ctx.playerInt(0xEA0) = m.markup;
    ctx.playerFloat(0xEA4) = m.param;
    ctx.playerInt(0xEA8) = 0x13;
    ctx.playerInt(0xC94) = 0; // recorded detach (ours: -1 none would be the markup index; 0 = tru's null pointer)
    ctx.playerInt(0xEAC) = 0;
    detachChartMarkup(ctx);
}

// Action 411 (PSCAF_GrabHelperObject). PreAnimEntry 0x75E590: player +0xEE8 = 1 when the jump target's owner object
// has flag bit 0 of +0x50 (not modelled: counted with a jump target). Entry 0x75E5C0 / Reentry 0x75E6C0 (the latter
// only with +0xEE8 == 1): +0xEE8 != 1 -> agent +0x340 = 0, player +0xEEC = 0; else the helper object is born (game
// side, counted). Exit 0x75E6E0 releases it: player +0x74C = 0, agent +0x340 = 0, +0xEEC = 0, and +0x8C = 0 under the
// attached markup's flag test (0x733F60 not modelled: counted with a markup). Process 0x76DB40 needs the markup.
void grabHelperPre(ChartContext& ctx, uint16_t id) {
    if (ctx.jumpTarget()) unportedPart(ctx, id);
}
void grabHelperEntry(ChartContext& ctx, uint16_t id) {
    if (ctx.playerInt(0xEE8) != 1) {
        ctx.chart().field(0x340) = 0;
        ctx.playerInt(0xEEC) = 0;
        return;
    }
    unportedPart(ctx, id);
}
void grabHelperReentry(ChartContext& ctx, uint16_t id) {
    if (ctx.playerInt(0xEE8) == 1) unportedPart(ctx, id);
}
void grabHelperExit(ChartContext& ctx, uint16_t id) {
    ctx.playerFloat(0x74C) = 0;
    ctx.chart().field(0x340) = 0;
    ctx.playerInt(0xEEC) = 0;
    if (ctx.attachedMarkup()) unportedPart(ctx, id);
}
void grabHelperProcess(ChartContext& ctx, uint16_t id) {
    if (ctx.attachedMarkup()) unportedPart(ctx, id);
}

// Action 415 (PSCAF_FlexMarkupSag). Entry 0x740D10 / Exit 0x740F70: agent +0x3D4 = +0x3D8 = 0, +0x3DC = the delay
// of the sag tuning block (pd +0xCEC, or +0xD0C for a flat flexible markup) +0x14; exit also clears the bend of the
// markup object (game side). Process 0x740D70 only acts on a flexible markup (flag 0x1000000): not ported.
void flexSagReset(ChartContext& ctx, uint16_t id) {
    ctx.agentFloat(0x3D4) = ctx.agentFloat(0x3D8) = 0;
    if (const auto m = ctx.attachedMarkup(); m && (m.flags & 0x1000000)) return unportedPart(ctx, id); // which block [U]
    ctx.agentFloat(0x3DC) = ctx.chart().charts().data<float>(0xCEC + 0x14);
}
void flexSagProcess(ChartContext& ctx, uint16_t id) {
    if (const auto m = ctx.attachedMarkup(); m && (m.flags & 0x1000000)) unportedPart(ctx, id);
}

// Action 423 (PSCAF_ScheduledJumpTargetSearch): two searches (0x7296C0 on player +0x980 with profile +0xB00, and on
// +0xB10 with +0xC90), each run in the slots its schedule property (479 / 480, type 0x23) names: 592 PreAnimEntry +
// Process, 593 PreAnimEntry + Exit, 594 Entry + Reentry, 595 Entry + Exit, 596 PreAnimEntry, 597 Entry, 598 Process,
// 599 Exit, 600 Reentry. The PreAnim / Entry / Reentry slots first record the jump reference height (0x72FBF0) when
// bool property 565 is set. The profiles are player +0xB00 / +0xC90, which properties 541 / 546 set when a state names
// one (0x766EC0) and which persist until another does. The primary search is chartMarkupSearch into player
// +0xA64; the secondary's object +0xB10 has no chart-side store yet (counted), nor has a search ours cannot model.
enum Slot { PreEntry, Entry, Reentry, Process, Exit };
bool scheduleRuns(int schedule, Slot slot) {
    switch (slot) {
    case PreEntry: return schedule == 593 || schedule == 596 || schedule == 592;
    case Entry: return schedule == 597 || schedule == 594 || schedule == 595;
    case Reentry: return schedule == 600 || schedule == 594;
    case Process: return schedule == 598 || schedule == 592;
    case Exit: return schedule == 599 || schedule == 595 || schedule == 593;
    }
    return false;
}
void recordJumpRefHeight(ChartContext& ctx) { // 0x72FBF0
    if (const PlayerState* s = ctx.player()) ctx.playerFloat(0x6DC) = s->pos.z;
    ctx.playerFloat(0x6E0) = ctx.attachedMarkup() ? ctx.playerFloat(0xCE8) : 0.0f;
}
template <Slot S>
void scheduledSearch(ChartContext& ctx, uint16_t id) {
    ChartRuntime& rt = ctx.chart();
    if constexpr (S == PreEntry || S == Entry || S == Reentry)
        if (rt.propBool(0x235).value_or(false)) recordJumpRefHeight(ctx);
    if (const auto schedule = rt.propInt(0x1DF); schedule && scheduleRuns(*schedule, S)) {
        const auto it = rt.field(0xB00); // player +0xB00, set by properties 541 (Jump.cpp searchProfileProp, 0x766EC0)
        if (!it || !chartMarkupSearch(ctx, it)) unportedPart(ctx, id);
    }
    if (const auto schedule = rt.propInt(0x1E0); schedule && scheduleRuns(*schedule, S)) unportedPart(ctx, id);
}

// Action 425 (PSCAF_LookAtAttachPoint). Process 0x76E190 acts only with an attached markup (the look-at target from
// player +0xBC0 / +0xC14, not modelled: counted). Exit 0x75EEB0 posts message 0x69 (clear the look-at; game side).
void lookAtProcess(ChartContext& ctx, uint16_t id) {
    if (ctx.attachedMarkup()) unportedPart(ctx, id);
}

// Action 501 (PSCAF_StickScaledAnimRate) Process 0x75BD30: thresholds T1..T3 = properties 522..524, hysteresis
// H1..H3 = 525..527, rate pairs 512..519, band 521 (type int). m = the stick level. 402: t = clamp01((m - (T1 - H1)) /
// ((H2 + T2) - (T1 - H1))), rate = lerp(p512, p513). 403: t over [T2 - H2, H3 + T3], lerp(p514, p515); 404: over
// [T3 - H3, 1], lerp(p516, p517); 405 the same t, lerp(p518, p519); 403..405 stamp agent +0x78 = +0x504. A rate in
// (0, 2] plays as is; above 2 it is a speed in units per 30 Hz frame: / sub-layer 0's clip root speed (0x4389C0:
// -header +0x1C x 100 / (keys x time per key), Request::rootSpeed), clamped to [0.5, 2] (beam walk R192: 4.725 -> 1.714
// in Wine h759, 4.0 units per 60 Hz tick) [O]; 0 or no band: 1. Then every sub-layer's rate (0x438C40). [U] 0x702AC0
// when agent +0xC == 1.
void stickScaledAnimRate(ChartContext& ctx, uint16_t id) {
    ChartRuntime& rt = ctx.chart();
    const float t1 = propF(rt, 0x20A), t2 = propF(rt, 0x20B), t3 = propF(rt, 0x20C);
    const float h1 = propF(rt, 0x20D), h2 = propF(rt, 0x20E), h3 = propF(rt, 0x20F);
    float r[8];
    for (int i = 0; i < 8; ++i) r[i] = propF(rt, uint16_t(0x200 + i));
    const int band = rt.propInt(0x209).value_or(0);
    const float m = ctx.stickLevel();
    const auto t01 = [](float x) { return x < 0 ? 0.0f : x > 1 ? 1.0f : x; };
    float rate = 1, out = 0;
    switch (band) {
    case 0x192: out = r[0] + t01((m - (t1 - h1)) / ((h2 + t2) - (t1 - h1))) * (r[1] - r[0]); break;
    case 0x193: out = r[2] + t01((m - (t2 - h2)) / ((h3 + t3) - (t2 - h2))) * (r[3] - r[2]); break;
    case 0x194: out = r[4] + t01((m - (t3 - h3)) / (1.0f - (t3 - h3))) * (r[5] - r[4]); break;
    case 0x195: out = r[6] + t01((m - (t3 - h3)) / (1.0f - (t3 - h3))) * (r[7] - r[6]); break;
    default: ctx.animRate = 1; return;
    }
    if (band != 0x192) ctx.agentFloat(0x78) = rt.totalTime();
    if (out != 0) {
        if (out <= 2) rate = out;
        else if (const Request* p = rt.pattern(); p && p->rootSpeed > 0) // sub-layer 0's clip (0x4389C0, layer +0xF8 = 0)
            rate = std::clamp(out / p->rootSpeed, 0.5f, 2.0f);
        else if (!p) return unportedPart(ctx, id); // an unresolved node: its clip's root speed is not known chart-side
    }
    ctx.animRate = rate;
}

// Action 509 (PSCAF_SetAnimRate), every slot 0x75E9D0: rate = float property 225 (at least 0.001); 1 when it is
// missing or bool-typed.
void setAnimRate(ChartContext& ctx, uint16_t) {
    const Property* p = ctx.chart().property(0xE1);
    ctx.animRate = !p || p->type == 2 ? 1.0f : std::max(0.001f, ctx.chart().charts().propFloat(*p));
}

// Action 515 (PSCAF_RecordJumpRefHeight): Entry 0x72FBF0 records player +0x6DC = her z, +0x6E0 = the attached
// markup's grab offset z (+0xCE8) or 0; Reentry 0x72FC40 the same when bool property 565 is set.
void jumpRefEntry(ChartContext& ctx, uint16_t) { recordJumpRefHeight(ctx); }
void jumpRefReentry(ChartContext& ctx, uint16_t) {
    if (ctx.chart().propBool(0x235).value_or(false)) recordJumpRefHeight(ctx);
}

// Action 517 (PSCAF_ClearJumpTarget) PreAnimEntry 0x75F9E0 (= 0x707500): player +0xA64 = 0, +0xA6C = 0, (u16) +0xAC0
// = 0, +0xA74 = 0.
void clearJumpTarget(ChartContext& ctx, uint16_t) {
    ctx.playerInt(0xA64) = 0, ctx.playerInt(0xA6C) = 0, ctx.playerInt(0xAC0) = 0;
    ctx.playerFloat(0xA74) = 0;
}

// Action 571 (PSCAF_PoleControlFrame): Entry 0x75E3A0, Process 0x75E3C0 and Exit 0x75E3E0 act only with an attached
// markup (the pole-relative control frame, not modelled: counted).
void poleControlFrame(ChartContext& ctx, uint16_t id) {
    if (ctx.attachedMarkup()) unportedPart(ctx, id);
}

// Markup-bound actions whose every slot needs the attached markup / jump target systems (counted each time a slot runs):
// 220 (0x75C790 / 0x75CA20 IK target bone), 230 (0x772CA0 markup anim target), 236 (0x745100 root-motion warp), 323
// (0x75E930 / 0x76E080 gear attachments), 403 (0x76D4A0 / 0x76D240 jump target frame), 426 (0x72BD40 limb IK), 502
// (0x75C540 -> 0x755A00 analog velocity), 505 (0x74D980 / 0x734920 ladder rung), 508 (0x76BF40 / 0x75C620 / 0x75C550
// slide speed).
void unportedSlot(ChartContext& ctx, uint16_t id) { unportedPart(ctx, id); }

// 0x5BE2F0: a markup is usable unless flagged 0x80000000 (or its owner instance is hidden / flagged 2, 0x20000000,
// 0x800: [I] ours has no hidden markup owners).
bool usable(const ChartContext::MarkupSlot& m) { return m && !(m.flags & 0x80000000u); }

// 0x75D6C0: the attach surface class, player +0xED8 = the old +0xED4, then +0xED4 by the markup's flags: none of
// 0x4F400010 -> 2; 0x400000 with her root not 110.62 (Lara data +0x770) below segment 1's end (0x75D650) -> 0; 0x10
// -> 1. Then the markup-type tables (0x74B5F0 / 0x731A50, 0x741720 / 0x731B00, 0x733F80, 0x733F60) for markups with
// those table flags (unported: counted as action 420's, +0xED4 kept), else !0x74F480 (ledgeBrace).
void attachSurfaceClass(ChartContext& ctx, const ChartContext::MarkupSlot& m) {
    ctx.playerInt(0xED8) = ctx.playerInt(0xED4);
    if (!(m.flags & 0x4F400010u)) {
        ctx.playerInt(0xED4) = 2;
        return;
    }
    if (m.flags & 0x400000u) {
        const auto seg = ctx.markupSegment(m.markup, 1);
        if (!seg) return unportedPart(ctx, 420);
        if (!(ctx.position().z - seg->first.z < -ctx.chart().charts().data<float>(0x770))) {
            ctx.playerInt(0xED4) = 0;
            return;
        }
    }
    if (m.flags & 0x10u) {
        ctx.playerInt(0xED4) = 1;
        return;
    }
    // 0x74B5F0 / 0x741720 / 0x733F80 / 0x733F60 test the markup data's attribute tables (+0x180 / +0x184) only for
    // markups flagged 0xC000000 / 0xA000000 (0x741670 / 0x733EC0); without those all four fail and the class is
    // !0x74F480 (group B's ledgeBrace: no wall under the hang = 1).
    if (!(m.flags & 0xE000000u)) {
        if (const auto brace = ledgeBrace(ctx, m)) {
            ctx.playerInt(0xED4) = *brace ? 0 : 1;
            return;
        }
    }
    unportedPart(ctx, 420);
}

// 0x75D8D0: the class of the attached markup (+0xC94), else of the jump target (+0xA64), when usable.
void classifyAttach(ChartContext& ctx) {
    const auto a = ctx.attachedMarkup();
    const auto m = a ? a : ctx.jumpTarget();
    if (usable(m)) attachSurfaceClass(ctx, m);
}

// 0x76D270 on the attached markup: the attach frame (agent +0xB0: edge-aligned 0x737D60, box 0x6EBEF0 or from the
// normal / edge direction; ours: group B's markupContactProbe builds its own frame [I]), player +0xEDC = 0, then
// 0x76D010 (markupAttachInit).
void attachFrameInit(ChartContext& ctx) {
    if (!usable(ctx.attachedMarkup())) return;
    ctx.playerInt(0xEDC) = 0;
    markupAttachInit(ctx);
}

bool prop414(ChartContext& ctx) { return ctx.chart().propBool(0x19E).value_or(false); }

// Action 420 (PSCAF_MarkupAttachFrame).
// PreAnimEntry 0x75DBC0: with bool property 414 the attach surface class (0x75D8D0).
void attachPre(ChartContext& ctx, uint16_t) {
    if (prop414(ctx)) classifyAttach(ctx);
}
// Entry 0x7712A0: installs agent +0x51C = 0x770ED0 (0x781D80); with property 414: the class, agent +0xF0 &= ~0x3F,
// group B's markupContactProbe (0x770BA0) and the attach frame 0x76D270. Reentry 0x771320 only reinstalls +0x51C.
// +0x51C is cleared on every state change and run by 0x781D90 on message 0xA6, which Lara's update 0x71BFA0 posts
// every frame: so the callback runs every frame while 420 is in the state's chain; ours runs it as 420's Process slot
// [I: its place in the frame against the chart tick].
void attachEntry(ChartContext& ctx, uint16_t) {
    if (!prop414(ctx)) return;
    classifyAttach(ctx);
    ctx.chart().field(0xF0) &= ~0x3F;
    markupContactProbe(ctx);
    attachFrameInit(ctx);
}
// The callback 0x770ED0. Grab offset: property 422 (0x1A6, not bool) -> player +0xCE8 = it + agent +0x3D4 (the flex
// sag); property 421 (0x1A5, not bool) -> +0xCE8 = it. With property 414: markupContactProbe, 0x76D010 on the attached
// point (markupAttachInit), the +0xF0 bit 0x40 (secondary markup +0xEAC handed to the attach update instead) cleared;
// then 0x708830 (the attach update with the property 412 / 413 / 425 / 429 / 418 / 434 / 415 / 424 / 426 / 390 / 391
// switches: not ported, counted) and with property 414 the attach surface class of the attached markup.
void attachTick(ChartContext& ctx, uint16_t id) {
    ChartRuntime& rt = ctx.chart();
    if (const Property* p = rt.property(0x1A6); p && p->type != 2) ctx.playerFloat(0xCE8) = rt.charts().propFloat(*p) + ctx.agentFloat(0x3D4);
    if (const Property* p = rt.property(0x1A5); p && p->type != 2) ctx.playerFloat(0xCE8) = rt.charts().propFloat(*p);
    const bool on = prop414(ctx);
    if (on) {
        markupContactProbe(ctx);
        markupAttachInit(ctx);
        rt.field(0xF0) &= ~0x40;
    }
    if (ctx.attachedMarkup()) unportedPart(ctx, id); // 0x708830
    if (on)
        if (const auto a = ctx.attachedMarkup()) attachSurfaceClass(ctx, a);
}
// Exit 0x771330: with property 414, 0x770AD0 (agent +0xB0 frame back to identity, player +0xED8 = +0xED4, +0xED4 = 2)
// and agent +0xF0 &= ~0x3F.
void attachExit(ChartContext& ctx, uint16_t) {
    if (!prop414(ctx)) return;
    for (int i = 0; i < 16; ++i) ctx.agentFloat(0xB0 + 4 * i) = i % 5 == 0 ? 1.0f : 0.0f;
    ctx.playerInt(0xED8) = ctx.playerInt(0xED4);
    ctx.playerInt(0xED4) = 2;
    ctx.chart().field(0xF0) &= ~0x3F;
}

// Action 402 (PSCAF_AttachToMarkupFrame): Entry 0x76D470 = 0x76D270 on the attached markup, Process 0x76D210 =
// 0x76D010 on the attached point (markupAttachInit); Exit 0x6457A0.
void attachFrameEntry(ChartContext& ctx, uint16_t) { attachFrameInit(ctx); }
void attachFrameProcess(ChartContext& ctx, uint16_t) { markupAttachInit(ctx); }

} // namespace

// 0x6F1950: the profile table, object data +0xDE4 count and +0xDE8 {id, record} pairs. Record fields as 0x771830 /
// 0x76FFC0 / 0x724F50 / 0x7296C0 read them (Game.hpp MarkupProfile). Class records (0x729540): +0 / +4 the flag bits
// set when an entry passes, +8 flags, +0xC / +0x10 the type list (0x7232E0), +0x14 / +0x18 the entries; an entry
// (0x729420): +0xC / +0x10 its bits, +0x14 flags, +0x18 / +0x1C types, +0x20 / +0x24 attach types, +0x28 / +0x2C its
// tests; a test (0x728F00): +0 kind, +4 / +8 point sources, +0xC value, +0x18 flags [O: decomp, lara.drm records].
std::optional<MarkupProfile> decodeProfile(const Charts& c, int id) {
    const auto u32 = [&](uint32_t at) {
        uint32_t v = 0;
        if (at + 4 <= c.bytes.size()) std::memcpy(&v, c.bytes.data() + at, 4);
        return v;
    };
    const auto f32 = [&](uint32_t at) {
        const uint32_t v = u32(at);
        float f;
        std::memcpy(&f, &v, 4);
        return f;
    };
    const auto byte = [&](uint32_t at) { return at < c.bytes.size() ? c.bytes[at] : uint8_t(0); };
    const uint32_t d = c.dataOffset;
    const auto table = c.pointer(d + 0xDE8);
    if (!table) return std::nullopt;
    std::optional<uint32_t> rec;
    for (uint32_t k = 0, n = u32(d + 0xDE4); k < n && k < 4096; ++k)
        if (int(u32(*table + 8 * k)) == id) {
            rec = c.pointer(*table + 8 * k + 4);
            break;
        }
    if (!rec) return std::nullopt;
    const uint32_t r = *rec;
    MarkupProfile p;
    p.id = id;
    p.base = u32(r + 0xCC), p.bone = byte(r + 0xC8);
    p.offset = vec3(f32(r + 0x58), f32(r + 0x5C), f32(r + 0x60));
    p.lead = f32(r + 0xD0);
    p.velocityMode = u32(r + 4);
    p.fixedVelocity = (u32(r) & 1) && p.velocityMode == 0;
    p.velocity = vec3(f32(r + 8), f32(r + 0xC), f32(r + 0x10));
    p.heading = u32(r + 0xA0);
    p.snap[0] = f32(r + 0xAC), p.snap[1] = f32(r + 0xB0), p.snap[2] = f32(r + 0xB4);
    p.minT = f32(r + 0x20), p.coneDeg = f32(r + 0x24);
    p.minDist = int16_t(u32(r + 0x28) & 0xFFFF);
    if (const auto lr = c.pointer(d + 0x584)) p.cornerZone = int16_t(u32(*lr + 0x9E) & 0xFFFF); // 0x737B30's +0x9E
    if (const auto br = c.pointer(d + 0x5BC)) // 0x6FE040's end insets on the beam class record
        p.beamInset[0] = int16_t(u32(*br + 0x98) & 0xFFFF), p.beamInset[1] = int16_t(u32(*br + 0x98) >> 16);
    const auto capsuleOf = [&](uint32_t slot, MarkupProfile::Capsule& out) {
        if (const auto cr = c.pointer(d + slot))
            out = {vec3(f32(*cr + 0x30), f32(*cr + 0x34), f32(*cr + 0x38)), f32(*cr + 0x54), f32(*cr + 0x58)};
    };
    capsuleOf(0x594, p.poleH), capsuleOf(0x58C, p.poleV), capsuleOf(0x5B8, p.hold);
    p.ladderFaceDeg = float(byte(d + 0x775));
    p.flags = u32(r + 0x2C);
    const int frames = int32_t(u32(r + 0xA4));
    p.frames = frames >= 1 ? frames : c.data<int16_t>(0xBDE);
    if (const auto s = c.pointer(r + 0x8C))
        for (uint32_t k = 0, n = u32(r + 0x88); k < n && k < 16; ++k)
            p.starts.push_back({int(u32(*s + 12 * k)), f32(*s + 12 * k + 4), f32(*s + 12 * k + 8)});
    const auto list8 = [&](uint32_t countAt, uint32_t ptrAt) {
        auto v = std::make_unique<std::vector<uint8_t>>();
        if (const auto pl = c.pointer(ptrAt))
            for (uint32_t k = 0, n = u32(countAt); k < n && k < 256; ++k) v->push_back(byte(*pl + k));
        p.listStore.push_back(std::move(v));
        return std::span<const uint8_t>(*p.listStore.back());
    };
    // One record's entries into `out`; its own type list and flags. False: malformed.
    const auto record = [&](uint32_t at, std::vector<MarkupEntry>& out, std::vector<uint8_t>& types, uint8_t& flags) {
        flags = byte(at + 8);
        const auto t = list8(at + 0xC, at + 0x10);
        types.assign(t.begin(), t.end());
        const auto ents = c.pointer(at + 0x18);
        const uint32_t n = u32(at + 0x14);
        if (n && !ents) return false;
        for (uint32_t e = 0; e < n && e < 64; ++e) {
            const auto en = c.pointer(*ents + 4 * e);
            if (!en) return false;
            MarkupEntry me{};
            me.flags = byte(*en + 0x14);
            me.offset = {f32(*en), f32(*en + 4), f32(*en + 8)};
            me.types = list8(*en + 0x18, *en + 0x1C);
            me.attach = list8(*en + 0x20, *en + 0x24);
            auto bits = std::make_unique<std::vector<uint16_t>>();
            if (const auto bl = c.pointer(*en + 0xC))
                for (uint32_t k = 0, m = u32(*en + 0x10); k < m && k < 64; ++k) bits->push_back(uint16_t(u32(*bl + 4 * k)));
            p.bitStore.push_back(std::move(bits));
            me.bits = *p.bitStore.back();
            auto tests = std::make_unique<std::vector<MarkupTest>>();
            const auto tl = c.pointer(*en + 0x2C);
            for (uint32_t k = 0, m = u32(*en + 0x28); tl && k < m && k < 64; ++k) {
                const auto ts = c.pointer(*tl + 4 * k);
                if (!ts) return false;
                tests->push_back({uint8_t(u32(*ts)), uint8_t(u32(*ts + 4)), uint8_t(u32(*ts + 8)), uint8_t(u32(*ts + 0x18)), f32(*ts + 0xC)});
            }
            p.testStore.push_back(std::move(tests));
            me.tests = *p.testStore.back();
            out.push_back(me);
        }
        return true;
    };
    if (const auto cr = c.pointer(r + 0x94)) {
        p.classRecord = *cr - d;
        if (!record(*cr, p.classes, p.recordTypes, p.recordFlags)) p.classes.clear();
    }
    if (const auto rr = c.pointer(r + 0x9C))
        if (!record(*rr, p.result, p.resultTypes, p.resultRecordFlags)) p.result.clear();
    return p;
}

std::optional<bool> chartMarkupSearch(ChartContext& ctx, int profile) {
    const auto p = decodeProfile(ctx.chart().charts(), profile);
    if (!p) return std::nullopt;
    ctx.playerInt(0xA84) = profile; // the jump query in effect: the profile a state last ran [O: docs/GAMEPLAY.md "Saving grab"]
    const auto a = ctx.markupSearch(*p);
    if (!a) return std::nullopt;
    ctx.chartTarget = a->target ? *a : ChartContext::Aim{};
    return bool(a->target);
}

void registerGround(Registry& player, Registry&) {
    const char* g = "ground";
    player.condition(0xA, jumpQueryIs, "PSCCF_LowCeiling (jump query +0xA84)", g);
    player.condition(0x16, timeSinceMarker, "PSCCF_TimeSinceMarkerAtLeast", g);
    player.condition(0x9B, heldLongerThanProperty, "PSCCF_ButtonHeldLongerThanProperty", g);
    player.condition(0x9C, pressedWithin, "PSCCF_ButtonPressedWithin", g);
    player.condition(0xD5, animFrameAtLeast, "SCCondFunction_AnimFrame_AtLeast", g);
    player.condition(0x12D, footSupport, "PSCCF_FootSupport", g);
    player.condition(0x192, moveDir, "PSCCF_MoveDir", g);
    player.condition(0x194, moveDirLeftRight, "PSCCF_MoveDirLeftOrRight", g);
    player.condition(0x197, moveDirMomentum, "PSCCF_MoveDirMomentum", g);
    player.condition(0x198, moveDirDownhill, "PSCCF_MoveDirDownhill", g);
    player.condition(0x19E, moveDirSpherical, "PSCCF_MoveDirSpherical", g);
    player.condition(0x1F7, lowCeiling, "PSCCF_LowCeiling", g);
    player.condition(0x1FA, inCombatMode, "PSCCF_InCombatMode", g);
    player.condition(0x1FB, usingGear, "PSCCF_UsingGear", g); // empty hands only; a held item stays unported
    player.condition(0x1E0, comboMove, "PSCCF_ComboMove", g);
    player.condition(0x1E5, envProbeClass32, "PSCCF_EnvProbeClass32", g);
    player.condition(0x1EA, envProbeClass, "PSCCF_EnvProbeClass", g);
    player.condition(0x1EF, envProbeClassB, "PSCCF_EnvProbeClassB", g);
    player.condition(0x1FC, agentFlag345, "PSCCF_AgentFlag345", g);
    player.condition(0x208, onStairs, "PSCCF_OnStairs", g);
    player.condition(0x1FD, onSlopeSlide, "PSCCF_OnSlopeSlide", g);
    player.condition(0x203, wallJumpAttach, "PSCCF_WallJumpAttach", g);
    player.condition(0x205, canSprint, "CanSprint", g);
    player.condition(0x206, isWallSlide, "PSCCF_IsWallSlide", g);
    player.condition(0x207, isInCheatMode, "PSCCF_IsInCheatMode", g);
    player.condition(0x214, stateFlag898, "PSCCF_StateFlag898", g);

    player.action(7, {noop}, "PSCAF_NoOp7", g);
    player.action(409, {noop, nullptr, nullptr, nullptr, noop, noop}, "PSCAF_Noop", g);
    player.action(201, {leanBlendEntry, nullptr, nullptr, nullptr, leanBlendExit, leanBlendProcess}, "PSCAF_LocomotionLeanBlend", g);
    player.action(207, {.process = markupLengthBlend}, "PSCAF_MarkupLengthBlend", g);
    player.action(210, {hurdleSpeed, nullptr, hurdleHeightBlend, hurdleHeightBlend, nullptr, hurdleSpeed}, "PSCAF_HurdleHeightBlendAndSpeed", g);
    player.action(221, {runLeanEntry, nullptr, nullptr, nullptr, runLeanExit, runLeanProcess}, "PSCAF_RunLeanToStick", g);
    player.action(222, {twistEntry, nullptr, nullptr, nullptr, twistExit, twistProcess}, "PSCAF_TorsoTwistTowardHeading", g);
    player.action(231, {.entry = alignToTarget}, "PSCAF_AlignToTargetOverAnim", g);
    player.action(407, {noop, nullptr, nullptr, nullptr, detachMarkupExit}, "PSCAF_DetachMarkupOnExit", g);
    player.action(411, {grabHelperEntry, grabHelperReentry, grabHelperPre, grabHelperPre, grabHelperExit, grabHelperProcess}, "PSCAF_GrabHelperObject", g);
    player.action(415, {flexSagReset, nullptr, nullptr, nullptr, flexSagReset, flexSagProcess}, "PSCAF_FlexMarkupSag", g);
    player.action(423, {scheduledSearch<Entry>, scheduledSearch<Reentry>, scheduledSearch<PreEntry>, scheduledSearch<PreEntry>,
                        scheduledSearch<Exit>, scheduledSearch<Process>}, "PSCAF_ScheduledJumpTargetSearch", g);
    player.action(425, {nullptr, nullptr, nullptr, nullptr, noop, lookAtProcess}, "PSCAF_LookAtAttachPoint", g);
    player.action(501, {.process = stickScaledAnimRate}, "PSCAF_StickScaledAnimRate", g);
    player.action(509, {setAnimRate, setAnimRate, nullptr, nullptr, setAnimRate, setAnimRate}, "PSCAF_SetAnimRate", g);
    player.action(515, {jumpRefEntry, jumpRefReentry, nullptr, nullptr, noop, noop}, "PSCAF_RecordJumpRefHeight", g);
    player.action(517, {.preEntry = clearJumpTarget}, "PSCAF_ClearJumpTarget", g);
    player.action(571, {poleControlFrame, nullptr, nullptr, nullptr, poleControlFrame, poleControlFrame}, "PSCAF_PoleControlFrame", g);
    // Every slot the registry fills runs unportedSlot (slots_table in actions.json).
    player.action(220, {.preEntry = unportedSlot, .preReentry = unportedSlot, .process = unportedSlot}, "PSCAF_ResolveIKTargetBone", g);
    player.action(230, {.preEntry = unportedSlot, .preReentry = unportedSlot}, "PSCPF_MarkUpAttach_SetAnimTarget", g);
    player.action(236, {.entry = unportedSlot}, "PSCAF_AlignRootMotionToTarget", g);
    player.action(323, {.preEntry = unportedSlot, .exit = unportedSlot}, "PSCAF_GearAttachmentVisibility", g);
    player.action(402, {attachFrameEntry, nullptr, nullptr, nullptr, noop, attachFrameProcess}, "PSCAF_AttachToMarkupFrame", g);
    player.action(403, {unportedSlot, nullptr, nullptr, nullptr, noop, unportedSlot}, "PSCAF_JumpTargetOrientAndTrack", g);
    // Reentry 0x771320 only reinstalls the callback, which ours runs as the Process slot.
    player.action(420, {attachEntry, noop, attachPre, nullptr, attachExit, attachTick}, "PSCAF_MarkupAttachFrame", g);
    player.action(426, {.process = unportedSlot}, "PSCAF_LimbIKOffNearSegmentEnd", g);
    player.action(502, {.process = unportedSlot}, "PSCAF_AnalogLocomotionVelocity", g);
    // 0x74D980 on entry / re-entry: the state's request kind 2 arms a LadderRungAlignAnimTarget (0x6F0FF0) for the
    // graph node's clip (S1055 / S1057 / S1060, the only states with 505) [O]
    const auto rungAlign = [](ChartContext& ctx, uint16_t) { ctx.ladderRungAlign(); };
    player.action(505, {.preEntry = rungAlign, .preReentry = rungAlign, .exit = unportedSlot, .process = noop}, "PSCAF_LadderRungAlign", g);
    player.action(508, {unportedSlot, nullptr, nullptr, nullptr, unportedSlot, unportedSlot}, "PSCAF_SlideSpeedController", g);
}

} // namespace ou::game::chart
