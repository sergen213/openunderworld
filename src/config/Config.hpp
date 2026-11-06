// Shared user configuration ($XDG_CONFIG_HOME/openunderworld/config.json), written by ou-launcher
// and read by ou-game/ou-viewer --config. Unknown keys are ignored; missing keys keep their defaults.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <map>
#include <vector>

namespace ou::config {

namespace fs = std::filesystem;

// Graphics preset plus per-feature overrides. "enhanced" (default) = 4x MSAA + directional sun shadows + TRU's
// translated shaders, plus ray-traced sun shadows where the Vulkan device supports ray queries (rt "auto");
// FXAA and HDR stay off because the HDR tonemap compresses the original shaders' LDR output.
// "original" = purist, as close to tru.exe as evidence allows (no MSAA, no shadow map, no post, no RT).
struct GraphicsConfig {
    std::string preset = "enhanced";       // "enhanced" | "original"
    int msaa = 0;                           // 0 = preset default, else 1, 2, 4 or 8 samples
    std::optional<bool> shadows, fxaa, hdr; // unset = preset default
    std::string rt = "auto";                // "auto" (enhanced -> shadows, original -> off) | "off" | "shadows" | "full"
                                            // (Vulkan ray queries; explicit shadows/full need the enhanced preset)
    std::string rtQuality = "medium";       // "low" | "medium" | "high": 1, 2, 4 shadow rays per pixel
    std::optional<bool> rtLights;           // RT shadows of the point / spot shadow lights (flashlight, fixtures); unset = on with rt "full"
    std::optional<bool> rtReflections;      // ray-traced water reflections (replace SSR); unset = on with rt "full"
    std::optional<bool> rtAo, rtGi;         // RT ambient occlusion / one-bounce sun GI; unset = on with rt "full"
    std::optional<bool> originalShaders;    // TRU's translated shaders (docs/formats/SHADERS.md); unset = preset default (on)
    std::string water = "auto";             // "auto" (enhanced -> enhanced, original -> original) | "original" | "enhanced"
    // Enhanced water / atmosphere / weather (docs/RENDERING.md); "auto" = on for the enhanced preset, off for original.
    std::string waterDetail = "auto";       // "auto" | "standard" | "high" (swell, flow, glitter, sun-occluded glint,
                                            // thickness-tested reflections, lamp reflections, geometry swell)
    std::string atmosphere = "auto";        // "auto" (enhanced -> medium) | "off" | "low" (height fog) | "medium"
                                            // (+ sun shafts, 24 taps) | "high" (48 taps)
    std::optional<bool> lightShafts;        // sun shafts of medium / high; unset = on
    std::optional<bool> cineDof;            // cutscene depth of field (cameras' focus; retail draws none); unset = off
    std::string weather = "auto";           // "auto" (enhanced -> the unit's own rain / snow) | "off" | "rain" | "snow"
    float brightness = 1.0f;                // 0.5..2.0, post-pass gamma (out = in^(1/brightness)); 1 = neutral
    // AMD FSR (Vulkan only, docs/RENDERING.md "FSR"): temporal upscaling from a lower render resolution (MSAA / FXAA off),
    // optionally with frame generation (one generated frame between real ones).
    std::string upscaler = "off";           // "off" | "fsr"
    std::string fsrQuality = "quality";     // "native" (AA only) | "quality" (1.5x) | "balanced" (1.7x) | "performance" (2x)
                                            // | "ultra_performance" (3x)
    float sharpness = 0.5f;                 // FSR sharpening 0..1, 0 = off
    bool frameGeneration = false;           // needs upscaler "fsr"
    bool effects = true;                    // the original particle effects (src/fx: particles, ribbons, lightning,
                                            // glows, effect lights, screen filters); false = none (sounds stay)
    bool antiLag = false;                   // AMD Anti-Lag (VK_AMD_anti_lag, Vulkan only; docs/RENDERING.md "Anti-Lag")
};
struct GraphicsSettings {
    int msaa = 1;
    bool shadows = false, fxaa = false, hdr = false;
    int rt = 0;     // 0 off, 1 shadows, 2 full (= render::RayTracing)
    int rtRays = 2;
    bool originalShaders = true; // TRU's translated shaders (docs/formats/SHADERS.md)
    int water = 0;      // render::WaterQuality: 0 original (TRU's own water shaders), 1 enhanced (ours)
    float brightness = 1.0f;
    bool waterDetail = false;
    int atmosphere = 0; // render::Atmosphere: 0 off .. 3 high
    bool lightShafts = true;
    int weather = 0;    // 0 off, 1 the unit's weather (game::unitWeather), 2 rain, 3 snow
    bool fsr = false;
    int fsrQuality = 1; // 0 native .. 4 ultra performance (= render::FrameOptions::fsrQuality)
    float sharpness = 0.5f;
    bool frameGeneration = false;
    bool effects = true;
    bool rtLights = false; // RT shadows of local shadow lights (= render::FrameOptions::rtLights); last: tests brace-init the rest
    bool rtReflections = false; // ray-traced water reflections (render::FrameOptions::rtReflections); needs rt > 0
    // Last, so positional initialisers of the fields above keep working.
    bool rtAo = false, rtGi = false; // RT ambient occlusion / sun bounce (rt on; rtAo / rtGi unset = rt full)
    bool antiLag = false; // AMD Anti-Lag (render::FrameOptions::antiLag); last, like the fields above
    bool cineDof = false; // cutscene depth of field (app::CineView::depthOfField); last, like the fields above
    bool pssm = false;    // tru.exe's sun shadows (render::FrameOptions::pssm): the original preset's `shadows`; last, as above
    bool operator==(const GraphicsSettings&) const = default;
};
// Throws std::runtime_error on an unknown preset, msaa, rt, rtQuality, upscaler or fsrQuality value, explicit rt without
// the enhanced preset, frame generation without FSR, brightness outside [0.5, 2.0] or sharpness outside [0, 1].
GraphicsSettings resolve(const GraphicsConfig& g);
// Command-line graphics flags shared by the apps: --preset, --msaa, --shadows/--fxaa/--hdr on|off, --shaders original|generic,
// --rt, --rt-quality, --rt-lights / --rt-reflections / --rt-ao / --rt-gi on|off, --water original|enhanced, --water-detail,
// --atmosphere, --shafts on|off, --weather, --brightness, --upscaler off|fsr, --fsr-quality, --sharpness, --frame-generation on|off, --fx on|off,
// --anti-lag on|off. false = not a graphics flag. Throws std::runtime_error on a bad value (on/off flags only; the rest via resolve()).
bool applyGraphicsFlag(GraphicsConfig& g, std::string_view flag, std::string_view value);

// Control bindings: action -> inputs, any of which triggers it. Input names (case-insensitive): SDL scancode
// names ("W", "Space", "Left Shift", "F5"), "Mouse Left" / "Mouse Right" / "Mouse Middle", or "Pad <button>"
// with SDL gamepad button names ("Pad a", "Pad dpright", "Pad leftshoulder", "Pad leftstick") plus
// "Pad lefttrigger" / "Pad righttrigger" (pressed past half). Sticks are fixed: left moves, right looks.
// Names are checked by ou::platform::parseInput (SDL); this layer only stores them.
struct InputConfig {
    std::map<std::string, std::vector<std::string>> bindings; // every bindableActions() entry
    float mouseSensitivity = 1.0f; // multiplier, 0.1..10
    bool invertY = false;          // mouse and right stick
    InputConfig();                 // default bindings
};
// Bindable actions in display order: forward, back, left, right, walk, sprint, jump, drop, interact, draw, fire,
// aim, switch, weapon, heal, grapple, save, load, hint, answer.
const std::vector<std::string>& bindableActions();

// In-game menu settings (ou-game GAME OPTIONS / AUDIO/VISUAL / VOLUME SETTINGS write them back to the config they were
// started with). Y inversion is InputConfig::invertY, brightness GraphicsConfig::brightness.
struct GameConfig {
    int sfxVolume = 8, musicVolume = 8, cinematicVolume = 8; // 0..10
    bool subtitles = true, reticles = true, helperButtons = true, trainingText = true;
    // Text, voice and subtitle language: a locale::languageCode of locale::kInstallLanguages (ou-game rejects others).
    std::string language = "en";
    bool operator==(const GameConfig&) const = default;
};

struct Config {
    std::string gameDir;
    std::string drm;                // start level, e.g. "pc-w\\lara.drm"; empty = placeholder scene
    std::string renderer = "auto"; // "auto" (Vulkan, else OpenGL) | "opengl" | "vulkan"
    int width = 1280, height = 720;
    bool fullscreen = false;
    float fov = 60.0f;
    bool vsync = true;
    bool updatesEnabled = true;
    std::string updateChannel = "stable";
    // Signed manifest of the newest GitHub release (manifest.json.sig next to it); empty = no update source.
    std::string updateManifestUrl = "https://github.com/sergen213/openunderworld/releases/latest/download/manifest.json";
    GraphicsConfig graphics;
    InputConfig controls;
    GameConfig game;
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
