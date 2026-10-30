#include "game/Weather.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>

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
    std::vector<std::pair<Precipitation, WeatherSource>> all;
    for (const auto& in : formats::parseUnit(unit).intros)
        if (const auto n = names.find(in.objectId); n != names.end())
            if (const UnitWeather w = weatherOfObject(n->second); w.kind != Precipitation::None) {
                all.push_back({w.kind, {n->second, {in.position[0], in.position[1], in.position[2]}}});
                if (w.intensity > best.intensity) best = w;
            }
    for (auto& [kind, source] : all)
        if (kind == best.kind) best.sources.push_back(std::move(source));
    return best;
}

float weatherCoverage(const UnitWeather& w, glm::vec3 p) {
    float best = 0;
    for (const auto& s : w.sources) {
        if (s.radius <= 0) return 1;
        if (p.z > s.position.z + s.radius || p.z < s.position.z - s.depth) continue;
        const float d = std::hypot(p.x - s.position.x, p.y - s.position.y);
        best = std::max(best, std::clamp((s.radius - d) / (0.2f * s.radius), 0.0f, 1.0f));
    }
    return best;
}

} // namespace ou::game
