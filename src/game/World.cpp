// World: collision + Lara + follow camera on a fixed tick; Lara's clip set; level spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
float lerpAngle(float a, float b, float t) {
    float d = std::fmod(b - a + kPi, 2 * kPi);
    if (d < 0) d += 2 * kPi;
    return a + (d - kPi) * t;
}
} // namespace

const std::vector<std::string_view> kLaraClipNames = {
    "ng_idle", "ng_walk", "ng_run_start", "ng_run", "ng_running_stop", "ng_turn_center",
    "ng_standing_jump", "ng_running_jump_forwards", "NG_InAir_Feet_Forward_stage1_standing",
    "NG_InAir_Feet_Forward_stage1", "NG_InAir_Feet_Forward_stage2", "NG_InAir_Feet_Forward_stage3",
    "ng_generic_land_forwards", "ng_running_land", "ledgeb_attach_M", "ledgeb_idle", "ledgeb_right",
    "ledgeb_climbup_fast",
};

Tuning defaultTuning() { return {}; } // values and provenance: Tuning in Game.hpp, docs/GAMEPLAY.md "Numbers"

AnimLibrary loadLaraAnims(const cdc::BigfileSet& bigfile) {
    auto bytes = bigfile.read("pc-w\\lara.drm");
    if (!bytes) throw std::runtime_error("pc-w\\lara.drm not in bigfile");
    const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
    std::vector<std::string> warnings;
    auto skeletons = anim::decodeSkeletons(drm, warnings);
    if (skeletons.empty()) throw std::runtime_error("pc-w\\lara.drm: no skeleton");
    AnimLibrary lib;
    // Lara's render model (2366) has the full skeleton; the other dtp Model is an empty placeholder.
    lib.skeleton = std::move(*std::max_element(skeletons.begin(), skeletons.end(), [](const auto& a, const auto& b) {
        return a.bones.size() < b.bones.size();
    }));
    const auto list = anim::animList(drm);
    for (const auto name : kLaraClipNames) {
        const auto e = std::find_if(list.begin(), list.end(), [&](const anim::AnimEntry& x) { return x.name == name; });
        if (e == list.end()) throw std::runtime_error("lara.drm: animation '" + std::string(name) + "' not in the list");
        const auto s = std::find_if(drm.sections.begin(), drm.sections.end(), [&](const cdc::Section& x) {
            return x.type == cdc::SectionType::Animation && x.id == e->animId;
        });
        if (s == drm.sections.end()) throw std::runtime_error("lara.drm: no section for '" + std::string(name) + "'");
        lib.clips.emplace(std::string(name), anim::decodeClip(s->data));
    }
    // Hands (bones 55, 80: the last segment of each arm chain before the fingers) in the hang pose.
    if (lib.skeleton.bones.size() > 80) {
        const auto w = anim::worldMatrices(lib.skeleton, anim::sample(*lib.find("ledgeb_idle"), lib.skeleton, 0, true));
        lib.hangHandOffset = (vec3(w[55][3]) + vec3(w[80][3])) * 0.5f;
    }
    return lib;
}

World::World(collision::CollisionMesh col, AnimLibrary anims, Spawn spawn, Tuning tuning)
    : col_(std::move(col)), anims_(std::move(anims)), player_(col_, anims_, [&] {
          // Run take-off speed = ng_run's root-motion speed when the clip is present.
          if (const anim::Clip* run = anims_.find("ng_run")) {
              const double d = run->durationSeconds();
              tuning.runSpeed = glm::length(rootTranslation(*run, d, true) - rootTranslation(*run, 0, true)) / float(d);
          }
          return tuning;
      }()) {
    player_.spawn(spawn.pos, spawn.yaw);
    prev_ = player_.state();
    cam_.yaw = player_.state().yaw;
    updateCamera({}, 0);
    prevCam_ = cam_;
}

void World::tick(const Actions& a) {
    prev_ = player_.state();
    prevCam_ = cam_;
    const float dt = float(kTickSeconds);
    // Stick is camera-relative.
    const vec2 fwd(std::cos(cam_.yaw), std::sin(cam_.yaw)), right(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    vec2 move = right * a.move.x + fwd * a.move.y;
    if (glm::length(move) > 1) move = glm::normalize(move);
    player_.tick(move, a, dt);
    updateCamera(a, dt);
    ++ticks_;
}

// Orbit camera around a point above Lara's root; pulled in along the view ray when level geometry is
// in the way. Distance/height/pitch limits are inferred (docs/GAMEPLAY.md).
void World::updateCamera(const Actions& a, float dt) {
    constexpr float kTargetHeight = 450.0f, kDistance = 700.0f, kProbeRadius = 20.0f;
    cam_.yaw += a.look.x * dt;
    cam_.pitch = std::clamp(cam_.pitch + a.look.y * dt, -1.2f, 0.7f);
    cam_.target = player_.state().pos + vec3(0, 0, kTargetHeight);
    const vec3 dir(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
    const vec3 back = -dir * kDistance;
    float f = 1;
    if (auto h = col_.sweepSphere(cam_.target, kProbeRadius, back)) f = std::max(0.0f, h->t - 2.0f / kDistance);
    cam_.position = cam_.target + back * f;
}

PlayerState World::interpolated(double alpha) const {
    const float t = float(alpha);
    PlayerState s = player_.state();
    s.pos = glm::mix(prev_.pos, s.pos, t);
    s.yaw = lerpAngle(prev_.yaw, s.yaw, t);
    if (prev_.clip == s.clip) s.clipTime = prev_.clipTime + (s.clipTime - prev_.clipTime) * alpha;
    return s;
}

CameraState World::interpolatedCamera(double alpha) const {
    const float t = float(alpha);
    CameraState c = cam_;
    c.position = glm::mix(prevCam_.position, cam_.position, t);
    c.target = glm::mix(prevCam_.target, cam_.target, t);
    c.yaw = lerpAngle(prevCam_.yaw, cam_.yaw, t);
    c.pitch = glm::mix(prevCam_.pitch, cam_.pitch, t);
    return c;
}

std::optional<Spawn> findSpawn(const cdc::BigfileSet& bigfile, const cdc::Drm& unit) {
    const auto u = formats::parseUnit(unit);
    const auto listBytes = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    const auto names = listBytes ? formats::parseObjectList({reinterpret_cast<const char*>(listBytes->data()), listBytes->size()})
                                 : std::map<uint16_t, std::string>{};
    const formats::Intro* pick = nullptr;
    for (const auto& in : u.intros) {
        const auto n = names.find(in.objectId);
        if (n != names.end() && n->second == "player") { pick = &in; break; }
    }
    if (!pick && !u.intros.empty()) pick = &u.intros.front();
    if (!pick) return std::nullopt;
    // Intro rotation z is the model yaw; model -Y is forward, so heading = rot.z - 90 degrees.
    return Spawn{{pick->position[0], pick->position[1], pick->position[2]}, pick->rotation[2] - kPi / 2,
                 names.count(pick->objectId) && names.at(pick->objectId) == "player"};
}

} // namespace ou::game
