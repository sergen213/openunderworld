// Walkability profile of a straight line over our collision (route planning for SPAWN-free chart takes): every STEP
// units from A to B, the floor under the point (ray down from 300 over the previous floor) and whether a horizontal
// ray at knee (+60) and chest (+150) height meets a wall before the next sample. Our frame. Build as markup.cpp:
//   g++ -std=c++23 -O1 -I$S/src -I$B floorline.cpp $B/libou_collision.a $B/libou_formats.a $B/libou_cdc.a -lz -o floorline
//   floorline "<game dir>" l3_chapel ax ay az bx by [step]
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 8) return 2;
    ou::cdc::BigfileSet bf(argv[1]);
    auto bytes = bf.read(std::string("pc-w\\") + argv[2] + ".drm");
    if (!bytes) return 1;
    const auto col = ou::collision::loadLevelCollision(ou::cdc::parseDrm(std::vector<uint8_t>(*bytes)));
    const float ax = std::atof(argv[3]), ay = std::atof(argv[4]), az = std::atof(argv[5]), bx = std::atof(argv[6]), by = std::atof(argv[7]);
    const float step = argc > 8 ? std::atof(argv[8]) : 25.0f;
    const float len = std::hypot(bx - ax, by - ay); const int n = int(len / step);
    const float dx = (bx - ax) / len, dy = (by - ay) / len;
    float z = az;
    for (int i = 0; i <= n; ++i) {
        const float x = ax + dx * step * i, y = ay + dy * step * i;
        auto g = col.groundProbe({x, y, z + 300}, 3000);
        if (!g) { std::printf("%6.0f %.1f %.1f  no floor within 3000 under %.1f\n", step * i, x, y, z + 300); continue; }
        const float fz = g->point.z;
        auto knee = col.raycast({x, y, fz + 60}, {dx, dy, 0}, step), chest = col.raycast({x, y, fz + 150}, {dx, dy, 0}, step);
        std::printf("%6.0f %.1f %.1f floor %.1f (%+.1f)%s%s\n", step * i, x, y, fz, fz - z, knee ? " wall@knee" : "", chest ? " wall@chest" : "");
        z = fz;
    }
}
