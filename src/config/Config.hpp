// Shared user configuration ($XDG_CONFIG_HOME/openunderworld/config.json), written by ou-launcher
// and read by ou-viewer --config. Unknown keys are ignored; missing keys keep their defaults.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ou::config {

namespace fs = std::filesystem;

// Graphics preset plus per-feature overrides. "original" = as close to tru.exe as evidence allows
// (no MSAA, no shadow map, no post); "enhanced" = 4x MSAA + directional shadow map.
struct GraphicsConfig {
    std::string preset = "original";        // "original" | "enhanced"
    int msaa = 0;                           // 0 = preset default, else 1, 2, 4 or 8 samples
    std::optional<bool> shadows, fxaa, hdr; // unset = preset default
};
struct GraphicsSettings {
    int msaa = 1;
    bool shadows = false, fxaa = false, hdr = false;
    bool operator==(const GraphicsSettings&) const = default;
};
// Throws std::runtime_error on an unknown preset or msaa value.
GraphicsSettings resolve(const GraphicsConfig& g);

struct Config {
    std::string gameDir;
    std::string drm;                // start level, e.g. "pc-w\\lara.drm"; empty = placeholder scene
    std::string renderer = "opengl"; // "opengl" | "vulkan"
    int width = 1280, height = 720;
    bool fullscreen = false;
    float fov = 60.0f;
    bool vsync = true;
    bool updatesEnabled = true;
    std::string updateChannel = "stable";
    // Signed manifest of the newest GitHub release (manifest.json.sig next to it); empty = no update source.
    std::string updateManifestUrl = "https://github.com/sergen213/openunderworld/releases/latest/download/manifest.json";
    GraphicsConfig graphics;
};

fs::path configHome(); // $XDG_CONFIG_HOME or ~/.config
fs::path dataHome();   // $XDG_DATA_HOME or ~/.local/share
fs::path stateHome();  // $XDG_STATE_HOME or ~/.local/state
fs::path defaultConfigPath(); // configHome()/openunderworld/config.json

// Missing file -> defaults. Throws std::runtime_error on unreadable/malformed JSON or bad values.
Config load(const fs::path& path);
// Atomic (write temp + rename); creates parent directories.
void save(const fs::path& path, const Config& c);

} // namespace ou::config
