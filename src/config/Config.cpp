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
    } catch (const std::exception& e) {
        throw std::runtime_error(path.string() + ": " + e.what());
    }
    if (c.renderer != "opengl" && c.renderer != "vulkan") throw std::runtime_error(path.string() + ": renderer must be opengl or vulkan");
    if (c.width <= 0 || c.height <= 0 || c.fov <= 1.0f || c.fov >= 179.0f) throw std::runtime_error(path.string() + ": bad width/height/fov");
    return c;
}

void save(const fs::path& path, const Config& c) {
    nlohmann::json j = {
        {"game_dir", c.gameDir}, {"drm", c.drm}, {"renderer", c.renderer}, {"width", c.width},
        {"height", c.height}, {"fullscreen", c.fullscreen}, {"fov", c.fov}, {"vsync", c.vsync},
        {"updates", {{"enabled", c.updatesEnabled}, {"channel", c.updateChannel}, {"manifest_url", c.updateManifestUrl}}},
    };
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
