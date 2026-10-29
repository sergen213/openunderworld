#include "config/Config.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace ou::config {

namespace {
fs::path xdg(const char* var, const char* fallback) {
    if (const char* v = std::getenv(var); v && *v == '/') return v;
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/tmp") / fallback;
}
} // namespace

fs::path configHome() { return xdg("XDG_CONFIG_HOME", ".config"); }
fs::path dataHome() { return xdg("XDG_DATA_HOME", ".local/share"); }
fs::path stateHome() { return xdg("XDG_STATE_HOME", ".local/state"); }
fs::path defaultConfigPath() { return configHome() / "openunderworld" / "config.json"; }

GraphicsSettings resolve(const GraphicsConfig& g) {
    GraphicsSettings s;
    if (g.preset == "enhanced") s.msaa = 4, s.shadows = true;
    else if (g.preset != "original") throw std::runtime_error("graphics preset must be original or enhanced, got " + g.preset);
    if (g.msaa != 0 && g.msaa != 1 && g.msaa != 2 && g.msaa != 4 && g.msaa != 8)
        throw std::runtime_error("msaa must be 1, 2, 4 or 8");
    if (g.msaa) s.msaa = g.msaa;
    s.shadows = g.shadows.value_or(s.shadows);
    s.fxaa = g.fxaa.value_or(s.fxaa);
    s.hdr = g.hdr.value_or(s.hdr);
    s.originalShaders = g.originalShaders.value_or(s.originalShaders);
    if (g.rt == "shadows") s.rt = 1;
    else if (g.rt == "full") s.rt = 2;
    else if (g.rt == "auto") s.rt = g.preset == "enhanced" ? 1 : 0;
    else if (g.rt != "off") throw std::runtime_error("rt must be auto, off, shadows or full, got " + g.rt);
    if (s.rt && g.preset != "enhanced") throw std::runtime_error("rt needs the enhanced preset");
    if (g.rtQuality == "low") s.rtRays = 1;
    else if (g.rtQuality == "high") s.rtRays = 4;
    else if (g.rtQuality != "medium") throw std::runtime_error("rt quality must be low, medium or high, got " + g.rtQuality);
    if (g.water == "enhanced") s.water = 1;
    else if (g.water == "original") s.water = 0;
    else if (g.water == "auto") s.water = g.preset == "enhanced" ? 1 : 0;
    else throw std::runtime_error("water must be auto, original or enhanced, got " + g.water);
    const bool enhanced = g.preset == "enhanced";
    if (g.waterDetail == "high") s.waterDetail = true;
    else if (g.waterDetail == "auto") s.waterDetail = enhanced && s.water == 1;
    else if (g.waterDetail != "standard") throw std::runtime_error("water detail must be auto, standard or high, got " + g.waterDetail);
    if (g.atmosphere == "auto") s.atmosphere = enhanced ? 2 : 0;
    else if (g.atmosphere == "off") s.atmosphere = 0;
    else if (g.atmosphere == "low") s.atmosphere = 1;
    else if (g.atmosphere == "medium") s.atmosphere = 2;
    else if (g.atmosphere == "high") s.atmosphere = 3;
    else throw std::runtime_error("atmosphere must be auto, off, low, medium or high, got " + g.atmosphere);
    s.lightShafts = g.lightShafts.value_or(true);
    if (g.weather == "auto") s.weather = enhanced ? 1 : 0;
    else if (g.weather == "off") s.weather = 0;
    else if (g.weather == "rain") s.weather = 2;
    else if (g.weather == "snow") s.weather = 3;
    else throw std::runtime_error("weather must be auto, off, rain or snow, got " + g.weather);
    if (!(g.brightness >= 0.5f && g.brightness <= 2.0f)) throw std::runtime_error("brightness must be within 0.5..2.0");
    s.brightness = g.brightness;
    return s;
}

bool applyGraphicsFlag(GraphicsConfig& g, std::string_view flag, std::string_view value) {
    if (flag == "--shadows" || flag == "--fxaa" || flag == "--hdr" || flag == "--shafts") {
        if (value != "on" && value != "off") throw std::runtime_error(std::string(flag) + " wants on or off");
        (flag == "--shadows" ? g.shadows : flag == "--fxaa" ? g.fxaa : flag == "--hdr" ? g.hdr : g.lightShafts) = value == "on";
    } else if (flag == "--shaders") {
        if (value != "original" && value != "generic") throw std::runtime_error("--shaders wants original or generic");
        g.originalShaders = value == "original";
    } else if (flag == "--preset") g.preset = value;
    else if (flag == "--msaa") g.msaa = std::atoi(std::string(value).c_str());
    else if (flag == "--rt") g.rt = value;
    else if (flag == "--water") g.water = value;
    else if (flag == "--water-detail") g.waterDetail = value;
    else if (flag == "--atmosphere") g.atmosphere = value;
    else if (flag == "--weather") g.weather = value;
    else if (flag == "--rt-quality") g.rtQuality = value;
    else if (flag == "--brightness") g.brightness = std::strtof(std::string(value).c_str(), nullptr);
    else return false;
    return true;
}

const std::vector<std::string>& bindableActions() {
    static const std::vector<std::string> a = {"forward", "back", "left", "right", "walk", "sprint", "jump", "drop", "interact",
                                               "draw", "fire", "aim", "switch", "weapon", "heal", "grapple", "save", "load", "hint", "answer", "pda",
                                               "light", "grenade"};
    return a;
}

// The hard-wired v0.9.0 layout (docs/GAMEPLAY.md "Controls").
InputConfig::InputConfig()
    : bindings{{"forward", {"W", "Up"}},
               {"back", {"S", "Down"}},
               {"left", {"A", "Left"}},
               {"right", {"D", "Right"}},
               {"walk", {"Left Shift", "Right Shift", "Pad leftshoulder"}},
               {"sprint", {"Left Alt", "Pad leftstick"}},
               {"jump", {"Space", "Pad a"}},
               {"drop", {"Left Ctrl", "Right Ctrl", "C", "Pad b"}},
               {"interact", {"E", "Pad x"}},
               {"draw", {"F", "Pad y"}},
               {"fire", {"Mouse Left", "Pad righttrigger"}},
               {"aim", {"Mouse Right", "Pad lefttrigger"}},
               {"switch", {"Q", "Pad rightstick"}},
               {"weapon", {"X", "Pad dpright"}},
               {"heal", {"H", "Pad dpup"}},
               {"grapple", {"G", "Pad rightshoulder"}},
               {"save", {"F5"}},
               {"load", {"F9"}},
               {"hint", {"T"}},    // PDA Hint / Answer (help on demand, locals.bin 168/169); keys are ours [I]
               {"answer", {"Y"}},
               {"pda", {"1"}},     // tru.exe default: 1 opens the PDA [O]
               // tru.exe 0x487700 default keys: T = action 13, R = action 15, on pad d-pad left / down, the HUD's light /
               // grenade items [I action order]; T is taken by "hint", so the light defaults to L.
               {"light", {"L", "Pad dpleft"}},
               {"grenade", {"R", "Pad dpdown"}}} {}

Config load(const fs::path& path) {
    Config c;
    std::ifstream f(path);
    if (!f) {
        if (!fs::exists(path)) return c;
        throw std::runtime_error("cannot read " + path.string());
    }
    try {
        auto j = nlohmann::json::parse(f);
        c.gameDir = j.value("game_dir", c.gameDir);
        c.drm = j.value("drm", c.drm);
        c.renderer = j.value("renderer", c.renderer);
        c.width = j.value("width", c.width);
        c.height = j.value("height", c.height);
        c.fullscreen = j.value("fullscreen", c.fullscreen);
        c.fov = j.value("fov", c.fov);
        c.vsync = j.value("vsync", c.vsync);
        if (auto u = j.find("updates"); u != j.end()) {
            c.updatesEnabled = u->value("enabled", c.updatesEnabled);
            c.updateChannel = u->value("channel", c.updateChannel);
            c.updateManifestUrl = u->value("manifest_url", c.updateManifestUrl);
        }
        if (auto g = j.find("graphics"); g != j.end()) {
            c.graphics.preset = g->value("preset", c.graphics.preset);
            c.graphics.msaa = g->value("msaa", c.graphics.msaa);
            for (auto [key, field] : {std::pair{"shadows", &c.graphics.shadows}, std::pair{"fxaa", &c.graphics.fxaa}, std::pair{"hdr", &c.graphics.hdr}})
                if (g->contains(key)) *field = g->at(key).get<bool>();
            c.graphics.rt = g->value("rt", c.graphics.rt);
            c.graphics.water = g->value("water", c.graphics.water);
            c.graphics.waterDetail = g->value("water_detail", c.graphics.waterDetail);
            c.graphics.atmosphere = g->value("atmosphere", c.graphics.atmosphere);
            c.graphics.weather = g->value("weather", c.graphics.weather);
            if (g->contains("light_shafts")) c.graphics.lightShafts = g->at("light_shafts").get<bool>();
            c.graphics.rtQuality = g->value("rt_quality", c.graphics.rtQuality);
            if (g->contains("original_shaders")) c.graphics.originalShaders = g->at("original_shaders").get<bool>();
            c.graphics.brightness = g->value("brightness", c.graphics.brightness);
        }
        if (auto in = j.find("controls"); in != j.end()) {
            c.controls.mouseSensitivity = in->value("mouse_sensitivity", c.controls.mouseSensitivity);
            c.controls.invertY = in->value("invert_y", c.controls.invertY);
            if (auto b = in->find("bindings"); b != in->end())
                for (const auto& [action, inputs] : b->items()) { // missing actions keep their defaults
                    if (!c.controls.bindings.contains(action)) throw std::runtime_error("unknown control action " + action);
                    c.controls.bindings[action] = inputs.get<std::vector<std::string>>();
                }
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
    if (c.renderer != "auto" && c.renderer != "opengl" && c.renderer != "vulkan")
        throw std::runtime_error(path.string() + ": renderer must be auto, opengl or vulkan");
    if (c.width <= 0 || c.height <= 0 || c.fov <= 1.0f || c.fov >= 179.0f) throw std::runtime_error(path.string() + ": bad width/height/fov");
    if (!(c.controls.mouseSensitivity >= 0.1f && c.controls.mouseSensitivity <= 10.0f))
        throw std::runtime_error(path.string() + ": controls.mouse_sensitivity must be 0.1..10");
    try {
        resolve(c.graphics);
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
    return c;
}

void save(const fs::path& path, const Config& c) {
    nlohmann::json j = {
        {"game_dir", c.gameDir}, {"drm", c.drm}, {"renderer", c.renderer}, {"width", c.width},
        {"height", c.height}, {"fullscreen", c.fullscreen}, {"fov", c.fov}, {"vsync", c.vsync},
        {"updates", {{"enabled", c.updatesEnabled}, {"channel", c.updateChannel}, {"manifest_url", c.updateManifestUrl}}},
    };
    auto& g = j["graphics"] = {{"preset", c.graphics.preset}, {"msaa", c.graphics.msaa}, {"rt", c.graphics.rt},
                               {"water", c.graphics.water}, {"water_detail", c.graphics.waterDetail},
                               {"atmosphere", c.graphics.atmosphere}, {"weather", c.graphics.weather},
                               {"rt_quality", c.graphics.rtQuality}, {"brightness", c.graphics.brightness}};
    for (auto [key, field] : {std::pair{"shadows", &c.graphics.shadows}, std::pair{"fxaa", &c.graphics.fxaa}, std::pair{"hdr", &c.graphics.hdr}})
        if (*field) g[key] = **field;
    if (c.graphics.originalShaders) g["original_shaders"] = *c.graphics.originalShaders;
    if (c.graphics.lightShafts) g["light_shafts"] = *c.graphics.lightShafts;
    j["controls"] = {{"mouse_sensitivity", c.controls.mouseSensitivity}, {"invert_y", c.controls.invertY},
                     {"bindings", c.controls.bindings}};
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        f << j.dump(2) << '\n';
        if (!f.flush()) throw std::runtime_error("cannot write " + tmp.string());
    }
    fs::rename(tmp, path);
}

} // namespace ou::config
