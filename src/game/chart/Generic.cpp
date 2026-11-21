// Generic chart conditions: the ones that read only the chart agent itself, the input actions and the stick
// (docs/re/PLAYER_CHART.md "Runtime"). Behaviour after tru.exe; each function returns the raw truth (the runtime
// applies the condition's negation).
#include "game/Chart.hpp"

#include <cmath>

namespace ou::game::chart {

namespace {

// tru.exe 0x781FF0: a condition's mode; group 1000 makes it 1000 (no case matches it).
int mode(const Cond& c) { return c.fromVariable() ? 1000 : c.value; }

// 0x782700 (id 1): always true.
bool always(ChartContext&, const Cond&) { return true; }

// 0x783B70 (id 0x14): the state's time, +0x508 truncated to whole 30 Hz frames, >= the value (0x782B40 mode 1).
bool stateTime(ChartContext& ctx, const Cond& c) { return int(ctx.chart().stateTime()) >= ctx.chart().value(c); }

// Player conditions 0x65..0x6E (0x760A30..0x760B30 -> 0x7607C0): one input action's timing each.
int buttonAction(int16_t id) {
    switch (id) {
    case 0x65: return 52; // Jump
    case 0x66: return 50; // Crouch
    case 0x67: return 42; // Sprint
    case 0x68: case 0x69: return 99; // Interact
    case 0x6A: return 112;
    case 0x6B: return 34;
    case 0x6C: return 25; // Manual Aim
    case 0x6D: return 28;
    case 0x6E: return 79;
    default: return -1;
    }
}
bool button(ChartContext& ctx, const Cond& c) {
    const ChartContext::Action a = ctx.action(buttonAction(c.id));
    if (!a.known) return ctx.chart().unported(c);
    const int v = actionValue(a);
    switch (mode(c)) { // table 0x7609C0
    case 0: return v < 0;    // up
    case 1: return v > 0;    // down
    case 2: return v == -1;  // released this frame
    case 3: return v == 1;   // pressed this frame
    case 0x65: return int(ctx.chart().charts().data<int16_t>(0x6E2)) < v; // held longer than Lara data +0x6E2 (15)
    default: return false;   // "PSCCF_Button unhandled condition"
    }
}

// Player conditions 0x96..0x99 / 0x9E (0x760B50 / 0x760BC0 / 0x760C40 / 0x760CB0): the flags of the action the
// value names (resolved through the variables, 0x7820E0): 0x04 down, 0x10 pressed, 0x20 released this frame.
bool actionFlag(ChartContext& ctx, const Cond& c) {
    const ChartContext::Action a = ctx.action(ctx.chart().value(c));
    if (!a.known) return ctx.chart().unported(c);
    switch (c.id) {
    case 0x96: return a.down;
    case 0x97: return !a.down;
    case 0x98: case 0x9E: return a.down && a.edge;
    case 0x99: return !a.down && a.edge;
    default: return false;
    }
}

// Player condition 0x1E (0x764B40 -> 0x783BD0 mode 1): the fall so far (player +0x6DC - z) >= the value.
bool fall(ChartContext& ctx, const Cond& c) { return ctx.fallDistance() >= ctx.chart().valueF(c); }

// Base condition 0xC9 (0x783840, SCCondFunction_AnimationEnded) on the agent's end counter +0x24, counting the clip as
// ended once its time reached its length while the counter is under 2.
bool animationEnded(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    int v = rt.animCounter();
    // 0x783840: the channel time (0x438910) past the clip's last key time (0x438930) counts as ended.
    if (const float len = rt.animLengthFrames(); len > 0 && !rt.animLoops() && rt.animFrames() > len && v < 2) v = 1;
    switch (mode(c)) {
    case 0: return v < 1;
    case 1: return v > 0;
    case 2: return v == 0;
    case 3: return v == 1;
    default: return false;
    }
}

// Player condition 0x191 (0x761CD0 -> 0x786AF0, SCCTest_MoveSpeed): the stick's level in zones 1 / 2 / 4 / 8 by the
// thresholds properties 0x20A / 0x20B / 0x20C, each widened (from below) or narrowed (from above) by the hysteresis
// 0x20D / 0x20E / 0x20F around the zone of the last frame it was evaluated (agent +0x74). Values 0x191..0x194 test
// zones 1 / 2 / 4 / 8 (none, walk, jog, run); 0x195 ("sprint") tests 8.
bool moveSpeed(ChartContext& ctx, const Cond& c) {
    ChartRuntime& rt = ctx.chart();
    auto& cache = rt.cache(c.id);
    int zone = cache.value;
    if (cache.frame != rt.frame()) {
        const auto t0 = rt.propFloat(0x20A), t1 = rt.propFloat(0x20B), t2 = rt.propFloat(0x20C);
        const auto h0 = rt.propFloat(0x20D), h1 = rt.propFloat(0x20E), h2 = rt.propFloat(0x20F);
        if (!t0 || !t1 || !t2 || !h0 || !h1 || !h2) return rt.unported(c);
        int& last = rt.field(0x74);
        float a = *t0, b = *t1, d = *t2;
        switch (last == 0 ? 1 : last) {
        case 1: a += *h0, b += *h1, d += *h2; break;
        case 2: a -= *h0, b += *h1, d += *h2; break;
        case 4: a -= *h0, b -= *h1, d += *h2; break;
        case 8: a -= *h0, b -= *h1, d -= *h2; break;
        default: break; // "LastZone not valid": the plain thresholds
        }
        const float speed = ctx.stickLevel();
        zone = speed < a ? 1 : speed < b ? 2 : speed < d ? 4 : 8;
        last = zone, cache = {rt.frame(), zone};
    }
    switch (mode(c)) {
    case 0x191: return zone & 1;
    case 0x192: return zone & 2;
    case 0x193: return zone & 4;
    case 0x194: case 0x195: return zone & 8;
    default: return false;
    }
}

} // namespace

void registerGeneric(Registry& player, Registry& upper) {
    for (Registry* r : {&player, &upper}) {
        r->condition(0x1, always, "Always", "generic");
        r->condition(0x14, stateTime, "StateTime", "generic");
        r->condition(0xC9, animationEnded, "AnimationEnded", "generic");
    }
    for (int16_t id = 0x65; id <= 0x6E; ++id) player.condition(id, button, "Button", "generic");
    for (const int16_t id : {0x96, 0x97, 0x98, 0x99, 0x9E}) player.condition(id, actionFlag, "ActionFlag", "generic");
    player.condition(0x1E, fall, "FallDistance", "generic");
    player.condition(0x191, moveSpeed, "MoveSpeed", "generic");
}

} // namespace ou::game::chart
