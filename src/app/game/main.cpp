// ou-game: playable Lara in a real unit. Simulation (src/game) runs on a fixed 60 Hz tick; rendering
// interpolates between the last two ticks. Docs: docs/GAMEPLAY.md.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "anim/Anim.hpp"
#include "audio/Audio.hpp"
#include "audio/Mixer.hpp"
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"
#include "config/Config.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"
#include "game/Game.hpp"
#include "platform/Sdl.hpp"
#include "save/Save.hpp"
#include "render/Renderer.hpp"
#include "video/Player.hpp"

namespace {

constexpr int kExitSkip = 77; // ctest SKIP_RETURN_CODE: no display available
constexpr float kMouseSensitivity = 0.0025f; // rad per pixel

struct Options {
    std::filesystem::path gameDir;
    std::string level = "pc-w\\l4_pool.drm";
    std::string renderer = "opengl";
    int width = 1280, height = 720;
    float fov = 60.0f;
    bool vsync = true, fullscreen = false, noOverlay = false, showCollision = false, showTriggers = false;
    int frames = 0; // 0 = until closed
    std::filesystem::path screenshot, inputScript;
    double renderHz = 0; // > 0: fixed virtual frame time (deterministic replays); default 60 with --input-script
    std::optional<std::array<float, 4>> spawn; // --spawn X,Y,Z,YAWDEG overrides the unit's player intro
    bool intro = false;  // --intro: play Movies/intro.bik first (skippable)
    int introFrames = -1; // --intro-frames N: stop the intro after N frames
    bool noAudio = false;
    std::string loadSlot; // --load SLOT: start from $XDG_DATA_HOME/openunderworld/saves/SLOT.ousave
    double die = -1;      // --die SECONDS: Lara dies then and respawns at the last checkpoint
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-game --game-dir DIR [--level pc-w\\\\l4_pool.drm] [--renderer opengl|vulkan]\n"
                 "               [--frames N] [--screenshot out.ppm] [--input-script FILE] [--render-hz HZ]\n"
                 "               [--width W --height H] [--fov DEG] [--no-vsync] [--fullscreen] [--no-overlay]\n"
                 "               [--spawn X,Y,Z,YAWDEG] [--intro] [--intro-frames N] [--no-audio]\n"
                 "               [--load SLOT] [--die SECONDS] [--show-collision] [--show-triggers] [--config config.json]\n"
                 "controls: WASD/arrows move (camera-relative), Shift walk, Space jump/climb, Ctrl or C drop, E use/push,\n"
                 "          mouse look; gamepad: left stick, right stick, A jump, B drop, X use, LB walk; F1 overlay, Esc quit\n"
                 "          F5 quicksave, F9 quickload (slot 'quick')\n"
                 "input script lines: <seconds> <forward|right|walk|jump|drop|interact|save|load|lookx|looky> [value]\n");
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
        if (a == "--show-triggers") { o.showTriggers = true; continue; }
        if (a == "--intro") { o.intro = true; continue; }
        if (a == "--no-audio") { o.noAudio = true; continue; }
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
        else if (a == "--intro-frames") o.intro = true, o.introFrames = std::atoi(v);
        else if (a == "--spawn") {
            std::array<float, 4> s{};
            if (std::sscanf(v, "%f,%f,%f,%f", &s[0], &s[1], &s[2], &s[3]) != 4) { std::fprintf(stderr, "--spawn wants X,Y,Z,YAWDEG\n"); return false; }
            o.spawn = s;
        }
        else if (a == "--load") o.loadSlot = v;
        else if (a == "--die") o.die = std::atof(v);
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

// Lara's skinned meshes: CPU-skinned in model space from World::pose (crossfaded clips + simulated hair;
// root segment held at bind, its motion is in the player position), then moved to the world.
struct LaraMeshes {
    std::vector<size_t> meshIndices; // into the uploaded scene
    std::vector<ou::scene::Vertex> scratch;

    void pose(const ou::game::World& world, const ou::game::PlayerState& s, double alpha, const ou::scene::SceneData& scene,
              ou::render::Renderer& r) {
        const auto palette = ou::anim::skinPalette(world.anims().skeleton, world.pose(alpha));
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

// Scripted objects (src/script): animated switches/doors and moved blocks, re-posed on the CPU in model space.
// Their level-scene meshes are tagged " @intro <unique id>" (formats::loadLevelScene).
struct ScriptedMeshes {
    std::map<int32_t, std::vector<size_t>> meshes; // intro unique id -> scene mesh indices
    std::set<int32_t> posed;                       // uploaded with a non-bind pose
    std::vector<ou::scene::Vertex> scratch;

    void index(const ou::scene::SceneData& s) {
        meshes.clear(), posed.clear();
        for (size_t i = 0; i < s.meshes.size(); ++i)
            if (const auto at = s.meshes[i].origin.rfind(" @intro "); at != std::string::npos)
                meshes[std::atoi(s.meshes[i].origin.c_str() + at + 8)].push_back(i);
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
                if (palette && !m.skin.empty() && palette->size() > 1) {
                    ou::anim::skinVertices(m, *palette, scratch);
                } else {
                    scratch = m.vertices;
                    if (palette)
                        for (auto& v : scratch) {
                            const glm::vec3 p((*palette)[0] * glm::vec4(v.position[0], v.position[1], v.position[2], 1));
                            v.position[0] = p.x, v.position[1] = p.y, v.position[2] = p.z;
                        }
                }
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

// Footstep and landing sounds from the simulated pose. [I] A foot (bones 8, 13; z ~0 when standing in
// ng_idle) dropping below 12 units in model space during walk/run/stop plays a heel wave; leaving the air
// plays a landing wave. Waves are lara.drm Wave sections, stone variants only: the game's surface-material
// to sound mapping is not decoded (AUDIO.md).
struct Footsteps {
    static constexpr uint32_t kHeel = 0x11A; // 'fst_boot_stone_b_heel_01' .. _10
    static constexpr uint32_t kLand = 0x1D3; // 'fst_boot_stone_a_land_01' .. _05
    bool down[2] = {true, true};
    ou::game::State last = ou::game::State::Idle;
    unsigned next = 0, plays = 0;

    void tick(const ou::game::World& w, ou::audio::Mixer& mix) {
        using ou::game::State;
        const auto& s = w.player().state();
        const bool wasAir = last == State::Jump || last == State::Fall, inAir = s.state == State::Jump || s.state == State::Fall;
        if (wasAir && !inAir && s.state != State::Hang) plays += mix.play(kLand + next++ % 5, {.volume = 0.9f}) != 0;
        last = s.state;
        const ou::anim::Clip* clip = w.anims().find(s.clip);
        if ((s.state != State::Walk && s.state != State::Run && s.state != State::Stop) || !clip) {
            down[0] = down[1] = true;
            return;
        }
        const auto& sk = w.anims().skeleton;
        const auto m = ou::anim::worldMatrices(sk, ou::anim::sample(*clip, sk, s.clipTime, s.loop));
        for (int f = 0; f < 2; ++f) {
            const float z = m[f ? 13 : 8][3].z;
            if (!down[f] && z < 12.0f) {
                down[f] = true;
                plays += mix.play(kHeel + next++ % 10, {.volume = s.state == State::Walk ? 0.5f : 0.8f}) != 0;
            } else if (z > 25.0f) {
                down[f] = false;
            }
        }
    }
};

// Render scene of the streamed units: each loaded unit's level scene (cached while it stays loaded) moved by
// its stream offset, then Lara's skinned meshes. Rebuilt and re-uploaded when World::unitsVersion changes.
struct StreamedScene {
    std::map<std::string, ou::scene::SceneData> units; // unit frame
    ou::scene::SceneData lara;                         // Lara's skinned meshes, skeletons, textures
    uint64_t version = ~0ull;

    ou::scene::SceneData build(const ou::cdc::BigfileSet& bf, const ou::game::World& w, LaraMeshes& laraMeshes,
                               std::vector<std::string>& warnings) {
        std::erase_if(units, [&](const auto& kv) {
            return std::none_of(w.units().begin(), w.units().end(), [&](const auto& u) { return u.data.name == kv.first; });
        });
        ou::scene::SceneData out;
        std::set<uint32_t> textureIds; // cdc section ids, unique across DRMs
        auto append = [&](const ou::scene::SceneData& s, glm::vec3 offset, std::vector<size_t>* indices) {
            const auto skelBase = int32_t(out.skeletons.size());
            out.skeletons.insert(out.skeletons.end(), s.skeletons.begin(), s.skeletons.end());
            for (auto m : s.meshes) {
                m.transform[12] += offset.x, m.transform[13] += offset.y, m.transform[14] += offset.z;
                if (m.skeleton >= 0) m.skeleton += skelBase;
                if (indices) indices->push_back(out.meshes.size());
                out.meshes.push_back(std::move(m));
            }
            for (const auto& t : s.textures)
                if (textureIds.insert(t.id).second) out.textures.push_back(t);
        };
        for (const auto& u : w.units()) {
            auto it = units.find(u.data.name);
            if (it == units.end()) {
                it = units.emplace(u.data.name, ou::formats::loadLevelScene(bf, "pc-w\\" + u.data.name + ".drm")).first;
                for (const auto& s : it->second.warnings) warnings.push_back(s);
            }
            append(it->second, u.offset, nullptr);
        }
        laraMeshes.meshIndices.clear();
        append(lara, glm::vec3(0), &laraMeshes.meshIndices);
        version = w.unitsVersion();
        return out;
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

} // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parseArgs(argc, argv, opt)) { usage(); return 2; }

    // ---- load: simulation data first (no window needed to fail early) ----
    std::optional<ou::game::InputScript> script;
    std::unique_ptr<const ou::cdc::BigfileSet> bigfile; // outlives `world`: its unit loader reads from it
    std::unique_ptr<ou::game::World> world;
    ou::scene::SceneData scene;
    StreamedScene streamed;
    LaraMeshes lara;
    ScriptedMeshes objects;
    std::vector<std::string> warnings;
    bool spawnFromIntro = false;
    try {
        if (!opt.inputScript.empty()) script = ou::game::InputScript::load(opt.inputScript);
        bigfile = std::make_unique<const ou::cdc::BigfileSet>(opt.gameDir);
        const auto& bf = *bigfile;
        // "pc-w\\l4_pool.drm" -> unit "l4_pool"; its portal neighbours stream in with it (World, LEVELS.md).
        std::string unit = opt.level.substr(opt.level.find_last_of('\\') + 1);
        if (unit.ends_with(".drm")) unit.resize(unit.size() - 4);
        std::optional<ou::game::Spawn> spawn;
        if (opt.spawn) spawn = ou::game::Spawn{{(*opt.spawn)[0], (*opt.spawn)[1], (*opt.spawn)[2]}, glm::radians((*opt.spawn)[3])};
        world = std::make_unique<ou::game::World>([&bf](const std::string& n) { return ou::game::loadUnitData(bf, n); }, unit,
                                                  ou::game::loadLaraAnims(bf), spawn);
        world->fovDeg = opt.fov;
        world->script().setBigfile(&bf);
        spawnFromIntro = !opt.spawn && world->currentUnit().data.spawn && world->currentUnit().data.spawn->fromIntro;
        if (!opt.loadSlot.empty() && !quickLoad(*world, opt.loadSlot)) return 1;

        auto laraScene = ou::formats::loadDrmScene(bf, "pc-w\\lara.drm");
        streamed.lara.skeletons = std::move(laraScene.skeletons);
        streamed.lara.textures = std::move(laraScene.textures);
        for (auto& m : laraScene.meshes)
            if (m.skeleton >= 0 && !m.indices.empty()) streamed.lara.meshes.push_back(std::move(m)); // skinned body only
        for (auto& w : laraScene.warnings) warnings.push_back(std::move(w));
        scene = streamed.build(bf, *world, lara, warnings);
        if (lara.meshIndices.empty()) warnings.push_back("pc-w\\lara.drm: no skinned mesh decoded");
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
    if (opt.intro) {
        // Own plain window: the movie player needs one without a GL/Vulkan context.
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) std::fprintf(stderr, "ou-game: no audio (%s); intro plays silent\n", SDL_GetError());
        ou::platform::WindowPtr movieWindow(SDL_CreateWindow("OpenUnderworld", opt.width, opt.height, opt.fullscreen ? SDL_WINDOW_FULLSCREEN : 0));
        try {
            ou::video::PlayOptions po;
            po.maxFrames = opt.introFrames;
            const auto st = ou::video::playMovie(movieWindow.get(), opt.gameDir / "Movies" / "intro.bik", po);
            std::fprintf(stderr, "ou-game: intro %d frames%s\n", st.frames, st.skipped ? " (skipped)" : "");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: intro: %s\n", e.what());
        }
    }

    // ---- audio (ou_audio): Lara's Wave sections for footsteps/landings, the unit's music track ----
    ou::audio::Mixer mixer; // after SdlInit: closes its device before SDL_Quit
    Footsteps steps;
    bool audioOn = false;
    if (!opt.noAudio) {
        try {
            const ou::cdc::BigfileSet bigfile(opt.gameDir);
            if (auto lara = bigfile.read("pc-w\\lara.drm")) mixer.addWaves(ou::cdc::parseDrm(std::move(*lara)));
            // [I] music file naming seen in the bigfile (pc-w\music\<unit>_mus_01\...); how the game picks a track is unknown.
            std::string unit = opt.level.substr(opt.level.find_last_of('\\') + 1);
            unit = unit.substr(0, unit.rfind(".drm"));
            const std::string musicPath = "pc-w\\music\\" + unit + "_mus_01\\" + unit + "_mus_01.mul";
            auto music = bigfile.read(musicPath);
            audioOn = mixer.openDevice();
            if (!audioOn) std::fprintf(stderr, "ou-game: no audio device (%s)\n", SDL_GetError());
            else if (music) mixer.playMusic(std::make_shared<ou::audio::Pcm>(ou::audio::decodeMul(*music).pcm), 1.0f);
            std::fprintf(stderr, "ou-game: audio %s, music %s\n", audioOn ? "on" : "off", music ? musicPath.c_str() : "none");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: audio: %s\n", e.what());
        }
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
    objects.index(scene);
    for (const auto& w : warnings) std::fprintf(stderr, "ou-game: warning: %s\n", w.c_str());
    auto debugLines = opt.showCollision ? collisionLines(world->collision()) : std::vector<ou::render::DebugLine>{};
    const auto& sp = world->player().state();
    std::fprintf(stderr, "ou-game: %s | spawn %s at %.1f %.1f %.1f yaw %.1f\n", opt.level.c_str(),
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
    ou::game::Actions held; // previous tick's actions: save/load react to the press edge
    std::string unitName = world->currentUnit().data.name;

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
            if (a.save && !held.save) quickSave(*world, "quick");
            if (a.load && !held.load) quickLoad(*world, "quick");
            held = a;
            if (world->ticks() == dieTick) {
                world->respawn();
                logState("died, respawned", *world);
            }
            world->tick(a);
            for (const auto& e : world->script().events()) {
                const auto* src = world->script().find(e.source);
                const auto* dst = world->script().find(e.target);
                std::fprintf(stderr, "ou-game: tick %llu script %s %d (%s) -> %d (%s)\n", (unsigned long long)e.tick,
                             ou::script::eventName(e.kind), e.source, src ? src->name.c_str() : "?", e.target,
                             dst ? dst->name.c_str() : "untracked");
            }
            if (audioOn) steps.tick(*world, mixer);
            if (world->currentUnit().data.name != unitName) {
                unitName = world->currentUnit().data.name;
                logUnits();
            }
        }
        if (world->unitsVersion() != streamed.version) { // streamed in/out, loaded or respawned: new render scene
            try {
                scene = streamed.build(*bigfile, *world, lara, warnings);
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

        const auto s = world->interpolated(clock.alpha());
        const auto c = world->interpolatedCamera(clock.alpha());
        lara.pose(*world, s, clock.alpha(), scene, *renderer);
        objects.pose(world->script(), scene, *renderer);
        ou::render::Camera cam;
        cam.verticalFovDeg = world->fovDeg;
        cam.nearZ = 5.0f;
        cam.farZ = 200000.0f;
        cam.position = c.position;
        cam.lookAt(c.target);
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
        if (overlay) {
            lines.push_back(fmt("OpenUnderworld ou-game | %s | %.1f FPS | tick %llu%s%s", opt.level.c_str(), fps,
                                (unsigned long long)world->ticks(), script ? " | REPLAY " : "", pad ? " | gamepad" : ""));
            lines.push_back(renderer->description());
            lines.push_back(fmt("lara %s | clip %s t %.2f | pos %.0f %.0f %.0f | yaw %.0f", std::string(ou::game::stateName(s.state)).c_str(),
                                s.clip.c_str(), s.clipTime, s.pos.x, s.pos.y, s.pos.z, glm::degrees(s.yaw)));
            lines.push_back(fmt("unit %s (%zu loaded) | checkpoint %d", world->currentUnit().data.name.c_str(), world->units().size(),
                                world->checkpoint() ? world->checkpoint()->id : 0));
            if (!warnings.empty()) lines.push_back(fmt("!%zu load warnings on stderr", warnings.size()));
            lines.push_back("WASD move | Shift walk | Space jump/climb | Ctrl/C drop | E use/push | mouse look | F5 save | F9 load | F1 overlay | Esc quit");
        }
        auto frameLines = debugLines;
        if (opt.showTriggers) appendTriggerLines(world->script(), frameLines);
        renderer->drawFrame(cam, {w, h}, {true, true, lines, frameLines});
        ++frame;
        if (opt.frames && frame >= opt.frames) {
            running = false;
            std::fprintf(stderr, "ou-game: final tick %llu unit %s checkpoint %d state %s pos %.1f %.1f %.1f yaw %.1f clip %s\n",
                         (unsigned long long)world->ticks(), world->currentUnit().data.name.c_str(),
                         world->checkpoint() ? world->checkpoint()->id : 0, std::string(ou::game::stateName(s.state)).c_str(), s.pos.x,
                         s.pos.y, s.pos.z, glm::degrees(s.yaw), s.clip.c_str());
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
    std::fprintf(stderr, "ou-game: %u footstep/landing sounds\n", steps.plays);
    renderer->shutdown();
    return 0;
}
