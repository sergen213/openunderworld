// Objects a unit's uber intros give birth to (state action 0x13, tru.exe 0x4D3B50): the treasures inside the
// reward urns. Evidence: docs/GAMEPLAY.md "Collectibles".
#include "formats/Formats.hpp"
#include "formats/Uber.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cstring>

namespace ou::formats {
namespace {

template <typename T> T at(const cdc::Section& s, size_t off) {
    if (off + sizeof(T) > s.data.size()) throw cdc::FormatError("births: read past section end");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof(T));
    return v;
}

// Model-space position of HInfo marker `id` of object model `model` (dtp Model +4 segment count, +0xC segments of
// 0x40 bytes: pivot +0x20, parent +0x38, HInfo +0x3C; HInfo slot 2 = markers of 0x20 bytes: +4 u16 id, +8 position;
// tru.exe 0x46DA20). The pivots are summed down the parents as physics::loadObjectPhysics does.
std::optional<glm::vec3> hinfoMarker(const cdc::Drm& d, uint32_t model, int32_t id) {
    const auto base = objectBaseSection(d);
    if (!base) return std::nullopt;
    const auto& ob = d.sections[*base];
    const auto ptrs = cdc::sectionPointers(d, *base);
    const auto lp = ptrs.find(0xA4);
    if (at<uint16_t>(ob, 0xA0) <= model || lp == ptrs.end()) return std::nullopt;
    const auto& list = d.sections[lp->second.section];
    if (cdc::resourceRefType(list, lp->second.offset + 4 * model) != int(cdc::SectionType::DtpData)) return std::nullopt;
    const uint32_t modelId = at<uint32_t>(list, lp->second.offset + 4 * model);
    for (const auto& m : d.sections) {
        if (m.type != cdc::SectionType::DtpData || m.id != modelId) continue;
        const uint32_t nseg = at<uint32_t>(m, 4);
        const auto segs = cdc::internalPointer(m, 0xC);
        if (!segs || nseg > 4096) return std::nullopt;
        std::vector<glm::vec3> origin;
        for (uint32_t k = 0; k < nseg; ++k) {
            const uint32_t s = *segs + 0x40 * k;
            const int32_t parent = at<int32_t>(m, s + 0x38);
            const glm::vec3 pivot(at<float>(m, s + 0x20), at<float>(m, s + 0x24), at<float>(m, s + 0x28));
            origin.push_back(pivot + (parent >= 0 && parent < int32_t(k) ? origin[size_t(parent)] : glm::vec3(0)));
            const auto h = cdc::internalPointer(m, s + 0x3C);
            if (!h) continue;
            const uint32_t n = at<uint32_t>(m, *h + 16);
            const auto p = cdc::internalPointer(m, *h + 20);
            if (!p || n > 4096) continue;
            for (uint32_t i = 0; i < n; ++i)
                if (const uint32_t o = *p + 0x20 * i; at<uint16_t>(m, o + 4) == id)
                    return origin.back() + glm::vec3(at<float>(m, o + 8), at<float>(m, o + 12), at<float>(m, o + 16));
        }
        break;
    }
    return std::nullopt;
}

} // namespace

std::vector<BirthIntro> birthIntros(const cdc::BigfileSet& bigfile, const Unit& unit, const std::map<uint16_t, std::string>& names) {
    std::vector<BirthIntro> out;
    struct Parent { std::vector<std::pair<UberBirth, glm::vec3>> births; }; // record, marker offset (model space)
    std::map<uint16_t, Parent> parents;
    for (const auto& in : unit.intros) {
        const auto n = names.find(in.objectId);
        if (n == names.end() || !birthParent(n->second) || dormant(in)) continue;
        auto [it, fresh] = parents.try_emplace(in.objectId);
        if (fresh) try {
            const auto bytes = bigfile.read("pc-w\\" + n->second + ".drm");
            if (!bytes) continue;
            const auto drm = cdc::parseDrm(*bytes);
            const auto uber = loadUberObject(drm);
            if (!uber) continue;
            for (const auto& b : uber->births) {
                // 0x4D3B50: without a model byte / marker, or when the lookup fails, the instance's own transform
                glm::vec3 at(0);
                if (b.model != -1 && b.marker != -1)
                    if (const auto m = hinfoMarker(drm, uint32_t(uint8_t(b.model)), b.marker)) at = *m;
                it->second.births.push_back({b, at});
            }
        } catch (const cdc::FormatError&) {}
        for (size_t k = 0; k < it->second.births.size(); ++k) {
            const auto& [b, local] = it->second.births[k];
            float m[16];
            introMatrix(in, m);
            const glm::vec4 w = glm::make_mat4(m) * glm::vec4(local, 1.0f);
            BirthIntro x;
            x.parent = in.uniqueId;
            x.intro = Intro{};
            std::copy_n(in.rotation, 3, x.intro.rotation);
            x.intro.position[0] = w.x, x.intro.position[1] = w.y, x.intro.position[2] = w.z;
            x.intro.scale[0] = x.intro.scale[1] = x.intro.scale[2] = 1.0f;
            x.intro.objectId = b.object;
            x.intro.uniqueId = bornUid(in.uniqueId, k);
            std::fill_n(x.intro.flags, 4, uint8_t(0));
            x.intro.flags[2] = 0x80; // dormant: not there until its parent gives birth
            out.push_back(std::move(x));
        }
    }
    return out;
}

} // namespace ou::formats
