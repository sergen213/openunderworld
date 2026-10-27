// ou-game: playable Lara in a real unit. Simulation (src/game) runs on a fixed 60 Hz tick; rendering
// interpolates between the last two ticks. Docs: docs/GAMEPLAY.md.
#include <algorithm>
#include <array>
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
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "app/game/Cinematic.hpp"
#include "anim/Anim.hpp"
#include "audio/Audio.hpp"
#include "audio/Mixer.hpp"
#include "cdc/Cdc.hpp"
#include "cine/Cine.hpp"
#include "collision/Collision.hpp"
#include "config/Config.hpp"
#include "engine/FrameClock.hpp"
#include "formats/Formats.hpp"
#include "game/Game.hpp"
#include "locale/Locale.hpp"
#include "platform/Sdl.hpp"
#include "save/Save.hpp"
#include "render/Renderer.hpp"
#include "video/Player.hpp"
#include "ui/Ui.hpp"

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
    std::filesystem::path screenshot, inputScript;
    double renderHz = 0; // > 0: fixed virtual frame time (deterministic replays); default 60 with --input-script
    std::optional<std::array<float, 4>> spawn; // --spawn X,Y,Z,YAWDEG overrides the unit's player intro
    bool intro = false;  // --intro: play Movies/intro.bik first (skippable)
    int introFrames = -1; // --intro-frames N: stop the intro / the New Game movie after N frames
    bool noMovies = false; // --no-movies: New Game skips Movies/explode.bik
    bool noAudio = false;
    std::filesystem::path musicWav; // --music-wav FILE: no device; every tick's mixer output goes to a WAV
    std::string loadSlot; // --load SLOT: start from $XDG_DATA_HOME/openunderworld/saves/SLOT.ousave
    std::string cine;     // --cine NAME: play pc-w\cinstream\NAME.mul in its unit (--frames/--screenshot apply), then exit
    double die = -1;      // --die SECONDS: Lara dies then (death clip) and respawns at the last checkpoint
    std::vector<std::array<float, 3>> targets; // --target X,Y,Z (repeatable): shootable dummy, feet in the unit frame
    std::optional<ou::game::Weapon> weapon;    // --weapon uzi|shotgun|speargun: carried secondary with max reserve
    ou::config::GraphicsConfig graphics; // --preset/--msaa/--shadows/--fxaa/--hdr/--rt/--rt-quality (or --config)
    ou::config::GraphicsSettings gfx;    // resolved
};

void usage() {
    std::fprintf(stderr,
                 "usage: ou-game --game-dir DIR [--level pc-w\\\\l4_pool.drm | --chapter 0-8] [--renderer auto|opengl|vulkan]\n"
                 "               [--frames N] [--screenshot out.ppm] [--input-script FILE] [--render-hz HZ]\n"
                 "               [--width W --height H] [--fov DEG] [--no-vsync] [--fullscreen] [--no-overlay] [--cine NAME]\n"
                 "               [--spawn X,Y,Z,YAWDEG] [--intro] [--intro-frames N] [--no-movies] [--no-audio] [--music-wav out.wav]\n"
                 "               [--load SLOT] [--die SECONDS] [--target X,Y,Z]... [--weapon uzi|shotgun|speargun]\n"
                 "               [--show-collision] [--show-triggers] [--show-enemies]\n"
                 "               [--preset original|enhanced] [--msaa 1|2|4|8] [--shadows on|off] [--fxaa on|off] [--hdr on|off]\n"
                 "               [--shaders original|generic]\n"
                 "               [--rt auto|off|shadows|full] [--rt-quality low|medium|high] [--brightness 0.5..2.0]\n"
                 "               [--water original|enhanced]\n"
                 "               [--config config.json]\n"
                 "without --level/--chapter/--load: main menu (New Game plays Movies\\explode.bik, then chapter 0)\n"
                 "controls: WASD/arrows move (camera-relative), Shift walk, Space jump/climb, Ctrl or C drop, E use/push/pick up,\n"
                 "          F draw/holster, mouse left fire, right aim/lock, Q switch target, X switch weapon, H health pack,\n"
                 "          E with a full adrenaline meter + lock close by: head-shot window, mouse look;\n"
                 "          gamepad: left stick, right stick, A jump, B drop, X use, Y draw, RT fire, LT lock, R3 switch, LB walk,\n"
                 "          d-pad right switch weapon, d-pad up health pack,\n"
                 "          Start pause; Esc pause menu, F1 overlay, F5 quicksave, F9 quickload (slot 'quick')\n"
                 "menus: arrows/WASD or d-pad navigate, Enter/Space/E or A accept, Esc/Backspace or B back\n"
                 "input script lines: <seconds> <forward|right|walk|jump|drop|interact|save|load|draw|fire|aim|switch|weapon|heal|pause|\n"
                 "                    lookx|looky>\n"
                 "                    [value]  (in menus forward/right +-1 navigate, jump or interact accepts, drop goes back;\n"
                 "                    script time runs on while paused)\n");
}

bool parseArgs(int argc, char** argv, Options& o) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) != "--config") continue;
        try {
            const auto c = ou::config::load(argv[i + 1]);
            o.gameDir = c.gameDir, o.renderer = c.renderer, o.width = c.width, o.height = c.height;
            o.fov = c.fov, o.vsync = c.vsync, o.fullscreen = c.fullscreen, o.graphics = c.graphics;
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
        else if (a == "--cine") o.cine = v;
        else if (a == "--target") {
            std::array<float, 3> t{};
            if (std::sscanf(v, "%f,%f,%f", &t[0], &t[1], &t[2]) != 3) { std::fprintf(stderr, "--target wants X,Y,Z\n"); return false; }
            o.targets.push_back(t);
        }
        else if (a == "--weapon") {
            o.weapon = ou::game::weaponByName(v);
            if (!o.weapon || *o.weapon == ou::game::Weapon::Pistols) { std::fprintf(stderr, "--weapon wants uzi, shotgun or speargun\n"); return false; }
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
    std::vector<ou::scene::Vertex> scratch;

    void index(const ou::scene::SceneData& s) {
        meshes.clear(), posed.clear(), hidden.clear();
        for (size_t i = 0; i < s.meshes.size(); ++i)
            if (const auto at = s.meshes[i].origin.rfind(" @intro "); at != std::string::npos)
                meshes[std::atoi(s.meshes[i].origin.c_str() + at + 8)].push_back(i);
    }
    // Picked-up rewards vanish: their meshes degenerate to one point (no scene rebuild).
    void hide(const std::set<int32_t>& ids, const ou::scene::SceneData& s, ou::render::Renderer& r) {
        for (const int32_t id : ids) {
            const auto it = meshes.find(id);
            if (it == meshes.end() || !hidden.insert(id).second) continue;
            for (const size_t i : it->second) {
                scratch = s.meshes[i].vertices;
                if (scratch.empty()) continue;
                for (auto& v : scratch) std::copy_n(scratch[0].position, 3, v.position);
                r.updateVertices(i, scratch);
            }
        }
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

// Enemies (src/game/ai): each enemy's model-0 skinned meshes, CPU-skinned from Enemy::pose and placed in the
// world like Lara's. Their intro meshes in the level scene are left out (StreamedScene::build). Enemy shots are
// drawn as tracers (yellow hit, white miss) for 0.2 s.
struct EnemyMeshes {
    std::map<std::string, ou::scene::SceneData> types; // object name -> model-0 skinned meshes + textures
    std::vector<std::vector<size_t>> meshIndices;       // per World::enemies().all() entry
    std::vector<ou::scene::Vertex> scratch;
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
    void pose(const ou::game::World& w, double alpha, const ou::scene::SceneData& scene, ou::render::Renderer& r) {
        const auto& all = w.enemies().all();
        for (size_t k = 0; k < all.size() && k < meshIndices.size(); ++k) {
            const auto& e = *all[k];
            const auto s = e.interpolated(alpha);
            const auto palette = ou::anim::skinPalette(e.type().skeleton, e.pose(alpha));
            const glm::mat4 m = ou::game::ai::Enemy::modelMatrix(s.pos, s.yaw, s.tip);
            for (const size_t i : meshIndices[k]) {
                ou::anim::skinVertices(scene.meshes[i], palette, scratch, m);
                r.updateVertices(i, scratch);
            }
        }
    }
};

// Combat (src/game/Combat.*): weapon models on Lara's hand / holster bones, --target dummies, muzzle flashes,
// tracers, impacts, the HUD (health bar, adrenaline meter, lock reticle or free-aim cross, the head-shot pip:
// screen-space lines just past the near plane) and each weapon's shot / reload waves. [I] the exe's flash/impact FX
// objects are not decoded.
struct CombatView {
    std::vector<ou::scene::Vertex> scratch;
    std::vector<std::unique_ptr<ou::game::DummyTarget>> dummies;
    struct Fx { ou::game::Shot shot; uint64_t tick; };
    std::vector<Fx> fx;
    struct Waves { std::vector<uint32_t> shots; uint32_t reloadStart = 0, reloadEnd = 0; };
    std::array<Waves, ou::game::kWeaponCount> waves; // by weapon
    uint32_t unholster = 0, next = 0;
    bool drawn = false, reloading = false, dead = false, slowMo = false;
    int healthPacksUsed = 0;
    ou::game::Weapon weapon = ou::game::Weapon::Pistols;

    // Shots: HG1_shot_c (handgun), MP5_shot (uzi), SG1_shot (shotgun), speargun_shot_b; reload: the HG1 magazine
    // release / insert (handgun, uzi), SG1_reload, speargun_cock_a.
    void addWaves(const ou::cdc::Drm& drm, ou::game::Weapon wpn) {
        static constexpr std::string_view kShot[] = {"HG1_shot_c", "MP5_shot", "SG1_shot", "speargun_shot_b"};
        static constexpr std::string_view kReload[] = {"HG1_mag_release", "HG1_mag_release", "SG1_reload", "speargun_cock_a_0"};
        auto& wv = waves[size_t(wpn)];
        for (const auto& s : drm.sections) {
            if (s.type != ou::cdc::SectionType::Wave) continue;
            const auto w = ou::audio::parseWave(s.data);
            if (w.bank.samples.empty()) continue;
            const std::string& n = w.bank.samples[0].name;
            if (n.starts_with(kShot[size_t(wpn)])) wv.shots.push_back(s.id);
            else if (n.starts_with(kReload[size_t(wpn)]) && !wv.reloadStart) wv.reloadStart = s.id;
            else if (n.starts_with("HG1_mag_in") && !wv.reloadEnd && wpn <= ou::game::Weapon::Uzi) wv.reloadEnd = s.id;
            else if (n.starts_with("hg1_unholster") && !unholster) unholster = s.id;
        }
    }

    void tick(const ou::game::World& w, ou::audio::Mixer* mix) {
        const auto& c = w.combat();
        const auto& s = w.player().state();
        const auto tick = (unsigned long long)w.ticks();
        const auto& wv = waves[size_t(c.weapon)];
        for (const auto& shot : c.shots) {
            fx.push_back({shot, w.ticks()});
            if (mix && !wv.shots.empty() && (&shot == &c.shots.front()))
                mix->play(wv.shots[next++ % wv.shots.size()], {.volume = 0.8f, .position = shot.from});
            std::fprintf(stderr, "ou-game: tick %llu shot %s %s %.0f %.0f %.0f -> %.0f %.0f %.0f%s damage %.1f\n", tick,
                         std::string(ou::game::weaponName(c.weapon)).c_str(), shot.hand ? "left" : "right", shot.from.x, shot.from.y,
                         shot.from.z, shot.to.x, shot.to.y, shot.to.z, shot.target ? " TARGET" : shot.hit ? " wall" : "", shot.damage);
        }
        std::erase_if(fx, [&](const Fx& f) { return w.ticks() - f.tick > 20; });
        if (mix && c.drawn && !drawn && unholster) mix->play(unholster, {.volume = 0.7f});
        if (mix && c.reload > 0 && !reloading && wv.reloadStart) mix->play(wv.reloadStart, {.volume = 0.7f});
        if (mix && c.reload <= 0 && reloading && wv.reloadEnd) mix->play(wv.reloadEnd, {.volume = 0.7f});
        if ((s.state == ou::game::State::Dead) != dead)
            std::fprintf(stderr, "ou-game: tick %llu %s (health %.0f, clip %s)\n", tick, dead ? "respawned" : "died", w.health().value(), s.clip.c_str());
        if (c.healthPacksUsed != healthPacksUsed)
            std::fprintf(stderr, "ou-game: tick %llu health pack used: health %.0f, %d left\n", tick, w.health().value(), w.found[2]);
        if (c.weapon != weapon) std::fprintf(stderr, "ou-game: tick %llu weapon %s\n", tick, std::string(ou::game::weaponName(c.weapon)).c_str());
        if ((c.slowMo > 0) != slowMo) std::fprintf(stderr, "ou-game: tick %llu adrenaline window %s\n", tick, c.slowMo > 0 ? "open" : "closed");
        if (s.state == ou::game::State::Hit && w.previous().clip != s.clip)
            std::fprintf(stderr, "ou-game: tick %llu hit reaction %s (health %.0f)\n", tick, s.clip.c_str(), w.health().value());
        drawn = c.drawn, reloading = c.reload > 0, dead = s.state == ou::game::State::Dead, slowMo = c.slowMo > 0;
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

    void lines(const ou::game::World& w, const ou::game::CameraState& cam, float fovDeg, int width, int height,
               std::vector<ou::render::DebugLine>& out) const {
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
        for (const auto& f : fx) {
            const uint64_t age = w.ticks() - f.tick;
            if (age <= 2) // muzzle flash star
                for (const glm::vec3 d : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1), glm::vec3(0.7f, 0.7f, 0), glm::vec3(0.7f, -0.7f, 0),
                                          glm::vec3(0, 0.7f, 0.7f), glm::vec3(0.7f, 0, -0.7f)})
                    line(f.shot.from - d * 30.0f, f.shot.from + d * 30.0f, 0xFF40D0FF);
            if (age <= 4) line(f.shot.from, f.shot.to, 0xFF90F0FF); // tracer
            if (f.shot.hit)
                for (const glm::vec3 d : {glm::vec3(1, 0, 0), glm::vec3(0, 1, 0), glm::vec3(0, 0, 1)})
                    line(f.shot.to - d * 15.0f, f.shot.to + d * 15.0f, f.shot.target ? 0xFF2020FF : 0xFFC0C0C0);
        }
        // HUD: pixel (x, y) from the top-left -> a point 10 units in front of the camera
        const glm::vec3 fwd = glm::normalize(cam.target - cam.position), right = glm::normalize(glm::cross(fwd, glm::vec3(0, 0, 1))),
                        up = glm::cross(right, fwd);
        const float ty = std::tan(glm::radians(fovDeg) * 0.5f), tx = ty * float(width) / float(height), d = 10.0f;
        auto px = [&](float x, float y) {
            return cam.position + fwd * d + right * ((2 * x / float(width) - 1) * d * tx) + up * ((1 - 2 * y / float(height)) * d * ty);
        };
        auto hline = [&](float x0, float x1, float y, uint32_t col) { line(px(x0, y), px(x1, y), col); };
        const float hp = std::clamp(w.health().value() / w.health().max(), 0.0f, 1.0f);
        const float x0 = 40, bw = 300, y0 = float(height) - 64;
        for (int y = -1; y <= 13; ++y) hline(x0 - 2, x0 + bw + 2, y0 + float(y), y == -1 || y == 13 ? 0xFF000000 : 0xFF303030);
        const uint32_t barCol = hp > 0.5f ? 0xFF40D040 : hp > 0.25f ? 0xFF20C0E0 : 0xFF2020E0;
        for (int y = 1; y <= 11; ++y) hline(x0, x0 + bw * hp, y0 + float(y), barCol);
        const auto& c = w.combat();
        for (int y = -1; y <= 5; ++y) // adrenaline meter under the health bar: orange, white while the window is open
            hline(x0 - 2, x0 + bw + 2, y0 + 18 + float(y), y == -1 || y == 5 ? 0xFF000000 : 0xFF303030);
        for (int y = 1; y <= 3; ++y) hline(x0, x0 + bw * c.adrenaline, y0 + 18 + float(y), c.slowMo > 0 ? 0xFFFFFFFF : 0xFF0090FF);
        if (!c.drawn) return;
        auto toScreen = [&](glm::vec3 p) -> std::optional<glm::vec2> {
            const glm::vec3 v = p - cam.position;
            const float z = glm::dot(v, fwd);
            if (z <= 1) return std::nullopt;
            return glm::vec2((glm::dot(v, right) / (z * tx) + 1) * 0.5f * float(width), (1 - glm::dot(v, up) / (z * ty)) * 0.5f * float(height));
        };
        const auto s = c.target ? toScreen(c.target->aimPoint()) : std::optional(glm::vec2(width, height) * 0.5f);
        if (!s) return;
        const uint32_t col = c.target ? (c.locked ? 0xFF2020FF : 0xFFFFFFFF) : 0xC0FFFFFF;
        const float r = c.target ? 22.0f : 8.0f;
        if (c.target) { // diamond + corner ticks
            const glm::vec2 q[4] = {{0, -r}, {r, 0}, {0, r}, {-r, 0}};
            for (int k = 0; k < 4; ++k) line(px(s->x + q[k].x, s->y + q[k].y), px(s->x + q[(k + 1) % 4].x, s->y + q[(k + 1) % 4].y), col);
        } else { // free-aim cross
            line(px(s->x - r, s->y), px(s->x + r, s->y), col);
            line(px(s->x, s->y - r), px(s->x, s->y + r), col);
        }
        if (c.slowMo > 0) { // head-shot pip: a small yellow cross, `pip` reticle radii from the reticle's centre
            const glm::vec2 p = *s + glm::vec2(c.pip.x, -c.pip.y) * r;
            line(px(p.x - 6, p.y), px(p.x + 6, p.y), 0xFF00FFFF);
            line(px(p.x, p.y - 6), px(p.x, p.y + 6), 0xFF00FFFF);
        }
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
    uint64_t version = ~0ull, enemyVersion = ~0ull;

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
        auto append = [&](const ou::scene::SceneData& s, glm::vec3 offset, std::vector<size_t>* indices) {
            const auto skelBase = int32_t(out.skeletons.size());
            out.skeletons.insert(out.skeletons.end(), s.skeletons.begin(), s.skeletons.end());
            for (auto m : s.meshes) {
                if (const auto at = m.origin.rfind(" @intro "); at != std::string::npos && enemyIntros.contains(std::atoi(m.origin.c_str() + at + 8)))
                    continue;
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
            for (auto l : s.environment.lights) {
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
            append(it->second, u.offset, nullptr);
        }
        laraMeshes.meshIndices.clear();
        append(lara, glm::vec3(0), &laraMeshes.meshIndices);
        enemyMeshes.meshIndices.clear();
        for (const auto& e : w.enemies().all()) {
            enemyMeshes.meshIndices.emplace_back();
            append(enemyMeshes.type(bf, e->type().name, warnings), glm::vec3(0), &enemyMeshes.meshIndices.back());
        }
        gunIndices.clear();
        append(guns, glm::vec3(0), &gunIndices);
        version = w.unitsVersion();
        enemyVersion = w.enemies().version();
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

// Music (docs/formats/AUDIO.md §5, GAMEPLAY.md "Music"): one track at a time, pc-w\music\<name>\<name>.mul
// streamed with the stem gains its .mus script computes (audio::MusSequencer). Tracks change when the current
// unit changes (the unit's own "<unit>_mus_01") and when Lara enters a volume whose script names a track
// (soundvolume/soundtriggervolume intros, UnitData::musicByIntro) [I]. The music variables stay at 0: who
// writes them (e.g. intensity 0x7D) is not decoded [U].
struct Music {
    std::string current;
    std::unique_ptr<ou::audio::MusFile> mus; // the sequencer keeps a reference to it
    std::unique_ptr<ou::audio::MusSequencer> seq;
    std::array<uint8_t, 256> vars{};

    bool play(const ou::cdc::BigfileSet& bf, ou::audio::Mixer& mixer, const std::string& name, const std::string& why, uint64_t tick) {
        if (name.empty() || name == current) return false;
        const std::string base = "pc-w\\music\\" + name + "\\" + name;
        try {
            auto mul = bf.read(base + ".mul");
            if (!mul) throw ou::cdc::FormatError(base + ".mul not in bigfile");
            auto pcm = std::make_shared<ou::audio::Pcm>(ou::audio::decodeMul(*mul).pcm);
            seq.reset(), mus.reset();
            if (auto m = bf.read(base + ".mus")) {
                mus = std::make_unique<ou::audio::MusFile>(ou::audio::parseMus(*m));
                seq = std::make_unique<ou::audio::MusSequencer>(*mus);
                seq->update(0, vars);
            }
            mixer.playMusic(std::move(pcm), current.empty() ? 1.0f : 2.0f, seq ? seq->stemGains() : std::vector<float>{});
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
    void tick(ou::audio::Mixer& mixer, float dt) {
        if (!seq) return;
        seq->update(dt, vars);
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

// Save slots in saveDir(), newest first.
std::vector<std::string> listSaves() {
    std::vector<std::pair<std::filesystem::file_time_type, std::string>> v;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ou::save::saveDir(), ec))
        if (e.path().extension() == ".ousave") v.emplace_back(e.last_write_time(ec), e.path().stem().string());
    std::sort(v.begin(), v.end(), std::greater<>());
    std::vector<std::string> out;
    for (auto& [t, s] : v) out.push_back(std::move(s));
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
    if (edge(a.jump, was.jump) || edge(a.interact, was.interact)) return Nav::Accept;
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
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: case SDLK_E: return Nav::Accept;
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return Nav::Back;
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
    case SDL_GAMEPAD_BUTTON_EAST: case SDL_GAMEPAD_BUTTON_START: return Nav::Back;
    default: return std::nullopt;
    }
}

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
    ou::scene::SceneData scene;
    StreamedScene streamed;
    LaraMeshes lara;
    EnemyMeshes enemyMeshes;
    ScriptedMeshes objects;
    std::vector<std::string> warnings;
    CombatView combat;
    bool spawnFromIntro = false;
    try {
        if (!opt.inputScript.empty()) script = ou::game::InputScript::load(opt.inputScript);
        bigfile = std::make_unique<const ou::cdc::BigfileSet>(opt.gameDir);
        const auto& bf = *bigfile;
        strings = ou::locale::loadStrings(opt.gameDir, ou::locale::Language::English);
        if (!strings) throw std::runtime_error("pc-w\\local\\locals.bin not in bigfile");
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
        if (!opt.loadSlot.empty() && !quickLoad(*world, opt.loadSlot)) return 1;

        auto laraScene = ou::formats::loadDrmScene(bf, "pc-w\\lara.drm");
        streamed.lara.skeletons = std::move(laraScene.skeletons);
        streamed.lara.textures = std::move(laraScene.textures);
        streamed.lara.materials = std::move(laraScene.materials);
        streamed.lara.shaders = std::move(laraScene.shaders);
        for (auto& m : laraScene.meshes)
            if (m.skeleton >= 0 && !m.indices.empty()) streamed.lara.meshes.push_back(std::move(m)); // skinned body only
        for (auto& w : laraScene.warnings) warnings.push_back(std::move(w));
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
        if (opt.weapon) // testing: the original gets its secondary and rounds from pickups
            world->giveWeapon(*opt.weapon, world->anims().weapons[size_t(*opt.weapon)].maxReserve);
        scene = streamed.build(bf, *world, lara, enemyMeshes, warnings);
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

    // ---- audio (ou_audio): Lara's Wave sections for footsteps/landings, music per unit / sound volume ----
    ou::audio::Mixer mixer; // after SdlInit: closes its device before SDL_Quit
    Footsteps steps;
    Music music;
    std::vector<float> wav; // --music-wav capture
    bool audioOn = false;
    if (!opt.noAudio) {
        try {
            if (auto laraDrm = bigfile->read("pc-w\\lara.drm")) mixer.addWaves(ou::cdc::parseDrm(std::move(*laraDrm)));
            for (int i = 0; i < ou::game::kWeaponCount; ++i) // shots, holster, reload waves
                if (auto gun = bigfile->read("pc-w\\" + std::string(ou::game::weaponObject(ou::game::Weapon(i))) + ".drm")) {
                    const auto drm = ou::cdc::parseDrm(std::move(*gun));
                    mixer.addWaves(drm);
                    combat.addWaves(drm, ou::game::Weapon(i));
                }
            audioOn = !opt.musicWav.empty() || mixer.openDevice();
            if (!audioOn) std::fprintf(stderr, "ou-game: no audio device (%s)\n", SDL_GetError());
            std::fprintf(stderr, "ou-game: audio %s\n", !opt.musicWav.empty() ? "to WAV" : audioOn ? "on" : "off");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: audio: %s\n", e.what());
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
    // Title card when the chapter changes (-2: not until New Game; --level/--load start without one).
    int shownChapter = menuStart ? -2 : opt.chapter >= 0 ? -1 : world->chapter;

    // ---- UI (src/ui): menus, PDA page, pickup notifications, chapter titles ----
    ou::ui::Menus menus;
    ou::ui::Hud hud;
    auto applyVolumes = [&] {
        mixer.setBusVolume(ou::audio::Bus::Music, float(menus.musicVolume) / 10);
        mixer.setBusVolume(ou::audio::Bus::Sfx, float(menus.sfxVolume) / 10);
    };
    applyVolumes();
    auto chapterName = [&](int c) { return std::string(strings->get(ou::game::kChapters[size_t(c)].nameId)); };
    auto openPause = [&] {
        menus.pda = {ou::game::kChapters[size_t(world->chapter)].nameId, world->found[0], world->found[1], world->found[2]};
        menus.saves = listSaves();
        menus.open(ou::ui::Screen::Pause);
        std::fprintf(stderr, "ou-game: tick %llu pause menu\n", (unsigned long long)world->ticks());
    };
    bool running = true;
    auto act = [&](ou::ui::Action action) {
        using ou::ui::Action;
        switch (action) {
        case Action::None: return;
        case Action::NewGame:
            std::fprintf(stderr, "ou-game: new game\n");
            if (!opt.noMovies) { // frontend state 8 PlayNewGameMovie (tru.exe 0x5DB110)
                SDL_GLContext gl = SDL_GL_GetCurrentContext(); // the movie's SDL_Renderer may switch contexts
                playMovie("explode.bik");
                if (gl) SDL_GL_MakeCurrent(window.get(), gl);
            }
            try {
                world->collected.clear(), world->found = {};
                world->script().restore(nlohmann::json::object());
                world->startChapter(0);
                shownChapter = -1;
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ou-game: new game: %s\n", e.what());
            }
            break;
        case Action::Resume: break;
        case Action::Save: {
            char slot[40];
            const std::time_t t = std::time(nullptr);
            std::strftime(slot, sizeof slot, "save-%Y%m%d-%H%M%S", std::localtime(&t));
            hud.notify(std::string(strings->get(quickSave(*world, slot) ? 18 : 5)), 2.0); // "Save Successful" / "Save failed."
            menus.saves = listSaves();
            break;
        }
        case Action::Load:
            if (!quickLoad(*world, menus.selectedSave())) hud.notify(std::string(strings->get(3)), 2.0); // "Load failed."
            shownChapter = world->chapter;
            break;
        case Action::QuitToMenu:
            menus.close();
            menus.saves = listSaves();
            menus.open(ou::ui::Screen::Main);
            shownChapter = -2;
            break;
        case Action::Quit: running = false; break;
        }
        std::fprintf(stderr, "ou-game: tick %llu menu action %d\n", (unsigned long long)world->ticks(), int(action));
    };
    auto nav = [&](ou::ui::Nav n) {
        const auto a = menus.input(n);
        applyVolumes();
        act(a);
    };
    if (menuStart) {
        menus.saves = listSaves();
        menus.open(ou::ui::Screen::Main);
    }
    // ---- cinematics (src/cine, app/game/Cinematic.*): --cine, and the cinematic_objects scripts activate (level ends) ----
    auto cineView = [&] {
        ou::app::CineView v{*bigfile, *world, *renderer, window.get(), audioOn ? &mixer : nullptr, scene, lara.meshIndices, streamed.gunIndices};
        v.renderHz = opt.renderHz;
        v.frame.msaa = opt.gfx.msaa, v.frame.shadows = opt.gfx.shadows, v.frame.fxaa = opt.gfx.fxaa, v.frame.hdr = opt.gfx.hdr;
        v.frame.rt = ou::render::RayTracing(opt.gfx.rt), v.frame.rtRays = opt.gfx.rtRays;
        v.frame.originalShaders = opt.gfx.originalShaders, v.frame.brightness = opt.gfx.brightness;
        v.overlay = !opt.noOverlay;
        v.musicVolume = float(menus.musicVolume) / 10;
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
            if (r.quit) running = false;
            if (!levelEnd && r.laraPos) { // the stream leaves Lara where it ends (0x455D90)
                auto s = world->player().state();
                s.pos = *r.laraPos, s.yaw = r.laraYaw, s.vel = glm::vec3(0);
                world->restore(s, world->camera().yaw, world->camera().pitch);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: cinematic %s: %s\n", name.c_str(), e.what());
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

    while (running) {
        const bool inMenu = menus.screen() != ou::ui::Screen::None;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_EVENT_QUIT: running = false; break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.key == SDLK_F1 && !e.key.repeat) overlay = !overlay;
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
        last = now;

        for (int n = clock.advance(dt); n > 0 && running; --n) {
            ou::game::Actions a = script ? script->at(appTicks)
                                         : ou::game::readDevices(SDL_GetKeyboardState(nullptr), pad.get(),
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
            // Mouse motion since the last tick, as a rate over this tick.
            a.look -= mouse * kMouseSensitivity / float(ou::game::kTickSeconds);
            mouse = glm::vec2(0);
            if (a.save && !held.save) quickSave(*world, "quick");
            if (a.load && !held.load) quickLoad(*world, "quick"), shownChapter = world->chapter;
            held = a;
            if (world->ticks() == dieTick) {
                world->kill(); // death clip, then World respawns her at the last checkpoint
                logState("killed (--die)", *world);
            }
            world->tick(a);
            combat.tick(*world, audioOn ? &mixer : nullptr);
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
                if (audioOn) music.play(*bigfile, mixer, world->currentUnit().data.music, "unit " + unitName, tick);
            }
            if (audioOn) {
                for (const auto& e : world->script().events())
                    if (e.kind == ou::script::EventKind::Enter)
                        for (const auto& u : world->units())
                            if (const auto it = u.data.musicByIntro.find(e.source); it != u.data.musicByIntro.end())
                                music.play(*bigfile, mixer, it->second, "volume " + std::to_string(e.source), tick);
                music.tick(mixer, float(ou::game::kTickSeconds));
                steps.tick(*world, mixer);
            }
            hud.tick(ou::game::kTickSeconds);
        }
        if (world->unitsVersion() != streamed.version || world->enemies().version() != streamed.enemyVersion) {
            // streamed in/out, loaded, respawned or enemies spawned: new render scene
            try {
                scene = streamed.build(*bigfile, *world, lara, enemyMeshes, warnings);
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
        enemyMeshes.pose(*world, clock.alpha(), scene, *renderer);
        objects.pose(world->script(), scene, *renderer);
        combat.pose(*world, s, clock.alpha(), scene, streamed.gunIndices, streamed.gunSlots, *renderer);
        objects.hide(world->collected, scene, *renderer);
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
            lines.push_back(fmt("OpenUnderworld ou-game | %s | %.1f FPS | tick %llu%s%s", world->currentUnit().data.name.c_str(), fps,
                                (unsigned long long)world->ticks(), script ? " | REPLAY " : "", pad ? " | gamepad" : ""));
            lines.push_back(renderer->description());
            lines.push_back(ou::render::rtSummary(renderer->rtStatus(), ou::render::RayTracing(opt.gfx.rt), opt.gfx.rtRays));
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
            lines.push_back("WASD move | Shift walk | Space jump/climb | Ctrl/C drop | E use/push/pick up | F draw | LMB fire | RMB lock | "
                            "Q switch | X weapon | H health pack | mouse look | F5 save | F9 load | F1 overlay | Esc menu");
        }
        { // combat HUD text (always shown)
            const auto& cs = world->combat();
            std::string t = fmt("health %.0f/%.0f | health packs %d | adrenaline %.0f%%%s", world->health().value(), world->health().max(),
                                world->found[2], cs.adrenaline * 100, cs.slowMo > 0 ? " HEAD SHOT" : "");
            const auto& wd = world->anims().weapons[size_t(cs.weapon)];
            const auto wi = size_t(cs.weapon);
            t += fmt(" | %s %d", std::string(ou::game::weaponName(cs.weapon)).c_str(), cs.clip[wi][0]);
            if (wd.dual) t += fmt("+%d", cs.clip[wi][1]);
            t += wd.infinite ? std::string(" / inf") : fmt(" / %d", cs.reserve[wi]);
            if (cs.secondary != ou::game::Weapon::Pistols && cs.secondary != cs.weapon)
                t += fmt(" (%s %d)", std::string(ou::game::weaponName(cs.secondary)).c_str(), cs.reserve[size_t(cs.secondary)]);
            if (cs.drawn)
                t += fmt("%s | %s", cs.reload > 0 ? " RELOADING" : "", cs.target ? (cs.locked ? "LOCKED" : "target") : "free aim");
            for (const auto& d : combat.dummies) t += fmt(" | dummy %.0f hp", d->hp);
            lines.push_back(t);
        }
        ou::ui::Canvas canvas(w, h);
        hud.draw(canvas);
        if (world->air() < 0.999f) { // breath bar while it is not full (under water / refilling)
            const float u = canvas.unit(), bw = 160 * u, bh = 10 * u, x = 20 * u, y = 40 * u;
            canvas.rect(x - 2 * u, y - 2 * u, bw + 4 * u, bh + 4 * u, ou::ui::kShade);
            canvas.rect(x, y, bw * world->air(), bh, world->air() < 0.25f ? 0xFF0000FFu : 0xFFC0A040u);
            canvas.text(x, y + 13 * u, 12 * u, "AIR", ou::ui::kWhite);
        }
        menus.draw(canvas, *strings);
        // Two persistent buffers, alternated: the GL backend caches line uploads by data pointer + size, and these
        // lines change every frame (HUD, effects), so consecutive frames must never share a pointer.
        auto& frameLines = lineBuffers[frame & 1];
        frameLines.assign(debugLines.begin(), debugLines.end());
        if (opt.showTriggers) appendTriggerLines(world->script(), frameLines);
        if (opt.showEnemies) appendEnemyLines(*world, frameLines);
        enemyMeshes.tracerLines(world->ticks(), frameLines);
        combat.lines(*world, c, world->fovDeg, w, h, frameLines);
        ou::render::FrameOptions fo{};
        fo.lighting = true, fo.cullBackFaces = true;
        fo.overlay = lines, fo.debugLines = frameLines;
        fo.ui = canvas.vertices;
        fo.msaa = opt.gfx.msaa, fo.shadows = opt.gfx.shadows, fo.fxaa = opt.gfx.fxaa, fo.hdr = opt.gfx.hdr;
        fo.rt = ou::render::RayTracing(opt.gfx.rt), fo.rtRays = opt.gfx.rtRays;
        fo.originalShaders = opt.gfx.originalShaders, fo.time = float((double(clock.ticks()) + clock.alpha()) * clock.step());
        fo.brightness = opt.gfx.brightness;
        fo.water = ou::render::WaterQuality(opt.gfx.water);
        const auto& ws = world->water();
        if (opt.gfx.water && ws.active())
            fo.ripples = {ws.originX(), ws.originY(), ou::game::WaterSim::kCell, ws.surfaceZ(), ou::game::WaterSim::kSize,
                          ws.heights(), ws.version()};
        waterVolumes.clear();
        for (const auto& m : world->collision().markup)
            if (m.type == 10 && m.points.size() == 2)
                waterVolumes.push_back({{m.points[0].x, m.points[0].y, m.points[0].z},
                                        {m.points[1].x, m.points[1].y, m.points[1].z}});
        fo.waterVolumes = waterVolumes;
        splashParticles.clear();
        if (opt.gfx.water)
            for (const auto& d : ws.drops()) splashParticles.push_back({{d.pos.x, d.pos.y, d.pos.z}, d.radius, d.alpha()});
        fo.splashes = splashParticles;
        renderer->drawFrame(cam, {w, h}, fo);
        ++frame;
        if (opt.frames && frame >= opt.frames) {
            running = false;
            std::fprintf(stderr, "ou-game: final tick %llu unit %s chapter %d checkpoint %d found %d/%d/%d state %s pos %.1f %.1f %.1f yaw %.1f clip %s\n",
                         (unsigned long long)world->ticks(), world->currentUnit().data.name.c_str(), world->chapter,
                         world->checkpoint() ? world->checkpoint()->id : 0, world->found[0], world->found[1], world->found[2],
                         std::string(ou::game::stateName(s.state)).c_str(), s.pos.x, s.pos.y, s.pos.z, glm::degrees(s.yaw), s.clip.c_str());
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
    if (!opt.musicWav.empty()) {
        if (!writeWav(opt.musicWav, wav, mixer.outputRate())) std::fprintf(stderr, "ou-game: cannot write %s\n", opt.musicWav.string().c_str());
        else std::fprintf(stderr, "ou-game: wrote %s (%.2f s)\n", opt.musicWav.string().c_str(), double(wav.size() / 2) / mixer.outputRate());
    }
    renderer->shutdown();
    return 0;
}
