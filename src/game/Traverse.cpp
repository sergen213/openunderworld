// Ladders (markup 9), perches (13) and pillars (15) with their original lad_* / perch_* clips.
// Layout observed: ladder/pillar points = {top, bottom, bottom + outward hint}; perches are a short
// ledge segment. Speeds/reach are ours [I].
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
vec3 unitXY(vec3 v, vec3 fallback) {
    const float l = glm::length(xy(v));
    return l > 1e-4f ? vec3(v.x / l, v.y / l, 0) : fallback;
}
constexpr float kSpeed = 240.0f; // climb rate, units/s [I]
} // namespace

// Attach to a ladder (9) or pillar (15) within reach, facing it. climbNormal = outward from it.
bool Player::tryLadder(bool fromAir) {
    if (s_.regrabCooldown > 0) return false;
    const vec3 fwd = forward();
    for (size_t i = 0; i < col_.markup.size(); ++i) {
        const auto& m = col_.markup[i];
        if ((m.type != 9 && m.type != 15) || m.points.size() < 2) continue;
        const vec3 top = m.points[0], bottom = m.points[1];
        vec3 out = m.points.size() >= 3 ? unitXY(m.points[2] - bottom, vec3(0)) : unitXY(s_.pos - top, vec3(0));
        if (glm::length(out) < 0.5f) out = unitXY(s_.pos - top, vec3(1, 0, 0));
        if (glm::dot(fwd, -out) < 0.5f) continue;
        if (s_.pos.z < bottom.z - 100.0f || s_.pos.z > top.z + 200.0f) continue;
        if (glm::length(xy(s_.pos - vec3(top.x, top.y, s_.pos.z))) > t_.radius + 100.0f) continue;
        s_.ledge = int(i);
        s_.climbNormal = out;
        s_.pos = vec3(top.x, top.y, s_.pos.z) + out * t_.radius;
        s_.yaw = yawOf(-out);
        s_.vel = vec3(0);
        s_.state = m.type == 9 ? State::Ladder : State::Pillar;
        play("lad_attach_up", false);
        return true;
    }
    return false;
}

void Player::ladder(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 top = m.points[0], bottom = m.points[1], out = s_.climbNormal;
    // The climb-off clips finish before control returns.
    if (s_.clip == "lad_ClimbOffTop" || s_.clip == "lad_ClimbOffBot") {
        advanceClip(dt);
        if (clipDone()) {
            s_.state = State::Idle;
            s_.ledge = -1;
            play("ng_idle", true);
        }
        return;
    }
    if (jumpPressed) {
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = out * 500.0f + vec3(0, 0, 900.0f);
        s_.fallStartZ = s_.pos.z;
        play("lad_detach", false);
        return;
    }
    if (dropPressed) {
        s_.ledge = -1;
        play("lad_detach_drop", false);
        startFall(out * 100.0f);
        return;
    }
    // Into the ladder = up, away = down; sideways rotates around a pillar.
    const vec3 wish(move.x, move.y, 0);
    const float climb = glm::dot(wish, -out);
    const float side = glm::dot(wish, glm::normalize(glm::cross(vec3(0, 0, 1), -out)));
    if (climb > 0.1f) play("lad_climbup", true);
    else if (climb < -0.1f) play("lad_climbdown", true);
    else play("lad_Idle", true);
    advanceClip(dt);
    s_.pos.z += climb * kSpeed * dt;
    if (m.type == 15 && std::abs(side) > 0.1f) { // pillars: rotate around the shaft
        const vec3 c(top.x, top.y, s_.pos.z);
        const float a = std::atan2(s_.pos.y - c.y, s_.pos.x - c.x) + side * 1.2f * dt;
        const vec3 n(std::cos(a), std::sin(a), 0);
        s_.pos = c + n * t_.radius;
        s_.climbNormal = n;
        s_.yaw = yawOf(-n);
    } else {
        s_.pos.x = top.x + out.x * t_.radius, s_.pos.y = top.y + out.y * t_.radius;
    }
    if (climb > 0 && s_.pos.z >= top.z) {
        s_.pos = top;
        play("lad_ClimbOffTop", false);
    } else if (climb < 0 && s_.pos.z <= bottom.z) {
        s_.pos = bottom + out * t_.radius;
        play("lad_ClimbOffBot", false);
    }
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

void Player::perch(bool jumpPressed, bool dropPressed, float dt) {
    if (jumpPressed) {
        s_.ledge = -1;
        s_.state = State::Jump;
        s_.vel = forward() * 600.0f + vec3(0, 0, 1200.0f);
        s_.fallStartZ = s_.pos.z;
        play("perch_jump_forward", false);
        return;
    }
    if (dropPressed) {
        s_.ledge = -1;
        play("perch_detach_drop", false);
        startFall(vec3(0));
        return;
    }
    play("perch_idle", true);
    advanceClip(dt);
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
