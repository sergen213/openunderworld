// Object physics data (HInfo primitives, model markers, physical class) and the rigid-body scene on Jolt Physics.
// Evidence and the Havok -> Jolt mapping: docs/formats/PHYSICS.md.
#include "physics/Physics.hpp"

#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/Collision/GroupFilterTable.h>
#include <Jolt/Physics/Constraints/SixDOFConstraint.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/SliderConstraint.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/Physics/Vehicle/VehicleCollisionTester.h>
#include <Jolt/Physics/Vehicle/VehicleConstraint.h>
#include <Jolt/Physics/Vehicle/WheeledVehicleController.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

namespace ou::physics {
namespace {

using cdc::FormatError;

template <class T> T at(const cdc::Section& s, size_t off) {
    if (off > s.data.size() || s.data.size() - off < sizeof(T)) throw FormatError("physics: read past section end");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}
vec3 vec(const cdc::Section& s, size_t off) { return {at<float>(s, off), at<float>(s, off + 4), at<float>(s, off + 8)}; }
quat quatXyzw(const cdc::Section& s, size_t off) {
    return glm::normalize(quat(at<float>(s, off + 12), at<float>(s, off), at<float>(s, off + 4), at<float>(s, off + 8)));
}
mat3 diag(float a, float b, float c) { return mat3(a, 0, 0, 0, b, 0, 0, 0, c); }

// HInfo (segment +0x3C): {u32 count, ptr list} pairs; slot 0 spheres, 1 boxes, 2 markers, 3 capsules.
constexpr uint32_t kSphereStride = 0x24, kBoxStride = 0x50, kCapsuleStride = 0x40;

void readHInfo(const cdc::Section& m, uint32_t h, int segment, std::vector<Prim>& out, std::vector<HMarker>& markers) {
    auto list = [&](int slot, uint32_t stride, auto&& one) {
        const uint32_t n = at<uint32_t>(m, h + 8 * slot);
        const auto p = cdc::internalPointer(m, h + 8 * slot + 4);
        if (!n || !p) return;
        if (n > 4096) throw FormatError("physics: HInfo count " + std::to_string(n));
        for (uint32_t i = 0; i < n; ++i) {
            Prim pr;
            pr.segment = segment;
            one(*p + stride * i, pr);
            out.push_back(pr);
        }
    };
    auto head = [&](uint32_t off, Prim& p) { for (int k = 0; k < 4; ++k) p.head[k] = at<uint8_t>(m, off + k); };
    list(0, kSphereStride, [&](uint32_t o, Prim& p) { // tru.exe mass 0x48E330 type 1
        p.kind = PrimKind::Sphere;
        head(o, p);
        p.id = at<uint16_t>(m, o + 4), p.radius = at<uint16_t>(m, o + 6);
        p.pos = {at<int16_t>(m, o + 8), at<int16_t>(m, o + 10), at<int16_t>(m, o + 12)};
        p.mass = at<uint16_t>(m, o + 0x14), p.damage = at<int16_t>(m, o + 0x1C);
    });
    list(1, kBoxStride, [&](uint32_t o, Prim& p) { // type 5
        p.kind = PrimKind::Box;
        p.size = vec(m, o), p.pos = vec(m, o + 0x10), p.rot = quatXyzw(m, o + 0x20);
        head(o + 0x30, p);
        p.id = at<uint16_t>(m, o + 0x34), p.mass = at<uint16_t>(m, o + 0x36), p.damage = at<int16_t>(m, o + 0x3E);
    });
    list(3, kCapsuleStride, [&](uint32_t o, Prim& p) { // type 10
        p.kind = PrimKind::Capsule;
        p.pos = vec(m, o), p.rot = quatXyzw(m, o + 0x10);
        head(o + 0x20, p);
        p.id = at<uint16_t>(m, o + 0x24), p.radius = at<uint16_t>(m, o + 0x26), p.length = at<uint16_t>(m, o + 0x28);
        p.mass = at<uint16_t>(m, o + 0x2A), p.damage = at<int16_t>(m, o + 0x32);
    });
    // Slot 2 markers (tru.exe 0x46DA20, HInfo primitive type 8): +4 u16 id, +8 float3 position, +0x14 float3 Euler.
    const uint32_t nm = at<uint32_t>(m, h + 16);
    if (const auto p = cdc::internalPointer(m, h + 20); nm && p && nm <= 4096)
        for (uint32_t i = 0; i < nm; ++i) {
            const uint32_t o = *p + 0x20 * i;
            markers.push_back({segment, at<uint16_t>(m, o + 4), vec(m, o + 8), vec(m, o + 0x14)});
        }
}

// Engagement set {id, -> 0x8C-byte records, count} at `set` in the object section; nullopt past its end.
std::optional<EngagementSet> engagementSetAt(const cdc::Section& ob, uint32_t set) {
    if (size_t(set) + 12 > ob.data.size()) return std::nullopt;
    const auto u32 = [&](uint32_t off) { return off + 4 <= ob.data.size() ? at<uint32_t>(ob, off) : 0u; };
    const auto f32 = [&](uint32_t off) { return off + 4 <= ob.data.size() ? at<float>(ob, off) : 0.0f; };
    constexpr float kCm = 3.1605f; // tru.exe's engagement units per centimetre
    EngagementSet out{int32_t(u32(set)), {}};
    const auto recs = cdc::internalPointer(ob, set + 4);
    for (uint32_t r = 0; recs && r < u32(set + 8) && r < 32; ++r) {
        const uint32_t q = *recs + 0x8C * r;
        if (q + 0x8C > ob.data.size()) break;
        Engagement g;
        g.kind = int32_t(u32(q)), g.segment = int32_t(u32(q + 4)), g.shape = int32_t(u32(q + 0x10));
        g.centre = vec3(f32(q + 0x14), f32(q + 0x18), f32(q + 0x1C)) * kCm;
        g.radius = f32(q + 0x20) * kCm, g.height = f32(q + 0x24) * kCm;
        g.heading = glm::radians(f32(q + 0x28)), g.half = glm::radians(f32(q + 0x2C));
        if (const auto ids = cdc::internalPointer(ob, q + 0x6C))
            for (uint32_t k = 0; k < u32(q + 0x70) && k < 16 && *ids + 2 * k + 2 <= ob.data.size(); ++k)
                g.needs.push_back(at<uint16_t>(ob, *ids + 2 * k));
        out.records.push_back(std::move(g));
    }
    return out;
}

} // namespace

std::optional<mat4> ObjectPhysics::frame(int field) const {
    const auto seg = [&](int s) { return s >= 0 && size_t(s) < segments.size() ? std::optional(segments[size_t(s)]) : std::nullopt; };
    if (field >= 0) return seg(field);
    for (const HMarker& m : hmarkers)
        if (m.id == -field)
            if (const auto s = seg(m.segment)) {
                const quat q = glm::angleAxis(m.rot.x, vec3(1, 0, 0)) * glm::angleAxis(m.rot.y, vec3(0, 1, 0)) * glm::angleAxis(m.rot.z, vec3(0, 0, 1));
                mat4 r = glm::mat4_cast(q);
                r[3] = glm::vec4(m.pos, 1);
                return *s * r;
            }
    return std::nullopt;
}

std::optional<EngagementSet> loadEngagementSet(const cdc::Drm& d, uint32_t offset) {
    const auto base = formats::objectBaseSection(d);
    if (!base) return std::nullopt;
    return engagementSetAt(d.sections[*base], offset);
}

ObjectPhysics loadObjectPhysics(const cdc::Drm& d, uint32_t model) {
    ObjectPhysics o;
    const auto base = formats::objectBaseSection(d);
    if (!base) return o;
    const auto& ob = d.sections[*base];
    o.oflags = at<uint32_t>(ob, 0);
    if (const auto p = cdc::internalPointer(ob, 0xC8); p && *p + 6 <= ob.data.size() && at<uint16_t>(ob, *p + 4) == 0xB00B) {
        o.physClass = at<uint16_t>(ob, *p + 2);
        // Rigid body (34): +0x2C -> {+4 lo, +8 hi} [O values: ms_rb_tomb_lock -1250..0 (its script's disengaged
        // position), ca_rb_crusher_slim 0..900 (motor 15 per frame for its 2 s clock), ms_rb_press_plate -50..0; I: that
        // they are the joint limit].
        if (o.physClass == kClassRigidBody && *p + 0x30 <= ob.data.size())
            if (const auto r = cdc::internalPointer(ob, *p + 0x2C); r && *r + 0xC <= ob.data.size()) {
                o.jointLimit = ObjectPhysics::Limit{at<float>(ob, *r + 4), at<float>(ob, *r + 8)};
                // +0x34 flags (bits 0-2 the axes' limits on), +0x61 sound position, +0x64 sound (0x6B6440).
                if (*r + 0x68 <= ob.data.size()) {
                    o.jointLimitOn = at<uint32_t>(ob, *r + 0x34) & 1;
                    o.jointMotor = {at<float>(ob, *r + 0x1C), at<float>(ob, *r + 0x20), (at<uint32_t>(ob, *r + 0x34) & 8) != 0};
                    o.jointSoundAt = at<uint8_t>(ob, *r + 0x61);
                    const auto ps = cdc::sectionPointers(d, uint32_t(*base));
                    if (const auto s = ps.find(*r + 0x64); s != ps.end()) o.jointSound = s->second;
                }
            }
        // Engagement sets (tru.exe 0x6C2530 / 0x6B5890 messages 0x23 / 0x1B1 pick one): {id, -> 0x8C-byte records,
        // count}, 12 bytes each, at +0x68 / +0x6C on anim-graph objects (class 6) and +0xA8 / +0xAC on portables (50).
        const uint32_t lo = o.physClass == 50 ? 0xA8 : 0x68;
        if (const auto list = cdc::internalPointer(ob, *p + lo); list && *p + lo + 8 <= ob.data.size() && at<uint32_t>(ob, *p + lo + 4) <= 32)
            for (uint32_t i = 0; i < at<uint32_t>(ob, *p + lo + 4); ++i)
                if (auto set = engagementSetAt(ob, *list + 12 * i)) o.engagements.push_back(std::move(*set));
        if (o.physClass == 6)
            if (const auto a = cdc::internalPointer(ob, *p + 0x3C); a && *a < ob.data.size()) o.attachMarker = ob.data[*a];
    }
    // Models: ObjectBase +0xA0 u16 count, +0xA4 -> resource refs of the model DTPs (formats::objectRenderModels).
    const auto ptrs = cdc::sectionPointers(d, *base);
    const auto lp = ptrs.find(0xA4);
    if (at<uint16_t>(ob, 0xA0) <= model || lp == ptrs.end()) return o;
    const auto& list = d.sections[lp->second.section];
    if (cdc::resourceRefType(list, lp->second.offset + 4 * model) != int(cdc::SectionType::DtpData)) return o;
    const uint32_t modelId = at<uint32_t>(list, lp->second.offset + 4 * model);
    const auto m = std::find_if(d.sections.begin(), d.sections.end(), [&](const cdc::Section& s) {
        return s.type == cdc::SectionType::DtpData && s.id == modelId;
    });
    if (m == d.sections.end()) throw FormatError("physics: model DTP " + std::to_string(modelId) + " missing");
    // dtp Model: +4 segment count, +0xC segments (0x40: pivot +0x20, parent +0x38, HInfo +0x3C), +0x38/+0x3C markers.
    const uint32_t nseg = at<uint32_t>(*m, 4);
    const auto segs = cdc::internalPointer(*m, 0xC);
    if (nseg > 4096 || (nseg && !segs)) throw FormatError("physics: bad segment list");
    for (uint32_t k = 0; k < nseg; ++k) {
        const uint32_t s = *segs + 0x40 * k;
        const int32_t parent = at<int32_t>(*m, s + 0x38);
        if (parent >= int32_t(k)) throw FormatError("physics: segment parent out of order");
        o.segments.push_back((parent >= 0 ? o.segments[parent] : mat4(1)) * glm::translate(mat4(1), vec(*m, s + 0x20)));
        if (const auto h = cdc::internalPointer(*m, s + 0x3C)) readHInfo(*m, *h, int(k), o.prims, o.hmarkers);
    }
    if (nseg) o.boxMin = vec(*m, *segs), o.boxMax = vec(*m, *segs + 0x10);
    const uint32_t nm = at<uint32_t>(*m, 0x38);
    if (const auto mk = cdc::internalPointer(*m, 0x3C); mk && nm < 4096)
        for (uint32_t i = 0; i < nm; ++i) {
            const uint32_t q = *mk + 0x30 * i;
            o.markers.push_back({vec(*m, q), vec(*m, q + 0x10), at<uint32_t>(*m, q + 0x20), at<int32_t>(*m, q + 0x24)});
        }
    const uint32_t nmu = m->data.size() >= 0x5C ? at<uint32_t>(*m, 0x54) : 0;
    if (const auto mu = nmu ? cdc::internalPointer(*m, 0x58) : std::nullopt; mu && nmu < 256)
        for (uint32_t i = 0; i < nmu; ++i) {
            const uint32_t q = *mu + 0x5C * i;
            const uint8_t type = at<uint8_t>(*m, q + 0x33);
            const uint32_t flags = at<uint32_t>(*m, q + 0x28);
            const auto pl = cdc::internalPointer(*m, q + 0x58);
            if (!type || !pl || (flags ? flags : collision::markupFlags(type)) & 0x80000000u) continue; // disabled [O 0x5CE4F0]
            const uint32_t n = at<uint32_t>(*m, *pl);
            if (n < 2 || n > 1024) continue;
            collision::Markup k;
            k.type = type, k.id = at<uint16_t>(*m, q + 0x2E);
            for (uint32_t j = 0; j < n; ++j) k.points.push_back(vec(*m, *pl + 0x10 + 0x10 * j));
            o.markup.push_back(std::move(k));
            o.markupSegment.push_back(at<int16_t>(*m, q + 0x30));
        }
    const uint32_t nft = m->data.size() >= 0x64 ? at<uint32_t>(*m, 0x5C) : 0;
    if (const auto ft = nft ? cdc::internalPointer(*m, 0x60) : std::nullopt; ft && nft < 256 && *ft + 0x20 * size_t(nft) <= m->data.size())
        for (uint32_t i = 0; i < nft; ++i) {
            const uint32_t q = *ft + 0x20 * i;
            o.features.push_back({int(at<uint16_t>(*m, q)), at<uint16_t>(*m, q + 2), vec(*m, q + 4)});
        }
    return o;
}

// ObjectBase +0x78 != 0: +0x7C -> {+4 mass, +8 radius scale, +0x2C bones, +0x30 count (at most 24, 0x4F1920 /
// 0x4F0B40)}, 0x78-byte bones read by 0x4F1390 (layout: RagdollBone).
std::optional<RagdollDef> loadRagdoll(const cdc::Drm& d) {
    const auto base = formats::objectBaseSection(d);
    if (!base || !at<uint32_t>(d.sections[*base], 0x78)) return std::nullopt;
    const auto ptrs = cdc::sectionPointers(d, *base);
    const auto rec = ptrs.find(0x7C);
    if (rec == ptrs.end()) return std::nullopt;
    const auto& s = d.sections[rec->second.section];
    const uint32_t r = rec->second.offset, n = at<uint32_t>(s, r + 0x30);
    if (n > 24) throw FormatError("physics: ragdoll of " + std::to_string(n) + " bones");
    const auto inner = cdc::sectionPointers(d, rec->second.section);
    const auto list = inner.find(r + 0x2C);
    if (!n || list == inner.end()) return std::nullopt;
    const auto& ls = d.sections[list->second.section];
    RagdollDef def{at<float>(s, r + 4), at<float>(s, r + 8), {}};
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t b = list->second.offset + 0x78 * i;
        RagdollBone o;
        const int16_t start = at<int16_t>(ls, b + 4), alt = at<int16_t>(ls, b + 6), end = at<int16_t>(ls, b + 0x14);
        o.segment = alt >= 0 ? alt : start;
        o.endSegment = end >= 0 ? end : start;
        o.startOffset = vec(ls, b + 8), o.endOffset = vec(ls, b + 0x18);
        o.parent = at<int32_t>(ls, b + 0x24);
        if (o.parent >= int(i)) throw FormatError("physics: ragdoll bone parent out of order");
        o.massShare = at<float>(ls, b + 0x28), o.radius = at<float>(ls, b + 0x2C) * def.radiusScale;
        o.joint = at<int32_t>(ls, b + 0x38);
        o.pivot = vec(ls, b + 0x3C), o.axis1 = vec(ls, b + 0x48), o.axis2 = vec(ls, b + 0x54);
        for (int k = 0; k < 5; ++k) o.limits[k] = at<float>(ls, b + 0x60 + 4 * k);
        o.friction = std::max(at<float>(ls, b + 0x74), 0.01f);
        def.bones.push_back(o);
    }
    return def;
}

std::shared_ptr<const collision::CollisionMesh> solidShape(const ObjectPhysics& o, int segment) {
    auto shape = std::make_shared<collision::CollisionMesh>();
    const uint8_t mask = kObjectMask | (o.oflags & 2 ? kCameraObjectMask : 0) | (o.oflags & 8 ? kSightObjectMask : 0);
    for (const auto& p : o.prims) {
        if (p.segment != segment || !p.solid()) continue;
        // ponytail: spheres/capsules as their bounding boxes; exact shapes when a curved prim must block precisely.
        const vec3 size = p.kind == PrimKind::Box      ? p.size
                          : p.kind == PrimKind::Sphere ? vec3(2 * p.radius)
                                                       : vec3(2 * p.radius, 2 * p.radius, p.length + 2 * p.radius);
        collision::addBox(*shape, glm::translate(mat4(1), p.pos) * glm::mat4_cast(p.rot), size, mask);
    }
    if (shape->faces.empty()) return nullptr;
    shape->build();
    return shape;
}

MassProps massProps(const ObjectPhysics& o) {
    struct Part { float m; vec3 c; mat3 i; };
    std::vector<Part> parts;
    for (const auto& p : o.prims) {
        if (!p.mass) continue;
        const float m = p.mass;
        mat3 i;
        if (p.kind == PrimKind::Box) { // 0x4E9100: each side at least 0.2 x the longest
            const float lo = 0.2f * std::max({p.size.x, p.size.y, p.size.z});
            const vec3 s = glm::max(p.size, vec3(lo));
            i = diag(m * (s.y * s.y + s.z * s.z) / 12, m * (s.x * s.x + s.z * s.z) / 12, m * (s.x * s.x + s.y * s.y) / 12);
        } else if (p.kind == PrimKind::Sphere) { // 0x4DC770
            i = diag(0.4f * m * p.radius * p.radius, 0.4f * m * p.radius * p.radius, 0.4f * m * p.radius * p.radius);
        } else { // 0x4E9230: cylinder + two hemispheres, radius at least 0.2 x length
            const float h = p.length, r = std::max(p.radius, 0.2f * h);
            const float cyl = r * 3.1415927f * r * h, hemi = r * 0.6666667f * r * r, vol = cyl + 2 * hemi;
            const float mc = m * cyl / vol, mh = m * hemi / vol, d = h * 0.5f + r * 0.375f;
            const float ixy = mh * 0.259375f * r * r * 2 + h * mc * h / 12 + mc * 0.25f * r * r + d * d * mh * 2;
            const float iz = 2 * (r * r * mh * 0.4f) + mc * 0.5f * r * r;
            i = diag(ixy, ixy, iz);
        }
        const mat4 seg = p.segment < int(o.segments.size()) ? o.segments[p.segment] : mat4(1);
        const mat3 r = mat3(seg) * glm::mat3_cast(p.rot);
        parts.push_back({m, vec3(seg * glm::vec4(p.pos, 1)), r * i * glm::transpose(r)});
    }
    MassProps out;
    for (const auto& p : parts) out.mass += p.m, out.com += p.m * p.c;
    if (out.mass <= 0) return out;
    out.com /= out.mass;
    out.inertia = mat3(0);
    for (const auto& p : parts) { // parallel axis theorem
        const vec3 d = p.c - out.com;
        out.inertia += p.i + p.m * (glm::dot(d, d) * mat3(1) - glm::outerProduct(d, d));
    }
    return out;
}

mat4 Body::model() const { return glm::translate(mat4(1), pos) * glm::mat4_cast(rot) * glm::translate(mat4(1), -com); }

// ---- Jolt scene -----------------------------------------------------------------------------------------------------
namespace {

constexpr float kM = 1.0f / kUnitsPerMetre;
constexpr float kFrame = 30.0f;  // tru.exe joint units are per 30 Hz frame
constexpr float kSolver = 60.0f; // [I] motor impulse bounds are per 1/60 s solver step (PHYSICS.md "Uber joints")
enum : JPH::ObjectLayer { kLayerJoint, kLayerSolid, kLayerLoose, kLayerVehicle, kLayerPlate, kLayerLara, kLayers };

JPH::Vec3 J(vec3 v) { return {v.x, v.y, v.z}; }
JPH::Vec3 Jm(vec3 p) { return J(p * kM); } // game units -> metres
JPH::Quat J(quat q) { return {q.x, q.y, q.z, q.w}; }
vec3 G(JPH::Vec3 v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
quat G(JPH::Quat q) { return {q.GetW(), q.GetX(), q.GetY(), q.GetZ()}; }
vec3 perpendicular(vec3 a) { return glm::normalize(glm::cross(a, std::abs(a.x) < 0.9f ? vec3(1, 0, 0) : vec3(0, 1, 0))); }
float wrap(float a) { return a - 6.2831853f * std::floor((a + 3.14159265f) / 6.2831853f); } // [-pi, pi)

void initJolt() {
    static const bool once = [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
        return true;
    }();
    (void)once;
}

// A world-fixed one-degree-of-freedom Jolt constraint (hinge or slider) of one body. Jolt wants its limits around 0
// (hinge limits within ±π), so our coordinate q (rad / m) = Jolt's value + ref; Impl::limits moves ref (rebuilding the
// constraint) when the data's limits leave it, as a locked joint (lo == hi != 0) does.
struct Axis {
    bool hinge = false;
    vec3 pivot{0}, axis{0, 0, 1}; // world, game units
    JPH::BodyID body;
    JPH::Ref<JPH::TwoBodyConstraint> c;
    float ref = 0;
    JPH::RVec3 pos0;   // the body's pose at coordinate 0 (constraints are built from poses derived from it)
    JPH::Quat rot0;
    JPH::HingeConstraint* h() const { return static_cast<JPH::HingeConstraint*>(c.GetPtr()); }
    JPH::SliderConstraint* s() const { return static_cast<JPH::SliderConstraint*>(c.GetPtr()); }
    float q() const { return ref + (hinge ? h()->GetCurrentAngle() : s()->GetCurrentPosition()); }
};

// follower = ratio * driver for any pair of world-fixed 1-DOF joints (tru.exe link joints, 0x4E17B0): Jolt's gear and
// rack-and-pinion parts cover hinge-hinge and hinge-slider but not slider-slider (the l4 grapple post's 2 -> 2 link).
// Both bodies are dynamic and their axes fixed in the world (each sits on a world-fixed hinge or slider).
class LinkConstraint final : public JPH::TwoBodyConstraint {
public:
    struct Side { const Axis* joint; bool hinge; JPH::Vec3 axis; };
    struct Settings final : JPH::TwoBodyConstraintSettings {
        JPH::TwoBodyConstraint* Create(JPH::Body&, JPH::Body&) const override { return nullptr; }
    };
    LinkConstraint(JPH::Body& a, JPH::Body& b, Side da, Side db, float ratio)
        : TwoBodyConstraint(a, b, Settings{}), a_(da), b_(db), r_(ratio) {}
    JPH::EConstraintSubType GetSubType() const override { return JPH::EConstraintSubType::User1; }
    void NotifyShapeChanged(const JPH::BodyID&, JPH::Vec3Arg) override {}
    void SetupVelocityConstraint(float) override {
        ka_ = response(*mBody1, a_), kb_ = response(*mBody2, b_);
        const float k = r_ * r_ * dot(a_, ka_) + dot(b_, kb_);
        mass_ = k > 0 ? 1 / k : 0;
    }
    void ResetWarmStart() override { total_ = 0; }
    void WarmStartVelocityConstraint(float ratio) override { total_ *= ratio, apply(total_, false); }
    bool SolveVelocityConstraint(float) override {
        if (mass_ == 0) return false;
        const float lambda = -mass_ * (r_ * speed(*mBody1, a_) - speed(*mBody2, b_));
        total_ += lambda;
        return apply(lambda, false);
    }
    bool SolvePositionConstraint(float, float baumgarte) override {
        float c = r_ * position(a_) - position(b_);
        if (b_.hinge) c = wrap(c);
        if (c == 0 || mass_ == 0) return false;
        return apply(-mass_ * baumgarte * c, true);
    }
    JPH::Ref<JPH::ConstraintSettings> GetConstraintSettings() const override { return new Settings; }
    JPH::Mat44 GetConstraintToBody1Matrix() const override { return JPH::Mat44::sIdentity(); }
    JPH::Mat44 GetConstraintToBody2Matrix() const override { return JPH::Mat44::sIdentity(); }
    void SaveState(JPH::StateRecorder& s) const override { TwoBodyConstraint::SaveState(s), s.Write(total_); }
    void RestoreState(JPH::StateRecorder& s) override { TwoBodyConstraint::RestoreState(s), s.Read(total_); }
#ifdef JPH_DEBUG_RENDERER
    void DrawConstraint(JPH::DebugRenderer*) const override {}
#endif

private:
    static JPH::Vec3 response(const JPH::Body& b, const Side& s) { // M^-1 J for the side's row
        return s.hinge ? b.GetMotionProperties()->MultiplyWorldSpaceInverseInertiaByVector(b.GetRotation(), s.axis)
                       : b.GetMotionProperties()->GetInverseMass() * s.axis;
    }
    static float dot(const Side& s, JPH::Vec3 k) { return s.axis.Dot(k); }
    static float speed(const JPH::Body& b, const Side& s) { return s.axis.Dot(s.hinge ? b.GetAngularVelocity() : b.GetLinearVelocity()); }
    static float position(const Side& s) { return s.joint->q(); }
    bool apply(float lambda, bool position) {
        if (lambda == 0) return false;
        const auto one = [&](JPH::Body& b, const Side& s, JPH::Vec3 k, float l) {
            if (position) s.hinge ? b.AddRotationStep(l * k) : b.AddPositionStep(l * k);
            else if (s.hinge) b.GetMotionProperties()->AddAngularVelocityStep(l * k);
            else b.GetMotionProperties()->AddLinearVelocityStep(l * k);
        };
        one(*mBody1, a_, ka_, r_ * lambda), one(*mBody2, b_, kb_, -lambda);
        return true;
    }
    Side a_, b_;
    float r_, mass_ = 0, total_ = 0;
    JPH::Vec3 ka_ = JPH::Vec3::sZero(), kb_ = JPH::Vec3::sZero();
};

JPH::RefConst<JPH::Shape> made(const JPH::ShapeSettings& s) {
    auto r = s.Create();
    return r.IsValid() ? r.Get() : nullptr;
}

// Triangles of `mesh` (each vertex through `to`, then in metres) as a Jolt mesh shape; null without triangles.
JPH::RefConst<JPH::Shape> meshShape(const collision::CollisionMesh& mesh, const mat4& to) {
    JPH::VertexList v;
    JPH::IndexedTriangleList t;
    v.reserve(mesh.vertices.size()), t.reserve(mesh.faces.size());
    for (const vec3& p : mesh.vertices) {
        const vec3 q = vec3(to * glm::vec4(p, 1)) * kM;
        v.push_back({q.x, q.y, q.z});
    }
    for (const auto& f : mesh.faces) t.push_back({f.v[0], f.v[1], f.v[2]});
    return t.empty() ? nullptr : made(JPH::MeshShapeSettings(std::move(v), std::move(t)));
}

// A loose body's collision: every primitive of its model, as tru.exe builds the rigid body from all of them (0x497270;
// the flag bit we call solid gates Lara's collision only [I]), in its centre-of-mass frame; no primitives: a 20-unit
// ball resting on the model origin.
JPH::RefConst<JPH::Shape> looseShape(const ObjectPhysics& o, vec3 com) {
    // [O] tru.exe 0x48E710 makes a Havok shape of a primitive only with flag bit 0x100 (Prim::solid), and of a sphere
    // only when its third flag byte is not 2 (the motorcycle's wheel spheres). [I] Flag byte 0 bit 0x40 marks shapes that
    // do not rest on the world: ms_pt_weight_c's 0x40 sphere (r 104 round its 126-unit cube, flag 0x10) reaches 40 units
    // under the base its intros stand on [O data], so the weight lies on its cube and does not roll; the same byte is on
    // the poles' end sphere and the snake head's handle capsules. A body left without any shape keeps them all.
    const auto lands = [](const Prim& p) { return p.solid() && !(p.head[0] & 0x40) && !(p.kind == PrimKind::Sphere && p.head[2] == 2); };
    const bool any = std::ranges::any_of(o.prims, lands);
    JPH::StaticCompoundShapeSettings c;
    for (const Prim& p : o.prims) {
        if (any && !lands(p)) continue;
        const mat4 seg = p.segment < int(o.segments.size()) ? o.segments[size_t(p.segment)] : mat4(1);
        const mat4 f = seg * glm::translate(mat4(1), p.pos) * glm::mat4_cast(p.rot);
        quat r = glm::normalize(glm::quat_cast(mat3(f)));
        JPH::Ref<JPH::ShapeSettings> s;
        if (p.kind == PrimKind::Box) {
            const vec3 h = glm::max(p.size * (0.5f * kM), vec3(0.002f));
            s = new JPH::BoxShapeSettings(J(h), std::min(JPH::cDefaultConvexRadius, 0.5f * std::min({h.x, h.y, h.z})));
        } else if (p.kind == PrimKind::Capsule && p.length > 0) {
            s = new JPH::CapsuleShapeSettings(0.5f * p.length * kM, std::max(p.radius * kM, 0.002f));
            r = r * glm::angleAxis(1.5707964f, vec3(1, 0, 0)); // Jolt capsules run along Y, ours along Z [I]
        } else {
            s = new JPH::SphereShapeSettings(std::max(p.radius * kM, 0.002f));
        }
        c.AddShape(Jm(vec3(f[3]) - com), J(r), s);
    }
    if (c.mSubShapes.empty()) c.AddShape(Jm(vec3(0, 0, 20) - com), JPH::Quat::sIdentity(), new JPH::SphereShapeSettings(20 * kM));
    const auto shape = made(c);
    if (!shape) return new JPH::SphereShape(20 * kM);
    return made(JPH::OffsetCenterOfMassShapeSettings(-shape->GetCenterOfMass(), shape)); // the data's centre of mass
}

} // namespace

struct Scene::Impl {
    struct Init { Init() { initJolt(); } } init; // before any Jolt member allocates
    JPH::BroadPhaseLayerInterfaceTable layers{kLayers, 2};
    JPH::ObjectLayerPairFilterTable pairs{kLayers};
    std::optional<JPH::ObjectVsBroadPhaseLayerFilterTable> broad;
    JPH::TempAllocatorMalloc temp;
    JPH::JobSystemSingleThreaded jobs{JPH::cMaxPhysicsJobs};
    JPH::PhysicsSystem sys;
    JPH::BodyInterface* bi = nullptr;

    std::vector<JPH::BodyID> hingeBodies;             // Scene::bodies
    std::vector<std::unique_ptr<Axis>> hingeJoints;   // Scene::hinges

    struct JointE {
        uint64_t key;
        Joint1 j; // frame and body as created
        JPH::BodyID body;
        std::unique_ptr<Axis> ax;
        float x = 0, v = 0; // as last written back
        Joint1* live = nullptr;
        bool solid = false; // carries a rail-riding loose body's shape (Scene::Loose::rail)
        bool plate = false; // carries a pressure plate's collision (Scene::setPlates)
    };
    std::vector<JointE> joints;
    struct LinkE { JointLink l; JPH::Ref<LinkConstraint> c; };
    std::vector<LinkE> links;

    struct LooseE { int32_t id; JPH::BodyID body; vec3 com; };
    std::vector<LooseE> loose;
    struct VehicleE { int32_t id; const VehicleSpec* spec; JPH::BodyID body; vec3 com; JPH::Ref<JPH::VehicleConstraint> c; VehicleDrive drive; vec3 shift, kick; };
    std::vector<VehicleE> vehicles;
    struct TerrainE { Terrain t; JPH::RefConst<JPH::Shape> shape; JPH::BodyID body; bool built = false; };
    std::vector<TerrainE> terrain;
    struct ProxyE { std::shared_ptr<const collision::CollisionMesh> shape; mat4 transform; JPH::BodyID body; };
    std::vector<ProxyE> proxies;
    std::vector<Weight> weights;          // Scene::setPlates, for the next step
    std::map<uint64_t, std::vector<collision::Dynamic>> plateMeshes; // Scene::setPlates: plate joint key -> collision
    std::optional<Character> character;   // Scene::setCharacter
    std::vector<Water> water;             // Scene::setWater
    void buoyancy(float dt);
    JPH::BodyID lara;                     // its proxy body (key "a")

    // Body IDs are ours, not Jolt's allocation order: the lowest free index (sequence 0), or the ID a restored state
    // gives the body's key (Scene::restore), so a loaded scene has the bodies, IDs and constraint order it was saved
    // with and Jolt's recorded state (velocities, contact cache, warm starts) applies bit for bit.
    std::vector<bool> used;
    std::map<std::string, JPH::BodyID> pendingIds; // restored key -> ID, until the body exists
    std::set<uint32_t> reserved;                   // their indices
    std::map<std::string, float> pendingRefs;      // restored constraint zeros (Axis::ref) by key
    std::unique_ptr<JPH::StateRecorderImpl> pending; // the recorded Jolt state, applied in the next step
    uint32_t pendingConstraints = 0;

    // Ragdolls (Scene::setRagdolls), by id: a body per bone (keys "r<id>.<bone>"), a joint per bone with a parent.
    struct RagdollE { int32_t id; std::vector<JPH::BodyID> bodies; std::vector<JPH::Ref<JPH::Constraint>> joints; };
    std::vector<RagdollE> ragdolls;
    void dropRagdoll(RagdollE& r) {
        for (auto& c : r.joints) sys.RemoveConstraint(c);
        for (auto& b : r.bodies) remove(b);
        r.joints.clear(), r.bodies.clear();
    }

    JPH::BodyID add(const JPH::BodyCreationSettings& s, const std::string& key, JPH::EActivation act) {
        JPH::BodyID id;
        if (const auto it = pendingIds.find(key); it != pendingIds.end()) {
            id = it->second, reserved.erase(id.GetIndex()), pendingIds.erase(it);
        } else {
            uint32_t i = 0;
            while (i < used.size() && (used[i] || reserved.contains(i))) ++i;
            id = JPH::BodyID(i, 0);
        }
        if (used.size() <= id.GetIndex()) used.resize(id.GetIndex() + 1);
        used[id.GetIndex()] = true;
        bi->CreateBodyWithID(id, s);
        bi->AddBody(id, act);
        return id;
    }

    Impl() {
        layers.MapObjectToBroadPhaseLayer(kLayerJoint, JPH::BroadPhaseLayer(1));
        layers.MapObjectToBroadPhaseLayer(kLayerSolid, JPH::BroadPhaseLayer(0));
        layers.MapObjectToBroadPhaseLayer(kLayerLoose, JPH::BroadPhaseLayer(1));
        layers.MapObjectToBroadPhaseLayer(kLayerVehicle, JPH::BroadPhaseLayer(1));
        layers.MapObjectToBroadPhaseLayer(kLayerPlate, JPH::BroadPhaseLayer(1));
        layers.MapObjectToBroadPhaseLayer(kLayerLara, JPH::BroadPhaseLayer(1));
        pairs.EnableCollision(kLayerSolid, kLayerLoose), pairs.EnableCollision(kLayerLoose, kLayerLoose);
        pairs.EnableCollision(kLayerSolid, kLayerVehicle), pairs.EnableCollision(kLayerLoose, kLayerVehicle);
        pairs.EnableCollision(kLayerPlate, kLayerLoose), pairs.EnableCollision(kLayerPlate, kLayerVehicle);
        pairs.EnableCollision(kLayerLara, kLayerLoose);
        broad.emplace(layers, 2, pairs, kLayers);
        sys.Init(4096, 0, 8192, 8192, layers, *broad, pairs);
        sys.SetGravity(Jm(vec3(0, 0, -kJointGravity * kFrame * kFrame)));
        JPH::PhysicsSettings ps;
        ps.mPenetrationSlop = 1.0f * kM; // resting bodies sink at most a unit (Jolt's 2 cm = 5.6 units shows) [I]
        sys.SetPhysicsSettings(ps);
        bi = &sys.GetBodyInterfaceNoLock();
    }
    ~Impl() {
        for (auto& v : vehicles) dropVehicle(v);
        for (auto& l : links) sys.RemoveConstraint(l.c);
        for (auto& e : joints) sys.RemoveConstraint(e.ax->c);
        for (auto& a : hingeJoints) sys.RemoveConstraint(a->c);
    }
    // The constraint of `a` with its zero at coordinate a.ref: built with the body posed there (derived from its pose at
    // 0, so the same everywhere), then the body is put back.
    JPH::Ref<JPH::TwoBodyConstraint> constraint(const Axis& a) {
        JPH::RVec3 p;
        JPH::Quat r;
        bi->GetPositionAndRotation(a.body, p, r);
        JPH::RVec3 at = a.pos0;
        JPH::Quat rot = a.rot0;
        if (a.ref != 0 && a.hinge) {
            const JPH::Quat t = JPH::Quat::sRotation(J(a.axis), a.ref);
            rot = (t * a.rot0).Normalized(), at = Jm(a.pivot) + t * (a.pos0 - Jm(a.pivot));
        } else if (a.ref != 0) {
            at = a.pos0 + J(a.axis) * a.ref;
        }
        bi->SetPositionAndRotation(a.body, at, rot, JPH::EActivation::DontActivate);
        JPH::Ref<JPH::TwoBodyConstraint> c;
        if (a.hinge) {
            JPH::HingeConstraintSettings s;
            s.mPoint1 = s.mPoint2 = Jm(a.pivot);
            s.mHingeAxis1 = s.mHingeAxis2 = J(a.axis);
            s.mNormalAxis1 = s.mNormalAxis2 = J(perpendicular(a.axis));
            c = bi->CreateConstraint(&s, JPH::BodyID(), a.body);
        } else {
            JPH::SliderConstraintSettings s;
            s.mPoint1 = s.mPoint2 = Jm(a.pivot);
            s.mSliderAxis1 = s.mSliderAxis2 = J(a.axis);
            s.mNormalAxis1 = s.mNormalAxis2 = J(perpendicular(a.axis));
            c = bi->CreateConstraint(&s, JPH::BodyID(), a.body);
        }
        bi->SetPositionAndRotation(a.body, p, r, JPH::EActivation::DontActivate);
        sys.AddConstraint(c);
        return c;
    }
    // The body is at coordinate 0 now; `key` finds a restored zero.
    std::unique_ptr<Axis> axis(JPH::BodyID body, bool hinge, vec3 pivot, vec3 dir, const std::string& key) {
        auto a = std::make_unique<Axis>();
        a->hinge = hinge, a->pivot = pivot, a->axis = glm::normalize(dir), a->body = body;
        bi->GetPositionAndRotation(body, a->pos0, a->rot0);
        if (const auto it = pendingRefs.find(key); it != pendingRefs.end()) a->ref = it->second, pendingRefs.erase(it);
        a->c = constraint(*a);
        return a;
    }
    void rebase(Axis& a, float ref) {
        sys.RemoveConstraint(a.c);
        a.ref = ref;
        a.c = constraint(a);
    }
    // Limits of `a` (rad / m) for this step.
    void limits(Axis& a, bool on, float lo, float hi) {
        constexpr float pi = JPH::JPH_PI;
        if (!on) return a.hinge ? a.h()->SetLimits(-pi, pi) : a.s()->SetLimits(-FLT_MAX, FLT_MAX);
        if (lo > hi) std::swap(lo, hi);
        if (lo > a.ref || hi < a.ref || (a.hinge && (a.ref - lo > pi || hi - a.ref > pi))) rebase(a, 0.5f * (lo + hi));
        const float l = std::min(0.0f, lo - a.ref), u = std::max(0.0f, hi - a.ref);
        if (a.hinge) a.h()->SetLimits(std::max(l, -pi), std::min(u, pi));
        else a.s()->SetLimits(l, u);
    }
    void remove(JPH::BodyID& id) {
        if (id.IsInvalid()) return;
        bi->RemoveBody(id), bi->DestroyBody(id);
        used[id.GetIndex()] = false;
        id = JPH::BodyID();
    }
    // A dynamic body at world centre of mass `com` with the data's mass and inertia (about com, body axes, game units).
    JPH::BodyID body(JPH::RefConst<JPH::Shape> shape, vec3 com, quat rot, JPH::ObjectLayer layer, float mass, mat3 inertia,
                     const std::string& key, float angularDamping = 0) {
        JPH::BodyCreationSettings s(shape, Jm(com), J(rot), JPH::EMotionType::Dynamic, layer);
        s.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
        s.mMassPropertiesOverride.mMass = mass;
        const mat3 i = inertia * (kM * kM);
        s.mMassPropertiesOverride.mInertia = JPH::Mat44(JPH::Vec4(i[0][0], i[0][1], i[0][2], 0), JPH::Vec4(i[1][0], i[1][1], i[1][2], 0),
                                                        JPH::Vec4(i[2][0], i[2][1], i[2][2], 0), JPH::Vec4(0, 0, 0, 1));
        s.mAllowSleeping = layer == kLayerLoose;
        if (layer == kLayerLoose) s.mMotionQuality = JPH::EMotionQuality::LinearCast;
        else s.mLinearDamping = 0, s.mAngularDamping = angularDamping;
        return add(s, key, JPH::EActivation::Activate);
    }
    void placeJoint(const JointE& e, float x, float v) {
        const Joint1& j = e.j;
        const quat rot = j.hinge ? glm::angleAxis(x, j.axis) : quat(1, 0, 0, 0);
        const vec3 com = j.hinge ? j.pivot + rot * j.arm : j.pivot + j.arm + j.axis * x;
        const vec3 ang = j.hinge ? j.axis * (v * kFrame) : vec3(0);
        const vec3 lin = j.hinge ? glm::cross(ang, com - j.pivot) : j.axis * (v * kFrame);
        bi->SetPositionRotationAndVelocity(e.body, Jm(com), J(rot), Jm(lin), J(ang));
    }
    JointE makeJoint(uint64_t key, const Joint1& j) {
        JointE e{key, j, {}, {}, j.x, j.v, nullptr};
        const float mass = j.mass > 0 ? j.mass : 1.0f; // massless: the motor sets the speed (unbounded below), no gravity
        float inertia = mass * kUnitsPerMetre * kUnitsPerMetre; // sliders lock rotation: any positive value
        if (j.hinge) // the data's inertia about the axis through the pivot, minus the centre of mass's parallel-axis share
            if (const float c = j.inertia - j.mass * (glm::dot(j.arm, j.arm) - glm::dot(j.arm, j.axis) * glm::dot(j.arm, j.axis)); c > 0) inertia = c;
        // A pressure plate's body has its collision from creation (a later SetShape would drop its contact cache, so a
        // restored scene would not continue alike).
        const auto plate = !j.hinge ? plateShape(key, j.pivot + j.arm) : nullptr;
        e.plate = plate != nullptr;
        e.body = body(plate ? plate : new JPH::SphereShape(0.05f), j.pivot + j.arm, quat(1, 0, 0, 0), plate ? kLayerPlate : kLayerJoint, mass,
                      mat3(inertia), jointKey(key));
        if (j.mass <= 0) bi->SetGravityFactor(e.body, 0);
        e.ax = axis(e.body, j.hinge, j.pivot, j.axis, jointKey(key));
        placeJoint(e, j.x, j.v);
        return e;
    }
    // The collision of plate `key` (Scene::setPlates) relative to its body's centre at coordinate 0; null: not a plate.
    JPH::RefConst<JPH::Shape> plateShape(uint64_t key, vec3 com0) const {
        const auto it = plateMeshes.find(key);
        if (it == plateMeshes.end()) return nullptr;
        JPH::VertexList v;
        JPH::IndexedTriangleList t;
        for (const auto& d : it->second) {
            if (!d.shape) continue;
            const uint32_t base = uint32_t(v.size());
            for (const vec3& q : d.shape->vertices) {
                const vec3 b = (vec3(d.transform * glm::vec4(q, 1)) - com0) * kM;
                v.push_back({b.x, b.y, b.z});
            }
            for (const auto& f : d.shape->faces) t.push_back({base + f.v[0], base + f.v[1], base + f.v[2]});
        }
        const auto mesh = t.empty() ? nullptr : made(JPH::MeshShapeSettings(std::move(v), std::move(t)));
        return mesh ? made(JPH::OffsetCenterOfMassShapeSettings(-mesh->GetCenterOfMass(), mesh)) : nullptr;
    }
    void dropJoint(JointE& e) { sys.RemoveConstraint(e.ax->c), remove(e.body); }
    // Limits and motor of a live joint for this step (Jolt units).
    void driveJoint(JointE& e, const Joint1& j, bool motorOff) {
        const float lin = j.hinge ? 1.0f : kM; // per-frame velocity -> rad/s or m/s
        const auto state = j.push != 0 || (j.motor && !motorOff) ? JPH::EMotorState::Velocity : JPH::EMotorState::Off;
        const float target = (j.push != 0 ? j.push : j.speed) * kFrame * lin;
        // impulse per 1/60 s step (mass * units per frame) -> force (N) / torque (N m); unbounded: <= 0, massless, pushed
        const float impulse = j.push != 0 ? j.pushBound : j.force;
        const float bound = impulse <= 0 || j.mass <= 0 ? FLT_MAX : impulse * kFrame * kSolver * kM * (j.hinge ? kM : 1.0f);
        limits(*e.ax, j.limited, j.lo * lin, j.hi * lin);
        if (j.hinge) {
            auto* h = e.ax->h();
            h->GetMotorSettings().SetTorqueLimit(bound);
            h->SetMotorState(state), h->SetTargetAngularVelocity(target);
        } else {
            auto* s = e.ax->s();
            s->GetMotorSettings().SetForceLimit(bound);
            s->SetMotorState(state), s->SetTargetVelocity(target);
        }
    }
    static std::string jointKey(uint64_t key) { return "j" + std::to_string(key); }
    JointE* findJoint(uint64_t key) {
        for (auto& e : joints)
            if (e.key == key) return &e;
        return nullptr;
    }
    // Terrain and object solids exist only while something can land on them.
    bool landing() const { return !loose.empty() || !vehicles.empty() || !ragdolls.empty() || std::ranges::any_of(joints, &JointE::solid); }
    void createSolids() {
        const bool active = landing();
        for (auto& t : terrain) {
            if (!active) { remove(t.body); continue; }
            if (!t.built) t.shape = meshShape(*t.t.mesh, glm::translate(mat4(1), t.t.offset)), t.built = true;
            if (t.shape && t.body.IsInvalid())
                t.body = add(JPH::BodyCreationSettings(t.shape, JPH::RVec3::sZero(), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                                       kLayerSolid), "t" + t.t.name, JPH::EActivation::DontActivate);
        }
        for (size_t i = 0; i < proxies.size(); ++i) {
            auto& p = proxies[i];
            if (!active) { remove(p.body); continue; }
            if (!p.body.IsInvalid()) continue;
            if (const auto shape = meshShape(*p.shape, mat4(1))) {
                JPH::BodyCreationSettings s(shape, Jm(vec3(p.transform[3])), J(glm::normalize(glm::quat_cast(mat3(p.transform)))),
                                            JPH::EMotionType::Kinematic, kLayerSolid);
                s.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided; // meshes have no volume
                s.mMassPropertiesOverride.mMass = 1, s.mMassPropertiesOverride.mInertia = JPH::Mat44::sIdentity();
                p.body = add(s, "p" + std::to_string(i), JPH::EActivation::Activate);
            }
        }
        const bool pushes = character && (!loose.empty() || !ragdolls.empty() || std::ranges::any_of(joints, &JointE::solid));
        if (!pushes) remove(lara);
        else if (lara.IsInvalid()) {
            const Character& c = *character;
            const float r = std::max(c.radius, 1.0f), half = std::max(0.5f * c.height - r, 1.0f);
            JPH::BodyCreationSettings s(new JPH::CapsuleShape(half * kM, r * kM), Jm(c.feet + vec3(0, 0, half + r)),
                                        J(glm::angleAxis(1.5707964f, vec3(1, 0, 0))), JPH::EMotionType::Dynamic, kLayerLara);
            s.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY | JPH::EAllowedDOFs::TranslationZ;
            s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            s.mMassPropertiesOverride.mMass = std::max(c.mass, 1.0f);
            s.mGravityFactor = 0, s.mAllowSleeping = false, s.mLinearDamping = 0;
            lara = add(s, "a", JPH::EActivation::Activate);
        }
    }
    // Lara's proxy heads for her capsule this step (a jump of more than 60 units, a spawn or a ledge snap, is a move),
    // at no more than 1000 units/s [I]; then the kinematic weights press on the plates under them.
    void driveCharacter(float dt) {
        if (!lara.IsInvalid()) {
            const Character& c = *character;
            const float r = std::max(c.radius, 1.0f), half = std::max(0.5f * c.height - r, 1.0f);
            const vec3 to = c.feet + vec3(0, 0, half + r), at = G(bi->GetCenterOfMassPosition(lara)) * kUnitsPerMetre;
            if (glm::length(to - at) > 60.0f) {
                bi->SetPositionRotationAndVelocity(lara, Jm(to), J(glm::angleAxis(1.5707964f, vec3(1, 0, 0))), JPH::Vec3::sZero(), JPH::Vec3::sZero());
            } else {
                vec3 v = (to - at) / dt;
                if (const float l = glm::length(v); l > 1000.0f) v *= 1000.0f / l;
                bi->SetLinearVelocity(lara, Jm(v));
            }
        }
        for (const Weight& w : weights) {
            const JPH::RRayCast ray(Jm(w.at + vec3(0, 0, 40)), Jm(vec3(0, 0, -70)));
            JPH::RayCastSettings rs;
            rs.SetBackFaceMode(JPH::EBackFaceMode::CollideWithBackFaces);
            JPH::BodyID best;
            float fraction = FLT_MAX;
            for (const auto& e : joints) {
                if (!e.plate) continue;
                JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> hit;
                bi->GetTransformedShape(e.body).CastRay(ray, rs, hit);
                if (hit.HadHit() && hit.mHit.mFraction < fraction) fraction = hit.mHit.mFraction, best = e.body;
            }
            if (!best.IsInvalid()) bi->AddImpulse(best, sys.GetGravity() * (w.mass * dt), ray.GetPointOnRay(fraction));
        }
    }
    void moveProxies(float dt) {
        for (auto& p : proxies)
            if (!p.body.IsInvalid())
                bi->MoveKinematic(p.body, Jm(vec3(p.transform[3])), J(glm::normalize(glm::quat_cast(mat3(p.transform)))), dt);
    }
    // Constraints in a fixed order (hinges, joints, links as listed): Jolt solves them by their index in its list,
    // which removals would otherwise shuffle by history.
    std::vector<JPH::Constraint*> ordered() const {
        std::vector<JPH::Constraint*> all;
        for (const auto& a : hingeJoints) all.push_back(a->c.GetPtr());
        for (const auto& e : joints) all.push_back(e.ax->c.GetPtr());
        for (const auto& l : links) all.push_back(l.c.GetPtr());
        for (const auto& v : vehicles) all.push_back(v.c.GetPtr());
        for (const auto& r : ragdolls)
            for (const auto& c : r.joints) all.push_back(c.GetPtr());
        return all;
    }
    void reorder() {
        auto all = ordered();
        if (all.empty()) return;
        sys.RemoveConstraints(all.data(), int(all.size()));
        sys.AddConstraints(all.data(), int(all.size()));
    }
    // The recorded state of Scene::restore, once every saved body exists again; otherwise the rebuilt scene stands.
    void applyRestore() {
        if (pendingIds.empty() && sys.GetConstraints().size() == pendingConstraints) {
            reorder();
            if (!sys.RestoreState(*pending)) std::fprintf(stderr, "physics: recorded state did not apply\n");
        } else {
            std::fprintf(stderr, "physics: saved scene not rebuilt alike (%zu bodies missing), state not restored\n", pendingIds.size());
        }
        pending.reset(), pendingIds.clear(), reserved.clear(), pendingRefs.clear();
    }
    // ---- vehicles ----
    VehicleE makeVehicle(int32_t id, const VehicleSpec& spec, const mat4& model);
    void dropVehicle(VehicleE& v) { sys.RemoveStepListener(v.c), sys.RemoveConstraint(v.c), remove(v.body); }
    void driveVehicles();
    void drive(const VehicleE& v);
};

Scene::Scene() : impl_(std::make_unique<Impl>()) {}
Scene::~Scene() = default;
Scene::Scene(Scene&&) noexcept = default;
Scene& Scene::operator=(Scene&&) noexcept = default;

int Scene::addBody(const MassProps& m, const mat4& model) {
    Body b;
    if (m.mass > 0) b.invMass = 1 / m.mass, b.invInertia = glm::inverse(m.inertia);
    b.com = m.com;
    b.rot = glm::normalize(glm::quat_cast(mat3(model)));
    b.pos = vec3(model * glm::vec4(m.com, 1));
    // angular damping 0.5 / s [I], as the hand-written solver before it
    const auto id = impl_->body(new JPH::SphereShape(0.05f), b.pos, b.rot, kLayerJoint, m.mass > 0 ? m.mass : 1.0f,
                                m.mass > 0 ? m.inertia : mat3(kUnitsPerMetre * kUnitsPerMetre), "h" + std::to_string(bodies.size()), 0.5f);
    if (m.mass <= 0) impl_->bi->SetMotionType(id, JPH::EMotionType::Static, JPH::EActivation::DontActivate);
    impl_->hingeBodies.push_back(id);
    bodies.push_back(b);
    return int(bodies.size()) - 1;
}

int Scene::addHinge(int body, vec3 anchor, vec3 axis, float lo, float hi) {
    const Body& b = bodies.at(size_t(body));
    Hinge h;
    h.body = body, h.lo = lo, h.hi = hi;
    h.localAnchor = anchor;
    h.localAxis = glm::normalize(axis);
    const mat4 m = b.model();
    h.worldAnchor = vec3(m * glm::vec4(anchor, 1));
    h.worldAxis = mat3(m) * h.localAxis;
    impl_->hingeJoints.push_back(impl_->axis(impl_->hingeBodies[size_t(body)], true, h.worldAnchor, h.worldAxis, "h" + std::to_string(hinges.size())));
    hinges.push_back(h);
    return int(hinges.size()) - 1;
}

float Scene::angle(int i) const { return impl_->hingeJoints.at(size_t(i))->q(); }

void Scene::setAngle(int i, float a) {
    const Hinge& h = hinges.at(size_t(i));
    Body& b = bodies[size_t(h.body)];
    b.rot = glm::normalize(glm::angleAxis(a - angle(i), h.worldAxis) * b.rot);
    b.pos = h.worldAnchor - glm::mat3_cast(b.rot) * (h.localAnchor - b.com);
    b.vel = b.angVel = vec3(0);
    impl_->bi->SetPositionRotationAndVelocity(impl_->hingeBodies[size_t(h.body)], Jm(b.pos), J(b.rot), JPH::Vec3::sZero(), JPH::Vec3::sZero());
}

void Scene::clearHinges() {
    for (auto& a : impl_->hingeJoints) impl_->sys.RemoveConstraint(a->c);
    for (auto& id : impl_->hingeBodies) impl_->remove(id);
    impl_->hingeJoints.clear(), impl_->hingeBodies.clear(), bodies.clear(), hinges.clear();
}

void Scene::setJoints(std::vector<std::pair<uint64_t, Joint1*>> joints, std::vector<JointLink> links) {
    Impl& m = *impl_;
    for (auto& l : m.links) m.sys.RemoveConstraint(l.c); // before any of their bodies goes; re-added below
    std::vector<Impl::JointE> next;
    std::vector<uint64_t> made; // joints (re)created now: their links are new too
    next.reserve(joints.size());
    for (auto& [key, j] : joints) {
        Impl::JointE* e = m.findJoint(key);
        const bool same = e && e->j.hinge == j->hinge && e->j.axis == j->axis && e->j.pivot == j->pivot && e->j.arm == j->arm &&
                          e->j.mass == j->mass && e->j.inertia == j->inertia;
        if (e && !same) m.dropJoint(*e), e->key = ~uint64_t(0), e = nullptr;
        if (!e) made.push_back(key);
        Impl::JointE n = e ? std::move(*e) : m.makeJoint(key, *j);
        if (e) e->key = ~uint64_t(0); // moved
        if (n.x != j->x || n.v != j->v) m.placeJoint(n, j->x, j->v), n.x = j->x, n.v = j->v; // set from outside
        n.live = j;
        next.push_back(std::move(n));
    }
    for (auto& e : m.joints)
        if (e.key != ~uint64_t(0)) m.dropJoint(e);
    m.joints = std::move(next);
    std::vector<Impl::LinkE> keep;
    for (const auto& l : links) {
        Impl::JointE* a = m.findJoint(l.driver);
        Impl::JointE* b = m.findJoint(l.follower);
        if (!a || !b || a == b || l.ratio == 0) continue;
        const auto old = std::ranges::find_if(m.links, [&](const Impl::LinkE& e) {
            return e.c && e.l.driver == l.driver && e.l.follower == l.follower && e.l.ratio == l.ratio;
        });
        JPH::Ref<LinkConstraint> c;
        if (old != m.links.end() && std::ranges::find(made, l.driver) == made.end() && std::ranges::find(made, l.follower) == made.end()) {
            c = old->c;
        } else {
            auto& lock = m.sys.GetBodyLockInterfaceNoLock();
            JPH::Body* ba = lock.TryGetBody(a->body);
            JPH::Body* bb = lock.TryGetBody(b->body);
            if (!ba || !bb) continue;
            // follower = ratio * driver in Jolt units (rad, m)
            const float r = l.ratio * (a->j.hinge ? 1.0f : kUnitsPerMetre) * (b->j.hinge ? 1.0f : kM);
            c = new LinkConstraint(*ba, *bb, {a->ax.get(), a->j.hinge, J(a->j.axis)}, {b->ax.get(), b->j.hinge, J(b->j.axis)}, r);
        }
        m.sys.AddConstraint(c);
        keep.push_back({l, c});
    }
    m.links = std::move(keep);
}

void Scene::setLoose(std::span<const Loose> loose) {
    Impl& m = *impl_;
    std::vector<Impl::LooseE> next;
    for (const auto& l : loose) {
        const auto it = std::ranges::find(m.loose, l.id, &Impl::LooseE::id);
        if (it != m.loose.end()) {
            next.push_back(*it), it->id = INT32_MIN;
            continue;
        }
        if (!l.physics) continue;
        if (l.rail) { // the joint body becomes the object, colliding, in the frame it has at coordinate 0
            if (auto* e = m.findJoint(l.rail); e && !e->solid) {
                const MassProps mp = massProps(*l.physics);
                m.bi->SetShape(e->body, new JPH::RotatedTranslatedShape(JPH::Vec3::sZero(), J(glm::normalize(glm::quat_cast(mat3(l.model)))),
                                                                        looseShape(*l.physics, mp.com)), false, JPH::EActivation::Activate);
                m.bi->SetObjectLayer(e->body, kLayerLoose);
                m.bi->SetMotionQuality(e->body, JPH::EMotionQuality::LinearCast);
                e->solid = true;
            }
            continue;
        }
        MassProps mp = massProps(*l.physics);
        if (mp.mass <= 0) mp.mass = 666, mp.com = vec3(0, 0, 20), mp.inertia = mat3(0.4f * 666 * 20 * 20); // tru.exe's fallback mass [O]
        const quat rot = glm::normalize(glm::quat_cast(mat3(l.model)));
        next.push_back({l.id, m.body(looseShape(*l.physics, mp.com), vec3(l.model * glm::vec4(mp.com, 1)), rot, kLayerLoose, mp.mass,
                                     mp.inertia, "l" + std::to_string(l.id)), mp.com});
        if (l.vel != vec3(0) || l.angVel != vec3(0)) m.bi->SetLinearAndAngularVelocity(next.back().body, Jm(l.vel), J(l.angVel));
    }
    for (auto& e : m.loose)
        if (e.id != INT32_MIN) m.remove(e.body);
    m.loose = std::move(next);
}

std::optional<mat4> Scene::loose(int32_t id) const {
    const auto it = std::ranges::find(impl_->loose, id, &Impl::LooseE::id);
    if (it == impl_->loose.end()) return std::nullopt;
    JPH::RVec3 p;
    JPH::Quat q;
    impl_->bi->GetPositionAndRotation(it->body, p, q);
    return glm::translate(mat4(1), G(p) * kUnitsPerMetre) * glm::mat4_cast(G(q)) * glm::translate(mat4(1), -it->com);
}

void Scene::setTerrain(std::span<const Terrain> units) {
    Impl& m = *impl_;
    std::vector<Impl::TerrainE> next;
    for (const auto& u : units) {
        const auto it = std::ranges::find_if(m.terrain, [&](const Impl::TerrainE& e) { return e.t.name == u.name && e.t.offset == u.offset && e.t.mesh == u.mesh; });
        if (it != m.terrain.end()) next.push_back(std::move(*it)), it->t.mesh = nullptr;
        else next.push_back({u, nullptr, {}, false});
    }
    for (auto& e : m.terrain)
        if (e.t.mesh) m.remove(e.body);
    m.terrain = std::move(next);
}

void Scene::setColliders(std::span<const collision::Dynamic> dynamics) {
    Impl& m = *impl_;
    std::vector<Impl::ProxyE> next;
    for (const auto& d : dynamics) {
        if (!d.shape) continue;
        const auto it = std::ranges::find(m.proxies, d.shape, &Impl::ProxyE::shape);
        if (it != m.proxies.end()) next.push_back({std::move(it->shape), d.transform, it->body}), it->body = JPH::BodyID();
        else next.push_back({d.shape, d.transform, {}});
    }
    for (auto& p : m.proxies) m.remove(p.body);
    m.proxies = std::move(next);
}

void Scene::setPlates(std::span<const Plate> plates, std::span<const Weight> weights) {
    Impl& m = *impl_;
    m.weights.assign(weights.begin(), weights.end());
    m.plateMeshes.clear();
    for (const auto& p : plates) m.plateMeshes[p.joint].assign(p.meshes.begin(), p.meshes.end());
}

void Scene::setCharacter(const Character* c) {
    if (c) impl_->character = *c;
    else impl_->character.reset();
}

void Scene::setWater(std::span<const Water> water) { impl_->water.assign(water.begin(), water.end()); }

// tru.exe 0x6AD070: each physics body overlapping a water volume gets Havok buoyancy with the volume's density +0x80
// (4.5555e-5 kg per unit^3 = 1000 kg/m^3), linear drag +0x84 0.5, angular +0x88 0.01 (scaled per object by its physics
// property bytes [I: 1]) and the current (0x6ACB60's per-frame vector as the fluid velocity). Jolt's factor is the
// fluid density over the body's: 1000 x volume / mass.
void Scene::Impl::buoyancy(float dt) {
    if (water.empty()) return;
    for (const auto& l : loose) {
        const vec3 c = G(bi->GetCenterOfMassPosition(l.body)) * kUnitsPerMetre;
        for (const Water& w : water) {
            const vec3 d = c - w.centre;
            const glm::vec2 local(w.yaw.x * d.x + w.yaw.y * d.y, -w.yaw.y * d.x + w.yaw.x * d.y);
            if (std::abs(local.x) > w.half.x || std::abs(local.y) > w.half.y || std::abs(d.z) > w.half.z) continue;
            float volume = 0, invMass = 0;
            {
                JPH::BodyLockRead lock(sys.GetBodyLockInterfaceNoLock(), l.body);
                if (!lock.Succeeded()) break;
                volume = lock.GetBody().GetShape()->GetVolume(), invMass = lock.GetBody().GetMotionProperties()->GetInverseMass();
            }
            const float mass = invMass > 0 ? 1.0f / invMass : 0.0f;
            if (mass <= 0 || volume <= 0) break;
            const vec3 fluid = w.flow ? w.flow(w.user, c) / 30.0f : vec3(0);
            bi->ApplyBuoyancyImpulse(l.body, JPH::RVec3(Jm(vec3(c.x, c.y, w.surface))), JPH::Vec3::sAxisZ(), 1000.0f * volume / mass,
                                     0.5f, 0.01f, Jm(fluid), sys.GetGravity(), dt);
            break;
        }
    }
}

// A blast's push on the bodies in its radius. tru.exe has two rules [O]: 0x493170 -> 0x492CC0 gives each Havok shape an
// impulse (0.0004 x its surface area x the knock x (1 - distance / radius), capped per shape) at its point nearest the
// centre (0x4DF090), along (shape - centre) raised by half their distance (+Z; level with the blast: 26.6 degrees up);
// and the area hit 0x69F1E0 knocks instances (message 0x91) at knock .. edge by their distance, heading away from the
// centre, at the explosion record's elevation. Ours [I]: one velocity change per body of near .. edge units/s over the
// radius along the raised line, at its centre of mass. With an `elevation` (degrees, an explosion record's: the squibs)
// the same momentum goes along the body's heading from the centre at that elevation, as the instances', and at the
// point of the body's bounds nearest the centre, as the shapes': squib_explode_door goes off at the foot of its own door
// leaf and kicks the foot out of the snug doorway, where a push through the centre of mass only hops the leaf and leaves
// it standing in its frame. A centre inside the body's bounds has no heading to it: the body goes the way it already
// moves (the leaf pulled off its slide by the rope, toward Lara), else the way its centre of mass lies from the centre
// (the leaf blown off untouched in l3_bedroom_hall: 30 units toward the hallway [O data], into the pit).
void Scene::blast(vec3 at, float radius, float near, float edge, float elevation) {
    const auto push = [&](JPH::BodyID id) {
        const vec3 p = G(impl_->bi->GetCenterOfMassPosition(id)) * kUnitsPerMetre, d = p - at;
        const float l = glm::length(d);
        if (l >= radius) return;
        const float speed = glm::mix(near, edge, l / radius);
        const float flat = glm::length(glm::vec2(d));
        if (elevation < 0 || flat <= 1e-3f) {
            impl_->bi->AddLinearVelocity(id, Jm((l > 1e-3f ? glm::normalize(d + vec3(0, 0, 0.5f * l)) : vec3(0, 0, 1)) * speed));
            return;
        }
        float invMass = 0;
        {
            JPH::BodyLockRead lock(impl_->sys.GetBodyLockInterfaceNoLock(), id);
            if (lock.Succeeded() && lock.GetBody().IsDynamic()) invMass = lock.GetBody().GetMotionProperties()->GetInverseMass();
        }
        if (invMass <= 0) return;
        const JPH::AABox box = impl_->bi->GetTransformedShape(id).GetWorldSpaceBounds();
        glm::vec2 heading = glm::vec2(d) / flat;
        if (const glm::vec2 v(G(impl_->bi->GetLinearVelocity(id)) * kUnitsPerMetre); box.Contains(Jm(at)) && glm::length(v) > 1.0f)
            heading = glm::normalize(v);
        const vec3 dir(heading * std::cos(glm::radians(elevation)), std::sin(glm::radians(elevation)));
        impl_->bi->AddImpulse(id, Jm(dir * speed) / invMass, JPH::RVec3(box.GetClosestPoint(Jm(at))));
    };
    for (size_t i = 0; i < bodies.size(); ++i)
        if (bodies[i].invMass > 0) push(impl_->hingeBodies[i]);
    for (const auto& l : impl_->loose) push(l.body);
    for (const auto& r : impl_->ragdolls)
        for (const auto b : r.bodies) push(b);
}

// ---- vehicles ---------------------------------------------------------------------------------------------------------
// tru.exe's Vehicle is one Havok body with a ray per wheel from its suspension top (0x792900) and a spring along it;
// Jolt's VehicleConstraint is the same model (ray collision tester). Mapping: docs/formats/PHYSICS.md "Vehicles".
Scene::Impl::VehicleE Scene::Impl::makeVehicle(int32_t id, const VehicleSpec& spec, const mat4& model) {
    MassProps mp = massProps(*spec.physics);
    if (mp.mass <= 0) mp.mass = 666, mp.com = vec3(0, 0, 20), mp.inertia = mat3(0.4f * 666 * 20 * 20); // as setLoose
    const quat rot = glm::normalize(glm::quat_cast(mat3(model)));
    const auto bodyId = body(looseShape(*spec.physics, mp.com), vec3(model * glm::vec4(mp.com, 1)), rot, kLayerVehicle,
                             mp.mass, mp.inertia, "v" + std::to_string(id));
    bi->SetMotionQuality(bodyId, JPH::EMotionQuality::LinearCast); // fast over thin terrain
    JPH::VehicleConstraintSettings s;
    s.mUp = JPH::Vec3(0, 0, 1), s.mForward = JPH::Vec3(0, -1, 0); // model -Y forward
    s.mMaxPitchRollAngle = spec.maxPitchRoll > 0 ? spec.maxPitchRoll : JPH::JPH_PI;
    auto* ctl = new JPH::WheeledVehicleControllerSettings;
    for (size_t i = 0; i < spec.wheels.size(); ++i) {
        const auto& w = spec.wheels[i];
        auto* ws = new JPH::WheelSettingsWV;
        ws->mPosition = Jm(w.attach - mp.com);
        ws->mSuspensionDirection = JPH::Vec3(0, 0, -1), ws->mSteeringAxis = ws->mWheelUp = JPH::Vec3(0, 0, 1);
        ws->mWheelForward = JPH::Vec3(0, -1, 0);
        ws->mSuspensionMinLength = 0, ws->mSuspensionMaxLength = std::max(w.travel * kM, 0.001f);
        ws->mSuspensionSpring.mFrequency = w.frequency, ws->mSuspensionSpring.mDamping = w.damping;
        ws->mRadius = std::max(w.radius * kM, 0.01f), ws->mWidth = std::max(w.width * kM, 0.01f);
        if (w.mass > 0) ws->mInertia = 0.5f * w.mass * ws->mRadius * ws->mRadius;
        ws->mAngularDamping = 0;
        if (w.friction > 0) { // Jolt's default curves (peak 1.2 at slip 0.06 / 3 degrees) scaled to the data's peak
            ws->mLongitudinalFriction.Clear(), ws->mLateralFriction.Clear();
            for (const auto& [slip, f] : {std::pair(0.0f, 0.0f), {0.06f, 1.0f}, {0.2f, 0.833f}}) ws->mLongitudinalFriction.AddPoint(slip, f * w.friction);
            for (const auto& [slip, f] : {std::pair(0.0f, 0.0f), {3.0f, 1.0f}, {20.0f, 0.833f}}) ws->mLateralFriction.AddPoint(slip, f * w.friction);
        }
        ws->mMaxSteerAngle = w.steer, ws->mMaxBrakeTorque = w.brake, ws->mMaxHandBrakeTorque = w.handBrake;
        // tru.exe applies a wheel's forces `raise` x (attach -> contact) above the contact (0x792900); Jolt takes one
        // fixed point: the same fraction of the full travel [I].
        ws->mEnableSuspensionForcePoint = w.raise > 0;
        ws->mSuspensionForcePoint = ws->mPosition + ws->mSuspensionDirection * ((1 - w.raise) * ws->mSuspensionMaxLength);
        s.mWheels.push_back(ws);
    }
    // The wheels roll free and brake; the drive is tru.exe's force per wheel (drive). Jolt's engine stays out: one
    // differential (Jolt needs a driven wheel) behind a manual gearbox left in neutral.
    ctl->mDifferentials.resize(1);
    ctl->mDifferentials[0].mRightWheel = int(spec.wheels.size()) - 1;
    ctl->mTransmission.mMode = JPH::ETransmissionMode::Manual;
    s.mController = ctl;
    JPH::Body* b = sys.GetBodyLockInterfaceNoLock().TryGetBody(bodyId);
    JPH::Ref<JPH::VehicleConstraint> c = new JPH::VehicleConstraint(*b, s);
    c->SetVehicleCollisionTester(new JPH::VehicleCollisionTesterRay(kLayerVehicle, JPH::Vec3(0, 0, 1)));
    sys.AddConstraint(c), sys.AddStepListener(c);
    return {id, &spec, bodyId, mp.com, c, {}, vec3(0), vec3(0)};
}

// The engine (tru.exe 0x791D10, 0x792900): rpm from the speed of the driven wheels over the ground, sum(drive x
// v / radius) x ratio; torque from the curve (none above the top rpm; reverse: its fixed torque) x throttle x ratio;
// each driven wheel on the ground pushes its drive share / radius of it along the body's forward axis, at its force
// point. (Uses only state Jolt saves: the wheels' contact bodies.)
void Scene::Impl::drive(const VehicleE& v) {
    const VehicleSpec& spec = *v.spec;
    if (v.drive.forward <= 0 || spec.torque.empty()) return;
    const JPH::RMat44 world = bi->GetWorldTransform(v.body);
    const JPH::Vec3 forward = world.Multiply3x3(JPH::Vec3(0, -1, 0)); // the force's direction: the body's (0x792900)
    const float speed = bi->GetLinearVelocity(v.body).Dot(forward); // m/s
    const float ratio = v.drive.reverse ? spec.reverseRatio : spec.ratio;
    float omega = 0;
    for (const auto& w : spec.wheels) omega += w.drive * std::abs(speed) / std::max(w.radius * kM, 0.01f);
    const float rpm = omega * ratio * 60 / (2 * JPH::JPH_PI);
    if (rpm > spec.maxRpm) return;
    float torque = spec.reverseTorque;
    if (!v.drive.reverse) {
        const float x = std::clamp(rpm, spec.minRpm, spec.maxRpm) / spec.maxRpm * float(spec.torque.size() - 1);
        const size_t i = std::min(size_t(x), spec.torque.size() - 1), j = std::min(i + 1, spec.torque.size() - 1);
        torque = spec.maxTorque * (spec.torque[i] + (spec.torque[j] - spec.torque[i]) * (x - float(i)));
    }
    torque *= v.drive.forward * ratio * (v.drive.reverse ? -1.0f : 1.0f);
    const auto& wheels = v.c->GetWheels();
    for (size_t i = 0; i < wheels.size(); ++i) {
        const auto& w = spec.wheels[i];
        if (w.drive <= 0 || !wheels[i]->HasContact()) continue;
        const JPH::WheelSettings* ws = wheels[i]->GetSettings();
        const JPH::Vec3 local = ws->mEnableSuspensionForcePoint ? ws->mSuspensionForcePoint : ws->mPosition;
        bi->AddForce(v.body, forward * (w.drive * torque / std::max(w.radius * kM, 0.01f)), world * local);
    }
}

// This step's driver input, frame shift and kicks (after a restored state, which records the previous inputs).
void Scene::Impl::driveVehicles() {
    for (auto& v : vehicles) {
        if (v.shift != vec3(0)) bi->SetPosition(v.body, bi->GetPosition(v.body) + Jm(v.shift), JPH::EActivation::Activate);
        if (v.kick != vec3(0)) bi->AddLinearVelocity(v.body, Jm(v.kick));
        auto* c = static_cast<JPH::WheeledVehicleController*>(v.c->GetController());
        c->SetDriverInput(0, v.drive.right, v.drive.brake, v.drive.handBrake);
        drive(v);
        // Lean: the roll about the forward axis (from the world's up) is servoed to drive.lean at leanRate on the ground
        // and in the air (tru.exe 0x793680: a motor on the body; Vehicle.cpp picks the target); pitch is free up to the
        // data's tilt limit (mMaxPitchRollAngle). Jolt's MotorcycleController leans with a spring that pumped energy
        // into this light, tall chassis (and a parked bike falls over).
        const JPH::Quat q = bi->GetRotation(v.body);
        const JPH::Vec3 fwd = q * JPH::Vec3(0, -1, 0), up = q * JPH::Vec3(0, 0, 1), side = fwd.Cross(JPH::Vec3(0, 0, 1));
        if (side.LengthSq() > 1e-4f) {
            const float roll = -std::asin(std::clamp(up.Dot(side.Normalized()), -1.0f, 1.0f)); // + = left side down
            const JPH::Vec3 w = bi->GetAngularVelocity(v.body);
            bi->SetAngularVelocity(v.body, w + fwd * (-v.spec->leanRate * (v.drive.lean - roll) - w.Dot(fwd)));
        }
        if (v.drive.yawTorque != 0) bi->AddTorque(v.body, up * v.drive.yawTorque);
        if (v.drive.sideForce != 0) bi->AddForce(v.body, q * JPH::Vec3(v.drive.sideForce, 0, 0));
        if (const auto& wheels = v.c->GetWheels(); v.drive.pushWheel >= 0 && size_t(v.drive.pushWheel) < wheels.size()) {
            const JPH::Wheel& wh = *wheels[size_t(v.drive.pushWheel)];
            const JPH::RVec3 at = wh.HasContact() ? wh.GetContactPosition() : bi->GetWorldTransform(v.body) * wh.GetSettings()->mPosition;
            bi->AddForce(v.body, q * JPH::Vec3(v.drive.push.x, v.drive.push.y, v.drive.push.z), at);
        }
        if (const JPH::Vec3 w = bi->GetAngularVelocity(v.body); v.drive.maxYawRate > 0 && std::abs(w.GetZ()) > v.drive.maxYawRate)
            bi->SetAngularVelocity(v.body, JPH::Vec3(w.GetX(), w.GetY(), std::copysign(v.drive.maxYawRate, w.GetZ())));
        if (const auto& wheels = v.c->GetWheels(); v.drive.pinWheel >= 0 && size_t(v.drive.pinWheel) < wheels.size() && wheels[size_t(v.drive.pinWheel)]->HasContact()) {
            const JPH::Vec3 r = JPH::Vec3(wheels[size_t(v.drive.pinWheel)]->GetContactPosition() - bi->GetCenterOfMassPosition(v.body));
            const JPH::Vec3 lin = bi->GetLinearVelocity(v.body), at = lin + bi->GetAngularVelocity(v.body).Cross(r);
            bi->SetLinearVelocity(v.body, lin - JPH::Vec3(at.GetX(), at.GetY(), 0));
        }
        v.shift = v.kick = vec3(0);
    }
}

void Scene::setVehicles(std::span<const Vehicle> vehicles) {
    Impl& m = *impl_;
    std::vector<Impl::VehicleE> next;
    for (const auto& v : vehicles) {
        const auto it = std::ranges::find(m.vehicles, v.id, &Impl::VehicleE::id);
        if (it != m.vehicles.end() && it->spec == v.spec) next.push_back(std::move(*it)), it->id = INT32_MIN;
        else if (v.spec && v.spec->physics) next.push_back(m.makeVehicle(v.id, *v.spec, v.model));
        else continue;
        next.back().drive = v.drive, next.back().shift += v.shift, next.back().kick += v.kick;
    }
    for (auto& v : m.vehicles)
        if (v.id != INT32_MIN) m.dropVehicle(v);
    m.vehicles = std::move(next);
}

std::optional<VehicleState> Scene::vehicle(int32_t id) const {
    const auto it = std::ranges::find(impl_->vehicles, id, &Impl::VehicleE::id);
    if (it == impl_->vehicles.end()) return std::nullopt;
    VehicleState s;
    JPH::RVec3 p;
    JPH::Quat q;
    impl_->bi->GetPositionAndRotation(it->body, p, q);
    s.model = glm::translate(mat4(1), G(p) * kUnitsPerMetre) * glm::mat4_cast(G(q)) * glm::translate(mat4(1), -it->com);
    s.vel = G(impl_->bi->GetLinearVelocity(it->body)) * kUnitsPerMetre;
    const auto& wheels = it->c->GetWheels();
    for (size_t i = 0; i < wheels.size(); ++i)
        if (wheels[i]->HasContact()) s.contacts |= 1u << i;
    if (!wheels.empty()) s.wheelAngle = wheels.back()->GetRotationAngle();
    s.rpm = static_cast<const JPH::WheeledVehicleController*>(it->c->GetController())->GetEngine().GetCurrentRPM();
    return s;
}

// Order: new solids, a pending restored state (Scene::restore), drives (which may rebuild constraints), kinematic
// moves, the fixed constraint order, Jolt's step. A loaded scene therefore steps exactly as the saved one would have.
void Scene::step(float dt) {
    Impl& m = *impl_;
    m.createSolids();
    if (m.pending) m.applyRestore();
    for (size_t i = 0; i < hinges.size(); ++i) { // limits; motor toward the target, rate-limited, landing on it
        const Hinge& h = hinges[i];
        Axis& a = *m.hingeJoints[i];
        m.limits(a, true, h.lo, h.hi);
        if (h.target && h.maxTorque > 0) {
            a.h()->GetMotorSettings().SetTorqueLimit(h.maxTorque * kM * kM);
            a.h()->SetMotorState(JPH::EMotorState::Velocity);
            a.h()->SetTargetAngularVelocity(std::clamp((*h.target - a.q()) / dt, -h.maxSpeed, h.maxSpeed));
        } else {
            a.h()->SetMotorState(JPH::EMotorState::Off);
        }
    }
    for (auto& e : m.joints) { // a pushed driver's followers drop their motors (tru.exe: her push drives the chain)
        bool followerOfPush = false;
        for (const auto& l : m.links)
            if (l.l.follower == e.key)
                if (const auto* d = m.findJoint(l.l.driver); d && d->live && d->live->push != 0) followerOfPush = true;
        m.driveJoint(e, *e.live, followerOfPush);
    }
    m.driveVehicles();
    m.moveProxies(dt);
    m.driveCharacter(dt);
    m.buoyancy(dt);
    m.reorder();
    m.sys.Update(dt, 1, &m.temp, &m.jobs);
    for (size_t i = 0; i < bodies.size(); ++i) {
        JPH::RVec3 p;
        JPH::Quat q;
        const auto id = m.hingeBodies[i];
        m.bi->GetPositionAndRotation(id, p, q);
        bodies[i].pos = G(p) * kUnitsPerMetre, bodies[i].rot = G(q);
        bodies[i].vel = G(m.bi->GetLinearVelocity(id)) * kUnitsPerMetre, bodies[i].angVel = G(m.bi->GetAngularVelocity(id));
    }
    for (auto& e : m.joints) {
        Joint1& j = *e.live;
        if (j.hinge) {
            j.x = e.x + wrap(e.ax->q() - wrap(e.x));
            j.v = glm::dot(G(m.bi->GetAngularVelocity(e.body)), j.axis) / kFrame;
        } else {
            j.x = e.ax->q() * kUnitsPerMetre;
            j.v = glm::dot(G(m.bi->GetLinearVelocity(e.body)), j.axis) * kUnitsPerMetre / kFrame;
        }
        j.push = 0, j.pushBound = 0;
        e.x = j.x, e.v = j.v, e.live = nullptr;
    }
}

// Layout (version 1): body count, {key, Jolt BodyID}; constraint zero count, {key, ref}; constraint count; then
// PhysicsSystem::SaveState(All): global, bodies, contact cache, constraints (our link rows record their warm start).
std::string Scene::save() const {
    const Impl& m = *impl_;
    JPH::StateRecorderImpl r;
    const auto str = [&](const std::string& s) { r.Write(uint32_t(s.size())), r.WriteBytes(s.data(), s.size()); };
    std::vector<std::pair<std::string, JPH::BodyID>> ids;
    std::vector<std::pair<std::string, float>> refs;
    for (size_t i = 0; i < m.hingeBodies.size(); ++i) ids.push_back({"h" + std::to_string(i), m.hingeBodies[i]});
    for (size_t i = 0; i < m.hingeJoints.size(); ++i) refs.push_back({"h" + std::to_string(i), m.hingeJoints[i]->ref});
    for (const auto& e : m.joints) ids.push_back({Impl::jointKey(e.key), e.body}), refs.push_back({Impl::jointKey(e.key), e.ax->ref});
    for (const auto& l : m.loose) ids.push_back({"l" + std::to_string(l.id), l.body});
    for (const auto& v : m.vehicles) ids.push_back({"v" + std::to_string(v.id), v.body});
    for (const auto& r : m.ragdolls)
        for (size_t b = 0; b < r.bodies.size(); ++b) ids.push_back({"r" + std::to_string(r.id) + "." + std::to_string(b), r.bodies[b]});
    for (const auto& t : m.terrain)
        if (!t.body.IsInvalid()) ids.push_back({"t" + t.t.name, t.body});
    for (size_t i = 0; i < m.proxies.size(); ++i)
        if (!m.proxies[i].body.IsInvalid()) ids.push_back({"p" + std::to_string(i), m.proxies[i].body});
    if (!m.lara.IsInvalid()) ids.push_back({"a", m.lara});
    r.Write(uint32_t(1));
    r.Write(uint32_t(ids.size()));
    for (const auto& [key, id] : ids) str(key), r.Write(id.GetIndexAndSequenceNumber());
    r.Write(uint32_t(refs.size()));
    for (const auto& [key, ref] : refs) str(key), r.Write(ref);
    r.Write(uint32_t(m.ordered().size()));
    m.sys.SaveState(r, JPH::EStateRecorderState::All);
    return r.GetData();
}

bool Scene::restore(std::string_view state) {
    Impl& m = *impl_;
    for (auto& l : m.links) m.sys.RemoveConstraint(l.c);
    for (auto& e : m.joints) m.dropJoint(e);
    clearHinges();
    for (auto& l : m.loose) m.remove(l.body);
    for (auto& v : m.vehicles) m.dropVehicle(v);
    for (auto& r : m.ragdolls) m.dropRagdoll(r);
    for (auto& t : m.terrain) m.remove(t.body);
    for (auto& p : m.proxies) m.remove(p.body);
    m.remove(m.lara);
    m.links.clear(), m.joints.clear(), m.loose.clear(), m.vehicles.clear(), m.proxies.clear(), m.ragdolls.clear();
    m.pending.reset(), m.pendingIds.clear(), m.reserved.clear(), m.pendingRefs.clear();
    auto r = std::make_unique<JPH::StateRecorderImpl>();
    r->WriteBytes(state.data(), state.size());
    const auto str = [&] {
        uint32_t n = 0;
        r->Read(n);
        std::string s(r->IsFailed() || n > state.size() ? 0 : n, '\0');
        r->ReadBytes(s.data(), s.size());
        return s;
    };
    uint32_t version = 0, count = 0;
    r->Read(version), r->Read(count);
    if (version != 1 || r->IsFailed()) return false;
    for (uint32_t i = 0; i < count && !r->IsFailed(); ++i) {
        const std::string key = str();
        uint32_t raw = 0;
        r->Read(raw);
        const JPH::BodyID id(raw);
        if (id.GetIndex() >= 4096) return m.pendingIds.clear(), m.reserved.clear(), false;
        m.pendingIds[key] = id, m.reserved.insert(id.GetIndex());
    }
    r->Read(count);
    for (uint32_t i = 0; i < count && !r->IsFailed(); ++i) {
        const std::string key = str();
        r->Read(m.pendingRefs[key]);
    }
    r->Read(m.pendingConstraints);
    if (r->IsFailed()) return m.pendingIds.clear(), m.reserved.clear(), m.pendingRefs.clear(), false;
    m.pending = std::move(r);
    return true;
}

// ---- ragdolls ---------------------------------------------------------------------------------------------------------
// tru.exe 0x4F0B40 per bone: a capsule from the start to the end point (radius, the total mass x share), and a joint to
// its parent at start + pivot: 1 a hinge about axis 1 (0x4DF5D0), 2 limits about axes 1 and 2 (0x4E08A0), 3 a cone
// and limits about its twist axis (0x4EE5C0): axis 1, or the bone when 0x49AFA0 says axis 1 is unset [I]. Ours: a Jolt
// SixDOF constraint (pyramid swing), translation fixed, X = axis 1 (type 3: the twist axis, swinging within the cone
// about Y and Z), Y = axis 2; built with the bodies in the bind pose, so a joint's zero is the bind pose [I] (the
// poacher's death pose lies inside the limits so measured, knees and elbows included); joint friction like the motor
// bounds (impulse per 1/60 s step, PHYSICS.md "Havok -> Jolt") [I]. Parent and child do not collide (Jolt's filter).
void Scene::setRagdolls(std::span<const Ragdoll> ragdolls) {
    constexpr float pi = JPH::JPH_PI, kFriction = 0.6f; // surface friction [I] (Jolt's 0.2 lets bodies slide down stairs)
    Impl& m = *impl_;
    std::vector<const Ragdoll*> sorted; // by id: a fixed constraint order (Impl::ordered)
    for (const auto& r : ragdolls) sorted.push_back(&r);
    std::ranges::sort(sorted, {}, [](const Ragdoll* r) { return r->id; });
    std::vector<Impl::RagdollE> next;
    for (const Ragdoll* r : sorted) {
        if (const auto it = std::ranges::find(m.ragdolls, r->id, &Impl::RagdollE::id); it != m.ragdolls.end()) {
            next.push_back(std::move(*it)), it->id = INT32_MIN;
            continue;
        }
        if (!r->def || r->def->bones.empty() || r->frames.size() < r->def->bones.size()) continue;
        const RagdollDef& d = *r->def;
        const size_t n = d.bones.size();
        Impl::RagdollE e{r->id, {}, {}};
        JPH::Ref<JPH::GroupFilterTable> filter = new JPH::GroupFilterTable(uint32_t(n));
        for (size_t b = 0; b < n; ++b)
            if (d.bones[b].parent >= 0) filter->DisableCollision(JPH::CollisionGroup::SubGroupID(b), JPH::CollisionGroup::SubGroupID(d.bones[b].parent));
        const auto bind = [&](int s) { return s >= 0 && size_t(s) < r->bind.size() ? r->bind[size_t(s)] : vec3(0); };
        std::vector<vec3> from(n), to(n);
        for (size_t b = 0; b < n; ++b) { // in the bind pose: body frame = its start segment's (rotation-free) bind frame
            const RagdollBone& o = d.bones[b];
            const vec3 seg = bind(o.segment);
            from[b] = seg + o.startOffset, to[b] = bind(o.endSegment) + o.endOffset;
            const vec3 dir = to[b] - from[b];
            const float len = glm::length(dir), radius = std::max(o.radius, 1.0f) * kM;
            JPH::Ref<JPH::ShapeSettings> s;
            if (len > 1e-3f) s = new JPH::CapsuleShapeSettings(0.5f * len * kM, radius);
            else s = new JPH::SphereShapeSettings(radius);
            const quat rot = len > 1e-3f ? quat(vec3(0, 1, 0), dir / len) : quat(1, 0, 0, 0); // Jolt capsules run along Y
            JPH::BodyCreationSettings bs(made(JPH::RotatedTranslatedShapeSettings(Jm(0.5f * (from[b] + to[b]) - seg), J(rot), s)), Jm(seg),
                                         JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, kLayerLoose);
            bs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            bs.mMassPropertiesOverride.mMass = std::max(d.mass * o.massShare * 0.01f, 0.1f);
            bs.mCollisionGroup = JPH::CollisionGroup(filter, JPH::CollisionGroup::GroupID(uint32_t(r->id)), JPH::CollisionGroup::SubGroupID(b));
            bs.mMotionQuality = JPH::EMotionQuality::LinearCast;
            bs.mFriction = kFriction, bs.mAngularDamping = 0.5f; // as the hinge bodies [I]
            e.bodies.push_back(m.add(bs, "r" + std::to_string(r->id) + "." + std::to_string(b), JPH::EActivation::Activate));
        }
        for (size_t b = 0; b < n; ++b) {
            const RagdollBone& o = d.bones[b];
            if (o.parent < 0 || o.joint < 1 || o.joint > 3) continue;
            const vec3 bone = to[b] - from[b];
            const bool axis = glm::length(o.axis1) > 1e-6f;
            const vec3 x = axis ? glm::normalize(o.axis1) : o.joint == 3 && glm::length(bone) > 1e-3f ? glm::normalize(bone) : vec3(1, 0, 0);
            const vec3 hint = o.joint == 2 ? o.axis2 : vec3(0);
            const vec3 y0 = hint - x * glm::dot(hint, x);
            const vec3 y = glm::length(y0) > 1e-3f ? glm::normalize(y0) : perpendicular(x);
            JPH::SixDOFConstraintSettings s; // world space, bodies in the bind pose
            s.mPosition1 = s.mPosition2 = Jm(from[b] + o.pivot);
            s.mAxisX1 = s.mAxisX2 = J(x), s.mAxisY1 = s.mAxisY2 = J(y);
            s.mSwingType = JPH::ESwingType::Pyramid;
            using A = JPH::SixDOFConstraintSettings::EAxis;
            for (const A a : {A::TranslationX, A::TranslationY, A::TranslationZ}) s.MakeFixedAxis(a);
            const auto rad = [&](int k) { return std::clamp(glm::radians(o.limits[k]), -pi, pi); };
            const auto limit = [&](A a, float lo, float hi) { s.SetLimitedAxis(a, std::min(lo, hi), std::max(lo, hi)); };
            limit(A::RotationX, rad(0), rad(1));
            if (o.joint == 1) s.MakeFixedAxis(A::RotationY), s.MakeFixedAxis(A::RotationZ);
            else if (o.joint == 2) limit(A::RotationY, rad(2), rad(3)), s.MakeFixedAxis(A::RotationZ);
            else limit(A::RotationY, -std::abs(rad(4)), std::abs(rad(4))), limit(A::RotationZ, -std::abs(rad(4)), std::abs(rad(4)));
            for (const A a : {A::RotationX, A::RotationY, A::RotationZ}) s.mMaxFriction[a] = o.friction * kFrame * kSolver * kM * kM;
            JPH::Ref<JPH::Constraint> c = m.bi->CreateConstraint(&s, e.bodies[size_t(o.parent)], e.bodies[b]);
            m.sys.AddConstraint(c);
            e.joints.push_back(c);
        }
        size_t hit = 0; // the bone nearest the hit point takes the impulse
        for (size_t b = 0; b < n; ++b) {
            const mat4& f = r->frames[b];
            const vec3 v = b < r->velocity.size() ? r->velocity[b] : vec3(0);
            m.bi->SetPositionRotationAndVelocity(e.bodies[b], Jm(vec3(f[3])), J(glm::normalize(glm::quat_cast(mat3(f)))), Jm(v), JPH::Vec3::sZero());
            if (glm::distance(vec3(f[3]), r->hitPoint) < glm::distance(vec3(r->frames[hit][3]), r->hitPoint)) hit = b;
        }
        if (r->impulse != vec3(0)) m.bi->AddImpulse(e.bodies[hit], Jm(r->impulse), m.bi->GetCenterOfMassPosition(e.bodies[hit]));
        next.push_back(std::move(e));
    }
    for (auto& e : m.ragdolls)
        if (e.id != INT32_MIN) m.dropRagdoll(e);
    m.ragdolls = std::move(next);
}

std::optional<Scene::RagdollPose> Scene::ragdoll(int32_t id) const {
    const auto it = std::ranges::find(impl_->ragdolls, id, &Impl::RagdollE::id);
    if (it == impl_->ragdolls.end()) return std::nullopt;
    RagdollPose p;
    p.asleep = true;
    for (const auto b : it->bodies) {
        JPH::RVec3 at;
        JPH::Quat q;
        impl_->bi->GetPositionAndRotation(b, at, q);
        p.frames.push_back(glm::translate(mat4(1), G(at) * kUnitsPerMetre) * glm::mat4_cast(G(q)));
        if (impl_->bi->IsActive(b)) p.asleep = false;
    }
    return p;
}

} // namespace ou::physics
