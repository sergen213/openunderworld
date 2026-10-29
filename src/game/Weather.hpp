// Enhanced weather: which precipitation a unit shows, from the weather effect objects the original places in it
// (objectlist names fx_snow_* / fx_rain_* / rainfx*, docs/RENDERING.md "Weather"). The original draws these as
// its own particle effects (not reimplemented); Enhanced adds a screen-space precipitation layer for them.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace ou::cdc { struct Drm; }

namespace ou::game {

enum class Precipitation : uint8_t { None, Rain, Snow };
struct UnitWeather {
    Precipitation kind = Precipitation::None;
    float intensity = 0; // 0..1
};

// One objectlist name -> the weather it implies (None for every other object).
UnitWeather weatherOfObject(std::string_view objectName);
// The strongest weather implied by the unit's intros (`names` = parseObjectList(objectlist.txt)).
UnitWeather unitWeather(const std::map<uint16_t, std::string>& names, const cdc::Drm& unit);

} // namespace ou::game
