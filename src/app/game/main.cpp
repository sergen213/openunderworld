// ou-game: playable Lara in a real unit. Simulation (src/game) runs on a fixed 60 Hz tick; rendering
// interpolates between the last two ticks. Docs: docs/GAMEPLAY.md.
#include <algorithm>
#include <bit>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "app/game/Cinematic.hpp"
#include "app/game/Sound.hpp"
#include "anim/Anim.hpp"
#include "audio/Audio.hpp"
#include "audio/Mixer.hpp"
#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "collision/Collision.hpp"
#include "config/Config.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"
#include "fx/Fx.hpp"
#include "game/Game.hpp"
#include "script/Vm.hpp"
#include "game/Weather.hpp"
#include "locale/Locale.hpp"
#include "platform/Sdl.hpp"
#include "save/Save.hpp"
#include "render/Renderer.hpp"
#include "render/Water.hpp"
#include "video/Player.hpp"
#include "video/Movie.hpp"
#include "ui/Art.hpp"
#include "ui/Ui.hpp"
#include "ui/Pda.hpp"

namespace {

constexpr int kExitSkip = 77; // ctest SKIP_RETURN_CODE: no display available
constexpr float kMouseSensitivity = 0.0025f; // rad per pixel

struct Options {
    std::filesystem::path gameDir;
    std::string level; // empty (and no --chapter/--load): start at the main menu
    int chapter = -1;  // --chapter N: start chapter N (game::kChapters)
    std::string renderer = ou::config::Config{}.renderer; // auto | opengl | vulkan
    int width = 1280, height = 720;
    float fov = 60.0f;
    bool vsync = true, fullscreen = false, noOverlay = false, showCollision = false, showTriggers = false, showEnemies = false;
    int frames = 0; // 0 = until closed
    int waterDebug = 0; // Enhanced water debug view (render::kWaterDebugViews; F7 cycles)
    std::filesystem::path screenshot, inputScript;
    double renderHz = 0; // > 0: fixed virtual frame time (deterministic replays); default 60 with --input-script
    std::optional<std::array<float, 4>> spawn; // --spawn X,Y,Z,YAWDEG overrides the unit's player intro
    std::optional<std::array<float, 6>> camera; // --camera X,Y,Z,TX,TY,TZ: fixed view (eye, target) for screenshots
    bool intro = false;  // --intro: play Movies/intro.bik first (skippable)
    int introFrames = -1; // --intro-frames N: stop the intro / the New Game movie after N frames
    bool noMovies = false; // --no-movies: New Game skips Movies/explode.bik
    bool drawLast = false; // --draw-last: with --frames, every frame runs its ticks but only the last is drawn (render-rate tests)
    bool noAudio = false;
    std::filesystem::path musicWav; // --music-wav FILE: no device; every tick's mixer output goes to a WAV
    std::string loadSlot; // --load SLOT: start from $XDG_DATA_HOME/openunderworld/saves/SLOT.ousave
    std::string cine;     // --cine NAME: play pc-w\cinstream\NAME.mul in its unit (--frames/--screenshot apply), then exit
    double die = -1;      // --die SECONDS: Lara dies then (death clip) and respawns at the last checkpoint
    std::vector<std::array<float, 3>> targets; // --target X,Y,Z (repeatable): shootable dummy, feet in the unit frame
    std::optional<ou::game::Weapon> weapon;    // --weapon uzi|shotgun|speargun|rifle|tranquilizer|thorhammer: carried secondary with max reserve (thorhammer: Mjolnir)
    ou::config::GraphicsConfig graphics; // --preset/--msaa/--shadows/--fxaa/--hdr/--rt/--rt-quality (or --config)
    ou::game::Controls controls;         // --config "controls" (default bindings without it)
    ou::config::GameConfig game;         // --config "game": in-game menu settings
    std::filesystem::path configPath;    // --config FILE: the menus write their settings back to it
    bool configInvertY = false;          // its controls.invert_y as last read / written
    ou::config::GraphicsSettings gfx;    // resolved
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-game --game-dir DIR [--level pc-w\\\\l4_pool.drm | --chapter 0-8] [--renderer auto|opengl|vulkan]\n"
                 "               [--frames N] [--screenshot out.ppm] [--input-script FILE] [--render-hz HZ]\n"
                 "               [--width W --height H] [--fov DEG] [--no-vsync] [--fullscreen] [--no-overlay] [--cine NAME]\n"
                 "               [--spawn X,Y,Z,YAWDEG] [--camera X,Y,Z,TX,TY,TZ] [--intro] [--intro-frames N] [--no-movies] [--no-audio] [--music-wav out.wav]\n"
                 "               [--load SLOT] [--die SECONDS] [--target X,Y,Z]... [--weapon uzi|shotgun|speargun|rifle|tranquilizer|thorhammer]\n"
                 "               [--show-collision] [--show-triggers] [--show-enemies] [--draw-last]\n"
                 "               [--preset original|enhanced] [--msaa 1|2|4|8] [--shadows on|off] [--fxaa on|off] [--hdr on|off]\n"
                 "               [--shaders original|generic]\n"
                 "               [--rt auto|off|shadows|full] [--rt-quality low|medium|high] [--brightness 0.5..2.0]\n"
                 "               [--water original|enhanced] [--water-detail auto|standard|high] [--water-debug 0-5]\n"
                 "               [--atmosphere auto|off|low|medium|high] [--shafts on|off] [--weather auto|off|rain|snow]\n"
                 "               [--upscaler off|fsr] [--fsr-quality native|quality|balanced|performance|ultra_performance]\n"
                 "               [--sharpness 0..1] [--frame-generation on|off] (FSR: Vulkan only) [--fx on|off]\n"
                 "               [--config config.json]\n"
                 "without --level/--chapter/--load: main menu (New Game plays Movies\\explode.bik, then chapter 0)\n"
                 "controls: WASD/arrows move (camera-relative), Shift walk, Space jump/climb, Ctrl or C drop, E use/push/pick up,\n"
                 "          F draw/holster, mouse left fire, right aim/lock, Q switch target, X switch weapon (pistols, secondary,\n"
                 "          Mjolnir), H health pack, L flashlight, R throw grenade, E with a full adrenaline meter + lock close by:\n"
                 "          head-shot window (Mjolnir: E + lock = ground pound), mouse look;\n"
                 "          gamepad: left stick, right stick, A jump, B drop, X use, Y draw, RT fire, LT lock, R3 switch, LB walk,\n"
                 "          d-pad right switch weapon, d-pad up health pack, d-pad left flashlight, d-pad down grenade,\n"
                 "          Start pause; Esc pause menu, F1 overlay, F5 quicksave, F9 quickload (slot 'quick'), F7 water debug view\n"
                 "motorcycle: E beside it gets on / off (almost stopped); forward throttle, back brake/reverse, left/right steer,\n"
                 "          Space hop\n"
                 "menus: arrows/WASD or d-pad navigate, Enter/Space or A accept, E or X alt (PDA task), Esc or B back,\n"
                 "          Backspace or Y deletes a save slot; 1 opens the PDA\n"
                 "input script lines: <seconds> <forward|right|walk|jump|drop|interact|save|load|draw|fire|aim|switch|weapon|heal|pause|\n"
                 "                    grapple|sprint|hint|answer|pda|light|grenade|lookx|looky>\n"
                 "                    [value]  (in menus forward/right +-1 navigate, jump accepts, interact is alt, drop goes back;\n"
                 "                    script time runs on while paused)\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--config") continue;
        try {
            const auto c = ou::config::load(argv[i + 1]);
            o.gameDir = c.gameDir, o.renderer = c.renderer, o.width = c.width, o.height = c.height;
            o.fov = c.fov, o.vsync = c.vsync, o.fullscreen = c.fullscreen, o.graphics = c.graphics;
            o.game = c.game, o.configPath = argv[i + 1], o.configInvertY = c.controls.invertY;
            o.controls = ou::game::Controls(c.controls);
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
        if (a == "--show-triggers") { o.showTriggers = true; continue; }
        if (a == "--show-enemies") { o.showEnemies = true; continue; }
        if (a == "--intro") { o.intro = true; continue; }
        if (a == "--no-audio") { o.noAudio = true; continue; }
        if (a == "--no-movies") { o.noMovies = true; continue; }
        if (a == "--draw-last") { o.drawLast = true; continue; }
        if (a == "--help" || a == "-h") return false;
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); return false; }
        const char* v = argv[++i];
        if (a == "--config") continue;
        try {
            if (ou::config::applyGraphicsFlag(o.graphics, a, v)) continue;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            return false;
        }
        if (a == "--game-dir") o.gameDir = v;
        else if (a == "--level") o.level = v;
        else if (a == "--chapter") o.chapter = std::atoi(v);
        else if (a == "--music-wav") o.musicWav = v;
        else if (a == "--renderer") o.renderer = v;
        else if (a == "--width") o.width = std::atoi(v);
        else if (a == "--height") o.height = std::atoi(v);
        else if (a == "--fov") o.fov = float(std::atof(v));
        else if (a == "--frames") o.frames = std::atoi(v);
        else if (a == "--water-debug") o.waterDebug = std::atoi(v);
        else if (a == "--screenshot") o.screenshot = v;
        else if (a == "--input-script") o.inputScript = v;
        else if (a == "--render-hz") o.renderHz = std::atof(v);
        else if (a == "--intro-frames") o.intro = true, o.introFrames = std::atoi(v);
        else if (a == "--spawn") {
            std::array<float, 4> s{};
            if (std::sscanf(v, "%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3]) != 4) { std::fprintf(stderr, "--spawn wants X,Y,Z,YAWDEG\n"); return false; }
            o.spawn = s;
        }
        else if (a == "--camera") {
            std::array<float, 6> c{};
            if (std::sscanf(v, "%f,%f,%f,%f,%f,%f", &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]) != 6) { std::fprintf(stderr, "--camera wants X,Y,Z,TX,TY,TZ\n"); return false; }
            o.camera = c;
        }
        else if (a == "--load") o.loadSlot = v;
        else if (a == "--die") o.die = std::atof(v);
        else if (a == "--cine") o.cine = v;
        else if (a == "--target") {
            std::array<float, 3> t{};
            if (std::sscanf(v, "%f,%f,%f", &t[0], &t[1], &t[2]) != 3) { std::fprintf(stderr, "--target wants X,Y,Z\n"); return false; }
            o.targets.push_back(t);
        }
        else if (a == "--weapon") {
            o.weapon = ou::game::weaponByName(v);
            if (!o.weapon || *o.weapon == ou::game::Weapon::Pistols) { std::fprintf(stderr, "--weapon wants uzi, shotgun, speargun, rifle, tranquilizer or thorhammer\n"); return false; }
        }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
    }
    if (o.gameDir.empty()) { std::fprintf(stderr, "--game-dir is required\n"); return false; }
    if (o.renderer != "auto" && o.renderer != "opengl" && o.renderer != "vulkan") {
        std::fprintf(stderr, "--renderer wants auto, opengl or vulkan\n");
        return false;
    }
    if (o.width <= 0 || o.height <= 0 || o.fov <= 1.0f || o.fov >= 179.0f || o.frames < 0 || o.renderHz < 0 ||
        o.chapter >= int(ou::game::kChapters.size())) {
        std::fprintf(stderr, "invalid --width/--height/--fov/--frames/--render-hz/--chapter\n");
        return false;
    }
    try {
        o.gfx = ou::config::resolve(o.graphics);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "%s\n", e.what());
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

// The motorcycle (src/game/Vehicle.*): its skinned model posed from World::bikes (wheels, handlebars) and placed by
// World::bikeMatrix; its intro mesh in the level scene is left out (StreamedScene::build).
using BikeIndices = std::vector<std::pair<int32_t, std::vector<size_t>>>; // bike uid -> scene mesh indices
void poseBikes(const ou::game::World& w, double alpha, const BikeIndices& bikes, const ou::scene::SceneData& scene,
               ou::render::Renderer& r, std::vector<ou::scene::Vertex>& scratch) {
    for (const auto& [uid, meshes] : bikes) {
        const auto m = w.bikeMatrix(uid, alpha);
        const auto it = w.bikes().find(uid);
        if (!m || it == w.bikes().end()) continue;
        for (const size_t i : meshes) {
            const auto& mesh = scene.meshes[i];
            if (mesh.skeleton < 0) continue;
            const auto& sk = scene.skeletons[size_t(mesh.skeleton)];
            const auto palette = ou::anim::skinPalette(sk, ou::anim::worldMatrices(sk, ou::game::bikePose(sk, it->second, alpha)));
            ou::anim::skinVertices(mesh, palette, scratch, *m);
            r.updateVertices(i, scratch);
        }
    }
}

// Lara's skinned meshes: CPU-skinned in model space from World::pose (crossfaded clips + simulated hair;
// root segment held at bind, its motion is in the player position), then moved to the world.
struct LaraMeshes {
    std::vector<size_t> meshIndices; // into the uploaded scene
    std::vector<ou::scene::Vertex> scratch;

    void pose(const ou::game::World& world, const ou::game::PlayerState& s, double alpha, const ou::scene::SceneData& scene,
              ou::render::Renderer& r) {
        const auto palette = ou::anim::skinPalette(world.anims().skeleton, world.pose(alpha));
        const glm::mat4 m = world.player().modelMatrix(s.pos, s.yaw, s.pitch);
        for (const size_t i : meshIndices) {
            ou::anim::skinVertices(scene.meshes[i], palette, scratch, m);
            r.updateVertices(i, scratch);
        }
    }
};

// Scripted objects (src/script): animated switches/doors and moved blocks, re-posed on the CPU in model space.
// Their level-scene meshes are tagged " @intro <unique id>" (formats::loadLevelScene).
struct ScriptedMeshes {
    std::map<int32_t, std::vector<size_t>> meshes; // intro unique id -> scene mesh indices
    std::set<int32_t> posed;                       // uploaded with a non-bind pose
    std::set<int32_t> hidden;                      // collected pickups, uploaded collapsed to a point
    std::vector<std::pair<int32_t, float>> faded;  // scratch: fx::System::fadedOwners
    std::vector<ou::scene::Vertex> scratch;

    void index(const ou::scene::SceneData& s) {
        meshes.clear(), posed.clear(), hidden.clear();
        for (size_t i = 0; i < s.meshes.size(); ++i)
            if (const auto at = s.meshes[i].origin.rfind(" @intro "); at != std::string::npos)
                meshes[std::atoi(s.meshes[i].origin.c_str() + at + 8)].push_back(i);
    }
    // Picked-up rewards vanish once their instance fade (class 7, their "pickup" list) is done: their meshes degenerate
    // to one point (no scene rebuild) and their effects end with the instance.
    void hide(const std::set<int32_t>& ids, const ou::scene::SceneData& s, ou::render::Renderer& r, ou::fx::System& fx) {
        for (const int32_t id : ids) {
            const auto it = meshes.find(id);
            if (it == meshes.end() || fx.fading(id) || !hidden.insert(id).second) continue;
            fx.stopOwner(id);
            for (const size_t i : it->second) {
                scratch = s.meshes[i].vertices;
                if (scratch.empty()) continue;
                for (auto& v : scratch) std::copy_n(scratch[0].position, 3, v.position);
                r.updateVertices(i, scratch);
            }
        }
    }
    // Instances with a fade (fx owner = intro uid): their meshes at opacity 1 - fade (FrameOptions::meshOpacity).
    void opacities(const ou::fx::System& fx, std::vector<ou::render::MeshOpacity>& out) {
        out.clear();
        fx.fadedOwners(faded);
        for (const auto& [uid, f] : faded)
            if (const auto it = meshes.find(uid); it != meshes.end() && !hidden.contains(uid))
                for (const size_t i : it->second) out.push_back({uint32_t(i), 1.0f - f});
    }
    void pose(const ou::script::Runtime& rt, const ou::scene::SceneData& s, ou::render::Renderer& r) {
        for (const auto& o : rt.objects()) {
            const auto it = meshes.find(o.uid);
            if (it == meshes.end()) continue;
            const auto palette = rt.pose(o);
            if (!palette && !posed.erase(o.uid)) continue; // bind pose already uploaded
            if (palette) posed.insert(o.uid);
            for (const size_t i : it->second) {
                const auto& m = s.meshes[i];
                if (palette && !m.skin.empty() && palette->size() > 1) ou::anim::skinVertices(m, *palette, scratch);
                else ou::anim::skinVertices(m, {}, scratch, palette ? (*palette)[0] : glm::mat4(1.0f)); // rigid: bone 0
                r.updateVertices(i, scratch);
            }
        }
    }
};

void appendTriggerLines(const ou::script::Runtime& rt, std::vector<ou::render::DebugLine>& out) {
    for (const auto& [ab, c] : rt.debugLines()) out.push_back({{ab.first.x, ab.first.y, ab.first.z}, {ab.second.x, ab.second.y, ab.second.z}, c});
}

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

// Enemies (src/game/ai): each enemy's model-0 skinned meshes, CPU-skinned from Enemy::pose and placed in the
// world like Lara's. Their intro meshes in the level scene are left out (StreamedScene::build). Enemy shots are
// drawn as tracers (yellow hit, white miss) for 0.2 s.
struct EnemyMeshes {
    std::map<std::string, ou::scene::SceneData> types; // object name -> model-0 skinned meshes + textures
    std::vector<std::vector<size_t>> meshIndices;       // per World::enemies().all() entry
    struct SkinJob { size_t mesh, palette; glm::mat4 model; };
    std::vector<SkinJob> jobs;
    std::vector<std::vector<glm::mat4>> palettes;     // per posed enemy this frame
    std::vector<std::vector<ou::scene::Vertex>> outs; // per job, reused across frames
    std::vector<std::pair<ou::game::ai::Shot, uint64_t>> tracers; // shot, World tick

    void tracerLines(uint64_t tick, std::vector<ou::render::DebugLine>& out) {
        std::erase_if(tracers, [&](const auto& t) { return tick > t.second + 12; });
        for (const auto& [s, at] : tracers) out.push_back({{s.from.x, s.from.y, s.from.z}, {s.to.x, s.to.y, s.to.z}, s.hit ? 0xFF00FFFFu : 0xFFFFFFFFu});
    }

    const ou::scene::SceneData& type(const ou::cdc::BigfileSet& bf, const std::string& name, std::vector<std::string>& warnings) {
        auto it = types.find(name);
        if (it != types.end()) return it->second;
        ou::scene::SceneData s;
        try {
            s = ou::formats::loadDrmScene(bf, "pc-w\\" + name + ".drm");
            uint32_t model = 0;
            if (auto bytes = bf.read("pc-w\\" + name + ".drm"))
                for (const uint32_t id : ou::formats::objectRenderModels(ou::cdc::parseDrm(std::move(*bytes))))
                    if ((model = id)) break;
            std::erase_if(s.meshes, [&](const auto& m) { return m.skeleton < 0 || s.skeletons[size_t(m.skeleton)].renderModelId != model; });
            if (s.meshes.empty()) warnings.push_back("pc-w\\" + name + ".drm: no skinned model-0 mesh");
        } catch (const std::exception& e) {
            warnings.push_back(name + ": " + e.what());
        }
        return types.emplace(name, std::move(s)).first->second;
    }
    // Enemies outside the view cone (bounding radius kRadius; everything within kNear of the camera still poses so
    // nearby off-screen shadows follow) keep their last uploaded pose: CPU skinning every enemy was a third of
    // l4_ballcourt's frame. fovDeg >= 180: pose all. The visible meshes skin on worker threads (skinVertices is pure),
    // then upload in order on this thread.
    void pose(const ou::game::World& w, double alpha, const ou::scene::SceneData& scene, ou::render::Renderer& r, glm::vec3 eye,
              glm::vec3 target, float fovDeg, float aspect) {
        constexpr float kNear = 1500.0f, kRadius = 400.0f;
        const glm::vec3 fwd = glm::normalize(target - eye);
        const float ty = std::tan(glm::radians(std::min(fovDeg, 179.0f)) * 0.5f);
        const float halfDiag = std::atan(ty * std::sqrt(1.0f + aspect * aspect)); // cone around the view rectangle
        const auto& all = w.enemies().all();
        jobs.clear();
        palettes.clear();
        for (size_t k = 0; k < all.size() && k < meshIndices.size(); ++k) {
            const auto& e = *all[k];
            const auto s = e.interpolated(alpha);
            if (fovDeg < 180.0f) {
                const glm::vec3 d = s.pos - eye;
                const float dist = glm::length(d);
                if (dist > kNear && std::acos(std::clamp(glm::dot(d / dist, fwd), -1.0f, 1.0f)) >
                                        halfDiag + std::asin(std::min(1.0f, kRadius / dist)))
                    continue;
            }
            palettes.push_back(ou::anim::skinPalette(e.type().skeleton, e.pose(alpha)));
            const glm::mat4 m = ou::game::ai::Enemy::modelMatrix(s.pos, s.yaw, s.tip, s.tilt);
            for (const size_t i : meshIndices[k]) jobs.push_back({i, palettes.size() - 1, m});
        }
        if (outs.size() < jobs.size()) outs.resize(jobs.size());
        std::atomic<size_t> next{0};
        auto work = [&] {
            for (size_t j; (j = next++) < jobs.size();)
                ou::anim::skinVertices(scene.meshes[jobs[j].mesh], palettes[jobs[j].palette], outs[j], jobs[j].model);
        };
        {
            // jobs may be empty (nothing posed this frame, or a frame before the first tick): 0 extra threads, not SIZE_MAX
            std::vector<std::jthread> pool(std::min<size_t>(std::max(1u, std::thread::hardware_concurrency()), std::clamp<size_t>(jobs.size(), 1, 8)) - 1);
            for (auto& t : pool) t = std::jthread(work);
            work();
        }
        for (size_t j = 0; j < jobs.size(); ++j) r.updateVertices(jobs[j].mesh, outs[j]);
    }
};

// World point -> screen pixels (top-left origin) for the camera; nullopt behind it.
std::optional<glm::vec2> toScreen(const ou::game::CameraState& cam, float fovDeg, int width, int height, glm::vec3 p) {
    const glm::vec3 fwd = glm::normalize(cam.target - cam.position), right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1))),
                    up = glm::cross(right, fwd);
    const float ty = std::tan(glm::radians(fovDeg) * 0.5f), tx = ty * float(width) / float(height);
    const glm::vec3 v = p - cam.position;
    const float z = glm::dot(v, fwd);
    if (z <= 1) return std::nullopt;
    return glm::vec2((glm::dot(v, right) / (z * tx) + 1) * 0.5f * float(width), (1 - glm::dot(v, up) / (z * ty)) * 0.5f * float(height));
}

// Combat (src/game/Combat.*): weapon models on Lara's hand / holster bones, --target dummies, the HUD state and reticle
// (ui::Hud). Muzzle flashes, impacts and weapon sounds are the original animation events (EffectsView, GameSound).
struct CombatView {
    std::vector<ou::scene::Vertex> scratch;
    std::vector<std::unique_ptr<ou::game::DummyTarget>> dummies;
    bool dead = false, slowMo = false;
    int healthPacksUsed = 0;
    ou::game::Weapon weapon = ou::game::Weapon::Pistols;

    void tick(const ou::game::World& w) {
        const auto& c = w.combat();
        const auto& s = w.player().state();
        const auto tick = (unsigned long long)w.ticks();
        for (const auto& shot : c.shots) {
            std::fprintf(stderr, "ou-game: tick %llu shot %s %s %.0f %.0f %.0f -> %.0f %.0f %.0f%s damage %.1f\n", tick,
                         std::string(ou::game::weaponName(c.weapon)).c_str(), shot.hand ? "left" : "right", shot.from.x, shot.from.y,
                         shot.from.z, shot.to.x, shot.to.y, shot.to.z, shot.target ? " TARGET" : shot.hit ? " wall" : "", shot.damage);
        }
        if ((s.state == ou::game::State::Dead) != dead)
            std::fprintf(stderr, "ou-game: tick %llu %s (health %.0f, clip %s)\n", tick, dead ? "respawned" : "died", w.health().value(), s.clip.c_str());
        if (c.healthPacksUsed != healthPacksUsed)
            std::fprintf(stderr, "ou-game: tick %llu health pack used: health %.0f, %d left\n", tick, w.health().value(), w.found[2]);
        if (c.weapon != weapon) std::fprintf(stderr, "ou-game: tick %llu weapon %s\n", tick, std::string(ou::game::weaponName(c.weapon)).c_str());
        if ((c.slowMo > 0) != slowMo) std::fprintf(stderr, "ou-game: tick %llu adrenaline window %s\n", tick, c.slowMo > 0 ? "open" : "closed");
        if (s.state == ou::game::State::Hit && w.previous().clip != s.clip)
            std::fprintf(stderr, "ou-game: tick %llu hit reaction %s (health %.0f)\n", tick, s.clip.c_str(), w.health().value());
        dead = s.state == ou::game::State::Dead, slowMo = c.slowMo > 0;
        healthPacksUsed = c.healthPacksUsed, weapon = c.weapon;
    }

    // Guns: world-space vertices from Lara's interpolated pose; slots not shown collapse to a point.
    void pose(const ou::game::World& w, const ou::game::PlayerState& s, double alpha, const ou::scene::SceneData& scene,
              const std::vector<size_t>& gunIndices, const std::vector<std::pair<ou::game::Weapon, int>>& gunSlots, ou::render::Renderer& r) {
        const auto bones = w.pose(alpha);
        for (size_t h = 0; h < gunIndices.size() && h < gunSlots.size(); ++h) {
            const auto mount = w.gunMount(gunSlots[h].first, gunSlots[h].second);
            if (!mount || size_t(mount->first) >= bones.size()) {
                scratch = scene.meshes[gunIndices[h]].vertices;
                for (auto& v : scratch) v.position[0] = v.position[1] = v.position[2] = 0;
            } else {
                const glm::mat4 m = w.player().modelMatrix(s.pos, s.yaw, s.pitch) * bones[size_t(mount->first)] * mount->second;
                ou::anim::skinVertices(scene.meshes[gunIndices[h]], {}, scratch, m);
            }
            r.updateVertices(gunIndices[h], scratch);
        }
    }

    void lines(const ou::game::World&, std::vector<ou::render::DebugLine>& out) const {
        auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t col) { out.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, col}); };
        for (const auto& d : dummies) { // capsule wireframe: red alive, grey dead
            const uint32_t col = d->alive() ? 0xFF3030FF : 0xFF808080;
            for (int k = 0; k < 12; ++k) {
                const float a0 = float(k) / 12 * 6.2832f, a1 = float(k + 1) / 12 * 6.2832f;
                const glm::vec3 p0(std::cos(a0) * d->r, std::sin(a0) * d->r, 0), p1(std::cos(a1) * d->r, std::sin(a1) * d->r, 0);
                for (const float z : {d->r, d->h * 0.5f, d->h - d->r}) line(d->pos + p0 + glm::vec3(0, 0, z), d->pos + p1 + glm::vec3(0, 0, z), col);
                if (k % 3 == 0) line(d->pos + p0 + glm::vec3(0, 0, d->r), d->pos + p0 + glm::vec3(0, 0, d->h - d->r), col);
            }
            line(d->pos, d->pos + glm::vec3(0, 0, d->h), col);
        }
    }

    // The original HUD's game values (ui::Hud display groups).
    static ou::ui::HudState hudState(const ou::game::World& w) {
        const auto& c = w.combat();
        const auto& wd = w.anims().weapons[size_t(c.weapon)];
        ou::ui::HudState h;
        h.health = w.health().value(), h.healthMax = w.health().max();
        h.adrenaline = c.adrenaline, h.breath = w.air();
        h.weapon = int(c.weapon), h.drawn = c.drawn, h.dual = wd.dual, h.dead = w.player().state().state == ou::game::State::Dead;
        h.clip[0] = c.clip[size_t(c.weapon)][0], h.clip[1] = c.clip[size_t(c.weapon)][1], h.clipSize = wd.clip, h.nextHand = c.hand;
        h.healthPacks = w.found[2];
        h.gearItem = c.healHeld ? 0 : c.weaponHeld || c.drawHeld ? 2 : -1;
        h.flashlight = w.flashlight, h.grenades = w.grenades, h.grenadesMax = w.maxGrenades();
        return h;
    }

    // Combat reticle in screen pixels: on the target's aim point, else centred (free aim) while a weapon is out.
    static ou::ui::Reticle reticle(const ou::game::World& w, const ou::game::CameraState& cam, float fovDeg, int width, int height) {
        const auto& c = w.combat();
        ou::ui::Reticle r;
        if (!c.drawn) return r;
        const auto s = c.target ? toScreen(cam, fovDeg, width, height, c.target->aimPoint()) : std::optional(glm::vec2(width, height) * 0.5f);
        if (!s) return r;
        using K = ou::ui::Reticle::Kind;
        r.kind = c.target ? (c.locked ? K::Locked : K::Target) : K::Free;
        r.x = s->x, r.y = s->y, r.slowMo = c.slowMo > 0, r.pipX = c.pip.x, r.pipY = c.pip.y;
        return r;
    }
};

// Debug lines (--show-enemies): body capsule (colour by mode: idle green, alert yellow, chase orange, attack red,
// hit magenta, dead grey, spawning cyan, fire pink, reload blue) and the line of sight to Lara while it perceives her.
void appendEnemyLines(const ou::game::World& w, std::vector<ou::render::DebugLine>& out) {
    using ou::game::ai::Mode;
    auto line = [&](glm::vec3 a, glm::vec3 b, uint32_t c) { out.push_back({{a.x, a.y, a.z}, {b.x, b.y, b.z}, c}); };
    const ou::game::ai::Target lara{w.player().state().pos, true, w.player().tuning().height};
    for (const auto& e : w.enemies().all()) {
        const auto& s = e->state();
        static constexpr uint32_t kColour[] = {0xFF00FF00, 0xFF00FFFF, 0xFF0080FF, 0xFF0000FF, 0xFFFF00FF,
                                               0xFF808080, 0xFFFFFF00, 0xFF8080FF, 0xFFFF8000};
        const uint32_t c = kColour[size_t(s.mode)];
        const float r = e->radius(), h = e->height();
        for (int k = 0; k < 16; ++k) {
            const float a0 = float(k) * 0.3927f, a1 = float(k + 1) * 0.3927f;
            for (const float z : {0.0f, h})
                line(s.pos + glm::vec3(r * std::cos(a0), r * std::sin(a0), z), s.pos + glm::vec3(r * std::cos(a1), r * std::sin(a1), z), c);
        }
        line(s.pos, s.pos + glm::vec3(0, 0, h), c);
        line(s.pos + glm::vec3(0, 0, h * 0.8f), s.pos + glm::vec3(0, 0, h * 0.8f) + glm::vec3(std::cos(s.yaw), std::sin(s.yaw), 0) * 300.0f, c);
        if (s.mode != Mode::Dead && e->perceives(w.collision(), lara))
            line(s.pos + glm::vec3(0, 0, h * 0.8f), lara.pos + glm::vec3(0, 0, lara.height * 0.75f), 0xFF00FFFF);
    }
}
// Render scene of the streamed units: each loaded unit's level scene (cached while it stays loaded) moved by
// its stream offset, then Lara's skinned meshes. Rebuilt and re-uploaded when World::unitsVersion changes.
struct StreamedScene {
    std::map<std::string, ou::scene::SceneData> units; // unit frame
    ou::scene::SceneData lara;                         // Lara's skinned meshes, skeletons, textures
    ou::scene::SceneData guns;                         // weapon models, one per gunSlots entry (CombatView::pose)
    std::vector<std::pair<ou::game::Weapon, int>> gunSlots; // weapon, hand of each guns mesh
    std::vector<size_t> gunIndices;
    ou::scene::SceneData grenades;                     // GadgetView::kSlots copies of pr_grenade's mesh (GadgetView::pose)
    std::vector<size_t> grenadeIndices;
    std::vector<ou::scene::Light> reservedLights;      // GadgetView::base + EffectsView slots: the first environment lights
    std::vector<ou::scene::Light> fixtureLights;       // EffectsView::fixtures: appended to the environment lights
    BikeIndices bikeIndices;
    uint64_t version = ~0ull, enemyVersion = ~0ull, lightsVersion = ~0ull, visibilityVersion = ~0ull;
    uint32_t hiddenGroups = 0; // World::laraHiddenGroups() the scene was built with
    std::string laraOutfit; // pc-w\<laraOutfit>.drm is in `lara` and rigs the world's pose skeleton

    // Lara's skinned meshes and pose rig from World::outfit's DRM (GAMEPLAY.md "Outfits") when it changed: level loads
    // and saves pick the outfit. Returns the parsed DRM (for its waves) on a change; the scene then needs a rebuild.
    // A missing DRM falls back to lara.drm, as tru.exe 0x5DE2B0 falls back to the unit's object.
    std::optional<ou::cdc::Drm> dress(const ou::cdc::BigfileSet& bf, ou::game::World& w, std::vector<std::string>& warnings) {
        if (w.outfit == laraOutfit) return std::nullopt;
        auto bytes = bf.read("pc-w\\" + w.outfit + ".drm");
        if (!bytes) {
            warnings.push_back("pc-w\\" + w.outfit + ".drm not in bigfile: Lara wears lara.drm");
            w.outfit = "lara";
            if (w.outfit == laraOutfit) return std::nullopt;
            bytes = bf.read("pc-w\\lara.drm");
            if (!bytes) throw std::runtime_error("pc-w\\lara.drm not in bigfile");
        }
        auto drm = ou::cdc::parseDrm(std::move(*bytes));
        w.setOutfitRig(drm);
        auto sc = ou::formats::loadDrmScene(bf, "pc-w\\" + w.outfit + ".drm");
        lara = {};
        lara.skeletons = std::move(sc.skeletons);
        lara.textures = std::move(sc.textures);
        lara.materials = std::move(sc.materials);
        lara.shaders = std::move(sc.shaders);
        for (auto& m : sc.meshes)
            if (m.skeleton >= 0 && !m.indices.empty()) lara.meshes.push_back(std::move(m)); // skinned body only
        if (lara.meshes.empty()) warnings.push_back("pc-w\\" + w.outfit + ".drm: no skinned mesh decoded");
        for (auto& x : sc.warnings) warnings.push_back(std::move(x));
        laraOutfit = w.outfit;
        return drm;
    }

    ou::scene::SceneData build(const ou::cdc::BigfileSet& bf, const ou::game::World& w, LaraMeshes& laraMeshes,
                               EnemyMeshes& enemyMeshes, std::vector<std::string>& warnings) {
        std::erase_if(units, [&](const auto& kv) {
            return std::none_of(w.units().begin(), w.units().end(), [&](const auto& u) { return u.data.name == kv.first; });
        });
        ou::scene::SceneData out;
        std::set<uint32_t> textureIds, materialIds; // cdc section ids, unique across DRMs
        std::set<uint64_t> shaderIds;
        std::set<int32_t> enemyIntros; // drawn posed by EnemyMeshes instead
        for (const auto& e : w.enemies().all())
            if (!e->generator) enemyIntros.insert(e->uid());
        std::set<int32_t> hiddenIntros;
        for (const auto& o : w.script().objects())
            if (o.hidden) hiddenIntros.insert(o.uid);
        for (const auto& [uid, b] : w.bikes()) hiddenIntros.insert(uid); // drawn posed (poseBikes)
        auto append = [&](const ou::scene::SceneData& s, glm::vec3 offset, std::vector<size_t>* indices,
                          const std::string* unitName = nullptr) {
            const auto skelBase = int32_t(out.skeletons.size());
            out.skeletons.insert(out.skeletons.end(), s.skeletons.begin(), s.skeletons.end());
            for (auto m : s.meshes) {
                if (const auto at = m.origin.rfind(" @intro "); at != std::string::npos) {
                    const int32_t uid = std::atoi(m.origin.c_str() + at + 8);
                    if (enemyIntros.contains(uid) || hiddenIntros.contains(uid)) continue;
                }
                m.transform[12] += offset.x, m.transform[13] += offset.y, m.transform[14] += offset.z;
                if (m.skeleton >= 0) m.skeleton += skelBase;
                if (indices) indices->push_back(out.meshes.size());
                out.meshes.push_back(std::move(m));
            }
            for (const auto& t : s.textures)
                if (textureIds.insert(t.id).second) out.textures.push_back(t);
            // Original-shader inputs (docs/formats/SHADERS.md): section / program ids are global, first one wins.
            for (const auto& m : s.materials)
                if (materialIds.insert(m.id).second) out.materials.push_back(m);
            for (const auto& c : s.shaders)
                if (shaderIds.insert(c.hash).second) out.shaders.push_back(c);
            // First unit's render settings; every unit's point/spot lights, moved by its stream offset.
            if (!out.environment.valid && s.environment.valid) {
                out.environment = s.environment;
                out.environment.lights.clear();
                for (int c = 0; c < 3; ++c) out.environment.sunOrigin[c] += offset[c];
            }
            for (size_t i = 0; i < s.environment.lights.size(); ++i) {
                if (unitName && !w.lightEnabled(*unitName, uint32_t(i))) continue;
                auto l = s.environment.lights[i];
                for (int c = 0; c < 3; ++c) l.position[c] += offset[c];
                out.environment.lights.push_back(std::move(l));
            }
        };
        for (const auto& u : w.units()) {
            auto it = units.find(u.data.name);
            if (it == units.end()) {
                it = units.emplace(u.data.name, ou::formats::loadLevelScene(bf, "pc-w\\" + u.data.name + ".drm")).first;
                for (const auto& s : it->second.warnings) warnings.push_back(s);
            }
            append(it->second, u.offset, nullptr, &u.data.name);
        }
        // Draw groups hidden on Lara (Thor's gear, wetsuit dive gear; GAMEPLAY.md). Lara's meshes are the last block of
        // `out` here, so the ones hideDrawGroups emptied are dropped in place.
        hiddenGroups = w.laraHiddenGroups();
        laraMeshes.meshIndices.clear();
        const size_t laraFirst = out.meshes.size();
        append(lara, glm::vec3(0), nullptr);
        size_t kept = laraFirst;
        for (size_t i = laraFirst; i < out.meshes.size(); ++i) {
            if (!ou::scene::hideDrawGroups(out.meshes[i], hiddenGroups)) continue;
            if (kept != i) out.meshes[kept] = std::move(out.meshes[i]);
            laraMeshes.meshIndices.push_back(kept++);
        }
        out.meshes.resize(kept);
        enemyMeshes.meshIndices.clear();
        for (const auto& e : w.enemies().all()) {
            enemyMeshes.meshIndices.emplace_back();
            append(enemyMeshes.type(bf, e->type().name, warnings), glm::vec3(0), &enemyMeshes.meshIndices.back());
        }
        bikeIndices.clear();
        for (const auto& [uid, b] : w.bikes())
            if (w.bikeMatrix(uid, 1)) {
                bikeIndices.push_back({uid, {}});
                append(enemyMeshes.type(bf, "motorcycle", warnings), glm::vec3(0), &bikeIndices.back().second);
            }
        gunIndices.clear();
        append(guns, glm::vec3(0), &gunIndices);
        grenadeIndices.clear();
        append(grenades, glm::vec3(0), &grenadeIndices);
        // GadgetView's and EffectsView's moving lights take the first environment slots (OriginalShading uses the first
        // 64 lights); effect fixtures follow the units' lights.
        if (out.environment.valid) {
            out.environment.lights.insert(out.environment.lights.begin(), reservedLights.begin(), reservedLights.end());
            out.environment.lights.insert(out.environment.lights.end(), fixtureLights.begin(), fixtureLights.end());
        }
        version = w.unitsVersion();
        enemyVersion = w.enemies().version();
        lightsVersion = w.lightsVersion();
        visibilityVersion = w.script().visibilityVersion();
        return out;
    }
};

// Original particle effects (src/fx, docs/formats/EFFECTS.md): every loaded unit's intros run their object's birth
// effects (enabled event-0 records, tru.exe 0x5BA480), simulated each game tick, drawn as sprites. Owner = intro uid.
// Combat: each shot plays its weapon object's "fire" clip events at the muzzle and its ammo's hit object (bullet_hit,
// ...) "idle" clip events at the impact, filtered by the hit surface's material; Lara's and the enemies' clips run
// their animation events (footstep dust, splashes, ...) on their posed bones. Owners < 0: transient.
struct EffectsView {
    std::optional<ou::fx::Library> library;
    std::map<uint16_t, std::string> names;
    ou::fx::System system;
    std::map<std::string, std::vector<int32_t>> unitOwners; // loaded unit -> its intros with running effects
    std::vector<ou::render::FxVertex> vertices;
    std::vector<ou::render::FxBatch> batches;
    // Effect lights (class 6, docs/formats/EFFECTS.md "Lights"): fixtures (endless lights born with placed objects)
    // join the scene's environment lights with their DTP falloff; the others (muzzle, impacts) take kLightSlots
    // reserved environment slots, drawn with a linear falloff [I] (the attenuation LUT is built per upload).
    static constexpr size_t kLightSlots = 4;
    static ou::scene::Light slotLight() {
        ou::scene::Light l;
        l.falloff.resize(256);
        for (int k = 0; k < 256; ++k) l.falloff[size_t(k)] = uint8_t(k < 255 ? 255 - k : 0);
        l.position[0] = l.position[1] = l.position[2] = 1e9f, l.radius = 1;
        return l;
    }
    void reserve(StreamedScene& s) const { s.reservedLights.insert(s.reservedLights.end(), kLightSlots, slotLight()); }
    std::vector<ou::fx::System::LiveLight> live; // scratch, reused
    // Fixtures for StreamedScene::fixtureLights: DTP type / cone / falloff / flicker, the record's colour and radius.
    std::vector<ou::scene::Light> fixtures() {
        std::vector<ou::scene::Light> out;
        system.lights(live);
        for (const auto& l : live) {
            if (!l.forever || l.owner < 0) continue;
            auto& s = out.emplace_back(slotLight());
            if (l.dtp) {
                s.type = l.dtp->type, s.coneInner = l.dtp->coneInner, s.coneOuter = l.dtp->coneOuter, s.nearWidth = l.dtp->nearWidth;
                s.flickerPeriod = l.dtp->flickerPeriod, s.flicker = l.dtp->flicker;
                if (l.dtp->falloff.size() == 256) s.falloff = l.dtp->falloff;
            }
            for (int i = 0; i < 3; ++i) s.position[i] = l.pos[i], s.direction[i] = l.dir[i], s.color[i] = l.colour[i];
            s.radius = std::max(1.0f, l.radius);
        }
        return out;
    }
    // dyn = gadget lights, then the kLightSlots transient effect lights (unused slots dark, far away): the order of
    // StreamedScene::reservedLights. Element-wise so the vectors' storage is reused frame to frame.
    void lightSlots(const std::vector<ou::scene::Light>& gadget, std::vector<ou::scene::Light>& dyn) {
        dyn.resize(gadget.size() + kLightSlots, slotLight());
        std::copy(gadget.begin(), gadget.end(), dyn.begin());
        system.lights(live);
        size_t n = gadget.size();
        for (const auto& l : live)
            if (!(l.forever && l.owner >= 0) && n < dyn.size()) {
                auto& s = dyn[n++];
                for (int i = 0; i < 3; ++i) s.position[i] = l.pos[i], s.color[i] = l.colour[i];
                s.radius = std::max(1.0f, l.radius);
                if (l.dtp) s.flickerPeriod = l.dtp->flickerPeriod, s.flicker = l.dtp->flicker;
                else s.flickerPeriod = 0;
            }
        for (; n < dyn.size(); ++n) dyn[n].position[0] = dyn[n].position[1] = dyn[n].position[2] = 1e9f, dyn[n].color[0] = dyn[n].color[1] = dyn[n].color[2] = 0;
    }

    void sync(const ou::cdc::BigfileSet& bf, const ou::game::World& w, std::vector<std::string>& warnings) {
        if (!library) {
            library.emplace(bf);
            if (const auto list = bf.read("\\tr8\\pc-w\\objectlist.txt"))
                names = ou::formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        }
        auto loaded = [&](const std::string& n) {
            return std::any_of(w.units().begin(), w.units().end(), [&](const auto& u) { return u.data.name == n; });
        };
        for (auto it = unitOwners.begin(); it != unitOwners.end();) {
            if (loaded(it->first)) { ++it; continue; }
            for (const int32_t uid : it->second) system.stopOwner(uid); // streamed out: emitters stop, particles finish
            it = unitOwners.erase(it);
        }
        for (const auto& u : w.units()) {
            if (unitOwners.contains(u.data.name) || !u.data.drm) continue;
            auto& owners = unitOwners[u.data.name];
            for (const auto& in : ou::formats::parseUnit(*u.data.drm).intros) {
                const auto n = names.find(in.objectId);
                if (n == names.end()) continue;
                const auto fx = library->get(n->second, &warnings);
                if (!fx) continue;
                glm::mat4 m;
                ou::formats::introMatrix(in, &m[0][0]);
                m[3] += glm::vec4(u.offset, 0);
                if (!fx->fades.empty()) intros[in.uniqueId] = {fx, m};
                if (library->flags(n->second) & ou::fx::kObjectNoBirthEffects) continue;
                if (system.startBirth(fx, m, in.uniqueId)) owners.push_back(in.uniqueId);
            }
            // Unit FX point sets (fires, heat haze without an object): owners 0x40000000 | set id [I: ids beside intros'].
            for (const auto& ps : library->pointSets(*u.data.drm, u.data.name, &warnings)) {
                const int32_t owner = 0x40000000 | ps.id;
                if (system.startPointSet(ps, glm::translate(glm::mat4(1), u.offset), owner)) owners.push_back(owner);
            }
        }
        // Combat / animation-event objects, loaded now so their textures reach the scene (addTextures): every weapon's
        // ammo hit objects too (loaded lazily at the first shot, their sprites drew white until the next rebuild).
        library->get("lara", &warnings);
        auto weapon = [&](const std::string& n) {
            library->get(n, &warnings);
            if (const auto ammo = names.find(library->dataU16(n, 0xE6)); ammo != names.end())
                for (const uint32_t off : {0x3Cu, 0xA8u})
                    if (const auto hit = names.find(library->dataU16(ammo->second, off)); hit != names.end()) library->get(hit->second, &warnings);
        };
        for (int k = 0; k < ou::game::kWeaponCount; ++k) weapon(std::string(ou::game::weaponObject(ou::game::Weapon(k))));
        for (const auto& e : w.enemies().all()) {
            library->get(e->type().name, &warnings);
            if (const auto n = names.find(e->type().weapon); n != names.end()) weapon(n->second);
        }
    }
    // Rigid frame at p: muzzle (axis 1, -Y along the shot, Z up) or impact (axis 2, Z = surface normal).
    static glm::mat4 frameAlong(glm::vec3 p, glm::vec3 d, int axis) {
        const glm::vec3 up = std::abs(d.z) < 0.9f ? glm::vec3(0, 0, 1) : glm::vec3(1, 0, 0);
        glm::mat4 m(1);
        if (axis == 1) {
            const glm::vec3 x = glm::normalize(glm::cross(d, up));
            m[0] = glm::vec4(x, 0), m[1] = glm::vec4(d, 0), m[2] = glm::vec4(glm::cross(x, d), 0);
        } else {
            const glm::vec3 x = glm::normalize(glm::cross(up, d));
            m[0] = glm::vec4(x, 0), m[1] = glm::vec4(glm::cross(d, x), 0), m[2] = glm::vec4(d, 0);
        }
        m[3] = glm::vec4(p, 1);
        return m;
    }
    // Terrain material of the collision face under / at a hit (face +0x10; tru.exe copies it to instance +0x186).
    static uint8_t material(const ou::collision::CollisionMesh& col, const ou::collision::Hit& h) {
        return h.dynamic < 0 && h.face < col.faces.size() ? col.faces[h.face].material : 0;
    }
    // One bullet: the weapon's "fire" events with its muzzle marker (-10) on `from`, -Y along the shot (tru.exe weapon
    // instance anim events), then the ammo's hit object (ammo data +0x3C, +0xA8 under water: 0x6C64D0 -> 0x6B7030)
    // at the impact with Z on the normal, filtered by the surface material (bodies: 23, flesh / blood [I]).
    void shot(const ou::game::World& w, const std::string& weapon, glm::vec3 from, glm::vec3 to, bool body, bool hit, bool wet) {
        const glm::vec3 d = to - from;
        const float len = glm::length(d);
        if (len < 1) return;
        const glm::vec3 dir = d / len;
        if (const auto gun = library->get(weapon))
            if (const auto* fire = gun->clip(wet ? "fire_submerged" : "fire"); fire || (fire = gun->clip("fire")))
                system.play(gun, *fire, frameAlong(from, -dir, 1) * glm::inverse(gun->frame(-10)), -1, 0, to); // tracers end at -100 [I]
        if (!hit) return;
        const auto ammo = names.find(library->dataU16(weapon, 0xE6)); // weapon data +0xE6: ammo object (game::loadWeapon)
        if (ammo == names.end()) return;
        const auto obj = names.find(library->dataU16(ammo->second, wet ? 0xA8 : 0x3C));
        const auto fx = obj == names.end() ? nullptr : library->get(obj->second);
        const auto* idle = fx ? fx->clip("idle") : nullptr;
        if (!idle) return;
        glm::vec3 at = to, normal = -dir;
        uint8_t mat = 23;
        if (!body)
            if (const auto h = w.collision().raycast(from, dir, len + 20.0f)) at = h->point, normal = h->normal, mat = material(w.collision(), *h);
        system.play(fx, *idle, frameAlong(at, normal, 2), -1, mat);
    }
    std::string layerClip; // last tick's combat layer clip (World::combatLayerClip)
    double layerTime = 0;
    // Clip events of one animated instance between the last tick and this one (30 Hz frames, looping clips wrap).
    void clipEvents(const std::shared_ptr<const ou::fx::ObjectFx>& fx, const std::string& clip, const std::string& prevClip,
                    double prevT, double t, double duration, uint8_t mat, const glm::mat4& world,
                    const std::function<std::vector<glm::mat4>()>& bones, int32_t owner = -1) {
        const auto* ev = fx ? fx->clip(clip) : nullptr;
        if (!ev || duration <= 0) return;
        const float f1 = float(std::fmod(t, duration) * 30.0);
        float f0 = clip == prevClip ? float(std::fmod(prevT, duration) * 30.0) : -1.0f;
        std::vector<std::pair<float, float>> spans{{f0, f1}};
        if (f1 < f0) spans = {{f0, 1e9f}, {-1.0f, f1}}; // wrapped
        if (f0 < 0 && f1 <= 0) spans = {{-1.0f, 0.5f}};  // just started
        std::vector<glm::mat4> b;
        for (const auto& [a, z] : spans)
            if (std::any_of(ev->begin(), ev->end(), [&](const auto& e) {
                    return (e.runsOn(mat) || e.soundOn(mat)) && float(e.frame) >= a && float(e.frame) < z;
                })) {
                if (b.empty()) b = bones();
                system.runEvents(fx, *ev, world, owner, mat, a, z, b);
            }
    }
    // Owners of animated instances whose ribbons / lightning follow their segments (fx::System::track): Lara, enemies
    // (transient: < 0) and script objects (their intro uid, as their birth effects).
    static constexpr int32_t kLaraOwner = -2;
    static int32_t enemyOwner(int32_t uid) { return -(0x10000000 | (uid & 0x0FFFFFFF)); }
    static bool follows(const std::shared_ptr<const ou::fx::ObjectFx>& fx) { return fx && (!fx->ribbons.empty() || !fx->bolts.empty()); }
    std::map<int32_t, std::pair<std::string, double>> objectClipAt; // script object uid -> last clip, time
    // Placed objects with class-7 fades (rewards, keys): intro uid -> effects, world matrix.
    std::map<int32_t, std::pair<std::shared_ptr<const ou::fx::ObjectFx>, glm::mat4>> intros;
    // A reward picked up: its "pickup" list's variant-1 records (tru.exe 0x5B26C0 with variant 0x400 [I: the pickup
    // script's request]) start its fade out (class 7: 0 -> 1 over 35 frames); it is hidden when the fade ends.
    void pickedUp(int32_t uid) {
        const auto it = intros.find(uid);
        if (it == intros.end()) return;
        if (const auto* list = it->second.first->clip("pickup")) system.runVariant(it->second.first, *list, it->second.second, uid, 1);
    }
    void events(const ou::game::World& w) {
        if (!library) return;
        const auto& c = w.combat();
        const std::string gun(ou::game::weaponObject(c.weapon));
        for (const auto& s : c.shots) shot(w, gun, s.from, s.to, s.target != nullptr, s.hit, w.submerged());
        for (const auto& s : w.enemies().shots) {
            const auto e = std::find_if(w.enemies().all().begin(), w.enemies().all().end(), [&](const auto& x) { return x->uid() == s.uid; });
            const auto n = e == w.enemies().all().end() ? names.end() : names.find((*e)->type().weapon);
            if (n != names.end()) shot(w, n->second, s.from, s.to, s.hit, true, false);
        }
        auto floorMaterial = [&](glm::vec3 p) -> uint8_t {
            const auto h = w.collision().groundProbe(p + glm::vec3(0, 0, 50), 250);
            return h ? material(w.collision(), *h) : 0;
        };
        const auto &ps = w.player().state(), &pp = w.previous();
        if (const auto* clip = w.anims().find(ps.clip)) {
            // Lara's material (0x6F06B0): the floor face's, 2 with the feet under a water surface [I: swimming], 3 under water [I].
            const uint8_t mat = w.submerged() ? 3 : ps.water >= 0 ? 2 : floorMaterial(ps.pos);
            // Vehicle clips ("object:clip", e.g. motorcycle mount / dismount) run that object's lists.
            const auto colon = ps.clip.find(':');
            const auto split = [&](const std::string& c) { return c.find(':') == std::string::npos ? c : c.substr(c.find(':') + 1); };
            const auto lara = library->get(colon == std::string::npos ? "lara" : ps.clip.substr(0, colon));
            const glm::mat4 model = w.player().modelMatrix(ps.pos, ps.yaw, ps.pitch);
            std::vector<glm::mat4> pose;
            if (follows(lara)) pose = w.pose(1.0), system.track(kLaraOwner, model, pose);
            clipEvents(lara, split(ps.clip), pp.clip.substr(0, pp.clip.find(':')) == ps.clip.substr(0, colon) ? split(pp.clip) : std::string(),
                       pp.clipTime, ps.clipTime, clip->durationSeconds(), mat, model, [&] { return pose.empty() ? w.pose(1.0) : pose; },
                       kLaraOwner);
        }
        // The combat upper-body clip (reload / equip / unequip): Lara's and the weapon object's lists for that clip
        // name (tru.exe keeps weapon-handling sounds in the weapon object's lists, e.g. wp_handgun "reload").
        if (auto [name, t] = w.combatLayerClip(); !name.empty() || !layerClip.empty()) {
            const auto m = w.player().modelMatrix(ps.pos, ps.yaw, ps.pitch);
            for (const auto& obj : {std::string("lara"), std::string(ou::game::weaponObject(c.weapon))})
                clipEvents(library->get(obj), name, layerClip, layerTime, t, 1e9, 0, m, [&] { return w.pose(1.0); });
            layerClip = std::move(name), layerTime = t;
        }
        // Mjolnir's swing / ground pound: wp_thorhammer's events of Lara's clip run on Lara (her segments: the ground
        // pound's lightning arcs from her root, wp_thorhammer bolt 1), frames (prevT, t].
        if (const auto& h = c.hammer; !h.clip.empty())
            if (const auto ham = library->get("wp_thorhammer"))
                if (const auto* ev = ham->clip(h.clip)) {
                    const glm::mat4 model = w.player().modelMatrix(ps.pos, ps.yaw, ps.pitch);
                    const auto pose = w.pose(1.0);
                    if (follows(ham)) system.track(kLaraOwner, model, pose);
                    system.runEvents(ham, *ev, model, kLaraOwner, w.submerged() ? 3 : 0, h.prevT * 30 + 1e-3f, h.t * 30 + 1e-3f, pose);
                }
        for (const auto& e : w.enemies().all()) {
            const auto &s = e->state(), &p = e->previous();
            const auto it = e->type().clips.find(s.clip);
            if (it == e->type().clips.end()) continue;
            const auto fx = library->get(e->type().name);
            const glm::mat4 model = ou::game::ai::Enemy::modelMatrix(s.pos, s.yaw, s.tip, s.tilt);
            std::vector<glm::mat4> pose;
            if (follows(fx)) pose = e->pose(1.0), system.track(enemyOwner(e->uid()), model, pose);
            clipEvents(fx, s.clip, p.clip, p.clipTime, s.clipTime, it->second.durationSeconds(), floorMaterial(s.pos), model,
                       [&] { return pose.empty() ? e->pose(1.0) : pose; }, enemyOwner(e->uid()));
        }
        // Script objects (switches, doors, uber objects): their clips' events as the characters' (tru.exe runs every
        // instance's animation events, 0x5BA0D0), owner = intro uid.
        std::map<int32_t, std::pair<std::string, double>> seen;
        for (const auto& oc : w.objectClips()) {
            const auto fx = library->get(oc.object);
            const auto* ev = fx ? fx->clip(oc.clip) : nullptr;
            const auto last = objectClipAt.find(oc.uid);
            seen[oc.uid] = {oc.clip, oc.t};
            if (!ev) continue;
            std::vector<glm::mat4> pose;
            if (follows(fx)) pose = w.objectBones(oc.uid), system.track(oc.uid, oc.model, pose);
            const bool same = last != objectClipAt.end() && last->second.first == oc.clip;
            clipEvents(fx, oc.clip, same ? oc.clip : std::string(), same ? last->second.second : 0.0, oc.t,
                       oc.loop ? std::max(oc.duration, 1e-3) : 1e9, 0, oc.model, [&] { return pose.empty() ? w.objectBones(oc.uid) : pose; }, oc.uid);
        }
        objectClipAt = std::move(seen);
    }
    // Particle textures the level scene does not already hold (renderers look textures up by id).
    void addTextures(ou::scene::SceneData& scene) const {
        if (!library) return;
        std::set<uint32_t> have;
        for (const auto& t : scene.textures) have.insert(t.id);
        for (const auto& t : library->textures())
            if (have.insert(t.id).second) scene.textures.push_back(t);
    }
    ou::render::ScreenFilter filter; // class-5 screen filters (docs/formats/EFFECTS.md "Screen filters")
    void frame(const ou::render::Camera& cam, float aspect, const ou::game::World& w, ou::render::FrameOptions& fo) {
        system.build(cam, aspect, vertices, batches);
        fo.fxVertices = vertices, fo.fxBatches = batches;
        // Sun-glare filters fade while the owner point is occluded (0x535C50): a collision ray from the camera [I].
        const glm::mat4 viewProj = cam.projection(aspect) * cam.view();
        system.screenFilter(cam.position, w.player().state().pos, filter, &viewProj, [&](const glm::vec3& p) {
            const glm::vec3 d = p - cam.position;
            const float len = glm::length(d);
            return len < 1 || !w.collision().raycast(cam.position, d / len, len - 1.0f).has_value();
        });
        fo.screenFilter = filter.active() ? &filter : nullptr;
    }
};

// Gadgets (GAMEPLAY.md "Flashlight", "Grenades"): g_personal_light's effect lights (its spot and omni) plus an explosion
// flash [I] are the scene's first environment lights (StreamedScene::reservedLights), moved every frame through
// FrameOptions::dynamicLights; pr_grenade's mesh is posed at the live grenades. Logs toggles, throws and explosions.
struct GadgetView {
    static constexpr int kSlots = 5; // grenades drawn at once (tru.exe tracks 5 attached ones, 0xED0128 cleared by 0x6988E0)
    std::vector<ou::scene::Light> base, lights; // spot, omni, flash
    std::vector<ou::scene::Vertex> scratch;
    glm::vec3 flashAt{0};
    uint64_t flashTick = 0;
    bool lightOn = false, any = false;
    int carried = -1;

    void load(const ou::cdc::BigfileSet& bf, StreamedScene& streamed, std::vector<std::string>& warnings) {
        std::vector<ou::scene::Light> found;
        if (auto b = bf.read("pc-w\\g_personal_light.drm")) found = ou::formats::objectLights(ou::cdc::parseDrm(std::move(*b)));
        const auto spot = std::find_if(found.begin(), found.end(), [](const auto& l) { return l.type == 1; });
        const auto omni = std::find_if(found.begin(), found.end(), [](const auto& l) { return l.type == 0; });
        if (spot == found.end() || omni == found.end()) {
            warnings.push_back("pc-w\\g_personal_light.drm: no spot / omni light decoded; the flashlight is dark");
        } else {
            base = {*spot, *omni, *omni};
            auto& flash = base[2]; // [I] grenade blast flash: the omni's falloff, orange, explosion-radius reach
            flash.radius = 2500.0f, flash.flicker.clear(), flash.flickerPeriod = 0;
            flash.color[0] = 3.0f, flash.color[1] = 1.8f, flash.color[2] = 0.7f;
            for (auto& l : base) l.position[0] = l.position[1] = l.position[2] = 1e9f; // off until update()
            lights = base;
            streamed.reservedLights = base;
        }
        auto sc = ou::formats::loadDrmScene(bf, "pc-w\\pr_grenade.drm");
        if (sc.meshes.empty()) return warnings.push_back("pc-w\\pr_grenade.drm: no mesh decoded");
        streamed.grenades.textures = std::move(sc.textures), streamed.grenades.materials = std::move(sc.materials);
        streamed.grenades.shaders = std::move(sc.shaders);
        for (int k = 0; k < kSlots; ++k) {
            auto m = sc.meshes[0]; // CPU-posed in world space, as the guns
            m.skeleton = -1, m.skin.clear();
            for (int i = 0; i < 16; ++i) m.transform[i] = i % 5 == 0 ? 1.0f : 0.0f;
            streamed.grenades.meshes.push_back(std::move(m));
        }
    }

    void tick(const ou::game::World& w) {
        const auto tick = (unsigned long long)w.ticks();
        if (w.flashlight != lightOn)
            std::fprintf(stderr, "ou-game: tick %llu flashlight %s (%s)\n", tick, w.flashlight ? "on" : "off", w.flashlightSpot() ? "spot + omni" : "omni");
        if (carried >= 0 && w.grenades < carried) std::fprintf(stderr, "ou-game: tick %llu grenade thrown, %d left\n", tick, w.grenades);
        for (const auto& e : w.explosions()) {
            std::fprintf(stderr, "ou-game: tick %llu grenade exploded at %.0f %.0f %.0f (health %.0f)\n", tick, e.x, e.y, e.z, w.health().value());
            flashAt = e, flashTick = w.ticks(), any = true;
        }
        lightOn = w.flashlight, carried = w.grenades;
    }

    const std::vector<ou::scene::Light>& update(const ou::game::World& w, double alpha) {
        if (base.empty()) return lights;
        lights = base; // same sizes: no allocation
        const auto [pos, dir] = w.flashlightPose(alpha);
        for (int i = 0; i < 2; ++i) {
            if (!w.flashlight || (i == 0 && !w.flashlightSpot())) continue; // omni alone outside spot units (0x6B17C0)
            auto& l = lights[size_t(i)];
            for (int c = 0; c < 3; ++c) l.position[c] = pos[c], l.direction[c] = dir[c];
        }
        const float f = any ? 1.0f - float((double(w.ticks()) + alpha) - double(flashTick)) / 18.0f : 0.0f; // [I] 0.3 s fade
        if (f > 0) {
            auto& l = lights[2];
            for (int c = 0; c < 3; ++c) l.position[c] = flashAt[c], l.color[c] = base[2].color[c] * f;
        }
        return lights;
    }

    void pose(const ou::game::World& w, const StreamedScene& streamed, const ou::scene::SceneData& scene, ou::render::Renderer& r) {
        const auto& live = w.liveGrenades();
        for (size_t k = 0; k < streamed.grenadeIndices.size(); ++k) {
            const size_t i = streamed.grenadeIndices[k];
            if (k < live.size()) {
                ou::anim::skinVertices(scene.meshes[i], {}, scratch, glm::translate(glm::mat4(1), live[k].pos));
            } else {
                scratch = scene.meshes[i].vertices;
                for (auto& v : scratch) v.position[0] = v.position[1] = v.position[2] = 0;
            }
            r.updateVertices(i, scratch);
        }
    }
};

// Quick save / load (slot "quick", src/save) with a log line replays compare.
void logState(const char* what, const ou::game::World& w) {
    const auto& s = w.player().state();
    const auto& u = w.currentUnit();
    std::fprintf(stderr, "ou-game: %s: unit %s state %s pos %.3f %.3f %.3f (unit frame) yaw %.4f clip %s t %.4f checkpoint %d\n", what,
                 u.data.name.c_str(), std::string(ou::game::stateName(s.state)).c_str(), s.pos.x - u.offset.x, s.pos.y - u.offset.y,
                 s.pos.z - u.offset.z, s.yaw, s.clip.c_str(), s.clipTime, w.checkpoint() ? w.checkpoint()->id : 0);
}
bool quickSave(const ou::game::World& w, const std::string& slot) {
    try {
        const auto path = ou::save::slotPath(slot);
        ou::save::writeFile(path, ou::save::serialize(ou::save::capture(w)));
        logState(("saved " + path.string()).c_str(), w);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-game: save failed: %s\n", e.what());
        return false;
    }
}
bool quickLoad(ou::game::World& w, const std::string& slot) {
    try {
        const auto path = ou::save::slotPath(slot);
        bool backup = false;
        ou::save::apply(ou::save::readFile(path, &backup), w);
        logState(("loaded " + path.string() + (backup ? " (from .bak)" : "")).c_str(), w);
        return true;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-game: load failed: %s\n", e.what());
        return false;
    }
}

// Music (docs/formats/AUDIO.md §5, GAMEPLAY.md "Music"): one track at a time, pc-w\music\<name>\<name>.mul
// streamed with the stem gains its .mus script computes (audio::MusSequencer). Scripts request tracks with `music`
// natives (script::Runtime::MusicRequest: unit scripts called from soundvolumes, cinematics); the exe has two slots
// (0xCC5260 + slot * 0x528) that fade independently, ours one voice: a request in another slot crossfades, a stop
// fades out only the slot that holds the playing track. Without any request the unit's own "<unit>_mus_01" plays [I].
// The music variables are the script runtime's gamebase values (tru.exe keeps one array, 0xCCB560, for scripts and
// the sequencer; AUDIO.md §5.3, §9.4).
struct Music {
    std::string current;
    int slot = 0; // the slot the playing track was requested in
    std::unique_ptr<ou::audio::MusFile> mus; // the sequencer keeps a reference to it
    std::unique_ptr<ou::audio::MusSequencer> seq;
    std::array<uint8_t, 256>* vars = nullptr; // world->script().gameValues()

    bool play(const ou::cdc::BigfileSet& bf, ou::audio::Mixer& mixer, const std::string& name, const std::string& why, uint64_t tick,
              float fade = 2, int inSlot = 0) {
        if (name.empty() || name == current) return false;
        slot = inSlot;
        const std::string base = "pc-w\\music\\" + name + "\\" + name;
        try {
            auto mul = bf.read(base + ".mul");
            if (!mul) throw ou::cdc::FormatError(base + ".mul not in bigfile");
            auto pcm = std::make_shared<ou::audio::Pcm>(ou::audio::decodeMul(*mul).pcm);
            seq.reset(), mus.reset();
            if (auto m = bf.read(base + ".mus")) {
                mus = std::make_unique<ou::audio::MusFile>(ou::audio::parseMus(*m));
                seq = std::make_unique<ou::audio::MusSequencer>(*mus);
                seq->update(0, *vars);
            }
            mixer.playMusic(std::move(pcm), current.empty() ? 1.0f : fade, seq ? seq->stemGains() : std::vector<float>{});
            std::fprintf(stderr, "ou-game: tick %llu music %s (%s)%s\n", (unsigned long long)tick, name.c_str(), why.c_str(),
                         seq ? "" : " no .mus");
            current = name;
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: music %s: %s\n", name.c_str(), e.what());
            current = name; // do not retry every tick
            return false;
        }
    }
    void stop(ou::audio::Mixer& mixer, float fade, const std::string& why, uint64_t tick, int inSlot = -1) {
        if (current.empty() || (inSlot >= 0 && inSlot != slot)) return;
        mixer.stopMusic(fade);
        std::fprintf(stderr, "ou-game: tick %llu music %s stops (%s, %.1f s fade)\n", (unsigned long long)tick, current.c_str(), why.c_str(), fade);
        current.clear(), seq.reset(), mus.reset();
    }
    void tick(ou::audio::Mixer& mixer, float dt) {
        if (!seq) return;
        seq->update(dt, *vars);
        mixer.setMusicStemGains(seq->stemGains());
    }
};

// 16-bit stereo PCM WAV of interleaved float frames.
bool writeWav(const std::filesystem::path& path, const std::vector<float>& lr, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };
    const auto bytes = uint32_t(lr.size() * 2);
    f.write("RIFF", 4), u32(36 + bytes), f.write("WAVEfmt ", 8), u32(16), u16(1), u16(2), u32(rate), u32(rate * 4), u16(4), u16(16);
    f.write("data", 4), u32(bytes);
    for (const float s : lr) u16(uint16_t(int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767))));
    return bool(f);
}

// Save slots in saveDir(), newest first, with where and when each was saved (ui::SaveSlot). The autosave is named after
// its place (a Treasure Hunt's "<AREA> - TREASURE HUNT"); "savegame-<n>" shows as "Savegame <n>" (the original's default
// name), other slots by their file name.
std::vector<ou::ui::SaveSlot> listSaves() {
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ou::save::saveDir(), ec))
        if (e.path().extension() == ".ousave") v.emplace_back(e.last_write_time(ec), e.path().stem().string());
    std::sort(v.begin(), v.end(), std::greater<>());
    std::vector<ou::ui::SaveSlot> out;
    for (auto& [t, s] : v) {
        ou::ui::SaveSlot slot{.slot = s, .autosave = s == ou::ui::kAutosaveSlot};
        if (!slot.autosave) slot.name = s.starts_with("savegame-") ? "Savegame " + s.substr(9) : s;
        try {
            const auto g = ou::save::readFile(ou::save::slotPath(s));
            const int c = std::clamp(g.chapter, 0, int(ou::game::kChapters.size()) - 1);
            slot.area = ou::game::kChapters[size_t(c)].nameId, slot.title = g.treasureHunt ? 49 : ou::ui::levelTitle(c, g.unit); // [I] "- TREASURE HUNT"
        } catch (const std::exception&) {
        }
        const std::time_t tt = std::chrono::system_clock::to_time_t(std::chrono::file_clock::to_sys(t));
        char time[16], date[16];
        std::strftime(time, sizeof time, "%H:%M:%S", std::localtime(&tt));
        std::strftime(date, sizeof date, "%d-%m-%Y", std::localtime(&tt));
        slot.time = time, slot.date = date;
        out.push_back(std::move(slot));
    }
    return out;
}

// Menu navigation from replay actions (press edges): forward/right +-1 move, jump/interact accept, drop back.
std::optional<ou::ui::Nav> scriptNav(const ou::game::Actions& a, const ou::game::Actions& was) {
    using ou::ui::Nav;
    auto edge = [](bool now, bool before) { return now && !before; };
    if (edge(a.move.y > 0.5f, was.move.y > 0.5f)) return Nav::Up;
    if (edge(a.move.y < -0.5f, was.move.y < -0.5f)) return Nav::Down;
    if (edge(a.move.x > 0.5f, was.move.x > 0.5f)) return Nav::Right;
    if (edge(a.move.x < -0.5f, was.move.x < -0.5f)) return Nav::Left;
    if (edge(a.jump, was.jump)) return Nav::Accept;
    if (edge(a.interact, was.interact)) return Nav::Alt;
    if (edge(a.drop, was.drop) || edge(a.pause, was.pause)) return Nav::Back;
    return std::nullopt;
}
std::optional<ou::ui::Nav> keyNav(SDL_Keycode k) {
    using ou::ui::Nav;
    switch (k) {
    case SDLK_UP: case SDLK_W: return Nav::Up;
    case SDLK_DOWN: case SDLK_S: return Nav::Down;
    case SDLK_LEFT: case SDLK_A: return Nav::Left;
    case SDLK_RIGHT: case SDLK_D: return Nav::Right;
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return Nav::Accept;
    case SDLK_E: return Nav::Alt; // Accept except on the PDA's Field Assistance (TASK)
    case SDLK_ESCAPE: return Nav::Back;
    case SDLK_BACKSPACE: case SDLK_DELETE: return Nav::Delete; // Save/Load slots; Back elsewhere
    default: return std::nullopt;
    }
}
std::optional<ou::ui::Nav> padNav(uint8_t b) {
    using ou::ui::Nav;
    switch (b) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return Nav::Up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return Nav::Down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return Nav::Left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return Nav::Right;
    case SDL_GAMEPAD_BUTTON_SOUTH: return Nav::Accept;
    case SDL_GAMEPAD_BUTTON_WEST: return Nav::Alt;
    case SDL_GAMEPAD_BUTTON_NORTH: return Nav::Delete;
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START: return Nav::Back;
    default: return std::nullopt;
    }
}

class MenuMovie {
public:
    explicit MenuMovie(std::filesystem::path file) : file_(std::move(file)) { restart(); }

    ou::render::RgbaImage update() {
        const double duration = movie_->info().duration;
        double t = duration > 0 ? std::fmod(double(SDL_GetTicksNS() - started_) * 1e-9, duration) : 0;
        if (t + 0.001 < previous_) restart();
        const double frameTime = movie_->info().fps > 0 ? 1.0 / movie_->info().fps : 1.0 / 30;
        while (!have_ || frame_.pts + frameTime <= t) {
            if (!movie_->next(frame_, audio_)) {
                restart();
                t = 0;
                continue;
            }
            have_ = true;
            ++version_;
        }
        previous_ = t;
        return {frame_.rgba, movie_->info().width, movie_->info().height, version_};
    }

private:
    void restart() {
        movie_ = std::make_unique<ou::video::Movie>(file_, -1); // menu visuals; movie audio is not mixed into the game
        frame_ = {};
        have_ = false;
        previous_ = 0;
    }

    std::filesystem::path file_;
    std::unique_ptr<ou::video::Movie> movie_;
    ou::video::VideoFrame frame_;
    std::vector<float> audio_;
    uint64_t started_ = SDL_GetTicksNS(), version_ = 0;
    double previous_ = 0;
    bool have_ = false;
};

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }
    const bool menuStart = opt.level.empty() && opt.chapter < 0 && opt.loadSlot.empty() && opt.cine.empty();

    // ---- load: simulation data first (no window needed to fail early) ----
    std::optional<ou::game::InputScript> script;
    std::unique_ptr<const ou::cdc::BigfileSet> bigfile; // outlives `world`: its unit loader reads from it
    std::unique_ptr<ou::game::World> world;
    std::optional<ou::locale::Strings> strings;
    std::optional<ou::ui::Art> art; // generalbank.drm menu art and font
    ou::scene::SceneData scene;
    StreamedScene streamed;
    LaraMeshes lara;
    EnemyMeshes enemyMeshes;
    ScriptedMeshes objects;
    std::vector<std::string> warnings;
    CombatView combat;
    GadgetView gadgets;
    EffectsView effects;
    effects.system.visuals = opt.gfx.effects; // --fx off: no particles / ribbons / lightning / glows / lights / filters
    std::vector<ou::scene::Light> dynamicLights; // gadget + effect light slots, per frame
    bool spawnFromIntro = false;
    try {
        if (!opt.inputScript.empty()) script = ou::game::InputScript::load(opt.inputScript);
        bigfile = std::make_unique<const ou::cdc::BigfileSet>(opt.gameDir);
        const auto& bf = *bigfile;
        strings = ou::locale::loadStrings(opt.gameDir, ou::locale::Language::English);
        if (!strings) throw std::runtime_error("pc-w\\local\\locals.bin not in bigfile");
        std::string why;
        art = ou::ui::loadArt(bf, &why);
        if (!art) throw std::runtime_error("menu art: " + why);
        // "pc-w\\l4_pool.drm" -> unit "l4_pool"; its portal neighbours stream in with it (World, LEVELS.md). The main
        // menu shows (and New Game starts) chapter 0's unit.
        std::string unit = opt.level.substr(opt.level.find_last_of('\\') + 1);
        if (unit.ends_with(".drm")) unit.resize(unit.size() - 4);
        if (opt.level.empty()) unit = std::string(ou::game::kChapters[size_t(std::max(opt.chapter, 0))].unit);
        if (!opt.cine.empty()) { // the cinematic's own unit (CINE header)
            const auto mul = bf.read("pc-w\\cinstream\\" + opt.cine + ".mul");
            if (!mul) throw std::runtime_error("pc-w\\cinstream\\" + opt.cine + ".mul not in bigfile");
            unit = ou::cine::parseHeader(ou::cine::demux(*mul)).unitName;
            std::transform(unit.begin(), unit.end(), unit.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
        }
        std::optional<ou::game::Spawn> spawn;
        if (opt.spawn) spawn = ou::game::Spawn{{(*opt.spawn)[0], (*opt.spawn)[1], (*opt.spawn)[2]}, glm::radians((*opt.spawn)[3])};
        world = std::make_unique<ou::game::World>([&bf](const std::string& n) { return ou::game::loadUnitData(bf, n); }, unit,
                                                  ou::game::loadLaraAnims(bf), spawn);
        world->fovDeg = opt.fov;
        world->script().setBigfile(&bf);
        world->enemies().setBigfile(&bf);
        world->chapter = opt.level.empty() ? std::max(opt.chapter, 0) : std::max(0, ou::game::chapterOfUnit(unit));
        spawnFromIntro = !opt.spawn && world->currentUnit().data.spawn && world->currentUnit().data.spawn->fromIntro;
        world->script().setPlayerObjects(ou::game::chapterGear(world->chapter)); // a direct chapter start
        world->chapterLoadout(world->chapter);
        if (opt.level.empty() && opt.chapter >= 0 && opt.loadSlot.empty()) world->announceChapter(); // its intro (World::startChapter)
        if (!opt.loadSlot.empty() && !quickLoad(*world, opt.loadSlot)) return 1;

        streamed.dress(bf, *world, warnings);
        // Weapons: each wp_* DRM's first mesh (the gun; wp_handgun's others are a low-detail copy and a small part):
        // pistols and uzi once per hand, shotgun and speargun once; posed in model space on the CPU (CombatView::pose).
        for (int i = 0; i < ou::game::kWeaponCount; ++i) {
            const auto wpn = ou::game::Weapon(i);
            const std::string file = "pc-w\\" + std::string(ou::game::weaponObject(wpn)) + ".drm";
            auto gunScene = ou::formats::loadDrmScene(bf, file);
            if (gunScene.meshes.empty()) {
                warnings.push_back(file + ": no mesh decoded");
                continue;
            }
            for (auto& t : gunScene.textures) streamed.guns.textures.push_back(std::move(t));
            for (auto& m : gunScene.materials) streamed.guns.materials.push_back(std::move(m));
            for (auto& c : gunScene.shaders) streamed.guns.shaders.push_back(std::move(c));
            for (int hand = 0; hand < (wpn <= ou::game::Weapon::Uzi ? 2 : 1); ++hand) {
                auto m = gunScene.meshes[0]; // CPU-posed in world space: identity transform, no skin
                m.skeleton = -1, m.skin.clear();
                for (int k = 0; k < 16; ++k) m.transform[k] = k % 5 == 0 ? 1.0f : 0.0f;
                streamed.guns.meshes.push_back(std::move(m));
                streamed.gunSlots.emplace_back(wpn, hand);
            }
        }
        for (const auto& t : opt.targets) {
            auto& d = combat.dummies.emplace_back(std::make_unique<ou::game::DummyTarget>());
            d->pos = glm::vec3(t[0], t[1], t[2]) + world->currentUnit().offset;
            world->targets.push_back(d.get());
        }
        if (opt.weapon) // testing: this secondary with its max reserve, whatever the chapter's
            world->giveWeapon(*opt.weapon, world->maxReserve(*opt.weapon));
        gadgets.load(bf, streamed, warnings);
        effects.reserve(streamed);
        effects.sync(bf, *world, warnings);
        streamed.fixtureLights = effects.fixtures();
        scene = streamed.build(bf, *world, lara, enemyMeshes, warnings);
        effects.addTextures(scene);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-game: %s\n", e.what());
        return 1;
    }
    for (auto& w : world->warnings) warnings.push_back(std::move(w));
    world->warnings.clear();

    ou::platform::SdlInit sdl(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
    if (!sdl.ok) {
        std::fprintf(stderr, "ou-game: SDL_Init failed (%s); no display, skipping\n", SDL_GetError());
        return kExitSkip;
    }
    // Movies get their own plain window: the movie player needs one without a GL/Vulkan context.
    auto playMovie = [&](const char* file) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) std::fprintf(stderr, "ou-game: no audio (%s); %s plays silent\n", SDL_GetError(), file);
        ou::platform::WindowPtr movieWindow(SDL_CreateWindow("OpenUnderworld", opt.width, opt.height, opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        try {
            ou::video::PlayOptions po;
            po.maxFrames = opt.introFrames;
            const auto st = ou::video::playMovie(movieWindow.get(), opt.gameDir / "Movies" / file, po);
            std::fprintf(stderr, "ou-game: movie %s %d frames%s\n", file, st.frames, st.skipped ? " (skipped)" : "");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: movie %s: %s\n", file, e.what());
        }
    };
    if (opt.intro) playMovie("intro.bik");

    // ---- audio (ou_audio): music per unit / sound volume; sound events (GameSound) ----
    ou::audio::Mixer mixer; // after SdlInit: closes its device before SDL_Quit
    Music music;
    music.vars = &world->script().gameValues();
    std::optional<ou::app::GameSound> sound; // sfxmarkers, script sounds, drowning, voice subtitles
    std::vector<float> wav; // --music-wav capture
    bool audioOn = false;
    if (!opt.noAudio) {
        try {
            if (auto laraDrm = bigfile->read("pc-w\\" + world->outfit + ".drm")) mixer.addWaves(ou::cdc::parseDrm(std::move(*laraDrm)));
            audioOn = !opt.musicWav.empty() || mixer.openDevice();
            if (!audioOn) std::fprintf(stderr, "ou-game: no audio device (%s)\n", SDL_GetError());
            std::fprintf(stderr, "ou-game: audio %s\n", !opt.musicWav.empty() ? "to WAV" : audioOn ? "on" : "off");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: audio: %s\n", e.what());
        }
        try {
            sound.emplace(*bigfile, mixer);
            sound->attach(*world);
            if (effects.library) // Lara's and the weapons' sounds decoded up front (not on the first step / shot)
                for (int i = -1; i < ou::game::kWeaponCount; ++i)
                    if (const auto fx = effects.library->get(i < 0 ? "lara" : std::string(ou::game::weaponObject(ou::game::Weapon(i)))))
                        sound->addObject(*fx);
            effects.system.sound = [&](const ou::fx::ObjectFx& fx, const ou::fx::Event& e, glm::vec3 at, uint8_t mat) {
                sound->animSound(fx, e, at, mat, world->ticks());
            };
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: sound events: %s\n", e.what());
        }
    }
    ou::platform::WindowPtr window; // before the renderer: GL context / VkSurface die first
    std::string chosen, note, error;
    auto renderer = ou::render::selectRenderer(opt.renderer, ou::render::createBackend, chosen, note, error);
    if (!note.empty()) std::fprintf(stderr, "ou-game: %s\n", note.c_str());
    if (!renderer) {
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
    objects.index(scene);
    for (const auto& w : warnings) std::fprintf(stderr, "ou-game: warning: %s\n", w.c_str());
    auto debugLines = opt.showCollision ? collisionLines(world->collision()) : std::vector<ou::render::DebugLine>{};
    std::vector<ou::render::DebugLine> lineBuffers[2]; // per-frame lines (see the draw call)
    std::vector<ou::render::WaterVolume> waterVolumes;     // loaded units' water boxes, each frame
    std::vector<ou::render::WaterParticle> splashParticles; // game::WaterSim droplets, each frame
    std::vector<ou::render::MeshOpacity> meshOpacity;      // fading instances (class-7 fades), each frame
    // Enhanced weather (graphics.weather): the unit's own rain / snow objects (game::unitWeather), eased out while a
    // roof is above the camera.
    std::map<uint16_t, std::string> objectNames;
    if (const auto list = bigfile->read("\\tr8\\pc-w\\objectlist.txt"))
        objectNames = ou::formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    std::map<std::string, ou::game::UnitWeather> unitWeathers;
    float shelter = 0;
    const auto& sp = world->player().state();
    std::fprintf(stderr, "ou-game: %s | chapter %d | spawn %s at %.1f %.1f %.1f yaw %.1f\n", world->currentUnit().data.name.c_str(),
                 world->chapter,
                 !opt.loadSlot.empty() ? "--load" : opt.spawn ? "--spawn" : spawnFromIntro ? "'player' intro" : "first intro (no 'player' intro)",
                 sp.pos.x, sp.pos.y, sp.pos.z, glm::degrees(sp.yaw));
    auto logUnits = [&] {
        std::string names;
        for (const auto& u : world->units()) names += " " + u.data.name + fmt("@%.0f,%.0f,%.0f", u.offset.x, u.offset.y, u.offset.z);
        std::fprintf(stderr, "ou-game: tick %llu in unit %s | loaded:%s\n", (unsigned long long)world->ticks(),
                     world->currentUnit().data.name.c_str(), names.c_str());
    };
    logUnits();
    const auto dieTick = opt.die >= 0 ? uint64_t(std::llround(opt.die / ou::game::kTickSeconds)) : ~0ull;
    ou::game::Actions held, menuHeld; // previous tick's actions: save/load/pause and menu input react to press edges
    std::string unitName;             // music follows the current unit
    int bikeMounts = 0;                                    // World::bikeMounts seen (first mount: bike music)
    std::string bikePhase = "parked";                      // ridden bike phase seen, for logs
    std::vector<ou::scene::Vertex> bikeScratch;
    // Title card when the chapter changes (-2: not until New Game; --level/--load start without one).
    int shownChapter = menuStart ? -2 : opt.chapter >= 0 ? -1 : world->chapter;

    // ---- UI (src/ui): menus, PDA page, pickup notifications, chapter titles ----
    ou::ui::Menus menus;
    const auto rewards = ou::ui::loadRewards(*bigfile); // EXTRAS
    if (rewards) menus.rewards = &*rewards;
    ou::ui::Hud hud;
    std::optional<MenuMovie> menuMovie;
    auto ensureMenuMovie = [&] {
        if (menuMovie || opt.noMovies) return;
        try {
            menuMovie.emplace(opt.gameDir / "Movies" / "medsea_loop.bik");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: main menu movie: %s\n", e.what());
        }
    };
    // Menu settings: volumes, subtitles, reticles, helper/training toggles (config "game"), mouse/stick Y inversion and
    // brightness (0..10, 5 = neutral; graphics brightness 0.5 * 2^(v/5)). Changed ones go back to --config's file.
    menus.sfxVolume = opt.game.sfxVolume, menus.musicVolume = opt.game.musicVolume, menus.cinematicVolume = opt.game.cinematicVolume;
    menus.subtitles = opt.game.subtitles, menus.reticles = opt.game.reticles;
    menus.helperButtons = opt.game.helperButtons, menus.trainingText = opt.game.trainingText;
    menus.invertY = opt.controls.invertY;
    menus.brightness = std::clamp(int(std::lround(5 * std::log2(2 * opt.gfx.brightness))), 0, 10);
    int menuBrightness = menus.brightness; // the launcher's finer value stays until the slider moves
    auto applyVolumes = [&] {
        mixer.setBusVolume(ou::audio::Bus::Music, float(menus.musicVolume) / 10);
        mixer.setBusVolume(ou::audio::Bus::Sfx, float(menus.sfxVolume) / 10);
        opt.controls.invertY = menus.invertY;
        if (menus.brightness != menuBrightness) menuBrightness = menus.brightness, opt.gfx.brightness = 0.5f * std::exp2(float(menus.brightness) / 5);
        const ou::config::GameConfig g{menus.sfxVolume, menus.musicVolume, menus.cinematicVolume, menus.subtitles,
                                       menus.reticles, menus.helperButtons, menus.trainingText};
        if (opt.configPath.empty() || (g == opt.game && opt.controls.invertY == opt.configInvertY && opt.gfx.brightness == opt.graphics.brightness))
            return;
        try { // re-read: the launcher may have changed other keys meanwhile
            auto c = ou::config::load(opt.configPath);
            c.game = opt.game = g;
            c.controls.invertY = opt.configInvertY = opt.controls.invertY;
            c.graphics.brightness = opt.graphics.brightness = opt.gfx.brightness;
            ou::config::save(opt.configPath, c);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: saving settings to %s: %s\n", opt.configPath.string().c_str(), e.what());
            opt.configPath.clear();
        }
    };
    applyVolumes();
    auto chapterName = [&](int c) { return std::string(strings->get(ou::game::kChapters[size_t(c)].nameId)); };
    auto openPause = [&] {
        menus.saves = listSaves();
        menus.open(ou::ui::Screen::Pause);
        std::fprintf(stderr, "ou-game: tick %llu pause menu\n", (unsigned long long)world->ticks());
    };
    // PDA sonar maps by unit (pc-w\<unit>_map.drm; nullopt: the unit has none) and the 3D models its pages show.
    std::map<std::string, std::optional<ou::ui::SonarMap>> sonarMaps;
    auto sonarMap = [&](const std::string& unit) -> const ou::ui::SonarMap* {
        auto it = sonarMaps.find(unit);
        if (it == sonarMaps.end()) it = sonarMaps.emplace(unit, ou::ui::loadSonarMap(*bigfile, unit)).first;
        return it->second ? &*it->second : nullptr;
    };
    std::map<std::string, ou::ui::ViewMesh> viewMeshes;
    auto viewMesh = [&](const std::string& object) -> const ou::ui::ViewMesh& {
        auto it = viewMeshes.find(object);
        if (it == viewMeshes.end())
            it = viewMeshes.emplace(object, ou::ui::viewMesh(ou::formats::loadDrmScene(*bigfile, "pc-w\\" + object + ".drm"))).first;
        return it->second;
    };
    // What the PDA shows (ui::PdaInfo): the chapter's relics/treasures found / in it (game::chapterRewards), WEAPON
    // SELECTION's secondaries in the original's order with reserve / max, the INVENTORY pages [O strings 76-81,
    // 1241-1275; V captures screens/pda].
    auto fillPda = [&] {
        using ou::game::Weapon;
        const auto found = [&](const ou::game::ChapterRewards& r) {
            return std::pair{int(std::count_if(r.treasures.begin(), r.treasures.end(), [&](int32_t id) { return world->collected.contains(id); })),
                             r.relic && world->collected.contains(r.relic) ? 1 : 0};
        };
        const auto& r = ou::game::chapterRewards(world->chapter);
        const auto [treasures, relics] = found(r);
        ou::ui::PdaInfo& p = menus.pda;
        p = {world->chapter, relics, r.relic ? 1 : 0, treasures, int(r.treasures.size())};
        p.sonar = sonarMap(world->currentUnit().data.name) != nullptr;
        const auto& c = world->combat();
        struct Gun { Weapon w; int16_t name, text; };
        static constexpr Gun kGuns[] = {{Weapon::Uzi, 76, 1243}, {Weapon::Shotgun, 77, 1244}, {Weapon::AssaultRifle, 78, 1245},
                                        {Weapon::Tranquilizer, 80, 1246}, {Weapon::Speargun, 79, 1247}};
        for (const auto& g : kGuns)
            p.weapons.push_back({int(g.w), g.name, c.reserve[size_t(g.w)], world->maxReserve(g.w), c.secondary == g.w,
                                 std::string(ou::game::weaponObject(g.w))});
        const auto count = [](int n) { return " [" + std::to_string(n) + "]"; };
        p.inventory[0] = {{1274, count(world->found[2]), 1275, {}, 706, {}}}; // HEALTH PACKS [0] shown too [V inventory_view]
        if (world->grenades > 0) p.inventory[0].push_back({1248, count(world->grenades), 1249, {}, 0, "pr_grenade"});
        const auto& carried = world->script().playerObjects();
        p.inventory[1] = {{1260, {}, 1261, {}, 0, "g_grapple"}, {1262, {}, 1263, {}, 0, "g_personal_light"},
                          {1264, {}, 1265, {}, 0, "g_binoculars"}, {1266, {}, 1267, {}, 0, "g_tanks"}, {1268, {}, 1269, {}, 0, "g_pda"}};
        if (carried.contains("g_gauntletleft")) p.inventory[1].push_back({1270, {}, 1272, {}, 0, "g_gauntletleft"});
        if (carried.contains("g_belt")) p.inventory[1].push_back({1271, {}, 1273, {}, 0, "g_belt"});
        p.inventory[2] = {{1241, {}, 1242, {}, 0, "wp_handgun"}};
        for (const auto& g : kGuns) p.inventory[2].push_back({g.name, {}, g.text, {}, 0, std::string(ou::game::weaponObject(g.w))});
        if (world->hasHammer()) p.inventory[2].push_back({81, {}, 1250, {}, 0, "wp_thorhammer"});
        for (int ch = 1; ch <= 7; ++ch) { // TREASURES: each area's found / total [I layout]
            const auto& cr = ou::game::chapterRewards(ch);
            const auto [t, rl] = found(cr);
            const std::string body = std::string(strings->get(1251)) + "  " + std::to_string(t) + "/" + std::to_string(cr.treasures.size()) +
                                     "      " + std::string(strings->get(1252)) + "  " + std::to_string(rl) + "/" + (cr.relic ? "1" : "0");
            p.inventory[3].push_back({int16_t(1252 + ch), {}, 0, body, uint32_t(680 + ch), {}});
        }
    };
    auto openPda = [&] {
        fillPda();
        menus.treasureHunt = world->treasureHunt; // REVISIT LOCATION
        menus.journal = ou::ui::journalEntries(world->treasureHunt ? 7 : world->chapter);
        menus.open(ou::ui::Screen::Pda);
        std::fprintf(stderr, "ou-game: tick %llu pda\n", (unsigned long long)world->ticks());
    };
    ou::ui::Sonar sonar;
    ou::ui::SonarCamera sonarCam;
    auto resetSonar = [&] { sonarCam.reset(world->player().state().pos, world->player().state().yaw); };
    // What uiImages[1] was last built from: the PDA page's model panel, the sonar view.
    ou::ui::Screen shownScreen = ou::ui::Screen::None;
    std::optional<ou::ui::PdaModel> shownModel;
    std::string shownModelBg;
    ou::ui::Image pdaImage;
    uint64_t pdaVersion = uint64_t(1) << 61, sonarVersion = uint64_t(1) << 60;
    ou::ui::SonarCamera shownCam;
    glm::ivec2 shownSonarSize(0);
    // Loading screen (pc-w\image\loading_bg_*, chapter title and story) shown for one frame before a load blocks.
    ou::ui::Image screenImage; // uiImages[1] outside the main menu: the PDA background or a loading screen
    ou::ui::Image extrasImage; // uiImages[2]: the EXTRAS picture (ui::Menus::extrasImage)
    std::string extrasName;
    uint64_t extrasVersion = uint64_t(1) << 59;
    uint64_t screenVersion = uint64_t(1) << 62;
    std::string screenName;
    auto useScreenImage = [&](std::string_view name) {
        if (name == screenName) return;
        screenName = name;
        if (auto img = ou::ui::loadRaw(*bigfile, name)) screenImage = std::move(*img);
        else screenImage = {}, std::fprintf(stderr, "ou-game: pc-w\\image\\%s.raw missing\n", screenName.c_str());
        ++screenVersion;
    };
    auto showLoading = [&](int chapter) {
        useScreenImage(ou::ui::loadingImage(chapter));
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window.get(), &w, &h);
        ou::ui::Canvas cv(w, h, &*art);
        cv.rect(0, 0, float(w), float(h), 0xFF000000u);
        ou::ui::drawLoading(cv, *strings, chapter);
        ou::render::FrameOptions fo{};
        fo.ui = cv.vertices;
        fo.uiImages = {ou::render::RgbaImage{art->rgba, art->width, art->height, art->version},
                       ou::render::RgbaImage{screenImage.rgba, screenImage.width, screenImage.height, screenVersion}};
        renderer->drawFrame(ou::render::Camera{}, {w, h}, fo);
        renderer->present();
        std::fprintf(stderr, "ou-game: loading screen chapter %d (%s)\n", chapter, screenName.c_str());
    };
    auto saveChapter = [&](const std::string& slot) { // for its loading screen
        try {
            return std::clamp(ou::save::readFile(ou::save::slotPath(slot)).chapter, 0, 8);
        } catch (const std::exception&) {
            return world->chapter;
        }
    };
    bool running = true;
    // What the player reached across games (save::Profile): chapter ends and the ending (TREASURE HUNT, EXTRAS unlocks).
    ou::save::Profile profile = ou::save::readProfile();
    auto showProfile = [&] {
        menus.finished = profile.finished;
        menus.unlocks = {profile.chapters, profile.finished, profile.treasures, profile.relics};
    };
    showProfile();
    auto reached = [&](uint32_t chapters, bool finished) { // also every treasure / relic found (EXTRAS)
        bool treasures = true, relics = true;
        for (int ch = 1; ch <= 7; ++ch) {
            const auto& r = ou::game::chapterRewards(ch);
            treasures = treasures && std::all_of(r.treasures.begin(), r.treasures.end(), [&](int32_t id) { return world->collected.contains(id); });
            relics = relics && (!r.relic || world->collected.contains(r.relic));
        }
        const ou::save::Profile p{profile.chapters | chapters, profile.finished || finished, profile.treasures || treasures,
                                  profile.relics || relics};
        if (p == profile) return;
        profile = p;
        showProfile();
        try {
            ou::save::writeProfile(profile);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: profile: %s\n", e.what());
        }
    };
    // Autosave (slot "autosave", ui::kAutosaveSlot) at every chapter start and checkpoint of a game started from the main
    // menu [I: the original autosaves at checkpoints]; key = (chapter, checkpoint) last saved, -1 = save at the next tick.
    const bool autosaves = menuStart && !script;
    std::optional<std::pair<int, std::optional<ou::game::CheckpointRef>>> autoKey;
    auto autosaveHere = [&] { autoKey = std::pair{world->chapter, world->checkpoint()}; };
    auto startChapter = [&](int n, bool keepFinds) { // New Game / Treasure Hunt / REVISIT LOCATION
        showLoading(n);
        try {
            if (!keepFinds) world->collected.clear(), world->found = {};
            world->script().restore(nlohmann::json::object());
            world->startChapter(n);
            shownChapter = -1;
            autoKey = std::pair{-1, std::optional<ou::game::CheckpointRef>{}};
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: chapter %d: %s\n", n, e.what());
        }
    };
    auto act = [&](ou::ui::Action action) {
        using ou::ui::Action;
        switch (action) {
        case Action::None: return;
        case Action::Continue: // PLAY GAME / RESUME GAME: the autosave, else the newest save, else a new game
            if (!menus.saves.empty()) {
                const auto it = std::find_if(menus.saves.begin(), menus.saves.end(), [](const auto& v) { return v.autosave; });
                const std::string slot = (it != menus.saves.end() ? *it : menus.saves.front()).slot;
                showLoading(saveChapter(slot));
                if (!quickLoad(*world, slot)) hud.notify(std::string(strings->get(3)), 2.0); // "Load failed."
                shownChapter = world->chapter;
                autosaveHere();
                break;
            }
            [[fallthrough]];
        case Action::NewGame:
            std::fprintf(stderr, "ou-game: new game\n");
            if (!opt.noMovies) { // frontend state 8 PlayNewGameMovie (tru.exe 0x5DB110)
                SDL_GLContext gl = SDL_GL_GetCurrentContext(); // the movie's SDL_Renderer may switch contexts
                playMovie("explode.bik");
                if (gl) SDL_GL_MakeCurrent(window.get(), gl);
            }
            world->treasureHunt = false;
            startChapter(0, false);
            break;
        case Action::TreasureHunt: // [O] l1_seatop, keeping what the finished game found (its autosave)
            try {
                const auto g = ou::save::readFile(ou::save::slotPath(ou::ui::kAutosaveSlot));
                world->collected = g.collected, world->found = g.found;
            } catch (const std::exception& e) {
                world->collected.clear(), world->found = {};
                std::fprintf(stderr, "ou-game: treasure hunt without the autosave: %s\n", e.what());
            }
            world->treasureHunt = true;
            startChapter(1, true);
            break;
        case Action::Revisit: startChapter(menus.revisitChapter(), true); break; // PDA REVISIT LOCATION (Treasure Hunt)
        case Action::Resume: break;
        case Action::Save: { // a used slot is overwritten, an empty one becomes the first free "savegame-<n>"
            std::string name = menus.selectedSave();
            for (int n = 1; name.empty(); ++n)
                if (std::none_of(menus.saves.begin(), menus.saves.end(), [&](const auto& s) { return s.slot == "savegame-" + std::to_string(n); }))
                    name = "savegame-" + std::to_string(n);
            hud.notify(std::string(strings->get(quickSave(*world, name) ? 18 : 5)), 2.0); // "Save Successful" / "Save failed."
            menus.saves = listSaves();
            break;
        }
        case Action::Delete: {
            std::error_code ec;
            const bool ok = std::filesystem::remove(ou::save::slotPath(menus.selectedSave()), ec);
            hud.notify(std::string(strings->get(ok ? 11 : 9)), 2.0); // "Delete Successful." / "Delete Failed."
            menus.saves = listSaves();
            break;
        }
        case Action::Hint:
        case Action::Answer:
            if (sound) sound->playHelp(*world, action == Action::Answer);
            break;
        case Action::Load:
            showLoading(saveChapter(menus.selectedSave()));
            if (!quickLoad(*world, menus.selectedSave())) hud.notify(std::string(strings->get(3)), 2.0); // "Load failed."
            shownChapter = world->chapter;
            autosaveHere();
            break;
        case Action::PlayMovie: // EXTRAS TRAILERS & CREDITS; in play the game is left after it (471)
            if (!opt.noMovies) {
                SDL_GLContext gl = SDL_GL_GetCurrentContext();
                playMovie(menus.selectedSave().c_str());
                if (gl) SDL_GL_MakeCurrent(window.get(), gl);
            }
            if (menus.mainMenu()) break;
            [[fallthrough]];
        case Action::QuitToMenu:
            menus.close();
            menus.saves = listSaves();
            menus.open(ou::ui::Screen::Main);
            ensureMenuMovie();
            shownChapter = -2;
            break;
        case Action::Quit: running = false; break;
        case Action::Equip: // PDA WEAPON SELECTION
            world->equipWeapon(ou::game::Weapon(menus.equipWeapon()));
            fillPda();
            std::fprintf(stderr, "ou-game: tick %llu equip %s\n", (unsigned long long)world->ticks(),
                         std::string(ou::game::weaponName(world->combat().secondary)).c_str());
            break;
        case Action::SonarReset: resetSonar(); break;
        }
        std::fprintf(stderr, "ou-game: tick %llu menu action %d\n", (unsigned long long)world->ticks(), int(action));
    };
    auto nav = [&](ou::ui::Nav n) {
        const int tailoring = menus.tailoring;
        const auto a = menus.input(n);
        applyVolumes();
        // PLAYER TAILORING picked (START NEW GAME, or GAME OPTIONS in play): every level of its preset (game::Tailoring).
        if (menus.tailoring != tailoring || a == ou::ui::Action::NewGame)
            world->setTailoring(ou::game::Tailoring::preset(menus.tailoring));
        act(a);
        if (const int p = world->tailoring().preset(); p >= 0) menus.tailoring = p; // a loaded save's
    };
    if (menuStart) {
        menus.saves = listSaves();
        menus.open(ou::ui::Screen::Main);
        ensureMenuMovie();
    }
    // ---- cinematics (src/cine, app/game/Cinematic.*): --cine, and the cinematic_objects scripts activate (level ends) ----
    auto cineView = [&] {
        ou::app::CineView v{*bigfile, *world, *renderer, window.get(), audioOn ? &mixer : nullptr, scene, lara.meshIndices, streamed.gunIndices};
        v.renderHz = opt.renderHz;
        v.frame.msaa = opt.gfx.msaa, v.frame.shadows = opt.gfx.shadows, v.frame.fxaa = opt.gfx.fxaa, v.frame.hdr = opt.gfx.hdr;
        v.frame.rt = ou::render::RayTracing(opt.gfx.rt), v.frame.rtRays = opt.gfx.rtRays;
        v.frame.originalShaders = opt.gfx.originalShaders, v.frame.brightness = opt.gfx.brightness;
        v.overlay = !opt.noOverlay;
        v.musicVolume = float(menus.musicVolume) / 10, v.sfxVolume = float(menus.sfxVolume) / 10;
        return v;
    };
    if (!opt.cine.empty()) {
        auto v = cineView();
        v.maxFrames = opt.frames;
        try {
            const auto r = ou::app::playCinematic(opt.cine, v);
            if (r.lastImage && !opt.screenshot.empty()) {
                if (!writePpm(opt.screenshot, *r.lastImage)) return 1;
                std::fprintf(stderr, "ou-game: wrote %s (%dx%d)\n", opt.screenshot.string().c_str(), r.lastImage->width, r.lastImage->height);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: --cine: %s\n", e.what());
            return 1;
        }
        renderer->shutdown();
        return 0;
    }
    std::set<std::string> playedCines; // [I] each plays once per session
    world->onCinematic = [&](const std::string& name, bool levelEnd) {
        if (!playedCines.insert(name).second) return;
        try {
            const auto r = ou::app::playCinematic(name, cineView());
            if (r.quit) running = false; // World places Lara and the bikes at the stream's tails afterwards
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: cinematic %s: %s\n", name.c_str(), e.what());
        }
        if (levelEnd) reached(1u << world->chapter, false); // before World::startChapter moves on
        if (levelEnd && running) showLoading(world->chapter + 1); // [I] the next chapter is the following one
        if (!levelEnd && world->chapter == 8 && running) { // the ending [O]: credits, then the main menu with TREASURE HUNT
            reached(1u << 7 | 1u << 8, true);
            if (autosaves) quickSave(*world, std::string(ou::ui::kAutosaveSlot)); // what was found, for Treasure Hunt
            if (!opt.noMovies) {
                SDL_GLContext gl = SDL_GL_GetCurrentContext();
                playMovie("credits.bik");
                if (gl) SDL_GL_MakeCurrent(window.get(), gl);
            }
            act(ou::ui::Action::QuitToMenu);
        }
        streamed.version = ~0ull; // re-upload the game scene
    };

    const bool interactive = !script;
    SDL_SetWindowRelativeMouseMode(window.get(), interactive && !menuStart);
    ou::platform::GamepadPtr pad;
    ou::engine::FrameClock clock(ou::game::kTickSeconds);
    glm::vec2 mouse(0);
    bool overlay = !opt.noOverlay;
    uint64_t last = SDL_GetTicksNS(), fpsStart = last, appTicks = 0; // appTicks: script time, runs on in menus
    int frame = 0, fpsFrames = 0;
    double fps = 0;
    std::vector<double> frameMs; // --frames: wall frame times for the final report

    while (running) {
        const bool inMenu = menus.screen() != ou::ui::Screen::None;
        const bool mainMenu = menus.mainMenu();
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_F1 && !e.key.repeat) overlay = !overlay;
                else if (e.key.key == SDLK_F7 && !e.key.repeat) opt.waterDebug = (opt.waterDebug + 1) % ou::render::kWaterDebugViews;
                else if (menus.screen() != ou::ui::Screen::None) {
                    if (const auto n = keyNav(e.key.key)) nav(*n);
                } else if (e.key.key == SDLK_ESCAPE && !e.key.repeat) openPause();
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (menus.screen() != ou::ui::Screen::None) {
                    if (const auto n = padNav(e.gbutton.button)) nav(*n);
                } else if (e.gbutton.button == SDL_GAMEPAD_BUTTON_START) openPause();
                break;
            case SDL_EVENT_MOUSE_MOTION:
                if (interactive && !inMenu) mouse += glm::vec2(e.motion.xrel, e.motion.yrel);
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
        if (interactive) SDL_SetWindowRelativeMouseMode(window.get(), menus.screen() == ou::ui::Screen::None);
        const uint64_t now = SDL_GetTicksNS();
        const double dt = opt.renderHz > 0 ? 1.0 / opt.renderHz : double(now - last) * 1e-9;
        if (opt.frames && frame > 30) frameMs.push_back(double(now - last) * 1e-6); // wall time, after 30 warm-up frames
        last = now;

        for (int n = clock.advance(dt); n > 0 && running; --n) {
            ou::game::Actions a = script ? script->at(appTicks)
                                         : ou::game::readDevices(opt.controls, SDL_GetKeyboardState(nullptr), pad.get(),
                                                                 interactive ? SDL_GetMouseState(nullptr, nullptr) : 0);
            ++appTicks;
            if (!opt.musicWav.empty()) { // a tick of mixer output (also while paused)
                const size_t frames = mixer.outputRate() / 60, at = wav.size();
                wav.resize(at + 2 * frames);
                mixer.mix(wav.data() + at, frames);
            }
            if (menus.screen() != ou::ui::Screen::None) { // paused: the world does not tick
                if (script)
                    if (const auto n = scriptNav(a, menuHeld)) nav(*n);
                if (menus.screen() == ou::ui::Screen::PdaSonar) { // SONAR MAP: the move keys turn / fly, H up, G down
                    const bool* k = SDL_GetKeyboardState(nullptr);
                    const float rise = (k[SDL_SCANCODE_H] ? 1.0f : 0.0f) - (k[SDL_SCANCODE_G] ? 1.0f : 0.0f);
                    sonarCam.fly({-a.move.x, a.move.y, rise}, float(ou::game::kTickSeconds));
                }
                menuHeld = a, held = a;
                mouse = glm::vec2(0);
                continue;
            }
            menuHeld = a;
            if (a.pause && !held.pause) {
                openPause();
                held = a;
                continue;
            }
            if (a.pda && !held.pda) {
                openPda();
                held = a;
                continue;
            }
            // Mouse motion since the last tick, as a rate over this tick.
            if (opt.controls.invertY) mouse.y = -mouse.y;
            a.look -= mouse * kMouseSensitivity * opt.controls.mouseScale / float(ou::game::kTickSeconds);
            mouse = glm::vec2(0);
            if (a.save && !held.save) quickSave(*world, "quick");
            if (sound && a.hint && !held.hint) sound->playHelp(*world, false); // PDA Hint / Answer (help on demand)
            if (sound && a.answer && !held.answer) sound->playHelp(*world, true);
            if (a.load && !held.load) quickLoad(*world, "quick"), shownChapter = world->chapter;
            held = a;
            if (world->ticks() == dieTick) {
                world->kill(); // death clip, then World respawns her at the last checkpoint
                logState("killed (--die)", *world);
            }
            world->tick(a);
            if (autosaves && autoKey && !menus.mainMenu()) {
                const std::pair key{world->chapter, world->checkpoint()};
                if (key != *autoKey) quickSave(*world, std::string(ou::ui::kAutosaveSlot)), autoKey = key;
            }
            combat.tick(*world);
            gadgets.tick(*world);
            effects.events(*world);
            // The emitters' camera-range gate uses the tick's camera, not the last rendered (interpolated) one: spawns and
            // the shared random stream must not depend on the render rate.
            effects.system.tick(opt.camera ? glm::vec3((*opt.camera)[0], (*opt.camera)[1], (*opt.camera)[2]) : world->camera().position);
            if (const auto* b = world->ridden(); b || bikePhase != "parked") { // bike phase changes + 1 Hz while riding
                const std::string phase(b ? ou::game::bikePhaseName(b->phase) : "parked");
                if (b && (phase != bikePhase || world->ticks() % 60 == 0)) {
                    const auto p = b->pos;
                    std::fprintf(stderr, "ou-game: tick %llu bike %d %s unit %s pos %.0f %.0f %.0f yaw %.3f pitch %.3f speed %.0f%s | lara %s %s\n",
                                 (unsigned long long)world->ticks(), b->uid, phase.c_str(), b->unit.c_str(), p.x, p.y, p.z, b->yaw, b->pitch,
                                 b->speed, b->air ? " air" : "", std::string(ou::game::stateName(world->player().state().state)).c_str(),
                                 world->player().state().clip.c_str());
                } else if (!b) {
                    std::fprintf(stderr, "ou-game: tick %llu off the bike: lara %s at %.0f %.0f %.0f\n", (unsigned long long)world->ticks(),
                                 std::string(ou::game::stateName(world->player().state().state)).c_str(), world->player().state().pos.x,
                                 world->player().state().pos.y, world->player().state().pos.z);
                }
                bikePhase = phase;
            }
            if (world->bikeMounts != bikeMounts) { // [I] Mexico: l4_laras_new_bike starts on the first mount
                if (bikeMounts == 0 && audioOn && world->currentUnit().data.name.starts_with("l4_"))
                    music.play(*bigfile, mixer, "l4_laras_new_bike", "bike", world->ticks());
                bikeMounts = world->bikeMounts;
            }
            const auto tick = (unsigned long long)world->ticks();
            for (const auto& e : world->script().events()) {
                const auto* src = world->script().find(e.source);
                const auto* dst = world->script().find(e.target);
                std::fprintf(stderr, "ou-game: tick %llu script %s %d (%s) -> %d (%s)\n", (unsigned long long)e.tick,
                             ou::script::eventName(e.kind), e.source, src ? src->name.c_str() : "?", e.target,
                             dst ? dst->name.c_str() : "untracked");
            }
            for (const auto& e : world->enemies().all()) { // enemy mode changes and blows, for replays
                const auto &p = e->previous(), &s = e->state();
                if (p.mode != s.mode || (s.blowLanded && !p.blowLanded))
                    std::fprintf(stderr, "ou-game: tick %llu enemy %d (%s) %s%s [%s] pos %.0f %.0f %.0f hp %.0f | lara %.0f %.0f %.0f\n",
                                 (unsigned long long)world->ticks(), e->uid(), e->type().name.c_str(),
                                 std::string(ou::game::ai::modeName(s.mode)).c_str(), s.blowLanded && !p.blowLanded ? " blow lands" : "",
                                 s.clip.c_str(), s.pos.x, s.pos.y, s.pos.z, s.health, world->player().state().pos.x,
                                 world->player().state().pos.y, world->player().state().pos.z);
            }
            for (const auto& sh : world->enemies().shots) { // enemy shots: logged, drawn as tracers for a moment
                std::fprintf(stderr, "ou-game: tick %llu enemy %d shot %s from %.0f %.0f %.0f to %.0f %.0f %.0f | health %.0f\n", tick,
                             sh.uid, sh.hit ? "hits Lara" : "misses", sh.from.x, sh.from.y, sh.from.z, sh.to.x, sh.to.y, sh.to.z,
                             world->health().value());
                enemyMeshes.tracers.push_back({sh, world->ticks()});
            }
            for (const auto& p : world->pickedUp()) {
                const std::string text = std::string(strings->get(ou::game::pickupStringId(p.kind))) + "  " +
                                         std::to_string(world->found[size_t(p.kind)]);
                hud.notify(text);
                effects.pickedUp(p.id);
                std::fprintf(stderr, "ou-game: tick %llu pickup %s %d (%s)\n", tick, ou::game::pickupKindName(p.kind), p.id, text.c_str());
            }
            if (world->chapter != shownChapter && shownChapter != -2) {
                const bool flashback = shownChapter == 0 && world->chapter == 1; // prologue -> one week earlier
                hud.title(chapterName(world->chapter), flashback ? std::string(strings->get(ou::game::kStringOneWeekEarlier)) : "");
                std::fprintf(stderr, "ou-game: tick %llu chapter %d %s (%s)\n", tick, world->chapter, chapterName(world->chapter).c_str(),
                             world->currentUnit().data.name.c_str());
                shownChapter = world->chapter;
            }
            if (world->currentUnit().data.name != unitName) {
                unitName = world->currentUnit().data.name;
                logUnits();
                if (audioOn && music.current.empty()) music.play(*bigfile, mixer, world->currentUnit().data.music, "unit " + unitName, tick);
            }
            if (audioOn) music.tick(mixer, float(ou::game::kTickSeconds));
            for (const auto& r : world->script().takeMusicRequests()) { // script `music` natives
                const auto* o = world->script().find(r.owner);
                const std::string why = "script " + (o ? o->name : std::string("unit")) + " " + std::to_string(r.owner) + " music " +
                                        std::to_string(r.function) + " slot " + std::to_string(r.slot);
                const float fade = r.fade > 0 && r.fade < 120 ? r.fade : 0.05f;
                if (!audioOn) std::fprintf(stderr, "ou-game: tick %llu music request %s (%s)\n", tick, r.function == 3 ? "stop" : r.track.c_str(), why.c_str());
                else if (r.function == 3) music.stop(mixer, fade, why, tick, r.slot);
                else music.play(*bigfile, mixer, r.track, why, tick, fade, std::max(0, r.slot));
            }
            if (sound) sound->tick(*world);
            hud.tick(ou::game::kTickSeconds);
            hud.update(CombatView::hudState(*world), ou::game::kTickSeconds);
        }
        try { // a level load or save changed the outfit: new Lara meshes, rig and waves
            if (const auto drm = streamed.dress(*bigfile, *world, warnings)) {
                streamed.version = ~0ull;
                if (audioOn) mixer.addWaves(*drm);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: outfit %s: %s\n", world->outfit.c_str(), e.what());
            streamed.laraOutfit = world->outfit; // keep the current meshes, do not retry every frame
        }
        if (world->unitsVersion() != streamed.version || world->enemies().version() != streamed.enemyVersion ||
            world->lightsVersion() != streamed.lightsVersion || world->script().visibilityVersion() != streamed.visibilityVersion ||
            world->laraHiddenGroups() != streamed.hiddenGroups) {
            // streamed in/out, loaded, respawned or enemies spawned: new render scene
            try {
                effects.sync(*bigfile, *world, warnings);
                streamed.fixtureLights = effects.fixtures();
                scene = streamed.build(*bigfile, *world, lara, enemyMeshes, warnings);
                effects.addTextures(scene);
                for (auto& w : renderer->upload(scene)) warnings.push_back(std::move(w));
                objects.index(scene);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ou-game: streaming the render scene failed: %s\n", e.what());
                streamed.version = world->unitsVersion();
            }
            if (opt.showCollision) debugLines = collisionLines(world->collision());
            for (auto& w : world->warnings) std::fprintf(stderr, "ou-game: warning: %s\n", w.c_str());
            world->warnings.clear();
        }

        // --draw-last: the frame pacing and ticks are those of the render rate, the drawing is skipped (simulation never
        // reads it; ctest game_render_rate_smoke under ASan).
        if (opt.drawLast && opt.frames && frame + 1 < opt.frames) {
            ++frame;
            continue;
        }
        const auto s = world->interpolated(clock.alpha());
        const auto c = world->interpolatedCamera(clock.alpha());
        lara.pose(*world, s, clock.alpha(), scene, *renderer);
        {
            int pw = 1, ph = 1;
            SDL_GetWindowSizeInPixels(window.get(), &pw, &ph);
            const bool fixedCam = bool(opt.camera); // --camera: no culling against the game camera
            enemyMeshes.pose(*world, clock.alpha(), scene, *renderer, c.position, c.target, fixedCam ? 360.0f : world->fovDeg,
                             float(pw) / float(std::max(ph, 1)));
        }
        poseBikes(*world, clock.alpha(), streamed.bikeIndices, scene, *renderer, bikeScratch);
        objects.pose(world->script(), scene, *renderer);
        combat.pose(*world, s, clock.alpha(), scene, streamed.gunIndices, streamed.gunSlots, *renderer);
        gadgets.pose(*world, streamed, scene, *renderer);
        objects.hide(world->collected, scene, *renderer, effects.system);
        ou::render::Camera cam;
        cam.verticalFovDeg = c.fovDeg > 0 ? c.fovDeg : world->fovDeg; // spline cameras have their own
        cam.nearZ = 5.0f;
        cam.farZ = 200000.0f;
        cam.position = c.position;
        cam.lookAt(c.target);
        cam.up = c.up;
        if (opt.camera) cam.position = {(*opt.camera)[0], (*opt.camera)[1], (*opt.camera)[2]}, cam.lookAt({(*opt.camera)[3], (*opt.camera)[4], (*opt.camera)[5]});
        if (audioOn) mixer.setListener({c.position, glm::normalize(c.target - c.position), {0, 0, 1}});

        ++fpsFrames;
        if (now - fpsStart >= 500'000'000ull) {
            fps = fpsFrames / (double(now - fpsStart) * 1e-9);
            fpsStart = now;
            fpsFrames = 0;
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window.get(), &w, &h);
        std::vector<std::string> lines;
        if (overlay && !mainMenu) {
            lines.push_back(fmt("OpenUnderworld ou-game | %s | %.1f FPS | tick %llu%s%s", world->currentUnit().data.name.c_str(), fps,
                                (unsigned long long)world->ticks(), script ? " | REPLAY " : "", pad ? " | gamepad" : ""));
            lines.push_back(renderer->description());
            lines.push_back(ou::render::rtSummary(renderer->rtStatus(), ou::render::RayTracing(opt.gfx.rt), opt.gfx.rtRays));
            lines.push_back(renderer->upscalerSummary());
            lines.push_back(fmt("lara %s | clip %s t %.2f | pos %.0f %.0f %.0f | yaw %.0f", std::string(ou::game::stateName(s.state)).c_str(),
                                s.clip.c_str(), s.clipTime, s.pos.x, s.pos.y, s.pos.z, glm::degrees(s.yaw)));
            lines.push_back(fmt("unit %s (%zu loaded) | checkpoint %d | chapter %d | found %d/%d/%d | music %s",
                                world->currentUnit().data.name.c_str(), world->units().size(), world->checkpoint() ? world->checkpoint()->id : 0,
                                world->chapter, world->found[0], world->found[1], world->found[2],
                                music.current.empty() ? "-" : music.current.c_str()));
            const ou::game::ai::Enemy* near = nullptr; // nearest living enemy
            for (const auto& e : world->enemies().all())
                if (e->alive() && (!near || glm::distance(e->position(), s.pos) < glm::distance(near->position(), s.pos))) near = e.get();
            if (near)
                lines.push_back(fmt("enemies %zu | nearest %s %d %s | hp %.0f/%.0f | %.0f units", world->enemies().all().size(),
                                    near->type().name.c_str(), near->uid(), std::string(ou::game::ai::modeName(near->state().mode)).c_str(),
                                    near->health(), near->type().tuning.health, glm::distance(near->position(), s.pos)));
            if (!warnings.empty()) lines.push_back(fmt("!%zu load warnings on stderr", warnings.size()));
            lines.push_back(fmt("water %s | ripples %s | splashes %zu", opt.gfx.water ? "enhanced" : "original",
                                world->water().active() ? "active" : "-", world->water().drops().size()));
            lines.push_back(fmt("effects: %zu emitters | %zu particles | %zu glows | %zu fixture lights | %zu sprite batches",
                                effects.system.emitterCount(), effects.system.particleCount(), effects.system.glowCount(),
                                streamed.fixtureLights.size(), effects.batches.size()));
            lines.push_back("WASD move | Shift walk | Space jump/climb | Ctrl/C drop | E use/push/pick up | G grapple (E/Shift rope) | "
                            "F draw | LMB fire | RMB lock | "
                            "Q switch | X weapon | H health pack | mouse look | F5 save | F9 load | F1 overlay | Esc menu");
        }
        if (overlay && !mainMenu) { // combat values (F1 overlay; the HUD shows them as art)
            const auto& cs = world->combat();
            std::string t = fmt("health %.0f/%.0f | health packs %d | adrenaline %.0f%%%s", world->health().value(), world->health().max(),
                                world->found[2], cs.adrenaline * 100, cs.slowMo > 0 ? " HEAD SHOT" : "");
            const auto& wd = world->anims().weapons[size_t(cs.weapon)];
            const auto wi = size_t(cs.weapon);
            t += fmt(" | %s", std::string(ou::game::weaponName(cs.weapon)).c_str());
            if (wd.clip >= 0) {
                t += fmt(" %d", cs.clip[wi][0]);
                if (wd.dual) t += fmt("+%d", cs.clip[wi][1]);
                t += wd.infinite ? std::string(" / inf") : fmt(" / %d", cs.reserve[wi]);
            }
            if (!cs.hammer.clip.empty()) t += " " + cs.hammer.clip;
            if (cs.secondary != ou::game::Weapon::Pistols && cs.secondary != cs.weapon)
                t += fmt(" (%s %d)", std::string(ou::game::weaponName(cs.secondary)).c_str(), cs.reserve[size_t(cs.secondary)]);
            if (cs.drawn)
                t += fmt("%s | %s", cs.reload > 0 ? " RELOADING" : "", cs.target ? (cs.locked ? "LOCKED" : "target") : "free aim");
            for (const auto& d : combat.dummies) t += fmt(" | dummy %.0f hp", d->hp);
            lines.push_back(t);
        }
        menus.tick(dt);
        ou::ui::Canvas canvas(w, h, &*art);
        // Main menu backdrop (tru.exe): black, the menu movie fitted to the window width and centred, dimmed [V
        // captures: ~0.66 of the decoded frame].
        ou::render::RgbaImage movieImage;
        if (mainMenu) {
            canvas.rect(0, 0, float(w), float(h), 0xFF000000u);
            if (menuMovie) {
                movieImage = menuMovie->update();
                const float mh = float(w) * float(movieImage.height) / float(std::max(1, movieImage.width));
                canvas.image(1, 0, (float(h) - mh) / 2, float(w), mh, 0, 0, 1, 1, 0xFFA8A8A8u);
            }
        }
        if (!mainMenu) {
            hud.reticle = CombatView::reticle(*world, c, world->fovDeg, w, h);
            if (!menus.reticles) hud.reticle = {}; // GAME OPTIONS > COMBAT RETICLES HIDDEN
            hud.grapple.reset();
            if (const auto target = world->grappleTarget(); target && !world->player().state().grappleUid)
                if (const auto p = toScreen(c, world->fovDeg, w, h, target->pos)) hud.grapple = std::array{p->x, p->y};
            if (menus.screen() == ou::ui::Screen::None) hud.draw(canvas); // menus hide the HUD [O pause captures]
            if (sound && menus.subtitles) ou::app::drawSubtitles(canvas, sound->subtitles());
        }
        const ou::ui::Screen scr = menus.screen();
        const bool pdaPage = scr == ou::ui::Screen::Pda || scr == ou::ui::Screen::PdaWeapons || scr == ou::ui::Screen::PdaInventory ||
                             scr == ou::ui::Screen::PdaItems || scr == ou::ui::Screen::Revisit ||
                             scr == ou::ui::Screen::Journal;
        if (scr == ou::ui::Screen::PdaSonar && shownScreen != scr) resetSonar(), shownSonarSize = {}; // opens behind Lara
        shownScreen = scr;
        if (pdaPage) // pda_bg_960 for narrow windows (ui::Menus::drawPda)
            useScreenImage(float(w) / float(std::max(1, h)) < 1.556f ? "pda_bg_960" : "pda_bg_1280");
        if (!mainMenu) movieImage = {screenImage.rgba, screenImage.width, screenImage.height, screenVersion};
        if (const auto m = pdaPage ? menus.pdaModel() : std::nullopt) { // the background with the model panel drawn in
            if (m != shownModel || screenName != shownModelBg) {
                pdaImage = screenImage;
                ou::ui::drawModelPanel(pdaImage, m->x - (screenName == "pda_bg_960" ? 160 : 0), m->y, m->w, m->h, &viewMesh(m->object));
                shownModel = m, shownModelBg = screenName, ++pdaVersion;
            }
            movieImage = {pdaImage.rgba, pdaImage.width, pdaImage.height, pdaVersion};
        }
        if (scr == ou::ui::Screen::PdaSonar) { // rendered at half the window size, again when the view moves
            const glm::ivec2 size(std::max(1, w / 2), std::max(1, h / 2));
            if (!(sonarCam == shownCam) || size != shownSonarSize) {
                std::vector<ou::ui::SonarUnit> units;
                for (const auto& u : world->units()) {
                    const auto r = world->mapReveal.find(u.data.name);
                    units.push_back({u.data.name, sonarMap(u.data.name), u.offset, r == world->mapReveal.end() ? nullptr : &r->second});
                }
                const auto& st = world->player().state();
                std::vector<ou::ui::SonarPawn> pawns{{&viewMesh("ping_map_icon_lara"),
                                                      glm::rotate(glm::translate(glm::mat4(1), st.pos), st.yaw, {0, 0, 1}), {0.1f, 0.8f, 0.55f}}};
                for (const auto& [uid, b] : world->bikes())
                    for (const auto& u : world->units())
                        if (u.data.name == b.unit) // grey [V sonar_full_1]
                            pawns.push_back({&viewMesh("ping_map_icon_motorcycle"),
                                             glm::rotate(glm::translate(glm::mat4(1), b.pos + u.offset), b.yaw, {0, 0, 1}), glm::vec3(0.75f)});
                sonar.render(size.x, size.y, sonarCam, units, pawns);
                shownCam = sonarCam, shownSonarSize = size, ++sonarVersion;
            }
            const auto& img = sonar.image();
            movieImage = {img.rgba, img.width, img.height, sonarVersion};
        }
        menus.draw(canvas, *strings);
        // Two persistent buffers, alternated: the GL backend caches line uploads by data pointer + size, and these
        // lines change every frame (HUD, effects), so consecutive frames must never share a pointer.
        auto& frameLines = lineBuffers[frame & 1];
        frameLines.assign(debugLines.begin(), debugLines.end());
        if (opt.showTriggers) appendTriggerLines(world->script(), frameLines);
        if (opt.showEnemies) appendEnemyLines(*world, frameLines);
        enemyMeshes.tracerLines(world->ticks(), frameLines);
        combat.lines(*world, frameLines);
        if (world->player().state().grappleUid) { // the rope from Lara's hand to the anchor
            const glm::vec3 hand = s.pos + glm::vec3(0, 0, 400);
            const glm::vec3 anchor = world->player().state().grappleAnchor;
            frameLines.push_back({{hand.x, hand.y, hand.z}, {anchor.x, anchor.y, anchor.z}, 0xFF60A0C0});
        }
        ou::render::FrameOptions fo{};
        fo.lighting = true, fo.cullBackFaces = true;
        fo.overlay = lines, fo.debugLines = mainMenu ? std::span<const ou::render::DebugLine>{} : std::span<const ou::render::DebugLine>(frameLines);
        fo.ui = canvas.vertices;
        if (const std::string name = menus.extrasImage(); !name.empty() && name != extrasName) { // EXTRAS preview / view
            extrasName = name;
            if (auto img = ou::ui::loadRaw(*bigfile, name)) extrasImage = std::move(*img);
            else extrasImage = {}, std::fprintf(stderr, "ou-game: pc-w\\image\\%s.raw missing\n", name.c_str());
            ++extrasVersion;
        }
        fo.uiImages = {ou::render::RgbaImage{art->rgba, art->width, art->height, art->version}, movieImage,
                       ou::render::RgbaImage{extrasImage.rgba, extrasImage.width, extrasImage.height, extrasVersion}};
        fo.msaa = opt.gfx.msaa, fo.shadows = opt.gfx.shadows, fo.fxaa = opt.gfx.fxaa, fo.hdr = opt.gfx.hdr;
        fo.rt = ou::render::RayTracing(opt.gfx.rt), fo.rtRays = opt.gfx.rtRays;
        fo.originalShaders = opt.gfx.originalShaders, fo.time = float((double(clock.ticks()) + clock.alpha()) * clock.step());
        fo.brightness = opt.gfx.brightness, fo.screenFade = world->script().screenFade();
        fo.upscaler = opt.gfx.fsr ? ou::render::Upscaler::Fsr : ou::render::Upscaler::Off;
        fo.fsrQuality = opt.gfx.fsrQuality, fo.sharpness = opt.gfx.sharpness, fo.frameGeneration = opt.gfx.frameGeneration;
        fo.water = ou::render::WaterQuality(opt.gfx.water);
        const auto& ws = world->water();
        if (opt.gfx.water && ws.active())
            fo.ripples = {ws.originX(), ws.originY(), ou::game::WaterSim::kCell, ws.surfaceZ(), ou::game::WaterSim::kSize,
                          ws.heights(), ws.version()};
        waterVolumes.clear();
        for (const auto& m : world->collision().markup)
            if (!mainMenu && m.type == 10 && m.points.size() >= 2)
                waterVolumes.push_back({{m.points[0].x, m.points[0].y, m.points[0].z},
                                        {m.points[1].x, m.points[1].y, m.points[1].z}});
        fo.waterVolumes = waterVolumes;
        splashParticles.clear();
        if (opt.gfx.water && !mainMenu)
            for (const auto& d : ws.drops()) splashParticles.push_back({{d.pos.x, d.pos.y, d.pos.z}, d.radius, d.alpha()});
        fo.splashes = splashParticles;
        objects.opacities(effects.system, meshOpacity);
        fo.meshOpacity = meshOpacity;
        fo.waterDetail = opt.gfx.waterDetail, fo.waterDebug = opt.waterDebug;
        fo.atmosphere = ou::render::Atmosphere(opt.gfx.atmosphere), fo.lightShafts = opt.gfx.lightShafts;
        if (opt.gfx.weather && !mainMenu) {
            using P = ou::game::Precipitation;
            ou::game::UnitWeather uw{opt.gfx.weather == 2 ? P::Rain : P::Snow, opt.gfx.weather == 2 ? 0.8f : 1.0f};
            if (opt.gfx.weather == 1) {
                const auto& u = world->currentUnit().data;
                auto it = unitWeathers.find(u.name);
                if (it == unitWeathers.end())
                    it = unitWeathers.emplace(u.name, u.drm ? ou::game::unitWeather(objectNames, *u.drm) : ou::game::UnitWeather{}).first;
                uw = it->second;
            }
            const bool roof = world->collision().raycast(cam.position, {0, 0, 1}, 3000.0f).has_value();
            shelter += (float(roof) - shelter) * (1.0f - std::exp(-3.0f * float(dt))); // same fade at any render rate
            fo.weather.kind = uw.kind == P::Rain ? ou::render::Weather::Kind::Rain
                              : uw.kind == P::Snow ? ou::render::Weather::Kind::Snow : ou::render::Weather::Kind::None;
            fo.weather.intensity = uw.intensity * (1.0f - shelter);
        }
        if (!mainMenu && opt.gfx.effects) effects.frame(cam, float(w) / float(std::max(1, h)), *world, fo);
        effects.lightSlots(gadgets.update(*world, clock.alpha()), dynamicLights);
        fo.dynamicLights = dynamicLights;
        renderer->drawFrame(cam, {w, h}, fo);
        ++frame;
        if (opt.frames && frame >= opt.frames) {
            running = false;
            const auto& fs = world->player().state(); // the simulated tick, not the interpolated frame: rate-independent
            std::fprintf(stderr, "ou-game: final tick %llu unit %s chapter %d checkpoint %d found %d/%d/%d state %s pos %.1f %.1f %.1f yaw %.1f clip %s sim %016llx fx %zu\n",
                         (unsigned long long)world->ticks(), world->currentUnit().data.name.c_str(), world->chapter,
                         world->checkpoint() ? world->checkpoint()->id : 0, world->found[0], world->found[1], world->found[2],
                         std::string(ou::game::stateName(fs.state)).c_str(), fs.pos.x, fs.pos.y, fs.pos.z, glm::degrees(fs.yaw), fs.clip.c_str(),
                         (unsigned long long)world->stateHash(), effects.system.particleCount());
            std::fprintf(stderr, "ou-game: %s\n",
                         ou::render::rtSummary(renderer->rtStatus(), ou::render::RayTracing(opt.gfx.rt), opt.gfx.rtRays).c_str());
            if (!frameMs.empty()) { // wall time per frame (ticks + draw), use with --no-vsync --render-hz 60
                double sum = 0;
                for (const double t : frameMs) sum += t;
                auto sorted = frameMs;
                std::sort(sorted.begin(), sorted.end());
                std::fprintf(stderr, "ou-game: frame time mean %.3f ms p95 %.3f ms (%zu frames)\n", sum / double(sorted.size()),
                             sorted[std::min(sorted.size() - 1, sorted.size() * 95 / 100)], sorted.size());
            }
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
    if (!opt.musicWav.empty()) {
        if (!writeWav(opt.musicWav, wav, mixer.outputRate())) std::fprintf(stderr, "ou-game: cannot write %s\n", opt.musicWav.string().c_str());
        else std::fprintf(stderr, "ou-game: wrote %s (%.2f s)\n", opt.musicWav.string().c_str(), double(wav.size() / 2) / mixer.outputRate());
    }
    renderer->shutdown();
    return 0;
}
