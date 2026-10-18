// BVH and queries over a CollisionMesh. Geometry routines follow Ericson, "Real-Time Collision
// Detection" (closest points) and Moller-Trumbore (ray/triangle).
#include "collision/Collision.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace ou::collision {
namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

struct Tri { vec3 a, b, c; };

// Closest point on triangle abc to p (Ericson 5.1.5).
vec3 closestOnTri(vec3 p, const Tri& t) {
    const vec3 ab = t.b - t.a, ac = t.c - t.a, ap = p - t.a;
    const float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return t.a;
    const vec3 bp = p - t.b;
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return t.b;
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) return t.a + ab * (d1 / (d1 - d3));
    const vec3 cp = p - t.c;
    const float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return t.c;
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) return t.a + ac * (d2 / (d2 - d6));
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0 && d4 - d3 >= 0 && d5 - d6 >= 0) return t.b + (t.c - t.b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    const float denom = 1 / (va + vb + vc);
    return t.a + ab * (vb * denom) + ac * (vc * denom);
}

// Closest points between segments p1-q1 and p2-q2 (Ericson 5.1.9).
void closestSegSeg(vec3 p1, vec3 q1, vec3 p2, vec3 q2, vec3& c1, vec3& c2) {
    const vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    float s = 0, t = 0;
    if (a <= 1e-12f && e <= 1e-12f) {
    } else if (a <= 1e-12f) {
        t = std::clamp(f / e, 0.f, 1.f);
    } else {
        const float c = dot(d1, r);
        if (e <= 1e-12f) {
            s = std::clamp(-c / a, 0.f, 1.f);
        } else {
            const float b = dot(d1, d2), denom = a * e - b * b;
            s = denom > 0 ? std::clamp((b * f - c * e) / denom, 0.f, 1.f) : 0.f;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = std::clamp(-c / a, 0.f, 1.f);
            else if (t > 1) t = 1, s = std::clamp((b - c) / a, 0.f, 1.f);
        }
    }
    c1 = p1 + d1 * s;
    c2 = p2 + d2 * t;
}

// Ray (origin o, direction d) vs triangle, double-sided; returns parametric t or -1.
// Rays within ~1e-5 rad of the triangle plane count as parallel (det = -d.(e1 x e2)): an in-plane ray
// would otherwise "hit" a wall at every point along it.
float rayTri(vec3 o, vec3 d, const Tri& t) {
    const vec3 e1 = t.b - t.a, e2 = t.c - t.a, p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::abs(det) <= 1e-5f * glm::length(d) * glm::length(cross(e1, e2))) return -1;
    const float inv = 1 / det;
    const vec3 s = o - t.a;
    const float u = dot(s, p) * inv;
    if (u < 0 || u > 1) return -1;
    const vec3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0 || u + v > 1) return -1;
    return dot(e2, q) * inv;
}

// Closest points between segment a-b (a == b: point) and a triangle.
float segTriDist(vec3 a, vec3 b, const Tri& t, vec3& onSeg, vec3& onTri) {
    const vec3 d = b - a;
    if (dot(d, d) > 0) {
        const float s = rayTri(a, d, t);
        if (s >= 0 && s <= 1) { onSeg = onTri = a + d * s; return 0; }
    }
    float best = kInf;
    auto consider = [&](vec3 ps, vec3 pt) {
        const float l = glm::length(ps - pt);
        if (l < best) best = l, onSeg = ps, onTri = pt;
    };
    consider(a, closestOnTri(a, t));
    if (dot(d, d) > 0) {
        consider(b, closestOnTri(b, t));
        const vec3 e[3][2] = {{t.a, t.b}, {t.b, t.c}, {t.c, t.a}};
        for (const auto& edge : e) {
            vec3 cs, ct;
            closestSegSeg(a, b, edge[0], edge[1], cs, ct);
            consider(cs, ct);
        }
    }
    return best;
}

float boxDist2(vec3 p, vec3 lo, vec3 hi) {
    const vec3 q = glm::clamp(p, lo, hi) - p;
    return dot(q, q);
}

} // namespace

vec3 CollisionMesh::faceNormal(uint32_t face) const {
    const Face& f = faces[face];
    const vec3 n = cross(vertices[f.v[1]] - vertices[f.v[0]], vertices[f.v[2]] - vertices[f.v[0]]);
    const float l = glm::length(n);
    return l > 0 ? n / l : vec3(0, 0, 1);
}

void CollisionMesh::build() {
    nodes_.clear();
    order_.resize(faces.size());
    std::vector<vec3> centroid(faces.size());
    for (uint32_t i = 0; i < faces.size(); ++i) {
        order_[i] = i;
        const Face& f = faces[i];
        centroid[i] = (vertices[f.v[0]] + vertices[f.v[1]] + vertices[f.v[2]]) / 3.f;
    }
    if (faces.empty()) return;
    nodes_.reserve(2 * faces.size() / 4 + 1);
    // Median split on the longest centroid axis; leaves of <= 4 faces. Children are allocated as a pair.
    // ponytail: no SAH; enough for static level queries, switch to binned SAH if traversal shows up in profiles.
    struct Item { uint32_t node, first, count; };
    std::vector<Item> stack{{0, 0, uint32_t(faces.size())}};
    nodes_.emplace_back();
    while (!stack.empty()) {
        const Item it = stack.back();
        stack.pop_back();
        vec3 lo(kInf), hi(-kInf), clo(kInf), chi(-kInf);
        for (uint32_t i = it.first; i < it.first + it.count; ++i) {
            const Face& f = faces[order_[i]];
            for (uint32_t v : f.v) lo = glm::min(lo, vertices[v]), hi = glm::max(hi, vertices[v]);
            clo = glm::min(clo, centroid[order_[i]]), chi = glm::max(chi, centroid[order_[i]]);
        }
        Node& n = nodes_[it.node];
        n.lo = lo;
        n.hi = hi;
        const vec3 ext = chi - clo;
        if (it.count <= 4 || std::max({ext.x, ext.y, ext.z}) <= 0) {
            n.first = it.first;
            n.count = it.count;
            continue;
        }
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        const uint32_t half = it.count / 2;
        std::nth_element(order_.begin() + it.first, order_.begin() + it.first + half, order_.begin() + it.first + it.count,
                         [&](uint32_t a, uint32_t b) { return centroid[a][axis] < centroid[b][axis]; });
        const auto child = uint32_t(nodes_.size());
        n.first = child; // `n` is dangling after emplace_back
        nodes_.emplace_back();
        nodes_.emplace_back();
        stack.push_back({child, it.first, half});
        stack.push_back({child + 1, it.first + half, it.count - half});
    }
}

template <class Visit> void CollisionMesh::overlap(vec3 lo, vec3 hi, Visit&& visit) const {
    if (nodes_.empty()) return;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& n = nodes_[stack[--sp]];
        if (glm::any(glm::greaterThan(n.lo, hi)) || glm::any(glm::lessThan(n.hi, lo))) continue;
        if (n.count)
            for (uint32_t i = n.first; i < n.first + n.count; ++i) visit(order_[i]);
        else
            stack[sp++] = n.first, stack[sp++] = n.first + 1;
    }
}

std::optional<Hit> CollisionMesh::raycast(vec3 origin, vec3 dir, float maxDist, uint8_t mask) const {
    const float len = glm::length(dir);
    if (len <= 0 || nodes_.empty()) return std::nullopt;
    const vec3 d = dir / len;
    // Zero components get a huge finite inverse: avoids 0 * inf = NaN for origins on a slab plane.
    auto safeInv = [](float c) { return 1 / (std::abs(c) > 1e-20f ? c : std::copysign(1e-20f, c)); };
    const vec3 inv(safeInv(d.x), safeInv(d.y), safeInv(d.z));
    std::optional<Hit> best;
    float bestT = maxDist;
    auto slab = [&](const Node& n) {
        const vec3 t0 = (n.lo - origin) * inv, t1 = (n.hi - origin) * inv;
        const vec3 tmin = glm::min(t0, t1), tmax = glm::max(t0, t1);
        const float enter = std::max({tmin.x, tmin.y, tmin.z, 0.f}), exit = std::min({tmax.x, tmax.y, tmax.z, bestT});
        return enter <= exit ? enter : kInf;
    };
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& n = nodes_[stack[--sp]];
        if (slab(n) == kInf) continue;
        if (!n.count) {
            const float a = slab(nodes_[n.first]), b = slab(nodes_[n.first + 1]);
            if (a < b) stack[sp++] = n.first + 1, stack[sp++] = n.first; // nearer child popped first
            else stack[sp++] = n.first, stack[sp++] = n.first + 1;
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i) {
            const Face& f = faces[order_[i]];
            if (!queryable(f, mask)) continue;
            const float t = rayTri(origin, d, {vertices[f.v[0]], vertices[f.v[1]], vertices[f.v[2]]});
            if (t >= 0 && t <= bestT) {
                bestT = t;
                vec3 nrm = faceNormal(order_[i]);
                if (dot(nrm, d) > 0) nrm = -nrm;
                best = Hit{t, origin + d * t, nrm, order_[i]};
            }
        }
    }
    return best;
}

std::optional<Hit> CollisionMesh::sweepCapsule(vec3 a, vec3 b, float radius, vec3 delta, uint8_t mask) const {
    const vec3 lo = glm::min(glm::min(a, b), glm::min(a, b) + delta) - radius,
               hi = glm::max(glm::max(a, b), glm::max(a, b) + delta) + radius;
    constexpr float kEps = 1e-3f; // contact tolerance, game units
    std::optional<Hit> best;
    overlap(lo, hi, [&](uint32_t fi) {
        const Face& f = faces[fi];
        if (!queryable(f, mask)) return;
        const Tri tri{vertices[f.v[0]], vertices[f.v[1]], vertices[f.v[2]]};
        // Conservative advancement: under pure translation the shape-triangle distance d(t) is convex,
        // so stepping to the root of its tangent never overshoots the first contact.
        float t = 0;
        for (int iter = 0; iter < 64; ++iter) {
            vec3 ps, pt;
            const float d = segTriDist(a + delta * t, b + delta * t, tri, ps, pt) - radius;
            vec3 dir = ps - pt;
            const float l = glm::length(dir);
            dir = l > 0 ? dir / l : faceNormal(fi);
            if (d <= kEps) {
                if (l == 0 && dot(dir, (a + b) * 0.5f + delta * t - pt) < 0) dir = -dir;
                if (!best || t < best->t) best = Hit{t, pt, dir, fi};
                return;
            }
            const float closing = -dot(delta, dir);
            if (closing <= 0) return; // moving apart: d(t) never decreases again
            t += d / closing;
            if (t > 1 || (best && t >= best->t)) return;
        }
        // ponytail: 64 Newton steps without contact counts as a miss (grazing within ~kEps).
    });
    return best;
}

std::optional<Hit> CollisionMesh::sweepSphere(vec3 center, float radius, vec3 delta, uint8_t mask) const {
    return sweepCapsule(center, center, radius, delta, mask);
}

std::optional<Hit> CollisionMesh::closestPoint(vec3 p, float maxDist, uint8_t mask) const {
    if (nodes_.empty()) return std::nullopt;
    std::optional<Hit> best;
    float best2 = maxDist * maxDist;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp) {
        const Node& n = nodes_[stack[--sp]];
        if (boxDist2(p, n.lo, n.hi) > best2) continue;
        if (!n.count) {
            stack[sp++] = n.first, stack[sp++] = n.first + 1;
            continue;
        }
        for (uint32_t i = n.first; i < n.first + n.count; ++i) {
            const Face& f = faces[order_[i]];
            if (!queryable(f, mask)) continue;
            const vec3 c = closestOnTri(p, {vertices[f.v[0]], vertices[f.v[1]], vertices[f.v[2]]});
            const float d2 = dot(c - p, c - p);
            if (d2 <= best2) {
                best2 = d2;
                const float d = std::sqrt(d2);
                vec3 nrm = d > 0 ? (p - c) / d : faceNormal(order_[i]);
                best = Hit{d, c, nrm, order_[i]};
            }
        }
    }
    return best;
}

} // namespace ou::collision
