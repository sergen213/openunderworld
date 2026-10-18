// ou-game cutscene playback. Docs: docs/formats/CINEMATICS.md "Playback in ou-game".
#include "app/game/Cinematic.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>

#include <SDL3/SDL.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "anim/Anim.hpp"
#include "audio/Audio.hpp"
#include "cine/Cine.hpp"
#include "formats/Formats.hpp"
#include "locale/Locale.hpp"
#include "ui/Ui.hpp"

namespace ou::app {

namespace {

std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

glm::mat4 introWorld(const formats::Intro& in, glm::vec3 offset) {
    float m[16];
    formats::introMatrix(in, m);
    glm::mat4 r = glm::make_mat4(m);
    r[3] += glm::vec4(offset, 0.0f);
    return r;
}

struct Actor {
    const cine::Instance* inst;
    const scene::Skeleton* skeleton; // null: rigid meshes follow bone 0
    std::vector<size_t> meshes;      // into the uploaded scene
};

} // namespace

CineResult playCinematic(const std::string& name, const CineView& v) {
    auto mul = v.bigfile.read("pc-w\\cinstream\\" + name + ".mul");
    if (!mul) throw std::runtime_error("pc-w\\cinstream\\" + name + ".mul not in bigfile");
    cine::Player player(cine::demux(*mul));
    const auto& h = player.header();

    // Intros of the loaded units, the cinematic's own unit first (ids are looked up per unit, 0x5B38D0).
    struct UnitIntros { const game::LoadedUnit* unit; std::vector<formats::Intro> intros; };
    std::vector<UnitIntros> units;
    for (const auto& u : v.world.units()) units.push_back({&u, formats::parseUnit(*u.data.drm).intros});
    std::stable_partition(units.begin(), units.end(), [&](const auto& x) { return x.unit->data.name == lower(h.unitName); });
    auto find = [&](int32_t id) -> std::pair<const formats::Intro*, glm::vec3> {
        for (const auto& u : units)
            for (const auto& in : u.intros)
                if (in.uniqueId == id) return {&in, u.unit->offset};
        return {nullptr, glm::vec3(0)};
    };

    // Scene origin from the first area marker present (0x457100 takes the one in the player's unit).
    const cine::Marker* marker = nullptr;
    glm::mat4 markerIntro(1.0f);
    for (const auto& m : h.markers)
        if (const auto [in, off] = find(m.intro); in) {
            marker = &m, markerIntro = introWorld(*in, off);
            break;
        }
    if (!marker) std::fprintf(stderr, "ou-game: cinematic %s: no area marker loaded (unit %s); origin at 0\n", name.c_str(), h.unitName.c_str());

    // Actors: Lara's meshes of the game scene, other instances from their object DRM (render model 0).
    const auto list = v.bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    const auto objectNames = list ? formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()})
                                  : std::map<uint16_t, std::string>{};
    scene::SceneData sc = v.scene;
    std::set<uint32_t> textureIds, materialIds;
    std::set<uint64_t> shaderIds;
    for (const auto& t : sc.textures) textureIds.insert(t.id);
    for (const auto& m : sc.materials) materialIds.insert(m.id);
    for (const auto& c : sc.shaders) shaderIds.insert(c.hash);
    std::vector<Actor> actors;
    std::set<int32_t> actorIntros;
    for (const auto& inst : h.instances) {
        if (inst.intro == -1) {
            actors.push_back({&inst, &v.world.anims().skeleton, v.laraMeshes});
            continue;
        }
        const auto [in, off] = find(inst.intro);
        if (!in) {
            std::fprintf(stderr, "ou-game: cinematic %s: intro %d not loaded\n", name.c_str(), inst.intro);
            continue;
        }
        actorIntros.insert(inst.intro);
        const auto obj = objectNames.find(in->objectId);
        if (obj == objectNames.end()) continue;
        scene::SceneData s;
        try {
            s = formats::loadDrmScene(v.bigfile, "pc-w\\" + obj->second + ".drm");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: cinematic %s: %s: %s\n", name.c_str(), obj->second.c_str(), e.what());
            continue;
        }
        uint32_t model = 0; // skinned meshes of render model 0, else every rigid mesh
        if (auto bytes = v.bigfile.read("pc-w\\" + obj->second + ".drm"))
            for (const uint32_t id : formats::objectRenderModels(cdc::parseDrm(std::move(*bytes))))
                if ((model = id)) break;
        const bool skinned = std::any_of(s.meshes.begin(), s.meshes.end(), [&](const auto& m) {
            return m.skeleton >= 0 && s.skeletons[size_t(m.skeleton)].renderModelId == model;
        });
        std::erase_if(s.meshes, [&](const auto& m) {
            return skinned ? m.skeleton < 0 || s.skeletons[size_t(m.skeleton)].renderModelId != model : m.skeleton >= 0;
        });
        if (s.meshes.empty()) continue; // rigs without geometry (light rigs, cameras)
        Actor a{&inst, nullptr, {}};
        const size_t skelBase = sc.skeletons.size();
        sc.skeletons.insert(sc.skeletons.end(), s.skeletons.begin(), s.skeletons.end());
        for (auto& m : s.meshes) {
            if (m.skeleton >= 0) m.skeleton += int32_t(skelBase);
            a.meshes.push_back(sc.meshes.size());
            sc.meshes.push_back(std::move(m));
        }
        for (auto& t : s.textures)
            if (textureIds.insert(t.id).second) sc.textures.push_back(std::move(t));
        for (auto& m : s.materials) // original shaders (docs/formats/SHADERS.md)
            if (materialIds.insert(m.id).second) sc.materials.push_back(std::move(m));
        for (auto& c : s.shaders)
            if (shaderIds.insert(c.hash).second) sc.shaders.push_back(std::move(c));
        actors.push_back(std::move(a));
    }
    // Pointers into sc.skeletons only now that it no longer grows.
    for (auto& a : actors)
        if (a.inst->intro != -1 && sc.meshes[a.meshes[0]].skeleton >= 0) a.skeleton = &sc.skeletons[size_t(sc.meshes[a.meshes[0]].skeleton)];

    for (auto& w : v.renderer.upload(sc)) std::fprintf(stderr, "ou-game: cinematic upload: %s\n", w.c_str());
    std::vector<scene::Vertex> scratch;
    auto collapse = [&](size_t i) {
        scratch = sc.meshes[i].vertices;
        if (scratch.empty()) return;
        for (auto& vx : scratch) std::copy_n(scratch[0].position, 3, vx.position);
        v.renderer.updateVertices(i, scratch);
    };
    for (const size_t i : v.hide) collapse(i);
    for (size_t i = 0; i < v.scene.meshes.size(); ++i) // the actors' static copies in the level scene
        if (const auto at = sc.meshes[i].origin.rfind(" @intro "); at != std::string::npos && actorIntros.contains(std::atoi(sc.meshes[i].origin.c_str() + at + 8)))
            collapse(i);

    // Audio: the .mul's own sound track (voice, music, effects), started with frame 0. [I] kept in step by wall time,
    // the exe drives the frame position from the stream's playback clock.
    audio::VoiceId voice = 0;
    if (v.mixer) {
        try {
            auto pcm = std::make_shared<const audio::Pcm>(audio::decodeMul(*mul).pcm);
            v.mixer->setBusVolume(audio::Bus::Music, 0);
            voice = v.mixer->playVoice(pcm);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: cinematic %s audio: %s\n", name.c_str(), e.what());
        }
    }
    std::fprintf(stderr, "ou-game: cinematic %s (unit %s): %zu actors, %zu cameras, %u channels\n", name.c_str(), h.unitName.c_str(),
                 actors.size(), h.cameras.size(), h.channels);

    CineResult r;
    locale::SubtitleTicker ticker;
    double t = 0;
    uint64_t last = SDL_GetTicksNS();
    std::vector<std::string> overlay;
    for (bool running = true; running;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) r.quit = true, running = false;
            else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE && !e.key.repeat) r.skipped = true, running = false;
            else if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && e.gbutton.button == SDL_GAMEPAD_BUTTON_START) r.skipped = true, running = false;
            else if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) v.renderer.resize({e.window.data1, e.window.data2});
        }
        if (!running) break;
        const uint64_t now = SDL_GetTicksNS();
        const double dt = r.frames == 0 ? 0.0 : v.renderHz > 0 ? 1.0 / v.renderHz : double(now - last) * 1e-9;
        last = now;
        t += dt;
        const bool more = player.seek(t * cine::kFps);
        if (marker) player.origin = player.originFromMarker(*marker, markerIntro);
        ticker.advance(dt * cine::kFps);
        for (const auto& s : player.newSubtitles())
            if (s.language == int(locale::Language::English))
                for (const auto& line : locale::cineSubtitleLines(s.text)) ticker.push(line);

        for (const auto& a : actors) {
            const scene::Skeleton one{{scene::Bone{}}, 0, {}};
            const auto bones = player.bones(*a.inst, a.skeleton ? *a.skeleton : one);
            const auto palette = a.skeleton ? anim::skinPalette(*a.skeleton, bones) : std::vector<glm::mat4>{};
            for (const size_t i : a.meshes) {
                const auto& m = sc.meshes[i];
                if (a.skeleton && !m.skin.empty()) {
                    anim::skinVertices(m, palette, scratch);
                } else { // rigid: model space = bone 0
                    scratch = m.vertices;
                    const glm::mat4 b0 = bones[0];
                    for (auto& vx : scratch) {
                        const glm::vec3 p(b0 * glm::vec4(vx.position[0], vx.position[1], vx.position[2], 1));
                        const glm::vec3 n(glm::mat3(b0) * glm::vec3(vx.normal[0], vx.normal[1], vx.normal[2]));
                        std::copy_n(&p.x, 3, vx.position), std::copy_n(&n.x, 3, vx.normal);
                    }
                }
                v.renderer.updateVertices(i, scratch);
            }
        }

        int w = 0, hgt = 0;
        SDL_GetWindowSizeInPixels(v.window, &w, &hgt);
        const auto pose = player.camera();
        render::Camera cam;
        cam.position = pose.position;
        cam.lookAt(pose.position + pose.forward);
        cam.up = pose.up;
        cam.verticalFovDeg = glm::degrees(pose.verticalFov(hgt > 0 ? float(w) / float(hgt) : 1.0f));
        cam.nearZ = 2.0f;
        cam.farZ = 200000.0f;
        if (v.mixer) v.mixer->setListener({pose.position, pose.forward, pose.up});

        ui::Canvas canvas(w, hgt); // subtitle ticker: two centred lines at y 352 / 374 of 640x480 (TEXT.md §4)
        const float u = canvas.unit(), g = std::round(12 * u);
        const std::string* lines[2] = {&ticker.top(), &ticker.bottom()};
        for (int k = 0; k < 2; ++k)
            if (!lines[k]->empty()) {
                canvas.textCentered(float(w) / 2 + std::max(1.0f, u), (352 + 22 * k) * u + std::max(1.0f, u), g, *lines[k], 0xC0000000u);
                canvas.textCentered(float(w) / 2, (352 + 22 * k) * u, g, *lines[k], 0xFFFFFFFFu);
            }
        overlay.clear();
        if (v.overlay) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "cinematic %s | frame %d | %.2f s | Esc skips", name.c_str(), player.frame(), t);
            overlay.emplace_back(buf);
        }
        render::FrameOptions fo = v.frame;
        fo.overlay = overlay;
        fo.debugLines = {};
        fo.ui = canvas.vertices;
        v.renderer.drawFrame(cam, {w, hgt}, fo);
        ++r.frames;
        if (v.maxFrames && r.frames >= v.maxFrames) {
            r.lastImage = v.renderer.readBackbuffer({w, hgt});
            running = false;
        }
        v.renderer.present();
        if (!more) running = false;
    }
    if (v.mixer) {
        if (voice) v.mixer->stop(voice, 0.3f);
        v.mixer->setBusVolume(audio::Bus::Music, v.musicVolume);
    }
    // Where Lara stands afterwards: tail position + the marker intro of her unit, heading from the tail rotation.
    for (const auto& inst : h.instances)
        if (inst.intro == -1 && inst.endPos)
            for (const auto& m : h.markers)
                if (const auto [in, off] = find(m.intro); in && m.unit == inst.endUnit) {
                    r.laraPos = *inst.endPos + glm::vec3(in->position[0], in->position[1], in->position[2]) + off;
                    r.laraYaw = (*inst.endRot).z - glm::half_pi<float>(); // intro yaw -> heading (World::findSpawn)
                }
    std::fprintf(stderr, "ou-game: cinematic %s: %d frames to frame %d%s\n", name.c_str(), r.frames, player.frame(),
                 r.skipped ? " (skipped)" : r.quit ? " (closed)" : "");
    return r;
}

} // namespace ou::app
