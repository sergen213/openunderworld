// World: collision + Lara + follow camera on a fixed tick; Lara's clip set; level spawn.
#include "game/Game.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"
#include "script/Vm.hpp"

#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <chrono>
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
// A 64-byte curve at x in [0, 1] (tru.exe 0x5E4530: bytes interpolated, rounded, / 255).
float curve64(const uint8_t* c, float x) {
    const float v = std::clamp(x, 0.0f, 1.0f) * 63.0f;
    const int k = int(v), k1 = std::min(63, k + 1);
    return std::round(float(c[k1] - c[k]) * (v - float(k)) + float(c[k])) / 255.0f;
}

// Camera force sets (generalbank GlobalInfo +0x164 -> DTP 0x1C yaw, +0x168 -> DTP 0x1D pitch: {count, 0x14-byte
// entries {angle deg, speed deg/s, dir type, force count, forces*}}, forces 0x44 bytes {type, k | 64-byte curve};
// loaded by 0x6088B0) [O]. Target angle = angle + 90 deg in the pivot frame (yaw: 90 = behind her; pitch: from
// straight up, so the look pitch = angle). Strength = product of the forces (0x607B90 -> 0x5F2180): 1 = |her move
// . the camera's x axis| x k, 2 = |her move| x k, 3 = curve(angle off behind her / 180 deg), 4 = curve(|move stick|),
// 5 = k x dt, 7 = her mean move over 10 ticks x k; each capped at 1 (3 / 4 through the curve).
struct CamForce {
    uint8_t type;
    float k;
    const uint8_t* curve;
};
struct CamForceSet {
    int id;
    float angle, speed;
    uint8_t dir, count; // dir: 0 fixed, 1 by her sideways move, 2 by her vertical move, 3 by her move along her markup
    CamForce force[2];
};
constexpr uint8_t kSideCurve[64] = {0,   0,   0,   0,   0,   0,   0,   3,   9,   14,  20,  27,  34,  41,  48,  55,
                                    62,  69,  76,  83,  89,  95,  101, 108, 115, 122, 128, 134, 140, 145, 148, 151,
                                    152, 151, 149, 144, 139, 132, 125, 117, 109, 101, 94,  88,  82,  75,  68,  61,
                                    54,  47,  40,  33,  26,  20,  14,  8,   3,   0,   0,   0,   0,   0,   0,   0}; // yaw set 11
constexpr uint8_t kStickCurve[64] = {0,   1,   2,   3,   3,   4,   5,   5,   6,   7,   8,   9,   10,  11,  13,  14,
                                     16,  19,  21,  23,  26,  28,  31,  34,  37,  40,  43,  47,  50,  54,  58,  62,
                                     66,  70,  74,  79,  83,  88,  93,  98,  103, 108, 113, 119, 124, 130, 136, 142,
                                     148, 154, 161, 168, 175, 182, 189, 196, 204, 211, 218, 226, 233, 241, 248, 255}; // yaw 4, pitch 16
constexpr CamForceSet kYawSets[] = {
    {1, 0, 20, 0, 1, {{2, 0.1f, nullptr}}},                                   // pillars
    {4, 0, 15, 0, 1, {{4, 0, kStickCurve}}},                                  // poles
    {5, 0, 70, 0, 1, {{5, 200, nullptr}}},                                    // perches
    {6, 0, 0, 0, 0, {}},                                                      // balance beams (speed 0)
    {11, 0, 180, 0, 2, {{3, 0, kSideCurve}, {1, 0.02f, nullptr}}},            // default (settings +0xE4)
    {15, -60, 10, 3, 1, {{7, 20, nullptr}}},                                  // free climbing, hanging
};
constexpr CamForceSet kPitchSets[] = {
    {4, -7.5f, 5, 0, 1, {{2, 1, nullptr}}},     // on foot
    {16, 45, 5, 2, 1, {{4, 0, kStickCurve}}},   // ladders, pillars
    {17, -10, 70, 0, 1, {{5, 1, nullptr}}},     // perches
};
const CamForceSet* camForceSet(const CamForceSet* sets, size_t n, int id) {
    for (size_t i = 0; i < n; ++i)
        if (sets[i].id == id) return &sets[i];
    return nullptr; // none: no forces, speed 0 (0x608D20)
}
// What the forces read this tick (0x5F2180's inputs).
struct ForceInputs {
    vec3 move;      // her move this tick, units per frame (0x5E7EF0)
    float meanMove; // the mean of her last 10 (0x5E9D70)
    float stick;    // |move stick| (0x5E9830)
    vec3 camX;      // the camera's x axis (BaseCamera +0x10 row 0, 0x5E7850): its right (Wine: row 2 = the look)
    float dt;
};
float camForceStrength(const CamForceSet* s, float offBehind, const ForceInputs& in) {
    float p = 1;
    for (int i = 0; s && i < s->count; ++i) {
        const CamForce& f = s->force[i];
        switch (f.type) {
        case 1: p *= std::min(1.0f, std::abs(glm::dot(in.move, in.camX)) * f.k); break;
        case 2: p *= std::min(1.0f, glm::length(in.move) * f.k); break;
        case 3: p *= curve64(f.curve, std::abs(offBehind) / kPi); break;
        case 4: p *= curve64(f.curve, std::min(1.0f, in.stick)); break;
        case 5: p *= std::min(1.0f, f.k * in.dt); break;
        case 6: p *= std::min(1.0f, std::abs(in.move.z) * f.k); break;
        case 7: p *= std::min(1.0f, in.meanMove * f.k); break;
        default: p = 0; break;
        }
    }
    return p;
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
        // under-water dodges (requests 863 / 864 / 875-878), swan dive (67 / 69 / 68, landings 66 / 897 / 843)
        {"swim_submerged_dodge_forward", 5, 1}, {"swim_submerged_dodge_backward", 5, 1}, {"swim_submerged_dodge_side", 5, 1},
        {"swim_submerged_dodge_up", 5, 1}, {"swim_submerged_dodge_down", 5, 1}, {"ng_inair_swan_stage1", 7, 7},
        {"ng_inair_swan_stage2", 3, 3}, {"ng_inair_swan_stage3", 1, 1}, {"ng_swan_land", 3, 2}, {"swim_swan_land", 4, 4},
        {"swim_land_swan_wading", 1, 1}, {"death_fall_swan", 1, 1},
        {"swim_surface_idle_turn_90", 1, 1}, {"swim_surface_idle_turn_180", 1, 1}, // requests 832 / 833
        // traversal (requests most often used for each clip)
        {"bbeam_walk", 5, 5}, {"bbeam_idle", 6, 6}, {"bbeam_walk_attach", 7, 1}, {"bbeam_jumpland", 5, 5}, {"bbeam_ledgeu_climbup", 1, 10}, {"ledgeb_jumpuptocrouch", 3, 3},
        {"bbeam_flip_foward", 5, 5},
        {"bbeam_walk_turn180", 5, 5}, {"drop_to_ledgeb", 3, 3}, {"ledgeb_jump_up", 8, 4}, {"ledgeb_jump_back", 5, 0},
        {"ledgeb_jump_right", 3, 1}, {"ledgeb_attach_U", 4, 4}, {"ledgeb_attach_S", 4, 4}, {"ledgeb_attach_F", 4, 4},
        {"HPole_idleHangPerp", 0, 0}, {"Hpole_perp_attach_M", 2, 4}, {"HPole_StartSwing", 4, 4}, {"HPole_swing", 1, 1},
        {"hpole_perp_fling", 1, 1}, {"crouch_Idle", 6, 6}, {"crouch_idleToCrouch", 4, 6}, {"Crouch_Front", 6, 3},
        {"ng_roll_front", 3, 1}, {"slope_slidefront", 6, 1},
        {"bbeam_idle_wobble_fwd", 3, 3}, // request 158
        // off balance (Player::beamAboutFall / startBeamFall): requests 177, 187 / 190, 186 / 188
        {"bbeam_aboutfall_center", 6, 6}, {"bbeam_fall_left", 5, 2}, {"bbeam_fall_right", 5, 2},
        {"bbeam_fall_left_no_ledge", 5, 2}, {"bbeam_fall_right_no_ledge", 5, 2},
        // poles: requests 194, 1337, 1351 / 1350 / 1352 / 1349, 1338 / 1343 / 1340 / 1341, 1309, 1333 / 1332 / 1331, 1330,
        // 1329, 1361 / 1362
        {"hpole_IdleHangPara", 4, 4}, {"Hpole_para_attach_S", 1, 1}, {"Hpole_PerptoPara", 6, 6}, {"Hpole_ParatoPerp", 3, 3},
        {"Hpole_Perpturn180", 6, 6}, {"Hpole_Paraturn180", 6, 6}, {"HPole_ParaForward_trans", 3, 3}, {"HPole_ParaForward", 3, 3},
        {"HPole_ParaForward_to_idle_L_hand", 8, 6}, {"HPole_ParaForward_to_idle_R_hand", 8, 6}, {"hpole_para_fling", 1, 1},
        {"Hpole_swingstopA", 2, 10}, {"Hpole_swingstopB", 2, 5}, {"Hpole_swingstopC", 8, 4}, {"Hpole_startswing_endA", 3, 3},
        {"Hpole_swingreverseA", 1, 1}, {"Hpole_detach_dropPerp", 3, 1}, {"Hpole_detach_dropPara", 3, 1},
        // pole top (Player::poleTop): requests 1306, 1365, 1291, 1299, 1292, 1293, 1294, 1295, 1296, 1297, 1298
        {"hpole_to_perch", 1, 1}, {"hpole_para_to_perch", 1, 1}, {"hpole_perch_ready", 4, 4}, {"hpole_perch_fall_to_hpole", 1, 1},
        {"hpole_perch_Jump_up", 10, 10}, {"hpole_perch_Jump_right", 10, 10}, {"hpole_perch_Jump_left", 10, 10},
        {"hpole_perch_Jump_forward_Antic", 15, 15}, {"hpole_perch_Jump_forward", 5, 5}, {"hpole_perch_Jump_back_Antic", 15, 15},
        {"hpole_perch_Jump_back", 10, 10},
        // ladders: requests 1161 (attach_M), 1163, 1165, 1175, 1169, 1168, 1173, 1172, 1171, 1176
        {"ladder_attach_M", 3, 1}, {"ladder_attach_ground_bottom", 14, 14}, {"ladder_attach_ground_top", 4, 0},
        {"ladder_idle", 3, 3}, {"ladder_climb_up", 3, 3}, {"ladder_climb_down", 3, 3}, {"ladder_detach_ground_top", 3, 1},
        {"ladder_detach_ground_bottom", 3, 1}, {"ladder_detach_drop", 3, 7}, {"ladder_jump_back", 3, 1},
        {"ladder_attach_air_bad", 3, 1}, {"ladder_bad_attach_recover", 3, 3}, // requests 1162, 1167 (the saving grab)
        // ladder jumps / leans / reaches: requests 1185, 1177, 1182, 1181, 1184, 1183, 1187, 1188, 1186, 1192, 1197, 1198, 1195
        {"ladder_jump_left", 3, 1}, {"ladder_jump_right", 3, 1}, {"ladder_jump_left_prepare", 3, 1},
        {"ladder_jump_left_prepare_untargeted", 6, 6}, {"ladder_jump_right_prepare", 3, 1},
        {"ladder_jump_right_prepare_untargeted", 6, 6}, {"ladder_idle_to_jump_back_antic", 3, 3}, {"ladder_jump_back_prepare", 1, 1},
        {"ladder_jump_back_antic_to_idle", 1, 1}, {"ladder_reach_left", 5, 5}, {"ladder_reach_right", 5, 5},
        {"ladder_settle_left", 4, 3}, {"ladder_settle_right", 4, 3},
        // ledge corners: requests 1040, 1041, 1030, 1031, 1045, 1046, 1038, 1039, 1037, 1036
        {"ledgeb_concave_right_phase1", 3, 2}, {"ledgeb_concave_right_phase2", 3, 1}, {"ledgeb_convex_right_phase1", 6, 4},
        {"ledgeb_convex_right_phase2", 6, 4}, {"ledgeb_concave_idle", 4, 2}, {"ledgeb_convex_idle", 8, 4},
        {"ledgeb_concave_jump_up_antic", 1, 1}, {"ledgeb_concave_jump_back_antic", 10, 1}, {"ledgeb_convex_jump_up_antic", 1, 1},
        {"ledgeb_convex_jump_back_antic", 10, 1},
        // pillars: requests 621 (attach_M), 617, 616, 632, 626, 624, 633, 634, 635, 630, 631, 629, 639, 640, 636, 638
        {"pillar_attach_M", 3, 3}, {"pillar_attach_ground", 3, 1}, {"pillar_attach_perch", 3, 3}, {"pillar_idle", 4, 4},
        {"pillar_climb_up", 3, 1}, {"pillar_climb_right", 3, 3}, {"pillar_slide_down_start", 3, 3}, {"pillar_slide_down", 3, 3},
        {"pillar_slide_down_stop", 3, 2}, {"pillar_detach_perch", 3, 1}, {"pillar_detach_ground", 1, 1},
        {"pillar_detach_drop", 3, 1}, {"pillar_jump_back", 3, 3}, {"pillar_jump_forward", 3, 3},
        {"pillar_jump_right", 3, 1}, {"pillar_jump_back_45R", 3, 3},
        // perches: requests 661, 647, 649, 658, 663, 664, 668 / 669 / 670
        {"perch_idle", 6, 6}, {"perch_attach_air", 2, 8}, {"perch_attach_ground", 3, 1}, {"perch_detach_drop", 5, 1},
        {"perch_jump_forward", 3, 3}, {"perch_jump_up", 3, 3}, {"perch_turn_right_45", 3, 3}, {"perch_turn_right_90", 5, 5},
        {"perch_turn_right_180", 3, 3},
        // wall hug: requests 1421, 1402, 1378, 1416, 1410, 1403, 1400 / 1399 (corners), 1377, 161
        {"Wall_Hug_Idle", 4, 4}, {"Wall_Hug_Shimmy_Left", 4, 4}, {"Wall_Hug_Walk_Off", 4, 4}, {"Wall_Hug_Jump_Forward", 4, 4},
        {"Wall_Hug_jump_left", 4, 4}, {"Wall_hug_Jump_Up_Twist", 4, 4}, {"Wall_Hug_Shimmy_to_Concave_left_70", 4, 4},
        {"Wall_Hug_Shimmy_to_Concave_left_90", 4, 4}, {"Wall_Hug_Shimmy_to_Concave_left_110", 4, 4},
        {"Wall_Hug_Shimmy_to_Convex_left_70", 4, 4}, {"Wall_Hug_Shimmy_to_Convex_left_90", 4, 4},
        {"Wall_Hug_Shimmy_to_Convex_left_110", 4, 4}, {"Wall_Hug_Drop_to_Water", 4, 4}, {"bbeam_somersault_forward", 5, 5},
        // chimney jumps: requests 1153, 1155, 1156, 1157 (WallJump.cpp)
        {"wall_jump_attach_air", 4, 1}, {"wall_jump_idle", 1, 1}, {"wall_jump_jump_back", 2, 1}, {"wall_jump_detach_fall", 3, 3},
        // DICE bad attach: requests 256 / 255 / 254 (B), 226 / 224 / 225 (LW), 231 / 229 / 230 (R), 280 / 281 / 282 (Rw),
        // 228 / 227 (L)
        {"DICE_B_air_attach_bad_start", 5, 5}, {"DICE_B_air_attach_bad_loop", 1, 1}, {"DICE_B_air_attach_bad_end", 5, 5},
        {"DICE_LW_air_attach_bad_start", 3, 3}, {"DICE_LW_air_attach_bad_loop", 1, 1}, {"DICE_LW_air_attach_bad_end", 5, 5},
        {"DICE_R_air_attach_bad_start", 3, 3}, {"DICE_R_air_attach_bad_loop", 1, 1}, {"DICE_R_air_attach_bad_end", 5, 5},
        {"DICE_Rw_air_attach_bad_start", 5, 5}, {"DICE_Rw_air_attach_bad_loop", 1, 1}, {"DICE_Rw_air_attach_bad_end", 3, 3},
        {"DICE_L_air_attach_bad", 3, 3}, {"DICE_L_air_attach_bad_recovery", 3, 3},
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

namespace {

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
    "bbeam_ledgeu_climbup", "ledgeb_jumpuptocrouch", "bbeam_flip_foward", // from the hang by its clearance (Player.cpp State::Hang)
    "bbeam_unbalance_walk_left", "bbeam_unbalance_walk_right", "bbeam_idle_wobble_fwd", // off balance (Player::beam)
    "bbeam_aboutfall_center", "bbeam_aboutfall_left", "bbeam_aboutfall_right", "bbeam_fall_left", "bbeam_fall_right",
    "bbeam_fall_left_no_ledge", "bbeam_fall_right_no_ledge", // about to fall, falling off (Player::beamAboutFall)
    "ledgeb_jump_up", "ledgeb_jump_back", "ledgeb_jump_right", "ledgeb_attach_U", "ledgeb_attach_S", "ledgeb_attach_F",
    // leaning into a hang jump (Player.cpp State::Hang)
    "ledgeb_idle_to_jump_up_antic", "ledgeb_jump_up_antic", "ledgeb_jump_up_antic_to_idle", "ledgeb_idle_to_jump_right_antic",
    "ledgeb_jump_right_antic", "ledgeb_jump_right_antic_to_idle", "ledgeb_jump_right_from_antic",
    "ledgeb_idle_to_jump_back_antic", "ledgeb_jump_back_antic", "ledgeb_jump_back_antic_to_idle",
    // ledge corners (Player::startHangCorner / hangCorner)
    "ledgeb_concave_right_phase1", "ledgeb_concave_right_phase2", "ledgeb_convex_right_phase1", "ledgeb_convex_right_phase2",
    "ledgeb_concave_idle", "ledgeb_convex_idle", "ledgeb_concave_jump_up_antic", "ledgeb_concave_jump_back_antic",
    "ledgeb_convex_jump_up_antic", "ledgeb_convex_jump_back_antic",
    "HPole_idleHangPerp", "Hpole_perp_attach_M", "HPole_StartSwing", "HPole_swing", "Hpole_swingstopA", "hpole_perp_fling",
    // poles (Player::pole): parallel hang, hand over hand, turns, swing stops / reverse, drops
    "hpole_IdleHangPara", "Hpole_para_attach_S", "Hpole_PerptoPara", "Hpole_ParatoPerp", "Hpole_Perpturn180",
    "Hpole_Paraturn180", "HPole_ParaForward_trans", "HPole_ParaForward", "HPole_ParaForward_to_idle_L_hand",
    "HPole_ParaForward_to_idle_R_hand", "hpole_para_fling", "Hpole_swingstopB", "Hpole_swingstopC", "Hpole_startswing_endA",
    "Hpole_swingreverseA", "Hpole_detach_dropPerp", "Hpole_detach_dropPara",
    "hpole_to_perch", "hpole_para_to_perch", "hpole_perch_ready", "hpole_perch_fall_to_hpole", "hpole_perch_Jump_up", // pole top
    "hpole_perch_Jump_right", "hpole_perch_Jump_left", "hpole_perch_Jump_forward_Antic", "hpole_perch_Jump_forward",
    "hpole_perch_Jump_back_Antic", "hpole_perch_Jump_back",
    "crouch_idleToCrouch", "crouch_Idle", "Crouch_Front", "ng_roll_front", "slope_slidefront",
    "sprint_loop", "ng_sprint_to_exhaust", "ng_run_exhaust", "ng_roll_front_sprinting", // sprint (Player::tick)
    "ledgeb_bad_attach_M", "ledgeb_bad_attach_recover", // saving grab (Player::tryGrab)
    "obj_block_grab", "obj_block_idle", "obj_block_release", "obj_block_front", "obj_block_back", // push blocks (Player::grip)
    "gear_weapon_pickup", "gear_weapon_pickup_crouch", // pickups (World::collectPickups)
    "death_fall_normal", "death_toknee", "death_drown", // combat (Combat.cpp)
    "wading_idle", "wading_walk", "swim_to_wade", "swim_land_wading", // wading (Player: water class 0x100)
    "hit_light_front", "hit_light_back", "hit_light_side", "hit_mediumfront", "hit_mediumback", "hit_mediumright",
    "hit_light_front_swim", "hit_light_back_swim", "hit_light_side_swim", "hit_mediumfront_swim", "hit_mediumback_swim",
    "hit_mediumright_swim",
    // free climbing (Climb.cpp), one set per hand stance B / L / LW / R / RW (the hold's markup type 16-20): attach,
    // idle, directional moves, jump off, drop; then every hold-to-hold transition DICE_<from>_to_<to>_<dir> as lara.drm
    // spells it (stance capitalisation varies: Lw/LW, Rw/RW; Climb.cpp looks them up case-insensitively)
    "DICE_B_ground_attach", "DICE_B_air_attach_M", "DICE_B_Idle", "DICE_B_Settle_up", "DICE_B_Settle_down",
    "DICE_B_Settle_Left", "DICE_B_Settle_right", "DICE_B_Jump_Back", "DICE_B_Drop",
    // bad attach (Climb.cpp badClip)
    "DICE_B_air_attach_bad_start", "DICE_B_air_attach_bad_loop", "DICE_B_air_attach_bad_end", "DICE_L_air_attach_bad",
    "DICE_L_air_attach_bad_recovery", "DICE_LW_air_attach_bad_start", "DICE_LW_air_attach_bad_loop",
    "DICE_LW_air_attach_bad_end", "DICE_R_air_attach_bad_start", "DICE_R_air_attach_bad_loop", "DICE_R_air_attach_bad_end",
    "DICE_Rw_air_attach_bad_start", "DICE_Rw_air_attach_bad_loop", "DICE_Rw_air_attach_bad_end",
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
    "grapple_rappel_free_jumpoff", "grapple_rappel_jumpoff_left", "grapple_rappel_jumpoff_right", "grapple_rappel_jumpoff_back",
    "grapple_rappel_free_rotate_left", "grapple_rappel_free_rotate_right",
    "grapple_rappel_wall_land", "grapple_rappel_wall_land_left", "grapple_rappel_wall_land_right", "grapple_rappel_up",
    "grapple_rappel_up_left_45", "grapple_rappel_up_right_45", "grapple_rappel_down", "grapple_rappel_down_left_45",
    "grapple_rappel_down_right_45", "ng_ground_to_wallrun_left", "ng_ground_to_wallrun_right", "grapple_rappel_left",
    "grapple_rappel_right", "grapple_rappel_walk_left", "grapple_rappel_walk_right", "grapple_rappel_overhang_descend_begin",
    "grapple_rappel_overhang_descend", "grapple_rappel_free_overhang_ascend",
    // ladders / pillars / perches (Traverse.cpp)
    "ladder_attach_M", "ladder_attach_ground_bottom", "ladder_attach_ground_top", "ladder_idle", "ladder_climb_up",
    "ladder_climb_down", "ladder_detach_ground_top", "ladder_detach_ground_bottom", "ladder_detach_drop", "ladder_jump_back",
    "ladder_attach_air_bad", "ladder_bad_attach_recover",
    "ladder_jump_up", "ladder_jump_up_at_top", "ladder_idle_to_jump_up_prepare", "ladder_jump_up_prepare",
    "ladder_jump_up_prepare_to_idle", "ladder_jump_left", "ladder_jump_right", "ladder_jump_left_prepare",
    "ladder_jump_left_prepare_untargeted", "ladder_jump_right_prepare", "ladder_jump_right_prepare_untargeted",
    "ladder_idle_to_jump_back_antic", "ladder_jump_back_prepare", "ladder_jump_back_antic_to_idle", "ladder_reach_left",
    "ladder_reach_right", "ladder_settle_left", "ladder_settle_right",
    "pillar_attach_M", "pillar_attach_ground", "pillar_attach_perch", "pillar_idle", "pillar_climb_up", "pillar_climb_right",
    "pillar_slide_down_start", "pillar_slide_down", "pillar_slide_down_stop", "pillar_detach_perch", "pillar_detach_ground",
    "pillar_detach_drop", "pillar_jump_back", "pillar_jump_forward", "pillar_jump_right", "pillar_jump_back_45R", "perch_attach_air", "perch_attach_ground", "perch_idle", "perch_jump_forward",
    "perch_jump_up", "perch_detach_drop", "perch_turn_right_45", "perch_turn_right_90", "perch_turn_right_180",
    "Wall_Hug_Idle", "Wall_Hug_Shimmy_Left", "Wall_Hug_Walk_Off", "Wall_Hug_Jump_Forward", // wall hug (Traverse.cpp)
    "Wall_Hug_jump_left", "Wall_hug_Jump_Up_Twist", "Wall_Hug_Shimmy_to_Concave_left_70", "Wall_Hug_Shimmy_to_Concave_left_90",
    "Wall_Hug_Shimmy_to_Concave_left_110", "Wall_Hug_Shimmy_to_Convex_left_70", "Wall_Hug_Shimmy_to_Convex_left_90",
    "Wall_Hug_Shimmy_to_Convex_left_110", "Wall_Hug_Drop_to_Water", "bbeam_somersault_forward",
    "wall_jump_attach_air", "wall_jump_idle", "wall_jump_jump_back", "wall_jump_detach_fall", // chimney jumps (WallJump.cpp)
    "grapple_pull_up_light_begin", "grapple_pull_mid_light_begin", "grapple_pull_down_light_begin",
    "grapple_pull_up_heavy_begin", "grapple_pull_mid_heavy_begin", "grapple_pull_down_heavy_begin",
    "grapple_pull_up_light", "grapple_pull_mid_light", "grapple_pull_down_light",
    "grapple_pull_up_heavy", "grapple_pull_mid_heavy", "grapple_pull_down_heavy",
    // surface stroke and swim sprint (Player::swim)
    "swim_surface_sprint_forward", "swim_submerged_sprint", "swim_surface_exhausted", "swim_submerged_exhausted",
    "swim_surface_fast_forward",
    // under-water evasive dodges, swan dive (Player::swim, Player::tick Jump)
    "swim_submerged_dodge_forward", "swim_submerged_dodge_backward", "swim_submerged_dodge_side", "swim_submerged_dodge_up",
    "swim_submerged_dodge_down", "ng_inair_swan_stage1", "ng_inair_swan_stage2", "ng_inair_swan_stage3", "ng_swan_land",
    "swim_swan_land", "swim_land_swan_wading", "death_fall_swan",
    "swim_surface_idle_turn_90", "swim_surface_idle_turn_180", // turning on the spot at the surface
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
    if (const auto* c = lib.find("hpole_IdleHangPara"))
        if (auto h = lib.handsAt(*c, 0)) lib.poleParaHandOffset = *h;
    lib.hair = anim::decodeHair(drm, lib.skeleton);
    lib.bind = anim::bindPositions(lib.skeleton);
    lib.ragdoll = physics::loadRagdoll(drm);
    if (lib.ragdoll && std::ranges::any_of(lib.ragdoll->bones, [&](const physics::RagdollBone& b) {
            return size_t(b.segment) >= lib.bind.size() || size_t(b.endSegment) >= lib.bind.size();
        }))
        lib.ragdoll.reset();
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
    loadClips(drm, kMeleeClipNames, path, ""); // close combat / carried objects (Melee.cpp)
    if (auto t = loadMeleeTuning(bigfile)) lib.melee = *t;
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
    if (auto t = loadGrappleTuning(bigfile)) lib.grapple = *t;
    if (auto t = loadAdrenalineTuning(bigfile)) lib.adrenaline = *t;
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
    // Lara's switch clips live in the switch DRMs (they animate her skeleton); their animation ids are shared, so these
    // four hold every switch's (survey: docs/GAMEPLAY.md "Interactions"). Kept as "use:<anim id>" (World::useSwitchClip).
    // ponytail: a fixed source list; load per switch DRM if a new id shows up.
    for (const char* object : {"ms_ei_switch_oneway_wall", "ar_ei_switch_oneway", "is_cheat_button", "ar_uo_rune_wall_switch"})
        if (auto b = bigfile.read(std::string("pc-w\\") + object + ".drm")) {
            const cdc::Drm d = cdc::parseDrm(std::move(*b));
            for (const auto& e : anim::animList(d))
                if (e.name == "obj_pull_lever" || e.name == "obj_pushbutton" || e.name == "interact")
                    for (const auto& s : d.sections)
                        if (s.type == cdc::SectionType::Animation && s.id == e.animId) {
                            lib.clips.emplace("use:" + std::to_string(e.animId), anim::decodeClip(s.data));
                            break;
                        }
        }
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
    bindFeedback();
    bindMarkup();
    bindEnemies();
    bindWater();
    setTailoring(tailoring_);
    script_.setCameraNative([this](int op, const std::string& unit, uint32_t index, uint32_t value) {
        return cameraNative(op, unit, index, value);
    });
    script_.setTimeNative([this](bool world, float target, float seconds) { setTimeScale(world, target, seconds); });
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
    joinStreams(false); // background neighbour loads due this tick (Stream.cpp)
    prev_ = player_.state();
    prevCam_ = cam_;
    // tru.exe's game loop (0x5DA1C0): both time scales follow their ramps, then every update's step is the frame's x
    // timeScale x worldScale and Lara's own x timeScale only (0x622418 divides worldScale back out). `dt` = Lara's step,
    // `wdt` = the world's (enemies, objects, water); the camera and the input clocks keep real time [I]. Scripts run on
    // their own tick count: their waits are not scaled [U].
    const float real = float(kTickSeconds);
    tickTimeScales(real);
    const float dt = real * combat_.timeScale, wdt = dt * combat_.worldScale;
    // On the motorcycle the bike drives Lara (Vehicle.cpp); use and the grapple wait until she is off it. Weapons stay
    // live: she rides with a pistol / uzi out (Combat.cpp).
    const bool onBike = tickVehicles(input, dt);
    Actions a = input;
    if (onBike) a.move = vec2(0), a.jump = a.drop = a.interact = a.grapple = false;
    // An interaction clip plays out with the input locked (State::Use: a switch, a pickup, letting go of a block).
    // During the head shot's hosted clip the fire and lock inputs stay live (its aim window, Aim.cpp).
    if (player_.state().state == State::Use) {
        a.move = vec2(0), a.jump = a.drop = a.interact = a.grapple = a.draw = false;
        if (combat_.headShot < 0) a.fire = a.aim = false;
    }
    if (!onBike) meleeInput(a); // a close-combat move's input lock, the carried portable's keys, evasive moves (Melee.cpp)
    // The grapple (virtual button 1) on its press edge: throw at the target, or let go of the rope. tru.exe's keyboard
    // defaults bind Q to both it and Recenter Camera (virtual button 8): 0x487710 binds button i to key 0x80 + DirectInput
    // scan code, buttons 1 and 8 both 0x90 = 0x80 + DIK_Q, as the registry's Input list writes them ("1"="144",
    // "8"="144") [O]. A press that throws or releases the grapple does not also recenter [I: the shared key must not
    // swing the camera off the throw].
    // With the adrenaline meter latched and a locked enemy close by, the press starts the head shot instead (state chart
    // T1404 on input action 46 = the Grapple button, in the close-combat input group; Combat.cpp).
    const bool headShotPress = a.grapple && !grappleHeld_ && !fieldCamera_ && headShotAvailable();
    combat_.headShotRequest = headShotPress;
    const bool grapplePress = a.grapple && !grappleHeld_ && !fieldCamera_ && !headShotPress; // the field camera view takes no grapple
    const std::optional<GrappleTarget> grappleAt = grapplePress && !player_.state().grappleUid ? grappleTarget() : std::nullopt;
    const bool grappleActs = grapplePress && (player_.state().grappleUid || grappleAt);
    grappleHeld_ = a.grapple;
    grapplePress_ = {};
    if (grapplePress)
        grapplePress_.kind = player_.state().grappleUid ? GrapplePress::Release : grappleAt ? GrapplePress::Throw : GrapplePress::NoTarget,
        grapplePress_.uid = grappleAt ? grappleAt->uid : player_.state().grappleUid;
    // Recenter camera (virtual button 8, patch 1.1) [I: snaps the orbit behind her at the rest pitch; the orbit
    // reseeds from the new angles at its distance].
    if (input.recenter && !recenterHeld_ && !grappleActs && !fieldCamera_ && !splineCam_)
        cam_.yaw = player_.state().yaw, cam_.pitch = CameraState{}.pitch, grapplePress_.recentered = grapplePress;
    recenterHeld_ = input.recenter;
    // Field camera (virtual button 13 "Camera"; tru.exe LookAroundState, vtable 0x8842C0; viewfinder g_binoculars.drm
    // texture 649 with its zoom - / + marks): toggled on the press edge while she is on foot; she holds still and
    // the look input aims it (updateFieldCamera). [I] forward / back zoom; leaving the standing states ends it.
    {
        const State st = player_.state().state;
        const bool standing = !onBike && !health_.dead() && !splineCam_ &&
                              (st == State::Idle || st == State::Walk || st == State::Run || st == State::Stop || st == State::Turn);
        if (input.fieldCamera && !fieldCameraHeld_) {
            fieldCamera_ = !fieldCamera_ && standing;
            if (fieldCamera_) cam_.yaw = player_.state().yaw, cam_.pitch = 0, fieldZoom_ = 0;
            else cam_.pitch = CameraState{}.pitch;
        } else if (fieldCamera_ && !standing) {
            fieldCamera_ = false, cam_.pitch = CameraState{}.pitch;
        }
        fieldCameraHeld_ = input.fieldCamera;
        if (fieldCamera_) {
            fieldZoom_ = std::clamp(fieldZoom_ + a.move.y * dt, 0.0f, 1.0f); // full zoom in 1 s
            const vec2 look = a.look;
            a = Actions{}, a.look = look;
        }
    }
    // Stick is camera-relative.
    const vec2 fwd(std::cos(cam_.yaw), std::sin(cam_.yaw)), right(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    vec2 move = right * a.move.x + fwd * a.move.y;
    if (glm::length(move) > 1) move = glm::normalize(move);
    if (script_.holding() || uberGrip_ != vec3(0)) move = vec2(0); // [I] gripping a push block: the stick moves the block (src/script)
    if (combat_.headShot >= 0) move = vec2(0); // the head shot move plays out (Combat.cpp)
    if (combat_.manualAim) a.walk = true, a.sprint = false; // [I] manual aim: she steps, she does not run
    if (splineCam_ && !script_.playerControl()) move = vec2(0); // reveal camera with control disabled (larabase 56)
    if (grappleActs) {
        if (player_.state().grappleUid) releaseGrapple(false);
        else attachGrapple(grappleAt->pos, grappleAt->uid, grappleAt->kind, grappleAt->rope),
             grapplePress_.attached = player_.state().grappleUid == grappleAt->uid;
    }
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
    player_.setCamera(cam_.yaw, cam_.pitch);
    const auto playerStart = std::chrono::steady_clock::now();
    if (!onBike) player_.tick(move, a, dt);
    tickTimes.playerMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - playerStart).count();
    hingePull_.clear();
    if (player_.state().state != State::GrapplePull) pullT_ = 0;
    else if (uberGrappleOff(player_.state().grappleUid)) player_.releaseGrapple(false); // its state dropped the target (action 0x1A)
    else if (a.interact)
        if (auto* target = const_cast<script::Object*>(script_.find(player_.state().grappleUid)); target) {
            if (target->kind == script::Kind::Block) {
                vec3 delta = player_.state().pos - (target->pos + target->offset);
                delta.z = 0;
                if (const float distance = glm::length(delta); distance > 1.0f)
                    target->offset += delta * std::min(240.0f * float(dt), distance) / distance; // [I] braced pull speed
            } else if (pullGrappled(a)) { // the rope drags the target's own joint (Objects.cpp)
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
    // A gripped block moves once her grab clip is done (Player::grip). Where she cannot grip (swimming, in the air) or
    // without the clip (synthetic libraries) the stick moves it at once.
    Actions push = a;
    const bool grabbing = player_.state().state == State::Block ? !player_.gripping() : player_.canInteract();
    if (grabbing && anims_.find("obj_block_grab")) push.move = vec2(0);
    const auto scriptStart = std::chrono::steady_clock::now();
    script_.setTimeScale(combat_.timeScale * combat_.worldScale);
    script_.tick({player_.state().pos, player_.state().yaw, push.move, a.interact, engagedUse(a.interact && !interactHeld_)}, &col_);
    tickTimes.scriptMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - scriptStart).count();
    if (const auto* o = script_.used() ? script_.find(script_.used()) : nullptr) useSwitchClip(*o);
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
    const bool interactPress = a.interact && !interactHeld_;
    const int32_t heldBefore = held_;
    const size_t insertedBefore = inserted_.size();
    collectPickups(interactPress);
    carryPortables(interactPress && pickedUp_.empty());
    pushUberJoints(push);
    tickObjects(wdt); // object collision + physics doors (Objects.cpp), before enemies and the camera query them
    interactHeld_ = a.interact;
    // Close combat (Melee.cpp): an interact press that no pickup, portable, switch, block or handle took kicks / swings.
    if (!onBike)
        tickMelee(a, interactPress && pickedUp_.empty() && held_ == heldBefore && inserted_.size() == insertedBefore && !script_.used() &&
                         !script_.holding() && !uberHandle_.uid && uberGrip_ == vec3(0) && player_.state().state != State::GrapplePull, dt);
    vec3 carry = script_.carried();
    if (const auto* j = uberPush_.uid ? uberJoint(uberPush_.uid, uberPush_.joint) : nullptr) { // she follows the pushed body
        const vec3 p = player_.state().pos;
        if (j->hinge) { // around its axis (calendar rings: World::pushUberHandle), on her floor
            const vec3 moved = vec3(glm::rotate(glm::mat4(1), j->x - uberPush_.before, j->axis) * glm::vec4(p - j->pivot, 1)) + j->pivot - p;
            carry += vec3(moved.x, moved.y, 0);
        } else {
            carry += j->axis * (j->x - uberPush_.before);
        }
    }
    if (carry != vec3(0)) { // Lara follows the block she pushes/pulls
        auto s = player_.state();
        s.pos += carry;
        player_.restore(s);
    }
    const vec3 grip = script_.gripAxis() != vec3(0) ? script_.gripAxis() : uberGrip_;
    if (!onBike) player_.grip(grip, carry == vec3(0) ? 0 : glm::dot(carry, grip) > 0 ? 1 : -1);
    enemies_.tick(col_, {player_.state().pos, !health().dead(), player_.tuning().height, player_.tuning().radius, player_.state().water >= 0},
                  [this](float damage, vec3 from, int type) { damageLara(damage, from, type); }, wdt);
    syncRagdolls(false); // bodies for this tick's starting ragdolls, so a save between ticks holds them
    updateCamera(a, real); // Camera.cpp: a playing spline camera, else the orbit camera
    if (std::exchange(cameraCut_, false)) prevCam_ = cam_; // a spline camera cuts in
    tickCombat(a, dt); // after the camera: free aim follows it
    if (!combat_.shots.empty()) enemies_.disturb(player_.state().pos); // her shots: disturbance message 0xBF (0x6FFFC0)
    tickGadgets(a, dt); // flashlight, grenades (Gadgets.cpp)
    tickWaterStates(wdt); // scripted water levels (0x6C10D0), before Lara's breath reads the surface
    tickAir(dt);
    // Swimming soaks every region (tru.exe under Wine: mask 0x7F while she swims); out of the water the mask is clear
    // and she dries (Wetness::tick). Not behind a black screen fade: l1_seatop's chapter start drops her into the sea
    // for 12 ticks at 030_med_yacht's end before 997 puts her on the stern (tru.exe: dry on deck after its larabase 68).
    wetness_.water = player_.state().water >= 0 && script_.screenFade() < 1.0f ? 0x7F : 0;
    wetness_.tick(dt * 30.0f);
    water_.tick(*this, wdt); // after combat: this tick's shots make rings
    updatePose(dt, false);
    rumble.tick(real), cameraShake.tick(real); // output only (Feedback.hpp): 0x4817B0, 0x5F3390
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
    case State::SwimUnder: case State::SwimDive: case State::Surfacing: case State::SwimDodge: // record 41 (no dead zones), command 8 (-5, 80)
        mode = {19, vec3(0), vec3(0), 1264.22f, false, kFree, glm::radians(-85.0f), glm::radians(75.0f)}; break;
    case State::Ladder: mode = {10, rec1, zones, 1896.3f, false, k90, -k55, k55, true}; break; // distance 1896.3
    case State::Perch: mode = {16, rec1, zones, 1264.22f, false, kFree, -1.2f, 0.7f, true}; break;
    case State::Pillar: mode = {17, rec1, zones, 1264.22f, false, kFree, -k55, k55, true}; break;
    case State::Beam: mode = {3, rec1, zones, 1264.22f, true, kFree, -1.2f, 0.7f, true}; break;
    case State::PoleHang: mode = {9, rec1, zones, 1264.22f, false, kFree, -1.2f, 0.7f, true}; break;
    default: break; // mode 1: record 1, command 0x13
    }
    // Manual aim (player state S718 and its siblings request camera mode 23, state property 1102; mode list entry 20:
    // command 2 distance 632.11, command 10 focus record 22 = bone 108 + (0, 0, 158.03), dead zones 126.42 / 3.16 /
    // 252.84, follow speed 632.11; no limits, force sets or floor probe of its own) [O camera DTP 0x14 / 0x1E]. Its
    // second record (command 11: 35, z 221.24, no dead zones) is the look target ours does not model [U].
    if (combat_.manualAim) mode = {23, {0, 0, 158.03f}, {126.42f, 3.16f, 252.84f}, 632.11f, false, kFree, -1.2f, 0.7f};
    // Its force sets (the mode list's command 0x10 yaw set, 1 pitch set; without 0x10 the default yaw set settings
    // +0xE4 = 11, without 1 none: 0x601D30 -> 0x609C40 / 0x608D20) [O: camera DTP 0x14 +0x144 / +0x148].
    const auto [yawSet, pitchSet] = [&]() -> std::pair<int, int> {
        switch (mode.id) {
        case 1: return {11, 4};
        case 23: return {11, -1};
        case 3: return {6, -1};
        case 4: case 11: return {15, -1};
        case 9: return {4, -1};
        case 10: return {11, 16};
        case 16: return {5, 17};
        case 17: return {1, 16};
        default: return {11, -1}; // 19, 20
        }
    }();
    // A new mode's focus record blends in over 0.5 s (settings +0x78 via 0x608200; 0x609830 lerps the records) and its
    // limits over 0.25 s (0x609C00 / 0x608F10: x 4 per second).
    // Its force sets switch with it (0x609C40 / 0x608D20): the old set B becomes set A, blended to where the last
    // switch had got (target angle and speed lerped by min(1, 4 x its clock)), the new set B starts with side sign 0.
    const auto switchForces = [](OrbitState::Forces& c, const CamForceSet* sets, size_t n, int id) {
        const float f = std::min(1.0f, c.blend * 4.0f);
        const CamForceSet* b = camForceSet(sets, n, c.set);
        c.fromBase += f * ((b ? glm::radians(b->angle + 90.0f) : 0.0f) - c.fromBase);
        c.fromSpeed += f * ((b ? glm::radians(b->speed) : 0.0f) - c.fromSpeed);
        c.fromSign = c.sign, c.fromSet = c.set, c.set = id, c.sign = 0, c.blend = 0;
    };
    if (mode.id != orbit_.mode) {
        orbit_.from = orbit_.now, orbit_.mode = mode.id, orbit_.modeTime = 0;
        switchForces(orbit_.yawForce, kYawSets, std::size(kYawSets), yawSet);
        switchForces(orbit_.pitchForce, kPitchSets, std::size(kPitchSets), pitchSet);
    }
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
    const glm::mat4 model = player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll);
    const vec3 goal = bones_.size() > 108 ? vec3(model * bones_[108] * glm::vec4(lim.offset, 1)) : s.pos + vec3(0, 0, 320.3f) + lim.offset;
    // The focus lags its goal (0x609830 -> 0x609440) per axis of the camera's horizontal frame (0x6091A0: right,
    // forward, up) by the record's dead zones: what is past a zone is caught up at once, the rest at up to 1896.3
    // units/s scaled by the record's ease curve at (distance / zone) (0x5E4530). Moving platforms carry it in the
    // original (0x7027B0); not modelled [U].
    const bool snap = !focus_ || bike || glm::distance(*focus_, goal) > 2000.0f; // spawns, teleports
    if (snap) focus_ = goal;
    {
        constexpr uint8_t kEase[64] = {0,   1,   2,   2,   2,   3,   3,   3,   4,   5,   6,   8,   10,  13,  16,  20,
                                       24,  29,  34,  40,  46,  52,  58,  65,  72,  79,  86,  93,  101, 109, 118, 127,
                                       137, 146, 155, 164, 173, 181, 188, 195, 201, 207, 213, 218, 223, 228, 232, 236,
                                       239, 241, 244, 245, 247, 248, 249, 250, 251, 251, 252, 252, 253, 253, 254, 255};
        constexpr float kSpeed = 1896.3f;
        const vec3 axis[3] = {{std::sin(cam_.yaw), -std::cos(cam_.yaw), 0}, {std::cos(cam_.yaw), std::sin(cam_.yaw), 0}, {0, 0, 1}};
        const vec3 d = goal - *focus_;
        for (int i = 0; i < 3; ++i) {
            const float off = glm::dot(d, axis[i]), dist = std::abs(off), zone = lim.zones[i];
            if (zone <= 1e-3f) { *focus_ += axis[i] * off; continue; }
            const float excess = std::max(0.0f, dist - zone), ease = curve64(kEase, dist / zone);
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
    // Forces (0x5FFB00 -> 0x608B60 per controller, x the look-idle weight 0x5FFA90: 0 until 5 s after the last look
    // input, 1 from 7 s, settings +0xC4 / +0xC8; the clock +0x1E4 runs at +0xCC = 1 x dt also while she moves and caps
    // at 1000), only while the camera is not being turned and no level camera record applies. Each controller turns
    // its angle toward its set's target by at most speed x strength x dt (sets and forces: kYawSets / kPitchSets):
    // - on foot, swimming and on ladders the default yaw set 11: toward behind her at 180 deg/s x curve(angle off
    //   behind her / 180 deg: 0 within 20 deg of behind or of in front, 0.6 at her side) x min(1, 0.02 x her move
    //   across the view): it swings round behind her only while she runs sideways to it, never while she stands;
    // - free climbing / hanging set 15: 10 deg/s x min(1, 20 x her mean move), 60 deg to the side she moves along her
    //   markup (dir 3; sign 0 = behind her until she moves);
    // - on foot pitch set 4: toward -7.5 deg at 5 deg/s x min(1, her move); ladders / pillars pitch set 16: 45 deg up
    //   while she climbs up, down while she climbs down (dir 2) at 5 deg/s x curve(|stick|); poles, perches, pillars,
    //   beams yaw sets 4 / 5 / 1 / 6, perches pitch set 17.
    // Per tick (0x608E20): the switch clock runs to 0.25 s (reaching it refills the move history with this move,
    // 0x607C00), the dir-type signs follow her move (0x608A50: the sign of her move across the view / up / along her
    // markup, kept within 0.001 of 0), her move this tick goes into the 10-entry history. Her move is the instance's
    // displacement since the last frame (0x5E7EF0: +0x20 - +0xC0, less the platform's); ours per 60 Hz tick: tru.exe runs
    // at 60 fps (Wine, l1_seatop surface swim: history 21.97 a frame at 1318 units/s; the original's own value scales
    // with its frame rate). Wine, the same swim with the stick held right (camera +0x8F8 = yaw set 11, +0xDA0 pitch
    // none): after a mouse nudge the camera circles her at 60 deg/s for 5 s, then 101 deg/s from 7 s (the swing adds
    // 41 deg/s; set 11 from tru.exe's own heading / velocity there predicts 41-43).
    orbit_.idle = a.look.x != 0 || a.look.y != 0 ? 0.0f : std::min(1000.0f, orbit_.idle + dt);
    const vec3 move = dt > 0 && orbit_.valid ? (s.pos - orbit_.lastPos) / (dt * 60.0f) : vec3(0);
    orbit_.lastPos = s.pos;
    const vec3 camX(std::sin(cam_.yaw), -std::cos(cam_.yaw), 0);
    float along = -1; // 0x6EC660: her position along the markup she hangs on, -1 without one [I: our distance / length]
    if (s.ledge >= 0 && size_t(s.ledge) < col_.markup.size() && (mode.id == 4 || mode.id == 11)) {
        const auto& pts = col_.markup[size_t(s.ledge)].points;
        float len = 0, at = 0;
        for (size_t i = 0; i + 1 < pts.size(); ++i) {
            const float seg = glm::distance(pts[i], pts[i + 1]);
            if (int(i) < s.ledgeSeg) at += seg;
            len += seg;
        }
        if (len > 0) along = (at + s.ledgeT) / len;
    }
    for (auto* c : {&orbit_.yawForce, &orbit_.pitchForce}) {
        const bool done = c->blend >= 0.25f;
        c->blend = std::min(0.25f, c->blend + dt);
        if (!done && c->blend >= 0.25f) std::fill(orbit_.moves, orbit_.moves + 10, glm::length(move));
        const CamForceSet* sets = c == &orbit_.yawForce ? kYawSets : kPitchSets;
        const size_t n = c == &orbit_.yawForce ? std::size(kYawSets) : std::size(kPitchSets);
        for (auto [id, sign] : {std::pair{c->fromSet, &c->fromSign}, std::pair{c->set, &c->sign}}) {
            const CamForceSet* set = camForceSet(sets, n, id);
            const int dir = set ? set->dir : 0;
            const float v = dir == 1 ? glm::dot(move, camX) : dir == 2 ? move.z : dir == 3 ? along - orbit_.along : 0.0f;
            if (std::abs(v) >= 0.001f) *sign = v >= 0 ? 1.0f : -1.0f;
        }
    }
    orbit_.along = along;
    std::rotate(orbit_.moves, orbit_.moves + 1, orbit_.moves + 10);
    orbit_.moves[9] = glm::length(move);
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
    } else if (weight > 0 && !bike && a.look.x == 0 && a.look.y == 0) {
        const ForceInputs in{move, meanMove, glm::length(a.move), camX, dt};
        const float off = std::remainder(cam_.yaw - s.yaw, 6.2831853f); // 0 = behind her
        // 0x608B60: during a switch (f = min(1, 4 x clock) < 1) the rate is speed x strength(B) x f x (1 - f) x
        // strength(A) toward the lerped target; the yaw frame angle = 90 deg + off [I: the side of +].
        const auto force = [&](const OrbitState::Forces& c, const CamForceSet* sets, size_t n, float angle) {
            const CamForceSet *A = camForceSet(sets, n, c.fromSet), *B = camForceSet(sets, n, c.set);
            const float f = std::min(1.0f, c.blend * 4.0f), kHalf = kPi / 2;
            const float baseB = B ? glm::radians(B->angle + 90.0f) : 0.0f, speedB = B ? glm::radians(B->speed) : 0.0f;
            const float targetA = A && A->dir ? c.fromSign * (c.fromBase - kHalf) + kHalf : c.fromBase;
            const float targetB = B && B->dir ? c.sign * (baseB - kHalf) + kHalf : baseB;
            const float other = std::abs(f - 1.0f) < 0.001f ? 1.0f : (1 - f) * camForceStrength(A, off, in);
            const float rate = glm::mix(c.fromSpeed, speedB, f) * camForceStrength(B, off, in) * f * other * dt;
            return std::clamp(std::remainder(glm::mix(targetA, targetB, f) - angle, 6.2831853f), -rate, rate);
        };
        cameraSwing_ = force(orbit_.yawForce, kYawSets, std::size(kYawSets), kPi / 2 + off) * weight;
        cam_.yaw += cameraSwing_;
        cam_.pitch += force(orbit_.pitchForce, kPitchSets, std::size(kPitchSets), kPi / 2 + cam_.pitch) * weight;
    }
    // [I] riding without looking: swing behind the bike, faster the faster it goes
    if (bike && bike->phase == BikePhase::Ride && a.look.x == 0 && bike->speed > 0)
        cam_.yaw = lerpAngle(cam_.yaw, bike->yaw, std::min(1.0f, dt * 3.0f * std::min(1.0f, bike->speed / 1500.0f)));
    // Look input does not turn the proposed point (0x600200 skips it when 0x5FFB00 flags the frame as turned): the
    // camera is turned after it has followed (below).
    const bool looking = a.look.x != 0 || a.look.y != 0;
    cam_.pitch = std::clamp(cam_.pitch, lim.pitchLo, lim.pitchHi);
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
    const auto plainPullIn = [&](vec3 p) { // 0x5F0810
        const float len = glm::length(p - target);
        if (len <= 10.0f) return p;
        const vec3 d = (p - target) / len;
        const auto h = col_.sweepSphere(target + d * 10.0f, kRadius, d * (len - 10.0f), kMask);
        return h ? target + d * std::clamp(10.0f + (len - 10.0f) * h->t, std::min(97.98f, len), len) : p;
    };
    const auto insideRecord = [&] { return orbit_.hintMarkup >= 0 && rec->inside && glm::distance(cam_.position, orbit_.hint) < rec->radius; };
    vec3 out = proposed;
    if (insideRecord()) {
        // inside the record's radius: no collision
    } else if (mode.pullIn) {
        out = plainPullIn(proposed);
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
    // The camera follows that point at 3793.1 units/s plus how far the proposed point moved from the last one (0x600710).
    const float jump = glm::distance(proposed, orbit_.desired);
    orbit_.desired = out;
    const vec3 toOut = out - cam_.position;
    const float gapPos = glm::length(toOut), step = 3793.1f * dt + jump;
    if (gapPos <= 0.01f) cam_.position = out;
    else cam_.position += toOut * std::min(1.0f, step / gapPos);
    if (glm::length(target - out) > 1e-3f) { // the angles of where it is headed
        const vec3 look = glm::normalize(target - out);
        cam_.yaw = std::atan2(look.y, look.x), cam_.pitch = std::asin(std::clamp(look.z, -1.0f, 1.0f));
    }
    // Turned (0x600710 with the 0x5FFB00 flag): once it got there, the look input turns that point about the pivot at
    // its distance (pitch and yaw limits 0x608700), only the plain pull-in 0x5F0810 (no swing) brings it in, and the
    // camera is put there at once. tru.exe: the camera turns freely round Lara on l2_cliffclimb's yacht deck, pulled in
    // by the cabin to ~960-1110 while turning.
    if (looking && gapPos < step + 0.001f) {
        cam_.yaw += a.look.x * dt;
        cam_.pitch = std::clamp(cam_.pitch + a.look.y * dt, lim.pitchLo, lim.pitchHi);
        if (lim.yawRange < 3.14f) cam_.yaw = s.yaw + std::clamp(std::remainder(cam_.yaw - s.yaw, 6.2831853f), -lim.yawRange, lim.yawRange);
        const vec3 d(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
        vec3 p = target - d * orbit_.dist;
        if (!insideRecord()) p = plainPullIn(p);
        cam_.position = orbit_.desired = p;
        const float turned = glm::distance(p, target);
        if (turned < orbit_.dist - 0.1f) orbit_.speed = 0;
        orbit_.dist = turned;
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

// Breath (tru.exe 0x755230): while the head is under water (state-chart variable 0x322; ours: the submerged swim states
// with root + 150 under the surface [I]) it drains a frame per frame from kAirSeconds; kDrownPeriodSeconds after it runs
// out Lara takes kDrownDamage x max health (type 3) and the period restarts. Out of it, it refills in kAirRefillSeconds.
// With scuba gear (World::scuba) it never drains. The drain is multiplied by state-chart property 0x392 (914) of the
// current state (0x6EEED0; the chart root S0 has 1.0): only the under-water sprint state S159 overrides it, with
// {3, 1.5} [O lara.drm chart], so sprinting under water spends the breath 1.5 times as fast (88.9 s).
void World::tickAir(float dt) {
    const PlayerState& ps = player_.state();
    submerged_ = false;
    if (ps.water >= 0 && size_t(ps.water) < col_.markup.size()) {
        const float surf = waterSurface(col_.markup[size_t(ps.water)], vec2(ps.pos), double(ticks_) * kTickSeconds);
        switch (ps.state) {
        case State::SwimUnder:
        case State::SwimDodge:
        case State::SwimDive:
        case State::Surfacing:
        case State::SwimSplash: submerged_ = ps.pos.z + 150.0f < surf; break;
        default: break;
        }
    }
    if (health_.dead()) { air_ = 1.0f; return; }
    if (submerged_ && !scuba()) { // 0x6EEF50: with the tanks in use she is treated as out of the water
        air_ -= dt / kAirSeconds * (ps.clip == "swim_submerged_sprint" ? kAirSprintDrain : 1.0f);
        if (constexpr float period = kDrownPeriodSeconds / kAirSeconds; air_ <= -period) {
            air_ += period;
            damageLara(health_.max() * kDrownDamage, ps.pos, kDamagePeriodic);
        }
    } else {
        air_ = std::min(1.0f, std::max(0.0f, air_) + dt / kAirRefillSeconds); // 0x756C40: a spent meter restarts at 0
    }
}

PlayerState World::interpolated(double alpha) const {
    const float t = float(alpha);
    PlayerState s = player_.state();
    s.pos = glm::mix(prev_.pos, s.pos, t);
    s.yaw = lerpAngle(prev_.yaw, s.yaw, t);
    s.pitch = glm::mix(prev_.pitch, s.pitch, t);
    s.roll = glm::mix(prev_.roll, s.roll, t);
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
        anim_.play(anims_.find(s.clip), 0, s.loop, s.clipRate, s.clipTime, overlay, anims_.find(s.clipMix), s.mix, s.mirror);
        hair_ = anim::HairSim(anims_.hair, anims_.skeleton, laraHair());
    } else {
        anim_.update(dt);
        // Player::play changed the clip (or restarted it): fade the new one in over what is showing.
        if (s.clip != prev_.clip || s.loop != prev_.loop || s.clipRate != prev_.clipRate || s.clipTime < prev_.clipTime ||
            s.clipMix != prev_.clipMix || s.mix != prev_.mix || s.mirror != prev_.mirror)
            anim_.play(anims_.find(s.clip), clipBlend(prev_.clip, s.clip).blendSeconds, s.loop, s.clipRate, s.clipTime, overlay,
                       anims_.find(s.clipMix), s.mix, s.mirror);
        else
            anim_.setTime(s.clipTime); // the player's clock is authoritative (root motion comes from it)
    }
    auto local = anim_.pose();
    aimLayer(local, reset ? 0 : dt);
    throwLayer(local);
    if (!local.empty()) local[0] = {{0, 0, 0}, {1, 1, 1}, glm::make_vec3(anims_.skeleton.bones[0].pivot)}; // root motion is in pos
    auto m = anim::worldMatrices(anims_.skeleton, local);
    // Her death ragdoll (combat_.ragdoll*) starts from the death clip's last pose once it is done: tru.exe's death state
    // builds it (0x6F1400 -> 0x4F1920) on message 1 (0x75B000; [I] sent at the clip's end) or at once for death
    // types 3 / 4 (DeathState_Entry 0x75AA50; which deaths those are [U]). Each segment then follows its nearest
    // ancestor-or-self that starts a bone, with its offset in that pose (as ai::Enemy). Respawn timing is unchanged.
    if (const auto& rd = anims_.ragdoll; rd && s.state == State::Dead) {
        const anim::Clip* clip = anims_.find(s.clip);
        if (!combat_.ragdollPhase && (!clip || s.clipTime >= clip->durationSeconds())) {
            combat_.ragdoll.clear();
            for (const auto& b : rd->bones) combat_.ragdoll.push_back(m[size_t(b.segment)]);
            combat_.ragdollPhase = 1;
        }
        if (combat_.ragdollPhase && combat_.ragdoll.size() == rd->bones.size()) {
            const auto& sk = anims_.skeleton.bones;
            std::vector<int> owner(sk.size(), 0);
            auto out = m;
            for (size_t k = 0; k < sk.size() && k < m.size(); ++k) {
                const auto own = std::ranges::find(rd->bones, int(k), &physics::RagdollBone::segment);
                owner[k] = own != rd->bones.end() ? int(own - rd->bones.begin()) : sk[k].parent >= 0 ? owner[size_t(sk[k].parent)] : 0;
                out[k] = combat_.ragdoll[size_t(owner[k])] * glm::inverse(m[size_t(rd->bones[size_t(owner[k])].segment)]) * m[k];
            }
            m = std::move(out);
        }
    }
    const glm::mat4 toWorld = player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll);
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
    for (const auto& [name, part] : stateParts()) h = (h ^ part) * 1099511628211ull;
    return h;
}

std::vector<std::pair<std::string, uint64_t>> World::stateParts() const {
    std::vector<std::pair<std::string, uint64_t>> out;
    uint64_t h = 0;
    auto part = [&](std::string name) { out.emplace_back(std::move(name), 0), h = 1469598103934665603ull; };
    auto bytes = [&](const void* p, size_t n) { // FNV-1a over 64-bit words, then the tail bytes
        const auto* c = static_cast<const unsigned char*>(p);
        for (; n >= 8; c += 8, n -= 8) {
            uint64_t w;
            std::memcpy(&w, c, 8);
            h = (h ^ w) * 1099511628211ull;
        }
        for (; n; ++c, --n) h = (h ^ *c) * 1099511628211ull;
        out.back().second = h;
    };
    auto add = [&](const auto& v) { bytes(&v, sizeof v); }; // padding-free values only (scalars, glm types)
    auto str = [&](const std::string& s) { bytes(s.data(), s.size()), add(s.size()); };
    const PlayerState& s = player_.state();
    part("player");
    add(ticks_), add(s.state), add(s.pos), add(s.yaw), add(s.vel), add(s.clipTime), add(s.clipRate), add(s.pitch), add(s.ledge);
    add(s.ledgeT), add(s.water), add(s.climbHold), add(s.climbNormal), add(s.grappleUid), add(s.ropeLength), str(s.clip);
    add(s.loop), str(s.clipMix), add(s.mix), add(s.mirror), add(s.ledgeSeg), add(s.ledgeSide), add(s.runJump), add(s.regrabCooldown), add(s.savingGrab);
    add(s.flight), add(s.fallStartZ), add(s.fallHeight), add(s.climbStance), add(s.climbWarp), add(s.climbFrom), add(s.climbTo);
    add(s.climbYawFrom), add(s.climbYawTo), add(s.grappleAnchor), add(s.grappleKind), add(s.sprintUsed);
    add(s.ropeBends), add(s.polePara), add(s.ropeMax);
    add(s.jumpStart), add(s.jumpTime), add(s.jumpAimed), add(s.wallJumps), add(s.wallTime), add(s.jumpLedge), add(s.jumpFromLedge);
    for (const ButtonTime* b : {&s.buttons.jump, &s.buttons.interact, &s.buttons.drop, &s.buttons.grapple})
        add(b->down), add(b->edge), add(b->time), add(b->buffer);
    add(s.droppedPole), add(s.fallStall);
    const auto& ba = s.balance;
    add(ba.build), add(ba.aboutFall), add(ba.grace), add(ba.unbal), add(ba.bal), add(ba.notImprove), add(ba.full), add(ba.clock);
    add(ba.lastFlip), add(ba.period), add(ba.ref), add(ba.stickYaw), add(ba.blend), add(ba.lean), add(ba.side), add(ba.osc), add(ba.fall);
    for (size_t i = 0; i < s.ropeBends; ++i) add(s.ropeBend[i]), add(s.ropeBendClear[i]);
    const auto& hu = s.hurdle;
    add(hu.next), add(hu.mix), add(hu.alt), add(hu.draw), add(hu.drawOff), add(hu.edge), add(hu.out), add(hu.h), add(hu.endZ);
    add(hu.farLedge), add(hu.farSeg), add(hu.farT), add(hu.farSide);
    add(health_.value()), add(air_), add(combat_.ragdollPhase);
    for (const auto& m : combat_.ragdoll) add(m);
    part("camera");
    add(cam_.position), add(cam_.target), add(cam_.yaw), add(cam_.pitch), add(cam_.up), add(cam_.fovDeg), add(blendOut_);
    add(splineCameraActive());
    part("bones");
    for (const auto& m : bones_) add(m);
    for (size_t i = 0; i < physics_.bodies.size(); ++i) {
        const auto& b = physics_.bodies[i];
        part("body " + std::to_string(i)), add(b.pos), add(b.rot), add(b.vel), add(b.angVel);
    }
    for (size_t i = 0; i < col_.dynamics.size(); ++i) part("dynamic " + std::to_string(col_.dynamics[i].id)), add(col_.dynamics[i].transform);
    for (const auto& o : script_.objects()) {
        part("object " + std::to_string(o.uid) + " " + o.name);
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
        part("enemy " + std::to_string(e->uid()));
        add(e->uid()), add(es.mode), add(es.pos), add(es.yaw), add(es.clipTime), add(es.health), str(es.clip), add(es.ragdollPhase);
        for (const auto& m : es.ragdoll) add(m);
        for (const auto& m : es.pieces) add(m);
    }
    for (const auto& [uid, rt] : uberRuntime_) {
        part("uber " + std::to_string(uid));
        add(uid), add(rt.fired), add(rt.commanded);
        for (const auto& s : rt.sections) add(s.state), add(s.transition), add(s.time), add(s.length), add(s.clip), add(s.clipTime), add(s.rate), add(s.loop), add(s.model);
        for (const auto& [index, j] : rt.joints) add(index), add(j.x), add(j.v), add(j.lo), add(j.hi), add(j.speed), add(j.force), add(j.motor), add(j.limited);
        for (const int32_t l : rt.links) add(l);
        for (const auto& w : rt.watches) add(w.section), add(w.joint), add(w.range), add(w.mode), add(w.inside);
    }
    part("held"), add(held_);
    for (const auto& [uid, in] : inserted_) part("inserted " + std::to_string(uid)), add(uid), add(in.receptacle);
    for (const auto& [uid, x] : slides_) part("slide " + std::to_string(uid)), add(uid), add(x);
    for (const int32_t uid : placed_) part("placed " + std::to_string(uid)), add(uid);
    for (const auto& [uid, w] : waterStates_) {
        part("water " + std::to_string(uid));
        add(uid), add(w.surface), add(w.target), add(w.rate), add(w.source), add(w.sink), add(w.strength), add(w.sourceAt), add(w.sinkAt);
    }
    return out;
}

// tru.exe: a water volume instance (object `watervolume`) gets a type-10 markup box (0x5D7130, MUD flag
// 0x100000): centred on the instance, rotated with it, half extents = intro data +0x54/+0x58/+0x5C (the centre
// offsets +0x60/+0x64 are 0 in all 82 units); the water data takes bottom/top from that box (0x69D350) and the
// surface is its top [O: l2_cliffclimb's sea 2563 tops out at z 108, l7_arctic_sea's 11 volumes, l4_pool 2661.5
// under the goo plane at 2699]. The player tests its water probe box against those (0x710A20).
// Markup points: {world AABB min, world AABB max (max.z = surface), centre, half extents, (cos yaw, sin yaw, 0),
// 4 x wave (kx, ky, speed), 4 x (wave height, 0, 0), sink, source, (flow strength, radius, flat), (source id, sink id, 0)}
// (kWaterSink.. in Game.hpp). Only yaw rotations occur (l5_valaskjalf, l7_yggdrasil_end).
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
        if (vol.data->offset + 0xB0 > bytes.size()) continue;
        const auto f = [&](size_t o) { float v; std::memcpy(&v, bytes.data() + vol.data->offset + o, 4); return v; };
        const auto s16 = [&](size_t o) { int16_t v; std::memcpy(&v, bytes.data() + vol.data->offset + o, 2); return v; };
        const vec3 half(f(0x54), f(0x58), f(0x5C));
        const vec3 c(vol.position[0], vol.position[1], vol.position[2]);
        const float cs = std::cos(vol.rotation[2]), sn = std::sin(vol.rotation[2]);
        const vec3 ext(std::abs(cs) * half.x + std::abs(sn) * half.y, std::abs(sn) * half.x + std::abs(cs) * half.y, half.z);
        auto& m = out.emplace_back(collision::Markup{10, uint16_t(vol.uniqueId), {c - ext, c + ext, c, half, vec3(cs, sn, 0)}});
        // 0x6AC720 reads the hook data = intro data + 4. Depth +0x34 > 0: the surface that far over the bottom (0 in
        // every unit: the box top). Waves: +0x4E bit 1 on; +0x6C height scale; wave i: kx +0x70 + 8i, ky +0x74 + 8i, height
        // share +0x90 + 4i, speed +0xA0 + 4i (rad/s) (l1_seatop / l2_cliffclimb / l1_ship_exterior / l6_ship_exterior's
        // seas); without bit 1 the heights are 0.
        if (f(0x34) > 0) m.points[1].z = m.points[0].z + f(0x34);
        const bool waves = s16(0x4E) & 2;
        for (int i = 0; i < 4; ++i) m.points.push_back(waves ? vec3(f(0x70 + 8 * i), f(0x74 + 8 * i), f(0xA0 + 4 * i)) : vec3(0));
        for (int i = 0; i < 4; ++i) m.points.push_back({waves ? f(0x6C) * f(0x90 + 4 * i) : 0.0f, 0, 0});
        // Current (waterFlow): source / sink marker ids +0x38 / +0x3A (>= 1 on), strength +0x3C (units per 30 Hz frame), radius
        // +0x40, +0x4E bit 0 = horizontal only. A marker id names the last intro whose +0x52 is it (0x69D690); none: (0, 0, 0).
        const int16_t source = s16(0x38), sink = s16(0x3A);
        vec3 at[2]{vec3(0), vec3(0)};
        for (const auto& i : u.intros)
            for (int k = 0; k < 2; ++k)
                if (int(i.marker) == (k ? source : sink)) at[k] = vec3(i.position[0], i.position[1], i.position[2]);
        m.points.push_back(at[0]), m.points.push_back(at[1]);
        m.points.push_back({f(0x3C), f(0x40), float(s16(0x4E) & 1)});
        m.points.push_back({float(source), float(sink), 0});
    }
    return out;
}

// tru.exe 0x6ACB60 (Lara's swim velocity 0x755A00 adds it, floating bodies 0x6AD070 take it as the fluid velocity):
// toward the sink / away from the source at the strength, within `radius` of them (or of the source -> sink line)
// and everywhere for radius 0; farther out it falls off as (radius / d)^2 and, between two markers, the push from
// the source and the pull to the sink share it by the position along the line (past either end it turns back).
vec3 waterFlow(const collision::Markup& m, vec3 p) {
    if (m.points.size() <= kWaterIds) return vec3(0);
    const vec3 sink = m.points[kWaterSink], source = m.points[kWaterSource], k = m.points[kWaterFlow];
    const int src = int(m.points[kWaterIds].x), snk = int(m.points[kWaterIds].y);
    const float strength = k.x * 30.0f, radius = k.y, surface = m.points[1].z;
    if (strength == 0 || src == snk || (src < 1 && snk < 1)) return vec3(0);
    const auto falloff = [&](float d) { return radius > 0 && d > radius ? (radius / d) * (radius / d) : 1.0f; };
    const auto unit = [](vec3 v) { const float l = glm::length(v); return l > 0 ? v / l : vec3(0); };
    const auto flat = [&](vec3 v) { return surface - p.z < 64.0f ? vec3(v.x, v.y, 0) : v; }; // at the surface: no lift
    vec3 out(0);
    if (snk < 1) {
        out = unit(flat(p - source)) * strength * falloff(glm::length(p - source));
    } else if (src < 1) {
        out = unit(sink - p) * strength * falloff(glm::length(sink - p));
    } else {
        const vec3 axis = sink - source;
        const float length = glm::length(axis);
        if (length == 0) return vec3(0);
        const vec3 dir = axis / length, a = p - source, b = sink - p;
        const float along = glm::dot(a, dir), off = std::sqrt(std::max(0.0f, glm::dot(a, a) - along * along));
        if (radius <= 0 || off <= radius) {
            out = dir * strength;
        } else {
            const float s = strength * falloff(off), t1 = 1 - along / length, t2 = 1 - glm::dot(b, dir) / length;
            // the source's push is level at the surface throughout, the sink's pull only past the sink
            const vec3 push = t1 > 1 ? -s * unit(flat(a)) : t1 >= 0 ? t1 * s * unit(flat(a)) : vec3(0);
            const vec3 pull = t2 > 1 ? -s * unit(flat(b)) : t2 >= 0 ? t2 * s * unit(b) : vec3(0);
            out = push + pull;
        }
    }
    if (k.z != 0) out.z = 0;
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
