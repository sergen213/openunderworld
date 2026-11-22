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
                s.requestKind = rd<int16_t>(d, *so + 2), s.request = rd<int16_t>(d, *so + 4), s.parent = rd<int16_t>(d, *so + 6); s.word8 = rd<uint16_t>(d, *so + 8);
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
        r.rate = p.rate, r.mode = p.mode, r.start = p.start, r.blend = p.blend, r.nextBlend = p.nextBlend, r.mirror = p.mirror;
        if (!p.anims.empty() && p.anims[0] < list.size()) {
            const auto& e = list[p.anims[0]];
            r.clip = e.name;
            if (const auto it = clips.find(e.animId); it != clips.end() && it->second->data.size() >= 0x36) {
                const auto& cd = it->second->data;
                r.seconds = float(double(rd<uint16_t>(cd, 0x32)) * rd<uint16_t>(cd, 0x34) / anim::kTimeUnitsPerSecond);
                r.keys = rd<uint16_t>(cd, 0x32), r.keyTime = rd<uint16_t>(cd, 0x34);
            }
        }
        c.requests.push_back(std::move(r));
    }
    // The anim graph's nodes (kind-2 requests): ObjectBase +0x4C count, +0x50 -> {u32 DTP id, u32} [O-data, lara: 218].
    const auto& base = o.drm.sections[o.base];
    if (base.data.size() >= 0x54)
        if (const auto table = cdc::internalPointer(base, 0x50)) {
            const uint32_t n = rd<uint32_t>(base.data, 0x4C);
            for (uint32_t i = 0; i < n && *table + 8 * i + 4 <= base.data.size(); ++i) {
                Charts::GraphNode g;
                const uint32_t id = rd<uint32_t>(base.data, *table + 8 * i);
                for (const auto& sec : o.drm.sections)
                    if (sec.type == cdc::SectionType::DtpData && sec.id == id && sec.data.size() >= 0x10) {
                        g.code.resize(sec.data.size() / 4);
                        std::memcpy(g.code.data(), sec.data.data(), g.code.size() * 4);
                        if (const auto e = cdc::internalPointer(sec, 4)) g.enter = *e / 4;
                        break;
                    }
                c.graph.push_back(std::move(g));
            }
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
// The pattern the state's request plays (resolveRequest).
const Request* ChartRuntime::pattern() const { return playing_ ? &*playing_ : nullptr; }
float ChartRuntime::animLengthFrames() const {
    const Request* p = pattern();
    return p ? p->lengthFrames() : 0.0f;
}
bool ChartRuntime::animLoops() const {
    const Request* p = pattern();
    return p && p->mode == 2;
}
float ChartRuntime::remainingFrames() const {
    const Request* p = pattern();
    if (!p) return 0.0f;
    float f = std::max(0.0f, p->lengthFrames() - animFrames_);
    for (const int q : queue_)
        if (q >= 0 && size_t(q) < charts_.requests.size()) f += std::max(0.0f, charts_.requests[size_t(q)].lengthFrames() - charts_.requests[size_t(q)].startFrame());
    return f;
}

namespace {
// tru.exe's anim graph VM (0x432B20) over one node program, enough to learn which pattern a kind-2 node plays: the stack
// opcodes (1 pop, 2 push the next word, 0x47..0x52 float arithmetic / compares / logic, 0x5A round, 0x5C int to float,
// 0x63 jump, 0x64 jump if false, 0x7B return) and module 0's natives (table 0x8AC128 -> 0x88F238): 0 Play (0x448100:
// pattern, flags, two floats), 1 IsPlaying (0x43B390: pattern or -1, from / to frame (-1 open), flags; flags & 1: false
// unless the mirror request differs from the playing clip's), 2 / 3 / 4 the node's result slot (0x434EE0 store /
// 0x434F00 commit / 0x434F30 push a selector), 5 / 6 open / close a blend node (0x434F50 / 0x444950) [O: decomp,
// natives disassembled]. Module 1's natives (table 0x8ABF90) pop nothing and push one player value (Module1, filled
// from the context): 4 (0x6E9680) +0xA80 the jump target type; 5 (0x6E96A0) 1 when the jump target +0xA64 has an owner
// object (+0x24) with flag bit 0 of +0x50, else +0xEE8; 7 (0x6E9700) +0xED4 == 0 and not bit 26 of game flags
// (0xE7F094 -> +0x50 -> +0xB90); 8 (0x6E9760) +0xEDC; 16 (0x6E98F0) the context's held object (VM context +0x24 ->
// first word, 0 until module 1's 17 sets it; consumed by the result slot); 76 (0x6EA1E0) 0x74FD70 on Lara's agent +0x460,
// the sprint stamina record = condition 0x205, ctx.canSprint(). A native value the context cannot give is unknown and a
// branch on it runs both ways; the run resolves when both agree. Any other native leaves it unresolved. Every Play is
// collected in queue order (a blend node's records are queued in turn by 0x4482C0); the first plays, the rest follow.
enum class GraphOutcome { Played, Nothing, Unresolved };
struct GraphRun {
    GraphOutcome outcome = GraphOutcome::Unresolved;
    std::vector<int> queue; // the patterns queued, in play order
    bool operator==(const GraphRun& o) const { return outcome == o.outcome && queue == o.queue; }
};
struct GraphVm {
    const Charts::GraphNode& g;
    int playing;
    float frame;
    bool mirrorChange;
    const std::unordered_map<uint32_t, std::optional<uint32_t>>& module1; // fn -> value (nullopt unknown)
    struct V { uint32_t v; bool known; };
    // The node's play state: the result slot (fn 2 stores a selector, fn 4's static one picks a random entry, 0x448320;
    // module 1's 16 the context's held one), its entries (one blend node each, fn 5 / fn 6), and the anim queue.
    struct Ctx {
        std::vector<V> st;
        bool active = false, random = false, inNode = false;
        std::vector<std::vector<int>> entries;
        std::vector<int> node, queue;
    };
    GraphRun run(size_t pc, Ctx c, int depth) const {
        auto& st = c.st;
        auto f = [](uint32_t v) { float x; std::memcpy(&x, &v, 4); return x; };
        auto u = [](float x) { uint32_t v; std::memcpy(&v, &x, 4); return v; };
        auto pop = [&]() { const V v = st.empty() ? V{0, true} : st.back(); if (!st.empty()) st.pop_back(); return v; };
        auto push = [&](uint32_t v, bool known = true) { st.push_back({v, known}); };
        for (int steps = 0; steps < 4096 && pc < g.code.size(); ++steps) {
            const uint32_t w = g.code[pc++];
            const int32_t rel = int32_t(w << 8) >> 8;
            const uint32_t op = w >> 24;
            switch (op) {
            case 0x01: pop(); break;
            case 0x02: push(pc < g.code.size() ? g.code[pc] : 0), ++pc; break;
            case 0x47: { const V a = pop(); push(u(-f(a.v)), a.known); break; }
            case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: {
                const V vb = pop(), va = pop();
                const float b = f(vb.v), a = f(va.v);
                const bool known = va.known && vb.known;
                if (op >= 0x4C) push(op == 0x4C ? a < b : op == 0x4D ? a <= b : a == b, known);
                else push(u(op == 0x48 ? a + b : op == 0x49 ? a - b : op == 0x4A ? a * b
                          : b != 0 ? a / b : a > 0 ? 3.4028235e38f : a < 0 ? -3.4028235e38f : 0.0f), known);
                break;
            }
            case 0x50: { const V a = pop(); push((a.v & 0xFF) == 0, a.known); break; }
            case 0x51: case 0x52: {
                const V b = pop(), a = pop();
                const bool ab = a.v & 0xFF, bb = b.v & 0xFF;
                // a known operand can decide it alone (false for and, true for or)
                const bool dec = op == 0x51 ? (a.known && !ab) || (b.known && !bb) : (a.known && ab) || (b.known && bb);
                push(op == 0x51 ? ab && bb : ab || bb, (a.known && b.known) || dec);
                break;
            }
            case 0x5A: { const V a = pop(); push(uint32_t(int32_t(std::lround(f(a.v)))), a.known); break; }
            case 0x5C: { const V a = pop(); push(u(float(int32_t(a.v))), a.known); break; }
            case 0x63: pc = size_t(int64_t(pc) + rel); break;
            case 0x64: {
                const V cond = pop();
                const size_t taken = size_t(int64_t(pc) + rel);
                if (cond.known) {
                    if ((cond.v & 0xFF) == 0) pc = taken;
                    break;
                }
                if (depth >= 8) return {};
                const GraphRun x = run(pc, c, depth + 1), y = run(taken, c, depth + 1);
                return x == y ? x : GraphRun{};
            }
            case 0x65: {
                const uint32_t fn = pop().v, module = pop().v;
                if (const auto m1 = module == 1 ? module1.find(fn) : module1.end(); m1 != module1.end()) {
                    push(m1->second.value_or(0), m1->second.has_value());
                } else if (module != 0) {
                    return {};
                } else if (fn == 0) { // Play (0x448100): into the open blend node, else queued (0x446BD0; type 2 flushes)
                    pop(), pop();
                    const V flags = pop(), pat = pop();
                    if (!pat.known || !flags.known) return {};
                    if (c.active && c.inNode) {
                        c.node.push_back(int32_t(pat.v));
                    } else {
                        if ((flags.v & 7) == 2) c.queue.clear();
                        c.queue.push_back(int32_t(pat.v));
                    }
                } else if (fn == 1) { // IsPlaying
                    const V flags = pop(), to = pop(), from = pop(), pat = pop();
                    const int p = int32_t(pat.v);
                    bool on = (p == -1 || p == playing) && (f(from.v) == -1.0f || f(from.v) <= frame) &&
                              (f(to.v) == -1.0f || frame < f(to.v));
                    if ((flags.v & 1) && !mirrorChange) on = false;
                    push(on, flags.known && to.known && from.known && pat.known);
                } else if (fn == 2) { // 0x434EE0: the result slot holds the selector
                    const V sel = pop();
                    c.active = true, c.random = sel.v == 0x88F26C, c.entries.clear();
                } else if (fn == 3) { // 0x434F00: the selector plays one entry (0x448320 picks it at random)
                    if (c.active && !c.entries.empty()) {
                        for (const auto& e : c.entries)
                            if (e != c.entries[0]) return {}; // [U] which entry the random pick takes
                        c.queue.insert(c.queue.end(), c.entries[0].begin(), c.entries[0].end()); // 0x4482C0 queues it
                    }
                    c.active = false, c.entries.clear();
                } else if (fn == 4) {
                    push(0x88F26C);
                } else if (fn == 5) { // 0x434F50
                    if (c.active) c.inNode = true, c.node.clear();
                } else if (fn == 6) { // 0x444950
                    if (c.active && c.inNode) c.entries.push_back(c.node), c.inNode = false;
                } else {
                    return {};
                }
                break;
            }
            case 0x7B: return c.queue.empty() ? GraphRun{GraphOutcome::Nothing, {}} : GraphRun{GraphOutcome::Played, c.queue};
            default: break; // tru.exe's switch ignores other opcodes
            }
        }
        return {};
    }
};
} // namespace

// The state's request (0x444BA0): kinds 0 / 1 play that pattern (0x4428A0); kind 2 runs its anim graph node's enter
// program (0x443720 -> 0x43E250 -> 0x4370C0 -> 0x432B20) against the clip playing until now: the pattern its Play
// native starts is what plays, in the pattern's own mode [U: Play's flags & 7 (0x448100) is not that mode: node 213's
// else branch passes 2, yet Wine ends S245 once ng_planted_jump_forward has played through]. A node that plays nothing
// leaves the clip and its clock running (false: no restart). Unresolved: no clip.
// Module 1's native values for one node run (GraphVm), from the chart-side stores: the jump target as the chart sees it
// (chartJumpTarget; its type from the chart's own query, else the game's), playerInt(0xEE8 / 0xED4 / 0xEDC).
std::unordered_map<uint32_t, std::optional<uint32_t>> module1Values(ChartContext& ctx) {
    std::unordered_map<uint32_t, std::optional<uint32_t>> v;
    const auto t = chartJumpTarget(ctx);
    std::optional<int> type = 0;
    if (ctx.chartTarget.target) type = ctx.chartTarget.type;
    else if (t) type = ctx.jumpTargetType();
    if (type) v[4] = uint32_t(*type);
    else v[4] = std::nullopt;
    // [I] an owner object = a markup riding a live instance (MarkupSlot::live); its +0x50 bit 0 is not modelled
    if (t && t.live) v[5] = std::nullopt;
    else v[5] = uint32_t(ctx.playerInt(0xEE8));
    // game flag bit 26 (0xE7F094 -> +0x50 -> +0xB90) is clear in every Wine anim recording (var_gb90 0x78004000, the
    // bedroom and chimney units) [O-runtime there, I elsewhere]: 7 = +0xED4 == 0
    v[7] = uint32_t(ctx.playerInt(0xED4) == 0);
    v[8] = uint32_t(ctx.playerInt(0xEDC));
    v[16] = 0u;
    // 13 (0x6E9780): an attached markup (+0xC94) that 0x733F60 accepts (0x733EC0: markup flags & 0xA000000 and the
    // pitch of its first segment, |atan2(-dz, |dxy|)| wrapped to (-pi, pi], within the global limit *(0xE7EE50)+0x184
    // = 40 degrees [O-runtime: Wine, l4_traprooms]) and whose first segment runs along her facing (0x733690: |dot| of
    // the flattened unit segment and model -Y over cos(object float +0x740 degrees); a segment with no horizontal part
    // passes with dot 1) [O: decomp].
    v[13] = 0u;
    if (const auto a = ctx.attachedMarkup(); a && (a.flags & 0xA000000))
        if (const auto seg = ctx.markupSegment(a.markup, 0)) {
            const glm::vec3 d = seg->first - seg->second;
            const float pitch = std::abs(std::atan2(-d.z, glm::length(glm::vec2(d))));
            if (pitch <= glm::radians(40.0f)) {
                const glm::vec2 dir(seg->second - seg->first);
                const float h = ctx.heading(), lim = ctx.chart().charts().data<float>(0x740);
                const glm::vec2 f(std::cos(h), std::sin(h));
                const float dot = glm::length(dir) > 1e-2f ? std::abs(glm::dot(glm::normalize(dir), f)) : 1.0f;
                v[13] = uint32_t(glm::length(dir) <= 1e-2f || dot > std::cos(glm::radians(lim)));
            }
        }
    // 64 (0x6EA050): the byte the class-603 attach setter 0x74C8D0 stores in the VM context (+0x24 -> +0x10): the side
    // she takes the pole on (the layer's +0xF4 0x20 it sets alongside) [O]; ChartContext::vmPoleSide (poleAttachSetter).
    v[64] = std::nullopt;
    if (ctx.vmPoleSide) v[64] = uint32_t(*ctx.vmPoleSide);
    // 3 (0x6E9660): byte player +0xEE0, which property 205's handler 0x767900 sets (a bool property's byte, else 1) [O:
    // disassembly, Lara's property table 0x8B2280]; the pull up's node 26 (S456) reads it. Ours: playerInt(0xEE0),
    // Generic.cpp's handler.
    v[3] = uint32_t(ctx.playerInt(0xEE0) != 0);
    if (const auto c = ctx.canSprint()) v[76] = uint32_t(*c);
    else v[76] = std::nullopt;
    return v;
}

bool ChartRuntime::resolveRequest(ChartContext& ctx) {
    const State* s = data_.state(state_);
    int pat = s ? s->request : -1;
    if (s && s->requestKind == 2) {
        const GraphRun r = size_t(s->request) < charts_.graph.size()
            ? GraphVm{charts_.graph[size_t(s->request)], playing_ ? playingIndex_ : -1, animFrames_, mirror_ != playingMirror_,
                      module1Values(ctx)}
                  .run(charts_.graph[size_t(s->request)].enter, GraphVm::Ctx{}, 0)
            : GraphRun{};
        if (r.outcome == GraphOutcome::Nothing && playing_) return false;
        pat = r.outcome == GraphOutcome::Played ? r.queue[0] : -1;
        queue_.assign(r.queue.size() > 1 ? r.queue.begin() + 1 : r.queue.end(), r.queue.end());
    } else {
        queue_.clear();
    }
    playing_.reset(), playingIndex_ = -1;
    if (pat < 0 || size_t(pat) >= charts_.requests.size()) return true;
    playing_ = charts_.requests[size_t(pat)], playingIndex_ = pat, playingMirror_ = mirror_;
    return true;
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
// (start - blend) frames, uncut, so that it stands at the pattern's start frame when the crossfade ends (Request::
// startFrame). Lara's layer always takes 0x4428A0's crossfade branch: +0xA4 1, +0xA8 0, +0xAC 1 in every Wine anim
// recording; its other branch (start - blend cut at 0, blend 0) is not ours to take [O-runtime: W/chart/wine/*_anim:
// 0x441FB0 (100, 400) -> 680's time -300; (100, 200) -> 678's -100; (600, 600) -> 52's 0; (207, 207) -> 1463's 0].
// 0x4428A0: the blend is the previous pattern's +0x1B when it set one (layer +0x314, then replaced by this one's).
void ChartRuntime::startClip(const Request& r) {
    animFrames_ = clipStart_ = r.startFrame(nextBlend_);
    nextBlend_ = r.nextBlend == 0xFF ? -1 : int(r.nextBlend);
}

void ChartRuntime::enterAnim(ChartContext& ctx) {
    animCounter_ = 0;
    if (!resolveRequest(ctx)) return;
    ++clipStarts_;
    const Request* p = pattern();
    // Kind 2 reaches the same start: Play (0x448100) queues a record (0x446BD0) that 0x443430 plays through 0x4428A0 [O].
    if (p) startClip(*p);
    else animFrames_ = clipStart_ = 0.0f;
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
    enterAnim(ctx);
    ctx.stateEntered(old, target);
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
    // Ours caps the chaining (tru.exe has no cap): a cycle that tru.exe's game state would break (an attach the shadow's
    // controller has not made) must not hang the frame. Counted in stats.chainCaps.
    int links = 0;
    do {
        chain = false;
        const int t = find(ctx, s);
        if (t < 0) return;
        change(ctx, t);
        if (active_ && state_ != orig && state_ != s && propBool(0x65).value_or(false)) ++frame_, chain = true;
        s = state_;
        if (chain && ++links >= 64) {
            ++stats.chainCaps;
            return;
        }
    } while (chain);
}

void ChartRuntime::start(ChartContext& ctx) {
    ctx.chart_ = this;
    state_ = data_.root, active_ = true, queued_ = lastTransition_ = -1, stateTime_ = totalTime_ = 0;
    applyProperties(ctx, state_);
    enterAnim(ctx);
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
    animFrames_ += ctx.timeMult * (p ? p->rate : 1.0f) * (layerRate ? ctx.animRate : 1.0f);
    if (len > 0 && (animLoops() ? std::floor(animFrames_ / len) > std::floor(before / len) : before < len && animFrames_ >= len)) {
        // A queued pattern follows (0x43E0C0 -> 0x443430 plays the next record through 0x4428A0); the chart hears the
        // end only when the queue has drained (0x43E0C0 calls the saved callback +0x304 then) [O].
        if (!queue_.empty()) {
            const int next = queue_.front();
            queue_.erase(queue_.begin());
            if (next >= 0 && size_t(next) < charts_.requests.size()) {
                playing_ = charts_.requests[size_t(next)], playingIndex_ = next, playingMirror_ = mirror_;
                startClip(*playing_);
                ++clipStarts_;
            }
        } else {
            animEnded(ctx);
        }
    }
}

void ChartRuntime::fastForward(float frames) {
    stateTime_ += frames, totalTime_ += frames;
    for (float left = frames; left > 0 && playing_;) {
        const float rate = playing_->rate > 0 ? playing_->rate : 1.0f;
        const float need = std::max(0.0f, animLengthFrames() - animFrames_) / rate; // state frames to this clip's end
        if (left <= need || animLoops() || queue_.empty()) {
            animFrames_ += left * rate;
            break;
        }
        animFrames_ += need * rate, left -= need;
        const int next = queue_.front(); // a queued pattern follows the one that ended (0x43E0C0 -> 0x443430)
        queue_.erase(queue_.begin());
        if (next < 0 || size_t(next) >= charts_.requests.size()) break;
        playing_ = charts_.requests[size_t(next)], playingIndex_ = next, playingMirror_ = mirror_;
        startClip(*playing_);
        ++clipStarts_;
    }
}

void ChartRuntime::enter(ChartContext& ctx, int state) {
    ctx.chart_ = this;
    if (!data_.state(state)) return;
    const int old = state_;
    runExit(ctx, old, state);
    state_ = state, active_ = true;
    applyProperties(ctx, state);
    runEnter(ctx, state, old, true);
    enterAnim(ctx);
    ctx.stateEntered(old, state);
    runEnter(ctx, state, old, false);
    stateTime_ = 0, queued_ = -1, lastTransition_ = -1;
}

ChartRuntime::Snapshot ChartRuntime::snapshot() const {
    return {state_, queued_, lastTransition_, animCounter_, stateTime_, totalTime_, animFrames_, mirror_, deferFlag7, fields_};
}

void ChartRuntime::restore(ChartContext& ctx, const Snapshot& s) {
    ctx.chart_ = this;
    if (!data_.state(s.state)) return;
    state_ = s.state, active_ = true, mirror_ = s.mirror;
    applyProperties(ctx, state_);
    enterAnim(ctx);
    queued_ = s.queued, lastTransition_ = s.lastTransition, animCounter_ = s.animCounter;
    stateTime_ = s.stateTime, totalTime_ = s.totalTime, animFrames_ = s.animFrames;
    deferFlag7 = s.deferFlag7, fields_ = s.fields;
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
        std::snprintf(buf, sizeof buf, "S%d req %d/k%d [%s] parent %d w8 %u", i, s.request, s.requestKind, clip(i).c_str(), s.parent, unsigned(s.word8));
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

