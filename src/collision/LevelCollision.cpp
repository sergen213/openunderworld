// Unit DRM -> CollisionMesh. Layouts: docs/formats/COLLISION.md. Pointers are DRM relocations
// (internal and external) resolved with cdc::sectionPointers.
#include "collision/Collision.hpp"

#include <cstring>
#include <map>

namespace ou::collision {
namespace {

using cdc::FormatError;
using cdc::Pointer;

struct Unit {
    const cdc::Drm& drm;
    std::map<uint32_t, std::unordered_map<uint32_t, Pointer>> ptrs;

    std::span<const uint8_t> data(uint32_t section) const { return drm.sections.at(section).data; }
    std::optional<Pointer> ptr(Pointer at) {
        auto it = ptrs.find(at.section);
        if (it == ptrs.end()) it = ptrs.emplace(at.section, cdc::sectionPointers(drm, at.section)).first;
        const auto p = it->second.find(at.offset);
        if (p == it->second.end()) return std::nullopt;
        return p->second;
    }
    Pointer need(Pointer at, const char* what) {
        const auto p = ptr(at);
        if (!p) throw FormatError(std::string("missing pointer: ") + what);
        return *p;
    }
    // Missing trailing bytes read as 0: the last face / BIH node of a section is stored without its
    // padding (sections are 3 resp. 2 bytes short of count * stride).
    template <class T> T at(Pointer p, size_t extra = 0) const {
        const auto d = data(p.section);
        const size_t off = size_t(p.offset) + extra;
        T v{};
        if (off < d.size()) std::memcpy(&v, d.data() + off, std::min(sizeof v, d.size() - off));
        return v;
    }
    void need(Pointer p, size_t bytes, const char* what) const {
        const auto d = data(p.section);
        if (p.offset > d.size() || d.size() - p.offset < bytes) throw FormatError(std::string(what) + " past section end");
    }
};

vec3 vec(const Unit& u, Pointer p, size_t off) {
    return {u.at<float>(p, off), u.at<float>(p, off + 4), u.at<float>(p, off + 8)};
}

// One cdc collision Mesh (TR8 layout, 0x60 bytes) at `m`, appended to `out` with `offset` added.
void decodeMesh(Unit& u, Pointer m, uint16_t group, vec3 offset, CollisionMesh& out, std::vector<std::string>& warn) {
    u.need(m, 0x58, "collision mesh header");
    const vec3 pos = vec(u, m, 0x20) + offset;
    const uint32_t numNodes = u.at<uint32_t>(m, 0x40), numFaces = u.at<uint32_t>(m, 0x44),
                   numVerts = u.at<uint32_t>(m, 0x48);
    const uint16_t vertexType = u.at<uint16_t>(m, 0x54);
    const std::string where = "terrain group " + std::to_string(group);
    if (vertexType != 1) { // 0 = int16 xyz in TRL/TRA; never observed in TRU (all 361 meshes are float)
        warn.push_back(where + ": unsupported collision vertex type " + std::to_string(vertexType));
        return;
    }
    if (numFaces == 0) return;
    const Pointer verts = u.need({m.section, m.offset + 0x30}, "collision vertices"),
                  faces = u.need({m.section, m.offset + 0x34}, "collision faces"),
                  nodes = u.need({m.section, m.offset + 0x38}, "collision BIH");
    u.need(verts, 16 * size_t(numVerts), "collision vertices");
    u.need(faces, 20 * size_t(numFaces) - 3, "collision faces");
    u.need(nodes, 16 * size_t(numNodes) - 2, "collision BIH");
    // The game's BIH (not used: we build our own BVH). Leaf face counts must cover every face once.
    uint64_t leafFaces = 0;
    for (uint32_t n = 0; n < numNodes; ++n) leafFaces += u.at<uint8_t>(nodes, 16 * size_t(n) + 13);
    if (leafFaces != numFaces)
        warn.push_back(where + ": BIH leaves cover " + std::to_string(leafFaces) + " of " + std::to_string(numFaces) + " faces");

    const auto base = uint32_t(out.vertices.size());
    for (uint32_t i = 0; i < numVerts; ++i) out.vertices.push_back(vec(u, verts, 16 * size_t(i)) + pos);
    for (uint32_t i = 0; i < numFaces; ++i) {
        const size_t f = 20 * size_t(i);
        Face face;
        for (int k = 0; k < 3; ++k) {
            const uint32_t v = u.at<uint32_t>(faces, f + 4 * k);
            if (v >= numVerts) throw FormatError(where + ": face vertex out of range");
            face.v[k] = base + v;
        }
        face.flags = u.at<uint8_t>(faces, f + 12);
        face.mask = u.at<uint8_t>(faces, f + 13);
        face.clientFlags = u.at<uint16_t>(faces, f + 14);
        face.material = u.at<uint8_t>(faces, f + 16);
        face.group = group;
        out.faces.push_back(face);
    }
}

// markup2 items (0xB0 bytes) listed by the unit data at Level+0x74 (+0x2C count, +0x30 item pointers).
void decodeMarkup(Unit& u, Pointer level, CollisionMesh& out, std::vector<std::string>& warn) {
    const auto ud = u.ptr({level.section, level.offset + 0x74});
    if (!ud) return;
    const uint32_t count = u.at<uint32_t>(*ud, 0x2C);
    if (count == 0) return;
    const Pointer list = u.need({ud->section, ud->offset + 0x30}, "markup list");
    std::vector<std::optional<Pointer>> items(count);
    std::map<std::pair<uint32_t, uint32_t>, uint16_t> idAt; // item -> markup id (camera records list items)
    for (uint32_t i = 0; i < count; ++i) {
        items[i] = u.ptr({list.section, list.offset + 4 * i});
        if (items[i]) idAt[{items[i]->section, items[i]->offset}] = u.at<uint16_t>(*items[i], 0x48);
    }
    // Camera records (unit data +0x208 count, +0x20C records of 0x70 bytes; tru.exe 0x606160 / 0x606310 / 0x605ED0).
    const uint32_t numRecords = u.at<uint32_t>(*ud, 0x208);
    const auto records = numRecords ? u.ptr({ud->section, ud->offset + 0x20C}) : std::nullopt;
    const auto record = [&](Pointer r) {
        CameraRecord c;
        c.type = u.at<uint32_t>(r, 0x4C);
        if (const uint32_t n = u.at<uint32_t>(r, 0x44); n)
            if (const auto p = u.ptr({r.section, r.offset + 0x48}))
                for (uint32_t k = 0; k < n; ++k) c.pos.push_back(vec(u, *p, 32 * k)), c.tangent.push_back(vec(u, *p, 32 * k + 16));
        if (const uint32_t n = u.at<uint32_t>(r, 0x50); n)
            if (const auto p = u.ptr({r.section, r.offset + 0x54}))
                for (uint32_t k = 0; k < n; ++k)
                    if (const auto it = u.ptr({p->section, p->offset + 4 * k}); it && idAt.contains({it->section, it->offset}))
                        c.markups.push_back(idAt.at({it->section, it->offset}));
                    else
                        c.markups.push_back(0xFFFF), warn.push_back("camera record: markup " + std::to_string(k) + " not a markup item");
        c.angleSpeed = u.at<float>(r, 0x58), c.distanceSpeed = u.at<float>(r, 0x5C), c.radius = u.at<float>(r, 0x60);
        if (const uint32_t n = u.at<uint32_t>(r, 0x64); n)
            if (const auto p = u.ptr({r.section, r.offset + 0x68}))
                for (uint32_t k = 0; k < n; ++k) c.forces.push_back({u.at<uint32_t>(*p, 0x44 * k), u.at<float>(*p, 0x44 * k + 4)});
        c.distance = u.at<uint8_t>(r, 0x6C) != 0, c.inside = u.at<uint8_t>(r, 0x6D) != 0;
        return c;
    };
    for (uint32_t i = 0; i < count; ++i) {
        const auto& item = items[i];
        if (!item) { warn.push_back("markup " + std::to_string(i) + ": null"); continue; }
        u.need(*item, 0xA4, "markup item");
        Markup mk;
        mk.id = u.at<uint16_t>(*item, 0x48);
        mk.intro = u.at<uint16_t>(*item, 0x4A);
        mk.type = u.at<uint8_t>(*item, 0x4D);
        const uint32_t n = u.at<uint32_t>(*item, 0x9C);
        if (n) {
            const Pointer pts = u.need({item->section, item->offset + 0xA0}, "markup points");
            u.need(pts, 32 * size_t(n), "markup points");
            // 4x4 matrix, rows = basis vectors, row 3 = translation (identity in l4_pool; l4_traprooms' poles are turned + moved).
            const vec3 r0 = vec(u, *item, 0), r1 = vec(u, *item, 16), r2 = vec(u, *item, 32), r3 = vec(u, *item, 48);
            for (uint32_t k = 0; k < n; ++k) {
                const vec3 p = vec(u, pts, 32 * size_t(k)); // +16: (0,0,1000,0) in every point seen, unknown
                mk.points.push_back(r0 * p.x + r1 * p.y + r2 * p.z + r3);
            }
        }
        // Camera commands {u32 command, args} x 0x18 bytes (+0x64 count, +0x68 list; tru.exe 0x5FE800 -> 0x6019D0):
        // command 0x12 points at one of the unit's camera records.
        if (const uint32_t nc = u.at<uint32_t>(*item, 0x64); nc && records)
            if (const auto cmds = u.ptr({item->section, item->offset + 0x68}))
                for (uint32_t k = 0; k < nc; ++k)
                    if (u.at<uint32_t>(*cmds, 0x18 * k) == 0x12)
                        if (const auto r = u.ptr({cmds->section, cmds->offset + 0x18 * k + 4});
                            r && r->section == records->section && r->offset >= records->offset &&
                            (r->offset - records->offset) % 0x70 == 0 && (r->offset - records->offset) / 0x70 < numRecords)
                            mk.camera = record(*r);
        out.markup.push_back(std::move(mk));
    }
}

} // namespace

uint32_t markupFlags(uint8_t type) {
    static constexpr uint32_t kType[21] = {0, 0x40000000, 0x20000000, 0x10000000, 0x8000000, 0x4000000, 0x2000000,
        0x1000000, 0x800000, 0x400000, 0x100000, 0x80000, 0x40000, 0x20000, 0x10000, 0x10, 0x4000, 0x2000, 0x1000,
        0x800, 0x400};
    const uint32_t a = type < 21 ? kType[type] : 0;
    const uint32_t b = type == 10 || type == 21 ? 0 : type >= 16 && type <= 20 ? 0x8001 : type >= 1 && type <= 15 ? 1 : 0x80000000;
    return a | b;
}

std::string_view markupTypeName(uint8_t type) {
    // Names of the MUD flag bits (TRLAU-menu-hook Markup.h, TR8 branch) the type maps to.
    static constexpr std::string_view kNames[] = {"none", "ledgegrab", "jumplanding", "enemydropdown", "pole",
        "vertpole", "horizpole", "zipline", "railslide", "ladder", "water", "rope", "cover", "perch",
        "balancebeam", "pillar"};
    return type < std::size(kNames) ? kNames[type] : "unknown";
}

CollisionMesh loadLevelCollision(const cdc::Drm& drm, std::vector<std::string>* warnings) {
    std::vector<std::string> local;
    auto& warn = warnings ? *warnings : local;
    if (drm.sections.empty() || drm.sections[0].type != cdc::SectionType::General)
        throw FormatError("not a unit DRM (section 0 is not general)");
    Unit u{drm, {}};
    const Pointer level{0, 0};
    const auto terrain = u.ptr(level);
    if (!terrain) throw FormatError("not a unit DRM (no Level::terrain)");
    u.need(level, 0x8C, "Level");
    u.need(*terrain, 0x18, "Terrain");
    // Collision vertices are stored relative to the Level offset at +0x80 (render terrain, markup
    // and portals are not); adding it aligns collision with the render terrain (COLLISION.md).
    const vec3 levelOffset = vec(u, level, 0x80);

    CollisionMesh out;
    const uint32_t numGroups = u.at<uint32_t>(*terrain, 8);
    out.groupModels_.assign(numGroups, ~0u);
    out.groupEnabled_.assign(numGroups, true);
    const auto unitData = u.ptr({level.section, level.offset + 0x74});
    std::optional<Pointer> models;
    uint32_t numModels = 0;
    if (unitData) {
        numModels = u.at<uint32_t>(*unitData, 0xA0);
        if (numModels) models = u.need({unitData->section, unitData->offset + 0xA4}, "unit model list");
    }
    if (numGroups) {
        const Pointer groups = u.need({terrain->section, terrain->offset + 12}, "terrain groups");
        u.need(groups, 0x50 * size_t(numGroups), "terrain groups");
        for (uint32_t g = 0; g < numGroups; ++g) {
            const Pointer group{groups.section, groups.offset + 0x50 * g};
            bool cameraIgnores = false, sightIgnores = false, playerIgnores = false;
            if (const auto extra = u.ptr({group.section, group.offset + 0x30})) {
                // tru.exe 0x5C2440 sets both disabled bits; 0x5C2480 confusingly returns them as IsEnabled.
                const uint32_t flags = u.at<uint32_t>(*extra);
                out.groupEnabled_[g] = (flags & 4) == 0 || (flags & 8) == 0;
                cameraIgnores = flags & 0x200;
                sightIgnores = flags & 0xE0008; // the targeting ray's filter (kSightMask)
                playerIgnores = flags & 0x800;  // the player's rays (flags 0x2C19E7, class bit 4) skip these (kSolidMask)
                if (models && extra->section == models->section)
                    for (uint32_t m = 0; m < numModels; ++m)
                        if (extra->offset == models->offset + 0x30 * m) {
                            out.groupModels_[g] = m; // NtTerrainGroup::ResolveTerrainGroup 0x4AACA0
                            break;
                        }
            }
            if (const auto mesh = u.ptr({group.section, group.offset + 0x38})) {
                const size_t first = out.faces.size();
                decodeMesh(u, *mesh, uint16_t(g), levelOffset, out, warn);
                for (size_t f = first; f < out.faces.size(); ++f) {
                    if (playerIgnores) { // not Lara's collision: only the camera's and the sight ray's own bits are left
                        const uint8_t raw = out.faces[f].mask;
                        out.faces[f].mask = uint8_t(((raw & 2) && !cameraIgnores ? kNotPlayerCameraBit : 0) | ((raw & 1) && !sightIgnores ? kNotPlayerSightBit : 0));
                        continue;
                    }
                    if (cameraIgnores) out.faces[f].mask &= uint8_t(~0x02); // the camera's terrain bit (kCameraMask)
                    if (!sightIgnores && (out.faces[f].mask & 1)) out.faces[f].mask |= 0x04; // the sight bit (kSightMask)
                }
            }
        }
    }
    decodeMarkup(u, level, out, warn);
    out.build();
    return out;
}

} // namespace ou::collision
