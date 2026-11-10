#include "formats/AnimGraph.hpp"

#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::formats {
namespace {
using cdc::FormatError;
template <class T> T at(const cdc::Section& s, size_t off) {
    if (off > s.data.size() || s.data.size() - off < sizeof(T)) throw FormatError("anim graph: read past object base");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}
// Records follow each other while their tag holds (tru.exe 0x6B4B10 walks them the same way).
std::vector<uint32_t> records(const cdc::Section& ob, std::optional<uint32_t> first, uint16_t tag, uint32_t size) {
    std::vector<uint32_t> out;
    for (uint32_t o = first.value_or(~0u); o <= ob.data.size() && ob.data.size() - o >= size && at<uint16_t>(ob, o) == tag; o += size)
        out.push_back(o);
    return out;
}
// 0x6B4B10: ids below 0 take the lowest id < 32 no record uses.
template <class R> void assignIds(std::vector<R>& rs) {
    uint32_t used = 0;
    for (const auto& r : rs) if (r.index >= 0 && r.index < 32) used |= 1u << r.index;
    int16_t next = 0;
    for (auto& r : rs) {
        if (r.index >= 0) continue;
        while (next < 32 && (used >> next & 1)) ++next;
        if (next >= 32) break;
        r.index = next, used |= 1u << next;
    }
}
template <class R> int byId(const std::vector<R>& rs, int32_t id) {
    for (size_t i = 0; i < rs.size(); ++i) if (rs[i].index == id) return int(i);
    return -1;
}
} // namespace

int AnimGraph::stateById(int32_t id) const { return byId(states, id); }
int AnimGraph::transitionById(int32_t id) const { return byId(transitions, id); }

double graphRandom(uint32_t& seed, double hi) {
    seed = seed * 0x41C64E6Du + 0x3039u;
    return double(float((seed >> 16) & 0x7FFF)) / 32767.0 * hi;
}

namespace {
// 0x6B3DD0 / 0x6A52E0: the reached ones (value - after, raised to the window's total - window) form a weighted draw;
// the first is taken without a draw. ponytail: x87 extended precision of the exe's comparison is double here.
template <class Eligible>
int weighted(const AnimGraph& g, int state, float value, float total, uint32_t& seed, Eligible eligible) {
    float sum = 0;
    int count = 0, picked = -1;
    for (const uint32_t ti : g.states[size_t(state)].out) {
        const auto& t = g.transitions[ti];
        if (!eligible(t)) continue;
        float d = value - t.after;
        d = std::max(d, t.window > 0 ? total - t.window : -1e6f);
        if (d < 0) continue; // the exe plays the nearest one's miss clip (+8): -1 in all data
        sum += d;
        if (++count < 2 || graphRandom(seed, sum) <= d) picked = int(ti);
    }
    return picked;
}
} // namespace

int AnimGraph::pick(int state, uint32_t mask, float value, float total, uint32_t& seed) const {
    return weighted(*this, state, value, total, seed, [&](const AnimGraphTransition& t) { return (t.mask & mask) && t.events.empty(); });
}

int AnimGraph::pickHit(int state, uint32_t damageClass, float damage, float total, uint32_t& seed) const {
    return weighted(*this, state, damage, total, seed, [&](const AnimGraphTransition& t) {
        return (t.mask & 0x800) && std::ranges::find(t.events, damageClass) != t.events.end();
    });
}

int AnimGraph::pickAny(int state, uint32_t mask, uint32_t& seed) const {
    int count = 0, picked = -1;
    for (const uint32_t ti : states[size_t(state)].out)
        if ((transitions[ti].mask & mask) && (++count < 2 || graphRandom(seed, count) < 1.0)) picked = int(ti);
    return picked;
}

int AnimGraph::pickUse(int state, uint32_t mask, float value, uint32_t& seed) const {
    float best = 1;
    int count = 0, picked = -1;
    for (const uint32_t ti : states[size_t(state)].out) {
        const auto& t = transitions[ti];
        if (!(t.mask & mask) || !t.events.empty()) continue;
        const float d = t.after == 0 ? 0.1f : std::abs(value - t.after);
        ++count;
        if (picked < 0 || d < best) count = 1, best = d, picked = int(ti);
        else if (d == best && graphRandom(seed, count) < 1.0) picked = int(ti);
    }
    return best <= 0.1f ? picked : -1; // farther: the exe plays its miss clip (+8, -1 in all data) instead
}

std::optional<AnimGraph> loadAnimGraph(const cdc::Drm& drm) {
    const auto base = objectBaseSection(drm);
    if (!base) return std::nullopt;
    const auto& ob = drm.sections[*base];
    const auto rec = cdc::internalPointer(ob, 0xC8);
    if (!rec || at<uint16_t>(ob, *rec + 4) != 0xB00B || at<uint16_t>(ob, *rec + 2) != 6) return std::nullopt;
    // Query 0xE6 / 0xE7 (0x6C7A60): record +0x30 / +0x34 -> pointer to the first state / transition.
    const auto deref = [&](uint32_t off) -> std::optional<uint32_t> {
        const auto p = cdc::internalPointer(ob, off);
        return p ? cdc::internalPointer(ob, *p) : std::nullopt;
    };
    const auto states = records(ob, deref(*rec + 0x30), 0x2714, 0x28);
    const auto transitions = records(ob, deref(*rec + 0x34), 0x2776, 0x34);
    if (states.empty()) return std::nullopt;
    const auto position = [](const std::vector<uint32_t>& rs, uint32_t o, const char* what) {
        for (size_t i = 0; i < rs.size(); ++i) if (rs[i] == o) return uint32_t(i);
        throw FormatError(std::string("anim graph: pointer outside the ") + what);
    };
    AnimGraph g;
    for (const uint32_t o : transitions) {
        AnimGraphTransition t;
        t.index = at<int16_t>(ob, o + 2), t.mask = at<uint32_t>(ob, o + 4), t.anim = at<int8_t>(ob, o + 9);
        t.after = at<float>(ob, o + 0xC), t.window = at<float>(ob, o + 0x10);
        if (const auto target = cdc::internalPointer(ob, o + 0x14)) t.target = int32_t(position(states, *target, "states"));
        if (const uint32_t n = at<uint32_t>(ob, o + 0x1C); n) {
            const auto list = cdc::internalPointer(ob, o + 0x20);
            if (!list || n > 64) throw FormatError("anim graph: event list");
            for (uint32_t k = 0; k < n; ++k) t.events.push_back(at<uint32_t>(ob, *list + 4 * k));
        }
        g.transitions.push_back(std::move(t));
    }
    for (const uint32_t o : states) {
        AnimGraphState s;
        s.index = at<int16_t>(ob, o + 2), s.flags = at<uint16_t>(ob, o + 4), s.anim = at<int8_t>(ob, o + 7);
        s.message = at<uint32_t>(ob, o + 8);
        if (const uint32_t n = at<uint32_t>(ob, o + 0x14); n) {
            const auto list = cdc::internalPointer(ob, o + 0x18);
            if (!list || n > 64) throw FormatError("anim graph: transition list");
            for (uint32_t k = 0; k < n; ++k) {
                const auto t = cdc::internalPointer(ob, *list + 4 * k);
                if (!t) throw FormatError("anim graph: null transition");
                s.out.push_back(position(transitions, *t, "transitions"));
            }
        }
        g.states.push_back(std::move(s));
    }
    assignIds(g.states), assignIds(g.transitions);
    // Record +0x2C -> the state an instance starts in (set in all 95 graph objects; 25 start in another than the first:
    // the receptacles' empty state, ma_ani_foor_collapse's `idle` before its `idle_collapsed`, the artifacts, ...).
    if (const auto start = cdc::internalPointer(ob, *rec + 0x2C))
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i] == *start) g.initial = int(i);
    return g;
}

} // namespace ou::formats
