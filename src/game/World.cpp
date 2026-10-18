// World: collision + Lara + follow camera on a fixed tick; Lara's clip set; level spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

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

// Crossfade length (+0x19) and start frame (+0x1A), in 30 Hz frames, of the lara.drm anim request (object
// DTP +0x60 table, 0x24-byte records) that plays each clip; several requests per clip, the locomotion one
// is picked (docs/formats/ANIMATION.md "Transitions"). Unlisted clips: blend 3 (the table's most common
// value), start 0.
ClipBlend clipBlend(std::string_view from, std::string_view to) {
    struct Row { std::string_view name; int blend, start; };
    static constexpr Row kRows[] = {
        {"ng_idle", 4, 4}, {"ng_walk", 4, 1}, {"ng_run_start", 4, 6}, {"ng_run", 4, 4}, {"ng_running_stop", 4, 4},
        {"ng_standing_jump", 6, 6}, {"ng_running_jump_forwards", 2, 1}, {"NG_InAir_Feet_Forward_stage1_standing", 2, 2},
        {"NG_InAir_Feet_Forward_stage1", 5, 1}, {"NG_InAir_Feet_Forward_stage2", 3, 3}, {"NG_InAir_Feet_Forward_stage3", 5, 1},
        {"ng_generic_land_forwards", 3, 3}, {"ng_running_land", 2, 1}, {"ledgeb_attach_M", 4, 4}, {"ledgeb_idle", 8, 4},
        {"ledgeb_right", 3, 3}, {"ledgeb_climbup_fast", 6, 6}, {"swim_surfacedive", 4, 1}, {"swim_surface", 6, 12},
        {"swim_climbout", 12, 6}, {"swim_sub", 6, 8}, {"swim_sub_up", 6, 8}, {"swim_sub_down", 6, 8},
        {"swim_surfaceidle", 8, 6}, {"swim_jump_land", 6, 6}, {"swim_submerged_idle", 10, 10},
    };
    Row r{to, 3, 0};
    for (const auto& k : kRows)
        if (k.name == to) r = k;
    if (from == "ng_run_start" && to == "ng_run") r.blend = 2, r.start = 14; // request 730: ng_run entered mid-stride
    return {float(r.blend) / 30.0f, double(r.start) / 30.0};
}

namespace {

// Body capsules for hair collision (Lara's bind pose has no rotations, so offsets are model axes): spine
// 14 -> below the neck 16, and the head from 17 up. Radii [I]; HairSim shrinks them to clear the bind hair.
anim::HairParams laraHair() {
    anim::HairParams p;
    p.body = {{14, 16, {0, 0, -20}, 38.0f}, {17, 17, {0, 0, 40}, 30.0f}};
    // lara.drm cloth parameter set mode 0 (object DTP 0x27420 / 0x274BC, docs/formats/ANIMATION.md "Hair"):
    // ponytail (group 0) gravity 12 m/s^2, damping 0.1; front strands (1, 2) 12 m/s^2, damping 0.25; target
    // drive 0 (nothing pulls the hair back to its bind shape). m/s^2 -> units/s^2: * 100 * 3.1605. Damping
    // is per 30 Hz frame [I]; per 60 Hz tick: 1 - (1 - d)^0.5.
    constexpr float kG = 12.0f * 100.0f * 3.1605f;
    p.groups = {{kG, 1 - std::sqrt(0.9f), 0}, {kG, 1 - std::sqrt(0.75f), 0}, {kG, 1 - std::sqrt(0.75f), 0}};
    p.iterations = 16;
    p.follow = 0.8f; // [I] keeps the short strands from lagging the run/jump root motion by whole segment lengths
    return p;
}

// Per-bone matrix interpolation: lerp translation and column lengths, slerp rotation.
glm::mat4 mixBone(const glm::mat4& a, const glm::mat4& b, float t) {
    glm::vec3 sa, sb;
    glm::mat3 ra(a), rb(b);
    for (int c = 0; c < 3; ++c) {
        sa[c] = glm::length(ra[c]), sb[c] = glm::length(rb[c]);
        if (sa[c] > 0) ra[c] /= sa[c];
        if (sb[c] > 0) rb[c] /= sb[c];
    }
    glm::mat4 m(glm::mat3_cast(glm::slerp(glm::quat_cast(ra), glm::quat_cast(rb), t)));
    const glm::vec3 s = glm::mix(sa, sb, t);
    for (int c = 0; c < 3; ++c) m[c] *= s[c];
    m[3] = glm::mix(a[3], b[3], t);
    return m;
}
} // namespace

const std::vector<std::string_view> kLaraClipNames = {
    "ng_idle", "ng_walk", "ng_run_start", "ng_run", "ng_running_stop", "ng_turn_center",
    "ng_standing_jump", "ng_running_jump_forwards", "NG_InAir_Feet_Forward_stage1_standing",
    "NG_InAir_Feet_Forward_stage1", "NG_InAir_Feet_Forward_stage2", "NG_InAir_Feet_Forward_stage3",
    "ng_generic_land_forwards", "ng_running_land", "ledgeb_attach_M", "ledgeb_idle", "ledgeb_right",
    "ledgeb_climbup_fast", "swim_jump_land", "swim_surfaceidle", "swim_surface", "swim_surfaceidle_to_swim",
    "swim_surfacedive", "swim_submerged_idle", "swim_sub", "swim_sub_up", "swim_sub_down", "swim_climbout",
    "death_fall_normal", "death_toknee", // combat (Combat.cpp)
};

const std::vector<std::string_view> kPistolClipNames = {
    "combat_pistol_poseA", "combat_pistol_poseB", "combat_pistol_poseC", "combat_pistol_poseD", "combat_pistol_poseE",
    "combat_pistol_poseF", "combat_pistol_poseG", "combat_pistol_poseH", "combat_pistol_poseI", "combat_pistol_RfireE",
    "combat_pistol_LfireE", "combat_pistols_reload",
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
    const auto loadClips = [&](const cdc::Drm& d, const std::vector<std::string_view>& names, const char* file) {
        const auto list = anim::animList(d);
        for (const auto name : names) {
            const auto e = std::find_if(list.begin(), list.end(), [&](const anim::AnimEntry& x) { return x.name == name; });
            if (e == list.end()) throw std::runtime_error(std::string(file) + ": animation '" + std::string(name) + "' not in the list");
            const auto s = std::find_if(d.sections.begin(), d.sections.end(), [&](const cdc::Section& x) {
                return x.type == cdc::SectionType::Animation && x.id == e->animId;
            });
            if (s == d.sections.end()) throw std::runtime_error(std::string(file) + ": no section for '" + std::string(name) + "'");
            lib.clips.emplace(std::string(name), anim::decodeClip(s->data));
        }
    };
    loadClips(drm, kLaraClipNames, "lara.drm");
    // Lara's pistol clips live in the weapon's DRM (they animate her skeleton).
    auto gun = bigfile.read("pc-w\\wp_handgun.drm");
    if (!gun) throw std::runtime_error("pc-w\\wp_handgun.drm not in bigfile");
    loadClips(cdc::parseDrm(std::move(*gun)), kPistolClipNames, "wp_handgun.drm");
    if (auto w = loadWeapon(bigfile, "wp_handgun")) lib.pistols = *w;
    // Hands (bones 55, 80: the last segment of each arm chain before the fingers) in the hang pose.
    if (auto h = lib.handsAt(*lib.find("ledgeb_idle"), 0)) lib.hangHandOffset = *h;
    // Swim speed: mean root speed of the clips that start swimming (their loops have no root motion).
    float dist = 0;
    double secs = 0;
    for (const char* n : {"swim_surfaceidle_to_swim", "swim_surfacedive"}) {
        const anim::Clip& c = *lib.find(n);
        dist += glm::length(rootTranslation(c, c.durationSeconds(), false) - rootTranslation(c, 0, false));
        secs += c.durationSeconds();
    }
    lib.swimSpeed = dist / float(secs);
    lib.hair = anim::decodeHair(drm, lib.skeleton);
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
    startSpawn_ = spawn; // respawn target without units
    player_.spawn(spawn.pos, spawn.yaw);
    prev_ = player_.state();
    cam_.yaw = player_.state().yaw;
    updateCamera({}, 0);
    prevCam_ = cam_;
    updatePose(0, true);
}

void World::tick(const Actions& a) {
    prev_ = player_.state();
    prevCam_ = cam_;
    const float dt = float(kTickSeconds);
    // Stick is camera-relative.
    const vec2 fwd(std::cos(cam_.yaw), std::sin(cam_.yaw)), right(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    vec2 move = right * a.move.x + fwd * a.move.y;
    if (glm::length(move) > 1) move = glm::normalize(move);
    if (script_.holding()) move = vec2(0); // [I] gripping a push block: the stick moves the block (src/script)
    player_.tick(move, a, dt);
    if (!units_.empty()) { // streaming world (src/game/Stream.cpp)
        crossPortals(prev_.pos, player_.state().pos);
        touchCheckpoints();
    }
    if (!units_.empty() && scriptUnits_ != unitsVersion_) { // scripted objects follow the loaded units
        std::vector<script::UnitRef> refs;
        for (const auto& u : units_) refs.push_back({u.data.name, u.data.drm, u.offset});
        script_.setUnits(refs);
        enemies_.setUnits(refs);
        scriptUnits_ = unitsVersion_;
    }
    script_.tick({player_.state().pos, player_.state().yaw, a.move, a.interact}, &col_);
    if (const vec3 d = script_.carried(); d != vec3(0)) { // Lara follows the block she pushes/pulls
        auto s = player_.state();
        s.pos += d;
        player_.restore(s);
    }
    enemies_.tick(col_, {player_.state().pos, !health().dead(), player_.tuning().height},
                  [this](float damage, vec3 from) { damageLara(damage, from); }, dt);
    updateCamera(a, dt);
    tickCombat(a, dt); // after the camera: free aim follows it
    updatePose(dt, false);
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

void World::updatePose(float dt, bool reset) {
    const PlayerState& s = player_.state();
    reset = reset || bones_.empty();
    if (reset) {
        anim_ = anim::AnimController(anims_.skeleton);
        anim_.play(anims_.find(s.clip), 0, s.loop, s.clipRate, s.clipTime);
        hair_ = anim::HairSim(anims_.hair, anims_.skeleton, laraHair());
    } else {
        anim_.update(dt);
        // Player::play changed the clip (or restarted it): fade the new one in over what is showing.
        if (s.clip != prev_.clip || s.loop != prev_.loop || s.clipRate != prev_.clipRate || s.clipTime < prev_.clipTime)
            anim_.play(anims_.find(s.clip), clipBlend(prev_.clip, s.clip).blendSeconds, s.loop, s.clipRate, s.clipTime);
        else
            anim_.setTime(s.clipTime); // the player's clock is authoritative (root motion comes from it)
    }
    auto local = anim_.pose();
    aimLayer(local, reset ? 0 : dt);
    if (!local.empty()) local[0] = {{0, 0, 0}, {1, 1, 1}, glm::make_vec3(anims_.skeleton.bones[0].pivot)}; // root motion is in pos
    auto m = anim::worldMatrices(anims_.skeleton, local);
    const glm::mat4 toWorld = player_.modelMatrix(s.pos, s.yaw);
    if (reset) hair_.reset(m, toWorld);
    else hair_.step(m, toWorld, dt);
    hair_.apply(m, toWorld);
    prevBones_ = reset ? m : std::move(bones_);
    bones_ = std::move(m);
}

std::vector<glm::mat4> World::pose(double alpha) const {
    std::vector<glm::mat4> out(bones_.size());
    for (size_t i = 0; i < out.size(); ++i) out[i] = mixBone(prevBones_[i], bones_[i], float(alpha));
    return out;
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

// tru.exe: a water volume instance (object `watervolume`) gets a type-10 markup box (0x5D7130, MUD flag
// 0x100000); the player tests its water probe box against those (0x710A20). The volume's intro data holds
// the unique id of its water-surface intro at +0x12 (l4_pool: 5608 -> ca_goo_pool_watersurface 5607) [O].
// The box bounds come from a component we have not decoded [U]; we use the surface model's bounds (x/y,
// surface = top) and let the box reach 100 000 units down [I].
std::vector<collision::Markup> findWater(const cdc::BigfileSet& bigfile, const cdc::Drm& unit) {
    const auto u = formats::parseUnit(unit);
    const auto listBytes = bigfile.read("\\tr8\\pc-w\\objectlist.txt");
    if (!listBytes) return {};
    const auto names = formats::parseObjectList({reinterpret_cast<const char*>(listBytes->data()), listBytes->size()});
    std::vector<collision::Markup> out;
    for (const auto& vol : u.intros) {
        const auto n = names.find(vol.objectId);
        if (n == names.end() || n->second != "watervolume" || !vol.data) continue;
        const auto& bytes = unit.sections.at(vol.data->section).data;
        if (vol.data->offset + 0x14 > bytes.size()) continue;
        const uint16_t surfaceId = uint16_t(bytes[vol.data->offset + 0x12] | bytes[vol.data->offset + 0x13] << 8);
        const auto s = std::find_if(u.intros.begin(), u.intros.end(), [&](const formats::Intro& i) { return i.uniqueId == surfaceId; });
        if (s == u.intros.end() || !names.count(s->objectId)) continue;
        scene::SceneData model;
        try {
            model = formats::loadDrmScene(bigfile, "pc-w\\" + names.at(s->objectId) + ".drm");
        } catch (const std::exception&) {
            continue;
        }
        float im[16];
        formats::introMatrix(*s, im);
        const glm::mat4 intro = glm::make_mat4(im);
        vec3 lo(1e30f), hi(-1e30f);
        for (const auto& mesh : model.meshes) {
            const glm::mat4 m = intro * glm::make_mat4(mesh.transform);
            for (const auto& v : mesh.vertices) {
                const vec3 p(m * glm::vec4(v.position[0], v.position[1], v.position[2], 1));
                lo = glm::min(lo, p), hi = glm::max(hi, p);
            }
        }
        if (lo.x > hi.x) continue;
        out.push_back({10, uint16_t(vol.uniqueId), {{lo.x, lo.y, hi.z - 100000.0f}, hi}});
    }
    return out;
}

} // namespace ou::game
