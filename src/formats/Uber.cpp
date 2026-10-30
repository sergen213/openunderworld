#include "formats/Uber.hpp"

#include "formats/Formats.hpp"

#include <cstring>
#include <functional>
#include <map>

namespace ou::formats {
namespace {
using cdc::FormatError;
template <class T> T at(const cdc::Section& s, size_t off) {
    if (off > s.data.size() || s.data.size() - off < sizeof(T)) throw FormatError("uber: read past object section");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}
uint32_t ptr(const cdc::Section& s, uint32_t off) {
    const auto p = cdc::internalPointer(s, off);
    if (!p) throw FormatError("uber: missing nested array pointer at " + std::to_string(off));
    return *p;
}
void bounded(uint32_t n, uint32_t offset, size_t stride, size_t bytes) {
    if (n > 4096 || (n && (offset > bytes || size_t(n) * stride > bytes - offset)) ||
        (!n && offset > bytes))
        throw FormatError("uber: nested array outside object section");
}
// Condition list {?, count, entries}: entries {section, kind, value} (0x4D1360).
std::vector<UberCondition> conditionList(const cdc::Section& ob, uint32_t list) {
    std::vector<UberCondition> out;
    const uint32_t n = at<uint32_t>(ob, list + 4);
    if (!n) return out;
    const uint32_t entries = ptr(ob, list + 8);
    bounded(n, entries, 0x0C, ob.data.size());
    for (uint32_t i = 0; i < n; ++i)
        out.push_back({at<int32_t>(ob, entries + 0x0C * i), at<uint32_t>(ob, entries + 0x0C * i + 4), at<uint32_t>(ob, entries + 0x0C * i + 8)});
    return out;
}

} // namespace

std::optional<UberObjectData> loadUberObject(const cdc::Drm& drm) {
    const auto base = objectBaseSection(drm);
    if (!base) return std::nullopt;
    const auto& ob = drm.sections[*base];
    const auto p = cdc::internalPointer(ob, 0xC8);
    if (!p || at<uint16_t>(ob, *p + 4) != 0xB00B) return std::nullopt;
    const uint16_t objectClass = at<uint16_t>(ob, *p + 2);
    if (objectClass != 79) return std::nullopt;
    const uint32_t rec = *p;
    UberObjectData out;
    out.objectClass = objectClass;
    const uint32_t ns = at<uint32_t>(ob, rec + 8), nc = at<uint32_t>(ob, rec + 0x10), ne = at<uint32_t>(ob, rec + 0x18);
    const uint32_t sections = ns ? ptr(ob, rec + 0x0C) : 0, commands = nc ? ptr(ob, rec + 0x14) : 0,
                   events = ne ? ptr(ob, rec + 0x1C) : 0;
    // Section records are 0x54 apart (the section component's +0x78; states 0x1C, transitions 0x38: 0x4D4BE0, 0x4D4A00).
    bounded(ns, sections, 0x54, ob.data.size());
    bounded(nc, commands, 0x0C, ob.data.size());
    bounded(ne, events, 0x1C, ob.data.size());
    // Joint and limit / motor / range records, shared by pointer between actions and transitions.
    std::map<uint32_t, int32_t> jointAt, paramAt;
    const auto param = [&](uint32_t o) {
        if (const auto it = paramAt.find(o); it != paramAt.end()) return it->second;
        out.params.push_back({at<uint8_t>(ob, o), at<float>(ob, o + 4), at<float>(ob, o + 8), at<uint8_t>(ob, o + 12) != 0});
        return paramAt[o] = int32_t(out.params.size() - 1);
    };
    std::function<int32_t(uint32_t)> joint = [&](uint32_t o) -> int32_t {
        if (const auto it = jointAt.find(o); it != jointAt.end()) return it->second;
        const int32_t index = jointAt[o] = int32_t(out.joints.size());
        out.joints.emplace_back();
        UberJoint j;
        j.type = at<uint8_t>(ob, o);
        if (j.type == 11) { // link {type, joint1, joint2, ratio}
            j.ratio = at<float>(ob, o + 12);
            j.link1 = joint(ptr(ob, o + 4));
            j.link2 = joint(ptr(ob, o + 8));
        } else {
            j.bodyA = at<int32_t>(ob, o + 4), j.bodyB = at<int32_t>(ob, o + 8);
            j.model = at<uint8_t>(ob, o + 0xC), j.marker = at<uint8_t>(ob, o + 0xD);
            j.lo = at<float>(ob, o + 0x2C), j.hi = at<float>(ob, o + 0x30);
            j.speed = at<float>(ob, o + 0x34), j.force = at<float>(ob, o + 0x38);
            j.flags = at<uint8_t>(ob, o + 0x3C);
            if (j.bodyA < -1 || j.bodyA >= int32_t(ns) || j.bodyB < -1 || j.bodyB >= int32_t(ns))
                throw FormatError("uber: joint body outside the sections");
        }
        out.joints[size_t(index)] = j;
        return index;
    };
    for (uint32_t i = 0; i < ns; ++i) {
        const uint32_t sec = sections + 0x54 * i;
        UberSection section;
        const uint32_t nst = at<uint32_t>(ob, sec + 4), ntr = at<uint32_t>(ob, sec + 0x0C);
        const uint32_t states = nst ? ptr(ob, sec + 8) : 0, transitions = ntr ? ptr(ob, sec + 0x10) : 0;
        bounded(nst, states, 0x1C, ob.data.size());
        bounded(ntr, transitions, 0x38, ob.data.size());
        section.defaultState = at<uint32_t>(ob, sec + 0x14);
        section.model = at<uint32_t>(ob, sec + 0x18);
        for (uint32_t t = 0; t < ntr; ++t) {
            const uint32_t tr = transitions + 0x38 * t;
            UberTransition x;
            x.from = at<uint32_t>(ob, tr), x.to = at<uint32_t>(ob, tr + 4);
            x.trigger = at<uint32_t>(ob, tr + 0x18), x.param = at<uint32_t>(ob, tr + 0x1C);
            if (x.trigger == 9)
                if (const auto list = cdc::internalPointer(ob, tr + 0x1C)) x.conditions = conditionList(ob, *list);
            if (x.trigger == 14 || x.trigger == 15) {
                if (const auto j = cdc::internalPointer(ob, tr + 0x20)) x.joint = joint(*j);
                if (const auto r = cdc::internalPointer(ob, tr + 0x24)) x.range = param(*r);
            }
            const uint16_t anim = at<uint16_t>(ob, tr + 0x2A);
            x.anim = anim == 0xFFFF ? -1 : anim;
            x.probability = at<uint32_t>(ob, tr + 0x2C), x.flags = at<uint32_t>(ob, tr + 0x34);
            if (x.to >= nst) throw FormatError("uber: transition target state");
            section.transitions.push_back(std::move(x));
        }
        for (uint32_t s = 0; s < nst; ++s) {
            const uint32_t st = states + 0x1C * s;
            const uint32_t nout = at<uint32_t>(ob, st + 0x0C);
            std::vector<uint32_t> out;
            if (nout) {
                const uint32_t list = ptr(ob, st + 0x10);
                bounded(nout, list, 4, ob.data.size());
                for (uint32_t k = 0; k < nout; ++k) {
                    const uint32_t tr = ptr(ob, list + 4 * k);
                    if (tr < transitions || (tr - transitions) % 0x38 || (tr - transitions) / 0x38 >= ntr)
                        throw FormatError("uber: state transition outside its section");
                    out.push_back((tr - transitions) / 0x38);
                }
            }
            section.stateOut.push_back(std::move(out));
            std::vector<UberAction> actions; // {type, 3 words} records 0x10 apart (0x4D5C60, 0x4D86A0)
            if (const uint32_t na = at<uint32_t>(ob, st + 0x14)) {
                const uint32_t list = ptr(ob, st + 0x18);
                bounded(na, list, 0x10, ob.data.size());
                for (uint32_t k = 0; k < na; ++k) {
                    const uint32_t r = list + 0x10 * k;
                    UberAction a{at<uint32_t>(ob, r), at<uint32_t>(ob, r + 4)};
                    const auto p1 = cdc::internalPointer(ob, r + 4), p2 = cdc::internalPointer(ob, r + 8),
                               p3 = cdc::internalPointer(ob, r + 12);
                    if ((a.type == 9 || a.type == 10 || a.type == 12 || a.type == 13) && p1) {
                        a.joint = joint(*p1);
                        if (a.type >= 12 && p2) a.param = param(*p2);
                    } else if ((a.type == 0x14 || a.type == 0x15) && p2 && p3) {
                        a.joint = joint(*p2), a.param = param(*p3);
                    }
                    actions.push_back(a);
                }
            }
            section.stateActions.push_back(std::move(actions));
        }
        if (nst && section.defaultState >= nst) throw FormatError("uber: default state");
        out.sections.push_back(std::move(section));
    }
    for (uint32_t i = 0; i < nc; ++i) {
        const uint32_t c = commands + 0x0C * i;
        UberCommand command;
        command.id = at<uint32_t>(ob, c);
        const uint32_t count = at<uint32_t>(ob, c + 4), targets = count ? ptr(ob, c + 8) : 0;
        bounded(count, targets, 0x0C, ob.data.size());
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t t = targets + 0x0C * j;
            const int32_t section = at<int32_t>(ob, t);
            const uint32_t type = at<uint32_t>(ob, t + 4);
            // tru.exe 0x4D7680 switches on the type (1 state, 2-4 section actions); type 0 is a no-op there.
            if (section < 0 || uint32_t(section) >= ns || type > 4)
                throw FormatError("uber: invalid command target");
            command.targets.push_back({section, type, at<uint32_t>(ob, t + 8)});
        }
        out.commands.push_back(std::move(command));
    }
    for (uint32_t i = 0; i < ne; ++i) {
        const uint32_t e = events + 0x1C * i;
        UberEvent event;
        event.section = at<int32_t>(ob, e + 0x0C), event.kind = at<uint32_t>(ob, e + 0x10), event.value = at<uint32_t>(ob, e + 0x14);
        event.once = at<uint8_t>(ob, e + 0x18) != 0;
        const uint32_t nl = at<uint32_t>(ob, e + 4);
        if (nl) {
            const uint32_t lists = ptr(ob, e + 8);
            bounded(nl, lists, 4, ob.data.size());
            for (uint32_t k = 0; k < nl; ++k) event.conditions.push_back(conditionList(ob, ptr(ob, lists + 4 * k)));
        }
        out.events.push_back(std::move(event));
    }
    if (const uint32_t nq = at<uint32_t>(ob, rec + 0x28)) { // condition lists in place, 0x0C apart
        const uint32_t queries = ptr(ob, rec + 0x2C);
        bounded(nq, queries, 0x0C, ob.data.size());
        for (uint32_t i = 0; i < nq; ++i) out.queries.push_back(conditionList(ob, queries + 0x0C * i));
    }
    return out;
}

} // namespace ou::formats
