// An object DRM's ObjectBase section and its data block (ObjectBase +0xC8): Lara's player data (lara.drm) holds her
// state charts, chart variables, attack records and tuning (Melee.cpp, Chart.cpp).
#pragma once

#include "cdc/Cdc.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace ou::game {

struct ObjData {
    cdc::Drm drm;
    uint32_t base = 0, section = 0, offset = 0; // ObjectBase section; the data block's section and offset
};
// pc-w\<object>.drm's object data; nullopt when the DRM or its ObjectBase is missing.
std::optional<ObjData> objData(const cdc::BigfileSet& bf, std::string_view object);

} // namespace ou::game
