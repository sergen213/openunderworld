// Enemy path finding on the units' NavPower nav meshes (formats::NavGraph; tru.exe's planner asks NavPower, e.g.
// MoveNav -> 0x60E690). Ours: A* over the convex areas through their shared edges, then the funnel (string pulling)
// for the corners. Evidence and deviations: docs/GAMEPLAY.md "Navigation".
#include "game/ai/Enemy.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace ou::game::ai {

namespace {
constexpr float kNavZ = 250.0f; // [I] a point is on an area whose floor lies this close above / below it (the mesh floats ~50 up)
float cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }
} // namespace

void NavMesh::add(const formats::NavGraph& g, vec3 offset) {
    const int base = int(areas_.size());
    radius = g.radius;
    for (const auto& a : g.areas) {
        Area& n = areas_.emplace_back();
        n.centre = a.centre + offset, n.normal = a.normal;
        float twice = 0;
        for (size_t k = 0; k < a.verts.size(); ++k) {
            n.verts.push_back(a.verts[k] + offset);
            n.adj.push_back(a.adj[k] < 0 ? -1 : a.adj[k] + base);
            twice += cross(glm::vec2(a.verts[k]), glm::vec2(a.verts[(k + 1) % a.verts.size()]));
        }
        n.ccw = twice > 0;
        n.lo = n.hi = glm::vec2(n.centre);
        for (const vec3 v : n.verts) n.lo = glm::min(n.lo, glm::vec2(v)), n.hi = glm::max(n.hi, glm::vec2(v));
    }
}

float NavMesh::floorZ(const Area& a, vec3 p) const {
    return std::abs(a.normal.z) < 1e-3f ? a.centre.z : a.centre.z - (a.normal.x * (p.x - a.centre.x) + a.normal.y * (p.y - a.centre.y)) / a.normal.z;
}

// ponytail: linear scan of every area (a few thousand at most); a grid when profiling says so.
int NavMesh::locate(vec3 p) const {
    int best = -1;
    float bestDz = kNavZ;
    for (size_t i = 0; i < areas_.size(); ++i) {
        const Area& a = areas_[i];
        if (p.x < a.lo.x || p.y < a.lo.y || p.x > a.hi.x || p.y > a.hi.y) continue;
        const float dz = std::abs(floorZ(a, p) - p.z);
        if (dz >= bestDz) continue;
        bool inside = true;
        for (size_t k = 0; k < a.verts.size() && inside; ++k) {
            const float c = cross(glm::vec2(a.verts[(k + 1) % a.verts.size()] - a.verts[k]), glm::vec2(p - a.verts[k]));
            inside = a.ccw ? c >= 0 : c <= 0;
        }
        if (inside) best = int(i), bestDz = dz;
    }
    return best;
}

std::vector<vec3> NavMesh::path(vec3 from, vec3 to) const {
    const int s = locate(from), t = locate(to);
    if (s < 0 || t < 0) return {};
    if (s == t) return {to};
    // A* (areas; g measured through the portal midpoints), ties by area index so the route is deterministic.
    const auto mid = [&](int a, size_t k) { return (areas_[size_t(a)].verts[k] + areas_[size_t(a)].verts[(k + 1) % areas_[size_t(a)].verts.size()]) * 0.5f; };
    std::vector<float> g(areas_.size(), INFINITY);
    std::vector<int> came(areas_.size(), -1);
    std::vector<vec3> at(areas_.size());
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    g[size_t(s)] = 0, at[size_t(s)] = from;
    open.push({glm::distance(from, to), s});
    while (!open.empty()) {
        const auto [f, a] = open.top();
        open.pop();
        if (a == t) break;
        if (f > g[size_t(a)] + glm::distance(at[size_t(a)], to) + 1e-3f) continue; // stale
        const Area& area = areas_[size_t(a)];
        for (size_t k = 0; k < area.adj.size(); ++k) {
            const int b = area.adj[k];
            if (b < 0) continue;
            const vec3 m = mid(a, k);
            const float ng = g[size_t(a)] + glm::distance(at[size_t(a)], m);
            if (ng >= g[size_t(b)]) continue;
            g[size_t(b)] = ng, came[size_t(b)] = a, at[size_t(b)] = m;
            open.push({ng + glm::distance(m, to), b});
        }
    }
    if (came[size_t(t)] < 0) return {};
    std::vector<int> route{t};
    while (route.back() != s) route.push_back(came[size_t(route.back())]);
    std::reverse(route.begin(), route.end());
    // Portals (left, right as seen walking through), then the funnel (Mononen's simple stupid funnel).
    std::vector<std::pair<vec3, vec3>> portals{{from, from}};
    for (size_t i = 0; i + 1 < route.size(); ++i) {
        const Area& a = areas_[size_t(route[i])];
        for (size_t k = 0; k < a.adj.size(); ++k)
            if (a.adj[k] == route[i + 1]) {
                const vec3 p = a.verts[k], q = a.verts[(k + 1) % a.verts.size()];
                portals.push_back(a.ccw ? std::pair{q, p} : std::pair{p, q}); // interior on the edge's left when ccw
                break;
            }
    }
    portals.push_back({to, to});
    std::vector<vec3> out;
    vec3 apex = from, left = from, right = from;
    size_t leftAt = 0, rightAt = 0;
    const auto flat = [](vec3 v) { return glm::vec2(v); };
    for (size_t i = 1; i < portals.size(); ++i) {
        const vec3 l = portals[i].first, r = portals[i].second;
        if (cross(flat(right - apex), flat(r - apex)) >= 0) { // the right side narrows
            if (flat(apex) == flat(right) || cross(flat(left - apex), flat(r - apex)) < 0) {
                right = r, rightAt = i;
            } else { // crossed the left side: its corner is the next apex
                out.push_back(left), apex = right = left, rightAt = i = leftAt;
                continue;
            }
        }
        if (cross(flat(left - apex), flat(l - apex)) <= 0) { // the left side narrows
            if (flat(apex) == flat(left) || cross(flat(right - apex), flat(l - apex)) > 0) {
                left = l, leftAt = i;
            } else {
                out.push_back(right), apex = left = right, leftAt = i = rightAt;
                continue;
            }
        }
    }
    if (out.empty() || glm::distance(out.back(), to) > 1e-3f) out.push_back(to);
    return out;
}

vec3 NavMesh::steer(vec3 from, vec3 to) const {
    for (const vec3 c : path(from, to))
        if (glm::length(glm::vec2(c - from)) > 10.0f) return c;
    return to; // off the mesh, unreachable, or there
}

} // namespace ou::game::ai
