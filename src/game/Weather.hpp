// Enhanced weather: which precipitation a unit shows, from the weather effect objects the original places in it
// (objectlist names fx_snow_* / fx_rain_* / rainfx*, docs/RENDERING.md "Weather"). The original draws these as
// its own particle effects (not reimplemented); Enhanced adds a screen-space precipitation layer for them.
#pragma once

#include <glm/vec3.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cdc { struct Drm; }

namespace ou::game {

enum class Precipitation : uint8_t { None, Rain, Snow };
// One placed weather object: its intro position (unit frame) and the column its particles cover, from the object's
// particle definitions (weatherExtent): `radius` around it horizontally, from `radius` above down to `depth` below.
struct WeatherSource {
    std::string object;
    glm::vec3 position{0};
    float radius = 0, depth = 0; // 0 = not measured: covers the whole unit
};
struct UnitWeather {
    Precipitation kind = Precipitation::None;
    float intensity = 0; // 0..1
    std::vector<WeatherSource> sources; // the unit's objects of `kind`
};

// One objectlist name -> the weather it implies (None for every other object).
UnitWeather weatherOfObject(std::string_view objectName);
// The strongest weather implied by the unit's intros (`names` = parseObjectList(objectlist.txt)).
UnitWeather unitWeather(const std::map<uint16_t, std::string>& names, const cdc::Drm& unit);
// 0..1: how much of `w` falls at `p` (unit frame): 1 inside a source's column, easing to 0 over its outer fifth; 1
// everywhere when a source is not measured. tru.exe draws the weather only as these objects' particles (the
// fx_snow_falling_* columns under Valaskjalf's broken roof), so Enhanced shows it only where they fall.
float weatherCoverage(const UnitWeather& w, glm::vec3 p);

} // namespace ou::game
