// ou-viewer: SDL3 window + debug fly camera over a renderer backend.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <array>
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
#include "platform/Sdl.hpp"
#include "render/Renderer.hpp"
#include "scene/Placeholder.hpp"

namespace {

constexpr int kExitSkip = 77; // ctest SKIP_RETURN_CODE: no display available

struct Options {
    std::filesystem::path gameDir;
    std::string drm;
    bool level = false; // --level: load --drm as a whole unit (formats::loadLevelScene)
    int width = 1280, height = 720;
    float fov = 60.0f;
    int frames = 0; // 0 = run until closed
    std::filesystem::path screenshot;
    bool vsync = true;
    std::optional<std::array<float, 5>> cam; // x y z yawDeg pitchDeg
    std::string renderer = ou::config::Config{}.renderer; // auto | opengl | vulkan
    bool noOverlay = false;
    bool fullscreen = false;
    std::string anim;      // --anim: anim id, #listIndex or name (clips of --drm)
    bool animList = false; // --anim-list: print the object's animations and exit
    double animTime = 0.0; // --anim-time: start time in seconds
    bool showCollision = false; // --show-collision: collision wireframe + markup of --drm (unit DRMs)
    ou::config::GraphicsConfig graphics; // --preset/--msaa/--shadows/--fxaa/--hdr (or --config)
    ou::config::GraphicsSettings gfx;    // resolved
    int waterDebug = 0; // --water-debug: Enhanced water debug view (render::kWaterDebugViews)
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-viewer [--game-dir DIR] [--drm pc-w\\\\x.drm] [--width W --height H] [--fov DEG]\n"
                 "                 [--frames N] [--screenshot out.ppm] [--no-vsync] [--cam X,Y,Z,YAW,PITCH] [--level]\n"
                 "                 [--renderer auto|opengl|vulkan] [--no-overlay] [--fullscreen]\n"
                 "                 [--anim ID|#INDEX|NAME] [--anim-time SECONDS] [--anim-list] [--show-collision]\n"
                 "                 [--preset original|enhanced] [--msaa 1|2|4|8] [--shadows on|off] [--fxaa on|off]\n"
                 "                 [--hdr on|off] [--shaders original|generic] [--rt auto|off|shadows|full] [--rt-quality low|medium|high]\n"
                 "                 [--water original|enhanced] [--water-detail auto|standard|high] [--water-debug 0-5]\n"
                 "                 [--atmosphere auto|off|low|medium|high] [--shafts on|off] [--weather off|rain|snow]\n"
                 "                 [--brightness 0.5..2.0] [--config config.json]  (launcher settings; later flags override)\n"
                 "controls: hold RMB to mouse look, WASD move, Q/E down/up, Shift fast, wheel speed,\n"
                 "          gamepad sticks + triggers, V vsync, L lighting, C backface cull, F1 overlay, Esc quit\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    // --config first, wherever it appears, so explicit flags override it.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--config") continue;
        try {
            auto c = ou::config::load(argv[i + 1]);
            o.gameDir = c.gameDir, o.drm = c.drm, o.renderer = c.renderer, o.width = c.width, o.height = c.height;
            o.fov = c.fov, o.vsync = c.vsync, o.fullscreen = c.fullscreen, o.graphics = c.graphics;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "--config: %s\n", e.what());
            return false;
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--no-vsync") { o.vsync = false; continue; }
        if (a == "--no-overlay") { o.noOverlay = true; continue; }
        if (a == "--fullscreen") { o.fullscreen = true; continue; }
        if (a == "--level") { o.level = true; continue; }
        if (a == "--anim-list") { o.animList = true; continue; }
        if (a == "--show-collision") { o.showCollision = true; continue; }
        if (a == "--help" || a == "-h") return false;
        if (!(v = next())) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
        if (a == "--config") continue; // applied above
        try {
            if (ou::config::applyGraphicsFlag(o.graphics, a, v)) continue;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return false;
        }
        if (a == "--game-dir") o.gameDir = v;
        else if (a == "--drm") o.drm = v;
        else if (a == "--renderer") o.renderer = v;
        else if (a == "--width") o.width = std::atoi(v);
        else if (a == "--height") o.height = std::atoi(v);
        else if (a == "--fov") o.fov = float(std::atof(v));
        else if (a == "--frames") o.frames = std::atoi(v);
        else if (a == "--water-debug") o.waterDebug = std::atoi(v);
        else if (a == "--screenshot") o.screenshot = v;
        else if (a == "--anim") o.anim = v;
        else if (a == "--anim-time") o.animTime = std::atof(v);
        else if (a == "--cam") {
            std::array<float, 5> c{};
            if (std::sscanf(v, "%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4]) != 5) {
                std::fprintf(stderr, "--cam wants X,Y,Z,YAW,PITCH\n");
                return false;
            }
            o.cam = c;
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    if (o.renderer != "auto" && o.renderer != "opengl" && o.renderer != "vulkan") {
        std::fprintf(stderr, "--renderer wants auto, opengl or vulkan\n");
        return false;
    }
    if (o.width <= 0 || o.height <= 0 || o.fov <= 1.0f || o.fov >= 179.0f || o.frames < 0) {
        std::fprintf(stderr, "invalid --width/--height/--fov/--frames\n");
        return false;
    }
    if (!o.screenshot.empty() && o.frames == 0) o.frames = 1;
    try {
        o.gfx = ou::config::resolve(o.graphics);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
        return false;
    }
    return true;
}

// Animation clips of --drm (object DRMs such as pc-w\lara.drm). --anim-list prints the object's list;
// --anim selects one clip. Returns false after printing a message when nothing usable was selected.
bool loadClip(const Options& o, std::optional<ou::anim::Clip>& clip, std::string& label) {
    const ou::cdc::BigfileSet bigfile(o.gameDir);
    auto bytes = bigfile.read(o.drm);
    if (!bytes) { std::fprintf(stderr, "ou-viewer: %s not in bigfile\n", o.drm.c_str()); return false; }
    const ou::cdc::Drm drm = ou::cdc::parseDrm(std::move(*bytes));
    const auto list = ou::anim::animList(drm);
    auto section = [&](uint32_t id) -> const ou::cdc::Section* {
        for (const auto& s : drm.sections)
            if (s.type == ou::cdc::SectionType::Animation && s.id == id) return &s;
        return nullptr;
    };
    if (o.animList) {
        std::printf("index\tid\tkeys\tseconds\tname\n");
        for (size_t i = 0; i < list.size(); ++i) {
            const auto* s = section(list[i].animId);
            try {
                const auto c = s ? ou::anim::decodeClip(s->data) : ou::anim::Clip{};
                std::printf("#%zu\t%u\t%u\t%.2f\t%s\n", i, list[i].animId, c.keyCount, c.durationSeconds(), list[i].name.c_str());
            } catch (const std::exception& e) {
                std::printf("#%zu\t%u\t-\t-\t%s (%s)\n", i, list[i].animId, list[i].name.c_str(), e.what());
            }
        }
        return true;
    }
    uint32_t id = 0;
    std::string name;
    if (o.anim[0] == '#') {
        const size_t i = size_t(std::atoll(o.anim.c_str() + 1));
        if (i >= list.size()) { std::fprintf(stderr, "ou-viewer: --anim %s: list has %zu entries\n", o.anim.c_str(), list.size()); return false; }
        id = list[i].animId, name = list[i].name;
    } else if (o.anim.find_first_not_of("0123456789") == std::string::npos) {
        id = uint32_t(std::atol(o.anim.c_str()));
        for (const auto& e : list) if (e.animId == id) { name = e.name; break; }
    } else {
        for (const auto& e : list) if (e.name == o.anim) { id = e.animId, name = e.name; break; }
    }
    const auto* s = section(id);
    if (!s) { std::fprintf(stderr, "ou-viewer: --anim %s: no such animation in %s\n", o.anim.c_str(), o.drm.c_str()); return false; }
    clip = ou::anim::decodeClip(s->data);
    label = "anim " + std::to_string(id) + (name.empty() ? "" : " '" + name + "'");
    return true;
}

// Real content when possible; otherwise a clearly labelled placeholder. `source` describes what is shown.
ou::scene::SceneData loadScene(const Options& o, std::string& source) {
    if (o.drm.empty()) {
        source = "PLACEHOLDER SCENE (no --drm given; synthetic checker cubes, no game data)";
        return ou::scene::makePlaceholderScene();
    }
    std::string failure;
    if (o.gameDir.empty()) {
        failure = "--drm needs --game-dir";
    } else {
        try {
            const ou::cdc::BigfileSet bigfile(o.gameDir);
            ou::scene::SceneData s = o.level ? ou::formats::loadLevelScene(bigfile, o.drm) : ou::formats::loadDrmScene(bigfile, o.drm);
            if (!s.meshes.empty()) {
                source = o.drm + " (original game assets" + (s.placeholder ? ", partly placeholder)" : ")");
                return s;
            }
            if (!s.textures.empty()) {
                s.meshes = ou::scene::makeTextureGallery(s.textures);
                s.placeholder = true;
                source = "REAL TEXTURES from " + o.drm + " on PLACEHOLDER quads (no meshes decoded)";
                return s;
            }
            failure = "no meshes or textures decoded";
            for (auto& w : s.warnings) std::fprintf(stderr, "ou-viewer: warning: %s\n", w.c_str());
        } catch (const std::exception& e) {
            failure = e.what();
        }
    }
    ou::scene::SceneData s = ou::scene::makePlaceholderScene();
    s.warnings.push_back("loading " + o.drm + " failed: " + failure);
    source = "PLACEHOLDER SCENE (loading " + o.drm + " failed)";
    return s;
}

bool writePpm(const std::filesystem::path& path, const ou::render::Image& img) {
    std::ofstream f(path, std::ios::binary);
    f << "P6\n" << img.width << ' ' << img.height << "\n255\n";
    f.write(reinterpret_cast<const char*>(img.rgb.data()), std::streamsize(img.rgb.size()));
    return bool(f);
}

// CPU-skins every mesh that has a skeleton with `clip` at `seconds` (looping) and uploads the vertices.
// Played in place: segment 0 carries root motion (ng_run moves it ~800 units per cycle) and is reset to
// bind. Returns the number of meshes posed.
size_t poseSkinnedMeshes(const ou::anim::Clip& clip, double seconds, const ou::scene::SceneData& scene,
                         ou::render::Renderer& renderer, std::vector<ou::scene::Vertex>& scratch) {
    std::vector<std::vector<glm::mat4>> palettes(scene.skeletons.size());
    size_t posed = 0;
    for (size_t i = 0; i < scene.meshes.size(); ++i) {
        const auto& m = scene.meshes[i];
        if (m.skin.empty() || m.skeleton < 0 || size_t(m.skeleton) >= scene.skeletons.size()) continue;
        const auto& sk = scene.skeletons[size_t(m.skeleton)];
        auto& palette = palettes[size_t(m.skeleton)];
        if (palette.empty()) {
            auto local = ou::anim::sample(clip, sk, seconds, true);
            local[0] = ou::anim::bindPose(sk)[0];
            palette = ou::anim::skinPalette(sk, ou::anim::worldMatrices(sk, local));
        }
        ou::anim::skinVertices(m, palette, scratch);
        posed += renderer.updateVertices(i, scratch);
    }
    return posed;
}

float deadzone(float v) { return std::abs(v) < 0.15f ? 0.0f : (v - std::copysign(0.15f, v)) / 0.85f; }
float axis(SDL_Gamepad* g, SDL_GamepadAxis a) { return g ? deadzone(SDL_GetGamepadAxis(g, a) / 32767.0f) : 0.0f; }

char buf[512];
template <class... A>
std::string fmt(const char* f, A... a) {
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

// Collision wireframe (red: terrain group 0, magenta: other groups, grey: faces tru.exe queries skip)
// and markup polylines (yellow: ledgegrab, cyan: balance beam, orange: other) with point ticks.
std::vector<ou::render::DebugLine> collisionLines(const Options& o, std::string& summary, std::vector<std::string>& warnings) {
    std::vector<ou::render::DebugLine> out;
    try {
        const ou::cdc::BigfileSet bigfile(o.gameDir);
        auto bytes = bigfile.read(o.drm);
        if (!bytes) throw ou::cdc::FormatError("not in bigfile");
        const auto col = ou::collision::loadLevelCollision(ou::cdc::parseDrm(std::move(*bytes)), &warnings);
        auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t c) { out.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, c}); };
        for (const auto& f : col.faces) {
            const uint32_t c = (f.flags & 0x40) ? 0xFF808080 : f.group ? 0xFFFF00FF : 0xFF2020FF;
            for (int k = 0; k < 3; ++k) line(col.vertices[f.v[k]], col.vertices[f.v[(k + 1) % 3]], c);
        }
        for (const auto& m : col.markup) {
            const uint32_t c = m.type == 1 ? 0xFF00FFFF : m.type == 14 ? 0xFFFFFF00 : 0xFF0080FF;
            for (size_t i = 0; i + 1 < m.points.size(); ++i) line(m.points[i], m.points[i + 1], c);
            for (const auto& p : m.points) line(p, p + glm::vec3(0, 0, 40), c);
        }
        summary = fmt("collision: %zu faces, %zu markup polylines", col.faces.size(), col.markup.size());
    } catch (const std::exception& e) {
        warnings.push_back("--show-collision: " + o.drm + ": " + e.what());
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }

    std::optional<ou::anim::Clip> clip;
    std::string clipLabel;
    if (opt.animList || !opt.anim.empty()) {
        if (opt.gameDir.empty() || opt.drm.empty()) { std::fprintf(stderr, "ou-viewer: --anim/--anim-list need --game-dir and --drm\n"); return 2; }
        try {
            if (!loadClip(opt, clip, clipLabel)) return 2;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-viewer: --anim: %s\n", e.what());
            return 1;
        }
        if (opt.animList) return 0;
    }

    ou::platform::SdlInit sdl(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl.ok) {
        std::fprintf(stderr, "ou-viewer: SDL_Init failed (%s); no display, skipping\n", SDL_GetError());
        return kExitSkip;
    }

    ou::platform::WindowPtr window; // declared before the renderer: GL context / VkSurface die first
    std::string chosen, note, error;
    auto renderer = ou::render::selectRenderer(opt.renderer, ou::render::createBackend, chosen, note, error);
    if (!note.empty()) std::fprintf(stderr, "ou-viewer: %s\n", note.c_str());
    if (!renderer) { // backend unavailable on this system (no loader / device)
        std::fprintf(stderr, "ou-viewer: %s renderer unavailable: %s; skipping\n", opt.renderer.c_str(), error.c_str());
        return kExitSkip;
    }
    opt.renderer = chosen;
    window.reset(SDL_CreateWindow(
        "OpenUnderworld viewer", opt.width, opt.height,
        SDL_WindowFlags(renderer->windowFlags()) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
            (opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0)));
    if (!window) { std::fprintf(stderr, "ou-viewer: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    if (!renderer->init(window.get(), error)) { std::fprintf(stderr, "ou-viewer: %s\n", error.c_str()); return 1; }
    bool vsync = renderer->setVsync(opt.vsync) && opt.vsync;
    std::fprintf(stderr, "ou-viewer: %s\n", renderer->description().c_str());

    std::string source;
    ou::scene::SceneData scene = loadScene(opt, source);
    std::vector<std::string> warnings = scene.warnings;
    for (auto& w : renderer->upload(scene)) warnings.push_back(std::move(w));
    size_t perFormat[5] = {}; // RGBA8, BGRA8, BC1, BC2, BC3 (scene::PixelFormat order)
    for (const auto& t : scene.textures) ++perFormat[std::min<size_t>(size_t(t.format), 4)];
    std::fprintf(stderr, "ou-viewer: %s | %zu meshes, %zu textures (rgba8 %zu, bgra8 %zu, bc1 %zu, bc2 %zu, bc3 %zu)\n",
                 source.c_str(), scene.meshes.size(), scene.textures.size(), perFormat[0], perFormat[1],
                 perFormat[2], perFormat[3], perFormat[4]);
    std::string collisionSummary;
    const auto debugLines = opt.showCollision && !opt.drm.empty() ? collisionLines(opt, collisionSummary, warnings)
                                                                  : std::vector<ou::render::DebugLine>{};
    if (!collisionSummary.empty()) std::fprintf(stderr, "ou-viewer: %s\n", collisionSummary.c_str());
    for (const auto& w : warnings) std::fprintf(stderr, "ou-viewer: warning: %s\n", w.c_str());
    double animTime = opt.animTime;
    std::vector<ou::scene::Vertex> skinScratch;
    if (clip && poseSkinnedMeshes(*clip, animTime, scene, *renderer, skinScratch) == 0) {
        warnings.push_back("--anim: no skinned mesh with a skeleton in this scene");
        clip.reset();
    }

    // Initial framing: fixed pose for the placeholder, scene bounds otherwise.
    ou::render::Camera cam;
    cam.verticalFovDeg = opt.fov;
    float speed = 500.0f;
    if (source.rfind("PLACEHOLDER SCENE", 0) == 0) {
        cam.position = {-1100.0f, -1300.0f, 700.0f};
        cam.lookAt({0.0f, 0.0f, 100.0f});
    } else {
        glm::vec3 lo(1e30f), hi(-1e30f);
        for (const auto& m : scene.meshes) {
            glm::mat4 t;
            std::memcpy(&t, m.transform, sizeof t);
            for (const auto& v : m.vertices) {
                glm::vec3 p(t * glm::vec4(v.position[0], v.position[1], v.position[2], 1.0f));
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        }
        if (lo.x <= hi.x) {
            glm::vec3 c = (lo + hi) * 0.5f;
            float r = std::max(1.0f, glm::length(hi - lo) * 0.5f);
            cam.position = c + glm::vec3(-0.9f, -1.2f, 0.7f) * r * 1.4f;
            cam.lookAt(c);
            cam.farZ = std::max(cam.farZ, r * 20.0f);
            cam.nearZ = cam.farZ * 1e-5f;
            speed = r * 0.25f;
        }
    }
    if (opt.cam) {
        const auto& c = *opt.cam;
        cam.position = {c[0], c[1], c[2]};
        cam.yaw = glm::radians(c[3]);
        cam.pitch = glm::radians(c[4]);
    }

    ou::platform::GamepadPtr pad;
    ou::engine::FrameClock clock; // 60 Hz fixed simulation tick
    glm::vec3 simPrev = cam.position, simCur = cam.position;
    bool running = true, relative = false, overlay = !opt.noOverlay, lighting = true, cull = true;
    uint64_t last = SDL_GetTicksNS(), fpsStart = last;
    int frame = 0, fpsFrames = 0;
    double fps = 0.0, frameMs = 0.0;
    constexpr int kTimingWarmup = 10;
    uint64_t timingStart = 0;

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.key == SDLK_ESCAPE) running = false;
                else if (e.key.key == SDLK_V) vsync = renderer->setVsync(!vsync) && !vsync;
                else if (e.key.key == SDLK_L) lighting = !lighting;
                else if (e.key.key == SDLK_C) cull = !cull;
                else if (e.key.key == SDLK_F1) overlay = !overlay;
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (e.button.button == SDL_BUTTON_RIGHT) { // hold RMB to look
                    relative = e.button.down;
                    SDL_SetWindowRelativeMouseMode(window.get(), relative);
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (relative) {
                    cam.yaw -= e.motion.xrel * 0.0025f;
                    cam.pitch -= e.motion.yrel * 0.0025f;
                }
                break;
            case SDL_EVENT_MOUSE_WHEEL: speed *= std::pow(1.25f, e.wheel.y); break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) pad.reset(SDL_OpenGamepad(e.gdevice.which));
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad.get()) == e.gdevice.which) pad.reset();
                break;
            case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
                renderer->resize({e.window.data1, e.window.data2});
                break;
            default: break;
            }
        }

        const uint64_t now = SDL_GetTicksNS();
        const double dt = double(now - last) * 1e-9;
        last = now;

        // Look is applied per render frame (latency matters); movement runs on the fixed tick.
        const float lookRate = 2.5f * float(dt);
        cam.yaw -= axis(pad.get(), SDL_GAMEPAD_AXIS_RIGHTX) * lookRate;
        cam.pitch -= axis(pad.get(), SDL_GAMEPAD_AXIS_RIGHTY) * lookRate;
        cam.pitch = std::clamp(cam.pitch, glm::radians(-89.0f), glm::radians(89.0f));

        const bool* keys = SDL_GetKeyboardState(nullptr);
        float fwd = float(keys[SDL_SCANCODE_W] - keys[SDL_SCANCODE_S]) - axis(pad.get(), SDL_GAMEPAD_AXIS_LEFTY);
        float side = float(keys[SDL_SCANCODE_D] - keys[SDL_SCANCODE_A]) + axis(pad.get(), SDL_GAMEPAD_AXIS_LEFTX);
        float up = float(keys[SDL_SCANCODE_E] - keys[SDL_SCANCODE_Q]) +
                   axis(pad.get(), SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) - axis(pad.get(), SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        glm::vec3 move = cam.forward() * fwd + cam.right() * side + glm::vec3(0, 0, 1) * up;
        if (glm::length(move) > 1.0f) move = glm::normalize(move);
        const float boost = (keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT]) ? 4.0f : 1.0f;

        const int ticks = clock.advance(dt);
        for (int n = ticks; n > 0; --n) {
            simPrev = simCur;
            simCur += move * speed * boost * float(clock.step());
            if (clip) animTime += clock.step(); // animation advances on the fixed tick
        }
        if (clip && ticks > 0) poseSkinnedMeshes(*clip, animTime, scene, *renderer, skinScratch);
        cam.position = glm::mix(simPrev, simCur, float(clock.alpha()));

        ++fpsFrames;
        if (now - fpsStart >= 500'000'000ull) {
            const double span = double(now - fpsStart) * 1e-9;
            fps = fpsFrames / span;
            frameMs = 1000.0 * span / fpsFrames;
            fpsStart = now;
            fpsFrames = 0;
        }

        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window.get(), &w, &h);
        const float aspect = h > 0 ? float(w) / float(h) : 1.0f;
        std::vector<std::string> lines;
        if (overlay) {
            lines.push_back(scene.placeholder ? "!" + source : "source: " + source);
            lines.push_back(fmt("OpenUnderworld ou-viewer | %.1f FPS (%.2f ms) | vsync %s | %s | cull %s", fps,
                                frameMs, vsync ? "on" : "off", lighting ? "lambert" : "unlit", cull ? "on" : "off"));
            lines.push_back(fmt("%s | preset %s | msaa %dx | shadows %s | fxaa %s | hdr %s | shaders %s | brightness %.2f",
                                opt.renderer.c_str(), opt.graphics.preset.c_str(), opt.gfx.msaa,
                                opt.gfx.shadows ? "on" : "off", opt.gfx.fxaa ? "on" : "off", opt.gfx.hdr ? "on" : "off",
                                opt.gfx.originalShaders ? "original" : "generic", opt.gfx.brightness));
            lines.push_back(renderer->description());
            lines.push_back(ou::render::rtSummary(renderer->rtStatus(), ou::render::RayTracing(opt.gfx.rt), opt.gfx.rtRays));
            lines.push_back(renderer->upscalerSummary());
            lines.push_back(fmt("%dx%d aspect %.3f | vFOV %.1f hFOV %.1f (Hor+)", w, h, aspect, cam.verticalFovDeg,
                                glm::degrees(ou::render::horizontalFov(glm::radians(cam.verticalFovDeg), aspect))));
            lines.push_back(fmt("cam %.1f %.1f %.1f | yaw %.1f pitch %.1f | speed %.0f/s | tick %llu%s",
                                cam.position.x, cam.position.y, cam.position.z, glm::degrees(cam.yaw),
                                glm::degrees(cam.pitch), speed, (unsigned long long)clock.ticks(),
                                pad ? " | gamepad" : ""));
            lines.push_back(fmt("meshes %zu | textures %zu | placeholder %s | warnings %zu", scene.meshes.size(),
                                scene.textures.size(), scene.placeholder ? "YES" : "no", warnings.size()));
            if (clip)
                lines.push_back(fmt("%s | %u keys | t %.2f / %.2f s (looping)", clipLabel.c_str(), clip->keyCount,
                                    std::fmod(animTime, clip->durationSeconds()), clip->durationSeconds()));
            if (!collisionSummary.empty()) lines.push_back(collisionSummary);
            for (size_t i = 0; i < warnings.size() && i < 8; ++i) lines.push_back("!warning: " + warnings[i]);
            if (warnings.size() > 8) lines.push_back(fmt("!... %zu more warnings on stderr", warnings.size() - 8));
            lines.push_back("hold RMB look | WASD/QE move | Shift fast | wheel speed | V vsync | L light | C cull | F1 overlay");
        }
        ou::render::FrameOptions fo{};
        fo.lighting = lighting, fo.cullBackFaces = cull;
        fo.overlay = lines, fo.debugLines = debugLines;
        fo.msaa = opt.gfx.msaa, fo.shadows = opt.gfx.shadows, fo.fxaa = opt.gfx.fxaa, fo.hdr = opt.gfx.hdr;
        fo.rt = ou::render::RayTracing(opt.gfx.rt), fo.rtRays = opt.gfx.rtRays;
        fo.originalShaders = opt.gfx.originalShaders, fo.time = float(frame) / 60.0f; // deterministic wind phase per frame
        fo.brightness = opt.gfx.brightness;
        fo.upscaler = opt.gfx.fsr ? ou::render::Upscaler::Fsr : ou::render::Upscaler::Off;
        fo.fsrQuality = opt.gfx.fsrQuality, fo.sharpness = opt.gfx.sharpness, fo.frameGeneration = opt.gfx.frameGeneration;
        fo.water = ou::render::WaterQuality(opt.gfx.water); // no ripple sim / volumes in the viewer
        fo.waterDetail = opt.gfx.waterDetail, fo.waterDebug = opt.waterDebug;
        fo.atmosphere = ou::render::Atmosphere(opt.gfx.atmosphere), fo.lightShafts = opt.gfx.lightShafts;
        if (opt.gfx.weather >= 2) // the viewer has no unit weather: only an explicit --weather rain|snow
            fo.weather = {opt.gfx.weather == 2 ? ou::render::Weather::Kind::Rain : ou::render::Weather::Kind::Snow,
                          opt.gfx.weather == 2 ? 0.8f : 1.0f};
        renderer->drawFrame(cam, {w, h}, fo);
        if (frame == kTimingWarmup) timingStart = SDL_GetTicksNS();
        ++frame;
        if (opt.frames && frame >= opt.frames) {
            running = false;
            if (!opt.screenshot.empty()) {
                if (!writePpm(opt.screenshot, renderer->readBackbuffer({w, h}))) {
                    std::fprintf(stderr, "ou-viewer: cannot write %s\n", opt.screenshot.c_str());
                    return 1;
                }
                std::fprintf(stderr, "ou-viewer: wrote %s (%dx%d)\n", opt.screenshot.c_str(), w, h);
            }
        }
        renderer->present();
    }
    std::fprintf(stderr, "ou-viewer: %d frames, %llu ticks, %llu clock drops\n", frame,
                 (unsigned long long)clock.ticks(), (unsigned long long)clock.droppedFrames());
    if (frame > kTimingWarmup + 1) // --frames N --no-vsync: average wall time per frame after warm-up
        std::fprintf(stderr, "ou-viewer: frame time %.3f ms avg over %d frames (after %d warm-up)\n",
                     double(SDL_GetTicksNS() - timingStart) * 1e-6 / (frame - kTimingWarmup - 1),
                     frame - kTimingWarmup - 1, kTimingWarmup + 1);
    renderer->shutdown();
    return 0;
}
