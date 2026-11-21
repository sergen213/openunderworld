// Group CD: Lara's climb / ladder / pillar / perch / pole / beam / wall hug / slide / climb-out conditions and actions,
// and the shared gates every ground exit tests (docs/re/PLAYER_CHART.md "Conditions", "Actions"; ownership
// openunderworld-work/chart/{conditions,actions}_owner.json by_group.CD). Behaviour after tru.exe, one function per
// registry entry, the decompiled tests in their order. A condition returns the raw truth (the runtime applies the
// negation; the exceptions are noted per function and folded back here); where the game cannot answer it returns
// ctx.chart().unported(c). Actions record their effect chart-side (ChartContext player / agent stores); shadow mode
// never applies them to the live Player.
#include "game/Chart.hpp"

#include <cmath>

namespace ou::game::chart {

namespace {

// 0x781FF0: a condition's mode; group 1000 makes it 1000 (no case matches it).
int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; }

// A function that applies the negation itself and returns a value the runtime must not negate again: hand back what
// the runtime's negation turns into `result`.
bool raw(const Cond& c, bool result) { return c.negate() ? !result : result; }

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

} // namespace

void registerClimb(Registry& player, Registry&) {
    player.condition(0xCA, criticalSection, "AnimationCriticalSection", "gate");
    player.condition(0x1F5, inWater, "InWater", "gate");
    player.condition(0x204, mantleLedgeAhead, "MantleLedgeAhead", "gate");
    player.condition(0x836, gearSlotIs, "GearSlotIs", "gate");
}

} // namespace ou::game::chart
