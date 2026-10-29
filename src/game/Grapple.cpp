// Grapple: throw at a *_ei_grapple_* anchor (World::grappleTarget selects it), then a rope-constrained
// pendulum with stick pumping, rope ascend/descend and release with momentum; feet on a wall it becomes a
// rappel (up / down / run along the wall), and from the ground a sideways start runs onto the anchor's wall.
// The original uses state-chart conditions plus [GrappleRope] bodies; our pendulum and rates are [I].
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
constexpr float kHandZ = 400.0f; // rope attaches this far above the root [I]
constexpr float kSteep = 0.35f;  // |normal.z| of a wall Lara can rappel on (as free climbing)
constexpr float kWallRunSpeed = 700.0f, kWallRunLift = 350.0f; // sideways start from the ground [I]
constexpr float kWallRunAccel = 2500.0f; // feet driving along the wall while the stick is held sideways [I]
constexpr float kWallReach = 400.0f;     // ground start: the anchor's wall within this of Lara's capsule [I]
bool wallLand(std::string_view c) { return c.starts_with("grapple_rappel_wall_land") || c.starts_with("ng_ground_to_wallrun"); }
} // namespace

void Player::attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind) {
    s_.grappleUid = uid;
    s_.grappleAnchor = anchor;
    s_.grappleKind = kind;
    s_.ropeLength = glm::distance(anchor, s_.pos + vec3(0, 0, kHandZ));
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.state = State::GrappleThrow;
    if (kind == GrappleKind::Mounted) {
        const float dz = anchor.z - s_.pos.z;
        play(dz > 400 ? "grapple_pull_up_light_begin" : dz < -400 ? "grapple_pull_down_light_begin"
                                                                  : "grapple_pull_mid_light_begin",
             false);
    } else {
        play("grapple_rappel_free_jumpon", false);
    }
}

// Let go of the rope standing (on the ground: after a pull, or rappelled down onto a floor) [I].
void Player::standUp() {
    s_.grappleUid = 0;
    s_.grappleKind = GrappleKind::Ceiling;
    s_.ropeLength = 0;
    s_.state = State::Idle;
    s_.vel = vec3(0);
    play("ng_idle", true);
}

void Player::releaseGrapple(bool jump) {
    if (s_.grappleKind == GrappleKind::Mounted) { standUp(); return; } // braced on the ground
    s_.grappleUid = 0;
    s_.grappleKind = GrappleKind::Ceiling;
    s_.ropeLength = 0;
    s_.state = State::Jump;
    s_.fallStartZ = s_.pos.z;
    if (jump) s_.vel.z += 400.0f;
    play(jump ? "grapple_rappel_free_jumpoff" : "NG_InAir_Feet_Forward_stage3", !jump);
}

// Feet onto wall `h` (outward normal): rappel state facing it, sideways velocity kept in the wall plane.
void Player::rappelOnto(const collision::Hit& h, vec3 vel, std::string_view clip) {
    const vec3 n = glm::normalize(vec3(h.normal.x, h.normal.y, 0));
    s_.state = State::GrappleRappel;
    s_.climbNormal = n;
    s_.yaw = yawOf(xy(-n));
    s_.vel = vel - n * glm::dot(vel, n);
    vec3 p = h.point + n * t_.radius;
    p.z = s_.pos.z;
    s_.pos = p;
    play(clip, false);
}

// On the wall: stick into the wall climbs (rope shortens), away descends, sideways runs along it; gravity keeps
// the pendulum in the wall plane. Jump kicks off into a swing, drop lets go, a floor underfoot ends it standing.
void Player::rappel(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    const vec3 n = s_.climbNormal;
    const vec3 right(-n.y, n.x, 0); // facing -n: (cos yaw, sin yaw) = -n.xy, right = (sin yaw, -cos yaw)
    if (jumpPressed) { // kick off the wall [I]
        s_.state = State::GrappleSwing;
        s_.vel += n * 700.0f + vec3(0, 0, 200.0f);
        play("grapple_rappel_free_swing_forward", true);
        return;
    }
    if (dropPressed) { releaseGrapple(false); return; }
    const float up = glm::dot(move, xy(-n)), side = glm::dot(move, xy(right));
    s_.ropeLength = std::clamp(s_.ropeLength - up * 300.0f * dt, 200.0f, 6000.0f);
    vec3 vel = s_.vel;
    if (std::abs(side) > std::abs(up)) { // wall run: the feet push her along the wall up to run speed, never brake [I]
        const float along = glm::dot(vel, right) * (side > 0 ? 1.0f : -1.0f), wish = std::abs(side) * kWallRunSpeed;
        if (along < wish) vel += right * (side > 0 ? 1.0f : -1.0f) * std::min(wish - along, kWallRunAccel * dt);
    }
    vel.z -= t_.gravity * dt;
    vel -= n * glm::dot(vel, n);
    if (std::abs(side) < 0.2f) vel *= std::exp(-3.0f * dt); // [I] feet friction on the wall when not running
    vec3 p = s_.pos + vel * dt;
    const vec3 d = p + vec3(0, 0, kHandZ) - s_.grappleAnchor;
    if (const float dist = glm::length(d); dist > s_.ropeLength && dist > 1e-4f) {
        const vec3 r = d / dist;
        p = s_.grappleAnchor + r * s_.ropeLength - vec3(0, 0, kHandZ);
        if (const float radial = glm::dot(vel, r); radial > 0) vel -= r * radial;
    }
    // Stay on the wall (following its steps and recesses); where it ends, step onto a floor at the feet (the far
    // ledge of a wall run) or swing free again.
    const auto h = col_.raycast(p + n * 50.0f, -n, t_.radius + 200.0f); // steps up to 150 deep [I]
    if (!h || h->dynamic >= 0 || std::abs(h->normal.z) > kSteep) {
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 60.0f); g && standable(p, *g)) {
            s_.pos = vec3(xy(p), g->point.z);
            standUp();
            return;
        }
        s_.state = State::GrappleSwing;
        s_.pos = p, s_.vel = vel;
        play("grapple_rappel_idle", true);
        return;
    }
    const vec3 hn = glm::normalize(vec3(h->normal.x, h->normal.y, 0));
    p = vec3(xy(h->point + hn * t_.radius), p.z);
    s_.climbNormal = hn;
    s_.yaw = yawOf(xy(-hn));
    // A floor underfoot while descending ends it standing, except running down the arc toward the point below the
    // anchor: then the feet run along the floor too (starting from a ledge beside the anchor) [I].
    if (vel.z < 0)
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 60.0f); g && standable(p, *g)) {
            if (std::abs(side) > std::abs(up) && side * glm::dot(right, s_.grappleAnchor - p) > 0) {
                p.z = std::max(p.z, g->point.z);
                vel.z = 0;
                s_.ropeLength = std::min(s_.ropeLength, glm::distance(p + vec3(0, 0, kHandZ), s_.grappleAnchor)); // reel in the slack
            } else {
                s_.pos = vec3(xy(p), g->point.z);
                standUp();
                return;
            }
        }
    s_.pos = p;
    s_.vel = vel;
    if (wallLand(s_.clip) && !clipDone()) { advanceClip(dt); return; }
    if (std::abs(up) < 0.2f && std::abs(side) < 0.2f) {
        if (!wallLand(s_.clip)) play("grapple_rappel_wall_land", false);
        return; // [I] holding still keeps the landing pose
    }
    std::string clip;
    if (std::abs(side) > std::abs(up)) // run / walk along the wall (the wall run), by stick deflection [I]
        clip = std::string(std::abs(side) > 0.6f ? "grapple_rappel_" : "grapple_rappel_walk_") + (side > 0 ? "right" : "left");
    else {
        clip = up >= 0 ? "grapple_rappel_up" : "grapple_rappel_down";
        if (side > 0.3f) clip += "_right_45";
        else if (side < -0.3f) clip += "_left_45";
    }
    play(clip, true);
    advanceClip(dt);
}

void Player::grapple(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt) {
    if (!s_.grappleUid || s_.ropeLength <= 0) { startFall(s_.vel); return; } // loaded without an anchor
    if (s_.grappleKind == GrappleKind::Mounted) {
        if (s_.state == State::GrappleThrow) {
            advanceClip(dt);
            if (clipDone()) {
                const float dz = s_.grappleAnchor.z - s_.pos.z;
                s_.state = State::GrapplePull;
                play(dz > 400 ? "grapple_pull_up_light" : dz < -400 ? "grapple_pull_down_light" : "grapple_pull_mid_light",
                     true);
            }
            return;
        }
        if (jumpPressed) { releaseGrapple(true); return; }
        if (dropPressed) { releaseGrapple(false); return; }
        s_.vel = vec3(0); // the world moves the mounted object; Lara remains braced [I]
        const float dz = s_.grappleAnchor.z - s_.pos.z;
        play(dz > 400 ? "grapple_pull_up_light" : dz < -400 ? "grapple_pull_down_light" : "grapple_pull_mid_light", true);
        return;
    }
    if (s_.state == State::GrappleRappel) { rappel(move, jumpPressed, dropPressed, dt); return; }
    // Wall run from the ground [I]: on a floor, roped to a wall anchor, with the anchor's wall (the surface closest to
    // the anchor; anchors sit a little in front of or inside it) within reach, a stick along that wall toward the point
    // below the anchor runs onto it (away from it the rope would only drag her back).
    const auto groundWallRun = [&] {
        if (s_.grappleKind != GrappleKind::Wall || glm::length(move) < 0.5f || s_.grappleAnchor.z < s_.pos.z + kHandZ) return false;
        const auto g = col_.groundProbe(s_.pos + vec3(0, 0, 50.0f), 80.0f);
        if (!g || !standable(s_.pos, *g)) return false;
        const auto a = col_.closestPoint(s_.grappleAnchor, 300.0f);
        if (!a || std::abs(a->normal.z) > kSteep) return false;
        vec3 an = glm::normalize(vec3(a->normal.x, a->normal.y, 0)); // out of the wall, on Lara's side
        if (glm::dot(an, s_.pos - a->point) < 0) an = -an;
        const auto h = col_.raycast(s_.pos + vec3(0, 0, 150.0f), -an, t_.radius + kWallReach);
        if (!h || h->dynamic >= 0 || std::abs(h->normal.z) > kSteep) return false;
        const vec3 n = glm::normalize(vec3(h->normal.x, h->normal.y, 0)), right(-n.y, n.x, 0);
        const float side = glm::dot(move, xy(right));
        if (std::abs(side) < 0.5f || side * glm::dot(xy(right), xy(s_.grappleAnchor - s_.pos)) <= 0) return false;
        rappelOnto(*h, right * (side > 0 ? kWallRunSpeed : -kWallRunSpeed) + vec3(0, 0, kWallRunLift),
                   side > 0 ? "ng_ground_to_wallrun_right" : "ng_ground_to_wallrun_left");
        return true;
    };
    if (s_.state == State::GrappleThrow) {
        advanceClip(dt);
        if (!clipDone() || groundWallRun()) return;
        s_.state = State::GrappleSwing;
        play("grapple_rappel_idle", true);
        return;
    }
    if (jumpPressed) { releaseGrapple(true); return; }
    if (dropPressed) { releaseGrapple(false); return; }
    if (groundWallRun()) return;

    vec3 vel = s_.vel;
    // Standing on a floor roped (thrown from the ground, or the rope slack): she holds the rope and walks with the
    // stick (300 u/s [I]) instead of being dragged along the floor; walking off an edge starts the swing.
    const auto feet = col_.groundProbe(s_.pos + vec3(0, 0, 20.0f), 30.0f);
    const bool grounded = feet && vel.z <= 0 && standable(s_.pos, *feet);
    if (grounded) {
        vel = vec3(move.x, move.y, 0) * 300.0f;
    } else {
        vel += vec3(move.x, move.y, 0) * 500.0f * dt; // stick pumping [I]
        vel.z -= t_.gravity * dt;
    }
    vec3 p = s_.pos + vel * dt;
    // Rope: the hand (root + kHandZ) may not get farther from the anchor than the rope is long.
    vec3 d = p + vec3(0, 0, kHandZ) - s_.grappleAnchor;
    const float dist = glm::length(d);
    if (dist > s_.ropeLength && dist > 1e-4f) {
        const vec3 n = d / dist;
        p = s_.grappleAnchor + n * s_.ropeLength - vec3(0, 0, kHandZ);
        const float radial = glm::dot(vel, n);
        if (radial > 0) vel -= n * radial; // the rope takes the outward velocity
    }
    vel *= std::exp(-0.15f * dt); // air drag [I]
    // Feet onto a wall: rappel on it; swinging down onto a floor lands her (lets go); other contacts (overhangs, the
    // floor she stands on) stop the motion into them.
    if (const vec3 step = p - s_.pos; glm::length(step) > 1e-4f)
        if (const auto h = col_.sweepSphere(s_.pos + vec3(0, 0, t_.radius), t_.radius, step); h && h->dynamic < 0) {
            if (std::abs(h->normal.z) <= kSteep && glm::dot(vel, h->normal) < 0) {
                const vec3 n = glm::normalize(vec3(h->normal.x, h->normal.y, 0)), right(-n.y, n.x, 0);
                const float side = glm::dot(vel, right);
                rappelOnto(*h, vel, side > 150.0f ? "grapple_rappel_wall_land_right"
                                    : side < -150.0f ? "grapple_rappel_wall_land_left" : "grapple_rappel_wall_land");
                return;
            }
            if (const vec3 at = s_.pos + step * h->t; h->normal.z >= t_.minFloorNormalZ && vel.z < -300.0f) // [I] landing speed
                if (const auto g = col_.groundProbe(at + vec3(0, 0, 50.0f), 80.0f); g && standable(at, *g)) {
                    s_.pos = vec3(xy(at), g->point.z);
                    standUp();
                    return;
                }
            const vec3 rest = step * (1.0f - h->t);
            p = s_.pos + step * h->t + h->normal * 0.5f + rest - h->normal * glm::dot(rest, h->normal); // slide [I]
            vel -= h->normal * std::min(0.0f, glm::dot(vel, h->normal));
        }
    // Out at the end of the arc (not hanging under the anchor), coming down from the air just over a floor: lands on
    // it, e.g. the far ledge of a wall run whose apex grazes its edge [I].
    if (vel.z <= 0 && glm::length(xy(p - s_.grappleAnchor)) > 0.3f * s_.ropeLength)
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 90.0f);
            g && g->normal.z >= t_.minFloorNormalZ && s_.pos.z > g->point.z + 2.0f && standable(p, *g)) {
            s_.pos = vec3(xy(p), g->point.z);
            standUp();
            return;
        }
    s_.pos = p;
    s_.vel = vel;

    if (a.interact) { // E climbs up the rope, Shift slides down [I bindings]
        s_.ropeLength = std::max(200.0f, s_.ropeLength - 300.0f * dt);
        play("grapple_rappel_free_ascend", true);
    } else if (a.walk) {
        s_.ropeLength = std::min(6000.0f, s_.ropeLength + 300.0f * dt);
        play("grapple_rappel_free_descend", true);
    } else if (grounded) { // standing or walking with the rope in hand [I]
        if (glm::length(move) > 0.1f) s_.yaw = yawOf(move);
        play(glm::length(move) > 0.1f ? "ng_walk" : "ng_idle", true);
    } else {
        const vec3 vxy(vel.x, vel.y, 0);
        if (glm::length(vxy) > 80.0f) s_.yaw = yawOf(xy(vxy));
        else {
            const vec3 toAnchor = s_.grappleAnchor - s_.pos;
            if (glm::length(xy(toAnchor)) > 1.0f) s_.yaw = yawOf(xy(toAnchor));
        }
        play(glm::dot(vxy, forward()) >= 0 ? "grapple_rappel_free_swing_forward" : "grapple_rappel_free_swing_backward",
             true);
    }
}

} // namespace ou::game
