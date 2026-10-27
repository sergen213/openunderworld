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

} // namespace ou::game
