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
    else if (g.rt != "off") throw std::runtime_error("rt must be off, shadows or full, got " + g.rt);
    if (s.rt && g.preset != "enhanced") throw std::runtime_error("rt needs the enhanced preset");
    if (g.rtQuality == "low") s.rtRays = 1;
    else if (g.rtQuality == "high") s.rtRays = 4;
    else if (g.rtQuality != "medium") throw std::runtime_error("rt quality must be low, medium or high, got " + g.rtQuality);
    return s;
}

bool applyGraphicsFlag(GraphicsConfig& g, std::string_view flag, std::string_view value) {
    if (flag == "--shadows" || flag == "--fxaa" || flag == "--hdr") {
        if (value != "on" && value != "off") throw std::runtime_error(std::string(flag) + " wants on or off");
        (flag == "--shadows" ? g.shadows : flag == "--fxaa" ? g.fxaa : g.hdr) = value == "on";
    } else if (flag == "--shaders") {
        if (value != "original" && value != "generic") throw std::runtime_error("--shaders wants original or generic");
        g.originalShaders = value == "original";
    } else if (flag == "--preset") g.preset = value;
    else if (flag == "--msaa") g.msaa = std::atoi(std::string(value).c_str());
    else if (flag == "--rt") g.rt = value;
    else if (flag == "--rt-quality") g.rtQuality = value;
    else return false;
    return true;
}

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
            c.graphics.rtQuality = g->value("rt_quality", c.graphics.rtQuality);
            if (g->contains("original_shaders")) c.graphics.originalShaders = g->at("original_shaders").get<bool>();
        }
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
    if (c.renderer != "opengl" && c.renderer != "vulkan") throw std::runtime_error(path.string() + ": renderer must be opengl or vulkan");
    if (c.width <= 0 || c.height <= 0 || c.fov <= 1.0f || c.fov >= 179.0f) throw std::runtime_error(path.string() + ": bad width/height/fov");
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
                               {"rt_quality", c.graphics.rtQuality}};
    for (auto [key, field] : {std::pair{"shadows", &c.graphics.shadows}, std::pair{"fxaa", &c.graphics.fxaa}, std::pair{"hdr", &c.graphics.hdr}})
        if (*field) g[key] = **field;
    if (c.graphics.originalShaders) g["original_shaders"] = *c.graphics.originalShaders;
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
