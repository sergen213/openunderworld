// Ladders (markup 9), pillars (15) and perches (13) with the clips of lara.drm's anim requests for them: ladder_*
// 1158-1198, pillar_* 616-646, perch_* 647-676 [O]. Layout observed: ladder points = {top, bottom, bottom + outward
// hint}, pillar points = {top, bottom}; perches are a short segment, 13 of the 29 pillars with one on their top
// (docs/GAMEPLAY.md "Ladders, pillars and perches").
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
float wrapAngle(float a) { return std::remainder(a, 2 * kPi); }
vec3 unitXY(vec3 v, vec3 fallback) {
    const float l = glm::length(xy(v));
    return l > 1e-4f ? vec3(v.x / l, v.y / l, 0) : fallback;
}
vec3 rotYaw(float yaw, vec3 m) { // model -Y = forward (as Player.cpp)
    const float c = std::cos(yaw + kPi / 2), s = std::sin(yaw + kPi / 2);
    return {c * m.x - s * m.y, s * m.x + c * m.y, m.z};
}
// A clip's whole root translation (model space); zero without the clip (synthetic libraries).
vec3 clipMove(const AnimLibrary& a, std::string_view clip) {
    const anim::Clip* c = a.find(clip);
    return c ? rootTranslation(*c, c->durationSeconds(), false) - rootTranslation(*c, 0, false) : vec3(0);
}

// Ladder rungs from lara.drm's player data, as tru.exe places Lara on them (0x734B70 rung count, 0x72B920 current
// rung, 0x74DCD0 the LadderRungAlignAnimTarget) [O]: rung r's root lies kRungTop + r * kRungSpacing below the top
// point, half the outward hint plus kLadderBack out from the line; rungs 0 .. trunc(trunc(length) / spacing) + 1 -
// kRungMargin (the lowest one has her feet at the bottom point: l1_ship_sinking_hall 234, 933.6 vs 940).
constexpr float kRungSpacing = 110.619f; // +0x770 (35 x 3.1605)
constexpr float kRungTop = 575.221f;     // +0x784 (ladder_detach_ground_top rises 575.4 onto the top)
constexpr float kLadderBack = 110.619f;  // -(+0x778)
constexpr int kRungMargin = 6;           // byte +0x774
int lowestRung(const collision::Markup& m) {
    const float len = glm::length(m.points[1] - m.points[0]);
    return std::max(0, int(std::trunc(std::trunc(len) / kRungSpacing)) + 1 - kRungMargin);
}
vec3 rungPos(const collision::Markup& m, int r, vec3 out) {
    const vec3 hint = m.points.size() >= 3 ? m.points[2] - m.points[1] : vec3(0);
    return m.points[0] - vec3(0, 0, kRungTop + kRungSpacing * float(r)) + vec3(xy(hint) * 0.5f, 0) + out * kLadderBack;
}
// The rung nearest to root height z (0x72B920 rounds the same way; its half-rung bias toward the motion is dropped).
int rungAt(const collision::Markup& m, float z) {
    return std::clamp(int(std::floor((m.points[0].z - z - kRungTop) / kRungSpacing + 0.5f)), 0, lowestRung(m));
}

// Pillars: the root on the axis, the clips holding her body round the shaft (tru.exe under Wine, l4_temple 1189 / 1194:
// Lara's instance stays on the axis while on a pillar) [O]; she attaches within kPillarReach of it [I]; from the floor
// she lands kPillarGroundRise up the shaft (player data short +0x7EE, the PillarAttachGroundAnimTarget 0x753F80; Wine:
// 198.6) [O]; climbing stops where pillar_detach_perch (rise 341.8) lifts her onto the top and where
// pillar_detach_ground (drop 193) sets her on the floor [I]. Wine (1194): she climbs a steady kPillarClimbSpeed and goes
// round the shaft a steady kPillarRoundRate while the stick is held.
constexpr float kPillarReach = 200.0f, kPillarGroundRise = 199.0f;
constexpr float kPillarClimbSpeed = 10.40f * 30.0f, kPillarRoundRate = 1.5708f; // units/s, rad/s
// Jumps off (state S1213): query q48, 55.63 out and 28.45 up a 30 Hz frame (lara.drm query table) [O].
constexpr float kPillarJumpSpeed = 55.63f * 30.0f, kPillarJumpVz = 28.45f * 30.0f;
// Sliding down (Wine, 1194, from idle): pillar_slide_down_start is its root motion (8.9, 26.8 then 39.9 a 30 Hz frame,
// 237 in 0.33 s); the slide loop then runs 10 a 60 Hz frame for three frames and a steady 15 (900 units/s) after [O].
constexpr float kPillarSlideFirst = 600.0f, kPillarSlideSpeed = 900.0f, kPillarSlideRamp = 2.5f / 60.0f;
constexpr float kPillarTopDrop = 341.8f, kPillarFootRise = 193.0f;
float pillarMaxZ(const collision::Markup& m) { return std::max(m.points[0].z - kPillarTopDrop, m.points[1].z + kPillarFootRise); }
float pillarMinZ(const collision::Markup& m) { return m.points[1].z + kPillarFootRise; }
// The perch (13) on a pillar's top, or -1, and the point on it nearest the axis.
int perchOnTop(const collision::CollisionMesh& col, const collision::Markup& p, vec3& at, int& seg, float& t) {
    for (size_t j = 0; j < col.markup.size(); ++j) {
        const auto& m = col.markup[j];
        if (m.type != 13) continue;
        for (size_t s = 0; s + 1 < m.points.size(); ++s) {
            const vec3 a = m.points[s], b = m.points[s + 1];
            const vec2 d = xy(b - a);
            const float len2 = glm::dot(d, d);
            const float u = len2 > 1e-4f ? std::clamp(glm::dot(xy(p.points[0] - a), d) / len2, 0.0f, 1.0f) : 0.0f;
            const vec3 q = glm::mix(a, b, u);
            if (glm::length(xy(q - p.points[0])) < 150.0f && std::abs(q.z - p.points[0].z) < 60.0f) {
                at = q, seg = int(s), t = u * std::sqrt(len2);
                return int(j);
            }
        }
    }
    return -1;
}
} // namespace

// Attach to a ladder (9) or pillar (15) within reach, facing it; climbNormal = outward from it. On the ground at a
// ladder's top, facing out over it: ladder_attach_ground_top turns her round onto the top rung.
bool Player::tryLadder(bool fromAir) {
    if (s_.regrabCooldown > 0) return false;
    const vec3 fwd = forward();
    for (size_t i = 0; i < col_.markup.size(); ++i) {
        const auto& m = col_.markup[i];
        if ((m.type != 9 && m.type != 15) || m.points.size() < 2) continue;
        const vec3 top = m.points[0], bottom = m.points[1];
        vec3 out = m.points.size() >= 3 ? unitXY(m.points[2] - bottom, vec3(0)) : unitXY(s_.pos - top, vec3(0));
        if (glm::length(out) < 0.5f) out = unitXY(s_.pos - top, vec3(1, 0, 0));
        if (m.type == 9) {
            const vec3 rung0 = rungPos(m, 0, out);
            const float fromLine = glm::dot(xy(s_.pos - top), xy(out));
            const bool atTop = !fromAir && std::abs(s_.pos.z - top.z) < 120.0f && glm::dot(fwd, out) > 0.7f && fromLine < 0 &&
                               fromLine > -250.0f && std::abs(glm::dot(xy(s_.pos - top), vec2(-out.y, out.x))) < 100.0f;
            const bool facing = glm::dot(fwd, -out) >= 0.5f && s_.pos.z >= bottom.z - 100.0f && s_.pos.z <= rung0.z + 200.0f &&
                                glm::length(xy(s_.pos - rung0)) <= 220.0f;
            if (!atTop && !facing) continue;
            s_.ledge = int(i);
            s_.climbNormal = out;
            s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw;
            s_.climbTo = rungPos(m, atTop ? 0 : fromAir || s_.pos.z > bottom.z + 200.0f ? rungAt(m, s_.pos.z) : lowestRung(m), out);
            // ladder_attach_ground_top turns her half round the way its root does (-180) [O]
            s_.climbYawTo = atTop ? s_.yaw + wrapAngle(yawOf(-xy(out)) - s_.yaw - 0.01f) : yawOf(-xy(out));
            if (atTop && s_.climbYawTo > s_.yaw) s_.climbYawTo -= 2 * kPi;
            s_.vel = vec3(0);
            s_.state = State::Ladder;
            play(atTop ? "ladder_attach_ground_top" : fromAir ? "ladder_attach_M" : "ladder_attach_ground_bottom", false);
            return true;
        }
        if (glm::dot(fwd, -out) < 0.5f) continue;
        if (s_.pos.z < bottom.z - 100.0f || s_.pos.z > top.z + 200.0f) continue;
        if (glm::length(xy(s_.pos - top)) > kPillarReach) continue;
        s_.ledge = int(i);
        s_.climbNormal = out;
        s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw, s_.climbYawTo = yawOf(-xy(out));
        s_.climbTo = vec3(xy(top), std::clamp(s_.pos.z + (fromAir ? 0.0f : kPillarGroundRise), pillarMinZ(m), pillarMaxZ(m)));
        s_.vel = vec3(0);
        s_.state = State::Pillar;
        play(fromAir ? "pillar_attach_M" : "pillar_attach_ground", false);
        return true;
    }
    return false;
}

namespace {
// Jumping up a ladder (tru.exe player chart, ladder group: jump with the stick up, T1871 / T1872 [O-data]): from the
// top kJumpUpRung + 1 rungs (0x72B920's rung <= chart variable 195 = 2; [U] the LookaheadResult 642 they exclude),
// with nothing within kJumpUpHeadroom over her (cond 0x20: variable 396 = 325 x 3.1605). With the
// top clear (cond 0x28A: player +0xEDC == 2, two 0x4935F0 sweeps: up to the ladder top + 110.62 (+0x770), then 297
// (+0x788) on over the top, both clear) ladder_jump_up_at_top leaps her off the top onto the floor up there, else
// ladder_jump_up leaps with query q23 (80.15 up a 30 Hz frame, gravity 5.72443, lara.drm query table) onto the ledge its
// arc meets, or straight up. Climbing off the top (T1817) needs the same clear top [O-data]; with it blocked, the stick
// up on the top rung leans into the jump (ladder_idle_to_jump_up_prepare, the ladder_jump_up_prepare loop, released:
// ladder_jump_up_prepare_to_idle) [I: which ladder-group request plays them is not decoded].
constexpr int kJumpUpRung = 2;
constexpr float kJumpUpHeadroom = 325.0f * 3.1605f, kTopOver = 297.0f, kLadderJumpVz = 80.15f;
bool topClear(const collision::CollisionMesh& col, const collision::Markup& m, vec3 root, vec3 out) {
    const float z = m.points[0].z + kRungSpacing;
    return !col.raycast(root, {0, 0, 1}, std::max(z - root.z, 1.0f)) && !col.raycast(vec3(xy(root), z), -out, kTopOver);
}
} // namespace

// On a ladder: rung to rung (ladder_climb_up / _down, one rung per clip, carried onto the rung as the exe's rung-align
// anim target does), off the top / bottom rung onto the floor, jump up / back, let go.
void Player::ladder(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 out = s_.climbNormal;
    const std::string clip = s_.clip;
    if (clip == "ladder_attach_M" || clip == "ladder_attach_ground_bottom" || clip == "ladder_attach_ground_top" ||
        clip == "ladder_climb_up" || clip == "ladder_climb_down" || clip == "ladder_detach_ground_top" ||
        clip == "ladder_detach_ground_bottom" || clip == "ladder_jump_up_at_top") {
        if (!warpStep(dt)) return;
        if (clip.starts_with("ladder_detach") || clip == "ladder_jump_up_at_top") {
            s_.ledge = -1;
            s_.regrabCooldown = 0.3f;
            if (clip == "ladder_jump_up_at_top" && !col_.groundProbe(s_.pos + vec3(0, 0, 50), 100)) { startFall(vec3(0)); return; }
            s_.state = State::Idle;
            play("ng_idle", true);
            return;
        }
        play("ladder_idle", true);
    }
    const float climb = glm::dot(vec3(move, 0), -out);
    const int r = rungAt(m, s_.pos.z);
    const auto start = [&](std::string_view c, vec3 to, float yawTo) {
        s_.climbFrom = s_.pos, s_.climbTo = to, s_.climbYawFrom = s_.yaw, s_.climbYawTo = yawTo;
        play(c, false);
    };
    if (jumpPressed && climb > 0.5f) { // the stick up: jump up, else nothing (no jump back, T1868)
        if (r > kJumpUpRung || col_.raycast(s_.pos, {0, 0, 1}, kJumpUpHeadroom)) return;
        s_.regrabCooldown = 0.3f;
        if (topClear(col_, m, s_.pos, out)) { // onto the floor at the top: the clip's root motion, its end on the floor
            vec3 to = s_.pos + rotYaw(s_.yaw, clipMove(anims_, "ladder_jump_up_at_top"));
            if (const auto g = col_.groundProbe(vec3(xy(to), m.points[0].z + 300.0f), 600.0f)) to.z = g->point.z;
            start("ladder_jump_up_at_top", to, s_.yaw);
            return;
        }
        const JumpTarget j = ledgeOnArc(s_.pos, vec3(0, 0, kLadderJumpVz), 40.0f, xy(-out));
        s_.fallStartZ = s_.pos.z;
        play("ladder_jump_up", false);
        if (j.markup >= 0) {
            s_.ledge = j.markup, s_.ledgeSeg = j.seg, s_.ledgeT = j.t, s_.ledgeSide = j.side, s_.polePara = false;
            s_.state = State::HangJump;
            launchHangJump(kLadderJumpVz * 30.0f);
            return;
        }
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = vec3(0, 0, kLadderJumpVz * 30.0f);
        return;
    }
    if (jumpPressed) { // ladder_jump_back turns her away from the ladder; the leap's speeds are [I]
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = out * 500.0f + vec3(0, 0, 900.0f);
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.3f;
        play("ladder_jump_back", false);
        return;
    }
    if (dropPressed) {
        s_.regrabCooldown = 0.3f;
        startFall(out * 100.0f);
        play("ladder_detach_drop", false);
        return;
    }
    const bool leaning = clip == "ladder_idle_to_jump_up_prepare" || clip == "ladder_jump_up_prepare";
    if (climb > 0.5f && r == 0 && !topClear(col_, m, s_.pos, out)) { // blocked top: lean into the jump
        if (!leaning) play("ladder_idle_to_jump_up_prepare", false);
        else if (clip == "ladder_idle_to_jump_up_prepare" && clipDone()) play("ladder_jump_up_prepare", true);
        advanceClip(dt);
        return;
    }
    if (leaning || (clip == "ladder_jump_up_prepare_to_idle" && !clipDone())) {
        if (leaning) play("ladder_jump_up_prepare_to_idle", false);
        advanceClip(dt);
        return;
    }
    if (climb > 0.5f) {
        if (r > 0) start("ladder_climb_up", rungPos(m, r - 1, out), s_.yaw);
        else start("ladder_detach_ground_top", s_.pos + rotYaw(s_.yaw, clipMove(anims_, "ladder_detach_ground_top")), s_.yaw);
        return;
    }
    if (climb < -0.5f) {
        if (r < lowestRung(m)) start("ladder_climb_down", rungPos(m, r + 1, out), s_.yaw);
        else start("ladder_detach_ground_bottom", s_.pos + rotYaw(s_.yaw, clipMove(anims_, "ladder_detach_ground_bottom")), s_.yaw - kPi);
        return;
    }
    play("ladder_idle", true);
    advanceClip(dt);
    s_.pos = rungPos(m, r, out);
}

// On a pillar: climb up (pillar_climb_up), slide down (pillar_slide_down_start / _down / _stop), round the shaft
// (pillar_climb_right, mirrored to the left: requests 623 / 624 [O]), onto a perch on its top (pillar_detach_perch),
// off at its foot (pillar_detach_ground), jump back (pillar_jump_back), let go (pillar_detach_drop).
void Player::pillar(vec2 move, vec2 stick, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 axis(xy(m.points[0]), 0);
    const std::string clip = s_.clip;
    if (clip == "pillar_attach_M" || clip == "pillar_attach_ground" || clip == "pillar_attach_perch" || clip == "pillar_climb_up" ||
        clip == "pillar_detach_perch" || clip == "pillar_detach_ground") {
        if (!warpStep(dt)) return;
        if (clip == "pillar_detach_ground") {
            s_.state = State::Idle;
            s_.ledge = -1;
            s_.regrabCooldown = 0.3f;
            play("ng_idle", true);
            return;
        }
        if (clip == "pillar_detach_perch") {
            vec3 at{0};
            int seg = 0;
            float t = 0;
            const int p = perchOnTop(col_, m, at, seg, t);
            s_.state = State::Perch;
            s_.ledge = p, s_.ledgeSeg = seg, s_.ledgeT = t;
            play("perch_idle", true);
            return;
        }
        play("pillar_idle", true);
    }
    vec3 out = s_.climbNormal;
    if (jumpPressed) {
        // The stick's angle from straight out picks the jump (state S1213, conds 0x1A9 / 0x1AA in degrees): within 22
        // pillar_jump_forward, to 78 the same mirrored on the left, to 112 pillar_jump_right, to 155
        // pillar_jump_back_45R (left: mirrored), beyond or no stick pillar_jump_back (S1215) [O]; which side mirrors
        // is [I]. She leaves along the stick (no stick: straight out) at q48's speeds.
        const float mag = glm::length(move);
        const vec2 dir = mag > 0.5f ? move / mag : xy(out);
        const float a = glm::degrees(std::atan2(xy(out).x * dir.y - xy(out).y * dir.x, glm::dot(xy(out), dir)));
        const float deg = std::abs(a);
        const bool back = mag <= 0.5f || deg > 155.0f;
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = vec3(dir * kPillarJumpSpeed, kPillarJumpVz);
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.3f;
        if (back) {
            play("pillar_jump_back", false); // its root turns her half round in the air
        } else {
            s_.yaw = yawOf(dir);
            play(deg <= 78.0f ? "pillar_jump_forward" : deg <= 112.0f ? "pillar_jump_right" : "pillar_jump_back_45R", false, 1, a > 22.0f);
        }
        return;
    }
    if (dropPressed) {
        s_.regrabCooldown = 0.3f;
        startFall(out * 100.0f);
        play("pillar_detach_drop", false);
        return;
    }
    // Screen-relative like tru.exe's stick quadrant (cond 0x195): up climbs, down slides, right / left go round
    // (Wine, l4_temple's fixed camera: W climbed whichever way she faced) [O].
    const float climb = stick.y, side = stick.x;
    const float maxZ = pillarMaxZ(m), minZ = pillarMinZ(m);
    // the clips at the measured rates: pillar_climb_up rises clipMove per clip, pillar_climb_right turns 1.19 in 0.7 s
    const anim::Clip* up = anims_.find("pillar_climb_up");
    const float kClimbRate = up ? float(up->durationSeconds()) * kPillarClimbSpeed / clipMove(anims_, "pillar_climb_up").z : 1.0f;
    constexpr float kRoundRate = kPillarRoundRate * 0.7f / 1.19f;
    const auto warp = [&](std::string_view c, vec3 to, float yawTo) {
        s_.climbFrom = s_.pos, s_.climbTo = to, s_.climbYawFrom = s_.yaw, s_.climbYawTo = yawTo;
        play(c, false, c == "pillar_climb_up" ? kClimbRate : 1.0f);
    };
    if (clip == "pillar_climb_right") { // round the shaft: the clip's root turn, Lara kept facing the axis
        s_.pos.z += advanceRoot(dt).z;
        out = -vec3(std::cos(s_.yaw), std::sin(s_.yaw), 0);
        s_.climbNormal = out;
        s_.pos = vec3(xy(axis), s_.pos.z);
        if (!clipDone()) return;
    }
    const bool sliding = clip == "pillar_slide_down_start" || clip == "pillar_slide_down" || clip == "pillar_slide_down_stop";
    if (sliding) {
        if (clip == "pillar_slide_down_stop") {
            if (!clipDone()) {
                s_.pos.z = std::max(minZ, s_.pos.z + advanceClip(dt).z);
                return;
            }
        } else {
            if (climb < -0.5f && s_.pos.z > minZ) {
                if (clip == "pillar_slide_down_start" && clipDone()) play("pillar_slide_down", true);
                if (s_.clip == "pillar_slide_down") { // the loop's speed is the code's, not its root motion (18.3 a frame)
                    const double t0 = clipBlend("", "pillar_slide_down").startSeconds; // the loop starts at its frame 3
                    const float v = s_.clipTime < t0 + kPillarSlideRamp ? kPillarSlideFirst : kPillarSlideSpeed;
                    advanceClip(dt);
                    s_.pos.z = std::max(minZ, s_.pos.z - v * dt);
                } else {
                    s_.pos.z = std::max(minZ, s_.pos.z + advanceClip(dt).z);
                }
            } else {
                play("pillar_slide_down_stop", false);
            }
            return;
        }
    }
    if (climb > 0.5f) {
        if (s_.pos.z < maxZ - 1.0f) {
            warp("pillar_climb_up", vec3(xy(s_.pos), std::min(maxZ, s_.pos.z + clipMove(anims_, "pillar_climb_up").z)), s_.yaw);
            return;
        }
        vec3 at{0};
        int seg = 0;
        float t = 0;
        if (perchOnTop(col_, m, at, seg, t) >= 0) {
            warp("pillar_detach_perch", at, s_.yaw);
            return;
        }
    } else if (climb < -0.5f) {
        if (s_.pos.z > minZ + 1.0f) {
            play("pillar_slide_down_start", false);
            s_.pos.z = std::max(minZ, s_.pos.z + advanceClip(dt).z);
            return;
        }
        warp("pillar_detach_ground", s_.pos + rotYaw(s_.yaw, clipMove(anims_, "pillar_detach_ground")), s_.yaw + kPi);
        return;
    } else if (std::abs(side) > 0.5f) {
        play("pillar_climb_right", false, kRoundRate, side < 0);
        return;
    }
    play("pillar_idle", true);
    advanceClip(dt);
    s_.pos = vec3(xy(axis), s_.pos.z);
}

// Perch (13): a short segment to balance on, from the air or a standing jump next to it.
bool Player::tryPerch(bool fromAir) {
    if (s_.regrabCooldown > 0) return false;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 13) continue;
        for (size_t si = 0; si + 1 < m.points.size(); ++si) {
            const vec3 p0 = m.points[si], p1 = m.points[si + 1];
            const vec2 seg = xy(p1 - p0);
            const float len2 = glm::dot(seg, seg);
            if (len2 < 1e-4f) continue;
            const float u = std::clamp(glm::dot(xy(s_.pos - p0), seg) / len2, 0.0f, 1.0f);
            const vec3 q = glm::mix(p0, p1, u);
            if (glm::length(xy(s_.pos - q)) > 90.0f || s_.pos.z < q.z - 600.0f || s_.pos.z > q.z + 200.0f) continue;
            const vec2 dir = seg / std::sqrt(len2);
            vec2 out(-dir.y, dir.x);
            if (glm::dot(xy(s_.pos - q), out) < 0) out = -out;
            s_.ledge = int(li), s_.ledgeSeg = int(si), s_.ledgeT = u * std::sqrt(len2);
            s_.pos = q;
            s_.yaw = yawOf(-out);
            s_.vel = vec3(0);
            s_.state = State::Perch;
            play(fromAir ? "perch_attach_air" : "perch_attach_ground", false);
            return true;
        }
    }
    return false;
}

// On a perch: turn on the spot toward the stick (perch_turn_right_45 / _90 / _180, requests 668-670; to the left the
// same clips mirrored, requests 671-673 [O]; the angle bands are [I]), jump forward, step down onto a pillar under it
// (pillar_attach_perch), or drop.
void Player::perch(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    const std::string clip = s_.clip;
    if (clip.starts_with("perch_turn_right")) {
        advanceRoot(dt);
        if (!clipDone()) return;
        play("perch_idle", true);
    }
    if ((clip == "perch_attach_air" || clip == "perch_attach_ground") && !clipDone()) {
        advanceClip(dt);
        return;
    }
    if (jumpPressed) {
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = forward() * 600.0f + vec3(0, 0, 1200.0f);
        s_.fallStartZ = s_.pos.z;
        play("perch_jump_forward", false);
        return;
    }
    if (dropPressed) {
        for (size_t i = 0; i < col_.markup.size(); ++i) { // a pillar under the perch: down onto its top
            const auto& p = col_.markup[i];
            vec3 at{0};
            int seg = 0;
            float t = 0;
            if (p.type != 15 || p.points.size() < 2 || perchOnTop(col_, p, at, seg, t) != s_.ledge) continue;
            const vec3 out = -forward();
            s_.ledge = int(i);
            s_.climbNormal = out;
            s_.climbFrom = s_.pos, s_.climbYawFrom = s_.climbYawTo = s_.yaw;
            s_.climbTo = vec3(xy(p.points[0]), std::max(pillarMinZ(p), s_.pos.z + clipMove(anims_, "pillar_attach_perch").z));
            s_.state = State::Pillar;
            play("pillar_attach_perch", false);
            return;
        }
        s_.ledge = -1;
        startFall(vec3(0));
        play("perch_detach_drop", false);
        return;
    }
    const float a = glm::length(move) > 0.5f ? wrapAngle(yawOf(move) - s_.yaw) : 0.0f;
    if (std::abs(a) > 0.4f) {
        const float m = std::abs(a);
        play(m < 1.18f ? "perch_turn_right_45" : m < 2.36f ? "perch_turn_right_90" : "perch_turn_right_180", false, 1, m < 2.36f && a > 0);
        return;
    }
    play("perch_idle", true);
    advanceClip(dt);
}

namespace {
// Ledge (markup 1) segment helpers (x/y geometry, as Player.cpp's).
vec2 ledgeDir(const collision::Markup& m, int seg) {
    const vec2 d = xy(m.points[size_t(seg) + 1] - m.points[size_t(seg)]);
    const float l = glm::length(d);
    return l > 1e-4f ? d / l : vec2(1, 0);
}
float ledgeLen(const collision::Markup& m, int seg) { return glm::length(xy(m.points[size_t(seg) + 1] - m.points[size_t(seg)])); }
vec3 ledgePoint(const collision::Markup& m, int seg, float t) {
    const float len = ledgeLen(m, seg);
    return glm::mix(m.points[size_t(seg)], m.points[size_t(seg) + 1], len > 1e-4f ? t / len : 0.0f);
}
bool ledgeMove(const collision::Markup& m, int& seg, float& t, float ds) { // true if it stopped at an end
    t += ds;
    while (t > ledgeLen(m, seg) && size_t(seg) + 2 < m.points.size()) t -= ledgeLen(m, seg++);
    while (t < 0 && seg > 0) t += ledgeLen(m, --seg);
    const float len = ledgeLen(m, seg);
    const bool end = t < 0 || t > len;
    t = std::clamp(t, 0.0f, len);
    return end;
}
// The wall-hug shelf test, tru.exe 0x74B920 with lara.drm player data [O]: in the segment frame (0x737D60: X = the
// horizontal of p[i] - p[i+1], Z up, Y = Z x X) rays at +0x6F0 = 252 above the line from +0x6E8 = 100 on the hang
// side toward -Y (the ledge's top, the left normal of p[i] -> p[i+1]) must hit a wall within +0x6EA = 139 behind the
// line but not within +0x71C = 88.496, and floor must lie 30 below to 40 above the line 88.496 behind it (0x72BAE0).
constexpr float kShelfRayZ = 252.0f, kShelfRayBack = 100.0f, kShelfWall = 139.0f, kShelfDepth = 88.496f;
// Stepping onto a shelf: within 30 vertically and 200 horizontally of the ledge, the stick along it (query 96's rule
// 0x2BD80) [O]; "along" as within 45 degrees of the line is [I].
constexpr float kHugReachZ = 30.0f, kHugReach = 200.0f;
} // namespace

bool Player::shelfAt(int markup, int seg, float t) const {
    const auto& m = col_.markup[size_t(markup)];
    const vec2 d = ledgeDir(m, seg);
    const vec3 shelf(-d.y, d.x, 0), q = ledgePoint(m, seg, t);
    const vec3 from = q - shelf * kShelfRayBack + vec3(0, 0, kShelfRayZ);
    const auto wall = col_.raycast(from, shelf, kShelfRayBack + kShelfWall);
    if (!wall || wall->t <= kShelfRayBack + kShelfDepth) return false;
    const auto floor = col_.raycast(q + shelf * kShelfDepth + vec3(0, 0, kShelfRayZ), vec3(0, 0, -1), kShelfRayZ + 30.0f);
    const float dz = floor ? floor->point.z - q.z : -30.0f;
    return dz > -30.0f && dz < 40.0f;
}

// Walking onto a shelf along it: bbeam_walk_turn180 (request 193) turns her back to the wall, Wall_Hug_Idle (1421)
// [O: the chart's S11 / S61 / S63 / S64 -> S1249 -> S1235]. She stands kShelfDepth in from the line [I].
bool Player::tryWallHug(vec2 move) {
    const float mag = glm::length(move);
    if (s_.regrabCooldown > 0 || mag < 0.1f) return false;
    const vec2 md = move / mag;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 d = ledgeDir(m, si);
            if (std::abs(glm::dot(md, d)) < 0.7f) continue;
            const float u = glm::dot(xy(s_.pos - m.points[size_t(si)]), d), len = ledgeLen(m, si);
            if ((u < 0 && glm::dot(md, d) < 0) || (u > len && glm::dot(md, d) > 0)) continue; // walking away from its end
            const float t = std::clamp(u, 0.0f, len);
            const vec3 q = ledgePoint(m, si, t);
            if (std::abs(q.z - s_.pos.z) > kHugReachZ || glm::length(xy(q - s_.pos)) > kHugReach || !shelfAt(int(li), si, t)) continue;
            const vec3 shelf(-d.y, d.x, 0);
            s_.ledge = int(li), s_.ledgeSeg = si, s_.ledgeT = t;
            s_.climbFrom = s_.pos, s_.climbTo = q + shelf * kShelfDepth;
            s_.climbYawFrom = s_.yaw, s_.climbYawTo = s_.yaw + wrapAngle(yawOf(-xy(shelf)) - s_.yaw);
            s_.vel = vec3(0);
            s_.state = State::WallHug;
            play("bbeam_walk_turn180", false);
            return true;
        }
    }
    return false;
}

// Back to the wall on a shelf: shuffle along it (Wall_Hug_Shimmy_Left, request 1402; to the right mirrored [I: the
// chart's two shimmy states differ only in property 201]), walk off where the shelf ends (Wall_Hug_Walk_Off, 1378,
// mirrored) [O], jump forward off it (Wall_Hug_Jump_Forward, 1416; a stick jump's take-off speeds [I]). Corners
// (Shimmy_to_Concave / _Convex, 1399 / 1400), the side and twist jumps and the drop to water are not done.
void Player::wallHug(vec2 move, bool jumpPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    const std::string clip = s_.clip;
    if (clip == "bbeam_walk_turn180") {
        if (!warpStep(dt)) return;
        play("Wall_Hug_Idle", true);
    } else if (clip == "Wall_Hug_Walk_Off") {
        s_.pos += advanceRoot(dt);
        if (!clipDone()) return;
        s_.state = State::Idle;
        s_.ledge = -1;
        play("ng_idle", true);
        return;
    } else if (clip == "Wall_Hug_Jump_Forward") {
        advanceClip(dt);
        if (!clipDone()) return;
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = forward() * t_.standJumpSpeed + vec3(0, 0, t_.standMoveJumpVz);
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.3f;
        play("NG_InAir_Feet_Forward_stage1_standing", false);
        return;
    }
    if (jumpPressed) {
        play("Wall_Hug_Jump_Forward", false);
        return;
    }
    const float mag = glm::length(move);
    const float left = mag > 0.5f ? glm::dot(move, xy(rotYaw(s_.yaw, vec3(1, 0, 0)))) / mag : 0.0f; // model +X = her left
    if (std::abs(left) > 0.5f) {
        play("Wall_Hug_Shimmy_Left", true, 1, left < 0);
        const vec3 d = rotYaw(s_.yaw, advanceClip(dt));
        int seg = s_.ledgeSeg;
        float t = s_.ledgeT;
        if (ledgeMove(m, seg, t, glm::dot(xy(d), ledgeDir(m, seg))) || !shelfAt(s_.ledge, seg, t)) {
            s_.regrabCooldown = 0.5f;
            play("Wall_Hug_Walk_Off", false, 1, left > 0); // it walks off to her right
            return;
        }
        s_.ledgeSeg = seg, s_.ledgeT = t;
    } else {
        play("Wall_Hug_Idle", true);
        advanceClip(dt);
    }
    const vec2 dir = ledgeDir(m, s_.ledgeSeg);
    const vec3 shelf(-dir.y, dir.x, 0);
    s_.pos = ledgePoint(m, s_.ledgeSeg, s_.ledgeT) + shelf * kShelfDepth;
    s_.yaw = yawOf(-xy(shelf));
}

namespace {
// Rail distances are fully 3D (unlike the mostly-horizontal ledge helpers in Player.cpp).
float railLen(const collision::Markup& m, int seg) { return glm::length(m.points[size_t(seg) + 1] - m.points[size_t(seg)]); }
vec3 railDir(const collision::Markup& m, int seg) {
    const vec3 d = m.points[size_t(seg) + 1] - m.points[size_t(seg)];
    const float l = glm::length(d);
    return l > 1e-4f ? d / l : vec3(1, 0, 0);
}
vec3 railPoint(const collision::Markup& m, int seg, float t) {
    const float len = railLen(m, seg);
    return glm::mix(m.points[size_t(seg)], m.points[size_t(seg) + 1], len > 1e-4f ? t / len : 0.0f);
}
bool moveRail(const collision::Markup& m, int& seg, float& t, float ds) {
    t += ds;
    while (t > railLen(m, seg) && size_t(seg) + 2 < m.points.size()) t -= railLen(m, seg++);
    while (t < 0 && seg > 0) t += railLen(m, --seg);
    const float len = railLen(m, seg);
    const bool end = t < 0 || t > len;
    t = std::clamp(t, 0.0f, len);
    return end;
}
} // namespace

// Rail slide (markup 8): attach near the line and follow it downhill. Reach and heading tolerance are [I].
bool Player::tryRail(bool fromAir) {
    if (s_.regrabCooldown > 0 || (fromAir && s_.vel.z > 0)) return false;
    int best = -1, bestSeg = 0;
    float bestT = 0, bestSide = 1, bestDist = fromAir ? 160.0f : 120.0f; // [I]
    vec3 bestQ(0), bestDirection(0);
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 8 || m.points.size() < 2) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec3 segment = railDir(m, si);
            const float side = m.points[size_t(si) + 1].z <= m.points[size_t(si)].z ? 1.0f : -1.0f;
            const vec3 direction = segment * side;
            if (!fromAir && glm::dot(forward(), unitXY(direction, vec3(1, 0, 0))) < 0.5f) continue;
            const float len = railLen(m, si);
            if (len < 1e-4f) continue;
            const float t = std::clamp(glm::dot(s_.pos - m.points[size_t(si)], segment), 0.0f, len);
            const vec3 q = railPoint(m, si, t), delta = s_.pos - q;
            const float dist = glm::length(delta);
            if (dist < bestDist) {
                bestDist = dist, best = int(li), bestSeg = si, bestT = t, bestSide = side, bestQ = q, bestDirection = direction;
            }
        }
    }
    if (best < 0) return false;
    s_.ledge = best, s_.ledgeSeg = bestSeg, s_.ledgeT = bestT, s_.ledgeSide = bestSide;
    s_.pos = bestQ;
    s_.yaw = yawOf(xy(bestDirection));
    s_.vel = bestDirection * std::max(0.0f, glm::dot(s_.vel, bestDirection));
    s_.state = State::RailSlide;
    s_.fallStartZ = s_.pos.z;
    play("slope_slidefront", true); // [I] original slide clip; lara.drm has no rail-specific clip name
    return true;
}

void Player::rail(bool jumpPressed, bool dropPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    if (m.type != 8 || s_.ledgeSeg < 0 || size_t(s_.ledgeSeg) + 1 >= m.points.size()) { startFall(s_.vel); return; }
    const vec3 direction = railDir(m, s_.ledgeSeg) * s_.ledgeSide;
    advanceClip(dt);
    s_.fallStartZ = s_.pos.z; // the slide itself is not a damaging fall
    if (jumpPressed) { // [I] jump keeps rail momentum and adds the normal standing-jump vertical speed
        s_.ledge = -1;
        s_.regrabCooldown = 0.3f;
        s_.vel += vec3(0, 0, t_.jumpVz);
        s_.state = State::Jump;
        s_.fallStartZ = s_.pos.z;
        play("NG_InAir_Feet_Forward_stage1", false);
        return;
    }
    if (dropPressed) { // [I] drop lets go with the current rail velocity
        s_.regrabCooldown = 0.4f;
        startFall(s_.vel);
        return;
    }
    // Tangential gravity: downhill accelerates, uphill decelerates, level rail preserves speed.
    const float speed = std::max(0.0f, glm::dot(s_.vel, direction) - t_.gravity * direction.z * dt);
    s_.vel = direction * speed;
    const bool end = moveRail(m, s_.ledgeSeg, s_.ledgeT, speed * dt);
    s_.pos = railPoint(m, s_.ledgeSeg, s_.ledgeT);
    s_.yaw = yawOf(xy(direction));
    if (end) { // [I] detach with a short regrab cooldown; normal collision handles the floor or the drop
        s_.regrabCooldown = 0.4f;
        startFall(s_.vel);
    }
}

} // namespace ou::game
