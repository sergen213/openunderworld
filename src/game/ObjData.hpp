// An object DRM's ObjectBase section and its data block (ObjectBase +0xC8): Lara's player data (lara.drm) holds her
// state charts, chart variables, attack records and tuning (Melee.cpp, Chart.cpp).
#pragma once

#include "cdc/Cdc.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::game {

struct ObjData {
    cdc::Drm drm;
    uint32_t base = 0, section = 0, offset = 0; // ObjectBase section; the data block's section and offset
};
// pc-w\<object>.drm's object data; nullopt when the DRM or its ObjectBase is missing.
std::optional<ObjData> objData(const cdc::BigfileSet& bf, std::string_view object);
// An object's clip command events (anim list entry's event DTP, records with info & 3 == 2; tru.exe 0x716F30 runs
// them): {command, parameter, frame (30 Hz)}, and the clip's animation id. clipEvents: by clip name (Melee.cpp).
struct ClipEvents {
    uint32_t animId = 0;
    std::vector<std::array<int, 3>> commands; // {command, parameter, frame}
};
std::map<std::string, ClipEvents, std::less<>> clipEvents(const ObjData& o);

} // namespace ou::game
