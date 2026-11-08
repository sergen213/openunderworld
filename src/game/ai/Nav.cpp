// Enemy path finding on the units' NavPower nav meshes (formats::NavGraph; tru.exe's planner asks NavPower, e.g.
// MoveNav -> 0x60E690). Ours: A* over the convex areas through their shared edges and the units' NavPower links
// (formats::NavLink) an agent's usage flags allow, then the funnel (string pulling) for the corners of each stretch
// between links. Evidence and deviations: docs/GAMEPLAY.md "Navigation" and "Nav links".
#include "game/ai/Enemy.hpp"

#include <algorithm>
#include <cmath>
#include <queue>

namespace ou::game::ai {

namespace {
constexpr float kNavZ = 250.0f; // [I] a point is on an area whose floor lies this close above / below it (the mesh floats ~50 up)
float cross(glm::vec2 a, glm::vec2 b) { return a.x * b.y - a.y * b.x; }
// Closest point of segment p-q to x (by the horizontal distance) and its parameter.
float closestT(vec3 p, vec3 q, vec3 x) {
    const glm::vec2 d(q - p);
    const float l = glm::dot(d, d);
    return l < 1e-6f ? 0.0f : std::clamp(glm::dot(glm::vec2(x - p), d) / l, 0.0f, 1.0f);
}
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

// NavPower snaps each end to the nearest area of every layer in the link's layer mask (all: 0x48B770 keeps the
// default -1) within maxSnapDist [I: nearest by horizontal distance among areas whose floor is within kNavZ].
void NavMesh::addLink(const formats::NavLink& l, vec3 offset) {
    Link n;
    n.a = {l.a[0] + offset, l.a[1] + offset}, n.b = {l.b[0] + offset, l.b[1] + offset};
    n.fromA = l.fromA, n.fromB = l.fromB, n.usage = l.usage, n.penalty = l.penalty;
    n.areaA = snap((n.a[0] + n.a[1]) * 0.5f, l.maxSnap), n.areaB = snap((n.b[0] + n.b[1]) * 0.5f, l.maxSnap);
    if (n.areaA >= 0 && n.areaB >= 0 && n.areaA != n.areaB) links_.push_back(n);
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

int NavMesh::snap(vec3 p, float maxDist) const {
    if (const int a = locate(p); a >= 0) return a;
    int best = -1;
    float bestD = maxDist;
    for (size_t i = 0; i < areas_.size(); ++i) {
        const Area& a = areas_[i];
        if (p.x < a.lo.x - bestD || p.y < a.lo.y - bestD || p.x > a.hi.x + bestD || p.y > a.hi.y + bestD) continue;
        for (size_t k = 0; k < a.verts.size(); ++k) {
            const vec3 v = a.verts[k], w = a.verts[(k + 1) % a.verts.size()];
            const vec3 c = glm::mix(v, w, closestT(v, w, p));
            const float d = glm::length(glm::vec2(c - p));
            if (d < bestD && std::abs(floorZ(a, c) - p.z) < kNavZ) best = int(i), bestD = d;
        }
    }
    return best;
}

std::vector<vec3> NavMesh::funnel(vec3 from, const std::vector<int>& route, vec3 to) const {
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

std::vector<NavStep> NavMesh::route(vec3 from, vec3 to, uint16_t usage) const {
    const int s = locate(from), t = locate(to);
    if (s < 0 || t < 0) return {};
    if (s == t) return {{to}};
    // A* (areas; g measured through the portal midpoints and over the links' take-off -> landing, times the link's
    // penalty multiplier [I: NavPower's cost]), ties by area index so the route is deterministic.
    const auto mid = [&](int a, size_t k) { return (areas_[size_t(a)].verts[k] + areas_[size_t(a)].verts[(k + 1) % areas_[size_t(a)].verts.size()]) * 0.5f; };
    const size_t n = areas_.size();
    std::vector<float> g(n, INFINITY);
    std::vector<int> came(n, -1), via(n, -1); // via: link index entered over, -1 a portal
    std::vector<vec3> at(n), off(n);          // where the area was entered; the take-off point of `via`
    using Item = std::pair<float, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> open;
    g[size_t(s)] = 0, at[size_t(s)] = from;
    open.push({glm::distance(from, to), s});
    const auto relax = [&](int a, int b, vec3 enter, float cost, int link, vec3 takeoff) {
        const float ng = g[size_t(a)] + cost;
        if (ng >= g[size_t(b)]) return;
        g[size_t(b)] = ng, came[size_t(b)] = a, at[size_t(b)] = enter, via[size_t(b)] = link, off[size_t(b)] = takeoff;
        open.push({ng + glm::distance(enter, to), b});
    };
    while (!open.empty()) {
        const auto [f, a] = open.top();
        open.pop();
        if (a == t) break;
        if (f > g[size_t(a)] + glm::distance(at[size_t(a)], to) + 1e-3f) continue; // stale
        const Area& area = areas_[size_t(a)];
        for (size_t k = 0; k < area.adj.size(); ++k)
            if (const int b = area.adj[k]; b >= 0) relax(a, b, mid(a, k), glm::distance(at[size_t(a)], mid(a, k)), -1, {});
        if (!usage) continue;
        for (size_t i = 0; i < links_.size(); ++i) {
            const Link& l = links_[i];
            if (!(l.usage & usage)) continue;
            for (int dir = 0; dir < 2; ++dir) {
                if (!(dir ? l.fromB : l.fromA) || (dir ? l.areaB : l.areaA) != a) continue;
                const auto& S = dir ? l.b : l.a;
                const auto& E = dir ? l.a : l.b;
                const float u = closestT(S[0], S[1], at[size_t(a)]); // strips: the closest take-off on the edge [I]
                const vec3 takeoff = glm::mix(S[0], S[1], u), land = glm::mix(E[0], E[1], u);
                relax(a, dir ? l.areaA : l.areaB, land,
                      glm::distance(at[size_t(a)], takeoff) + glm::distance(takeoff, land) * l.penalty, int(i * 2 + size_t(dir)), takeoff);
            }
        }
    }
    if (came[size_t(t)] < 0) return {};
    std::vector<int> areas{t};
    while (areas.back() != s) areas.push_back(came[size_t(areas.back())]);
    std::reverse(areas.begin(), areas.end());
    // Stretches between links: funnel each from where it starts (from / a landing) to where it ends (a take-off / to).
    std::vector<NavStep> out;
    vec3 start = from;
    std::vector<int> stretch{areas[0]};
    for (size_t i = 1; i <= areas.size(); ++i) {
        const int link = i < areas.size() ? via[size_t(areas[i])] : -1;
        if (i < areas.size() && link < 0) {
            stretch.push_back(areas[i]);
            continue;
        }
        const vec3 end = i < areas.size() ? off[size_t(areas[i])] : to;
        for (const vec3 c : funnel(start, stretch, end)) out.push_back({c});
        if (i == areas.size()) break;
        out.back().link = link / 2, out.back().reverse = (link & 1) != 0, out.back().land = at[size_t(areas[i])];
        start = at[size_t(areas[i])], stretch = {areas[i]};
    }
    return out;
}

std::vector<vec3> NavMesh::path(vec3 from, vec3 to) const {
    std::vector<vec3> out;
    for (const NavStep& c : route(from, to, 0)) out.push_back(c.at);
    return out;
}

NavStep NavMesh::steer(vec3 from, vec3 to, uint16_t usage) const {
    for (const NavStep& c : route(from, to, usage))
        if (c.link >= 0 || glm::length(glm::vec2(c.at - from)) > 10.0f) return c; // never skips a take-off
    return {to}; // off the mesh, unreachable, or there
}

} // namespace ou::game::ai
