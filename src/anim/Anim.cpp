#include "anim/Anim.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <glm/gtc/matrix_transform.hpp>

namespace ou::anim {
namespace {

using cdc::FormatError;

struct Reader {
    std::span<const uint8_t> d;
    template <class T> T at(size_t off) const {
        if (off > d.size() || d.size() - off < sizeof(T)) throw FormatError("read past end at " + std::to_string(off));
        T v;
        std::memcpy(&v, d.data() + off, sizeof v);
        return v;
    }
    uint32_t u32(size_t o) const { return at<uint32_t>(o); }
    uint16_t u16(size_t o) const { return at<uint16_t>(o); }
    int16_t s16(size_t o) const { return at<int16_t>(o); }
    float f32(size_t o) const { return at<float>(o); }
    uint8_t u8(size_t o) const { return at<uint8_t>(o); }
};

uint32_t pointer(const cdc::Section& s, uint32_t offset) {
    const auto p = cdc::internalPointer(s, offset);
    if (!p) throw FormatError("missing relocated pointer at " + std::to_string(offset));
    return *p;
}

constexpr uint32_t kModelRenderMesh = 0x64; // dtp Model: render-model resource id (DXHR dtp::Model::renderMesh)
constexpr uint32_t kSegmentStride = 0x40;   // Segment: pivot vec4 at +0x20, parent i32 at +0x38
constexpr uint32_t kResourceModel = 26;

// Hair/cloth chains of a render model: resource header +12 -> {groups*, count};
// group (20 bytes) {segments*, ?*, u32 attachBone, u16 count, u16, u32};
// segment (48 bytes) {vec4 modelSpacePos, ..., +0x1C u32 (bone index << 16 | flags), ...}.
void appendChains(const cdc::Section& rm, scene::Skeleton& sk, std::vector<std::string>& warnings) {
    const auto head = cdc::internalPointer(rm, 12);
    if (!head) return;
    const Reader r{rm.data};
    const auto groups = cdc::internalPointer(rm, *head);
    const uint32_t numGroups = r.u32(*head + 4);
    if (!groups || numGroups > 64) return;
    const auto bind = bindPositions(sk);
    std::vector<glm::vec3> pos(bind.begin(), bind.end());
    for (uint32_t g = 0; g < numGroups; ++g) {
        const uint32_t G = *groups + 20 * g;
        const uint32_t segs = pointer(rm, G);
        const uint32_t attach = r.u32(G + 8), count = r.u16(G + 12);
        if (attach >= sk.bones.size()) {
            warnings.push_back(sk.origin + ": chain attach bone " + std::to_string(attach) + " out of range");
            return;
        }
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t S = segs + 48 * j;
            const uint32_t index = r.u32(S + 0x1C) >> 16;
            if (index != sk.bones.size()) {
                warnings.push_back(sk.origin + ": chain segment index " + std::to_string(index) + " not sequential");
                return;
            }
            const glm::vec3 p(r.f32(S), r.f32(S + 4), r.f32(S + 8));
            const int32_t parent = j == 0 ? int32_t(attach) : int32_t(index - 1);
            const glm::vec3 piv = p - pos[size_t(parent)];
            sk.bones.push_back({parent, {piv.x, piv.y, piv.z}});
            pos.push_back(p);
        }
    }
}

} // namespace

std::vector<scene::Skeleton> decodeSkeletons(const cdc::Drm& drm, std::vector<std::string>& warnings) {
    std::unordered_map<uint32_t, const cdc::Section*> renderModels;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::RenderResource && s.resourceType() == kResourceModel) renderModels[s.id] = &s;
    std::vector<scene::Skeleton> out;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::DtpData ||
            cdc::resourceRefType(s, kModelRenderMesh) != int(cdc::SectionType::RenderResource))
            continue;
        scene::Skeleton sk;
        sk.origin = "dtp section " + std::to_string(s.id);
        try {
            const Reader r{s.data};
            sk.renderModelId = r.u32(kModelRenderMesh);
            const uint32_t n = r.u32(4);
            if (n == 0 || n > 4096) throw FormatError("segment count " + std::to_string(n));
            const uint32_t list = pointer(s, 0xC);
            for (uint32_t i = 0; i < n; ++i) {
                const size_t o = list + size_t(i) * kSegmentStride;
                const int32_t parent = r.at<int32_t>(o + 0x38);
                if (parent >= int32_t(i) || parent < -1 || (i > 0 && parent < 0))
                    throw FormatError("segment " + std::to_string(i) + " parent " + std::to_string(parent));
                sk.bones.push_back({parent, {r.f32(o + 0x20), r.f32(o + 0x24), r.f32(o + 0x28)}});
            }
            if (const auto it = renderModels.find(sk.renderModelId); it != renderModels.end())
                appendChains(*it->second, sk, warnings);
        } catch (const FormatError& e) {
            warnings.push_back(sk.origin + ": " + e.what());
            continue;
        }
        out.push_back(std::move(sk));
    }
    return out;
}

// Chain groups as in appendChains, plus group +4 -> {f32 rest, u16 child, u16 parent} constraint records
// (count = group u16 at +14), segment +0x1C flags bit 0 = pinned, +0x20 = index along the strand.
HairRig decodeHair(const cdc::Drm& drm, const scene::Skeleton& sk) {
    HairRig rig;
    const cdc::Section* rm = nullptr;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::RenderResource && s.resourceType() == kResourceModel && s.id == sk.renderModelId) rm = &s;
    if (!rm) return rig;
    const auto head = cdc::internalPointer(*rm, 12);
    if (!head) return rig;
    const Reader r{rm->data};
    const auto groups = cdc::internalPointer(*rm, *head);
    const uint32_t numGroups = r.u32(*head + 4);
    if (!groups || numGroups > 64) return rig;
    for (uint32_t g = 0; g < numGroups; ++g) {
        const uint32_t G = *groups + 20 * g;
        const uint32_t segs = pointer(*rm, G), count = r.u16(G + 12), numLinks = r.u16(G + 14);
        const auto links = cdc::internalPointer(*rm, G + 4);
        const auto base = uint32_t(rig.particles.size());
        std::vector<uint32_t> strandIndex(count);
        for (uint32_t j = 0; j < count; ++j) {
            const uint32_t S = segs + 48 * j, packed = r.u32(S + 0x1C);
            if ((packed >> 16) >= sk.bones.size()) throw FormatError("hair segment bone out of range");
            rig.particles.push_back({packed >> 16, -1, (packed & 1) != 0, uint8_t(g)});
            strandIndex[j] = r.u32(S + 0x20);
        }
        std::vector<float> best(count, 1e30f);
        for (uint32_t k = 0; links && k < numLinks; ++k) {
            const uint32_t L = *links + 8 * k, a = r.u16(L + 4), b = r.u16(L + 6);
            if (a >= count || b >= count) throw FormatError("hair link index out of range");
            const float rest = r.f32(L);
            rig.links.push_back({base + a, base + b, rest});
            // Orientation reference: the shortest link to the previous segment of the same strand.
            for (const auto [c, p] : {std::pair{a, b}, std::pair{b, a}})
                if (strandIndex[c] == strandIndex[p] + 1 && rest < best[c]) {
                    best[c] = rest;
                    rig.particles[base + c].pred = int32_t(base + p);
                }
        }
    }
    return rig;
}

Clip decodeClip(std::span<const uint8_t> data) {
    const Reader r{data};
    Clip c;
    c.id = uint32_t(r.s16(0x30));
    c.keyCount = r.u16(0x32);
    c.timePerKey = r.u16(0x34);
    const uint32_t segments = r.u8(0x36), sections = r.u8(0x37);
    if (sections != 1) throw FormatError("section count " + std::to_string(sections) + " (only 1 supported)");
    if (c.keyCount == 0 || c.timePerKey == 0) throw FormatError("empty clip");
    // Component masks: three bit streams (rot, scale, trans), 3 bits per segment, each
    // ((segments*3+7)&~7) bits long, after an 8-bit presence byte at 0x44 + sections*4 (tru.exe 0x44aee0).
    const size_t flagsAt = 0x44 + 4 * size_t(sections);
    const uint8_t present = r.u8(flagsAt);
    const size_t streamBits = (size_t(segments) * 3 + 7) & ~size_t(7);
    size_t streamStart[3] = {}, bit = 8;
    for (int k = 0; k < 3; ++k)
        if (present & (1 << k)) { streamStart[k] = bit; bit += streamBits; }
    auto mask3 = [&](int k, size_t seg) {
        if (!(present & (1 << k))) return 0u;
        uint32_t v = 0;
        for (size_t b = 0; b < 3; ++b) {
            const size_t at = streamStart[k] + seg * 3 + b;
            v |= uint32_t((r.u8(flagsAt + at / 8) >> (at % 8)) & 1) << b;
        }
        return v;
    };
    // Channel data: self-relative offset at 0x44 (tru.exe 0x44cac0).
    size_t p = 0x44 + size_t(r.u32(0x44));
    const size_t K = c.keyCount;
    c.tracks.resize(segments);
    for (size_t seg = 0; seg < segments; ++seg) {
        const uint32_t m[3] = {mask3(0, seg), mask3(1, seg), mask3(2, seg)};
        for (int group = 0; group < 3; ++group)
            for (int comp = 0; comp < 3; ++comp) {
                if (!(m[group] & (1u << comp))) continue;
                auto& out = c.tracks[seg][size_t(group * 3 + comp)];
                out.resize(K);
                const int16_t mode = r.s16(p);
                if (mode == 0) { // one f32 per key
                    for (size_t k = 0; k < K; ++k) out[k] = r.f32(p + 4 + 4 * k);
                    p += 4 + 4 * K;
                } else if (mode == 1) { // constant
                    std::fill(out.begin(), out.end(), r.f32(p + 4));
                    p += 8;
                } else if (mode == 2) { // piece-wise linear (tru.exe 0x432a70)
                    const size_t n = size_t(r.s16(p + 2)); // floats: base + n-1 slopes
                    if (n == 0) throw FormatError("empty piece-wise channel");
                    const size_t lens = p + 4, vals = lens + ((n + 2) >> 2) * 4;
                    for (size_t k = 0; k < K; ++k) {
                        float base = r.f32(vals);
                        size_t i = 0, kk = k, len = r.u8(lens);
                        while (len < kk) {
                            if (i + 1 >= n) throw FormatError("piece-wise channel shorter than clip");
                            base += r.f32(vals + 4 + 4 * i);
                            kk -= len;
                            len = r.u8(lens + ++i);
                        }
                        if (len == 0) throw FormatError("zero-length piece-wise segment");
                        out[k] = (i + 1 < n ? float(kk) * r.f32(vals + 4 + 4 * i) / float(len) : 0.0f) + base;
                    }
                    p = vals + 4 * n;
                } else {
                    throw FormatError("channel mode " + std::to_string(mode));
                }
            }
    }
    // Self-relative offset at 0x3C = end of channel data / start of extra channels (exact on all
    // 1744 clips of pc-w\lara.drm). Extra (non-bone) channels are not decoded.
    if (p != 0x3C + size_t(r.u32(0x3C))) throw FormatError("channel data does not end at the 0x3C offset");
    return c;
}

std::vector<AnimEntry> animList(const cdc::Drm& drm) {
    // The Object section (type 11) names its object DTP through the resource relocation at +0.
    const cdc::Section* object = nullptr;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Object && cdc::resourceRefType(s, 0) == int(cdc::SectionType::DtpData) && s.data.size() >= 4) {
            const uint32_t id = Reader{s.data}.u32(0);
            for (const auto& d : drm.sections)
                if (d.type == cdc::SectionType::DtpData && d.id == id) { object = &d; break; }
            break;
        }
    std::vector<AnimEntry> out;
    if (!object) return out;
    const Reader r{object->data};
    const uint32_t n = r.u32(0x54);
    const auto list = cdc::internalPointer(*object, 0x58);
    if (!list || n > 65535) return out;
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = *list + 12 * i;
        AnimEntry a{uint32_t(r.s16(e)), {}};
        if (const auto name = cdc::internalPointer(*object, e + 8)) {
            for (size_t k = *name; k < object->data.size() && object->data[k]; ++k) a.name += char(object->data[k]);
        }
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<LocalTransform> bindPose(const scene::Skeleton& sk) {
    std::vector<LocalTransform> out(sk.bones.size());
    for (size_t i = 0; i < out.size(); ++i) out[i].trans = glm::vec3(sk.bones[i].pivot[0], sk.bones[i].pivot[1], sk.bones[i].pivot[2]);
    return out;
}

std::vector<LocalTransform> sample(const Clip& c, const scene::Skeleton& sk, double seconds, bool loop) {
    std::vector<LocalTransform> out = bindPose(sk);
    const double tpk = c.timePerKey, duration = tpk * c.keyCount;
    double t = seconds * kTimeUnitsPerSecond;
    if (loop) t = std::fmod(std::fmod(t, duration) + duration, duration);
    t = std::clamp(t, 0.0, duration);
    const int keyB = std::min(int(t / tpk), int(c.keyCount) - 1);
    float frac = float(std::min((t - keyB * tpk) / tpk, 1.0));
    int keyA = keyB - 1;
    if (keyA < 0) {
        if (loop) keyA = c.keyCount - 1;
        else { keyA = 0; frac = 1.0f; }
    }
    const size_t n = std::min(c.tracks.size(), out.size());
    for (size_t s = 0; s < n; ++s) {
        glm::vec3 a[3] = {glm::vec3(0.0f), glm::vec3(1.0f), out[s].trans}, b[3] = {a[0], a[1], a[2]};
        bool any = false;
        for (size_t ch = 0; ch < kChannels; ++ch) {
            const auto& v = c.tracks[s][ch];
            if (v.empty()) continue;
            any = true;
            a[ch / 3][int(ch % 3)] = v[size_t(keyA)];
            b[ch / 3][int(ch % 3)] = v[size_t(keyB)];
        }
        if (!any) continue;
        // Shortest-way rotation interpolation as in tru.exe: if the rotation vectors differ by more than
        // half a turn, rewind A by whole turns along its axis.
        constexpr float kTwoPi = 6.2831855f;
        const glm::vec3 d = (b[0] - a[0]) / kTwoPi;
        const float dd = glm::dot(d, d);
        if (dd > 0.25f) {
            const float la = glm::length(a[0]);
            if (la > 0.0f) a[0] -= a[0] / la * (kTwoPi * std::floor(std::sqrt(dd) + 0.5f));
        }
        out[s].rot = glm::mix(a[0], b[0], frac);
        out[s].scale = glm::mix(a[1], b[1], frac);
        out[s].trans = glm::mix(a[2], b[2], frac);
    }
    return out;
}

glm::mat3 rotationFromVector(glm::vec3 v) {
    const float angle = glm::length(v);
    if (angle < 1e-6f) return glm::mat3(1.0f);
    const glm::vec3 k = v / angle;
    const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
    // Column-major: columns are the images of the basis vectors (Rodrigues).
    return glm::mat3(t * k.x * k.x + c, t * k.x * k.y + s * k.z, t * k.x * k.z - s * k.y,
                     t * k.x * k.y - s * k.z, t * k.y * k.y + c, t * k.y * k.z + s * k.x,
                     t * k.x * k.z + s * k.y, t * k.y * k.z - s * k.x, t * k.z * k.z + c);
}

std::vector<glm::mat4> worldMatrices(const scene::Skeleton& sk, std::span<const LocalTransform> local) {
    std::vector<glm::mat4> w(sk.bones.size(), glm::mat4(1.0f));
    for (size_t i = 0; i < w.size() && i < local.size(); ++i) {
        glm::mat4 l(rotationFromVector(local[i].rot));
        l[0] *= local[i].scale.x;
        l[1] *= local[i].scale.y;
        l[2] *= local[i].scale.z;
        l[3] = glm::vec4(local[i].trans, 1.0f);
        const int32_t p = sk.bones[i].parent;
        w[i] = p >= 0 ? w[size_t(p)] * l : l;
    }
    return w;
}

std::vector<glm::vec3> bindPositions(const scene::Skeleton& sk) {
    std::vector<glm::vec3> out(sk.bones.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const auto& b = sk.bones[i];
        out[i] = glm::vec3(b.pivot[0], b.pivot[1], b.pivot[2]) + (b.parent >= 0 ? out[size_t(b.parent)] : glm::vec3(0.0f));
    }
    return out;
}

std::vector<glm::mat4> skinPalette(const scene::Skeleton& sk, std::span<const glm::mat4> world) {
    const auto bind = bindPositions(sk);
    std::vector<glm::mat4> out(world.size());
    for (size_t i = 0; i < out.size() && i < bind.size(); ++i) out[i] = glm::translate(world[i], -bind[i]);
    return out;
}

void skinVertices(const scene::MeshData& mesh, std::span<const glm::mat4> palette, std::vector<scene::Vertex>& out) {
    out = mesh.vertices;
    if (mesh.skin.size() != mesh.vertices.size()) return;
    for (size_t v = 0; v < out.size(); ++v) {
        const auto& in = mesh.vertices[v];
        const auto& s = mesh.skin[v];
        glm::mat4 m(0.0f);
        float total = 0.0f;
        for (int k = 0; k < 4; ++k)
            if (s.weight[k] && s.bone[k] < palette.size()) {
                m += palette[s.bone[k]] * (s.weight[k] / 255.0f);
                total += s.weight[k] / 255.0f;
            }
        if (total <= 0.0f) continue;
        m /= total;
        const glm::vec3 p(m * glm::vec4(in.position[0], in.position[1], in.position[2], 1.0f));
        glm::vec3 n = glm::mat3(m) * glm::vec3(in.normal[0], in.normal[1], in.normal[2]);
        const float len = glm::length(n);
        if (len > 0.0f) n /= len;
        auto& o = out[v];
        o.position[0] = p.x, o.position[1] = p.y, o.position[2] = p.z;
        o.normal[0] = n.x, o.normal[1] = n.y, o.normal[2] = n.z;
    }
}

} // namespace ou::anim
