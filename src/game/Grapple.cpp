// Grapple: throw at a *_ei_grapple_* anchor (World::grappleTarget selects it); roped she stands, walks and jumps on
// the ground, off an edge or into a gap she hangs on the rope: a rope-constrained pendulum driven by tru.exe's swing
// law (0x746F60: the stick along her facing pushes her the way she is moving), Interact + forward / back climbs the
// rope, left / right turns her; Jump leaps off. Feet on a wall it becomes a rappel (Interact + forward / back, run
// along the wall with left / right, Jump leaps left / right / back off it; 0x745FA0), and from the ground a sideways
// start runs onto the anchor's wall.
// The rope bends round geometry and straightens again as tru.exe's (wrapRope: 0x6C8840 per frame -> 0x6C5490); it
// swings her on its last bend. Overhangs: off a wall that recedes below her feet she drops into a free swing, and
// climbing the rope beside a wall below the pivot she gets onto it (PlayerSC_Grapple::OverhangDetected 0x73AB70).
// The original runs the swing on a [GrappleRope] rigid body; ours is a point pendulum under the same forces.
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
constexpr float kHandZ = 400.0f; // rope attaches this far above the root [I]
// Rope inputs and swing law [O lara.drm player data, read by tru.exe 0x746F60 (hanging free) / 0x745FA0 (on a wall) /
// 0x73C640 (jump-offs); per 30 Hz frame in the data, per second here]. Stick: dead below +0x9C8, "along her facing"
// = |cos| of its angle from her facing over 1 - sin(+0x9CC = 12.5 deg), climbing / turning over 1 - sin(32.5 deg).
constexpr float kStickDead = 0.35f, kPumpAxis = 0.78356f, kClimbAxis = 0.46270f;
constexpr float kPumpHold = 10.0f / 30.0f;   // +0x9D8: the swing stays driven this long after the stick leaves the axis
constexpr float kPumpBackCut = 150.0f / 30.0f; // +0x9DC: the stick held back this long stops driving it
constexpr float kRopeShort = 948.16693f, kRopeLong = 2844.50073f; // +0x8E4 / +0x8E8: the force table's rope lengths
// Drive acceleration along her facing by free rope length (0 -> short, short -> long, long -> longest), units/frame^2:
// moving forward +0xA10.. / moving backward +0xA18..
constexpr float kPumpFwd[3][2] = {{3.00253f, 3.16056f}, {3.00253f, 3.16056f}, {4.26675f, 4.42478f}};
constexpr float kPumpBack[3][2] = {{3.00253f, 3.00253f}, {3.31858f, 3.47661f}, {4.42478f, 4.74083f}};
constexpr float kWallRun[3][2] = {{1.89633f, 2.52845f}, {3.16056f, 3.95070f}, {5.53097f, 6.32111f}}; // on a wall, x the stick sideways: +0x95C..
constexpr float kWallMaxAngle = 0.9599311f, kWallMaxSpeed = 97.97725f * 30.0f; // +0x944 = 55 deg, +0x940
// A force table's value for the free rope length `len` of a rope `longest` long, units/s^2.
float ropeTable(const float (&tab)[3][2], float len, float longest) {
    const int seg = len < kRopeShort ? 0 : len < kRopeLong ? 1 : 2;
    const float lo = seg == 0 ? 0.0f : seg == 1 ? kRopeShort : kRopeLong, hi = seg == 0 ? kRopeShort : seg == 1 ? kRopeLong : std::max(longest, kRopeLong + 1.0f);
    return glm::mix(tab[seg][0], tab[seg][1], std::clamp((len - lo) / (hi - lo), 0.0f, 1.0f)) * 900.0f;
}
constexpr float kSwingMaxAngle = 0.7853982f; // +0xA04 = 45 deg from straight down: beyond it, going out, no drive but a brake
constexpr float kClimbLockAngle = 0.6981317f; // +0x9EC = 40 deg: no rope climbing swung out farther
constexpr float kOverBrake = 2.5f, kRestBrake = 1.0f, kSideBrake = 3.5f; // +0xA08 / +0x9E0 / +0xA0C: forces per velocity
constexpr float kBodyMass = 55.0f;  // +0x8D8: the rope body's mass (0x72E5xx -> 0x6A48B0 -> 0x6A4570); the brakes divide by it
constexpr float kBodyDamp = 0.022f; // +0x8DC: the body's own damping (0x4D8D00), per frame [I time base]
constexpr float kFreeMaxSpeed = 379.26675f * 30.0f;                         // +0xA00
constexpr float kFreeAscend = 23.70417f * 30.0f, kFreeDescend = 41.08723f * 30.0f; // +0x9E4 / +0x9E8, x stick
constexpr float kWallAscend = 34.76612f * 30.0f, kWallDescend = 44.24779f * 30.0f; // +0x924 / +0x928, x stick
constexpr float kTurnRate = 3.5f * 30.0f * 0.017453292f, kTurnRamp = 50.0f / 30.0f; // +0x9F4 deg / frame, eased in over +0x9FC frames
// Jump-offs (state chart S406 / S322 / S327 / S326, property 710 = 734 / 730 / 731): hanging free max(+0xA48, +0xA50 x
// her horizontal speed) along her facing and max(+0xA4C, +0xA54 x her vertical speed) up; off a wall to its left /
// right +0x994 along it, +0x998 up; back off it +0x9A4 out, +0x9A8 up.
constexpr float kFreeJumpFwd = 63.21113f * 30.0f, kFreeJumpUp = 47.40834f * 30.0f, kFreeJumpKeep = 0.45f, kFreeJumpKeepUp = 0.85f;
constexpr float kWallJumpSide = 45.82807f * 30.0f, kWallJumpBack = 53.72946f * 30.0f, kWallJumpUp = 44.24779f * 30.0f;
constexpr float kHangClear = 316.0f; // her jump becomes a hang with no floor this near under her feet [I: GroundDetected 0x739D70 not decoded; over a standing jump's 202]
constexpr float kRopedWalk = 300.0f; // roped on the ground she moves in the ordinary ground set (chart S705 / S722) [I speed]
constexpr float kSteep = 0.35f;  // |normal.z| of a wall Lara can rappel on (as free climbing)
constexpr float kWallRunSpeed = 700.0f, kWallRunLift = 350.0f; // sideways start from the ground [I]
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

// Feet set onto a wall (outward normal n) the taut rope may be too short for the spot: she is moved along the wall
// toward the pivot until it reaches (first order).
static void ropeOnWall(const PlayerState& s, vec3& p, vec3 n) {
    const vec3 d = ropePivot(s) - (p + vec3(0, 0, kHandZ));
    const float dist = glm::length(d), free = std::max(1.0f, s.ropeLength - ropeBentLength(s));
    const vec3 along = d - n * glm::dot(d, n);
    if (const float a = glm::length(along); dist > free && a > 1.0f) p += along * ((dist - free) * dist / (a * a));
}

void Player::attachGrapple(vec3 anchor, int32_t uid, GrappleKind kind, float ropeMax) {
    s_.grappleUid = uid;
    s_.grappleAnchor = anchor;
    s_.grappleKind = kind;
    s_.ropeMax = ropeMax;
    s_.ropeLength = glm::distance(anchor, s_.pos + vec3(0, 0, kHandZ));
    s_.vel = vec3(0);
    s_.ledge = -1, s_.ropeBends = 0;
    s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0; // the swing drive's timers (PlayerState)
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
    s_.ropeLength = 0, s_.ropeBends = 0, s_.ropeMax = 0;
    s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0;
    s_.state = State::Idle;
    s_.vel = vec3(0);
    play("ng_idle", true);
}

void Player::releaseGrapple(bool jump) {
    if (s_.grappleKind == GrappleKind::Mounted) { standUp(); return; } // braced on the ground
    if (jump) { jumpOffRope(vec2(0)); return; }
    s_.grappleUid = 0;
    s_.grappleKind = GrappleKind::Ceiling;
    s_.ropeLength = 0, s_.ropeBends = 0, s_.ropeMax = 0;
    s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0;
    s_.state = State::Jump;
    s_.fallStartZ = s_.pos.z;
    play("NG_InAir_Feet_Forward_stage3", true);
}

// Jump on the rope [O tru.exe 0x73C640, chart S406 hanging free / S320 -> S322 / S327 / S326 on a wall]: she leaps off
// with a launch of her own (the swing's speed counts only where 0.45 / 0.85 of it is more), hanging free along her
// facing, on a wall to the side the stick is held (she turns to it and leaps along the wall) or back off it. The rope
// is let go [I: the chart's jump-off states are outside its rope states; a kept rope would catch her again, T291].
void Player::jumpOffRope(vec2 move) {
    std::string_view clip = "grapple_rappel_free_jumpoff";
    vec3 vel;
    if (s_.state == State::GrappleRappel) {
        const vec3 n = s_.climbNormal, right(-n.y, n.x, 0);
        const float side = glm::dot(move, xy(right)), away = glm::dot(move, xy(n));
        const int dir = glm::length(move) >= kStickDead && std::abs(side) > std::abs(away) ? (side > 0 ? 1 : -1) : 0; // [I] 90 degree sectors
        const vec3 out = dir ? right * float(dir) : n;
        vel = out * (dir ? kWallJumpSide : kWallJumpBack) + vec3(0, 0, kWallJumpUp);
        s_.yaw = yawOf(xy(out)); // [I] at once; the clips turn her
        clip = dir > 0 ? "grapple_rappel_jumpoff_right" : dir < 0 ? "grapple_rappel_jumpoff_left" : "grapple_rappel_jumpoff_back";
        if (!anims_.find(clip)) clip = "grapple_rappel_free_jumpoff";
    } else {
        vel = forward() * std::max(kFreeJumpFwd, kFreeJumpKeep * glm::length(xy(s_.vel))) + vec3(0, 0, std::max(kFreeJumpUp, kFreeJumpKeepUp * s_.vel.z));
    }
    s_.grappleUid = 0;
    s_.grappleKind = GrappleKind::Ceiling;
    s_.ropeLength = 0, s_.ropeBends = 0, s_.ropeMax = 0;
    s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0;
    s_.jumpStart = s_.pos, s_.jumpAimed = false;
    s_.state = State::Jump;
    s_.vel = vel;
    s_.fallStartZ = s_.pos.z;
    play(clip, false);
}

// Feet onto wall `h` (outward normal): rappel state facing it, sideways velocity kept in the wall plane.
void Player::rappelOnto(const collision::Hit& h, vec3 vel, std::string_view clip) {
    const vec3 n = glm::normalize(vec3(h.normal.x, h.normal.y, 0));
    s_.state = State::GrappleRappel;
    s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0;
    s_.climbNormal = n;
    s_.yaw = yawOf(xy(-n));
    s_.vel = vel - n * glm::dot(vel, n);
    vec3 p = h.point + n * t_.wallOffset;
    p.z = s_.pos.z;
    s_.pos = p;
    ropeOnWall(s_, p, n); // the step onto the wall
    s_.pos = p;
    play(clip, false);
}

// On the wall [O inputs and climb rates, tru.exe 0x745FA0]: with Interact held the stick into the wall climbs (the rope
// shortens), away descends (where it is more up / down than sideways); without it the stick sideways runs along the
// wall; gravity keeps the pendulum in the wall plane. Jump leaps off (jumpOffRope), drop lets go, a floor underfoot
// ends it standing.
void Player::rappel(vec2 move, bool climb, bool jumpPressed, bool dropPressed, float dt) {
    const vec3 n = s_.climbNormal;
    const vec3 right(-n.y, n.x, 0); // facing -n: (cos yaw, sin yaw) = -n.xy, right = (sin yaw, -cos yaw)
    if (jumpPressed) { jumpOffRope(move); return; }
    if (dropPressed) { releaseGrapple(false); return; }
    if (glm::length(move) < kStickDead) move = vec2(0);
    float up = glm::dot(move, xy(-n)), side = glm::dot(move, xy(right));
    if (climb) side = 0, up = std::abs(up) > std::abs(glm::dot(move, xy(right))) ? up : 0.0f; // Interact: the rope only
    else up = 0; // no Interact: the wall run only
    s_.ropeLength = std::clamp(s_.ropeLength - up * (up > 0 ? kWallAscend : kWallDescend) * dt, anims_.grapple.minRope, maxRope());
    vec3 vel = s_.vel;
    // Wall run [O drive, tru.exe 0x745FA0]: the stick sideways pushes her along the wall by the rope-length table, not
    // past 55 degrees from under the pivot, where going outward she is braked; speed capped.
    {
        const vec3 ropeV = s_.pos + vec3(0, 0, kHandZ) - ropePivot(s_);
        const float len = glm::length(ropeV), angle = len > 1.0f ? std::acos(std::clamp(-ropeV.z / len, -1.0f, 1.0f)) : 0.0f;
        const bool over = angle > kWallMaxAngle, outward = glm::dot(xy(ropeV), xy(vel)) > 0;
        if (std::abs(side) > std::abs(up) && !(over && side * glm::dot(ropeV, right) > 0))
            vel += right * (side * ropeTable(kWallRun, len, maxRope()) * dt);
        if (over && outward) vel -= vel * std::min(1.0f, kOverBrake / kBodyMass * 30.0f * dt);
        if (const float sp = glm::length(vel); sp > kWallMaxSpeed) vel *= kWallMaxSpeed / sp;
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
    ropeOnWall(s_, p, hn); // into a recess
    s_.climbNormal = hn;
    s_.yaw = yawOf(xy(-hn));
    // A floor underfoot while descending, or reached running along the wall (chart T378: WallLedgeDetected ->
    // grapple_rappel_ledge_stepon), ends it standing, except running toward the point below the anchor: then the
    // feet run along the floor too (starting from a ledge beside the anchor) [I].
    if (const bool running = std::abs(side) > std::abs(up); vel.z < 0 || running)
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), running && vel.z > 0 ? 110.0f : 60.0f); g && standable(p, *g)) { // rising past a ledge: up to 60 under her [I]
            if (running && side * glm::dot(right, s_.grappleAnchor - p) > 0) {
                if (vel.z < 0) {
                    p.z = std::max(p.z, g->point.z);
                    vel.z = 0;
                    s_.ropeLength = std::min(s_.ropeLength, ropeBentLength(s_) + glm::distance(p + vec3(0, 0, kHandZ), ropePivot(s_))); // reel in the slack
                }
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
        // Roped to a pull target she still walks (the walkthrough's l1_baldrs_tomb gate: the ring is grappled, she steps
        // back onto the press plate and only then pulls with Interact): without Interact the stick walks her, the rope
        // paying out to its longest; Interact braces her and the world pulls the target (World::pullGrappled). [I] walk
        // speed and clip (the original's roped walk is not traced).
        if (!a.interact && glm::length(move) > 0.2f) {
            constexpr float kRopedWalk = 350.0f; // units/s [I]: a walk
            const vec2 dir = glm::normalize(move);
            const vec3 next = s_.pos + vec3(dir, 0) * (kRopedWalk * dt);
            const float longest = maxRope();
            if (glm::distance(next + vec3(0, 0, kHandZ), s_.grappleAnchor) <= longest) {
                s_.yaw = std::atan2(dir.y, dir.x);
                groundMove(vec3(dir, 0) * (kRopedWalk * dt), dt);
                if (s_.state != State::GrapplePull) { // walked off an edge: the rope is let go
                    s_.grappleUid = 0, s_.ropeBends = 0;
                    return;
                }
                s_.ropeLength = std::max(s_.ropeLength, glm::distance(s_.pos + vec3(0, 0, kHandZ), s_.grappleAnchor));
                play("ng_walk", true);
                advanceClip(dt);
                return;
            }
        }
        s_.vel = vec3(0); // the world moves the mounted object; Lara remains braced [I]
        const float dz = s_.grappleAnchor.z - s_.pos.z;
        play(dz > 400 ? "grapple_pull_up_light" : dz < -400 ? "grapple_pull_down_light" : "grapple_pull_mid_light", true);
        return;
    }
    if (s_.state == State::GrappleRappel) { // on the wall the stick is read in its own frame (below): forward = into the wall
        const vec3 n = s_.climbNormal;
        rappel(vec2(-n.y, n.x) * a.move.x - xy(n) * a.move.y, a.interact, jumpPressed, dropPressed, dt);
        return;
    }
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
    if (dropPressed) { releaseGrapple(false); return; }
    if (groundWallRun()) return;

    const vec3 oldHand = s_.pos + vec3(0, 0, kHandZ);
    // On a floor with the rope in hand (chart S705 / S722 grapple_idle: the ordinary ground set, the rope kept): she
    // walks with the stick, the rope following her taut [I speed, rope take-up]. Off an edge under the anchor she is
    // on the rope (T1409), and Jump takes her into the gap with it (T1407 at an edge, else the jump S752 -> S753 that
    // T1387 turns into the hang on its way down) [I: one launch, the ordinary stick jump's].
    const auto feet = col_.groundProbe(s_.pos + vec3(0, 0, 20.0f), 30.0f);
    if (feet && s_.vel.z <= 0 && standable(s_.pos, *feet)) {
        s_.flight = 0, s_.savingGrab = 0, s_.wallTime = 0;
        const float m = glm::length(move);
        const vec2 dir = m > 0.2f ? move / m : xy(forward());
        if (jumpPressed) {
            s_.yaw = yawOf(dir);
            s_.vel = m > 0.2f ? vec3(dir * t_.standJumpSpeed, t_.standMoveJumpVz) : vec3(0, 0, t_.jumpVz);
            play("grapple_rappel_free_jumpon", false);
            return;
        }
        s_.vel = vec3(0);
        if (m > 0.2f && ropeBentLength(s_) + glm::distance(s_.pos + vec3(dir * (kRopedWalk * dt), kHandZ), ropePivot(s_)) <= maxRope()) {
            s_.yaw = yawOf(dir);
            groundMove(vec3(dir, 0) * (kRopedWalk * dt), dt);
            if (s_.state != State::GrappleSwing) { // off an edge
                if (s_.state == State::Fall && ropePivot(s_).z > s_.pos.z + kHandZ) {
                    s_.state = State::GrappleSwing;
                    play("grapple_rappel_free_swing_forward", true);
                } else {
                    s_.grappleUid = 0, s_.ropeBends = 0, s_.ropeLength = 0, s_.ropeMax = 0;
                    s_.grappleKind = GrappleKind::Ceiling;
                }
                return;
            }
            play("ng_walk", true);
        } else {
            play("ng_idle", true);
        }
        wrapRope(oldHand, s_.pos + vec3(0, 0, kHandZ));
        s_.ropeLength = std::clamp(ropeBentLength(s_) + glm::distance(s_.pos + vec3(0, 0, kHandZ), ropePivot(s_)), anims_.grapple.minRope, maxRope());
        return;
    }
    if (jumpPressed) { jumpOffRope(move); return; }

    // Hanging on the rope [O tru.exe 0x746F60]. The stick is read against her facing, which stays while she swings.
    vec3 vel = s_.vel;
    const bool jumping0 = s_.clip == "grapple_rappel_free_jumpon"; // her own jump, not yet a hang (below)
    const vec3 pivot = ropePivot(s_), ropeV = oldHand - pivot;
    const float ropeNow = glm::length(ropeV);
    const float angle = ropeNow > 1.0f ? std::acos(std::clamp(-ropeV.z / ropeNow, -1.0f, 1.0f)) : 0.0f; // from straight down
    const bool outward = glm::dot(xy(ropeV), xy(vel)) > 0; // getting farther from under the pivot
    const vec2 f = xy(forward());
    // The stick in its own frame, not the camera's (tru.exe reads the pad's angle and deflection, input +0x10 / +0x18):
    // forward = along her facing, `side` > 0 = left.
    const float m = glm::length(a.move) < kStickDead ? 0.0f : std::min(1.0f, glm::length(a.move));
    const float fwd = m > 0 ? a.move.y / glm::length(a.move) : 0.0f, side = m > 0 ? -a.move.x / glm::length(a.move) : 0.0f;
    const bool wasDriven = s_.flight > 0;
    std::string_view ropeClip;
    if (a.interact) { // Interact held: the stick climbs the rope (chart S391 -> S395 / S392), never drives the swing
        s_.flight = 0, s_.savingGrab = 0;
        if (m > 0 && std::abs(fwd) > kClimbAxis && angle <= kClimbLockAngle) {
            if (fwd > 0) {
                // climbing beside a wall under the pivot she gets onto it (grapple_rappel_free_overhang_ascend, then
                // the rappel; S421 -> S314). OverhangDetected hanging free: probes at kOverhangUp over her root, front
                // then 90 degrees round each time; the lip is the top found by a ray up from 31.6 inside and 158 below
                // the contact (else the contact).
                const vec3 o = s_.pos + vec3(0, 0, kOverhangUp);
                vec2 d = f;
                for (int k = 0; k < 4; ++k, d = vec2(-d.y, d.x)) {
                    const auto h = col_.raycast(o, vec3(d, 0), kOverhangNear);
                    if (!h || h->dynamic >= 0 || std::abs(h->normal.z) >= 0.5f) continue; // 60..120 degrees from up
                    float lip = h->point.z;
                    if (const auto top = col_.raycast(h->point - h->normal * 31.605f - vec3(0, 0, 158.025f), {0, 0, 1}, 173.8275f)) lip = top->point.z;
                    if (lip < pivot.z - 158.025f) { rappelOnto(*h, vec3(0), "grapple_rappel_free_overhang_ascend"); return; }
                }
                s_.ropeLength = std::max(anims_.grapple.minRope, s_.ropeLength - kFreeAscend * fwd * m * dt);
                ropeClip = "grapple_rappel_free_ascend";
            } else {
                s_.ropeLength = std::min(maxRope(), s_.ropeLength - kFreeDescend * fwd * m * dt);
                ropeClip = "grapple_rappel_free_descend";
            }
        }
    } else {
        // The swing is driven while the stick is along her facing, forward or back, and kPumpHold after it left; from
        // rest only forward starts it; held back for kPumpBackCut it stops.
        const bool onAxis = m > 0 && std::abs(fwd) > kPumpAxis, back = onAxis && fwd < 0;
        s_.savingGrab = back ? s_.savingGrab + dt : 0.0f;
        if (onAxis && (wasDriven || !back)) s_.flight = kPumpHold;
        else s_.flight = std::max(0.0f, s_.flight - dt);
        if (s_.savingGrab > kPumpBackCut) s_.flight = 0;
    }
    const bool driven = s_.flight > 0;
    const bool over = angle > kSwingMaxAngle && outward;
    if (over) vel -= vel * std::min(1.0f, kOverBrake / kBodyMass * 30.0f * dt);
    if (driven) {
        s_.wallTime = 0;
        if (!over) { // the push follows her motion: forward while she swings forward (or hangs still), back while she swings back
            const bool goingFwd = glm::dot(xy(vel), f) >= 0;
            const float acc = ropeTable(goingFwd ? kPumpFwd : kPumpBack, ropeNow, maxRope());
            vel += vec3(f, 0) * ((goingFwd ? acc : -acc) * m * dt);
        }
        // motion across her swing is braked (a force of kSideBrake x that share of her motion)
        if (const vec3 across = glm::cross(ropeV, vec3(f, 0)); glm::length(across) > 1e-3f && glm::length(vel) > 1e-3f) {
            const vec3 s = glm::normalize(across);
            const float share = glm::dot(glm::normalize(vel), s), dv = kSideBrake / kBodyMass * 900.0f * dt * std::abs(share);
            vel -= s * std::clamp(glm::dot(vel, s), -dv, dv);
        }
    } else {
        // Not swinging: left / right turns her on the rope (grapple_rappel_free_rotate_*), eased in; going outward she is braked.
        if (m > 0 && std::abs(side) > kClimbAxis) {
            s_.wallTime += dt;
            const float ease = 0.5f * (1.0f - std::cos(3.1415927f * std::min(1.0f, s_.wallTime / kTurnRamp)));
            s_.yaw += (side > 0 ? kTurnRate : -kTurnRate) * ease * dt;
            if (const std::string_view turn = side > 0 ? "grapple_rappel_free_rotate_left" : "grapple_rappel_free_rotate_right"; ropeClip.empty() && anims_.find(turn)) ropeClip = turn;
        } else {
            s_.wallTime = 0;
        }
        if (outward) vel -= vel * std::min(1.0f, kRestBrake / kBodyMass * 30.0f * dt);
    }
    if (!jumping0) vel *= std::pow(1.0f - kBodyDamp, 30.0f * dt);
    if (const float sp = glm::length(vel); sp > kFreeMaxSpeed) vel *= kFreeMaxSpeed / sp;
    vel.z -= t_.gravity * dt;
    vec3 p = s_.pos + vel * dt;
    // Her jump with the rope is no hang until it comes down with no ground under her (chart T1387 / T291: velocity
    // downward, GroundDetected 0): until then the rope still follows her.
    const bool jumping = s_.clip == "grapple_rappel_free_jumpon" && (vel.z > 0 || col_.groundProbe(p + vec3(0, 0, 50.0f), 50.0f + kHangClear));
    if (jumping)
        s_.ropeLength = std::clamp(ropeBentLength(s_) + glm::distance(p + vec3(0, 0, kHandZ), pivot), anims_.grapple.minRope, maxRope());
    const float free = std::max(1.0f, s_.ropeLength - ropeBentLength(s_));
    ropeConstrain(s_, p, vel);
    // Feet onto a wall: rappel on it; coming down onto a floor lands her (off a taut rope she lets go; her own jump
    // with the rope slack keeps it); other contacts (overhangs) stop the motion into them.
    if (const vec3 step = p - s_.pos; glm::length(step) > 1e-4f)
        if (const auto h = col_.sweepSphere(s_.pos + vec3(0, 0, t_.wallOffset), t_.wallOffset, step); h && h->dynamic < 0) {
            if (std::abs(h->normal.z) <= kSteep && glm::dot(vel, h->normal) < 0) {
                const vec3 n = glm::normalize(vec3(h->normal.x, h->normal.y, 0)), right(-n.y, n.x, 0);
                const float along = glm::dot(vel, right);
                rappelOnto(*h, vel, along > 150.0f ? "grapple_rappel_wall_land_right"
                                    : along < -150.0f ? "grapple_rappel_wall_land_left" : "grapple_rappel_wall_land");
                return;
            }
            if (const vec3 at = s_.pos + step * h->t; h->normal.z >= t_.minFloorNormalZ && vel.z < -300.0f) // [I] landing speed
                if (const auto g = col_.groundProbe(at + vec3(0, 0, 50.0f), 80.0f); g && standable(at, *g)) {
                    s_.pos = vec3(xy(at), g->point.z);
                    if (jumping || glm::distance(s_.pos + vec3(0, 0, kHandZ), pivot) < free - 20.0f) s_.vel = vec3(0); // her jump, or slack: the rope is kept
                    else standUp();
                    return;
                }
            const vec3 rest = step * (1.0f - h->t);
            p = s_.pos + step * h->t + h->normal * 0.5f + rest - h->normal * glm::dot(rest, h->normal); // slide [I]
            vel -= h->normal * std::min(0.0f, glm::dot(vel, h->normal));
        }
    // Out at the end of the arc (not hanging under the anchor), coming down from the air just over a floor, or rising
    // past its level over it: lands on it, e.g. the far ledge of a wall run (chart T471: GroundDetected) [I window].
    if (!jumping && glm::length(xy(p - pivot)) > 0.3f * free)
        if (const auto g = col_.groundProbe(p + vec3(0, 0, 50.0f), 90.0f);
            g && g->normal.z >= t_.minFloorNormalZ && (vel.z <= 0 ? s_.pos.z > g->point.z + 2.0f : p.z >= g->point.z - 20.0f) && standable(p, *g)) {
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
    if (jumping) { advanceClip(dt); return; } // the jump into the gap
    if (!ropeClip.empty()) {
        play(ropeClip, true);
    } else if (driven || glm::length(xy(vel)) > 80.0f) {
        play(glm::dot(xy(vel), f) >= 0 ? "grapple_rappel_free_swing_forward" : "grapple_rappel_free_swing_backward", true);
    } else {
        play("grapple_rappel_idle", true);
    }
    advanceClip(dt);
}

} // namespace ou::game
