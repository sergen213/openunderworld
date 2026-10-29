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
        // traversal (requests most often used for each clip)
        {"bbeam_walk", 5, 5}, {"bbeam_idle", 6, 6}, {"bbeam_walk_attach", 7, 1}, {"bbeam_jumpland", 5, 5},
        {"bbeam_walk_turn180", 5, 5}, {"drop_to_ledgeb", 3, 3}, {"ledgeb_jump_up", 8, 4}, {"ledgeb_jump_back", 5, 0},
        {"ledgeb_jump_right", 3, 1}, {"ledgeb_attach_U", 4, 4}, {"ledgeb_attach_S", 4, 4}, {"ledgeb_attach_F", 4, 4},
        {"HPole_idleHangPerp", 0, 0}, {"Hpole_perp_attach_M", 2, 4}, {"HPole_StartSwing", 4, 4}, {"HPole_swing", 1, 1},
        {"hpole_perp_fling", 1, 1}, {"crouch_Idle", 6, 6}, {"crouch_idleToCrouch", 4, 6}, {"Crouch_Front", 6, 3},
        {"ng_roll_front", 3, 1}, {"slope_slidefront", 6, 1},
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
    "bbeam_walk", "bbeam_idle", "bbeam_walk_attach", "bbeam_jumpland", "bbeam_walk_turn180", "drop_to_ledgeb",
    "ledgeb_jump_up", "ledgeb_jump_back", "ledgeb_jump_right", "ledgeb_attach_U", "ledgeb_attach_S", "ledgeb_attach_F",
    "HPole_idleHangPerp", "Hpole_perp_attach_M", "HPole_StartSwing", "HPole_swing", "Hpole_swingstopA", "hpole_perp_fling",
    "crouch_idleToCrouch", "crouch_Idle", "Crouch_Front", "ng_roll_front", "slope_slidefront",
    "death_fall_normal", "death_toknee", // combat (Combat.cpp)
    "hit_light_front", "hit_light_back", "hit_light_side", "hit_mediumfront", "hit_mediumback", "hit_mediumright",
    "hit_light_front_swim", "hit_light_back_swim", "hit_light_side_swim", "hit_mediumfront_swim", "hit_mediumback_swim",
    "hit_mediumright_swim",
    // free climbing (Climb.cpp): attach, idle, directional moves, jump off, drop
    "DICE_B_ground_attach", "DICE_B_air_attach_M", "DICE_B_Idle", "DICE_B_Settle_up", "DICE_B_Settle_down",
    "DICE_B_Settle_Left", "DICE_B_Settle_right", "DICE_B_Jump_Back", "DICE_B_Drop",
    // grapple (Grapple.cpp)
    "grapple_rappel_free_jumpon", "grapple_rappel_idle", "grapple_rappel_free_swing_forward",
    "grapple_rappel_free_swing_backward", "grapple_rappel_free_ascend", "grapple_rappel_free_descend",
    "grapple_rappel_free_jumpoff",
    // ladders / pillars / perches (Traverse.cpp)
    "lad_attach_up", "lad_climbup", "lad_climbdown", "lad_Idle", "lad_detach", "lad_detach_drop",
    "lad_ClimbOffTop", "lad_ClimbOffBot", "perch_attach_air", "perch_attach_ground", "perch_idle",
    "perch_jump_forward", "perch_detach_drop",
    "grapple_pull_up_light_begin", "grapple_pull_mid_light_begin", "grapple_pull_down_light_begin",
    "grapple_pull_up_heavy_begin", "grapple_pull_mid_heavy_begin", "grapple_pull_down_heavy_begin",
    "grapple_pull_up_light", "grapple_pull_mid_light", "grapple_pull_down_light",
    "grapple_pull_up_heavy", "grapple_pull_mid_heavy", "grapple_pull_down_heavy",
};

Tuning defaultTuning() { return {}; } // values and provenance: Tuning in Game.hpp, docs/GAMEPLAY.md "Numbers"

void rigOutfit(AnimLibrary& lib, const cdc::Drm& drm) {
    std::vector<std::string> warnings;
    auto skeletons = anim::decodeSkeletons(drm, warnings);
    if (skeletons.empty()) throw std::runtime_error("outfit DRM: no skeleton");
    // The render model (lara.drm 2366) has the full skeleton; the other dtp Model is an empty placeholder.
    lib.skeleton = std::move(*std::max_element(skeletons.begin(), skeletons.end(), [](const auto& a, const auto& b) {
        return a.bones.size() < b.bones.size();
    }));
    // Hands (bones 55, 80: the last segment of each arm chain before the fingers) in the hang pose.
    if (const auto* c = lib.find("ledgeb_idle"))
        if (auto h = lib.handsAt(*c, 0)) lib.hangHandOffset = *h;
    if (const auto* c = lib.find("HPole_idleHangPerp"))
        if (auto h = lib.handsAt(*c, 0)) lib.poleHandOffset = *h;
    lib.hair = anim::decodeHair(drm, lib.skeleton);
}

AnimLibrary loadLaraAnims(const cdc::BigfileSet& bigfile, const std::string& outfit) {
    const std::string path = "pc-w\\" + outfit + ".drm";
    auto bytes = bigfile.read(path);
    if (!bytes) throw std::runtime_error(path + " not in bigfile");
    const cdc::Drm drm = cdc::parseDrm(std::move(*bytes));
    AnimLibrary lib;
    const auto loadClips = [&](const cdc::Drm& d, const std::vector<std::string_view>& names, const std::string& file,
                               const std::string& prefix) {
        const auto list = anim::animList(d);
        for (const auto name : names) {
            const auto e = std::find_if(list.begin(), list.end(), [&](const anim::AnimEntry& x) { return x.name == name; });
            if (e == list.end()) throw std::runtime_error(file + ": animation '" + std::string(name) + "' not in the list");
            const auto s = std::find_if(d.sections.begin(), d.sections.end(), [&](const cdc::Section& x) {
                return x.type == cdc::SectionType::Animation && x.id == e->animId;
            });
            if (s == d.sections.end()) throw std::runtime_error(file + ": no section for '" + std::string(name) + "'");
            lib.clips.emplace(prefix + std::string(name), anim::decodeClip(s->data));
        }
    };
    loadClips(drm, kLaraClipNames, path, "");
    // Lara's weapon clips live in each weapon's DRM (they animate her skeleton); stats from its data.
    for (int i = 0; i < kWeaponCount; ++i) {
        const std::string object(weaponObject(Weapon(i))), file = "pc-w\\" + object + ".drm";
        auto gun = bigfile.read(file);
        if (!gun) throw std::runtime_error(file + " not in bigfile");
        loadClips(cdc::parseDrm(std::move(*gun)), weaponClipNames(Weapon(i)), file, object + ":");
        if (auto w = loadWeapon(bigfile, object)) lib.weapons[size_t(i)] = *w;
    }
    // The motorcycle: Lara's riding clips and the bike's tuning (Vehicle.cpp).
    if (auto bike = bigfile.read("pc-w\\motorcycle.drm")) {
        const cdc::Drm bd = cdc::parseDrm(std::move(*bike));
        loadClips(bd, kBikeClipNames, "pc-w\\motorcycle.drm", "motorcycle:");
        lib.bike = loadBikeTuning(bd);
    }
    rigOutfit(lib, drm);
    // Swim speed: mean root speed of the clips that start swimming (their loops have no root motion).
    float dist = 0;
    double secs = 0;
    for (const char* n : {"swim_surfaceidle_to_swim", "swim_surfacedive"}) {
        const anim::Clip& c = *lib.find(n);
        dist += glm::length(rootTranslation(c, c.durationSeconds(), false) - rootTranslation(c, 0, false));
        secs += c.durationSeconds();
    }
    lib.swimSpeed = dist / float(secs);
    return lib;
}

void World::setOutfitRig(const cdc::Drm& drm) {
    rigOutfit(anims_, drm); // in place: Player and AnimController keep referring to anims_
    updatePose(0, true);
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
    bindMultibody();
    bindUber();
    bindTerrain();
    bindLightHealth();
    bindMarkup();
    player_.spawn(spawn.pos, spawn.yaw);
    prev_ = player_.state();
    cam_.yaw = player_.state().yaw;
    updateCamera({}, 0);
    prevCam_ = cam_;
    updatePose(0, true);
    bindEnemies();
}

std::vector<script::UnitRef> World::unitRefs() const {
    std::vector<script::UnitRef> refs;
    for (const auto& u : units_) refs.push_back({u.data.name, u.data.drm, u.offset});
    refs.insert(refs.end(), cinematicRefs_.begin(), cinematicRefs_.end());
    return refs;
}

void World::restoreEnemies(const nlohmann::json& j) {
    enemies_.restore(j);
    if (!units_.empty()) enemies_.setUnits(unitRefs());
}

void World::tick(const Actions& input) {
    prev_ = player_.state();
    prevCam_ = cam_;
    const float dt = float(kTickSeconds);
    // On the motorcycle the bike drives Lara (Vehicle.cpp); weapons, use and the grapple wait until she is off it.
    const bool onBike = tickVehicles(input, dt);
    Actions a = input;
    if (onBike) a.move = vec2(0), a.jump = a.drop = a.interact = a.draw = a.fire = a.aim = a.switchTarget = a.weapon = a.grapple = false;
    // Stick is camera-relative.
    const vec2 fwd(std::cos(cam_.yaw), std::sin(cam_.yaw)), right(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    vec2 move = right * a.move.x + fwd * a.move.y;
    if (glm::length(move) > 1) move = glm::normalize(move);
    if (script_.holding()) move = vec2(0); // [I] gripping a push block: the stick moves the block (src/script)
    if (combat_.slowMo > 0) move = vec2(0); // [I] adrenaline window: the stick steers the pip (Combat.cpp)
    if (a.grapple && !grappleHeld_) {
        if (player_.state().grappleUid) releaseGrapple(false);
        else if (const auto t = grappleTarget()) attachGrapple(t->pos, t->uid, t->kind);
    }
    grappleHeld_ = a.grapple;
    if (!onBike) player_.tick(move, a, dt);
    if (player_.state().state == State::GrapplePull && a.interact)
        if (auto* target = const_cast<script::Object*>(script_.find(player_.state().grappleUid));
            target && target->kind == script::Kind::Block) {
            vec3 delta = player_.state().pos - (target->pos + target->offset);
            delta.z = 0;
            if (const float distance = glm::length(delta); distance > 1.0f)
                target->offset += delta * std::min(240.0f * float(dt), distance) / distance; // [I] braced pull speed
        }
    if (!units_.empty()) { // streaming world (src/game/Stream.cpp)
        crossPortals(prev_.pos, player_.state().pos);
        touchCheckpoints();
    }
    if (!units_.empty() && scriptUnits_ != unitsVersion_) { // scripted objects follow the loaded units
        const auto refs = unitRefs();
        script_.setUnits(refs);
        enemies_.setUnits(refs);
        scriptUnits_ = unitsVersion_;
    }
    script_.tick({player_.state().pos, player_.state().yaw, a.move, a.interact}, &col_);
    collectPickups(a.interact && !interactHeld_);
    tickObjects(dt); // object collision + physics doors (Objects.cpp), before enemies and the camera query them
    interactHeld_ = a.interact;
    if (const vec3 d = script_.carried(); d != vec3(0)) { // Lara follows the block she pushes/pulls
        auto s = player_.state();
        s.pos += d;
        player_.restore(s);
    }
    enemies_.tick(col_, {player_.state().pos, !health().dead(), player_.tuning().height},
                  [this](float damage, vec3 from, int type) { damageLara(damage, from, type); }, dt * combat_.timeScale);
    updateCamera(a, dt);
    tickCombat(a, dt); // after the camera: free aim follows it
    tickAir(dt);
    water_.tick(*this, dt); // after combat: this tick's shots make rings
    updatePose(dt, false);
    ++ticks_;
    if (!units_.empty()) checkLevelEnd(); // last: a chapter change re-spawns Lara (enterUnit)
}

// Orbit camera around a point above Lara's root; pulled in along the view ray when level geometry is
// in the way. Distance/height/pitch limits are inferred (docs/GAMEPLAY.md).
void World::updateCamera(const Actions& a, float dt) {
    constexpr float kTargetHeight = 450.0f, kProbeRadius = 20.0f;
    const Bike* bike = ridden();
    const float kDistance = bike ? 950.0f : 700.0f; // [I] VehicleCamera pulls back on the bike
    cam_.yaw += a.look.x * dt;
    // [I] riding without looking: swing behind the bike, faster the faster it goes
    if (bike && bike->phase == BikePhase::Ride && a.look.x == 0 && bike->speed > 0)
        cam_.yaw = lerpAngle(cam_.yaw, bike->yaw, std::min(1.0f, dt * 3.0f * std::min(1.0f, bike->speed / 1500.0f)));
    cam_.pitch = std::clamp(cam_.pitch + a.look.y * dt, -1.2f, 0.7f);
    cam_.target = player_.state().pos + vec3(0, 0, kTargetHeight);
    const vec3 dir(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
    const vec3 back = -dir * kDistance;
    float f = 1;
    if (auto h = col_.sweepSphere(cam_.target, kProbeRadius, back)) f = std::max(0.0f, h->t - 2.0f / kDistance);
    cam_.position = cam_.target + back * f;
}

// Oxygen: the head is under water in the submerged swim states (root + 150 = head height [I]).
// At 0 air Lara takes max/6 health per second until she surfaces or dies [I rates].
void World::tickAir(float dt) {
    const PlayerState& ps = player_.state();
    submerged_ = false;
    if (ps.water >= 0 && size_t(ps.water) < col_.markup.size()) {
        const float surf = col_.markup[size_t(ps.water)].points[1].z;
        switch (ps.state) {
        case State::SwimUnder:
        case State::SwimDive:
        case State::Surfacing:
        case State::SwimSplash: submerged_ = ps.pos.z + 150.0f < surf; break;
        default: break;
        }
    }
    if (health_.dead()) { air_ = 1.0f; return; }
    if (submerged_) {
        air_ = std::max(0.0f, air_ - dt / kAirSeconds);
        if (air_ <= 0.0f) damageLara(health_.max() / 6.0f * dt, ps.pos, kDamageBypass);
    } else {
        air_ = std::min(1.0f, air_ + dt / kAirRefillSeconds);
    }
}

// Grapple target: within 5000 units and a 60-degree cone of the camera, with clear line of sight
// from Lara's eye (range and cone are ours [I]; the exe's state-chart conditions do not decode to a
// single range check). Units contribute their intros in world frame.
std::optional<GrappleTarget> World::grappleTarget() const {
    if (player_.state().grappleUid) return std::nullopt;
    const vec3 eye = player_.state().pos + vec3(0, 0, targeting.eyeHeight);
    const vec3 fwd(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw),
                   std::sin(cam_.pitch));
    std::optional<GrappleTarget> best;
    float bestScore = 1e30f;
    for (const auto& u : units_)
        for (const auto& g : u.data.grapples) {
            GrappleTarget t = g;
            t.pos += u.offset;
            const vec3 d = t.pos - eye;
            const float dist = glm::length(d);
            if (dist < 300.0f || dist > 5000.0f) continue;
            const float c = glm::dot(d / dist, fwd);
            if (c < 0.5f) continue;
            if (col_.raycast(eye, d / dist, dist - 60.0f)) continue; // something blocks the hook
            const float score = (1.0f - c) * 2000.0f + dist * 0.2f;
            if (score < bestScore) bestScore = score, best = t;
        }
    return best;
}

PlayerState World::interpolated(double alpha) const {
    const float t = float(alpha);
    PlayerState s = player_.state();
    s.pos = glm::mix(prev_.pos, s.pos, t);
    s.yaw = lerpAngle(prev_.yaw, s.yaw, t);
    s.pitch = glm::mix(prev_.pitch, s.pitch, t);
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
    const glm::mat4 toWorld = player_.modelMatrix(s.pos, s.yaw, s.pitch);
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
