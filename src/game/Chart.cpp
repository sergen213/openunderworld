// Lara's state charts: loader, runtime, registry, chart-dump. Evidence: docs/re/PLAYER_CHART.md "Runtime".
#include "game/Chart.hpp"

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"
#include "game/ObjData.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <ostream>
#include <set>
#include <span>

namespace ou::game::chart {

namespace {

template <class T>
T rd(std::span<const uint8_t> d, size_t o) {
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

float asF32(uint32_t raw) {
    float f;
    std::memcpy(&f, &raw, 4);
    return f;
}

Variable tuning(const Charts& c, const Property& p) {
    if (p.target < 0) return {};
    return {rd<uint32_t>(c.bytes, size_t(p.target)), rd<uint32_t>(c.bytes, size_t(p.target) + 4)};
}

} // namespace

bool Transition::hasFlag(int16_t f) const { return std::ranges::find(flags, f) != flags.end(); }

int Variable::asInt() const {
    if (type == 3 || type == 4) return int(asF32(raw)); // _ftol2: truncation
    if (type == 5) return (raw & 0xFF) != 0;
    return int32_t(raw);
}
float Variable::asFloat() const {
    if (type == 3 || type == 4) return asF32(raw);
    if (type == 5) return (raw & 0xFF) != 0 ? 1.0f : 0.0f;
    return float(int32_t(raw));
}

int Charts::value(const Cond& c) const {
    if (!c.fromVariable()) return c.value;
    return c.value >= 0 && size_t(c.value) < variables.size() ? variables[size_t(c.value)].asInt() : 0;
}
float Charts::valueF(const Cond& c) const {
    if (!c.fromVariable()) return float(c.value);
    return c.value >= 0 && size_t(c.value) < variables.size() ? variables[size_t(c.value)].asFloat() : 0.0f;
}
std::optional<uint32_t> Charts::pointer(uint32_t at) const {
    const auto it = pointers.find(at);
    return it == pointers.end() ? std::nullopt : std::optional<uint32_t>(it->second);
}
bool Charts::propBool(const Property& p) const {
    switch (p.type) {
    case 0x02: return (p.raw & 0xFF) != 0;
    case 0x10: return p.raw != 0;
    case 0x29: {
        const Variable v = tuning(*this, p);
        return v.type == 3 ? asF32(v.raw) != 0.0f : v.type == 5 ? (v.raw & 0xFF) != 0 : v.raw != 0;
    }
    default: return false;
    }
}
int Charts::propInt(const Property& p) const {
    switch (p.type) {
    case 0x02: return int(p.raw & 0xFF);
    case 0x07: case 0x09: case 0x0A: case 0x0B: case 0x10: case 0x11: case 0x17: case 0x1A: case 0x1D: case 0x23:
        return int32_t(p.raw);
    case 0x0C: case 0x0D: return int(asF32(p.raw));
    case 0x29: return tuning(*this, p).asInt();
    default: return 0;
    }
}
float Charts::propFloat(const Property& p) const {
    switch (p.type) {
    case 0x0C: case 0x0D: return asF32(p.raw);
    case 0x10: case 0x11: return float(int32_t(p.raw));
    case 0x29: return tuning(*this, p).asFloat();
    default: return 0.0f;
    }
}

// ---- loader -------------------------------------------------------------------------------------

std::optional<Charts> loadCharts(const ObjData& o) {
    Charts c;
    c.bytes.assign(o.drm.sections[o.section].data.begin(), o.drm.sections[o.section].data.end());
    for (const auto& [at, p] : cdc::sectionPointers(o.drm, o.section))
        if (p.section == o.section) c.pointers.emplace(at, p.offset);
    c.dataOffset = o.offset;
    const auto& d = c.bytes;
    const auto ptr = [&](uint32_t at) { return c.pointer(at); };
    const auto array = [&](uint32_t at, size_t cap) { // a run of pointers
        std::vector<uint32_t> out;
        for (uint32_t k = 0; k < cap; ++k) {
            const auto p = ptr(at + 4 * k);
            if (!p) break;
            out.push_back(*p);
        }
        return out;
    };
    const auto s16s = [&](std::optional<uint32_t> at, uint32_t n) {
        std::vector<int16_t> v;
        if (at) for (uint32_t k = 0; k < n; ++k) v.push_back(rd<int16_t>(d, *at + 2 * k));
        return v;
    };
    const auto readChart = [&](uint32_t h, std::string name) -> std::optional<ChartData> {
        ChartData ch;
        ch.name = std::move(name);
        ch.root = rd<int16_t>(d, h), ch.agent = rd<int16_t>(d, h + 2);
        const uint32_t ns = rd<uint16_t>(d, h + 8), nt = rd<uint16_t>(d, h + 0xA);
        const auto sa = ptr(h + 0xC), ta = ptr(h + 0x10), ea = ptr(h + 0x14);
        if (!sa || !ta) return std::nullopt;
        for (uint32_t i = 0; i < ns; ++i) {
            State s;
            if (const auto so = ptr(*sa + 4 * i)) {
                s.requestKind = rd<int16_t>(d, *so + 2), s.request = rd<int16_t>(d, *so + 4), s.parent = rd<int16_t>(d, *so + 6);
                const uint32_t np = rd<uint16_t>(d, *so + 0xA), na = rd<uint16_t>(d, *so + 0xC);
                const uint32_t ntr = rd<uint16_t>(d, *so + 0xE), nev = rd<uint16_t>(d, *so + 0x10);
                if (const auto pp = ptr(*so + 0x14))
                    for (uint32_t k = 0; k < np; ++k) {
                        const uint32_t e = *pp + 8 * k;
                        const auto t = ptr(e + 4);
                        s.props.push_back({rd<uint16_t>(d, e), rd<uint16_t>(d, e + 2), rd<uint32_t>(d, e + 4), t ? int64_t(*t) : -1});
                    }
                if (const auto ap = ptr(*so + 0x18))
                    for (uint32_t k = 0; k < na; ++k) s.actions.push_back(rd<uint16_t>(d, *ap + 2 * k));
                s.transitions = s16s(ptr(*so + 0x1C), ntr);
                s.events = s16s(ptr(*so + 0x20), nev);
            }
            ch.states.push_back(std::move(s));
        }
        for (uint32_t i = 0; i < nt; ++i) {
            Transition t;
            if (const auto to = ptr(*ta + 4 * i)) {
                t.target = rd<int16_t>(d, *to);
                const uint32_t nf = rd<uint16_t>(d, *to + 2), nc = rd<uint16_t>(d, *to + 4);
                t.flags = s16s(ptr(*to + 8), nf);
                if (const auto cp = ptr(*to + 0xC))
                    for (uint32_t k = 0; k < nc; ++k) {
                        const uint32_t e = *cp + 8 * k;
                        t.conds.push_back({rd<int16_t>(d, e), rd<int16_t>(d, e + 2), rd<int16_t>(d, e + 4), rd<uint8_t>(d, e + 6)});
                    }
            }
            ch.transitions.push_back(std::move(t));
        }
        if (ea)
            for (const uint32_t eo : array(*ea, 4096)) {
                Event e{rd<uint32_t>(d, eo), rd<uint8_t>(d, eo + 4), rd<uint8_t>(d, eo + 5), rd<uint8_t>(d, eo + 6), {}};
                if (const auto tp = ptr(eo + 0xC))
                    for (uint32_t k = 0, n = rd<uint16_t>(d, eo + 8); k < n; ++k) e.transitions.push_back(rd<int16_t>(d, *tp + 4 * k));
                ch.events.push_back(std::move(e));
            }
        return ch;
    };
    auto player = readChart(o.offset + 0x12500, "player");
    auto upper = readChart(o.offset + 0x12528, "upper");
    if (!player || !upper) return std::nullopt;
    c.player = std::move(*player), c.upper = std::move(*upper);
    if (const auto vt = ptr(o.offset + 0x18CC))
        for (const uint32_t v : array(*vt, 4096)) c.variables.push_back({rd<uint32_t>(d, v), rd<uint32_t>(d, v + 4)});
    // Anim requests: the object's patterns, each named by its first clip, with the clip's length (header +0x32 key
    // count x +0x34 time per key, anim::Clip).
    const auto list = anim::animList(o.drm);
    std::unordered_map<uint32_t, const cdc::Section*> clips;
    for (const auto& s : o.drm.sections)
        if (s.type == cdc::SectionType::Animation) clips.emplace(s.id, &s);
    for (const auto& p : anim::animPatterns(o.drm)) {
        Request r;
        r.rate = p.rate, r.mode = p.mode, r.start = p.start, r.blend = p.blend;
        if (!p.anims.empty() && p.anims[0] < list.size()) {
            const auto& e = list[p.anims[0]];
            r.clip = e.name;
            if (const auto it = clips.find(e.animId); it != clips.end() && it->second->data.size() >= 0x36) {
                const auto& cd = it->second->data;
                r.seconds = float(double(rd<uint16_t>(cd, 0x32)) * rd<uint16_t>(cd, 0x34) / anim::kTimeUnitsPerSecond);
            }
        }
        c.requests.push_back(std::move(r));
    }
    return c;
}

std::optional<Charts> loadCharts(const cdc::BigfileSet& bigfile) {
    const auto o = objData(bigfile, "lara");
    return o ? loadCharts(*o) : std::nullopt;
}

// ---- registry -----------------------------------------------------------------------------------

void Registry::condition(int16_t id, CondFn fn, std::string name, std::string family) {
    conds_[id] = {fn, std::move(name), std::move(family)};
}
void Registry::action(uint16_t id, ActionFns fns, std::string name, std::string family) {
    actions_[id] = {fns, std::move(name), std::move(family)};
}
void Registry::property(uint16_t id, PropFn fn, std::string name, std::string family) {
    props_[id] = {fn, std::move(name), std::move(family)};
}
const Registry::CondEntry* Registry::condition(int16_t id) const {
    const auto it = conds_.find(id);
    return it == conds_.end() ? nullptr : &it->second;
}
const Registry::ActionEntry* Registry::action(uint16_t id) const {
    const auto it = actions_.find(id);
    return it == actions_.end() ? nullptr : &it->second;
}
const Registry::PropEntry* Registry::property(uint16_t id) const {
    const auto it = props_.find(id);
    return it == props_.end() ? nullptr : &it->second;
}
namespace {
struct Registries {
    Registry player, upper;
    Registries() { registerFamilies(player, upper); }
};
const Registries& registries() {
    static const Registries r;
    return r;
}
} // namespace
const Registry& Registry::player() { return registries().player; }
const Registry& Registry::upper() { return registries().upper; }

int actionValue(const ChartContext::Action& a) {
    const int frames = std::max(1, int(a.seconds * 30.0f)); // 0x7607C0: ms x 30 / 1000, at least 1
    if (a.down) return a.edge ? 1 : frames + 1;
    return a.edge ? -1 : -1 - frames;
}

// ---- runtime ------------------------------------------------------------------------------------

ChartRuntime::ChartRuntime(const Charts& charts, const ChartData& data, const Registry& registry)
    : charts_(charts), data_(data), reg_(registry) {}

int ChartRuntime::parent() const {
    const State* s = data_.state(state_);
    return s ? s->parent : -1;
}
int ChartRuntime::request() const {
    const State* s = data_.state(state_);
    return s ? s->request : -1;
}
// The state's request as an anim pattern: request kinds 0 / 1 (0x444BA0 -> 0x4428A0). Kind 2 asks the anim graph for a
// node instead (0x443720 -> 0x43E250): not a pattern, so the chart keeps no clip clock for it.
const Request* ChartRuntime::pattern() const {
    const State* s = data_.state(state_);
    if (!s || s->request < 0 || s->requestKind == 2 || size_t(s->request) >= charts_.requests.size()) return nullptr;
    return &charts_.requests[size_t(s->request)];
}
float ChartRuntime::animLengthFrames() const {
    const Request* p = pattern();
    return p ? p->seconds * 30.0f : 0.0f;
}
bool ChartRuntime::animLoops() const {
    const Request* p = pattern();
    return p && p->mode == 2;
}
const Property* ChartRuntime::property(uint16_t id) const {
    const auto it = props_.find(id);
    return it == props_.end() ? nullptr : it->second;
}
std::optional<bool> ChartRuntime::propBool(uint16_t id) const {
    const Property* p = property(id);
    return p ? std::optional<bool>(charts_.propBool(*p)) : std::nullopt;
}
std::optional<int> ChartRuntime::propInt(uint16_t id) const {
    const Property* p = property(id);
    return p ? std::optional<int>(charts_.propInt(*p)) : std::nullopt;
}
std::optional<float> ChartRuntime::propFloat(uint16_t id) const {
    const Property* p = property(id);
    return p ? std::optional<float>(charts_.propFloat(*p)) : std::nullopt;
}
bool ChartRuntime::chainHasAction(int state, uint16_t action) const {
    for (const State* s = data_.state(state); s; s = data_.state(s->parent))
        if (std::ranges::find(s->actions, action) != s->actions.end()) return true;
    return false;
}
const Transition* ChartRuntime::transition(int i) const {
    return i >= 0 && size_t(i) < data_.transitions.size() ? &data_.transitions[size_t(i)] : nullptr;
}
bool ChartRuntime::unported(const Cond&) {
    unportedHit_ = true;
    return false;
}

// 0x785920: every condition, in order, through its registered function; the first false one stops.
bool ChartRuntime::holds(ChartContext& ctx, const Transition& t) {
    for (const Cond& c : t.conds) {
        ++stats.evaluations;
        const auto* e = reg_.condition(c.id);
        if (!e || !e->fn) {
            ++stats.unportedConds[c.id];
            return false;
        }
        unportedHit_ = false;
        const bool r = e->fn(ctx, c);
        if (unportedHit_) {
            ++stats.unportedConds[c.id];
            return false;
        }
        if (r == c.negate()) return false;
    }
    return true;
}

// 0x787570: the queued transition when its moment has come; else the state's own transitions in order (the first
// whose conditions hold wins), else its parent's, recursively. A winner flagged 1 waits for the animation-end tick,
// one flagged 7 for +0x53C to clear: it is queued instead.
int ChartRuntime::find(ChartContext& ctx, int s) {
    if (const Transition* q = transition(queued_); q && ((animCounter_ == 1 && q->hasFlag(1)) || (!deferFlag7 && q->hasFlag(7))))
        return queued_;
    const State* st = data_.state(s);
    if (!st) return -1;
    int found = -1;
    for (const int16_t t : st->transitions) {
        if (const Transition* tr = transition(t); tr && holds(ctx, *tr)) {
            found = t;
            break;
        }
    }
    if (found < 0) {
        if (st->parent < 0) return -1;
        found = find(ctx, st->parent);
        if (found < 0) return -1;
    }
    const Transition* tr = transition(found);
    if ((tr->hasFlag(1) && animCounter_ != 1) || (tr->hasFlag(7) && deferFlag7)) {
        queued_ = found;
        return -1;
    }
    return found;
}

void ChartRuntime::call(ChartContext& ctx, uint16_t action, ActFn ActionFns::*which) {
    const auto* e = reg_.action(action);
    if (!e) {
        ++stats.unportedActions[action];
        return;
    }
    if (const ActFn f = e->fns.*which) f(ctx, action);
}

// 0x7859A0: the old state's actions (parent first) that the new state's chain does not run too get their Exit.
void ChartRuntime::runExit(ChartContext& ctx, int from, int to) {
    const State* s = data_.state(from);
    if (!s) return;
    runExit(ctx, s->parent, to);
    for (const uint16_t a : s->actions)
        if (to < 0 || !chainHasAction(to, a)) call(ctx, a, &ActionFns::exit);
}

// 0x785AE0 (pre-anim) / 0x785A30: the new state's actions, parent first: (PreAnim)Entry for the ones new to the
// chain, (PreAnim)Reentry for the ones the old state ran too.
void ChartRuntime::runEnter(ChartContext& ctx, int to, int from, bool preAnim) {
    const State* s = data_.state(to);
    if (!s) return;
    runEnter(ctx, s->parent, from, preAnim);
    for (const uint16_t a : s->actions) {
        const bool again = from >= 0 && chainHasAction(from, a);
        call(ctx, a, preAnim ? (again ? &ActionFns::preReentry : &ActionFns::preEntry) : (again ? &ActionFns::reentry : &ActionFns::entry));
    }
}

// 0x7857D0: Process of every action of the state's chain, parent first, while the agent stays active.
void ChartRuntime::runProcess(ChartContext& ctx, int state) {
    const State* s = data_.state(state);
    if (!s) return;
    runProcess(ctx, s->parent);
    if (!active_) return;
    for (const uint16_t a : s->actions) {
        call(ctx, a, &ActionFns::process);
        if (!active_) return;
    }
}

// 0x785E70 -> 0x785CC0: the state's properties, then its parent's ..., then the root's chain; each id once per pass
// (stamp), so the most derived state's value wins. Ids none of them sets keep their last value.
void ChartRuntime::applyProperties(ChartContext& ctx, int state) {
    ++stamp_;
    int s = state;
    for (;;) {
        int top = s;
        for (const State* st = data_.state(s); st; st = data_.state(st->parent)) {
            top = int(st - data_.states.data());
            for (const Property& p : st->props) {
                uint32_t& seen = propStamp_[p.id];
                if (seen == stamp_) continue;
                seen = stamp_;
                if (const auto* e = reg_.property(p.id); e && e->fn) e->fn(ctx, p);
                props_[p.id] = &p;
            }
        }
        if (!data_.state(data_.root) || data_.root == top) break;
        s = data_.root;
    }
}

// 0x783470: the state's anim request plays; the animation-end counter restarts. 0x4428A0 -> 0x441FB0 starts the clip at
// (start - blend) frames, so that it stands at the pattern's start frame when the crossfade ends; a negative lead is cut
// to 0 (ng_running_jump_forwards 2 / 1 under Wine) [U: the wall jump's clips keep it, held at their first frame].
void ChartRuntime::enterAnim() {
    animCounter_ = 0;
    const Request* p = pattern();
    animFrames_ = p ? float(std::max(0, int(p->start) - int(p->blend))) : 0.0f;
}

// 0x787890
void ChartRuntime::change(ChartContext& ctx, int t) {
    const Transition* tr = transition(t);
    if (!tr) return;
    if (tr->hasFlag(6) && queued_ >= 0) t = queued_, tr = transition(t); // TriggerQueuedTrans
    const int target = data_.state(tr->target) ? tr->target : state_;
    if (target == state_) {
        if (animCounter_ > 0) animCounter_ = 0;
        if (!tr->hasFlag(4)) {
            lastTransition_ = t, queued_ = -1;
            return;
        }
    }
    const int old = state_;
    runExit(ctx, old, target);
    state_ = target;
    if (tr->hasFlag(2)) mirror_ = true;
    else if (tr->hasFlag(5)) mirror_ = false;
    applyProperties(ctx, target);
    runEnter(ctx, target, old, true);
    enterAnim();
    runEnter(ctx, target, old, false);
    stateTime_ = 0, queued_ = -1, lastTransition_ = t;
    ++stats.changes;
}

// 0x787C50: transitions until none fires; a change chains on in the same frame while property 0x65 is true.
void ChartRuntime::evaluate(ChartContext& ctx) {
    ++frame_;
    const int orig = state_;
    int s = state_;
    bool chain;
    do {
        chain = false;
        const int t = find(ctx, s);
        if (t < 0) return;
        change(ctx, t);
        if (active_ && state_ != orig && state_ != s && propBool(0x65).value_or(false)) ++frame_, chain = true;
        s = state_;
    } while (chain);
}

void ChartRuntime::start(ChartContext& ctx) {
    ctx.chart_ = this;
    state_ = data_.root, active_ = true, queued_ = lastTransition_ = -1, stateTime_ = totalTime_ = 0;
    applyProperties(ctx, state_);
    enterAnim();
    runEnter(ctx, state_, -1, false);
    evaluate(ctx);
}

void ChartRuntime::tick(ChartContext& ctx, float timeMult) {
    ctx.chart_ = this;
    ctx.timeMult = timeMult; // the tick's one time step: conditions and actions read ctx.timeMult
    if (!active_) return;
    totalTime_ += ctx.timeMult, stateTime_ += ctx.timeMult;
    animCounter_ += animCounter_ < 1 ? -1 : 1;
    for (const int f : {0x3C4, 0x3C8, 0x8C, 0x94}) ++fields_[f]; // agent frame counters (0x787E60; +0x8C: since a floor)
    runProcess(ctx, state_);
    if (active_ && data_.state(state_)) evaluate(ctx);
    // The requested clip's clock [I: the anim system's notification order relative to the chart tick is not pinned]:
    // a clip played to its end (once) or across a loop wrap notifies the end (0x7181A0 reasons 1 / 2 -> 0x768CF0).
    const Request* p = pattern();
    const float len = animLengthFrames(), before = animFrames_;
    animFrames_ += ctx.timeMult * (p ? p->rate : 1.0f);
    if (len > 0 && (animLoops() ? std::floor(animFrames_ / len) > std::floor(before / len) : before < len && animFrames_ >= len))
        animEnded(ctx);
}

void ChartRuntime::enter(ChartContext& ctx, int state) {
    ctx.chart_ = this;
    if (!data_.state(state)) return;
    const int old = state_;
    runExit(ctx, old, state);
    state_ = state, active_ = true;
    applyProperties(ctx, state);
    runEnter(ctx, state, old, true);
    enterAnim();
    runEnter(ctx, state, old, false);
    stateTime_ = 0, queued_ = -1, lastTransition_ = -1;
}

void ChartRuntime::animEnded(ChartContext& ctx) {
    ctx.chart_ = this;
    if (!active_ || animCounter_ >= 0) return;
    animCounter_ = 1;
    evaluate(ctx);
    if (active_ && stateTime_ > 0) post(ctx, 0x15);
}

// 0x7876E0: the state's (else the nearest ancestor's) handlers of the event, in order; the first transition whose
// conditions hold is the result. 4 = no state in the chain handles it; 0 / 1 fire `found`; 2 / 3 nothing fires (or
// it was queued: flags 1 / 7). The low bit is the handler's result byte.
int ChartRuntime::fire(ChartContext& ctx, int s, uint32_t event, int& found) {
    for (const State* st = data_.state(s); st; st = data_.state(st->parent)) {
        bool matched = false;
        int hit = -1;
        uint8_t result = 0;
        for (const int16_t ei : st->events) {
            if (hit >= 0) break;
            if (ei < 0 || size_t(ei) >= data_.events.size() || data_.events[size_t(ei)].id != event) continue;
            const Event& e = data_.events[size_t(ei)];
            matched = true;
            for (const int16_t t : e.transitions) {
                if (hit >= 0) break;
                const Transition* tr = transition(t);
                if (tr && holds(ctx, *tr)) hit = t, result = e.onFire;
                else result = e.onMiss;
            }
        }
        if (!matched) continue;
        if (hit < 0) return result ? 3 : 2;
        found = hit;
        const Transition* tr = transition(hit);
        if (tr->hasFlag(1) || tr->hasFlag(7)) {
            queued_ = hit;
            return result ? 3 : 2;
        }
        return result ? 1 : 0;
    }
    return 4;
}

bool ChartRuntime::post(ChartContext& ctx, uint32_t event) {
    ctx.chart_ = this;
    ++frame_;
    const int orig = state_;
    int found = -1;
    int r = fire(ctx, state_, event, found);
    if (r == 4) found = -1, r = fire(ctx, data_.root, event, found);
    if ((r == 0 || r == 1) && found >= 0) {
        change(ctx, found);
        if (active_ && state_ != orig)
            while (propBool(0x65).value_or(false)) {
                ++frame_;
                const int t = find(ctx, state_);
                if (t < 0) break;
                change(ctx, t);
                if (!active_ || state_ == orig) break;
            }
    }
    return r == 1 || r == 3 || r == 4;
}

// ---- chart-dump ---------------------------------------------------------------------------------

namespace {
std::string hex(int v) {
    char b[16];
    std::snprintf(b, sizeof b, "0x%x", unsigned(uint16_t(v)));
    return b;
}
} // namespace

int dump(const cdc::BigfileSet& bigfile, std::optional<int> only, bool coverage, bool upper, const std::filesystem::path* names,
         std::ostream& out) {
    const auto charts = loadCharts(bigfile);
    if (!charts) {
        out << "chart-dump: lara.drm player data not found\n";
        return 1;
    }
    const ChartData& ch = upper ? charts->upper : charts->player;
    const Registry& reg = upper ? Registry::upper() : Registry::player();
    nlohmann::json catalog;
    if (names) {
        std::ifstream f(*names);
        if (f) catalog = nlohmann::json::parse(f, nullptr, false);
    }
    const auto condName = [&](int16_t id) -> std::string {
        if (const auto* e = reg.condition(id)) return e->name;
        if (catalog.is_object())
            for (const std::string k : {hex(id), std::to_string(id)})
                if (catalog.contains(k) && catalog[k].is_object() && catalog[k].contains("name")) return catalog[k]["name"].get<std::string>();
        return "c" + hex(id);
    };
    const auto clip = [&](int s) -> std::string {
        const State* st = ch.state(s);
        if (!st || st->request < 0 || size_t(st->request) >= charts->requests.size()) return "";
        return charts->requests[size_t(st->request)].clip;
    };
    char buf[512];
    const auto showState = [&](int i) {
        const State& s = ch.states[size_t(i)];
        std::snprintf(buf, sizeof buf, "S%d req %d/k%d [%s] parent %d", i, s.request, s.requestKind, clip(i).c_str(), s.parent);
        out << buf;
        out << " props [";
        for (const auto& p : s.props) out << " " << p.id << ":" << p.type << "=" << (p.target >= 0 ? "->" + hex(int(p.target)) : std::to_string(int32_t(p.raw)));
        out << " ] acts [";
        for (const auto a : s.actions) out << " " << a << (reg.action(a) ? "" : "?");
        out << " ]\n";
        const auto showTransition = [&](int16_t t, const char* lead) {
            if (t < 0 || size_t(t) >= ch.transitions.size()) return;
            const Transition& tr = ch.transitions[size_t(t)];
            std::snprintf(buf, sizeof buf, "%sT%d -> S%d [%s] flags", lead, t, tr.target, clip(tr.target).c_str());
            out << buf << " [";
            for (const auto f : tr.flags) out << " " << f;
            out << " ] conds";
            for (const auto& c : tr.conds)
                out << " " << (c.negate() ? "!" : "") << condName(c.id) << "(" << hex(c.id) << (reg.condition(c.id) ? "" : "?") << ")/"
                    << c.group << ":" << c.value;
            out << "\n";
        };
        for (const auto t : s.transitions) showTransition(t, "   ");
        for (const auto e : s.events)
            if (e >= 0 && size_t(e) < ch.events.size()) {
                out << "   event " << ch.events[size_t(e)].id << " (" << int(ch.events[size_t(e)].onFire) << "/" << int(ch.events[size_t(e)].onMiss) << ")\n";
                for (const auto t : ch.events[size_t(e)].transitions) showTransition(t, "      ");
            }
    };
    out << ch.name << " chart: " << ch.states.size() << " states, " << ch.transitions.size() << " transitions, " << ch.events.size()
        << " events, root S" << ch.root << ", " << charts->variables.size() << " variables, " << charts->requests.size() << " requests\n";
    if (only) {
        if (*only < 0 || size_t(*only) >= ch.states.size()) {
            out << "no state " << *only << "\n";
            return 1;
        }
        showState(*only);
    } else if (!coverage) {
        for (size_t i = 0; i < ch.states.size(); ++i) showState(int(i));
    }
    if (!coverage) return 0;
    // Reachable from the root: every transition a state can take (its own, its ancestors', its events' and the root
    // chain's events), and the ancestors whose actions / properties run.
    std::vector<bool> seen(ch.states.size());
    std::deque<int> todo{ch.root};
    std::map<int16_t, uint64_t> condUses;
    std::map<uint16_t, uint64_t> actUses;
    std::set<int16_t> transSeen;
    const auto visitTransition = [&](int16_t t) {
        if (t < 0 || size_t(t) >= ch.transitions.size() || !transSeen.insert(t).second) return;
        const Transition& tr = ch.transitions[size_t(t)];
        for (const auto& c : tr.conds) ++condUses[c.id];
        if (ch.state(tr.target)) todo.push_back(tr.target);
    };
    while (!todo.empty()) {
        const int s = todo.front();
        todo.pop_front();
        if (!ch.state(s) || seen[size_t(s)]) continue;
        seen[size_t(s)] = true;
        for (const State* st = ch.state(s); st; st = ch.state(st->parent)) {
            for (const auto a : st->actions) ++actUses[a];
            for (const auto t : st->transitions) visitTransition(t);
            for (const auto e : st->events)
                if (e >= 0 && size_t(e) < ch.events.size())
                    for (const auto t : ch.events[size_t(e)].transitions) visitTransition(t);
            if (st->parent >= 0 && ch.state(st->parent)) todo.push_back(st->parent);
        }
    }
    const size_t reachable = size_t(std::ranges::count(seen, true));
    size_t condPorted = 0, actPorted = 0;
    uint64_t condUsesPorted = 0, condUsesAll = 0;
    out << "\ncoverage from S" << ch.root << ": " << reachable << " / " << ch.states.size() << " states, " << transSeen.size()
        << " transitions\nconditions (id uses ported name family):\n";
    for (const auto& [id, n] : condUses) {
        const auto* e = reg.condition(id);
        condPorted += e != nullptr, condUsesAll += n, condUsesPorted += e ? n : 0;
        std::snprintf(buf, sizeof buf, "  %-6s %5llu %s %s %s\n", hex(id).c_str(), (unsigned long long)n, e ? "yes" : "NO ",
                      condName(id).c_str(), e ? e->family.c_str() : "");
        out << buf;
    }
    out << "actions (id uses ported name family):\n";
    for (const auto& [id, n] : actUses) {
        const auto* e = reg.action(id);
        actPorted += e != nullptr;
        std::snprintf(buf, sizeof buf, "  %-6u %5llu %s %s %s\n", unsigned(id), (unsigned long long)n, e ? "yes" : "NO ",
                      e ? e->name.c_str() : "", e ? e->family.c_str() : "");
        out << buf;
    }
    std::snprintf(buf, sizeof buf, "summary: conditions %zu / %zu ported (%.1f %% of %llu uses), actions %zu / %zu ported\n", condPorted,
                  condUses.size(), condUsesAll ? 100.0 * double(condUsesPorted) / double(condUsesAll) : 0.0,
                  (unsigned long long)condUsesAll, actPorted, actUses.size());
    out << buf;
    return 0;
}

} // namespace ou::game::chart
