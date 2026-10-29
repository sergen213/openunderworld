#include "formats/AnimGraph.hpp"

#include "formats/Formats.hpp"

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
        t.after = at<float>(ob, o + 0xC);
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
    return g;
}

} // namespace ou::formats
