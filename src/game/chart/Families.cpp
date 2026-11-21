// The ported chart families: one register function per src/game/chart/<Family>.cpp, one line each here.
#include "game/Chart.hpp"

namespace ou::game::chart {

void registerGeneric(Registry& player, Registry& upper);
void registerGround(Registry& player, Registry& upper);
void registerJump(Registry& player, Registry& upper);
void registerLedge(Registry& player, Registry& upper);
void registerClimb(Registry& player, Registry& upper);

void registerFamilies(Registry& player, Registry& upper) {
    registerGeneric(player, upper);
    registerGround(player, upper);
    registerJump(player, upper);
    registerLedge(player, upper);
    registerClimb(player, upper);
}

} // namespace ou::game::chart
