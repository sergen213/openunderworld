// Grapple: throw at a *_ei_grapple_* anchor (World::grappleTarget selects it), then a rope-constrained
// pendulum with stick pumping, rope ascend/descend and release with momentum. The original uses state-chart
// conditions plus [GrappleRope] bodies; our pendulum and rates are [I] (no exe numbers decoded yet).
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
constexpr float kHandZ = 400.0f; // rope attaches this far above the root [I]
} // namespace

void Player::attachGrapple(vec3 anchor, int32_t uid) {
    s_.grappleUid = uid;
    s_.grappleAnchor = anchor;
    s_.ropeLength = glm::distance(anchor, s_.pos + vec3(0, 0, kHandZ));
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.state = State::GrappleThrow;
    play("grapple_rappel_free_jumpon", false);
}

void Player::releaseGrapple(bool jump) {
    s_.grappleUid = 0;
    s_.ropeLength = 0;
    s_.state = State::Jump;
    s_.fallStartZ = s_.pos.z;
    if (jump) s_.vel.z += 400.0f;
    play(jump ? "grapple_rappel_free_jumpoff" : "NG_InAir_Feet_Forward_stage3", !jump);
}

void Player::grapple(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt) {
    if (!s_.grappleUid || s_.ropeLength <= 0) { startFall(s_.vel); return; } // loaded without an anchor
    if (s_.state == State::GrappleThrow) {
        advanceClip(dt);
        if (clipDone()) {
            s_.state = State::GrappleSwing;
            play("grapple_rappel_idle", true);
        }
        return;
    }
    if (jumpPressed) { releaseGrapple(true); return; }
    if (dropPressed) { releaseGrapple(false); return; }

    vec3 vel = s_.vel;
    vel += vec3(move.x, move.y, 0) * 500.0f * dt; // stick pumping [I]
    vel.z -= t_.gravity * dt;
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
    s_.pos = p;
    s_.vel = vel;

    if (a.interact) { // E climbs up the rope, Shift slides down [I bindings]
        s_.ropeLength = std::max(200.0f, s_.ropeLength - 300.0f * dt);
        play("grapple_rappel_free_ascend", true);
    } else if (a.walk) {
        s_.ropeLength = std::min(6000.0f, s_.ropeLength + 300.0f * dt);
        play("grapple_rappel_free_descend", true);
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
