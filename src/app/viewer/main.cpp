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

#include "cdc/Cdc.hpp"
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
    int width = 1280, height = 720;
    float fov = 60.0f;
    int frames = 0; // 0 = run until closed
    std::filesystem::path screenshot;
    bool vsync = true;
    std::optional<std::array<float, 5>> cam; // x y z yawDeg pitchDeg
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-viewer [--game-dir DIR] [--drm pc-w\\\\x.drm] [--width W --height H] [--fov DEG]\n"
                 "                 [--frames N] [--screenshot out.ppm] [--no-vsync] [--cam X,Y,Z,YAW,PITCH]\n"
                 "controls: hold RMB to mouse look, WASD move, Q/E down/up, Shift fast, wheel speed,\n"
                 "          gamepad sticks + triggers, V vsync, L lighting, C backface cull, F1 overlay, Esc quit\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char* v = nullptr;
        if (a == "--no-vsync") { o.vsync = false; continue; }
        if (a == "--help" || a == "-h") return false;
        if (!(v = next())) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
        if (a == "--game-dir") o.gameDir = v;
        else if (a == "--drm") o.drm = v;
        else if (a == "--width") o.width = std::atoi(v);
        else if (a == "--height") o.height = std::atoi(v);
        else if (a == "--fov") o.fov = float(std::atof(v));
        else if (a == "--frames") o.frames = std::atoi(v);
        else if (a == "--screenshot") o.screenshot = v;
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
    if (o.width <= 0 || o.height <= 0 || o.fov <= 1.0f || o.fov >= 179.0f || o.frames < 0) {
        std::fprintf(stderr, "invalid --width/--height/--fov/--frames\n");
        return false;
    }
    if (!o.screenshot.empty() && o.frames == 0) o.frames = 1;
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
            ou::cdc::Bigfile bigfile(o.gameDir / "bigfile.000");
            ou::scene::SceneData s = ou::formats::loadDrmScene(bigfile, o.drm);
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

float deadzone(float v) { return std::abs(v) < 0.15f ? 0.0f : (v - std::copysign(0.15f, v)) / 0.85f; }
float axis(SDL_Gamepad* g, SDL_GamepadAxis a) { return g ? deadzone(SDL_GetGamepadAxis(g, a) / 32767.0f) : 0.0f; }

char buf[512];
template <class... A>
std::string fmt(const char* f, A... a) {
    std::snprintf(buf, sizeof buf, f, a...);
    return buf;
}

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }

    ou::platform::SdlInit sdl(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl.ok) {
        std::fprintf(stderr, "ou-viewer: SDL_Init failed (%s); no display, skipping\n", SDL_GetError());
        return kExitSkip;
    }

    ou::platform::WindowPtr window; // declared before the renderer: GL context dies first
    auto renderer = ou::render::createGlRenderer();
    std::string error;
    if (!renderer->preWindowSetup(error)) { std::fprintf(stderr, "ou-viewer: %s\n", error.c_str()); return 1; }
    window.reset(SDL_CreateWindow(
        "OpenUnderworld viewer", opt.width, opt.height,
        SDL_WindowFlags(renderer->windowFlags()) | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY));
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
    for (const auto& w : warnings) std::fprintf(stderr, "ou-viewer: warning: %s\n", w.c_str());

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
    bool running = true, relative = false, overlay = true, lighting = true, cull = true;
    uint64_t last = SDL_GetTicksNS(), fpsStart = last;
    int frame = 0, fpsFrames = 0;
    double fps = 0.0, frameMs = 0.0;

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

        for (int n = clock.advance(dt); n > 0; --n) {
            simPrev = simCur;
            simCur += move * speed * boost * float(clock.step());
        }
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
            lines.push_back(renderer->description());
            lines.push_back(fmt("%dx%d aspect %.3f | vFOV %.1f hFOV %.1f (Hor+)", w, h, aspect, cam.verticalFovDeg,
                                glm::degrees(ou::render::horizontalFov(glm::radians(cam.verticalFovDeg), aspect))));
            lines.push_back(fmt("cam %.1f %.1f %.1f | yaw %.1f pitch %.1f | speed %.0f/s | tick %llu%s",
                                cam.position.x, cam.position.y, cam.position.z, glm::degrees(cam.yaw),
                                glm::degrees(cam.pitch), speed, (unsigned long long)clock.ticks(),
                                pad ? " | gamepad" : ""));
            lines.push_back(fmt("meshes %zu | textures %zu | placeholder %s | warnings %zu", scene.meshes.size(),
                                scene.textures.size(), scene.placeholder ? "YES" : "no", warnings.size()));
            for (size_t i = 0; i < warnings.size() && i < 8; ++i) lines.push_back("!warning: " + warnings[i]);
            if (warnings.size() > 8) lines.push_back(fmt("!... %zu more warnings on stderr", warnings.size() - 8));
            lines.push_back("hold RMB look | WASD/QE move | Shift fast | wheel speed | V vsync | L light | C cull | F1 overlay");
        }
        renderer->drawFrame(cam, {w, h}, {lighting, cull, lines});
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
    renderer->shutdown();
    return 0;
}
