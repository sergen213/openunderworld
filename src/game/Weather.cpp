#include "game/Weather.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

namespace ou::game {

// Survey of all units: docs/RENDERING.md "Weather". Open-sky precipitation objects only; rain sheets and gutters
// (water pouring through openings, fx_rain_sheet_* / fx_rain_gutter_*) and cinematic-only effects are local
// effects of the original and imply no weather. Intensities are ours [I].
UnitWeather weatherOfObject(std::string_view n) {
    if (n.starts_with("fx_snow_blizzard") || n.starts_with("fx_snow_white_out")) return {Precipitation::Snow, 1.0f};
    if (n.starts_with("fx_snow_falling")) return {Precipitation::Snow, 0.55f};
    if (n == "rainfx" || n == "rainfx_mex") return {Precipitation::Rain, 0.8f};
    return {};
}

UnitWeather unitWeather(const std::map<uint16_t, std::string>& names, const cdc::Drm& unit) {
    UnitWeather best;
    for (const auto& in : formats::parseUnit(unit).intros)
        if (const auto n = names.find(in.objectId); n != names.end())
            if (const UnitWeather w = weatherOfObject(n->second); w.intensity > best.intensity) best = w;
    return best;
}

} // namespace ou::game
