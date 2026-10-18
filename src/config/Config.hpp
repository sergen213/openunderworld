// Shared user configuration ($XDG_CONFIG_HOME/openunderworld/config.json), written by ou-launcher
// and read by ou-viewer --config. Unknown keys are ignored; missing keys keep their defaults.
#pragma once

#include <filesystem>
#include <string>

namespace ou::config {

namespace fs = std::filesystem;

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
    std::string updateManifestUrl;  // empty = no update source configured
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
