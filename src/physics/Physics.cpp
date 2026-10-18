// Object physics data (HInfo primitives, model markers, physical class) and the rigid-body solver.
// Evidence: docs/formats/PHYSICS.md.
#include "physics/Physics.hpp"

#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

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
mat3 skew(vec3 v) { return mat3(0, v.z, -v.y, -v.z, 0, v.x, v.y, -v.x, 0); } // skew(v) * u = v x u
mat3 diag(float a, float b, float c) { return mat3(a, 0, 0, 0, b, 0, 0, 0, c); }

// HInfo (segment +0x3C): {u32 count, ptr list} pairs; slot 0 spheres, 1 boxes, 2 markers, 3 capsules.
constexpr uint32_t kSphereStride = 0x20, kBoxStride = 0x50, kCapsuleStride = 0x40;

void readHInfo(const cdc::Section& m, uint32_t h, int segment, std::vector<Prim>& out) {
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
        p.mass = at<uint16_t>(m, o + 0x14);
    });
    list(1, kBoxStride, [&](uint32_t o, Prim& p) { // type 5
        p.kind = PrimKind::Box;
        p.size = vec(m, o), p.pos = vec(m, o + 0x10), p.rot = quatXyzw(m, o + 0x20);
        head(o + 0x30, p);
        p.id = at<uint16_t>(m, o + 0x34), p.mass = at<uint16_t>(m, o + 0x36);
    });
    list(3, kCapsuleStride, [&](uint32_t o, Prim& p) { // type 10
        p.kind = PrimKind::Capsule;
        p.pos = vec(m, o), p.rot = quatXyzw(m, o + 0x10);
        head(o + 0x20, p);
        p.id = at<uint16_t>(m, o + 0x24), p.radius = at<uint16_t>(m, o + 0x26), p.length = at<uint16_t>(m, o + 0x28);
        p.mass = at<uint16_t>(m, o + 0x2A);
    });
}

} // namespace

ObjectPhysics loadObjectPhysics(const cdc::Drm& d) {
    ObjectPhysics o;
    const auto base = formats::objectBaseSection(d);
    if (!base) return o;
    const auto& ob = d.sections[*base];
    o.oflags = at<uint32_t>(ob, 0);
    if (const auto p = cdc::internalPointer(ob, 0xC8); p && *p + 6 <= ob.data.size() && at<uint16_t>(ob, *p + 4) == 0xB00B)
        o.physClass = at<uint16_t>(ob, *p + 2);
    // Model 0: ObjectBase +0xA0 u16 count, +0xA4 -> resource refs of the model DTPs (formats::objectRenderModels).
    const auto ptrs = cdc::sectionPointers(d, *base);
    const auto lp = ptrs.find(0xA4);
    if (!at<uint16_t>(ob, 0xA0) || lp == ptrs.end()) return o;
    const auto& list = d.sections[lp->second.section];
    if (cdc::resourceRefType(list, lp->second.offset) != int(cdc::SectionType::DtpData)) return o;
    const uint32_t modelId = at<uint32_t>(list, lp->second.offset);
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
        if (const auto h = cdc::internalPointer(*m, s + 0x3C)) readHInfo(*m, *h, int(k), o.prims);
    }
    const uint32_t nm = at<uint32_t>(*m, 0x38);
    if (const auto mk = cdc::internalPointer(*m, 0x3C); mk && nm < 4096)
        for (uint32_t i = 0; i < nm; ++i) {
            const uint32_t q = *mk + 0x30 * i;
            o.markers.push_back({vec(*m, q), vec(*m, q + 0x10), at<uint32_t>(*m, q + 0x20), at<int32_t>(*m, q + 0x24)});
        }
    return o;
}

std::shared_ptr<const collision::CollisionMesh> solidShape(const ObjectPhysics& o, int segment) {
    auto shape = std::make_shared<collision::CollisionMesh>();
    for (const auto& p : o.prims) {
        if (p.segment != segment || !p.solid()) continue;
        // ponytail: spheres/capsules as their bounding boxes; exact shapes when a curved prim must block precisely.
        const vec3 size = p.kind == PrimKind::Box      ? p.size
                          : p.kind == PrimKind::Sphere ? vec3(2 * p.radius)
                                                       : vec3(2 * p.radius, 2 * p.radius, p.length + 2 * p.radius);
        collision::addBox(*shape, glm::translate(mat4(1), p.pos) * glm::mat4_cast(p.rot), size, kObjectMask);
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

vec3 Mover::position() const {
    if (points.size() < 2) return points.empty() ? vec3(0) : points[0];
    float len = 0;
    for (size_t i = 1; i < points.size(); ++i) len += glm::distance(points[i - 1], points[i]);
    if (len <= 0) return points[0];
    float u = std::fmod(s, 2 * len);
    if (u > len) u = 2 * len - u; // ping-pong
    for (size_t i = 1; i < points.size(); ++i) {
        const float l = glm::distance(points[i - 1], points[i]);
        if (u <= l) return points[i - 1] + (points[i] - points[i - 1]) * (l > 0 ? u / l : 0.f);
        u -= l;
    }
    return points.back();
}

int Scene::addBody(const MassProps& m, const mat4& model) {
    Body b;
    if (m.mass > 0) b.invMass = 1 / m.mass, b.invInertia = glm::inverse(m.inertia);
    b.com = m.com;
    b.rot = glm::normalize(glm::quat_cast(mat3(model)));
    b.pos = vec3(model * glm::vec4(m.com, 1));
    bodies.push_back(b);
    return int(bodies.size()) - 1;
}

int Scene::addHinge(int body, vec3 anchor, vec3 axis, float lo, float hi) {
    const Body& b = bodies.at(body);
    Hinge h;
    h.body = body, h.lo = lo, h.hi = hi;
    h.localAnchor = anchor;
    h.localAxis = glm::normalize(axis);
    h.localRef = glm::normalize(glm::cross(h.localAxis, std::abs(h.localAxis.x) < 0.9f ? vec3(1, 0, 0) : vec3(0, 1, 0)));
    const mat4 m = b.model();
    h.worldAnchor = vec3(m * glm::vec4(anchor, 1));
    h.worldAxis = mat3(m) * h.localAxis;
    h.worldRef = mat3(m) * h.localRef;
    hinges.push_back(h);
    return int(hinges.size()) - 1;
}

float Scene::angle(int i) const {
    const Hinge& h = hinges.at(i);
    const vec3 ref = glm::mat3_cast(bodies[h.body].rot) * h.localRef;
    return std::atan2(glm::dot(glm::cross(h.worldRef, ref), h.worldAxis), glm::dot(h.worldRef, ref));
}

void Scene::setAngle(int i, float a) {
    const Hinge& h = hinges.at(i);
    Body& b = bodies[h.body];
    b.rot = glm::normalize(glm::angleAxis(a - angle(i), h.worldAxis) * b.rot);
    b.pos = h.worldAnchor - glm::mat3_cast(b.rot) * (h.localAnchor - b.com);
    b.vel = b.angVel = vec3(0);
}

// Sequential impulses on velocities (Baumgarte position feedback), then semi-implicit Euler. Same inputs and dt
// give the same result: no time- or order-dependent state besides the bodies.
void Scene::step(float dt) {
    constexpr float kBeta = 0.2f;
    for (auto& m : movers) m.s += m.speed * dt;
    for (auto& b : bodies) {
        if (b.invMass <= 0) continue;
        b.vel += gravity * dt;
        b.angVel *= std::exp(-angularDamping * dt);
    }
    std::vector<float> accLo(hinges.size()), accHi(hinges.size()), accMotor(hinges.size());
    std::vector<float> theta(hinges.size());
    for (size_t i = 0; i < hinges.size(); ++i) theta[i] = angle(int(i));
    for (int it = 0; it < iterations; ++it)
        for (size_t i = 0; i < hinges.size(); ++i) {
            const Hinge& h = hinges[i];
            Body& b = bodies[h.body];
            if (b.invMass <= 0) continue;
            const mat3 R = glm::mat3_cast(b.rot), invI = R * b.invInertia * glm::transpose(R);
            const vec3 ax = h.worldAxis;
            auto angular = [&](vec3 dir, float bias, float lo, float hi, float& acc) {
                const float k = glm::dot(dir, invI * dir);
                if (k <= 0) return;
                const float old = acc;
                acc = std::clamp(acc - (glm::dot(b.angVel, dir) + bias) / k, lo, hi);
                b.angVel += invI * dir * (acc - old);
            };
            // Keep the body axis on the hinge axis (two rows perpendicular to it).
            const vec3 err = glm::cross(R * h.localAxis, ax);
            const vec3 t1 = glm::normalize(glm::cross(ax, std::abs(ax.x) < 0.9f ? vec3(1, 0, 0) : vec3(0, 1, 0))), t2 = glm::cross(ax, t1);
            for (const vec3 t : {t1, t2}) {
                float acc = 0; // unbounded rows: nothing to accumulate
                angular(t, -kBeta / dt * glm::dot(err, t), -1e30f, 1e30f, acc);
            }
            // Motor toward the target angle, then the limits.
            const float inf = 1e30f, th = theta[i];
            if (h.target) {
                const float speed = std::clamp(h.gain * (*h.target - th), -h.maxSpeed, h.maxSpeed);
                angular(ax, -speed, -h.maxTorque * dt, h.maxTorque * dt, accMotor[i]);
            }
            if (th < h.lo) angular(ax, kBeta / dt * (th - h.lo), 0, inf, accLo[i]);
            if (th > h.hi) angular(ax, kBeta / dt * (th - h.hi), -inf, 0, accHi[i]);
            // Anchor point.
            const vec3 r = R * (h.localAnchor - b.com);
            const mat3 K = b.invMass * mat3(1) - skew(r) * invI * skew(r);
            const vec3 cdot = b.vel + glm::cross(b.angVel, r) + kBeta / dt * (b.pos + r - h.worldAnchor);
            const vec3 P = -(glm::inverse(K) * cdot);
            b.vel += b.invMass * P;
            b.angVel += invI * glm::cross(r, P);
        }
    for (auto& b : bodies) {
        if (b.invMass <= 0) continue;
        b.pos += b.vel * dt;
        b.rot = glm::normalize(b.rot + quat(0, b.angVel.x, b.angVel.y, b.angVel.z) * b.rot * (0.5f * dt));
    }
}

} // namespace ou::physics
