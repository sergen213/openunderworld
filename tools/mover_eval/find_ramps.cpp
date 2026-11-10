#include "collision/Collision.hpp"
#include "cdc/Cdc.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
using namespace ou; using collision::vec3;
// Largest face per normal-z bin (0.02 wide, 0.50..0.90) of a unit with open space above its centroid: ramp series for
// measuring tru.exe's walkable slope limit. Prints our coordinates and tru's (ours + offset).
int main(int argc, char** argv) {
    const cdc::BigfileSet bf(argv[1]);
    const vec3 off(atof(argv[3]), atof(argv[4]), atof(argv[5]));
    auto col = collision::loadLevelCollision(cdc::parseDrm(*bf.read(std::string("pc-w\\") + argv[2] + ".drm")));
    struct Best { float area = 0; vec3 c{}, n{}; };
    std::map<int, Best> bins;
    for (uint32_t i = 0; i < col.faces.size(); ++i) {
        const auto& f = col.faces[i];
        if (!collision::queryable(f, 0xFF)) continue;
        const vec3 a = col.vertices[f.v[0]], b = col.vertices[f.v[1]], c = col.vertices[f.v[2]];
        vec3 n = glm::cross(b - a, c - a);
        const float area = 0.5f * glm::length(n);
        if (area < 20000) continue;
        n = glm::normalize(n);
        if (n.z < 0) n = -n;
        if (n.z < 0.5f || n.z > 0.9f) continue;
        const vec3 cen = (a + b + c) / 3.0f;
        // inscribed-ish: the centroid must be 120 from every edge
        const auto edge = [&](vec3 p, vec3 q) { const vec3 d = glm::normalize(q - p); return glm::length((cen - p) - d * glm::dot(cen - p, d)); };
        if (std::min({edge(a, b), edge(b, c), edge(c, a)}) < 110) continue;
        if (col.raycast(cen + n * 5.0f, vec3(0, 0, 1), 700)) continue;         // roofed
        const auto g = col.groundProbe(cen + vec3(0, 0, 300), 320);
        if (!g || std::abs(g->point.z - cen.z) > 2) continue;                  // not the top surface
        const int bin = int(n.z * 50);
        if (area > bins[bin].area) bins[bin] = {area, cen, n};
    }
    for (const auto& [bin, b] : bins)
        std::printf("nz %.3f area %.0f ours %.1f %.1f %.1f tru %.2f %.2f %.2f downhill yaw %.3f\n", b.n.z, b.area, b.c.x, b.c.y, b.c.z, b.c.x + off.x, b.c.y + off.y,
                    b.c.z + off.z, std::atan2(b.n.y, b.n.x));
}
