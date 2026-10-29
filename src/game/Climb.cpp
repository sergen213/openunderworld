// Free climbing on climbable rock (lara.drm DICE_B_* clips): attach from the ground or the air,
// climb with the stick, top out, jump off or drop. Rock is climbable only where the level places free-climb
// holds (markup types 16-20, docs/GAMEPLAY.md "Free climbing"); speeds and reach are ours [I], the settle
// clips have no root motion.
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
constexpr float kSpeed = 240.0f;   // units/s along the wall [I]
constexpr float kReach = 90.0f;    // probe past the capsule radius [I]
constexpr float kHoldReach = 350.0f; // chest (root + 300) to hold centroid; holds sit 100-250 apart [I]

// Free-climb holds [O]: markup types 16-20 (MUD 0x8000 + one of 0x4000..0x400), 3-point triangles 20-35
// units off the rock. tru.exe disables 0x8000 markup unless it has exactly 3 points (0x5D6F00) and places
// Lara at point 0 - 158.025 z when attaching to one (0x707C60). A hold counts when it is within reach of the
// chest and on this wall (near the plane through `wall` with normal `n`), not one behind a thin wall.
bool holdNear(const collision::CollisionMesh& col, vec3 chest, vec3 wall, vec3 n) {
    for (const auto& m : col.markup) {
        if (m.type < 16 || m.type > 20 || m.points.size() != 3) continue;
        const vec3 c = (m.points[0] + m.points[1] + m.points[2]) / 3.0f;
        if (std::abs(glm::dot(c - wall, n)) < 120.0f && glm::length(c - chest) < kHoldReach) return true;
    }
    return false;
}
} // namespace

// Attach to a climbable wall ahead of Lara (chest-height probe, facing it, nearly vertical).
bool Player::tryClimb(bool fromAir) {
    if (s_.regrabCooldown > 0) return false;
    if (fromAir) { // a ledge in grab range wins: do not preempt the arc onto it
        const vec3 hand = s_.pos + toWorld(anims_.hangHandOffset);
        for (const auto& m : col_.markup) {
            if (m.type != 1) continue;
            for (size_t si = 0; si + 1 < m.points.size(); ++si) {
                const vec3 p0 = m.points[si], p1 = m.points[si + 1];
                const vec2 seg = xy(p1 - p0);
                const float len2 = glm::dot(seg, seg);
                if (len2 < 1e-4f) continue;
                const float u = std::clamp(glm::dot(xy(hand - p0), seg) / len2, 0.0f, 1.0f);
                const vec3 q = glm::mix(p0, p1, u);
                if (glm::length(xy(hand - q)) < 300.0f && std::abs(q.z - hand.z) < 600.0f) return false;
            }
        }
    }
    const vec3 fwd = forward();
    const auto h = col_.raycast(s_.pos + vec3(0, 0, 300), fwd, t_.radius + kReach);
    if (!h || h->dynamic >= 0) return false;
    if (std::abs(h->normal.z) > 0.35f || !holdNear(col_, s_.pos + vec3(0, 0, 300), h->point, h->normal)) return false;
    if (glm::dot(fwd, -h->normal) < 0.6f) return false;
    s_.climbFace = h->face;
    s_.climbNormal = h->normal;
    s_.yaw = yawOf(xy(-h->normal));
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.state = State::Climb;
    vec3 p = h->point + h->normal * t_.radius;
    p.z = s_.pos.z; // the attach clip supplies the rise
    s_.pos = p;
    play(fromAir ? "DICE_B_air_attach_M" : "DICE_B_ground_attach", false);
    return true;
}

void Player::climb(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.climbFace == UINT32_MAX || s_.climbFace >= col_.faces.size()) { startFall(s_.vel); return; }
    const vec3 n = s_.climbNormal;
    const vec3 fwd = -n; // into the wall
    const bool attaching = s_.clip == "DICE_B_air_attach_M" || s_.clip == "DICE_B_ground_attach";
    if (!attaching && jumpPressed) { // leap away from the wall
        s_.climbFace = UINT32_MAX;
        s_.state = State::Jump;
        s_.vel = n * 700.0f + vec3(0, 0, 1400.0f);
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.4f;
        play("DICE_B_Jump_Back", false);
        return;
    }
    if (!attaching && dropPressed) {
        s_.climbFace = UINT32_MAX;
        s_.regrabCooldown = 0.4f;
        play("DICE_B_Drop", false);
        startFall(n * 150.0f);
        return;
    }

    const vec3 prev = s_.pos;
    vec3 delta(0);
    if (attaching) {
        delta = toWorld(advanceClip(dt));
        if (clipDone()) play("DICE_B_Idle", true);
    } else {
        // The stick is a world-space horizontal wish (as on the ground): its component into the wall
        // climbs up (away = down), its component along the wall moves sideways.
        const vec3 right = glm::normalize(glm::cross(vec3(0, 0, 1), fwd));
        const vec3 wish(move.x, move.y, 0);
        const float side = glm::dot(wish, right), up = glm::dot(wish, fwd);
        delta = right * (side * kSpeed * dt) + vec3(0, 0, up * kSpeed * dt);
        if (std::abs(up) >= std::abs(side) && std::abs(up) > 0.1f)
            play(up > 0 ? "DICE_B_Settle_up" : "DICE_B_Settle_down", true);
        else if (std::abs(side) > 0.1f)
            play(side > 0 ? "DICE_B_Settle_right" : "DICE_B_Settle_Left", true);
        else
            play("DICE_B_Idle", true);
        advanceClip(dt); // the settle clips have no root motion; kSpeed moves the root
    }
    s_.pos = slide(s_.pos, delta, t_.stepUp, true);

    // Stay on the rock: probe back into the wall at chest height (at the feet the probe grazes the floor), follow
    // it (also around corners) while holds are in reach, or top out.
    const vec3 chest = s_.pos + vec3(0, 0, 300);
    const auto h = col_.raycast(chest + n * 20.0f, -n, t_.radius + kReach + 40.0f);
    if (h && h->dynamic < 0 && std::abs(h->normal.z) <= 0.35f && holdNear(col_, chest, h->point, h->normal)) {
        s_.climbFace = h->face;
        s_.climbNormal = h->normal;
        s_.yaw = yawOf(xy(-h->normal));
        const vec3 p = h->point + h->normal * t_.radius;
        s_.pos.x = p.x, s_.pos.y = p.y; // pinned at capsule distance from the rock
        return;
    }
    // No holds here: a standable floor above the feet, just into the wall = climb onto it (not the floor at the
    // wall's foot, found through the rock when the holds end sideways).
    if (const auto top = col_.groundProbe(s_.pos + fwd * (t_.radius + 100.0f) + vec3(0, 0, 450), 550.0f);
        top && top->normal.z >= t_.minFloorNormalZ && top->point.z > prev.z + 10.0f) {
        s_.pos = top->point + n * t_.radius;
        s_.climbFace = UINT32_MAX;
        s_.state = State::Idle;
        play("ng_idle", true);
        return;
    }
    s_.pos = prev; // the rock ended: stop at the edge instead of falling off it
}

} // namespace ou::game
