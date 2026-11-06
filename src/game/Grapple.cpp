// Grapple: throw at a *_ei_grapple_* anchor (World::grappleTarget selects it), then a rope-constrained
// pendulum with stick pumping, rope ascend/descend and release with momentum; feet on a wall it becomes a
// rappel (up / down / run along the wall), and from the ground a sideways start runs onto the anchor's wall.
// The rope bends round geometry and straightens again as tru.exe's (wrapRope: 0x6C8840 per frame -> 0x6C5490); it
// swings her on its last bend. Overhangs: off a wall that recedes below her feet she drops into a free swing, and
// climbing the rope beside a wall below the pivot she gets onto it (PlayerSC_Grapple::OverhangDetected 0x73AB70).
// The original runs the swing on [GrappleRope] Havok bodies; our pendulum and rates are [I].
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
bool wallLand(std::string_view c) {
    return c.starts_with("grapple_rappel_wall_land") || c.starts_with("ng_ground_to_wallrun") || c == "grapple_rappel_free_overhang_ascend";
}
bool overhangDrop(std::string_view c) { return c.starts_with("grapple_rappel_overhang_descend"); }
// Rope bends [O tru.exe]: segments are tested shortened at both ends by min(9.4815, 0.45 x length) (0x6B2CC0), a
// new bend sits 7.90125 off the surface it touched, none within 15.8 (sqrt 249.719, 0x6A50B0) of another; a clear
// bend slides toward its neighbours' line by 15 x the clear ticks (15..200) and goes once nearer than that (0x6B3570).
constexpr float kRopeTrim = 9.481501f, kBendLift = 7.90125f, kBendApart2 = 249.719f;
// OverhangDetected [O lara.drm player data]: on a wall, nothing within +0x984 (553.1) into it +0x980 (-110.6) below
// her feet; hanging free, a wall within +0xA44 (63.2) at +0xA40 (316.1) over her root, 60..120 degrees from up, whose
// lip is more than 158.025 under the pivot.
constexpr float kOverhangDrop = -110.6195f, kOverhangReach = 553.0974f, kOverhangUp = 316.0556f, kOverhangNear = 63.2111f;

// The rope segment a -> b (shortened as above) runs into geometry; the grapple target's own body never counts.
std::optional<collision::Hit> ropeHit(const collision::CollisionMesh& col, vec3 a, vec3 b, int32_t uid) {
    vec3 d = b - a;
    const float len = glm::length(d);
    if (len < 1e-3f) return std::nullopt;
    d /= len;
    const float trim = std::min(kRopeTrim, 0.45f * len);
    auto h = col.raycast(a + d * trim, d, len - 2 * trim);
    if (h && h->dynamic >= 0 && col.dynamics[size_t(h->dynamic)].id == uid) return std::nullopt;
    return h;
}
} // namespace

vec3 ropePivot(const PlayerState& s) { return s.ropeBends ? s.ropeBend[s.ropeBends - 1u] : s.grappleAnchor; }

float ropeBentLength(const PlayerState& s) {
    float l = 0;
    vec3 p = s.grappleAnchor;
    for (int i = 0; i < s.ropeBends; ++i) l += glm::distance(p, s.ropeBend[size_t(i)]), p = s.ropeBend[size_t(i)];
    return l;
}

// tru.exe 0x6C5490 [O rules; one ray per segment instead of its 4.74-radius sphere casts, I]: first straighten from
// the hand side (each bend whose neighbours see each other slides off, then goes), then bend the pivot -> hand segment
// where it first touched between last tick's hand and this one. The total rope length stays: the free part between
// the pivot and the hand takes up what the bends add or give back.
void Player::wrapRope(vec3 oldHand, vec3 hand) {
    auto& s = s_;
    vec3 next = hand;
    for (int i = s.ropeBends - 1; i >= 0; --i) {
        const auto k = size_t(i);
        const vec3 prev = i ? s.ropeBend[k - 1] : s.grappleAnchor;
        if (ropeHit(col_, prev, next, s.grappleUid)) {
            s.ropeBendClear[k] = 0, next = s.ropeBend[k];
            continue;
        }
        s.ropeBendClear[k] = uint8_t(std::min(255, s.ropeBendClear[k] + 1));
        const float step = std::clamp(15.0f * s.ropeBendClear[k], 15.0f, 200.0f);
        const vec3 d = next - prev;
        const float l2 = glm::dot(d, d);
        const vec3 off = (l2 > 1e-6f ? prev + d * (glm::dot(s.ropeBend[k] - prev, d) / l2) : prev) - s.ropeBend[k];
        if (const float dist = glm::length(off); dist >= step) {
            s.ropeBend[k] += off * (step / dist);
            next = s.ropeBend[k];
            continue;
        }
        for (size_t j = k; j + 1 < s.ropeBends; ++j) s.ropeBend[j] = s.ropeBend[j + 1], s.ropeBendClear[j] = s.ropeBendClear[j + 1];
        --s.ropeBends;
    }
    while (s.ropeBends < std::min(anims_.grapple.bends, kRopeBends)) {
        const vec3 pivot = ropePivot(s);
        if (!ropeHit(col_, pivot, hand, s.grappleUid) || ropeHit(col_, pivot, oldHand, s.grappleUid)) break; // [I] blocked already: left
        float lo = 0, hi = 1; // where between the two hands the segment first touches
        for (int it = 0; it < 10; ++it) {
            const float mid = 0.5f * (lo + hi);
            (ropeHit(col_, pivot, glm::mix(oldHand, hand, mid), s.grappleUid) ? hi : lo) = mid;
        }
        const auto h = ropeHit(col_, pivot, glm::mix(oldHand, hand, hi), s.grappleUid);
        if (!h) break;
        const vec3 bend = h->point + h->normal * kBendLift;
        bool apart = true;
        for (int i = 0; i < s.ropeBends && apart; ++i) apart = glm::dot(bend - s.ropeBend[size_t(i)], bend - s.ropeBend[size_t(i)]) >= kBendApart2;
        if (!apart) break;
        s.ropeBend[s.ropeBends] = bend, s.ropeBendClear[s.ropeBends] = 0;
        ++s.ropeBends;
        oldHand = glm::mix(oldHand, hand, lo); // the rest of the sweep from the new pivot
    }
}

// The hand (root + kHandZ) may not get farther from the pivot than the rope's free part; the rope takes the outward
// velocity.
static void ropeConstrain(const PlayerState& s, vec3& p, vec3& vel) {
    const vec3 pivot = ropePivot(s);
    const float free = std::max(1.0f, s.ropeLength - ropeBentLength(s));
    const vec3 d = p + vec3(0, 0, kHandZ) - pivot;
    if (const float dist = glm::length(d); dist > free && dist > 1e-4f) {
        const vec3 r = d / dist;
        p = pivot + r * free - vec3(0, 0, kHandZ);
        if (const float radial = glm::dot(vel, r); radial > 0) vel -= r * radial;
    }
}

void Player::attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind) {
    s_.grappleUid = uid;
    s_.grappleAnchor = anchor;
    s_.grappleKind = kind;
    s_.ropeLength = glm::distance(anchor, s_.pos + vec3(0, 0, kHandZ));
    s_.vel = vec3(0);
    s_.ledge = -1, s_.ropeBends = 0;
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
    s_.ropeLength = 0, s_.ropeBends = 0;
    s_.state = State::Idle;
    s_.vel = vec3(0);
    play("ng_idle", true);
}

void Player::releaseGrapple(bool jump) {
    if (s_.grappleKind == GrappleKind::Mounted) { standUp(); return; } // braced on the ground
    s_.grappleUid = 0;
    s_.grappleKind = GrappleKind::Ceiling;
    s_.ropeLength = 0, s_.ropeBends = 0;
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
    vec3 p = h.point + n * t_.wallOffset;
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
    s_.ropeLength = std::clamp(s_.ropeLength - up * 300.0f * dt, anims_.grapple.minRope, anims_.grapple.maxRope);
    vec3 vel = s_.vel;
    if (std::abs(side) > std::abs(up)) { // wall run: the feet push her along the wall up to run speed, never brake [I]
        const float along = glm::dot(vel, right) * (side > 0 ? 1.0f : -1.0f), wish = std::abs(side) * kWallRunSpeed;
        if (along < wish) vel += right * (side > 0 ? 1.0f : -1.0f) * std::min(wish - along, kWallRunAccel * dt);
    }
    vel.z -= t_.gravity * dt;
    vel -= n * glm::dot(vel, n);
    if (std::abs(side) < 0.2f) vel *= std::exp(-3.0f * dt); // [I] feet friction on the wall when not running
    vec3 p = s_.pos + vel * dt;
    ropeConstrain(s_, p, vel);
    const vec3 oldHand = s_.pos + vec3(0, 0, kHandZ);
    // Overhang (OverhangDetected on the wall): climbing down, the wall recedes below her feet -> she drops off it under
    // the overhang (grapple_rappel_overhang_descend_begin, then _descend) into a free swing.
    if (up < -0.2f && !col_.raycast(p + vec3(0, 0, kOverhangDrop), -n, kOverhangReach)) {
        s_.state = State::GrappleSwing;
        s_.pos = p, s_.vel = vel;
        wrapRope(oldHand, p + vec3(0, 0, kHandZ));
        play("grapple_rappel_overhang_descend_begin", false);
        return;
    }
    // Stay on the wall (following its steps and recesses); where it ends, step onto a floor at the feet (the far
    // ledge of a wall run) or swing free again.
    const auto h = col_.raycast(p + n * 50.0f, -n, t_.wallOffset + 200.0f); // steps up to 150 deep [I]
    if (!h || h->dynamic >= 0 || std::abs(h->normal.z) > kSteep) {
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 60.0f); g && standable(p, *g)) {
            s_.pos = vec3(xy(p), g->point.z);
            standUp();
            return;
        }
        s_.state = State::GrappleSwing;
        s_.pos = p, s_.vel = vel;
        wrapRope(oldHand, p + vec3(0, 0, kHandZ));
        play("grapple_rappel_idle", true);
        return;
    }
    const vec3 hn = glm::normalize(vec3(h->normal.x, h->normal.y, 0));
    p = vec3(xy(h->point + hn * t_.wallOffset), p.z);
    s_.climbNormal = hn;
    s_.yaw = yawOf(xy(-hn));
    // A floor underfoot while descending ends it standing, except running down the arc toward the point below the
    // anchor: then the feet run along the floor too (starting from a ledge beside the anchor) [I].
    if (vel.z < 0)
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 60.0f); g && standable(p, *g)) {
            if (std::abs(side) > std::abs(up) && side * glm::dot(right, s_.grappleAnchor - p) > 0) {
                p.z = std::max(p.z, g->point.z);
                vel.z = 0;
                s_.ropeLength = std::min(s_.ropeLength, ropeBentLength(s_) + glm::distance(p + vec3(0, 0, kHandZ), ropePivot(s_))); // reel in the slack
            } else {
                s_.pos = vec3(xy(p), g->point.z);
                standUp();
                return;
            }
        }
    s_.pos = p;
    wrapRope(oldHand, p + vec3(0, 0, kHandZ));
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
        const auto h = col_.raycast(s_.pos + vec3(0, 0, 150.0f), -an, t_.wallOffset + kWallReach);
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
    ropeConstrain(s_, p, vel);
    const vec3 oldHand = s_.pos + vec3(0, 0, kHandZ);
    vel *= std::exp(-0.15f * dt); // air drag [I]
    // Feet onto a wall: rappel on it; swinging down onto a floor lands her (lets go); other contacts (overhangs, the
    // floor she stands on) stop the motion into them.
    if (const vec3 step = p - s_.pos; glm::length(step) > 1e-4f)
        if (const auto h = col_.sweepSphere(s_.pos + vec3(0, 0, t_.wallOffset), t_.wallOffset, step); h && h->dynamic < 0) {
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
    if (vel.z <= 0 && glm::length(xy(p - ropePivot(s_))) > 0.3f * (s_.ropeLength - ropeBentLength(s_)))
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 90.0f);
            g && g->normal.z >= t_.minFloorNormalZ && s_.pos.z > g->point.z + 2.0f && standable(p, *g)) {
            s_.pos = vec3(xy(p), g->point.z);
            standUp();
            return;
        }
    s_.pos = p;
    s_.vel = vel;
    wrapRope(oldHand, p + vec3(0, 0, kHandZ));

    // Off an overhang: its drop clips play out (_begin, then _descend; state chart S379 -> S378) as she swings.
    if (overhangDrop(s_.clip)) {
        if (!clipDone()) { advanceClip(dt); return; }
        if (s_.clip == "grapple_rappel_overhang_descend_begin") { play("grapple_rappel_overhang_descend", false); return; }
    }
    // Overhang above (OverhangDetected hanging free): probes at kOverhangUp over her root, front then 90 degrees round
    // each time; the lip is the top found by a ray up from 31.6 inside and 158 below the contact (else the contact).
    const auto overhangAbove = [&]() -> std::optional<collision::Hit> {
        const vec3 o = s_.pos + vec3(0, 0, kOverhangUp);
        vec2 f = xy(forward());
        for (int k = 0; k < 4; ++k, f = vec2(-f.y, f.x)) {
            const auto h = col_.raycast(o, vec3(f, 0), kOverhangNear);
            if (!h || h->dynamic >= 0 || std::abs(h->normal.z) >= 0.5f) continue; // 60..120 degrees from up
            float lip = h->point.z;
            if (const auto top = col_.raycast(h->point - h->normal * 31.605f - vec3(0, 0, 158.025f), {0, 0, 1}, 173.8275f)) lip = top->point.z;
            if (lip < ropePivot(s_).z - 158.025f) return h;
        }
        return std::nullopt;
    };
    if (a.interact) { // E climbs up the rope, Shift slides down [I bindings]
        // climbing beside a wall under the pivot she gets onto it (grapple_rappel_free_overhang_ascend, then the
        // rappel; S421 -> S314)
        if (const auto h = overhangAbove()) { rappelOnto(*h, vec3(0), "grapple_rappel_free_overhang_ascend"); return; }
        s_.ropeLength = std::max(anims_.grapple.minRope, s_.ropeLength - 300.0f * dt);
        play("grapple_rappel_free_ascend", true);
    } else if (a.walk) {
        s_.ropeLength = std::min(anims_.grapple.maxRope, s_.ropeLength + 300.0f * dt);
        play("grapple_rappel_free_descend", true);
    } else if (grounded) { // standing or walking with the rope in hand [I]
        if (glm::length(move) > 0.1f) s_.yaw = yawOf(move);
        play(glm::length(move) > 0.1f ? "ng_walk" : "ng_idle", true);
    } else {
        const vec3 vxy(vel.x, vel.y, 0);
        if (glm::length(vxy) > 80.0f) s_.yaw = yawOf(xy(vxy));
        else {
            const vec3 toPivot = ropePivot(s_) - s_.pos;
            if (glm::length(xy(toPivot)) > 1.0f) s_.yaw = yawOf(xy(toPivot));
        }
        play(glm::dot(vxy, forward()) >= 0 ? "grapple_rappel_free_swing_forward" : "grapple_rappel_free_swing_backward",
             true);
    }
}

} // namespace ou::game
