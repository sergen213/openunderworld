// Dump a unit's intros: uid objectId x y z (our frame: parseUnit adds the level origin). Used by py/offset.py to
// measure a unit's tru-ours offset from instances (tools/wine-ref/README.md "Chart traces"). Build against a configured
// openunderworld build dir B (static libs) and its source tree S:
//   g++ -std=c++23 -O1 -I$S/src -I$B intros.cpp $B/libou_formats.a $B/libou_cdc.a -lz -o intros
//   intros "<game dir>" l3_chapel > l3_chapel.txt
#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    ou::cdc::BigfileSet bf(argv[1]);
    auto bytes = bf.read(std::string("pc-w\\") + argv[2] + ".drm");
    if (!bytes) return 1;
    auto u = ou::formats::parseUnit(ou::cdc::parseDrm(std::vector<uint8_t>(*bytes)));
    for (auto& i : u.intros) std::printf("%d %u %.2f %.2f %.2f\n", i.uniqueId, i.objectId, i.position[0], i.position[1], i.position[2]);
}
