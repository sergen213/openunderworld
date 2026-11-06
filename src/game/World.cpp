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
#include <map>
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
        // surface stroke (request 881), swim sprint (896, 934) and its exhaustion (880, 921)
        {"swim_surface_fast_forward", 8, 8}, {"swim_surface_sprint_forward", 8, 8}, {"swim_submerged_sprint", 10, 10},
        {"swim_surface_exhausted", 8, 6}, {"swim_submerged_exhausted", 8, 4},
        // traversal (requests most often used for each clip)
        {"bbeam_walk", 5, 5}, {"bbeam_idle", 6, 6}, {"bbeam_walk_attach", 7, 1}, {"bbeam_jumpland", 5, 5},
        {"bbeam_walk_turn180", 5, 5}, {"drop_to_ledgeb", 3, 3}, {"ledgeb_jump_up", 8, 4}, {"ledgeb_jump_back", 5, 0},
        {"ledgeb_jump_right", 3, 1}, {"ledgeb_attach_U", 4, 4}, {"ledgeb_attach_S", 4, 4}, {"ledgeb_attach_F", 4, 4},
        {"HPole_idleHangPerp", 0, 0}, {"Hpole_perp_attach_M", 2, 4}, {"HPole_StartSwing", 4, 4}, {"HPole_swing", 1, 1},
        {"hpole_perp_fling", 1, 1}, {"crouch_Idle", 6, 6}, {"crouch_idleToCrouch", 4, 6}, {"Crouch_Front", 6, 3},
        {"ng_roll_front", 3, 1}, {"slope_slidefront", 6, 1},
        // obstacles: requests 127 / 128 / 129 attach low / mid / high, 130 / 131 over, 132 / 133 onto, 134 / 135 onto the
        // far side into a hang, 139 / 140 / 141 down, 147 / 148 / 149 cancel (blend space: each request's first clip)
        {"hurdle_ground_attach_low_50", 3, 3}, {"hurdle_ground_attach_mid_100", 2, 2}, {"hurdle_ground_attach_high_150", 4, 4},
        {"hurdle_low_over", 3, 1}, {"hurdle_over_low_long_50", 3, 1}, {"hurdle_over", 1, 1}, {"hurdle_low_onto", 4, 1},
        {"hurdle_onto", 5, 7}, {"hurdle_to_ledgeb_low_25", 1, 1}, {"hurdle_to_ledgeb_low_65", 1, 1},
        {"hurdle_to_ledgeb_mid_25", 1, 1}, {"hurdle_to_ledgeb_mid_65", 1, 1}, {"hurdle_down_low_50", 3, 1},
        {"hurdle_down_mid_150", 1, 1}, {"hurdle_down_mid_100", 1, 1}, {"hurdle_ground_attach_low_50_cancel", 5, 5},
        {"hurdle_ground_attach_mid_150_cancel", 5, 5}, {"hurdle_ground_attach_mid_100_cancel", 5, 5},
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
    "ledgeb_climbup_fast", "hurdle_low_onto", "hurdle_low_onto_100", "hurdle_onto",
    // obstacles (Player::tryHurdle / tryHurdleDown): lara.drm anim requests 127-141, 147-149 (blend spaces)
    "hurdle_ground_attach_low_50", "hurdle_ground_attach_low_100", "hurdle_ground_attach_mid_100",
    "hurdle_ground_attach_mid_150", "hurdle_ground_attach_high_150", "hurdle_ground_attach_high_200",
    "hurdle_low_over", "hurdle_low_over_100", "hurdle_over_low_long_50", "hurdle_over_low_long_100", "hurdle_over",
    "hurdle_over_long_100", "hurdle_to_ledgeb_low_25", "hurdle_to_ledgeb_low_65", "hurdle_to_ledgeb_low_100",
    "hurdle_to_ledgeb_mid_25", "hurdle_to_ledgeb_mid_65", "hurdle_to_ledgeb_mid_100", "hurdle_down_low_50",
    "hurdle_down_low_100", "hurdle_down_mid_100", "hurdle_down_mid_150", "hurdle_down_high_200",
    "hurdle_ground_attach_low_50_cancel", "hurdle_ground_attach_low_100_cancel", "hurdle_ground_attach_mid_100_cancel",
    "hurdle_ground_attach_mid_150_cancel", "hurdle_ground_attach_high_200_cancel",
    "swim_jump_land", "swim_surfaceidle",
    "swim_surface", "swim_surfaceidle_to_swim",
    "swim_surfacedive", "swim_submerged_idle", "swim_sub", "swim_sub_up", "swim_sub_down", "swim_climbout",
    "bbeam_walk", "bbeam_idle", "bbeam_walk_attach", "bbeam_jumpland", "bbeam_walk_turn180", "drop_to_ledgeb",
    "ledgeb_jump_up", "ledgeb_jump_back", "ledgeb_jump_right", "ledgeb_attach_U", "ledgeb_attach_S", "ledgeb_attach_F",
    "HPole_idleHangPerp", "Hpole_perp_attach_M", "HPole_StartSwing", "HPole_swing", "Hpole_swingstopA", "hpole_perp_fling",
    "crouch_idleToCrouch", "crouch_Idle", "Crouch_Front", "ng_roll_front", "slope_slidefront",
    "sprint_loop", "ng_sprint_to_exhaust", "ng_run_exhaust", "ng_roll_front_sprinting", // sprint (Player::tick)
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
    // reaching for a hold that is not there (pushing toward no hold), the leap up onto a ledge, stepping off at the foot
    "DICE_B_Reach_up", "DICE_B_Reach_down", "DICE_B_Reach_Left", "DICE_B_Reach_Right", "DICE_L_reach_up",
    "DICE_L_reach_down", "DICE_L_reach_left", "DICE_L_reach_right", "DICE_LW_reach_up", "DICE_LW_reach_down",
    "DICE_LW_reach_left", "DICE_LW_reach_right", "DICE_R_reach_up", "DICE_R_reach_down", "DICE_R_reach_left",
    "DICE_R_reach_right", "DICE_Rw_Reach_up", "DICE_Rw_Reach_down", "DICE_Rw_Reach_Left", "DICE_Rw_Reach_Right",
    "DICE_B_jump_up", "DICE_L_jump_up", "DICE_LW_jump_up", "DICE_R_jump_up", "DICE_Rw_jump_up",
    // DICE jumps to far holds: the anticipation (held while the stick points at one), then the launch sideways
    "DICE_B_antic_Up", "DICE_B_antic_Left", "DICE_B_antic_Right", "DICE_L_antic_up", "DICE_L_antic_left",
    "DICE_L_antic_right", "DICE_LW_antic_up", "DICE_LW_antic_left", "DICE_LW_antic_right", "DICE_R_antic_up",
    "DICE_R_antic_left", "DICE_R_antic_right", "DICE_Rw_antic_Up", "DICE_Rw_antic_Left", "DICE_Rw_antic_Right",
    "DICE_B_Jump_Left", "DICE_B_jump_right", "DICE_L_jump_left", "DICE_L_jump_right", "DICE_LW_jump_left",
    "DICE_LW_jump_right", "DICE_R_jump_left", "DICE_R_jump_right", "DICE_Rw_Jump_Left", "DICE_Rw_jump_right",
    "DICE_attach_M", // its root motion = where a DICE jump catches the hold
    "DICE_L_step_off_ground", "DICE_LW_step_off_ground", "DICE_R_step_off_ground",
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
    // surface stroke and swim sprint (Player::swim)
    "swim_surface_sprint_forward", "swim_submerged_sprint", "swim_surface_exhausted", "swim_submerged_exhausted",
    "swim_surface_fast_forward",
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
    : col_(std::move(col)), anims_(std::move(anims)), player_(col_, anims_, tuning) {
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
    // On the motorcycle the bike drives Lara (Vehicle.cpp); use and the grapple wait until she is off it. Weapons stay
    // live: she rides with a pistol / uzi out (Combat.cpp).
    const bool onBike = tickVehicles(input, dt);
    Actions a = input;
    if (onBike) a.move = vec2(0), a.jump = a.drop = a.interact = a.grapple = false;
    // Stick is camera-relative.
    const vec2 fwd(std::cos(cam_.yaw), std::sin(cam_.yaw)), right(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    vec2 move = right * a.move.x + fwd * a.move.y;
    if (glm::length(move) > 1) move = glm::normalize(move);
    if (script_.holding() || uberGrip_) move = vec2(0); // [I] gripping a push block: the stick moves the block (src/script)
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
    player_.setWaveSeconds(double(ticks_) * kTickSeconds);
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
    if (!units_.empty()) { // bend_* plants (game/Bend.hpp) follow the loaded units, then sway and give way to Lara
        if (bendUnits_ != unitsVersion_) {
            std::vector<Bends::Unit> list;
            for (const auto& u : units_) list.push_back({u.data.name, u.offset, &u.data.bends, &u.data.wind});
            bends_.sync(list);
            bendUnits_ = unitsVersion_;
        }
        bends_.tick(player_.state().pos, prev_.pos);
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
    pushUberJoints(a);
    tickObjects(dt); // object collision + physics doors (Objects.cpp), before enemies and the camera query them
    interactHeld_ = a.interact;
    if (const auto* j = uberPush_.uid ? uberJoint(uberPush_.uid, uberPush_.joint) : nullptr) { // she follows the pushed body
        auto s = player_.state();
        s.pos += j->axis * (j->x - uberPush_.before);
        player_.restore(s);
    }
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
    // Swimming soaks every region (tru.exe under Wine: mask 0x7F while she swims); out of the water the mask is clear
    // and she dries (Wetness::tick). Not behind a black screen fade: l1_seatop's chapter start drops her into the sea
    // for 12 ticks at 030_med_yacht's end before 997 puts her on the stern (tru.exe: dry on deck after its larabase 68).
    wetness_.water = player_.state().water >= 0 && script_.screenFade() < 1.0f ? 0x7F : 0;
    wetness_.tick(dt * 30.0f);
    water_.tick(*this, dt); // after combat: this tick's shots make rings
    updatePose(dt, false);
    ++ticks_;
    if (!units_.empty()) checkLevelEnd(); // last: a chapter change re-spawns Lara (enterUnit)
}

// Orbit camera (tru.exe's gameplay camera LaraCameraRewrite, vtable 0x85600C, *0xE80534 under Wine; update 0x602690).
// It orbits a pivot near Lara at the camera mode's distance, its yaw/pitch read back each tick from where it ended up
// (0x5FF320) relative to the moved pivot: whatever the collider swung it to stays, and as she moves it drags after her
// (a held sideways stick circles her round it; running off flattens a looked-down pitch: Wine trace -1.05 -> -0.47 in
// 1 s, no separate pitch return). The camera settings are DTP 0x14 (the global data +0x144's id): distance +0x60
// 1264.22, return acceleration +0x70 1106.36, return speed cap +0x6C 4741.4, follow speed +0x74 3793.1, collider
// radius +0x68 13.91; the mode list +0x148 (0x5F99F0 / 0x5FF080) picks per camera mode the focus record (command 10),
// the distance (2), yaw/pitch limits (7 / 8), pull-in only (15), force sets (0x10 / 1) and the 300 floor probe (0x13;
// defaults 0x601D30 / 0x5FEE10). Not modelled [U]: the default yaw force set 11, the look target blend (0x6009C0),
// level camera records (0x5FE800 resources, command 0x12), moving platforms (0x7027B0 / 0x5FF650).
void World::updateOrbitCamera(const Actions& a, float dt) {
    constexpr float kRadius = 13.91f;
    const Bike* bike = ridden();
    const PlayerState& s = player_.state();
    // Camera mode (CameraManager 0xE804F0 +0x54) the player's state machine requests (message 0x44E -> 0x7683B0 ->
    // 0x5E8210; Wine, hooked at 0x5E8210 while she acted: on foot, jumping and falling 1, free climb 4, ledge hang and
    // shimmy 11, surface swim 20, under water 19), and what its mode-list entry sets; a mode without command 10 / 2
    // gets the defaults (0x601D30: record 1, settings +0x60). Look pitch limits from command 8 (0x607A30: centre
    // (c + 90) deg from straight up, half-range r): look pitch within -c +- r. Ladders, perches, pillars, beams and
    // poles were not observed; their modes come from the settings' markup-type table +0x14C (0x5F9960: ladder 9 -> 10,
    // perch 13 -> 16, pillar 15 -> 17, balance beam 14 -> 3, pole 4 -> 9) [I]. Grapple states: on-foot mode [U].
    struct Mode {
        int id = 1;
        vec3 offset{0, 0, 142.23f}, zones{3.16f, 126.42f, 252.84f}; // focus record offset / dead zones
        float distance = 1264.22f;
        bool floorProbe = true;                                   // command 0x13
        float yawRange = 3.1415927f, pitchLo = -1.2f, pitchHi = 0.7f; // commands 7 / 8 ([I] our free on-foot pitch)
        bool pullIn = false;                                      // command 15: 0x5F0810 instead of the collider
    } mode;
    const float k55 = glm::radians(55.0f), k90 = glm::radians(90.0f), kFree = mode.yawRange;
    const vec3 rec1 = mode.offset, zones = mode.zones;
    switch (s.state) {
    case State::Climb: mode = {4, {0, 0, 79.01f}, zones, 1264.22f, false, k90, -k55, k55, true}; break; // record 10
    case State::Hang: case State::Shimmy: case State::DropToHang: case State::ClimbUp: // [I] the last two
        mode = {11, {0, 63.21f, 316.06f}, zones, 1896.3f, false, k90, -k55, k55, true}; break; // record 8, distance 1896.3
    case State::SwimSurface: case State::SwimTread: case State::SwimSplash: // command 8 (-40, 30)
        mode = {20, rec1, zones, 1264.22f, false, kFree, glm::radians(-70.0f), glm::radians(-10.0f)}; break;
    case State::SwimUnder: case State::SwimDive: case State::Surfacing: // record 41 (no dead zones), command 8 (-5, 80)
        mode = {19, vec3(0), vec3(0), 1264.22f, false, kFree, glm::radians(-85.0f), glm::radians(75.0f)}; break;
    case State::Ladder: mode = {10, rec1, zones, 1896.3f, false, k90, -k55, k55, true}; break; // distance 1896.3
    case State::Perch: mode = {16, rec1, zones, 1264.22f, false, kFree, -1.2f, 0.7f, true}; break;
    case State::Pillar: mode = {17, rec1, zones, 1264.22f, false, kFree, -k55, k55, true}; break;
    case State::Beam: mode = {3, rec1, zones, 1264.22f, true, kFree, -1.2f, 0.7f, true}; break;
    case State::PoleHang: mode = {9, rec1, zones, 1264.22f, false, kFree, -1.2f, 0.7f, true}; break;
    default: break; // mode 1: record 1, command 0x13
    }
    // A new mode's focus record blends in over 0.5 s (settings +0x78 via 0x608200; 0x609830 lerps the records) and its
    // limits over 0.25 s (0x609C00 / 0x608F10: x 4 per second).
    if (mode.id != orbit_.mode) orbit_.from = orbit_.now, orbit_.mode = mode.id, orbit_.modeTime = 0;
    orbit_.modeTime += dt;
    {
        const float f = std::min(1.0f, orbit_.modeTime / 0.5f), g = std::min(1.0f, orbit_.modeTime * 4.0f);
        const auto& o = orbit_.from;
        orbit_.now = {glm::mix(o.offset, mode.offset, f), glm::mix(o.zones, mode.zones, f), glm::mix(o.yawRange, mode.yawRange, g),
                      glm::mix(o.pitchLo, mode.pitchLo, g), glm::mix(o.pitchHi, mode.pitchHi, g)};
        if (orbit_.from.zones.x < 0) orbit_.now = {mode.offset, mode.zones, mode.yawRange, mode.pitchLo, mode.pitchHi}; // first tick
    }
    const auto& lim = orbit_.now;
    // Focus goal (0x607D40): bone 108 (a helper the clips carry: 320 over her feet standing, by the pelvis 95 off the
    // rock climbing) through the mode's focus record (settings +0x50, 0x6C bytes: bone, offset, dead zones, speed,
    // 64-byte ease curve): record 1 (0, 0, 142.23) on foot, 10 (0, 0, 79.01) free climbing, 8 (0, 63.21, 316.06) hanging.
    const glm::mat4 model = player_.modelMatrix(s.pos, s.yaw, s.pitch);
    const vec3 goal = bones_.size() > 108 ? vec3(model * bones_[108] * glm::vec4(lim.offset, 1)) : s.pos + vec3(0, 0, 320.3f) + lim.offset;
    // The focus lags its goal (0x609830 -> 0x609440) per axis of the camera's horizontal frame (0x6091A0: right,
    // forward, up) by the record's dead zones: what is past a zone is caught up at once, the rest at up to 1896.3
    // units/s scaled by the record's ease curve at (distance / zone) (0x5E4530). Moving platforms carry it in the
    // original (0x7027B0); not modelled [U].
    const bool snap = !focus_ || bike || glm::distance(*focus_, goal) > 2000.0f; // spawns, teleports
    if (snap) focus_ = goal;
    {
        static constexpr uint8_t kEase[64] = {0,   1,   2,   2,   2,   3,   3,   3,   4,   5,   6,   8,   10,  13,  16,  20,
                                              24,  29,  34,  40,  46,  52,  58,  65,  72,  79,  86,  93,  101, 109, 118, 127,
                                              137, 146, 155, 164, 173, 181, 188, 195, 201, 207, 213, 218, 223, 228, 232, 236,
                                              239, 241, 244, 245, 247, 248, 249, 250, 251, 251, 252, 252, 253, 253, 254, 255};
        constexpr float kSpeed = 1896.3f;
        const vec3 axis[3] = {{std::sin(cam_.yaw), -std::cos(cam_.yaw), 0}, {std::cos(cam_.yaw), std::sin(cam_.yaw), 0}, {0, 0, 1}};
        const vec3 d = goal - *focus_;
        for (int i = 0; i < 3; ++i) {
            const float off = glm::dot(d, axis[i]), dist = std::abs(off), zone = lim.zones[i];
            if (zone <= 1e-3f) { *focus_ += axis[i] * off; continue; }
            const float excess = std::max(0.0f, dist - zone), x = std::min(1.0f, dist / zone) * 63.0f;
            const int k = int(x), k1 = std::min(63, k + 1);
            const float ease = std::round(float(kEase[k1] - kEase[k]) * (x - float(k)) + float(kEase[k])) / 255.0f;
            const float step = std::min(dist, excess + ease * std::max(0.0f, kSpeed * dt - excess));
            *focus_ += axis[i] * std::copysign(step, off);
        }
    }
    // Pivot clearance (0x5EA050): when the 474.075 above her root is blocked, the pivot is pulled back toward bone 1 to
    // keep 2 x 13.91 clear of what is in the way. Every camera query sees only what tru.exe's camera collides with
    // (collision::kCameraMask: not the terrain groups it skips, only objects with oflags bit 2).
    constexpr uint8_t kMask = collision::kCameraMask;
    vec3 target = *focus_;
    if (col_.raycast(s.pos, {0, 0, 1}, 474.075f, kMask) && bones_.size() > 1) {
        const vec3 from(model * bones_[1][3]), ray = target - from;
        if (glm::length(ray) > 1e-3f) {
            const vec3 dir = glm::normalize(ray);
            if (const auto h = col_.raycast(from, dir, glm::length(ray) + 2 * kRadius, kMask)) target = from + dir * (h->t - 2 * kRadius);
        }
    }
    const float kDistance = bike ? 1711.0f : mode.distance; // [I] VehicleCamera pulls back on the bike (our old 950/700 ratio)
    const bool reseed = snap || !orbit_.valid || cam_.yaw != orbit_.yaw || cam_.pitch != orbit_.pitch; // [I] set from outside
    if (reseed) { // where the angles put it (at full distance after a spawn or teleport)
        const vec3 dir(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
        const float dist = snap || !orbit_.valid ? kDistance : orbit_.dist;
        orbit_.desired = target - dir * dist, orbit_.dist = dist, orbit_.speed = 0, orbit_.valid = true;
        if (snap) cam_.position = orbit_.desired;
    } else if (glm::length(orbit_.desired - target) > 1e-3f) { // angles from where it ended up (0x5FF320)
        const vec3 look = glm::normalize(target - orbit_.desired);
        cam_.yaw = std::atan2(look.y, look.x), cam_.pitch = std::asin(std::clamp(look.z, -1.0f, 1.0f));
    }
    cam_.yaw += a.look.x * dt;
    // Forces (0x608B60, x weight 0x5FFA90: 0 until 5 s after the last look input, 1 from 7 s: settings +0xC4 / +0xC8;
    // the clock +0x1E4 caps at 1000), only while the camera is not being turned. Strength = product of the set's
    // forces (0x5F2180); the angle moves toward the set's target by at most speed x strength x weight x dt.
    // - yaw, free climbing and hanging (command 0x10 -> global data +0x164 set 15): 10 deg/s, flag 3 = toward
    //   (0 + 90) deg in her frame, behind her; force type 7 x 20 = the mean of her last 10 per-frame moves
    //   (0x5E9D70 / 0x608E20), capped at 1 (our 10 samples are 60 Hz ticks [I]).
    // - pitch, on foot (command 1 -> +0x168 set 4): toward -7.5 deg at 5 deg/s; force type 2 x 1 (a speed, capped at
    //   1; [I] her speed in units per 30 Hz frame).
    // Without command 0x10 the default yaw set +0xE4 = 11 applies (180 deg/s behind her, forces of the pitch and her
    // sideways motion; not modelled [U]).
    orbit_.idle = a.look.x != 0 || a.look.y != 0 ? 0.0f : std::min(1000.0f, orbit_.idle + dt);
    std::rotate(orbit_.moves, orbit_.moves + 1, orbit_.moves + 10);
    orbit_.moves[9] = dt > 0 && orbit_.valid ? glm::distance(s.pos, orbit_.lastPos) / (dt * 30.0f) : 0.0f;
    orbit_.lastPos = s.pos;
    float meanMove = 0;
    for (const float m : orbit_.moves) meanMove += m / 10;
    const float weight = std::clamp((orbit_.idle - 5.0f) / 2.0f, 0.0f, 1.0f);
    // Level camera record (0x601D30 / 0x5FE800: the markup she is on (player +0xC94 -> CameraManager +0x74 +0x24: the
    // free-climb hold, the ledge) carries camera commands; command 0x12 selects a record of its unit, used while the
    // camera mode is one the settings' markup-type table +0x14C lists for its type). Its point (0x606160): type 3 =
    // the Hermite spline at u = (index of her markup in the record's list) / (list size - 1), 0 when not listed
    // (0x605ED0); 1 / 2 = at her position along the markup / one minus it (0x5F7510 / 0x5F7550; [I] our ledge
    // distance over its length); 0 = the spline point nearest the pivot (0x605A20); it eases 0.25 per frame toward
    // that. The camera turns toward the point's angles by at most speed (+0x58 deg/s) x strength x dt and, with +0x6C,
    // its distance toward the point's at +0x5C units/s x strength (0x606310), both x the look-idle weight 0x5FFA90, in
    // place of the forces and of the distance return (0x5FFB00). Strength = product of its forces (0x605980 ->
    // 0x5F2180: type 5 = min(1, k x dt), 7 = min(1, k x her mean move), others 0 [I]). Wine, l2_cliffclimb hold wall
    // (record type 3, 300 deg/s, force 5 x 0.2): after 5 s without looking the camera turns 1.0 deg/s toward its point.
    static const std::map<uint8_t, std::vector<int>> kMarkupModes = {{1, {11, 14, 15, 42, 37, 38, 39, 21, 22}}, {14, {3, 43}},
        {9, {10}}, {13, {16}}, {15, {17}}, {4, {9, 3}}, {16, {4}}, {17, {4}}, {18, {4}}, {19, {4}}, {20, {4}}};
    const int on = s.state == State::Climb ? s.climbHold : s.ledge;
    const collision::Markup* onMarkup = on >= 0 && size_t(on) < col_.markup.size() ? &col_.markup[size_t(on)] : nullptr;
    const collision::CameraRecord* rec = nullptr;
    if (onMarkup && onMarkup->camera && !bike)
        if (const auto g = kMarkupModes.find(onMarkup->type); g != kMarkupModes.end() && std::ranges::find(g->second, mode.id) != g->second.end())
            rec = &*onMarkup->camera;
    if (rec && !rec->pos.empty()) {
        const auto& r = *rec;
        const auto spline = [&](float u) { // u in [0, 1] over the whole spline (0x6057F0 / 0x5F7340)
            if (r.pos.size() == 1) return r.pos[0];
            const float sv = std::clamp(u, 0.0f, 1.0f) * float(r.pos.size() - 1);
            const size_t i = std::min(size_t(sv), r.pos.size() - 2);
            const float t = std::min(1.0f, sv - float(i)), t2 = t * t, t3 = t2 * t;
            return (2 * t3 - 3 * t2 + 1) * r.pos[i] + (t3 - 2 * t2 + t) * r.tangent[i] + (3 * t2 - 2 * t3) * r.pos[i + 1] +
                   (t3 - t2) * r.tangent[i + 1];
        };
        float u = 0;
        if (r.type == 3) {
            if (const auto k = std::ranges::find(r.markups, onMarkup->id); k != r.markups.end())
                u = float(k - r.markups.begin()) / float(std::max<size_t>(r.markups.size() - 1, 1));
        } else if (r.type == 1 || r.type == 2) {
            float len = 0, at = 0;
            for (size_t i = 0; i + 1 < onMarkup->points.size(); ++i) {
                const float seg = glm::distance(onMarkup->points[i], onMarkup->points[i + 1]);
                if (int(i) < s.ledgeSeg) at += seg;
                len += seg;
            }
            u = len > 0 ? std::clamp((at + s.ledgeT) / len, 0.0f, 1.0f) : 0.0f;
            if (r.type == 2) u = 1 - u;
        } else { // nearest point to the pivot: coarse samples, then a local refinement
            float best = FLT_MAX;
            for (int i = 0; i <= 64; ++i)
                if (const float d = glm::distance(spline(float(i) / 64), target); d < best) best = d, u = float(i) / 64;
            for (float h = 1.0f / 128; h > 1e-4f; h *= 0.5f)
                for (const float c : {u - h, u + h})
                    if (const float d = glm::distance(spline(c), target); c >= 0 && c <= 1 && d < best) best = d, u = c;
        }
        const vec3 point = spline(u);
        orbit_.hint = orbit_.hintMarkup == on ? orbit_.hint + (point - orbit_.hint) * (1 - std::pow(0.75f, dt * 60.0f)) : point;
        orbit_.hintMarkup = on;
    } else {
        orbit_.hintMarkup = -1;
    }
    bool recordDistance = false;
    if (orbit_.hintMarkup >= 0 && weight > 0 && a.look.x == 0 && a.look.y == 0) {
        float strength = 1;
        for (const auto& [type, k] : rec->forces)
            strength *= type == 5 ? std::min(1.0f, k * dt) : type == 7 ? std::min(1.0f, k * meanMove) : 0.0f;
        const vec3 to = target - orbit_.hint;
        if (glm::length(to) > 1e-3f) {
            const vec3 look = glm::normalize(to);
            const float rate = glm::radians(rec->angleSpeed) * strength * dt;
            cam_.yaw += std::clamp(std::remainder(std::atan2(look.y, look.x) - cam_.yaw, 6.2831853f), -rate, rate) * weight;
            cam_.pitch += std::clamp(std::asin(std::clamp(look.z, -1.0f, 1.0f)) - cam_.pitch, -rate, rate) * weight;
            if (rec->distance) {
                const float step = rec->distanceSpeed * strength * dt;
                orbit_.dist += std::clamp(glm::length(to) - orbit_.dist, -step, step) * weight;
                recordDistance = true;
            }
        }
    } else if (weight > 0 && !bike && a.look.x == 0) {
        if (mode.id == 4 || mode.id == 11) {
            const float rate = glm::radians(10.0f) * std::min(1.0f, meanMove * 20.0f) * weight * dt;
            cam_.yaw += std::clamp(std::remainder(s.yaw - cam_.yaw, 6.2831853f), -rate, rate);
        } else if (mode.id == 1) {
            const float rate = glm::radians(5.0f) * std::min(1.0f, orbit_.moves[9]) * weight * dt;
            cam_.pitch += std::clamp(glm::radians(-7.5f) - cam_.pitch, -rate, rate);
        }
    }
    // [I] riding without looking: swing behind the bike, faster the faster it goes
    if (bike && bike->phase == BikePhase::Ride && a.look.x == 0 && bike->speed > 0)
        cam_.yaw = lerpAngle(cam_.yaw, bike->yaw, std::min(1.0f, dt * 3.0f * std::min(1.0f, bike->speed / 1500.0f)));
    cam_.pitch = std::clamp(cam_.pitch + a.look.y * dt, lim.pitchLo, lim.pitchHi);
    // Yaw limit (0x608700, command 7 via 0x609C00 / 0x607A30: (0 + 90) deg in her frame = behind her, half-range 90):
    // the camera stays on her back side, its look within yawRange of her heading.
    if (lim.yawRange < 3.14f) cam_.yaw = s.yaw + std::clamp(std::remainder(cam_.yaw - s.yaw, 6.2831853f), -lim.yawRange, lim.yawRange);
    cam_.target = target;
    // Distance back toward the mode's (0x5FFB00): its speed accelerates by 1106.36 x clamp((gap - 2 step) / step, -1, 1)
    // per second (step = speed x dt, at least 1) up to 4741.4; it moves by at most last tick's step.
    if (!recordDistance) {
        const float gap = kDistance - orbit_.dist, step = dt * orbit_.speed;
        const float r = std::clamp((std::abs(gap) - 2 * step) / std::max(step, 1.0f), -1.0f, 1.0f);
        orbit_.speed = std::clamp(orbit_.speed + 1106.36f * r * dt, 0.0f, 4741.4f);
        orbit_.dist += std::copysign(std::min(std::abs(gap), step), gap);
    }
    const vec3 dir(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
    const vec3 proposed = target - dir * orbit_.dist;
    // Collider (0x5ED010, settings +0x68 radius): the proposed point stands when a sphere swept out from the pivot
    // reaches it (0x5EC5B0). Else the straight pull-in (0x5ECCE0: where the sweep stops; within 10 of the pivot, where a
    // ray stops) competes with the smallest swing about the pivot that clears (0x5EC8F0 / 0x5EC660: bisection, 10 steps
    // up to 45 deg about the vertical the way it was moving, 5 steps the other way, 5 steps up to 25 deg up and down,
    // the last three costing 5x): cost = distance from the proposed point; the swing wins when strictly cheaper (weight
    // +0x74 = -1: 0.5 / 0.5) and its straight pull-in is at least 10 from the proposed point.
    // Modes with command 15 (0x1328 = 1: climbing, hanging, ladders, perches, pillars, beams, poles) skip it for the
    // plain pull-in 0x5F0810: the nearest contact of the swept sphere more than 10 along (+0x138) puts it there, never
    // nearer than 97.98 (+0x14). A camera record with +0x6D lets it through within its radius of the record's point
    // (0x606070).
    const auto clear = [&](vec3 to) { return !col_.sweepSphere(target, kRadius, to - target, kMask); };
    vec3 out = proposed;
    if (orbit_.hintMarkup >= 0 && rec->inside && glm::distance(cam_.position, orbit_.hint) < rec->radius) {
        // inside the record's radius: no collision
    } else if (mode.pullIn) {
        const float len = glm::length(proposed - target);
        if (len > 10.0f) {
            const vec3 d = (proposed - target) / len;
            if (const auto h = col_.sweepSphere(target + d * 10.0f, kRadius, d * (len - 10.0f), kMask))
                out = target + d * std::clamp(10.0f + (len - 10.0f) * h->t, std::min(97.98f, len), len);
        }
    } else if (!clear(proposed)) {
        vec3 straight = proposed;
        if (const auto h = col_.sweepSphere(target, kRadius, proposed - target, kMask)) straight = target + (proposed - target) * h->t;
        float straightCost = glm::distance(straight, proposed);
        if (glm::distance(straight, target) < 10.0f) {
            const vec3 ray = proposed - target;
            const float len = glm::length(ray);
            const auto h = len > 1e-3f ? col_.raycast(target, ray / len, len, kMask) : std::nullopt;
            straight = h ? h->point : proposed;
            straightCost = len;
        }
        out = straight;
        if (straightCost >= 10.0f) {
            const vec3 v = proposed - target;
            const auto swing = [&](vec3 axis, float maxDeg, int steps, float& cost) {
                float lo = 0, hi = glm::radians(maxDeg);
                std::optional<vec3> best;
                for (int i = 0; i < steps; ++i) {
                    const float mid = (lo + hi) * 0.5f;
                    const vec3 p = target + vec3(glm::rotate(glm::mat4(1), mid, axis) * glm::vec4(v, 0));
                    if (clear(p)) best = p, hi = mid;
                    else lo = mid;
                }
                cost = best ? glm::distance(*best, proposed) : FLT_MAX;
                return best.value_or(proposed);
            };
            const vec3 from = orbit_.desired - proposed, to = target - proposed;
            const float turn = from.x * to.y - from.y * to.x <= 0 ? -1.0f : 1.0f;
            float c[4];
            vec3 p[4] = {swing({0, 0, turn}, 45, 10, c[0]), swing({0, 0, -turn}, 45, 5, c[1]), {}, {}};
            c[1] *= 5;
            const vec3 side(to.y, -to.x, 0);
            if (glm::length(side) > 1e-3f) {
                p[2] = swing(glm::normalize(side), 25, 5, c[2]), p[3] = swing(-glm::normalize(side), 25, 5, c[3]);
                c[2] *= 5, c[3] *= 5;
            } else {
                c[2] = c[3] = FLT_MAX;
            }
            const int best = int(std::min_element(c, c + 4) - c);
            if (c[best] < straightCost && clear(p[best])) out = p[best];
        }
    }
    // Its distance is where it ended up (0x5FE9A0: pulled in by more than 0.1 stops the return speed).
    const float dist = glm::distance(out, target);
    if (dist < orbit_.dist - 0.1f) orbit_.speed = 0;
    orbit_.dist = dist;
    // On foot a ray probe (0x600200 with 0x13 set; settings +0xD0 radius 0) pulls it in to what it sees, never under 300.
    if (mode.floorProbe && dist > 1e-3f) {
        const vec3 d = (out - target) / dist;
        const float far = std::max(dist, kDistance);
        const auto h = col_.raycast(target, d, far, kMask);
        const float f = std::max(300.0f, h ? h->t : far);
        if (f < orbit_.dist) orbit_.dist = f, out = target + d * f, orbit_.speed = 0;
    }
    // The camera follows that point at 3793.1 units/s plus how far the proposed point moved from the last one (0x600710);
    // looking around moves it there at once [I].
    const float jump = glm::distance(proposed, orbit_.desired);
    orbit_.desired = out;
    const vec3 toOut = out - cam_.position;
    const float gapPos = glm::length(toOut);
    if (a.look.x != 0 || a.look.y != 0 || gapPos <= 0.01f) cam_.position = out;
    else cam_.position += toOut * std::min(1.0f, (3793.1f * dt + jump) / gapPos);
    if (glm::length(target - out) > 1e-3f) { // the angles of where it is headed
        const vec3 look = glm::normalize(target - out);
        cam_.yaw = std::atan2(look.y, look.x), cam_.pitch = std::asin(std::clamp(look.z, -1.0f, 1.0f));
    }
    orbit_.yaw = cam_.yaw, orbit_.pitch = cam_.pitch;
}

// tru.exe 0x6FF200 (region soak / dry, no rain or mud) + 0x6ED3F0 (clamp) + 0x6ED130 (bone chains, in its call order).
void Wetness::tick(float t) {
    for (int i = 0; i < kRegions; ++i)
        region[size_t(i)] = std::clamp(region[size_t(i)] + ((water >> i & 1) ? kSoak : -kDry) * t, 0.0f, kMax);
    // Bones of a chain take the region value when it is higher; a wetter bone dries (kDry) only once the bone before it
    // dropped below 1 - 1 / (5 n), so drying runs down the chain. Returns the last bone's value before this tick.
    auto chain = [&](std::initializer_list<uint8_t> bones, int r, float prev) {
        const float start = 1.0f - 1.0f / (float(bones.size()) * 5.0f), v = region[size_t(r)];
        for (const uint8_t b : bones) {
            float& w = bone[b];
            const float cur = w;
            if (cur <= v || (prev < start && prev < cur && (w -= kDry * t) < v)) w = v;
            prev = cur;
        }
        return prev;
    };
    // Bone lists 0x85DC60.. (Lara's skeleton): head/face, spine, pelvis, legs, arms, hands.
    const float head = chain({17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
                              35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51}, 6, 0.0f);
    const float hips = chain({1, 2, 3}, 4, chain({16, 52, 77, 102, 104, 103, 105, 15, 14}, 5, head));
    chain({9, 10}, 1, hips);
    const float thighs = chain({4, 5}, 1, hips);
    chain({11, 12, 13}, 1, thighs);
    chain({6, 7, 8}, 1, thighs);
    chain({78, 79, 97, 98, 99, 100, 101}, 3, head);
    const float arm = chain({53, 54, 72, 73, 74, 75, 76}, 3, head);
    chain({80, 81, 82, 84, 87, 90, 93, 83, 85, 88, 91, 94, 86, 89, 92, 95}, 2, arm);
    chain({55, 56, 57, 59, 62, 65, 68, 58, 60, 63, 66, 69, 61, 64, 67, 70}, 2, arm);
}

// Oxygen: the head is under water in the submerged swim states (root + 150 = head height [I]).
// At 0 air Lara takes max/6 health per second until she surfaces or dies [I rates].
void World::tickAir(float dt) {
    const PlayerState& ps = player_.state();
    submerged_ = false;
    if (ps.water >= 0 && size_t(ps.water) < col_.markup.size()) {
        const float surf = waterSurface(col_.markup[size_t(ps.water)], vec2(ps.pos), double(ticks_) * kTickSeconds);
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
            if (const auto hit = col_.raycast(eye, d / dist, dist - 60.0f); // something blocks the hook (not its own object)
                hit && (hit->dynamic < 0 || col_.dynamics[size_t(hit->dynamic)].id != t.uid))
                continue;
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
    // Light hit reactions are overlay clips (only the spine / head / arm-twist segments keyed, as deltas): played as a
    // full pose they left every other bone at bind, Lara in a T-pose (swimming: shark bites). They go on top of what
    // is showing (anim::AnimController additive layer).
    const bool overlay = s.clip.starts_with("hit_light_");
    if (reset) {
        anim_ = anim::AnimController(anims_.skeleton);
        if (overlay) anim_.play(anims_.find(s.water >= 0 ? "swim_submerged_idle" : "ng_idle"), 0, true); // [I] a base under it
        anim_.play(anims_.find(s.clip), 0, s.loop, s.clipRate, s.clipTime, overlay, anims_.find(s.clipMix), s.mix);
        hair_ = anim::HairSim(anims_.hair, anims_.skeleton, laraHair());
    } else {
        anim_.update(dt);
        // Player::play changed the clip (or restarted it): fade the new one in over what is showing.
        if (s.clip != prev_.clip || s.loop != prev_.loop || s.clipRate != prev_.clipRate || s.clipTime < prev_.clipTime ||
            s.clipMix != prev_.clipMix || s.mix != prev_.mix)
            anim_.play(anims_.find(s.clip), clipBlend(prev_.clip, s.clip).blendSeconds, s.loop, s.clipRate, s.clipTime, overlay,
                       anims_.find(s.clipMix), s.mix);
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
    add(s.ledgeT), add(s.water), add(s.climbHold), add(s.climbNormal), add(s.grappleUid), add(s.ropeLength), str(s.clip);
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
        for (const auto& s : rt.sections) add(s.state), add(s.transition), add(s.time), add(s.length);
        for (const auto& [index, j] : rt.joints) add(index), add(j.x), add(j.v), add(j.lo), add(j.hi), add(j.speed), add(j.force), add(j.motor), add(j.limited);
        for (const int32_t l : rt.links) add(l);
        for (const auto& w : rt.watches) add(w.section), add(w.joint), add(w.range), add(w.mode), add(w.inside);
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
        auto& m = out.emplace_back(collision::Markup{10, uint16_t(vol.uniqueId), {c - ext, c + ext, c, half, vec3(cs, sn, 0)}});
        // Waves (0x6AC720 reads the hook data = intro data + 4): +0x4E bit 1 on; +0x6C height scale; wave i: kx +0x70 + 8i,
        // ky +0x74 + 8i, height share +0x90 + 4i, speed +0xA0 + 4i (rad/s). l1_seatop / l2_cliffclimb / l1_ship_exterior /
        // l6_ship_exterior's seas.
        const auto f = [&](size_t o) { float v; std::memcpy(&v, bytes.data() + vol.data->offset + o, 4); return v; };
        uint16_t flags;
        std::memcpy(&flags, bytes.data() + vol.data->offset + 0x4E, 2);
        if (!(flags & 2) || vol.data->offset + 0xB0 > bytes.size()) continue;
        for (int i = 0; i < 4; ++i) m.points.push_back({f(0x70 + 8 * i), f(0x74 + 8 * i), f(0xA0 + 4 * i)});
        for (int i = 0; i < 4; ++i) m.points.push_back({f(0x6C) * f(0x90 + 4 * i), 0, 0});
    }
    return out;
}

namespace {
float waveSeconds(double seconds) { return float(std::fmod(seconds, 628.3185307179586)); } // 0x6C10D0: fmod(t, 200 pi)
} // namespace

float waveHeight(const collision::Markup& m, vec2 xy, double seconds) {
    if (m.points.size() < 13) return 0;
    const float t = waveSeconds(seconds);
    float h = 0;
    for (int i = 0; i < 4; ++i) {
        const vec3 w = m.points[size_t(5 + i)];
        h += m.points[size_t(9 + i)].x * (std::cos(w.x * xy.x + w.y * xy.y + w.z * t) - 1.0f);
    }
    return h;
}

std::array<vec4, 4> waveRegisters(const collision::Markup& m, double seconds) {
    std::array<vec4, 4> r{};
    if (m.points.size() < 13) return r;
    const float t = waveSeconds(seconds);
    for (int i = 0; i < 4; ++i) {
        const vec3 w = m.points[size_t(5 + i)];
        r[0][i] = w.x, r[1][i] = w.y, r[2][i] = w.z * t, r[3][i] = m.points[size_t(9 + i)].x;
    }
    return r;
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

std::optional<EyeWater> eyeWater(const World& w, vec3 eye, double seconds) {
    const auto& markup = w.collision().markup;
    const auto& s = w.player().state();
    int idx = s.water >= 0 && size_t(s.water) < markup.size() ? s.water : -1;
    for (size_t i = 0; idx < 0 && i < markup.size(); ++i) {
        const auto& m = markup[i];
        if (m.type == 10 && m.points.size() >= 2 && insideWater(m, s.pos) && s.pos.z < waterSurface(m, vec2(s.pos), seconds))
            idx = int(i);
    }
    if (idx < 0) return std::nullopt;
    return EyeWater{idx, waterSurface(markup[size_t(idx)], vec2(eye), seconds)};
}

} // namespace ou::game
