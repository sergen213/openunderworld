// Dump a unit's collision markup in our frame: `id type typename x0,y0,z0 x1,y1,z1 ...` (one line per markup), to find
// ladders (type 9), wall-hug shelves and ledges for chart sessions; tru = ours + d (py/offset.py). Build as intros.cpp:
//   g++ -std=c++23 -O1 -I$S/src -I$B markup.cpp $B/libou_collision.a $B/libou_formats.a $B/libou_cdc.a -lz -o markup
//   markup "<game dir>" l1_ship_sinking_hall [type]
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"
#include <cstdio>
#include <cstdlib>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const int want = argc > 3 ? std::atoi(argv[3]) : -1;
    ou::cdc::BigfileSet bf(argv[1]);
    auto bytes = bf.read(std::string("pc-w\\") + argv[2] + ".drm");
    if (!bytes) return 1;
    const auto col = ou::collision::loadLevelCollision(ou::cdc::parseDrm(std::vector<uint8_t>(*bytes)));
    for (const auto& m : col.markup) {
        if (want >= 0 && m.type != want) continue;
        std::printf("%u %u %s", m.id, m.type, std::string(ou::collision::markupTypeName(m.type)).c_str());
        for (const auto& p : m.points) std::printf(" %.1f,%.1f,%.1f", p.x, p.y, p.z);
        std::printf("\n");
    }
}
