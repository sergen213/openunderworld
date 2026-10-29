#include "formats/Uber.hpp"

#include "formats/Formats.hpp"

#include <cstring>

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
    const uint32_t ns = at<uint32_t>(ob, rec + 8), nc = at<uint32_t>(ob, rec + 0x10), nq = at<uint32_t>(ob, rec + 0x28);
    const uint32_t sections = ptr(ob, rec + 0x0C), commands = nc ? ptr(ob, rec + 0x14) : 0,
                   queries = nq ? ptr(ob, rec + 0x2C) : 0;
    bounded(ns, sections, 0x140, ob.data.size());
    bounded(nc, commands, 0x0C, ob.data.size());
    bounded(nq, queries, 0x0C, ob.data.size());
    for (uint32_t i = 0; i < ns; ++i) out.sectionModels.push_back(at<uint8_t>(ob, sections + 0x140 * i + 0x28));
    for (uint32_t i = 0; i < nc; ++i) {
        const uint32_t c = commands + 0x0C * i;
        UberCommand command;
        command.id = at<uint32_t>(ob, c);
        const uint32_t count = at<uint32_t>(ob, c + 4), targets = ptr(ob, c + 8);
        bounded(count, targets, 0x0C, ob.data.size());
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t t = targets + 0x0C * j;
            const int32_t section = at<int32_t>(ob, t);
            const uint32_t type = at<uint32_t>(ob, t + 4);
            if (section < 0 || uint32_t(section) >= ns || type < 1 || type > 4)
                throw FormatError("uber: invalid command target");
            command.targets.push_back({section, type, at<uint32_t>(ob, t + 8)});
        }
        out.commands.push_back(std::move(command));
    }
    for (uint32_t i = 0; i < nq; ++i) {
        const uint32_t q = queries + 0x0C * i;
        const int32_t section = at<int32_t>(ob, q);
        const uint32_t kind = at<uint32_t>(ob, q + 4);
        if (section < 0 || uint32_t(section) >= ns || (kind != 1 && kind != 2))
            throw FormatError("uber: invalid query target");
        out.queries.push_back({section, kind, at<uint32_t>(ob, q + 8)});
    }
    return out;
}

} // namespace ou::formats
