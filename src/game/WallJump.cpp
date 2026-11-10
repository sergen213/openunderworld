// Chimney jumps: a free jump that meets a wall plants her feet on it (wall_jump_attach_air, wall_jump_idle); a jump
// press there turns her round (wall_jump_jump_back) and leaps back off it, higher, toward the wall behind; without
// one she lets go (wall_jump_detach_fall). docs/GAMEPLAY.md "Chimney jumps".
//
// tru.exe [O]: the player chart's air states under S288 (every free jump's flight, S294 / S306, and S293) take T2348
// to S1291 (wall_jump_attach_air, request 1153) on PSCCF_WallJumpAttach (cond 0x203, 0x7633E0 -> 0x7594C0 == 2) while
// the jump has no markup target (cond 0x262 kind 600 = 0). S1291's anim target 0xEB (0x758BB0) is the wall hit of the
// lower ray + 110 (player data int +0x83C) along the wall's normal at her own height, facing the wall. Its clip done:
// S1294 wall_jump_idle (1155), which after chart variable 208 = 12 (state time, cond 0x14) goes to S1296
// wall_jump_detach_fall (1157) and on into the fall S1006. From S1291 and S1294 the jump button (cond 0x98 / 136) with
// PSCCF_WallJumpAllowed (cond 0x21C, 0x7660D0: chart context +0x4EC < player data byte +0x843 = 5) takes T2352 to S1295
// wall_jump_jump_back (1156), which counts the jump (property 917, action 0x75E350), runs jump query q12 (property 541;
// lara.drm query table: 46.81 along direction source 0x13 = the stored wall normal + 77.27 up a 30 Hz frame, start
// 79.01 out, cone 90, record 0x2A9EC, ledge target types 1 at the start and 3 372.95 over it) and, its clip done (a
// half turn, no root translation), hands over to S289, the same flight dispatch every jump uses.
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
float wrapAngle(float a) { return std::remainder(a, 2 * kPi); }
// The pelvis (segment 1, whose position 0x758DD0 casts from) over the root where the library has no skeleton [I].
constexpr float kPelvis = 300.0f;
// wall_jump_attach_air is 3 frames and request 1153 (blend 4, start 1) starts it 3 frames before its first (clipBlend):
// six frames onto the wall [O-data; tru.exe under Wine: 0.200 / 0.216 / 0.200 s, chimney4-wine.txt h1].
constexpr float kAttachSeconds = 6.0f / 30.0f;
constexpr float kQueryOut = 79.0139f, kQueryHang = 372.9456f; // q12's target type offsets (+0x8C) [O-data]
} // namespace

// tru.exe 0x7594C0 returning 2. Its rays (0x4935F0, mask 0x2C19E7) run from the pelvis (segment 1's position), raised
// by +0x824 and then lowered by +0x828, to where 0x758DD0 expects it +0x821 = 3 frames on: along her heading by 3 x the
// length of the vector at player +0xA20, and 2.862215 x 3^2 down. +0xA20 is the velocity the jump was launched with
// (the launch function before 0x7522A0 stores it and hands it to 0x70E050; the flight's own velocity is +0x2A0), so
// the reach does not shrink as she slows: 271 for a jump back off a wall (46.81 + 77.27), 192 for the stick jump and
// the leap off a hang [O]. tru.exe under Wine (chimney4-wine.txt h1): the rebounds take the next wall from 224.1 and
// 235.5 short of its face (as ours' mesh has it) and not from 247.4, 24..35 less than these rays from over her root
// reach, while her holds there are 119..131 out of the faces, not 110 [U: both; her pelvis in the flight's clips is
// within 1 of the root's line here, up to 31 behind it later in NG_InAir_Feet_Forward_stage1]. Ours casts from over
// the root. A terrain face with client flag 0x80 is no wall for it (0x5D3BA0; 45 % of the
// faces of the game's units carry it); 0x758F70 wants the wall within 7 degrees of vertical, her horizontal velocity
// (her heading when she has none) within 45 and her heading within 65 degrees of square to it.
bool Player::tryWallJump() {
    if (s_.vel.z < t_.wallJumpMinVz || s_.vel.z > t_.wallJumpMaxVz) return false;
    const vec3 f = forward();
    const float n = t_.wallJumpLook, frameG = t_.gravity / 900.0f;
    // the launch velocity: the take-off's horizontal part (airBrake keeps it) and the vertical before gravity
    const float launch = glm::length(vec3(s_.jumpVel0, s_.vel.z + t_.gravity * s_.jumpTime));
    const vec3 ahead = f * (n * launch / 30.0f) - vec3(0, 0, 0.5f * frameG * n * n);
    const float len = glm::length(ahead);
    if (len < 1e-3f) return false;
    const vec3 pelvis = s_.pos + vec3(0, 0, anims_.bind.size() > 1 ? anims_.bind[1].z : kPelvis);
    // tru.exe tests the velocity the jump was given (+0x2A0 keeps its 40 a frame forward while she is against the
    // wall: Wine, l2_junglehub); ours has had its horizontal part clipped by the contact by then, leaving a remnant
    // along the wall. Under 10 units a frame (every jump with a horizontal launch has 40 or more) her heading counts,
    // as it does for no velocity at all [I: the threshold].
    vec2 motion = xy(s_.vel);
    motion = glm::length(motion) > 300.0f ? glm::normalize(motion) : xy(f);
    const auto wall = [&](float h) -> std::optional<collision::Hit> {
        auto hit = col_.raycast(pelvis + vec3(0, 0, h), ahead / len, len);
        if (!hit) return std::nullopt;
        if (hit->dynamic < 0 && hit->face < col_.faces.size() && (col_.faces[hit->face].clientFlags & 0x80)) return std::nullopt;
        const vec3 in = -hit->normal;
        if (std::abs(in.z) > std::sin(glm::radians(t_.wallJumpSlope))) return std::nullopt;
        if (glm::dot(xy(in), motion) < std::cos(glm::radians(t_.wallJumpVelCone))) return std::nullopt;
        if (glm::dot(xy(in), xy(f)) < std::cos(glm::radians(t_.wallJumpFaceCone))) return std::nullopt;
        return hit;
    };
    if (!wall(t_.wallJumpHigh)) return false;
    const auto low = wall(t_.wallJumpLow);
    if (!low) return false;
    if (col_.raycast(s_.pos, vec3(0, 0, -1), t_.wallJumpFloor)) return false;         // 0x7591E0
    if (glm::length(xy(low->point) - xy(s_.jumpStart)) > t_.wallJumpReach) return false; // 0x758B00
    const vec2 out = glm::normalize(xy(low->normal));
    s_.climbNormal = vec3(out, 0);
    s_.climbFrom = s_.pos, s_.climbTo = vec3(xy(low->point) + out * t_.wallJumpStandoff, s_.pos.z);
    s_.climbYawFrom = s_.yaw, s_.climbYawTo = yawOf(-out);
    s_.vel = vec3(0);
    s_.ledge = -1, s_.ledgeSeg = 0, s_.ledgeT = 0, s_.ledgeSide = 1; // no ledge: what a save keeps of one (Save.cpp)
    s_.wallTime = 0;
    s_.state = State::WallJump;
    play("wall_jump_attach_air", false);
    return true;
}

// q12 from where she holds on: the root's arc starts 79.01 out along the wall's normal, the hands' 372.95 over that.
Player::JumpQuery Player::wallJumpQuery() const {
    const vec2 out = xy(s_.climbNormal);
    JumpQuery q;
    q.base = s_.pos;
    q.start = q.root = s_.pos + vec3(out * kQueryOut, 0), q.hang = q.start + vec3(0, 0, kQueryHang);
    q.v = vec3(out * (t_.wallJumpOut / 30.0f), t_.wallJumpVz / 30.0f), q.dir = out, q.cone = glm::radians(90.0f);
    // Ours [I]: ledges out of sight do not count. The query's capsule (radius 316) reaches through l2_junglehub's 320
    // thick column to ledge 804 behind it, which the original's climb there plainly does not go for.
    q.record = JumpRecord::HangLadder, q.classify = true, q.sight = true;
    return q;
}

void Player::wallJump(bool jumpPressed, float dt) {
    if (s_.clip == "wall_jump_jump_back") { // the half turn on the wall, then the leap
        advanceRoot(dt);
        if (!clipDone()) return;
        const vec2 out = xy(s_.climbNormal);
        s_.yaw = yawOf(out);
        s_.fallStartZ = s_.pos.z;
        s_.jumpStart = s_.pos, s_.jumpTime = 0, s_.jumpAimed = false;
        // not back to the ledge these jumps began on: ledgeOnArc leaves out the one in `ledge`
        s_.ledge = -1;
        for (size_t li = 0; s_.jumpFromLedge && li < col_.markup.size() && s_.ledge < 0; ++li) {
            const auto& m = col_.markup[li];
            for (size_t si = 0; m.type == 1 && si + 1 < m.points.size(); ++si) {
                const vec3 a = m.points[si], ab = m.points[si + 1] - a;
                const float l2 = glm::dot(ab, ab);
                const float u = l2 > 1e-6f ? std::clamp(glm::dot(s_.jumpLedge - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
                if (glm::distance(a + ab * u, s_.jumpLedge) < 2.0f) { s_.ledge = int(li); break; }
            }
        }
        if (const JumpTarget j = ledgeOnArc(wallJumpQuery()); j.markup >= 0) { // onto the ledge the query met (S289)
            s_.ledge = j.markup, s_.ledgeSeg = j.seg, s_.ledgeT = j.t, s_.ledgeSide = j.side, s_.polePara = false;
            s_.state = State::HangJump;
            launchHangJump(t_.wallJumpVz);
            return;
        }
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = vec3(out * t_.wallJumpOut, t_.wallJumpVz);
        return;
    }
    if (s_.clip == "wall_jump_detach_fall") { // off the wall: the clip's root motion (33.7 back, 154.4 down), then the fall
        const vec3 d = advanceRoot(dt);
        if (const auto g = d.z < 0 ? col_.groundProbe(s_.pos + vec3(0, 0, t_.radius), t_.radius - d.z) : std::nullopt) {
            s_.pos.z = g->point.z;
            startFall(vec3(0));
            return;
        }
        s_.pos += d;
        if (!clipDone()) return;
        startFall(vec3(0));
        return;
    }
    s_.wallTime += dt;
    const float u = std::min(1.0f, s_.wallTime / kAttachSeconds);
    s_.pos = glm::mix(s_.climbFrom, s_.climbTo, u);
    s_.yaw = wrapAngle(s_.climbYawFrom + wrapAngle(s_.climbYawTo - s_.climbYawFrom) * u);
    advanceClip(dt);
    // tru.exe under Wine (chimney4-wine.txt h1): a press while she is still drawn onto the wall does nothing, and is
    // not kept for later (Jump held from 23 ms before the idle began: no jump); the jump comes from the idle [O-runtime].
    if (jumpPressed && u >= 1 && int(s_.wallJumps) < t_.wallJumpMax) {
        ++s_.wallJumps;
        s_.pos = s_.climbTo, s_.yaw = s_.climbYawTo;
        play("wall_jump_jump_back", false);
        return;
    }
    if (u >= 1 && s_.clip == "wall_jump_attach_air") play("wall_jump_idle", true);
    if (s_.wallTime >= kAttachSeconds + t_.wallJumpHold) play("wall_jump_detach_fall", false);
}

} // namespace ou::game
