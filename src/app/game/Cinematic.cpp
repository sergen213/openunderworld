// ou-game cutscene playback. Docs: docs/formats/CINEMATICS.md "Playback in ou-game".
#include "app/game/Cinematic.hpp"
#include "app/game/Sound.hpp"

#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <optional>
#include <stdexcept>

#include <SDL3/SDL.h>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "anim/Anim.hpp"
#include "audio/Audio.hpp"
#include "cine/Cine.hpp"
#include "formats/Formats.hpp"
#include "fx/Fx.hpp"
#include "locale/Locale.hpp"
#include "render/RenderCommon.hpp"
#include "ui/Art.hpp"
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
    const scene::Skeleton* skeleton = nullptr; // null: one bone, rigid meshes follow bone 0
    int32_t skelIndex = -1;          // into the cinematic scene's skeletons until they stop growing
    std::vector<size_t> meshes;      // into the uploaded scene
    bool rows = false;               // meshes take the bones' skin-palette 4th rows in COLOR0 (Material::skinParams)
    size_t firstLight = 0;           // its lights' slots in the scene's environment lights
};

// globalaniminfo's cinematic settings (ObjectBase +0xC8; tru.exe 0x4533A0 keeps the pointer at handler +0x10C):
// +0x14 the 20 light DTP templates (cine::Light::table), +0x68 the filter resource of the DOF cameras (0x4547D0).
struct CineSettings {
    std::array<std::optional<scene::Light>, 20> lights;
    std::shared_ptr<const fx::FilterResource> dofFilter;
};

CineSettings cineSettings(const cdc::BigfileSet& bf) {
    CineSettings s;
    auto bytes = bf.read("pc-w\\globalaniminfo.drm");
    if (!bytes) return s;
    const auto drm = cdc::parseDrm(std::move(*bytes));
    const auto ob = formats::objectBaseSection(drm);
    if (!ob) return s;
    const auto ptrs = cdc::sectionPointers(drm, *ob);
    const auto p = ptrs.find(0xC8);
    if (p == ptrs.end()) return s;
    const auto& d = drm.sections.at(p->second.section).data;
    auto id = [&](uint32_t o) {
        uint32_t v = 0;
        if (p->second.offset + o + 4 <= d.size()) std::memcpy(&v, d.data() + p->second.offset + o, 4);
        return v == ~0u ? 0u : v;
    };
    for (uint32_t k = 0; k < s.lights.size(); ++k)
        for (uint32_t i = 0; i < drm.sections.size() && id(0x14 + 4 * k); ++i)
            if (drm.sections[i].type == cdc::SectionType::DtpData && drm.sections[i].id == id(0x14 + 4 * k))
                s.lights[k] = formats::lightFromDtp(drm, i);
    if (const uint32_t f = id(0x68)) s.dofFilter = fx::loadFilterResource(drm, f);
    return s;
}

// A light without a template (0x4551B0's defaults): radius 5000, intensity 1, falloff 1 - (d / radius)^2.
scene::Light defaultCineLight() {
    scene::Light l;
    l.radius = 5000;
    l.falloff.resize(256);
    for (int k = 0; k < 256; ++k) l.falloff[size_t(k)] = uint8_t(k < 255 ? std::lround((1.0 - (k / 255.0) * (k / 255.0)) * 255.0) : 0);
    return l;
}

// The scene light of a posed cinematic light: the template's radius, falloff, flicker and shadow flag, the channels'
// type, colour x intensity and cone; off = dark and far away (the slot stays reserved).
void poseLight(scene::Light& l, const cine::LightPose& p) {
    l.type = p.type == 1 ? 1 : 0;
    for (int i = 0; i < 3; ++i) {
        l.position[i] = p.on ? p.matrix[3][i] : 1e9f;
        l.color[i] = p.on ? p.colour[i] * p.intensity : 0.0f;
    }
    const glm::vec3 dir = glm::normalize(glm::vec3(p.matrix[2]));
    for (int i = 0; i < 3; ++i) l.direction[i] = dir[i];
    l.coneInner = p.coneInner, l.coneOuter = p.coneOuter;
    l.mask = p.mask; // reaches the receivers (MeshData::lightMask) it shares a bit with
}

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
            actors.push_back({.inst = &inst, .skeleton = &v.world.anims().skeleton, .meshes = v.laraMeshes});
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
        // Light rigs (co_light_rig*) have no geometry but bones for their lights.
        if (s.meshes.empty() && (inst.lights.empty() || s.skeletons.empty())) continue;
        Actor a{&inst};
        const size_t skelBase = sc.skeletons.size();
        sc.skeletons.insert(sc.skeletons.end(), s.skeletons.begin(), s.skeletons.end());
        if (s.meshes.empty()) a.skelIndex = int32_t(skelBase);
        for (auto& m : s.meshes) {
            if (m.skeleton >= 0) m.skeleton += int32_t(skelBase), a.skelIndex = m.skeleton;
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
    std::set<uint32_t> rowMaterials;
    for (const auto& m : sc.materials)
        if (m.skinParams) rowMaterials.insert(m.id);
    for (auto& a : actors) {
        if (a.skelIndex >= 0) a.skeleton = &sc.skeletons[size_t(a.skelIndex)];
        for (const size_t i : a.meshes)
            for (const auto& sm : sc.meshes[i].submeshes) a.rows |= a.inst->intro == -1 || rowMaterials.contains(sm.materialId);
        // Instance channel 2 -> render component +0x20: the receiver mask the rig lights test (16 for most actors) [I].
        for (const size_t i : a.meshes) sc.meshes[i].lightMask = uint32_t(std::lround(player.raw(a.inst->channel + 2)));
    }

    // Lights (0x454BD0): one environment slot each, first in the list (OriginalShading takes the first lights that
    // reach an object), template from globalaniminfo, posed per frame through FrameOptions::dynamicLights.
    const CineSettings settings = cineSettings(v.bigfile);
    std::vector<scene::Light> lights;
    for (auto& a : actors) {
        a.firstLight = lights.size();
        for (const auto& l : a.inst->lights) {
            auto& s = lights.emplace_back(l.table < settings.lights.size() && settings.lights[l.table] ? *settings.lights[l.table] : defaultCineLight());
            s.position[0] = s.position[1] = s.position[2] = 1e9f, s.color[0] = s.color[1] = s.color[2] = 0;
        }
    }
    if (sc.environment.valid) sc.environment.lights.insert(sc.environment.lights.begin(), lights.begin(), lights.end());
    else lights.clear();

    // Lara's hair (render-model cloth chains) simulated on the stream's pose, as in play (World::updatePose).
    anim::HairSim hair;
    if (!v.world.anims().hair.particles.empty()) hair = anim::HairSim(v.world.anims().hair, v.world.anims().skeleton, game::laraHair());

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

    // Audio: the .mul's own sound track (voice, music, effects), started with frame 0; its playback position is the
    // frame clock (cine::Clock), as the exe drives the frame position from the stream's playback clock.
    audio::VoiceId voice = 0;
    if (v.mixer) {
        try {
            auto pcm = std::make_shared<const audio::Pcm>(audio::decodeMul(*mul).pcm);
            v.mixer->setBusVolume(audio::Bus::Music, 0), v.mixer->setBusVolume(audio::Bus::Sfx, 0); // [I] gameplay audio paused
            voice = v.mixer->playVoice(pcm);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: cinematic %s audio: %s\n", name.c_str(), e.what());
        }
    }
    std::fprintf(stderr, "ou-game: cinematic %s (unit %s): %zu actors, %zu cameras, %u channels\n", name.c_str(), h.unitName.c_str(),
                 actors.size(), h.cameras.size(), h.channels);

    CineResult r;
    locale::SubtitleTicker ticker;
    cine::Clock clock;
    double t = 0, hairTime = 0;
    bool hairFresh = true;
    glm::vec3 laraRoot(0);
    uint64_t last = SDL_GetTicksNS();
    std::vector<std::string> overlay;
    std::vector<scene::Light> posedLights = lights;
    render::ScreenFilter filter;
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
        // Fixed frame time (replays, screenshots) stays deterministic; else the sound track's position leads.
        const double step = clock.update(dt, voice && v.renderHz <= 0 ? v.mixer->position(voice) : std::nullopt) - t;
        t = clock.seconds();
        const bool more = player.seek(t * cine::kFps);
        if (marker) player.origin = player.originFromMarker(*marker, markerIntro);
        ticker.advance(step * cine::kFps);
        for (const auto& s : player.newSubtitles())
            if (s.language == v.language)
                for (const auto& line : locale::cineSubtitleLines(s.text)) ticker.push(line);

        for (const auto& a : actors) {
            const scene::Skeleton one{{scene::Bone{}}, 0, {}};
            auto bones = player.bones(*a.inst, a.skeleton ? *a.skeleton : one);
            if (a.inst->intro == -1 && !hair.empty()) { // world-space bones: the hair's model frame is the world
                const glm::vec3 root(bones[0][3]);
                // [I] reset when she jumps between shots (the exe resets at frame 1 and at record +0x3C's frame)
                if (hairFresh || glm::distance(root, laraRoot) > 200.0f) hair.reset(bones, glm::mat4(1.0f)), hairTime = 0;
                for (hairTime += step; hairTime >= 1.0 / 60; hairTime -= 1.0 / 60) hair.step(bones, glm::mat4(1.0f), 1.0f / 60);
                hair.apply(bones, glm::mat4(1.0f));
                hairFresh = false, laraRoot = root;
            }
            if (!lights.empty() && !a.inst->lights.empty()) {
                const auto lp = player.lights(*a.inst, bones);
                for (size_t k = 0; k < lp.size(); ++k) poseLight(posedLights[a.firstLight + k], lp[k]);
            }
            const auto palette = a.skeleton ? anim::skinPalette(*a.skeleton, bones) : std::vector<glm::mat4>{};
            const auto rows = a.rows ? player.boneRows(*a.inst) : std::vector<glm::vec4>{};
            for (const size_t i : a.meshes) {
                const auto& m = sc.meshes[i];
                if (a.skeleton && !m.skin.empty()) anim::skinVertices(m, palette, scratch);
                else anim::skinVertices(m, {}, scratch, bones[0]); // rigid: model space = bone 0
                // COLOR0 = the weighted 4th palette rows the skinned VS blends (bones the stream lacks: 0, 0x5B60F0
                // after a memset), each component a byte of the sum (weights sum to 255), as LaraMeshes::pose.
                for (size_t vx = 0; vx < m.skin.size() && vx < scratch.size() && !rows.empty(); ++vx) {
                    glm::vec4 s(0);
                    for (int j = 0; j < 4; ++j)
                        if (m.skin[vx].bone[j] < rows.size()) s += float(m.skin[vx].weight[j]) * rows[m.skin[vx].bone[j]];
                    uint32_t c = 0;
                    for (int j = 0; j < 4; ++j) c |= uint32_t(std::clamp(std::lround(s[j]), 0L, 255L)) << (8 * j);
                    scratch[vx].color0 = c;
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
        // Depth of field (camera flag bit 6, 0x4547D0): globalaniminfo's filter with the camera's depthOfField. That
        // filter has its ColorDOF block off (+0x130E 0), so the original draws nothing; Enhanced turns it on (without
        // its scene matrix) to show the cameras' focus.
        render::ScreenFilter* screenFilter = nullptr;
        if (v.depthOfField && pose.dof && settings.dofFilter) {
            fx::FilterResource res = *settings.dofFilter;
            const auto d = cine::depthOfField(pose.focus, pose.fStop, pose.focalLength);
            res.blur = d.blur, res.dofNear = d.near, res.dofFar = d.far, res.dofCurve = d.curve;
            res.colorDof = true, res.matrix1 = false;
            const std::pair<const fx::FilterResource*, float> one{&res, 1.0f};
            fx::System().screenFilter(pose.position, pose.position, filter, nullptr, {}, std::span(&one, 1));
            if (filter.active()) screenFilter = &filter;
        }

        ui::Canvas canvas(w, hgt, v.art);
        drawSubtitles(canvas, ticker);
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
        if (v.art) fo.uiImages[0] = {v.art->rgba, v.art->width, v.art->height, v.art->version};
        const auto clips = unitClips(v.world, v.unitMeshes, lower(h.unitName), cam, hgt > 0 ? float(w) / float(hgt) : 1.0f);
        fo.unitClips = clips;
        fo.dynamicLights = posedLights;
        fo.screenFilter = screenFilter;
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
        v.mixer->setBusVolume(audio::Bus::Music, v.musicVolume), v.mixer->setBusVolume(audio::Bus::Sfx, v.sfxVolume);
        std::fprintf(stderr, "ou-game: cinematic %s ends: music / sfx buses back to %.1f / %.1f\n", name.c_str(), v.musicVolume, v.sfxVolume);
    }
    std::fprintf(stderr, "ou-game: cinematic %s: %d frames to frame %d%s\n", name.c_str(), r.frames, player.frame(),
                 r.skipped ? " (skipped)" : r.quit ? " (closed)" : "");
    return r;
}

std::vector<render::UnitClip> unitClips(const game::World& world, std::span<const std::pair<uint32_t, uint32_t>> unitMeshes,
                                        const std::string& cameraUnit, const render::Camera& camera, float aspect) {
    const auto& units = world.units();
    if (unitMeshes.size() != units.size()) return {};
    std::vector<render::PortalUnit> pu(units.size());
    size_t cam = units.size();
    for (size_t i = 0; i < units.size(); ++i) {
        pu[i].firstMesh = unitMeshes[i].first, pu[i].meshCount = unitMeshes[i].second;
        if (units[i].data.name == cameraUnit) cam = i;
        for (const auto& p : units[i].data.portals)
            for (size_t j = 0; j < units.size(); ++j) {
                if (units[j].data.name != p.toUnit) continue;
                render::PortalUnit::Link l{j, {}, glm::vec3(p.normal[0], p.normal[1], p.normal[2])};
                for (int k = 0; k < 4; ++k) l.quad[k] = glm::vec3(p.quad[k][0], p.quad[k][1], p.quad[k][2]) + units[i].offset;
                pu[i].links.push_back(l);
            }
    }
    return render::portalClips(pu, cam, camera, aspect);
}

} // namespace ou::app
