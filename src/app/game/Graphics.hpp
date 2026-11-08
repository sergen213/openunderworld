// AUDIO/VISUAL > GRAPHICS (ui::GraphicsOptions) <-> config: the launcher's graphics and display settings, which ou-game
// applies live and writes back to its --config file.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "config/Config.hpp"
#include "ui/Ui.hpp"

namespace ou::app {

// The display keys of config::Config.
struct Display {
    std::string renderer = "auto";
    int width = 1280, height = 720;
    bool fullscreen = false, vsync = true;
    float fov = 60.0f;
};

namespace graphics_detail {
// Menu index order of each config string (ui::GraphicsOptions field comments).
inline constexpr std::array<std::string_view, 3> kRenderers{"auto", "vulkan", "opengl"};
inline constexpr std::array<std::string_view, 2> kPresets{"enhanced", "original"};
inline constexpr std::array<std::string_view, 4> kRt{"auto", "off", "shadows", "full"};
inline constexpr std::array<std::string_view, 3> kRtQuality{"low", "medium", "high"};
inline constexpr std::array<int, 5> kMsaa{0, 1, 2, 4, 8};
inline constexpr std::array<std::string_view, 3> kWater{"auto", "original", "enhanced"};
inline constexpr std::array<std::string_view, 3> kWaterDetail{"auto", "standard", "high"};
inline constexpr std::array<std::string_view, 5> kAtmosphere{"auto", "off", "low", "medium", "high"};
inline constexpr std::array<std::string_view, 4> kWeather{"auto", "off", "rain", "snow"};
inline constexpr std::array<std::string_view, 5> kFsrQuality{"native", "quality", "balanced", "performance", "ultra_performance"};
inline constexpr std::array<std::string_view, 2> kTonemap{"original", "agx"};

int indexOf(const auto& values, const auto& v) { // unknown: 0
    const auto it = std::find(values.begin(), values.end(), v);
    return it == values.end() ? 0 : int(it - values.begin());
}
template <class T, size_t N> T at(const std::array<T, N>& values, int i) { return values[size_t(std::clamp(i, 0, int(N) - 1))]; }
inline int toTri(std::optional<bool> v) { return !v ? 0 : *v ? 1 : 2; } // 0 default, 1 on, 2 off
inline std::optional<bool> fromTri(int v) { return v == 1 ? std::optional(true) : v == 2 ? std::optional(false) : std::nullopt; }
inline int fovStep(float fov) { return std::clamp(int(std::lround(fov)), 30, 120); }
inline int sharpnessStep(float s) { return std::clamp(int(std::lround(s * 10)), 0, 10); }
} // namespace graphics_detail

// Window sizes offered: the launcher's presets, the desktop and the current size, sorted, no duplicates.
inline std::vector<std::pair<int, int>> resolutionChoices(std::pair<int, int> desktop, std::pair<int, int> current) {
    std::vector<std::pair<int, int>> r{{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160}, current};
    if (desktop.first > 0 && desktop.second > 0) r.push_back(desktop);
    std::sort(r.begin(), r.end());
    r.erase(std::unique(r.begin(), r.end()), r.end());
    return r;
}

// Menu values of a graphics config + display. rtSupported / fsrSupported / antiLagSupported stay true (the app sets them from the renderer).
inline ui::GraphicsOptions graphicsMenu(const config::GraphicsConfig& g, const Display& d, std::pair<int, int> desktop) {
    using namespace graphics_detail;
    ui::GraphicsOptions m;
    m.renderer = indexOf(kRenderers, d.renderer);
    m.resolutions = resolutionChoices(desktop, {d.width, d.height});
    m.resolution = indexOf(m.resolutions, std::pair(d.width, d.height));
    m.fullscreen = d.fullscreen, m.vsync = d.vsync, m.fov = fovStep(d.fov);
    m.preset = indexOf(kPresets, g.preset), m.rt = indexOf(kRt, g.rt), m.rtQuality = indexOf(kRtQuality, g.rtQuality);
    m.rtLights = toTri(g.rtLights), m.rtReflections = toTri(g.rtReflections), m.rtAo = toTri(g.rtAo), m.rtGi = toTri(g.rtGi);
    m.msaa = indexOf(kMsaa, g.msaa), m.shadows = toTri(g.shadows), m.originalShaders = g.originalShaders.value_or(true);
    m.water = indexOf(kWater, g.water), m.waterDetail = indexOf(kWaterDetail, g.waterDetail);
    m.atmosphere = indexOf(kAtmosphere, g.atmosphere), m.lightShafts = toTri(g.lightShafts), m.cineDof = toTri(g.cineDof), m.weather = indexOf(kWeather, g.weather);
    m.fsr = g.upscaler == "fsr", m.fsrQuality = indexOf(kFsrQuality, g.fsrQuality), m.sharpness = sharpnessStep(g.sharpness);
    m.frameGeneration = g.frameGeneration, m.effects = g.effects, m.antiLag = g.antiLag, m.tonemap = indexOf(kTonemap, g.tonemap);
    return m;
}

// Menu values back into the config + display. Keys the menu does not show (brightness, fxaa, hdr) stay; finer launcher
// values (fov 62.5, sharpness 0.33) stay while their slider step is unchanged. Sanitized so config::resolve() accepts it:
// explicit RT with the original preset -> "auto" (off), frame generation needs FSR.
inline void applyGraphicsMenu(const ui::GraphicsOptions& m, config::GraphicsConfig& g, Display& d) {
    using namespace graphics_detail;
    d.renderer = at(kRenderers, m.renderer);
    if (m.resolution >= 0 && m.resolution < int(m.resolutions.size())) std::tie(d.width, d.height) = m.resolutions[size_t(m.resolution)];
    d.fullscreen = m.fullscreen, d.vsync = m.vsync;
    if (m.fov != fovStep(d.fov)) d.fov = float(std::clamp(m.fov, 30, 120));
    g.preset = at(kPresets, m.preset);
    g.rt = at(kRt, m.rt);
    if (g.preset == "original" && (g.rt == "shadows" || g.rt == "full")) g.rt = "auto";
    g.rtQuality = at(kRtQuality, m.rtQuality);
    g.rtLights = fromTri(m.rtLights), g.rtReflections = fromTri(m.rtReflections), g.rtAo = fromTri(m.rtAo), g.rtGi = fromTri(m.rtGi);
    g.msaa = at(kMsaa, m.msaa), g.shadows = fromTri(m.shadows);
    g.originalShaders = m.originalShaders ? std::nullopt : std::optional(false); // unset = original (as the launcher writes it)
    g.water = at(kWater, m.water), g.waterDetail = at(kWaterDetail, m.waterDetail);
    g.atmosphere = at(kAtmosphere, m.atmosphere), g.lightShafts = fromTri(m.lightShafts), g.cineDof = fromTri(m.cineDof), g.weather = at(kWeather, m.weather);
    g.upscaler = m.fsr ? "fsr" : "off", g.fsrQuality = at(kFsrQuality, m.fsrQuality);
    g.tonemap = at(kTonemap, m.tonemap);
    if (m.sharpness != sharpnessStep(g.sharpness)) g.sharpness = float(std::clamp(m.sharpness, 0, 10)) / 10;
    g.frameGeneration = m.fsr && m.frameGeneration, g.effects = m.effects, g.antiLag = m.antiLag;
}

} // namespace ou::app
