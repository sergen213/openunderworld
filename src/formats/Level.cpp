// Unit (level) DRM structures: intros, stream groups, IMF placements; object model lists.
// Evidence and offsets: docs/formats/LEVELS.md.
#include "formats/Formats.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <set>
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

// Type-3/type-5 resource relocation for patch offset `off`: section type of the referenced resource (last entry wins),
// or ~0u. The id itself stays in the data at the patch offset (as in materialTextures()). A linear scan: building a
// map per lookup was a third of a level load.
uint32_t resourceRef(const cdc::Section& s, uint32_t off) {
    const auto r = s.relocations;
    uint32_t type = ~0u;
    if (r.size() < 20) return type;
    uint32_t c[5];
    std::memcpy(c, r.data(), sizeof c);
    const size_t t3 = 20 + 8 * size_t(c[0]) + 8 * size_t(c[1]), t5 = t3 + 4 * (size_t(c[2]) + c[3]);
    for (const auto [p0, n] : {std::pair{t3, c[2]}, std::pair{t5, c[4]}})
        for (uint32_t k = 0; k < n; ++k) {
            if (p0 + 4 * (k + 1ull) > r.size()) throw FormatError("resource relocation table truncated");
            uint32_t v;
            std::memcpy(&v, r.data() + p0 + 4 * size_t(k), 4);
            if ((v & 0x1FFFFFF) * 4 == off) type = v >> 25;
        }
    return type;
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
    if (resourceRef(s, off) != uint32_t(t) || off + 4 > s.data.size()) return 0;
    uint32_t id;
    std::memcpy(&id, s.data.data() + off, 4);
    return id;
}

std::shared_ptr<const MultiSpline> parseMultiSpline(const cdc::Drm& d, cdc::Pointer p) {
    const View v(d, p.section);
    auto out = std::make_shared<MultiSpline>();
    const View level(d, 0);
    for (int c = 0; c < 3; ++c) out->origin[c] = level.at<float>(0x80 + 4 * c);
    for (const auto [off, rotation] : {std::pair{0x40u, false}, std::pair{0x44u, true}}) {
        const auto it = v.ptrs.find(p.offset + off);
        if (it == v.ptrs.end()) continue;
        const View t(d, it->second.section);
        const uint32_t base = it->second.offset;
        SplineTrack track;
        const uint16_t count = t.at<uint16_t>(base);
        track.flags = t.at<uint8_t>(base + 3);
        track.end = t.at<float>(base + 4);
        const size_t stride = rotation ? 8 : 16;
        if (!count) throw FormatError("spline track without keys");
        track.keys.resize(stride * count);
        for (size_t k = 0; k < track.keys.size(); ++k) track.keys[k] = t.at<float>(base + 0x10 + 4 * k);
        (rotation ? out->rotation : out->position) = std::move(track);
    }
    return out;
}

} // namespace

// Cursor 0x47D320 / 0x47D3B0 (segment = first key whose end time t1 >= t) and evaluation 0x47CD40 / 0x47D180.
std::array<float, 4> evalSpline(const SplineTrack& track, float t, bool rotation) {
    const size_t stride = rotation ? 8 : 16, count = track.keys.size() / stride;
    t = std::clamp(t, 0.0f, track.end);
    size_t i = 0;
    while (i + 1 < count && t > track.keys[stride * i + (rotation ? 5 : 13)]) ++i;
    const float* k = &track.keys[stride * i];
    std::array<float, 4> out{k[0], k[1], k[2], k[3]};
    if (i + 1 >= count) return out;
    const float* n = k + stride;
    const float u = (t - k[rotation ? 4 : 12]) * k[rotation ? 7 : 15];
    if (u == 0) return out;
    if (u == 1) return {n[0], n[1], n[2], n[3]};
    if (rotation) { // slerp (FUN_0049D860), shortest arc
        float dot = k[0] * n[0] + k[1] * n[1] + k[2] * n[2] + k[3] * n[3];
        const float sign = dot < 0 ? -1.0f : 1.0f;
        dot *= sign;
        float a = 1 - u, b = u * sign;
        if (dot < 0.9995f) {
            const float th = std::acos(dot), s = std::sin(th);
            a = std::sin((1 - u) * th) / s, b = std::sin(u * th) / s * sign;
        }
        float len = 0;
        for (int c = 0; c < 4; ++c) out[c] = a * k[c] + b * n[c], len += out[c] * out[c];
        for (auto& c : out) c /= std::sqrt(len);
        return out;
    }
    const float u2 = u * u, u3 = u2 * u;
    if (track.flags & 8) { // Bezier: key, its control [8..11], the next key's control [4..7], next key
        const float v = 1 - u;
        for (int c = 0; c < 4; ++c) out[c] = v * v * v * k[c] + 3 * u * v * v * k[8 + c] + 3 * v * u2 * n[4 + c] + u3 * n[c];
    } else { // Hermite: out-tangent [4..7], the next key's in-tangent [8..11]
        for (int c = 0; c < 4; ++c)
            out[c] = (2 * u3 - 3 * u2 + 1) * k[c] + (u3 - 2 * u2 + u) * k[4 + c] + (3 * u2 - 2 * u3) * n[c] + (u3 - u2) * n[8 + c];
    }
    return out;
}

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
            if (const auto it = il.ptrs.find(uint32_t(b + 0x78)); it != il.ptrs.end()) in.data = it->second;
            in.scriptId = il.at<uint32_t>(b + 0x7C);
            if (const auto it = il.ptrs.find(uint32_t(b + 0x68)); it != il.ptrs.end()) {
                in.spline = parseMultiSpline(d, it->second);
                in.splineFlags = il.at<uint16_t>(b + 0x62);
                in.splineMode = il.at<uint32_t>(b + 0x64);
                in.splineFrom = il.at<int16_t>(b + 0x6C);
                in.splineTo = il.at<int16_t>(b + 0x6E);
            }
            u.intros.push_back(in);
        }
    }

    // Planner nodes (0x5C8EA0 / 0x669D00): +0x118 count, +0x11C -> 0x60-byte records {matrix, +0x4C node count,
    // +0x50 -> 0x60-byte nodes {matrix, ...}}; a node's point = record matrix * node translation. Scripts name one
    // as a noderef {record, node} (enemybase 27 GuardNode).
    if (const auto n = ud.at<uint32_t>(up.offset + 0x118); n && ud.ptrs.count(up.offset + 0x11C)) {
        const auto rp = ud.ptr(up.offset + 0x11C, "unit planner records");
        const View rv(d, rp.section);
        for (uint32_t r = 0; r < n; ++r) {
            const size_t b = rp.offset + 0x60 * size_t(r);
            glm::mat4 rm;
            for (int i = 0; i < 16; ++i) rm[i / 4][i % 4] = rv.at<float>(b + 4 * i);
            auto& nodes = u.plannerNodes.emplace_back();
            const auto np = rv.ptrs.find(uint32_t(b + 0x50));
            if (np == rv.ptrs.end()) continue;
            const View nv(d, np->second.section);
            for (uint32_t k = 0, nk = rv.at<uint32_t>(b + 0x4C); k < nk; ++k) {
                const size_t q = np->second.offset + 0x60 * size_t(k);
                nodes.push_back(glm::vec3(rm * glm::vec4(nv.at<float>(q + 0x30), nv.at<float>(q + 0x34), nv.at<float>(q + 0x38), 1)));
            }
        }
    }

    // Camera splines (LEVELS.md "Camera splines"): record +0x30 offset, +0x44 time mode, +0x48 duration, +0x4C FOV,
    // +0x50 count / +0x54 -> 0xC0-byte camera keys, +0x58 -> target track, +0x5C has target, +0x5D target fits.
    // Camera key: +0x00 position, +0x10 quaternion, +0x20 tangent, +0x30 time, +0x34 u16[64] ease, +0xB4 event
    // count, +0xB8 -> 8-byte {type, arg}. Target track: +0x44 mode, +0x48 duration, +0x4C count / +0x50 -> 0xB0-byte
    // keys {+0x00 position, +0x10 tangent, +0x20 time, +0x24 ease, +0xA4 follow Lara, +0xA8 count, +0xAC -> 12-byte
    // {type, arg, arg2}}. Time modes map to the player's (0x60AC00: 0 -> 1, 2 -> 2, 3 -> 3; 0x60ADA0: 0 -> 2,
    // 1 -> 1, 2 -> 3).
    if (const auto n = ud.at<uint32_t>(up.offset + 0x68); n && ud.ptrs.count(up.offset + 0x6C)) {
        const auto lp = ud.ptr(up.offset + 0x6C, "unit camera splines");
        const View cl(d, lp.section);
        auto events = [&](const View& kv, uint32_t countOff, uint32_t listOff, uint32_t stride) {
            std::vector<CameraSplineEvent> out;
            const auto it = kv.ptrs.find(listOff);
            if (it == kv.ptrs.end()) return out;
            const View ev(d, it->second.section);
            for (uint32_t e = 0, ne = kv.at<uint32_t>(countOff); e < ne; ++e) {
                const uint32_t b = it->second.offset + stride * e;
                CameraSplineEvent x;
                x.type = ev.at<int32_t>(b);
                x.arg = ev.at<uint32_t>(b + 4);
                x.value = ev.at<float>(b + 4);
                if (stride == 12) x.value2 = ev.at<float>(b + 8);
                out.push_back(x);
            }
            return out;
        };
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t r = lp.offset + 0x60 * k;
            CameraSpline s;
            for (int c = 0; c < 3; ++c) s.offset[c] = cl.at<float>(r + 0x30 + 4 * c);
            const uint32_t mode = cl.at<uint32_t>(r + 0x44);
            s.camera.mode = mode == 2 ? 2 : mode == 3 ? 3 : 1;
            s.camera.duration = cl.at<float>(r + 0x48);
            s.fovDeg = cl.at<float>(r + 0x4C);
            if (const auto it = cl.ptrs.find(r + 0x54); it != cl.ptrs.end()) {
                const View kv(d, it->second.section);
                for (uint32_t i = 0, nk = cl.at<uint32_t>(r + 0x50); i < nk; ++i) {
                    const uint32_t b = it->second.offset + 0xC0 * i;
                    CameraSplineKey key;
                    for (int c = 0; c < 3; ++c) key.pos[c] = kv.at<float>(b + 4 * c), key.tangent[c] = kv.at<float>(b + 0x20 + 4 * c);
                    for (int c = 0; c < 4; ++c) key.rotation[c] = kv.at<float>(b + 0x10 + 4 * c);
                    key.time = kv.at<float>(b + 0x30);
                    for (int c = 0; c < 64; ++c) key.ease[c] = kv.at<uint16_t>(b + 0x34 + 2 * c);
                    key.events = events(kv, b + 0xB4, b + 0xB8, 8);
                    s.camera.keys.push_back(std::move(key));
                }
            }
            const auto tp = cl.ptrs.find(r + 0x58);
            if (cl.at<uint8_t>(r + 0x5C) && tp != cl.ptrs.end()) {
                const View tv(d, tp->second.section);
                const uint32_t t = tp->second.offset;
                CameraSplineTrack track;
                const uint32_t tmode = tv.at<uint32_t>(t + 0x44);
                track.mode = tmode == 0 ? 2 : tmode == 2 ? 3 : 1;
                track.duration = tv.at<float>(t + 0x48);
                if (const auto it = tv.ptrs.find(t + 0x50); it != tv.ptrs.end()) {
                    const View kv(d, it->second.section);
                    for (uint32_t i = 0, nk = tv.at<uint32_t>(t + 0x4C); i < nk; ++i) {
                        const uint32_t b = it->second.offset + 0xB0 * i;
                        CameraSplineKey key;
                        for (int c = 0; c < 3; ++c) key.pos[c] = kv.at<float>(b + 4 * c), key.tangent[c] = kv.at<float>(b + 0x10 + 4 * c);
                        key.time = kv.at<float>(b + 0x20);
                        for (int c = 0; c < 64; ++c) key.ease[c] = kv.at<uint16_t>(b + 0x24 + 2 * c);
                        key.followPlayer = kv.at<uint8_t>(b + 0xA4) != 0;
                        key.events = events(kv, b + 0xA8, b + 0xAC, 12);
                        track.keys.push_back(std::move(key));
                    }
                }
                if (!track.keys.empty()) s.target = std::move(track);
                s.targetFitsCamera = cl.at<uint8_t>(r + 0x5D) != 0;
            }
            u.cameraSplines.push_back(std::move(s));
        }
    }

    // Unit data +0x00 numLights, +0x04 -> pointers to 0xE0-byte light records {+0x00 matrix rows X, Y, Z = direction,
    // pos; +0x48 light DTP; +0x4C intensity (if +0x50); +0x51..+0x53 colour (if +0x54); +0x58 radius (if +0x5C)}.
    // The light DTP is copied to PCLightResource +0x42C (0x50A000): +0x00 u32 type (0 point, 1 spot, 2 sun),
    // +0x04 radius, +0x08 intensity, +0x0C/+0x10 spot cone angles, +0x14 spot near width, +0x18 u8 rgb,
    // +0x1C f32[255] falloff curve, +0x41C flicker flag, +0x420 period, +0x424 f32[256] flicker curve,
    // +0x824 cookie flag, +0x828 cookie texture, +0x82C scale, +0x830 rotation, +0x834 scroll, +0x144C shadows
    // (tru.exe 0x5CEC60, 0x5C6530, 0x50A3C0, 0x50AFB0, 0x50B910; LEVELS.md "Lights").
    if (const auto n = ud.at<uint32_t>(up.offset); n && ud.ptrs.count(up.offset + 4)) {
        const auto lp = ud.ptr(up.offset + 4, "unit light list");
        const View ll(d, lp.section);
        for (uint32_t k = 0; k < n; ++k) {
            const auto rp = ll.ptrs.find(uint32_t(lp.offset + 4 * k));
            if (rp == ll.ptrs.end()) continue;
            const View rec(d, rp->second.section);
            const uint32_t r = rp->second.offset;
            // +0x48 is a type-7 resource reference (id in the data), resolved within the unit DRM.
            uint32_t dtpIndex = 0;
            const uint32_t dtpId = resourceId(d.sections[rp->second.section], r + 0x48, cdc::SectionType::DtpData);
            if (!dtpId || !findSection(d, cdc::SectionType::DtpData, dtpId, &dtpIndex)) continue;
            const View dtp(d, dtpIndex);
            const uint32_t type = dtp.at<uint32_t>(0);
            const float intensity = rec.at<uint8_t>(r + 0x50) ? rec.at<float>(r + 0x4C) : dtp.at<float>(0x08);
            const bool colourOverride = rec.at<uint8_t>(r + 0x54) != 0;
            float color[3];
            for (int c = 0; c < 3; ++c)
                color[c] = intensity * (colourOverride ? rec.at<uint8_t>(r + 0x51 + c) : dtp.at<uint8_t>(0x18 + c)) / 255.0f;
            auto& e = u.environment;
            if (type == 2) {
                if (e.sun) continue; // first sun wins
                for (int c = 0; c < 3; ++c) {
                    e.sunDirection[c] = rec.at<float>(r + 0x20 + 4 * c);
                    e.sunColor[c] = color[c];
                    e.sunAxes[0][c] = rec.at<float>(r + 4 * c);
                    e.sunAxes[1][c] = rec.at<float>(r + 0x10 + 4 * c);
                    e.sunOrigin[c] = rec.at<float>(r + 0x30 + 4 * c);
                }
                e.sun = true;
                if (dtp.at<uint8_t>(0x824))
                    e.sunCookie = resourceId(d.sections[dtpIndex], 0x828, cdc::SectionType::Texture);
                e.cookieScale = dtp.at<float>(0x82C);
                e.cookieRotation = dtp.at<float>(0x830);
                e.cookieScroll[0] = dtp.at<float>(0x834), e.cookieScroll[1] = dtp.at<float>(0x838);
                continue;
            }
            auto l = lightFromDtp(d, dtpIndex);
            if (!l) continue;
            for (int c = 0; c < 3; ++c) {
                l->position[c] = rec.at<float>(r + 0x30 + 4 * c);
                l->direction[c] = rec.at<float>(r + 0x20 + 4 * c);
                l->color[c] = color[c];
            }
            if (rec.at<uint8_t>(r + 0x5C) && rec.at<float>(r + 0x58) > 0) l->radius = rec.at<float>(r + 0x58);
            e.lights.push_back(std::move(*l));
        }
    }

    // Level+0x70 -> render-settings DTP: fog colour u8[3] +0x18, far plane +0x38, fog far +0x3C (0 = far plane),
    // fog near +0x40, ambient u8[3] +0x1E4 x f32 +0x1E8 (tru.exe 0x5C7CA0, 0x5CAFF0; SHADERS.md).
    if (const auto it = level.ptrs.find(0x70); it != level.ptrs.end()) {
        const View rs(d, it->second.section);
        const uint32_t o = it->second.offset;
        auto& e = u.environment;
        for (int k = 0; k < 3; ++k) {
            e.fogColor[k] = rs.at<uint8_t>(o + 0x18 + k) / 255.0f;
            e.ambient[k] = rs.at<uint8_t>(o + 0x1E4 + k) * rs.at<float>(o + 0x1E8) / 255.0f;
        }
        e.fogNear = rs.at<float>(o + 0x40);
        e.fogFar = rs.at<float>(o + 0x3C);
        if (e.fogFar == 0) e.fogFar = rs.at<float>(o + 0x38);
        e.valid = true;
        u.playerObjectId = rs.at<uint16_t>(o + 0x294);
        u.scriptId = rs.at<uint32_t>(o + 0x1C0);
        if (const auto f = rs.ptrs.find(o + 0x2B4); f != rs.ptrs.end())
            u.flashlightSpot = View(d, f->second.section).at<uint8_t>(f->second.offset + 0x0A) != 0;
        // Under water (RENDERING.md "Under water"): fog set +0x1C / +0x4C / +0x48 (0 -> far plane; near >= far ->
        // far - 1, 0x5C1F50), screen filters +0x84 / +0x88 over +0x90 / +0x8C (0x5C78A0), event lists +0x68 / +0x6C.
        for (int k = 0; k < 3; ++k) e.waterFogColor[k] = rs.at<uint8_t>(o + 0x1C + k) / 255.0f;
        e.waterFogFar = rs.at<float>(o + 0x48);
        if (e.waterFogFar == 0) e.waterFogFar = rs.at<float>(o + 0x38);
        e.waterFogNear = std::min(rs.at<float>(o + 0x4C), e.waterFogFar - 1);
        e.waterFilter[0] = rs.at<uint32_t>(o + 0x84), e.waterFilter[1] = rs.at<uint32_t>(o + 0x88);
        e.waterFilterRange[0] = rs.at<float>(o + 0x90), e.waterFilterRange[1] = rs.at<float>(o + 0x8C);
        e.waterList[0] = rs.at<uint32_t>(o + 0x68), e.waterList[1] = rs.at<uint32_t>(o + 0x6C);
    }
    // Full-screen water pass (tru.exe 0x5D4AB0 -> 0x6DA130 registers unit data +0x160 count / +0x164 0x100-byte
    // records; 0x6D9DA0 draws those with +0x84 bit 0 through 0x6D8C20 while the camera is under water): the +0x74
    // material's MaterialParams. The +0x6C / +0x70 sun-glow passes and the bit-0-clear ray records are not drawn [U].
    if (const auto n = d.sections[up.section].data.size() >= up.offset + 0x168 ? ud.at<uint32_t>(up.offset + 0x160) : 0;
        n && n < 64 && ud.ptrs.count(up.offset + 0x164)) {
        const auto rp = ud.ptr(up.offset + 0x164, "unit water records");
        const View rv(d, rp.section);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t r = rp.offset + 0x100 * k;
            if (!(rv.at<uint32_t>(r + 0x84) & 1) || cdc::resourceRefType(d.sections[rp.section], r + 0x74) != int(cdc::SectionType::Material))
                continue;
            uint32_t mi = 0;
            if (!findSection(d, cdc::SectionType::Material, rv.at<uint32_t>(r + 0x74), &mi)) continue;
            const auto m = decodeMaterial(d.sections[mi]);
            if (m.psParams.size() < 8) continue;
            for (int i = 0; i < 8; ++i) u.environment.waterPass[i / 4][i % 4] = m.psParams[size_t(i)];
            break;
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
    // +0x14 -> {count, CellTerrainData[count] {terrain ref, flags, light mask, user data}}: the unit's exterior
    // terrains (panorama cylinders / sky domes), added to the scene's "Exterior Cellgroup" (tru.exe 0x572BD0 ->
    // 0x56DE80) and drawn in the far 2 % of the depth range (PCRenderTerrain +0xC8, 0x51D0A0; LEVELS.md).
    if (const auto it = cg.ptrs.find(cp.offset + 0x14); it != cg.ptrs.end()) {
        const View ev(d, it->second.section);
        const auto n = ev.at<uint32_t>(it->second.offset);
        for (uint32_t k = 0; k < n; ++k)
            if (const auto id = resourceId(d.sections[it->second.section], it->second.offset + 4 + 16 * k,
                                           cdc::SectionType::RenderResource))
                u.exteriorTerrains.push_back(id);
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
            // +0x4C..+0x58: instance textures (classes 1-4, light map in +0x4C) for every primgroup (IMFObjectDrawable 0x5B5160).
            for (int t = 0; t < 4; ++t)
                p.textures[t] = resourceId(d.sections[ip.section], uint32_t(b + 0x4C + 4 * t), cdc::SectionType::Texture);
            if (const auto it = im.ptrs.find(uint32_t(b + 0x44)); it != im.ptrs.end()) p.name = im.str(it->second);
            u.imfs.push_back(std::move(p));
        }
    }
    // Level+0 -> Terrain {i16 ?, i16 numPortals, StreamUnitPortal*, ...}; portals are 0x100 bytes, stored like
    // the collision meshes relative to the Level+0x80 offset, which we add (LEVELS.md "Streaming").
    if (!level.ptrs.count(0)) return u; // no Terrain (synthetic levels): no portals
    const auto tp = level.ptr(0, "Level.terrain");
    const View tv(d, tp.section);
    const float frame[3] = {level.at<float>(0x80), level.at<float>(0x84), level.at<float>(0x88)};
    if (const auto n = tv.at<int16_t>(tp.offset + 2); n > 0) {
        const auto pp = tv.ptr(tp.offset + 4, "stream unit portals");
        const View pv(d, pp.section);
        for (int16_t k = 0; k < n; ++k) {
            const size_t b = pp.offset + 0x100 * size_t(k);
            Portal p{};
            pv.at<float>(b + 0xFC); // bounds check of the whole record
            p.toUnit = pv.str({pp.section, uint32_t(b)});
            if (p.toUnit.size() >= 128) throw FormatError("portal unit name not terminated");
            p.toSignal = pv.at<int16_t>(b + 0x80), p.signal = pv.at<int16_t>(b + 0x82), p.toStreamId = pv.at<int16_t>(b + 0x84);
            for (int i = 0; i < 3; ++i) {
                p.min[i] = pv.at<float>(b + 0x90 + 4 * i) + frame[i], p.max[i] = pv.at<float>(b + 0xA0 + 4 * i) + frame[i];
                p.normal[i] = pv.at<float>(b + 0xF0 + 4 * i);
                for (int c = 0; c < 4; ++c) p.quad[c][i] = pv.at<float>(b + 0xB0 + 16 * c + 4 * i) + frame[i];
            }
            u.portals.push_back(std::move(p));
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

std::optional<uint32_t> objectBaseSection(const cdc::Drm& d) {
    for (const auto& s : d.sections) {
        if (s.type != cdc::SectionType::Object) continue;
        uint32_t rootIndex = 0;
        const uint32_t rootId = resourceId(s, 0, cdc::SectionType::DtpData);
        if (!rootId || !findSection(d, cdc::SectionType::DtpData, rootId, &rootIndex))
            throw FormatError("object section without its ObjectBase DTP");
        return rootIndex;
    }
    return std::nullopt;
}

std::optional<scene::Light> lightFromDtp(const cdc::Drm& d, uint32_t dtpIndex) {
    const View dtp(d, dtpIndex);
    const uint32_t type = dtp.at<uint32_t>(0);
    if (type > 1) return std::nullopt;
    scene::Light l;
    l.type = uint8_t(type);
    l.shadow = dtp.at<uint8_t>(0x144C) != 0;
    for (int c = 0; c < 3; ++c) l.color[c] = dtp.at<float>(0x08) * dtp.at<uint8_t>(0x18 + c) / 255.0f;
    l.radius = dtp.at<float>(0x04);
    l.coneInner = dtp.at<float>(0x0C), l.coneOuter = dtp.at<float>(0x10);
    l.nearWidth = dtp.at<float>(0x14);
    // AttenuationSampler row: texel k = round(curve[k] * 255) in R for k < 255, texel 255 = 0 (0x50AFB0).
    l.falloff.assign(256, 0);
    for (int t = 0; t < 255; ++t)
        l.falloff[t] = uint8_t(std::lround(std::clamp(dtp.at<float>(0x1C + 4 * t), 0.0f, 1.0f) * 255.0f));
    if (dtp.at<uint8_t>(0x41C) && dtp.at<float>(0x420) > 0) {
        l.flickerPeriod = dtp.at<float>(0x420);
        l.flicker.resize(256);
        for (int t = 0; t < 256; ++t) l.flicker[t] = dtp.at<float>(0x424 + 4 * t);
    }
    return l;
}

std::vector<scene::Light> objectLights(const cdc::Drm& d) {
    // tru.exe 0x546710 reads the effect list from *(Object +8): the object section's third DTP reference, not ObjectBase.
    const auto obj = std::find_if(d.sections.begin(), d.sections.end(), [](const cdc::Section& s) { return s.type == cdc::SectionType::Object; });
    if (obj == d.sections.end() || obj->data.size() < 12) return {};
    uint32_t fxIndex = 0;
    const uint32_t fxId = resourceId(*obj, 8, cdc::SectionType::DtpData);
    if (!fxId || !findSection(d, cdc::SectionType::DtpData, fxId, &fxIndex)) return {};
    const View root(d, fxIndex);
    const auto n = root.at<uint32_t>(0x3C);
    if (!n || !root.ptrs.count(0x40)) return {};
    const auto lp = root.ptr(0x40, "object effect list");
    const View list(d, lp.section);
    std::vector<scene::Light> out;
    std::set<uint32_t> seen;
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t id = list.at<uint32_t>(lp.offset + 0x24 * k + 0x20); // plain DTP id (no relocation)
        uint32_t index = 0;
        if (!id || !seen.insert(id).second || !findSection(d, cdc::SectionType::DtpData, id, &index)) continue;
        if (auto l = lightFromDtp(d, index)) out.push_back(std::move(*l));
    }
    return out;
}

uint32_t objectFlags(const cdc::Drm& d) {
    const auto base = objectBaseSection(d);
    return base ? View(d, *base).at<uint32_t>(0) : 0;
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
