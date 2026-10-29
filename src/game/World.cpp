// World: collision + Lara + follow camera on a fixed tick; Lara's clip set; level spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"
#include "script/Vm.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
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
    "ledgeb_bad_attach_M", "ledgeb_bad_attach_recover", // saving grab (Player::tryGrab)
    "death_fall_normal", "death_toknee", // combat (Combat.cpp)
    "hit_light_front", "hit_light_back", "hit_light_side", "hit_mediumfront", "hit_mediumback", "hit_mediumright",
    "hit_light_front_swim", "hit_light_back_swim", "hit_light_side_swim", "hit_mediumfront_swim", "hit_mediumback_swim",
    "hit_mediumright_swim",
    // free climbing (Climb.cpp), one set per hand stance B / L / LW / R / RW (the hold's markup type 16-20): attach,
    // idle, directional moves, jump off, drop; then every hold-to-hold transition DICE_<from>_to_<to>_<dir> as lara.drm
    // spells it (stance capitalisation varies: Lw/LW, Rw/RW; Climb.cpp looks them up case-insensitively)
    "DICE_B_ground_attach", "DICE_B_air_attach_M", "DICE_B_Idle", "DICE_B_Settle_up", "DICE_B_Settle_down",
    "DICE_B_Settle_Left", "DICE_B_Settle_right", "DICE_B_Jump_Back", "DICE_B_Drop",
    "DICE_L_air_attach_mid", "DICE_L_drop", "DICE_L_ground_attach", "DICE_L_Idle", "DICE_L_jump_back",
    "DICE_L_settle_down", "DICE_L_settle_left", "DICE_L_settle_right", "DICE_L_settle_up", "DICE_LW_air_attach_mid",
    "DICE_LW_drop", "DICE_LW_ground_attach", "DICE_LW_Idle", "DICE_LW_jump_back", "DICE_LW_settle_down",
    "DICE_LW_settle_left", "DICE_LW_settle_right", "DICE_LW_settle_up", "DICE_R_air_attach_mid", "DICE_R_drop",
    "DICE_R_ground_attach", "DICE_R_Idle", "DICE_R_jump_back", "DICE_R_settle_down", "DICE_R_settle_left",
    "DICE_R_settle_right", "DICE_R_settle_up", "DICE_Rw_air_attach_mid", "DICE_Rw_drop", "DICE_Rw_ground_attach",
    "DICE_RW_Idle", "DICE_Rw_Jump_Back", "DICE_RW_Settle_down", "DICE_Rw_Settle_Left", "DICE_Rw_Settle_right",
    "DICE_Rw_Settle_up",
    "DICE_B_to_B_DownDiagR", "DICE_B_to_B_DownR", "DICE_B_to_B_Left", "DICE_B_to_B_Right", "DICE_B_to_B_Up",
    "DICE_B_to_B_UpDiagL", "DICE_B_to_B_UpDiagR", "DICE_B_to_L_DownDiagL", "DICE_B_to_L_DownDiagR",
    "DICE_B_to_L_DownR", "DICE_B_to_L_Right", "DICE_B_to_L_Up", "DICE_B_to_L_UpDiagL", "DICE_B_to_Lw_DownDiagL",
    "DICE_B_to_Lw_DownDiagR", "DICE_B_to_Lw_DownL", "DICE_B_to_Lw_DownR", "DICE_B_to_Lw_Left", "DICE_B_to_Lw_Right",
    "DICE_B_to_Lw_UpDiagL", "DICE_B_to_Lw_UpDiagR", "DICE_B_to_Lw_UpL", "DICE_B_to_Lw_UpR", "DICE_B_to_R_DownDiagL",
    "DICE_B_to_R_DownDiagR", "DICE_B_to_R_DownR", "DICE_B_to_R_Left", "DICE_B_to_R_Right", "DICE_B_to_R_UpDiagL",
    "DICE_B_to_R_UpDiagR", "DICE_B_to_Rw_DownL", "DICE_B_to_Rw_DownR", "DICE_B_to_Rw_Left", "DICE_B_to_Rw_Right",
    "DICE_B_to_Rw_UpL", "DICE_B_to_Rw_UpR", "DICE_L_to_B_DownDiagR", "DICE_L_to_B_DownR", "DICE_L_to_B_Left",
    "DICE_L_to_B_Right", "DICE_L_to_B_Up", "DICE_L_to_B_UpDiagL", "DICE_L_to_B_UpDiagR", "DICE_L_to_L_DownDiagL",
    "DICE_L_to_L_DownDiagR", "DICE_L_to_L_DownR", "DICE_L_to_L_Left", "DICE_L_to_L_Right", "DICE_L_to_L_Up",
    "DICE_L_to_L_UpDiagL", "DICE_L_to_L_UpDiagR", "DICE_L_to_LW_DownDiagR", "DICE_L_to_LW_DownL",
    "DICE_L_to_LW_DownR", "DICE_L_to_LW_Right", "DICE_L_to_LW_UpDiagL", "DICE_L_to_LW_UpDiagR", "DICE_L_to_LW_UpL",
    "DICE_L_to_LW_UpR", "DICE_L_to_R_DownDiagL", "DICE_L_to_R_DownDiagR", "DICE_L_to_R_DownR", "DICE_L_to_R_Left",
    "DICE_L_to_R_Right", "DICE_L_to_R_UpDiagL", "DICE_L_to_R_UpDiagR", "DICE_L_to_R_UpL", "DICE_L_to_Rw_DownDiagL",
    "DICE_L_to_Rw_DownDiagR", "DICE_L_to_Rw_DownL", "DICE_L_to_Rw_DownR", "DICE_L_to_Rw_Left", "DICE_L_to_Rw_Right",
    "DICE_L_to_Rw_UpDiagL", "DICE_L_to_Rw_UpDiagR", "DICE_L_to_Rw_UpL", "DICE_L_to_Rw_UpR", "DICE_Lw_to_B_DownDiagL",
    "DICE_Lw_to_B_DownDiagR", "DICE_Lw_to_B_DownL", "DICE_Lw_to_B_DownR", "DICE_Lw_to_B_Left", "DICE_Lw_to_B_Right",
    "DICE_Lw_to_B_UpDiagL", "DICE_Lw_to_B_UpDiagR", "DICE_Lw_to_B_UpL", "DICE_Lw_to_B_UpR", "DICE_LW_to_L_DownDiagL",
    "DICE_LW_to_L_DownDiagR", "DICE_LW_to_L_DownL", "DICE_LW_to_L_DownR", "DICE_LW_to_L_Left", "DICE_LW_to_L_Right",
    "DICE_LW_to_L_UpDiagL", "DICE_LW_to_L_UpDiagR", "DICE_LW_to_L_UpL", "DICE_LW_to_L_UpR",
    "DICE_LW_to_LW_DownDiagL", "DICE_LW_to_LW_DownR", "DICE_LW_to_LW_UpDiagR", "DICE_LW_to_LW_UpL",
    "DICE_LW_to_R_DownDiagL", "DICE_LW_to_R_DownDiagR", "DICE_LW_to_R_DownR", "DICE_LW_to_R_Left",
    "DICE_LW_to_R_Right", "DICE_LW_to_R_UpDiagL", "DICE_LW_to_R_UpL", "DICE_LW_to_R_UpR", "DICE_Lw_to_Rw_DownDiagL",
    "DICE_Lw_to_Rw_DownR", "DICE_Lw_to_Rw_Left", "DICE_Lw_to_Rw_UpDiagL", "DICE_Lw_to_Rw_UpDiagR",
    "DICE_Lw_to_Rw_UpL", "DICE_R_to_B_DownDiagL", "DICE_R_to_B_DownDiagR", "DICE_R_to_B_Left", "DICE_R_to_B_Right",
    "DICE_R_to_B_Up", "DICE_R_to_B_UpDiagL", "DICE_R_to_B_UpDiagR", "DICE_R_to_L_Down", "DICE_R_to_L_DownDiagL",
    "DICE_R_to_L_DownDiagR", "DICE_R_to_L_Left", "DICE_R_to_L_Right", "DICE_R_to_L_Up", "DICE_R_to_L_UpDiagL",
    "DICE_R_to_L_UpDiagR", "DICE_R_to_LW_DownDiagR", "DICE_R_to_LW_DownL", "DICE_R_to_LW_DownR", "DICE_R_to_LW_Left",
    "DICE_R_to_LW_Right", "DICE_R_to_LW_UpDiagL", "DICE_R_to_LW_UpL", "DICE_R_to_LW_UpR", "DICE_R_to_R_DownDiagL",
    "DICE_R_to_R_DownDiagR", "DICE_R_to_R_DownR", "DICE_R_to_R_Left", "DICE_R_to_R_Right", "DICE_R_to_R_UpDiagL",
    "DICE_R_to_R_UpDiagR", "DICE_R_to_R_UpL", "DICE_R_to_Rw_DownL", "DICE_R_to_Rw_DownR", "DICE_R_to_Rw_Left",
    "DICE_R_to_Rw_Right", "DICE_R_to_Rw_UpDiagR", "DICE_R_to_Rw_UpL", "DICE_R_to_Rw_UpR", "DICE_Rw_to_B_DownDiagR",
    "DICE_Rw_to_B_DownL", "DICE_Rw_to_B_DownR", "DICE_Rw_to_B_Left", "DICE_Rw_to_B_Right", "DICE_Rw_to_B_UpDiagL",
    "DICE_Rw_to_B_UpL", "DICE_Rw_to_B_UpR", "DICE_Rw_to_L_DownDiagL", "DICE_Rw_to_L_DownL", "DICE_Rw_to_L_UpR",
    "DICE_Rw_to_Lw_Down", "DICE_Rw_to_Lw_DownDiagL", "DICE_Rw_to_Lw_Right", "DICE_Rw_to_Lw_UpDiagR",
    "DICE_Rw_to_Lw_UpL", "DICE_Rw_to_R_DownDiagL", "DICE_Rw_to_R_DownDiagR", "DICE_Rw_to_R_DownL",
    "DICE_Rw_to_R_DownR", "DICE_Rw_to_R_Left", "DICE_Rw_to_R_Right", "DICE_Rw_to_R_UpDiagL", "DICE_Rw_to_R_UpDiagR",
    "DICE_Rw_to_R_UpL", "DICE_Rw_to_R_UpR", "DICE_Rw_to_Rw_DownL", "DICE_Rw_to_Rw_UpR",
    // grapple (Grapple.cpp)
    "grapple_rappel_free_jumpon", "grapple_rappel_idle", "grapple_rappel_free_swing_forward",
    "grapple_rappel_free_swing_backward", "grapple_rappel_free_ascend", "grapple_rappel_free_descend",
    "grapple_rappel_free_jumpoff",
    "grapple_rappel_wall_land", "grapple_rappel_wall_land_left", "grapple_rappel_wall_land_right", "grapple_rappel_up",
    "grapple_rappel_up_left_45", "grapple_rappel_up_right_45", "grapple_rappel_down", "grapple_rappel_down_left_45",
    "grapple_rappel_down_right_45", "ng_ground_to_wallrun_left", "ng_ground_to_wallrun_right", "grapple_rappel_left",
    "grapple_rappel_right", "grapple_rappel_walk_left", "grapple_rappel_walk_right",
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
    for (const auto& m : physics::loadObjectPhysics(drm).hmarkers) { // weapon mounts (WeaponDef::hand / stow)
        const glm::quat q = glm::angleAxis(m.rot.x, vec3(1, 0, 0)) * glm::angleAxis(m.rot.y, vec3(0, 1, 0)) *
                            glm::angleAxis(m.rot.z, vec3(0, 0, 1));
        glm::mat4 frame = glm::mat4_cast(q);
        frame[3] = glm::vec4(m.pos, 1);
        lib.markers.emplace(m.id, std::pair{m.segment, frame});
    }
    // Sticky grenades: the throw clip lives in g_grenade_thrower.drm (it animates Lara); tuning from its + pr_grenade's data.
    if (auto g = bigfile.read("pc-w\\g_grenade_thrower.drm")) {
        loadClips(cdc::parseDrm(std::move(*g)), {"combat_grenade_Throw1"}, "pc-w\\g_grenade_thrower.drm", "g_grenade_thrower:");
        if (auto t = loadGrenadeTuning(bigfile)) lib.grenade = *t;
    }
    if (auto d = loadDifficulty(bigfile)) lib.difficulty = *d;
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

// Wetsuit dive gear [O] (tru.exe under Wine): the player's hidden-group mask (+0x23C | +0x240) is 0xE03 | 2 on
// l1_seatop's boat at the chapter start (groups 1 face mask + regulator and 2 hidden; 3, the harness, drawn) and
// 0xE02 | 2 swimming in l7_arctic_sea (only 2 hidden). [I] swimming is what puts the mask on.
uint32_t World::laraHiddenGroups() const {
    const uint32_t m = script_.playerHiddenGroups();
    if (!outfit.starts_with("lara_wetsuit")) return m;
    return (m & ~3u) | (player_.state().water >= 0 ? 2u : 3u);
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
    grenades = maxGrenades(); // [I] Lara carries the maximum (no grenade pickups in any unit)
    bindMultibody();
    bindUber();
    bindTerrain();
    bindLightHealth();
    bindMarkup();
    bindEnemies();
    setTailoring(tailoring_);
    script_.setCameraNative([this](int op, const std::string& unit, uint32_t index, uint32_t value) {
        return cameraNative(op, unit, index, value);
    });
    player_.spawn(spawn.pos, spawn.yaw);
    prev_ = player_.state();
    cam_.yaw = player_.state().yaw;
    updateCamera({}, 0);
    prevCam_ = cam_;
    updatePose(0, true);
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
    if (splineCam_ && !script_.playerControl()) move = vec2(0); // reveal camera with control disabled (larabase 56)
    if (a.grapple && !grappleHeld_) {
        if (player_.state().grappleUid) releaseGrapple(false);
        else if (const auto t = grappleTarget()) attachGrapple(t->pos, t->uid, t->kind);
    }
    grappleHeld_ = a.grapple;
    // Freshly loaded units (a chapter start) get their scripted objects and colliders before Lara moves, so she
    // starts on l2_cliffclimb's yacht deck rather than in the sea under it.
    if (!units_.empty() && scriptUnits_ != unitsVersion_) {
        const auto refs = unitRefs();
        script_.setUnits(refs);
        enemies_.setUnits(refs);
        scriptUnits_ = unitsVersion_;
    }
    if (script_.objects().size() != syncedObjects_ || col_.dynamics.size() != colliders_.size()) syncObjects();
    if (!onBike) player_.tick(move, a, dt);
    if (player_.state().state != State::GrapplePull) pullT_ = 0;
    else if (a.interact)
        if (auto* target = const_cast<script::Object*>(script_.find(player_.state().grappleUid)); target) {
            if (target->kind == script::Kind::Block) {
                vec3 delta = player_.state().pos - (target->pos + target->offset);
                delta.z = 0;
                if (const float distance = glm::length(delta); distance > 1.0f)
                    target->offset += delta * std::min(240.0f * float(dt), distance) / distance; // [I] braced pull speed
            } else if ((pullT_ += dt) >= 1.0f) { // [I] a one-second heave runs the target's pull handler, then lets go
                if (script_.grapplePulled(target->uid)) lastPulled_ = target->uid;
                player_.releaseGrapple(false);
            }
        }
    if (!units_.empty()) { // streaming world (src/game/Stream.cpp)
        crossPortals(prev_.pos, player_.state().pos);
        touchCheckpoints();
        recordReveal();
    }
    if (!units_.empty() && scriptUnits_ != unitsVersion_) { // scripted objects follow the loaded units
        const auto refs = unitRefs();
        script_.setUnits(refs);
        enemies_.setUnits(refs);
        scriptUnits_ = unitsVersion_;
    }
    script_.tick({player_.state().pos, player_.state().yaw, a.move, a.interact}, &col_);
    // The unit scripts' player-unit callbacks, after the script tick so that their events count for this tick.
    if (!units_.empty() && units_[current_].data.name != playerUnit_) {
        const auto streamId = [&](const std::string& name) -> uint32_t {
            for (const auto& u : units_)
                for (const auto& p : u.data.portals)
                    if (p.toUnit == name) return uint32_t(uint16_t(p.toStreamId));
            return 0;
        };
        const std::string& now = units_[current_].data.name;
        script_.setPlayerUnit(streamId(now), playerUnit_.empty() ? std::nullopt : std::optional<uint32_t>(streamId(playerUnit_)));
        playerUnit_ = now;
    }
    if (const auto to = script_.takeTeleport(); to && !units_.empty()) // instance 26 on Lara: onto an object / its marker
        if (const auto* o = script_.find(to->first)) {
            const std::string unit = o->unit; // `o` may move when the units change
            vec3 at = o->pos;
            float yaw = o->rot.z - kPi / 2; // intro heading as findSpawn: facing the frame's -Y
            if (const auto m = to->second >= 0 ? markerFrame(*o, to->second) : std::nullopt)
                at = vec3((*m)[3]), yaw = std::atan2(-(*m)[1].y, -(*m)[1].x);
            player_.spawn(at, yaw); // standing (ng_idle)
            cam_.yaw = yaw, cam_.pitch = CameraState{}.pitch, cameraCut_ = true; // the camera behind her [I]
            script_.playerJumped();
            for (size_t i = 0; i < units_.size(); ++i)
                if (units_[i].data.name == unit && i != current_) {
                    streamAround(i);
                    break;
                }
        }
    for (const int32_t bike : script_.takeMounts()) mountBike(bike); // larabase 52 from a sleeping handler (l5: 0.6 s)
    collectPickups(a.interact && !interactHeld_);
    carryPortables(a.interact && !interactHeld_ && pickedUp_.empty());
    tickObjects(dt); // object collision + physics doors (Objects.cpp), before enemies and the camera query them
    interactHeld_ = a.interact;
    if (const vec3 d = script_.carried(); d != vec3(0)) { // Lara follows the block she pushes/pulls
        auto s = player_.state();
        s.pos += d;
        player_.restore(s);
    }
    enemies_.tick(col_, {player_.state().pos, !health().dead(), player_.tuning().height, player_.tuning().radius, player_.state().water >= 0},
                  [this](float damage, vec3 from, int type) { damageLara(damage, from, type); }, dt * combat_.timeScale);
    updateCamera(a, dt); // Camera.cpp: a playing spline camera, else the orbit camera
    if (std::exchange(cameraCut_, false)) prevCam_ = cam_; // a spline camera cuts in
    tickCombat(a, dt); // after the camera: free aim follows it
    tickGadgets(a, dt); // flashlight, grenades (Gadgets.cpp)
    tickAir(dt);
    water_.tick(*this, dt); // after combat: this tick's shots make rings
    updatePose(dt, false);
    ++ticks_;
    if (!units_.empty()) checkLevelEnd(); // last: a chapter change re-spawns Lara (enterUnit)
}

// Orbit camera around a point above Lara's root; pulled in along the view ray when level geometry is
// in the way. Distance/height/pitch limits are inferred (docs/GAMEPLAY.md).
void World::updateOrbitCamera(const Actions& a, float dt) {
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
    c.up = glm::normalize(glm::mix(prevCam_.up, cam_.up, t));
    c.fovDeg = prevCam_.fovDeg > 0 && cam_.fovDeg > 0 ? glm::mix(prevCam_.fovDeg, cam_.fovDeg, t) : cam_.fovDeg;
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
    throwLayer(local);
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

uint64_t World::stateHash() const {
    uint64_t h = 1469598103934665603ull;
    auto bytes = [&](const void* p, size_t n) { // FNV-1a over 64-bit words, then the tail bytes
        const auto* c = static_cast<const unsigned char*>(p);
        for (; n >= 8; c += 8, n -= 8) {
            uint64_t w;
            std::memcpy(&w, c, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        for (; n; ++c, --n) h = (h ^ *c) * 1099511628211ull;
    };
    auto add = [&](const auto& v) { bytes(&v, sizeof v); }; // padding-free values only (scalars, glm types)
    auto str = [&](const std::string& s) { bytes(s.data(), s.size()), add(s.size()); };
    const PlayerState& s = player_.state();
    add(ticks_), add(s.state), add(s.pos), add(s.yaw), add(s.vel), add(s.clipTime), add(s.clipRate), add(s.pitch), add(s.ledge);
    add(s.ledgeT), add(s.water), add(s.climbFace), add(s.climbNormal), add(s.grappleUid), add(s.ropeLength), str(s.clip);
    add(cam_.position), add(cam_.target), add(cam_.yaw), add(cam_.pitch), add(health_.value()), add(air_);
    for (const auto& m : bones_) add(m);
    for (const auto& b : physics_.bodies) add(b.pos), add(b.rot), add(b.vel), add(b.angVel);
    for (const auto& m : physics_.movers) add(m.s);
    for (const auto& d : col_.dynamics) add(d.transform);
    for (const auto& o : script_.objects()) {
        add(o.uid), add(o.state), add(o.t), add(o.pos), add(o.offset), add(o.body), add(o.inside), add(o.hidden), add(o.dead);
        if (o.spline) add(o.spline->t), add(o.spline->dir), add(o.spline->playing);
        if (o.joint) add(o.joint->position);
        add(o.loose);
        if (o.graph) add(o.graph->state), add(o.graph->transition), add(o.graph->frames), add(o.graph->clip), add(o.graph->t);
        if (o.script) {
            for (const auto& [k, v] : o.script->fields) add(k), add(v);
            for (const auto& [k, v] : o.script->properties) add(k), add(v);
        }
    }
    for (const auto& e : enemies_.all()) {
        const auto& es = e->state();
        add(e->uid()), add(es.mode), add(es.pos), add(es.yaw), add(es.clipTime), add(es.health), str(es.clip);
    }
    for (const auto& [uid, rt] : uberRuntime_) {
        add(uid), add(rt.fired), add(rt.commanded);
        for (const auto& s : rt.sections) add(s.state), add(s.transition), add(s.time), add(s.length), add(s.carried);
    }
    for (const auto& [uid, m] : uberMotion_) add(uid), add(m.t), add(m.active);
    add(held_);
    return h;
}

// tru.exe: a water volume instance (object `watervolume`) gets a type-10 markup box (0x5D7130, MUD flag
// 0x100000): centred on the instance, rotated with it, half extents = intro data +0x54/+0x58/+0x5C (the centre
// offsets +0x60/+0x64 are 0 in all 82 units); the water data takes bottom/top from that box (0x69D350) and the
// surface is its top [O: l2_cliffclimb's sea 2563 tops out at z 108, l7_arctic_sea's 11 volumes, l4_pool 2661.5
// under the goo plane at 2699]. The player tests its water probe box against those (0x710A20). Intro data +0x12 /
// +0x10 are marker ids (0x69D690), not the surface.
// Markup points: {world AABB min, world AABB max (max.z = surface), centre, half extents, (cos yaw, sin yaw, 0)}.
// Only yaw rotations occur (l5_valaskjalf, l7_yggdrasil_end).
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
        if (vol.data->offset + 0x60 > bytes.size()) continue;
        vec3 half;
        std::memcpy(&half, bytes.data() + vol.data->offset + 0x54, sizeof half);
        const vec3 c(vol.position[0], vol.position[1], vol.position[2]);
        const float cs = std::cos(vol.rotation[2]), sn = std::sin(vol.rotation[2]);
        const vec3 ext(std::abs(cs) * half.x + std::abs(sn) * half.y, std::abs(sn) * half.x + std::abs(cs) * half.y, half.z);
        out.push_back({10, uint16_t(vol.uniqueId), {c - ext, c + ext, c, half, vec3(cs, sn, 0)}});
    }
    return out;
}

// Inside water box `m` (type 10): `p` in its rotated frame, grown by `grow` on each axis. Two-point markups
// (tests) are plain AABBs.
bool insideWater(const collision::Markup& m, vec3 p, vec3 grow) {
    if (m.points.size() < 5)
        return m.points.size() >= 2 && glm::all(glm::lessThanEqual(m.points[0] - grow, p)) &&
               glm::all(glm::lessThanEqual(p, m.points[1] + grow));
    const vec3 d = p - m.points[2], r = m.points[4];
    const vec3 local(r.x * d.x + r.y * d.y, -r.y * d.x + r.x * d.y, d.z);
    return glm::all(glm::lessThanEqual(glm::abs(local), m.points[3] + grow));
}

} // namespace ou::game
