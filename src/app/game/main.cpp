// ou-game: playable Lara in a real unit. Simulation (src/game) runs on a fixed 60 Hz tick; rendering
// interpolates between the last two ticks. Docs: docs/GAMEPLAY.md.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"
#include "config/Config.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"
#include "game/Game.hpp"
#include "platform/Sdl.hpp"
#include "render/Renderer.hpp"

namespace {

constexpr int kExitSkip = 77; // ctest SKIP_RETURN_CODE: no display available
constexpr float kMouseSensitivity = 0.0025f; // rad per pixel

struct Options {
    std::filesystem::path gameDir;
    std::string level = "pc-w\\l4_pool.drm";
    std::string renderer = "opengl";
    int width = 1280, height = 720;
    float fov = 60.0f;
    bool vsync = true, fullscreen = false, noOverlay = false, showCollision = false;
    int frames = 0; // 0 = until closed
    std::filesystem::path screenshot, inputScript;
    double renderHz = 0; // > 0: fixed virtual frame time (deterministic replays); default 60 with --input-script
    std::optional<std::array<float, 4>> spawn; // --spawn X,Y,Z,YAWDEG overrides the unit's player intro
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-game --game-dir DIR [--level pc-w\\\\l4_pool.drm] [--renderer opengl|vulkan]\n"
                 "               [--frames N] [--screenshot out.ppm] [--input-script FILE] [--render-hz HZ]\n"
                 "               [--width W --height H] [--fov DEG] [--no-vsync] [--fullscreen] [--no-overlay]\n"
                 "               [--spawn X,Y,Z,YAWDEG]\n"
                 "               [--show-collision] [--config config.json]\n"
                 "controls: WASD/arrows move (camera-relative), Shift walk, Space jump/climb, Ctrl or C drop,\n"
                 "          mouse look; gamepad: left stick, right stick, A jump, B drop, LB walk; F1 overlay, Esc quit\n"
                 "input script lines: <seconds> <forward|right|walk|jump|drop|lookx|looky> [value]\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--config") continue;
        try {
            const auto c = ou::config::load(argv[i + 1]);
            o.gameDir = c.gameDir, o.renderer = c.renderer, o.width = c.width, o.height = c.height;
            o.fov = c.fov, o.vsync = c.vsync, o.fullscreen = c.fullscreen;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "--config: %s\n", e.what());
            return false;
        }
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-vsync") { o.vsync = false; continue; }
        if (a == "--fullscreen") { o.fullscreen = true; continue; }
        if (a == "--no-overlay") { o.noOverlay = true; continue; }
        if (a == "--show-collision") { o.showCollision = true; continue; }
        if (a == "--help" || a == "-h") return false;
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
        const char* v = argv[++i];
        if (a == "--config") continue;
        if (a == "--game-dir") o.gameDir = v;
        else if (a == "--level") o.level = v;
        else if (a == "--renderer") o.renderer = v;
        else if (a == "--width") o.width = std::atoi(v);
        else if (a == "--height") o.height = std::atoi(v);
        else if (a == "--fov") o.fov = float(std::atof(v));
        else if (a == "--frames") o.frames = std::atoi(v);
        else if (a == "--screenshot") o.screenshot = v;
        else if (a == "--input-script") o.inputScript = v;
        else if (a == "--render-hz") o.renderHz = std::atof(v);
        else if (a == "--spawn") {
            std::array<float, 4> s{};
            if (std::sscanf(v, "%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3]) != 4) { std::fprintf(stderr, "--spawn wants X,Y,Z,YAWDEG\n"); return false; }
            o.spawn = s;
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    if (o.gameDir.empty()) { std::fprintf(stderr, "--game-dir is required\n"); return false; }
    if (o.renderer != "opengl" && o.renderer != "vulkan") { std::fprintf(stderr, "--renderer wants opengl or vulkan\n"); return false; }
    if (o.width <= 0 || o.height <= 0 || o.fov <= 1.0f || o.fov >= 179.0f || o.frames < 0 || o.renderHz < 0) {
        std::fprintf(stderr, "invalid --width/--height/--fov/--frames/--render-hz\n");
        return false;
    }
    if (!o.screenshot.empty() && o.frames == 0) o.frames = 1;
    if (!o.inputScript.empty() && o.renderHz == 0) o.renderHz = 60;
    return true;
}

bool writePpm(const std::filesystem::path& path, const ou::render::Image& img) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << img.width << ' ' << img.height << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.rgb.data()), std::streamsize(img.rgb.size()));
    return bool(f);
}

char buf[512];
template <class... A>
std::string fmt(const char* f, A... a) {
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

// Lara's skinned meshes: CPU-skinned in model space (root segment held at bind; its motion is already in
// the player position), then moved to the world by the player matrix.
struct LaraMeshes {
    std::vector<size_t> meshIndices; // into the uploaded scene
    std::vector<ou::scene::Vertex> scratch;

    void pose(const ou::game::World& world, const ou::game::PlayerState& s, const ou::scene::SceneData& scene,
              ou::render::Renderer& r) {
        const auto& anims = world.anims();
        const ou::anim::Clip* clip = anims.find(s.clip);
        auto local = clip ? ou::anim::sample(*clip, anims.skeleton, s.clipTime, s.loop) : ou::anim::bindPose(anims.skeleton);
        local[0] = ou::anim::bindPose(anims.skeleton)[0];
        const auto palette = ou::anim::skinPalette(anims.skeleton, ou::anim::worldMatrices(anims.skeleton, local));
        const glm::mat4 m = world.player().modelMatrix(s.pos, s.yaw);
        const glm::mat3 n(m);
        for (const size_t i : meshIndices) {
            ou::anim::skinVertices(scene.meshes[i], palette, scratch);
            for (auto& v : scratch) {
                const glm::vec3 p = glm::vec3(m * glm::vec4(v.position[0], v.position[1], v.position[2], 1));
                const glm::vec3 q = n * glm::vec3(v.normal[0], v.normal[1], v.normal[2]);
                v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
                v.normal[0] = q.x, v.normal[1] = q.y, v.normal[2] = q.z;
            }
            r.updateVertices(i, scratch);
        }
    }
};

std::vector<ou::render::DebugLine> collisionLines(const ou::collision::CollisionMesh& col) {
    std::vector<ou::render::DebugLine> out;
    auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t c) { out.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, c}); };
    for (const auto& f : col.faces)
        for (int k = 0; k < 3; ++k) line(col.vertices[f.v[k]], col.vertices[f.v[(k + 1) % 3]], (f.flags & 0x40) ? 0xFF808080 : 0xFF2020FF);
    for (const auto& m : col.markup) {
        const uint32_t c = m.type == 1 ? 0xFF00FFFF : 0xFF0080FF;
        for (size_t i = 0; i + 1 < m.points.size(); ++i) line(m.points[i], m.points[i + 1], c);
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }

    // ---- load: simulation data first (no window needed to fail early) ----
    std::optional<ou::game::InputScript> script;
    std::unique_ptr<ou::game::World> world;
    ou::scene::SceneData scene;
    LaraMeshes lara;
    std::vector<std::string> warnings;
    bool spawnFromIntro = false;
    try {
        if (!opt.inputScript.empty()) script = ou::game::InputScript::load(opt.inputScript);
        const ou::cdc::BigfileSet bigfile(opt.gameDir);
        auto bytes = bigfile.read(opt.level);
        if (!bytes) throw ou::cdc::FormatError(opt.level + " not in bigfile");
        const auto unit = ou::cdc::parseDrm(std::move(*bytes));
        auto col = ou::collision::loadLevelCollision(unit, &warnings);
        auto spawn = ou::game::findSpawn(bigfile, unit);
        if (opt.spawn) spawn = ou::game::Spawn{{(*opt.spawn)[0], (*opt.spawn)[1], (*opt.spawn)[2]}, glm::radians((*opt.spawn)[3])};
        if (!spawn) throw ou::cdc::FormatError(opt.level + ": no intro to spawn at");
        spawnFromIntro = spawn->fromIntro;
        world = std::make_unique<ou::game::World>(std::move(col), ou::game::loadLaraAnims(bigfile), *spawn);
        world->fovDeg = opt.fov;

        scene = ou::formats::loadLevelScene(bigfile, opt.level);
        auto laraScene = ou::formats::loadDrmScene(bigfile, "pc-w\\lara.drm");
        const auto skelBase = int32_t(scene.skeletons.size());
        for (auto& s : laraScene.skeletons) scene.skeletons.push_back(std::move(s));
        for (auto& m : laraScene.meshes) {
            if (m.skeleton < 0 || m.indices.empty()) continue; // Lara's skinned body only
            m.skeleton += skelBase;
            lara.meshIndices.push_back(scene.meshes.size());
            scene.meshes.push_back(std::move(m));
        }
        // Texture ids are cdc section ids, unique across DRMs: skip ones the level already loaded.
        for (auto& t : laraScene.textures)
            if (std::none_of(scene.textures.begin(), scene.textures.end(), [&](const auto& x) { return x.id == t.id; }))
                scene.textures.push_back(std::move(t));
        for (auto& w : laraScene.warnings) warnings.push_back(std::move(w));
        if (lara.meshIndices.empty()) warnings.push_back("pc-w\\lara.drm: no skinned mesh decoded");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-game: %s\n", e.what());
        return 1;
    }
    for (auto& w : scene.warnings) warnings.push_back(std::move(w));

    ou::platform::SdlInit sdl(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl.ok) {
        std::fprintf(stderr, "ou-game: SDL_Init failed (%s); no display, skipping\n", SDL_GetError());
        return kExitSkip;
    }
    ou::platform::WindowPtr window; // before the renderer: GL context / VkSurface die first
    auto renderer = opt.renderer == "vulkan" ? ou::render::createVulkanRenderer() : ou::render::createGlRenderer();
    std::string error;
    if (!renderer->preWindowSetup(error)) {
        std::fprintf(stderr, "ou-game: %s renderer unavailable: %s; skipping\n", opt.renderer.c_str(), error.c_str());
        return kExitSkip;
    }
    window.reset(SDL_CreateWindow("OpenUnderworld", opt.width, opt.height,
                                  SDL_WindowFlags(renderer->windowFlags()) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
                                      (opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0)));
    if (!window) { std::fprintf(stderr, "ou-game: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    if (!renderer->init(window.get(), error)) { std::fprintf(stderr, "ou-game: %s\n", error.c_str()); return 1; }
    renderer->setVsync(opt.vsync);
    std::fprintf(stderr, "ou-game: %s\n", renderer->description().c_str());
    for (auto& w : renderer->upload(scene)) warnings.push_back(std::move(w));
    for (const auto& w : warnings) std::fprintf(stderr, "ou-game: warning: %s\n", w.c_str());
    const auto debugLines = opt.showCollision ? collisionLines(world->collision()) : std::vector<ou::render::DebugLine>{};
    const auto& sp = world->player().state();
    std::fprintf(stderr, "ou-game: %s | spawn %s at %.1f %.1f %.1f yaw %.1f\n", opt.level.c_str(),
                 opt.spawn ? "--spawn" : spawnFromIntro ? "'player' intro" : "first intro (no 'player' intro)", sp.pos.x, sp.pos.y, sp.pos.z,
                 glm::degrees(sp.yaw));

    const bool interactive = !script;
    SDL_SetWindowRelativeMouseMode(window.get(), interactive);
    ou::platform::GamepadPtr pad;
    ou::engine::FrameClock clock(ou::game::kTickSeconds);
    glm::vec2 mouse(0);
    bool running = true, overlay = !opt.noOverlay;
    uint64_t last = SDL_GetTicksNS(), fpsStart = last;
    int frame = 0, fpsFrames = 0;
    double fps = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.key == SDLK_ESCAPE) running = false;
                else if (e.key.key == SDLK_F1) overlay = !overlay;
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (interactive) mouse += glm::vec2(e.motion.xrel, e.motion.yrel);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) pad.reset(SDL_OpenGamepad(e.gdevice.which));
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad.get()) == e.gdevice.which) pad.reset();
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: renderer->resize({e.window.data1, e.window.data2}); break;
            default: break;
            }
        }
        const uint64_t now = SDL_GetTicksNS();
        const double dt = opt.renderHz > 0 ? 1.0 / opt.renderHz : double(now - last) * 1e-9;
        last = now;

        for (int n = clock.advance(dt); n > 0; --n) {
            ou::game::Actions a = script ? script->at(world->ticks()) : ou::game::readDevices(SDL_GetKeyboardState(nullptr), pad.get());
            // Mouse motion since the last tick, as a rate over this tick.
            a.look -= mouse * kMouseSensitivity / float(ou::game::kTickSeconds);
            mouse = glm::vec2(0);
            world->tick(a);
        }

        const auto s = world->interpolated(clock.alpha());
        const auto c = world->interpolatedCamera(clock.alpha());
        lara.pose(*world, s, scene, *renderer);
        ou::render::Camera cam;
        cam.verticalFovDeg = world->fovDeg;
        cam.nearZ = 5.0f;
        cam.farZ = 200000.0f;
        cam.position = c.position;
        cam.lookAt(c.target);

        ++fpsFrames;
        if (now - fpsStart >= 500'000'000ull) {
            fps = fpsFrames / (double(now - fpsStart) * 1e-9);
            fpsStart = now;
            fpsFrames = 0;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window.get(), &w, &h);
        std::vector<std::string> lines;
        if (overlay) {
            lines.push_back(fmt("OpenUnderworld ou-game | %s | %.1f FPS | tick %llu%s%s", opt.level.c_str(), fps,
                                (unsigned long long)world->ticks(), script ? " | REPLAY " : "", pad ? " | gamepad" : ""));
            lines.push_back(renderer->description());
            lines.push_back(fmt("lara %s | clip %s t %.2f | pos %.0f %.0f %.0f | yaw %.0f", std::string(ou::game::stateName(s.state)).c_str(),
                                s.clip.c_str(), s.clipTime, s.pos.x, s.pos.y, s.pos.z, glm::degrees(s.yaw)));
            if (!warnings.empty()) lines.push_back(fmt("!%zu load warnings on stderr", warnings.size()));
            lines.push_back("WASD move | Shift walk | Space jump/climb | Ctrl/C drop | mouse look | F1 overlay | Esc quit");
        }
        renderer->drawFrame(cam, {w, h}, {true, true, lines, debugLines});
        ++frame;
        if (opt.frames && frame >= opt.frames) {
            running = false;
            std::fprintf(stderr, "ou-game: final tick %llu state %s pos %.1f %.1f %.1f yaw %.1f clip %s\n",
                         (unsigned long long)world->ticks(), std::string(ou::game::stateName(s.state)).c_str(), s.pos.x, s.pos.y,
                         s.pos.z, glm::degrees(s.yaw), s.clip.c_str());
            if (!opt.screenshot.empty()) {
                if (!writePpm(opt.screenshot, renderer->readBackbuffer({w, h}))) {
                    std::fprintf(stderr, "ou-game: cannot write %s\n", opt.screenshot.string().c_str());
                    return 1;
                }
                std::fprintf(stderr, "ou-game: wrote %s (%dx%d)\n", opt.screenshot.string().c_str(), w, h);
            }
        }
        renderer->present();
    }
    renderer->shutdown();
    return 0;
}
