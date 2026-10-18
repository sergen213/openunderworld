// Unit (level) DRM structures: intros, stream groups, IMF placements; object model lists.
// Evidence and offsets: docs/formats/LEVELS.md.
#include "formats/Formats.hpp"

#include <cctype>
#include <cmath>
#include <cstring>
#include <utility>

namespace ou::formats {
namespace {

using cdc::FormatError;

// One section's bytes plus its relocated pointers.
struct View {
    const cdc::Drm& drm;
    uint32_t index;
    std::unordered_map<uint32_t, cdc::Pointer> ptrs;
    View(const cdc::Drm& d, uint32_t i) : drm(d), index(i), ptrs(cdc::sectionPointers(d, i)) {}

    template <class T>
    T at(size_t off) const {
        const auto d = drm.sections[index].data;
        if (off > d.size() || d.size() - off < sizeof(T))
            throw FormatError("section " + std::to_string(index) + ": read past end at " + std::to_string(off));
        T v;
        std::memcpy(&v, d.data() + off, sizeof v);
        return v;
    }
    cdc::Pointer ptr(uint32_t off, const char* what) const {
        const auto it = ptrs.find(off);
        if (it == ptrs.end()) throw FormatError(std::string("no relocated pointer for ") + what);
        return it->second;
    }
    std::string str(const cdc::Pointer& p) const {
        const auto d = drm.sections.at(p.section).data;
        if (p.offset >= d.size()) throw FormatError("string pointer out of section");
        const auto* s = reinterpret_cast<const char*>(d.data() + p.offset);
        return {s, strnlen(s, d.size() - p.offset)};
    }
};

// Type-3/type-5 resource relocations: patch offset -> section type of the referenced resource.
// The id itself stays in the data at the patch offset (as in materialTextures()).
std::unordered_map<uint32_t, uint32_t> resourceRefs(const cdc::Section& s) {
    std::unordered_map<uint32_t, uint32_t> out;
    const auto r = s.relocations;
    if (r.size() < 20) return out;
    uint32_t c[5];
    std::memcpy(c, r.data(), sizeof c);
    const size_t t3 = 20 + 8 * size_t(c[0]) + 8 * size_t(c[1]), t5 = t3 + 4 * (size_t(c[2]) + c[3]);
    for (const auto [p0, n] : {std::pair{t3, c[2]}, std::pair{t5, c[4]}})
        for (uint32_t k = 0; k < n; ++k) {
            if (p0 + 4 * (k + 1ull) > r.size()) throw FormatError("resource relocation table truncated");
            uint32_t v;
            std::memcpy(&v, r.data() + p0 + 4 * size_t(k), 4);
            out[(v & 0x1FFFFFF) * 4] = v >> 25;
        }
    return out;
}

const cdc::Section* findSection(const cdc::Drm& d, cdc::SectionType t, uint32_t id, uint32_t* index = nullptr) {
    for (uint32_t i = 0; i < d.sections.size(); ++i)
        if (d.sections[i].type == t && d.sections[i].id == id) {
            if (index) *index = i;
            return &d.sections[i];
        }
    return nullptr;
}

// u32 resource id at `off` of `s` if a relocation marks it as a reference to section type `t`.
uint32_t resourceId(const cdc::Section& s, uint32_t off, cdc::SectionType t) {
    const auto refs = resourceRefs(s);
    const auto it = refs.find(off);
    if (it == refs.end() || it->second != uint32_t(t) || off + 4 > s.data.size()) return 0;
    uint32_t id;
    std::memcpy(&id, s.data.data() + off, 4);
    return id;
}

} // namespace

Unit parseUnit(const cdc::Drm& d) {
    if (d.sections.empty() || d.sections[0].type != cdc::SectionType::General)
        throw FormatError("unit DRM: section 0 is not a general (level) section");
    const View level(d, 0);
    Unit u;

    // Level+0x74 -> unit data; two intro arrays {u32 count @+0x0C, Intro* @+0x10} and
    // {count @+0x1C, Intro* @+0x20}, 0x90 bytes each (searched by FUN_005B3860).
    const auto up = level.ptr(0x74, "Level.unitData");
    const View ud(d, up.section);
    for (const auto [countOff, listOff] : {std::pair{0x0Cu, 0x10u}, std::pair{0x1Cu, 0x20u}}) {
        const auto n = ud.at<uint32_t>(up.offset + countOff);
        if (n == 0) continue;
        const auto lp = ud.ptr(up.offset + listOff, "unit intro list");
        const View il(d, lp.section);
        for (uint32_t k = 0; k < n; ++k) {
            const size_t b = lp.offset + 0x90 * size_t(k);
            Intro in{};
            for (int i = 0; i < 3; ++i) {
                in.rotation[i] = il.at<float>(b + 4 * i);
                in.position[i] = il.at<float>(b + 0x10 + 4 * i);
                in.scale[i] = il.at<float>(b + 0x40 + 4 * i);
            }
            in.objectId = il.at<uint16_t>(b + 0x50);
            in.uniqueId = il.at<int32_t>(b + 0x58);
            for (int i = 0; i < 4; ++i) in.flags[i] = il.at<uint8_t>(b + 0x70 + i);
            u.intros.push_back(in);
        }
    }

    // Level+0x90 -> cell group data {header*, ?, streamGroups*, symbols*, cells*, ?, imfs*}.
    const auto cp = level.ptr(0x90, "Level.cellGroupData");
    const View cg(d, cp.section);
    const auto hp = cg.ptr(cp.offset, "cell group header");
    const View hdr(d, hp.section);
    const auto numStreamGroups = hdr.at<uint32_t>(hp.offset + 0x0C), numImfs = hdr.at<uint32_t>(hp.offset + 0x1C);
    if (numStreamGroups) {
        // 20 bytes {name*, numCells, cells*, streamFileName* (null = no DRM), resolveObject}.
        const auto sp = cg.ptr(cp.offset + 0x08, "stream group list");
        const View sg(d, sp.section);
        for (uint32_t k = 0; k < numStreamGroups; ++k) {
            const auto it = sg.ptrs.find(sp.offset + 20 * k + 12);
            sg.at<uint32_t>(sp.offset + 20 * k + 16); // bounds check of the whole record
            if (it != sg.ptrs.end()) u.streamGroups.push_back(sg.str(it->second));
        }
    }
    if (numImfs) {
        // 0x70 bytes: D3D row-major matrix, +0x44 name*, +0x48 DTP id (resource relocation).
        const auto ip = cg.ptr(cp.offset + 0x18, "IMF list");
        const View im(d, ip.section);
        for (uint32_t k = 0; k < numImfs; ++k) {
            const size_t b = ip.offset + 0x70 * size_t(k);
            ImfPlacement p{};
            for (int i = 0; i < 16; ++i) p.matrix[i] = im.at<float>(b + 4 * i);
            p.dtpId = im.at<uint32_t>(b + 0x48);
            if (const auto it = im.ptrs.find(uint32_t(b + 0x44)); it != im.ptrs.end()) p.name = im.str(it->second);
            u.imfs.push_back(std::move(p));
        }
    }
    return u;
}

std::map<uint16_t, std::string> parseObjectList(std::string_view text) {
    std::map<uint16_t, std::string> out;
    bool first = true;
    while (!text.empty()) {
        auto nl = text.find('\n');
        auto line = text.substr(0, nl);
        text = nl == text.npos ? std::string_view{} : text.substr(nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        if (std::exchange(first, false)) continue; // object count
        const auto comma = line.find(',');
        if (comma == line.npos) continue;
        std::string name(line.substr(comma + 1));
        for (auto& ch : name) ch = char(std::tolower(uint8_t(ch)));
        out[uint16_t(std::strtoul(std::string(line.substr(0, comma)).c_str(), nullptr, 10))] = std::move(name);
    }
    return out;
}

std::vector<uint32_t> objectRenderModels(const cdc::Drm& d) {
    const cdc::Section* object = nullptr;
    for (const auto& s : d.sections)
        if (s.type == cdc::SectionType::Object) { object = &s; break; }
    if (!object) return {};
    uint32_t rootIndex = 0;
    const uint32_t rootId = resourceId(*object, 0, cdc::SectionType::DtpData);
    if (!rootId || !findSection(d, cdc::SectionType::DtpData, rootId, &rootIndex))
        throw FormatError("object section without its ObjectBase DTP");
    // ObjectSection construct (0x54E200): Object.numModels = base+0xA0 (u16), Object.modelList = base+0xA4.
    const View root(d, rootIndex);
    const auto n = root.at<uint16_t>(0xA0);
    if (n == 0) return {};
    const auto lp = root.ptr(0xA4, "Object model list");
    const auto& list = d.sections[lp.section];
    std::vector<uint32_t> out;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t modelDtp = resourceId(list, lp.offset + 4 * k, cdc::SectionType::DtpData);
        const auto* model = modelDtp ? findSection(d, cdc::SectionType::DtpData, modelDtp) : nullptr;
        out.push_back(model ? resourceId(*model, 0x64, cdc::SectionType::RenderResource) : 0);
    }
    return out;
}

void introMatrix(const Intro& in, float out[16]) {
    const float sx = std::sin(in.rotation[0]), cx = std::cos(in.rotation[0]), sy = std::sin(in.rotation[1]),
                cy = std::cos(in.rotation[1]), sz = std::sin(in.rotation[2]), cz = std::cos(in.rotation[2]);
    // R = Rx * Ry * Rz (column vectors), row-major r[row][col].
    const float r[3][3] = {
        {cy * cz, -cy * sz, sy},
        {sx * sy * cz + cx * sz, -sx * sy * sz + cx * cz, -sx * cy},
        {-cx * sy * cz + sx * sz, cx * sy * sz + sx * cz, cx * cy},
    };
    for (int c = 0; c < 3; ++c) {
        const float s = in.scaled() ? in.scale[c] : 1.0f;
        for (int row = 0; row < 3; ++row) out[c * 4 + row] = r[row][c] * s;
        out[c * 4 + 3] = 0;
    }
    for (int i = 0; i < 3; ++i) out[12 + i] = in.position[i];
    out[15] = 1;
}

} // namespace ou::formats
