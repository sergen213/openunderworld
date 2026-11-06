// Free climbing on climbable rock (lara.drm DICE_* clips): attach from the ground or the air, move hold to hold with
// the stick, jump to far holds, leap up to a ledge, step off at the foot, jump off or drop. Rock is climbable only where
// the level places free-climb holds (markup types 16-20, docs/GAMEPLAY.md "Free climbing"). Lara's root sits at a hold;
// each move plays DICE_<from>_to_<to>_<dir>, whose root motion is the step to the next hold, ending in the new stance's
// idle; a jump launches, flies to a catch point below the hold and air-attaches onto it.
#include "game/Game.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>

namespace ou::game {

namespace {
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
constexpr float kReach = 90.0f;        // attach probe past the capsule radius [I]
constexpr float kAttachError = 300.0f; // hold anchor within this of where the attach clip ends [I]

// Which hold a move or jump goes to: lara.drm's markup-detection queries [O] (object data +0xDE4 count, +0xDE8 table of
// (id, record), tru.exe 0x6F1950; fields as 0x771830 reads them, per 30 Hz frame). A query launches a point and takes
// the first markup box it enters between frame +0x20 and +0xA4 (0x70F300 / 0x70EC60; 0x710100 keeps the first accepted):
// - DICE moves q73 sideways (+0xA0 5: the stick), q80 / q81 up / down: 15.80 units a frame sideways or 19.28 vertically
//   (+0x08..+0x10) from the chest (bone 16, +0xC8), no gravity (flag 0x400: -0.001), frames 1..10, within 90 degrees of
//   the stick (+0x24, 0x6FC530).
// - DICE jumps q28 / q29 sideways (+0xA0 17 / 15: along the attached hold), q31 up (16): 63.21 sideways + 34.77 up /
//   74.12 up a frame from the root under gravity 5.72443 / frame^2, frames 1..40 (+0xA4 -1: object +0xBDE), sideways
//   only to holds more than 410 away horizontally (+0x28, 0x6EC360) within 120 degrees (+0x24). Their start offsets
//   (+0x58: -316.06 / -158.03 down, +0x5C: 63.21 out) equal their offsets for target type 1 (+0x8C).
// The player state chart (lara.drm objdata +0x12508) pairs them per stick quadrant: up q80 / q31, down q81 / q30, left
// q82 / q28, right q83 / q29 (states S575..S578; seen live in tru.exe at player+0xB00 / +0xC90); the jump button runs
// the quadrant's jump query (neutral or down: q30) and jumps even without a target (S601..S605).
// Ours [I]: sweepHit() approximates the 3D sweep in 2D (docs/GAMEPLAY.md "Free climbing").
constexpr float kStepSide = 15.80f, kStepUp = 19.28f, kStepFrames = 10.0f;               // q73 / q80 / q81
constexpr float kJumpSide = 63.21f, kJumpSideUp = 34.77f, kJumpUp = 74.12f;              // q28 / q29 / q31
constexpr float kJumpGravity = 5.72443f, kJumpFrames = 40.0f, kJumpSideMin = 410.0f;     // q28 / q29 +0x28
constexpr float kJumpOut = 63.21f;                                                      // +0x5C / +0x8C
constexpr float kJumpBack = 50.57f;                                                     // q30 out from the rock
constexpr float kJumpSideCone = 0.5f;                                                   // cos 120 degrees
// +0x6C: 30 (moves) / 25 (jumps); taken as the swept point's radius [I: no reader of it traced].
constexpr float kStepRadius = 30.0f, kJumpRadius = 25.0f;

// Hand stance per hold markup type 16..20 [I]: each hold is a triangle whose points 1 and 2 sit, relative to point 0,
// (right, up) = 16: (-67, 144) (69, 174) both hands high; 17: (-75, 174) (41, 16) left high; 18: (-134, 143) (136, 16)
// left high, wide; 19: (-72, 14) (70, 143) right high; 20: (-96, 20) (125, 169) right high, wide (means over all 336
// holds in 8 units, 2026-10-29) -> DICE stances B, L, LW, R, RW.
constexpr std::array<std::string_view, 5> kStance = {"b", "l", "lw", "r", "rw"};
// The 12 direction suffixes lara.drm's DICE_<from>_to_<to>_<dir> clips use (clockwise from up in 30-degree steps).
constexpr std::array<std::string_view, 12> kDir = {"up",    "upr",   "updiagr", "right", "downdiagr", "downr",
                                                   "down",  "downl", "downdiagl", "left", "updiagl",  "upl"};

// Exact DICE clip name (kLaraClipNames spells them as lara.drm does) for a lower-case key, or "" if lara.drm has none.
std::string_view dice(const std::string& key) {
    static const std::map<std::string, std::string_view> table = [] {
        std::map<std::string, std::string_view> t;
        for (const auto n : kLaraClipNames) {
            std::string k(n);
            for (char& c : k) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (k.starts_with("dice_")) t.emplace(std::move(k), n);
        }
        return t;
    }();
    const auto it = table.find(key);
    return it == table.end() ? std::string_view{} : it->second;
}
std::string_view stanceClip(uint8_t stance, std::string_view what) {
    if (what == "air_attach") what = stance == 0 ? "air_attach_m" : "air_attach_mid";
    return dice("dice_" + std::string(kStance[stance]) + "_" + std::string(what));
}
// Bad attach (caught falling too fast): the stance's air_attach_bad start (hands slip ~115 down the hold), its loop,
// then its end back to the idle pose (lara.drm DICE_<B/LW/R/Rw>_air_attach_bad_start / _loop / _end; L has
// DICE_L_air_attach_bad, ending in the low pose, and DICE_L_air_attach_bad_recovery). "" = none.
std::string_view badClip(uint8_t stance, std::string_view part) {
    if (stance == 1) return part == "start" ? dice("dice_l_air_attach_bad") : part == "end" ? dice("dice_l_air_attach_bad_recovery") : "";
    return stanceClip(stance, "air_attach_bad_" + std::string(part));
}

// Free-climb holds [O]: markup types 16-20 (MUD 0x8000 + one of 0x4000..0x400), 3-point triangles 20-35 units off the
// rock. tru.exe disables 0x8000 markup unless it has exactly 3 points (0x5D6F00) and puts Lara's root at point 0
// - 158.025 z on one (0x707C60). The holds sit on a 158-unit grid and every DICE_<from>_to_<to>_<dir> clip's root
// motion is one grid step (Up = (0, 157), Left = (159, -29) in Lara's frame, wide stances 236 sideways): Lara moves
// from hold to hold, the transition clip's root motion carrying her, not along the rock at a fixed speed.
bool isHold(const collision::Markup& m) { return m.type >= 16 && m.type <= 20 && m.points.size() == 3; }
vec3 holdAnchor(const collision::Markup& m) { return m.points[0] - vec3(0, 0, 158.025f); }
// The rock's outward normal at a hold: its triangle lies parallel to the rock; oriented toward `ref`.
vec3 holdNormal(const collision::Markup& m, vec3 ref) {
    vec3 c = glm::cross(m.points[2] - m.points[0], m.points[1] - m.points[0]);
    c.z = 0;
    if (glm::dot(c, c) < 1e-6f) return ref;
    c = glm::normalize(c);
    return glm::dot(c, ref) < 0 ? -c : c;
}
// Model-space vector -> world for heading `yaw` (model -Y = forward; as Player.cpp's rotYaw) and back.
vec3 rotYaw(float yaw, vec3 m) {
    const float c = std::cos(yaw + 1.57079633f), s = std::sin(yaw + 1.57079633f);
    return {c * m.x - s * m.y, s * m.x + c * m.y, m.z};
}
vec3 unrotYaw(float yaw, vec3 w) {
    const float c = std::cos(yaw + 1.57079633f), s = std::sin(yaw + 1.57079633f);
    return {c * w.x + s * w.y, -s * w.x + c * w.y, w.z};
}
float lerpYaw(float a, float b, float t) { return a + std::remainder(b - a, 6.28318531f) * t; }
vec3 rootAt(const anim::Clip& c, double t) { return rootTranslation(c, t, false) - rootTranslation(c, 0, false); }

// First frame in [tMin, tMax] at which Lara's root, launched from her hold's anchor `here` with `v` a frame (right, up)
// falling `g` a frame^2, is in hold m's box: around its anchor, half its points' extent in its own plane plus `radius`.
// -1 if she is in it before tMin or never gets there. [I] tru.exe sweeps its start point
// (chest / root + offsets) in 3D against the markup boxes; here the start and target offsets are taken to cancel.
float sweepHit(const collision::Markup& m, vec3 here, vec3 right, vec2 v, float g, float tMin, float tMax, float radius) {
    const vec3 own = glm::normalize(glm::cross(-holdNormal(m, glm::cross(right, vec3(0, 0, 1))), vec3(0, 0, 1)));
    vec2 lo(1e9f), hi(-1e9f);
    for (const vec3& p : m.points) {
        const vec2 q(glm::dot(p, own), p.z);
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    }
    const vec3 d = holdAnchor(m) - here;
    const vec2 c(glm::dot(d, right), d.z), h = (hi - lo) * 0.5f + radius;
    for (float t = 0; t <= tMax; t += 0.25f) {
        const vec2 p = v * t - vec2(0, 0.5f * g * t * t) - c;
        if (std::abs(p.x) <= h.x && std::abs(p.y) <= h.y) return t >= tMin ? t : -1.0f;
    }
    return -1.0f;
}
} // namespace

// Attach to the free-climb holds on a wall ahead of Lara (chest-height probe, facing it, nearly vertical): the hold
// where the stance's attach clip would put her (ground: its root motion from her feet; air: where she is), which that
// clip then carries her to.
bool Player::tryClimb(bool fromAir) {
    if (s_.regrabCooldown > 0) return false;
    // Faster than a firm ledge grab (up to twice) only a bad attach holds, as a ledge's saving grab [I: tru.exe's DICE
    // bad-attach condition is not traced]; faster still she falls past.
    if (fromAir && s_.vel.z < 2 * t_.grabMinVz) return false;
    const bool bad = fromAir && s_.vel.z < t_.grabMinVz;
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
    const auto h = col_.raycast(s_.pos + vec3(0, 0, 300), fwd, t_.wallOffset + kReach);
    if (!h || h->dynamic >= 0) return false;
    if (std::abs(h->normal.z) > 0.35f || glm::dot(fwd, -h->normal) < 0.6f) return false;
    int32_t best = -1;
    float bestErr = kAttachError;
    for (size_t i = 0; i < col_.markup.size(); ++i) {
        const auto& m = col_.markup[i];
        if (!isHold(m) || glm::length(m.points[0] - h->point) > 700.0f) continue;
        const vec3 a = holdAnchor(m), n = holdNormal(m, h->normal);
        if (std::abs(glm::dot(a - h->point, h->normal)) > 120.0f || glm::dot(n, h->normal) < 0.5f) continue; // this wall
        const anim::Clip* c = anims_.find(stanceClip(uint8_t(m.type - 16), fromAir ? "air_attach" : "ground_attach"));
        if (!c) continue;
        const float err = glm::length(a - (s_.pos + rotYaw(yawOf(xy(-n)), rootAt(*c, c->durationSeconds()))));
        if (err < bestErr) best = int32_t(i), bestErr = err;
    }
    if (best < 0) return false;
    const auto& m = col_.markup[size_t(best)];
    s_.climbHold = best;
    s_.climbStance = uint8_t(m.type - 16);
    s_.climbNormal = holdNormal(m, h->normal);
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.state = State::Climb;
    s_.climbWarp = true;
    s_.climbFrom = s_.pos, s_.climbTo = holdAnchor(m);
    s_.climbYawFrom = s_.yaw, s_.climbYawTo = yawOf(xy(-s_.climbNormal));
    play(bad ? badClip(s_.climbStance, "start") : stanceClip(s_.climbStance, fromAir ? "air_attach" : "ground_attach"), false);
    s_.savingGrab = bad ? t_.savingGrabSeconds : 0.0f;
    return true;
}

// The playing clip carries Lara from climbFrom to climbTo: its root motion, in the frame she started in (heading
// climbYawFrom), scaled per axis to the actual offset (the residual eased in where the clip has no such motion); the
// heading turns to climbYawTo with it (corners). True once the clip is done (she is at climbTo).
bool Player::warpStep(float dt) {
    const anim::Clip* c = anims_.find(s_.clip);
    advanceClip(dt);
    const double dur = c ? c->durationSeconds() : 0;
    const float u = dur > 0 ? float(std::clamp(s_.clipTime / dur, 0.0, 1.0)) : 1.0f, ease = u * u * (3 - 2 * u);
    const vec3 want = unrotYaw(s_.climbYawFrom, s_.climbTo - s_.climbFrom);
    const vec3 end = c ? rootAt(*c, dur) : vec3(0), now = c ? rootAt(*c, s_.clipTime) : vec3(0);
    vec3 p;
    for (int a = 0; a < 3; ++a) {
        const float r = std::abs(end[a]) > 20.0f ? want[a] / end[a] : 0.0f;
        p[a] = r > 0.33f && r < 3.0f ? now[a] * r : now[a] + (want[a] - end[a]) * ease;
    }
    s_.pos = s_.climbFrom + rotYaw(s_.climbYawFrom, p);
    s_.yaw = lerpYaw(s_.climbYawFrom, s_.climbYawTo, ease);
    if (!clipDone()) return false;
    s_.pos = s_.climbTo, s_.yaw = s_.climbYawTo;
    return true;
}

void Player::climb(vec2 stick, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.climbHold < 0 || size_t(s_.climbHold) >= col_.markup.size()) { startFall(s_.vel); return; }
    static constexpr std::array<std::string_view, 4> kSide = {"up", "right", "down", "left"};
    // Where a jump catches the hold: DICE_attach_M's root motion before it (168 out, 161 down; tru.exe under Wine,
    // l2_cliffclimb 16 -> 1: caught 167 out, 163 down, then carried in over 0.8 s).
    const auto attachRoot = [&] {
        const anim::Clip* a = anims_.find("DICE_attach_M");
        return a ? rootAt(*a, a->durationSeconds()) : vec3(0);
    };
    if (s_.flight > 0) { // a DICE jump: the launch clip, the flight to the catch point, then the air attach onto the hold
        advanceClip(dt);
        const anim::Clip* c = anims_.find(s_.clip);
        if (s_.vel == vec3(0) && c && !clipDone()) { // the launch clip carries her off the rock with its root motion
            s_.pos = s_.climbFrom + rotYaw(s_.climbYawFrom, rootAt(*c, s_.clipTime));
            s_.flight = std::max(s_.flight - dt, 1.0f / 30.0f);
            return;
        }
        const vec3 catchAt = s_.climbTo - rotYaw(s_.climbYawTo, attachRoot());
        if (s_.vel == vec3(0)) { // launched: the rest of the query's flight time to the catch point
            const float time = s_.flight;
            s_.vel = (catchAt - s_.pos) / time + vec3(0, 0, 0.5f * t_.gravity * time);
        }
        const float step = std::min(dt, s_.flight);
        s_.pos += s_.vel * step + vec3(0, 0, -0.5f * t_.gravity * step * step);
        s_.vel.z -= t_.gravity * step;
        s_.flight -= step;
        const vec3 span = catchAt - s_.climbFrom;
        const float u = glm::dot(span, span) > 1 ? std::clamp(glm::dot(s_.pos - s_.climbFrom, span) / glm::dot(span, span), 0.0f, 1.0f) : 1.0f;
        s_.yaw = lerpYaw(s_.climbYawFrom, s_.climbYawTo, u);
        if (s_.flight > 0) return;
        s_.pos = catchAt, s_.yaw = s_.climbYawTo, s_.vel = vec3(0), s_.flight = 0;
        s_.climbWarp = true; // the stance's air attach carries her the rest of the way, as an attach from a fall does
        s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw;
        play(stanceClip(s_.climbStance, "air_attach"), false);
        return;
    }
    // Attach / transition / step-off clips carry her to their target (warpStep); the idle / reach / antic
    // clips have no root motion: she stays at her hold.
    bool arrived = false;
    if (s_.climbWarp) {
        if (!warpStep(dt)) return;
        s_.climbWarp = false;
        if (s_.clip.find("step_off") != std::string::npos) { // down off the rock onto the floor
            s_.climbHold = -1;
            s_.state = State::Idle;
            play("ng_idle", true);
            return;
        }
        arrived = s_.clip.find("_to_") != std::string::npos;
        if (s_.savingGrab > 0) { // a bad attach: hanging low on the hold (the loop, or the start's last frame)
            if (const auto loop = badClip(s_.climbStance, "loop"); !loop.empty()) play(loop, true);
            return;
        }
        if (!arrived) play(stanceClip(s_.climbStance, "idle"), true); // attached
    } else {
        advanceClip(dt);
    }

    const vec3 n = s_.climbNormal;
    const uint8_t stance = s_.climbStance;
    // Bad attach: jump pulls her back up to the idle pose (the bad end clip, then the idle); else she lets go when
    // the saving-grab time (difficulty "TIME FOR SAVING GRABS") runs out.
    if (s_.savingGrab > 0) {
        s_.savingGrab = std::max(0.0f, s_.savingGrab - dt);
        if (jumpPressed) {
            s_.savingGrab = 0;
            s_.climbWarp = true;
            s_.climbFrom = s_.climbTo = s_.pos;
            s_.climbYawFrom = s_.climbYawTo = s_.yaw;
            play(badClip(stance, "end"), false);
        } else if (s_.savingGrab <= 0) {
            s_.climbHold = -1;
            s_.regrabCooldown = 0.4f;
            play(stanceClip(stance, "drop"), false);
            startFall(n * 150.0f);
        }
        return;
    }
    // `stick`: the raw stick (x right, y up the rock), whichever way the camera looks.
    const vec3 right = glm::normalize(glm::cross(-n, vec3(0, 0, 1)));
    const vec2 dir = stick;
    const bool held = glm::length(dir) > 0.3f;
    const size_t verb = std::abs(dir.y) >= std::abs(dir.x) ? (dir.y > 0 ? 0 : 2) : (dir.x > 0 ? 1 : 3);
    const vec3 here = holdAnchor(col_.markup[size_t(s_.climbHold)]);
    // The first hold on this rock [I: within 150 of its plane, facing the same way] the query's sweep enters, if
    // `accept` takes it.
    const auto firstHold = [&](vec2 v, float g, float tMin, float tMax, float radius, auto accept) {
        int32_t best = -1;
        float bestT = 1e9f;
        for (size_t i = 0; i < col_.markup.size(); ++i) {
            const auto& m = col_.markup[i];
            if (!isHold(m) || int32_t(i) == s_.climbHold) continue;
            const vec3 d = holdAnchor(m) - here;
            if (std::abs(glm::dot(d, n)) > 150.0f || glm::dot(holdNormal(m, n), n) < 0.5f || !accept(d)) continue;
            const float t = sweepHit(m, here, right, v, g, tMin, tMax, radius) + glm::length(d) * 1e-4f; // ties: the nearer
            if (t >= 0 && t < bestT) best = int32_t(i), bestT = t;
        }
        return std::pair{best, bestT};
    };
    // DICE jump target that way (verb 0 up, 1 right, 3 left): q31 / q28 / q29. Their target type 1 offsets (+0x8C)
    // equal their start offsets, so ledges are met by her root's arc as holds are (Player::ledgeOnArc); 0x710100 keeps
    // the first one met.
    struct Target { int32_t hold = -1; JumpTarget ledge; float frames = 0; bool any() const { return hold >= 0 || ledge.markup >= 0; } };
    const auto jumpTarget = [&](size_t way) {
        Target r;
        if (way == 2) return r;
        const float side = way == 1 ? 1.0f : way == 3 ? -1.0f : 0.0f;
        const vec2 v = way == 0 ? vec2(0, kJumpUp) : vec2(side * kJumpSide, kJumpSideUp);
        const vec3 start = here + n * kJumpOut;
        const auto sideways = [&](vec3 to) { // q28 / q29: beyond 410 horizontally, within 120 degrees
            const vec2 h = xy(to - start);
            return glm::length(h) > kJumpSideMin && glm::dot(glm::normalize(h), xy(right) * side) >= kJumpSideCone;
        };
        const auto [hold, frames] = firstHold(v, kJumpGravity, 1.0f, kJumpFrames, kJumpRadius, [&](vec3 d) {
            // q31: 20 horizontally, 180 degrees (the root launches 63 off the rock: always met); its arc comes back down
            // past the holds below, which a jump up does not catch [I: 0x6FC6B0's direction test, not traced].
            return way == 0 ? d.z > 0 : sideways(here + d);
        });
        JumpTarget l = ledgeOnArc(here, right * v.x + vec3(0, 0, v.y), kJumpFrames, xy(-n));
        if (l.markup >= 0) { // up: not below one hold step under her (the arc's fall would meet any) [I]
            const vec3 to = attachPose(l.markup, l.seg, l.t, l.side).first;
            if (way == 0 ? to.z < here.z - 158.0f : !sideways(to)) l = {};
        }
        if (hold >= 0 && (l.markup < 0 || frames <= l.frames)) r.hold = hold, r.frames = frames;
        else if (l.markup >= 0) r.ledge = l, r.frames = l.frames;
        return r;
    };
    // q30 (jump with the stick neutral or down): 50.57 out from the rock + 34.77 up a frame, gravity 5.72443, frames
    // 1..40, to a hold on rock facing back at her (in 3D: the arc within half the hold's extent + the radius of it).
    const auto backTarget = [&] {
        int32_t best = -1;
        float bestT = 1e9f;
        for (size_t i = 0; i < col_.markup.size(); ++i) {
            const auto& m = col_.markup[i];
            if (!isHold(m) || int32_t(i) == s_.climbHold) continue;
            const vec3 hn = holdNormal(m, -n);
            if (glm::dot(hn, n) > -0.5f) continue;
            const vec3 own = glm::normalize(glm::cross(-hn, vec3(0, 0, 1)));
            vec2 lo(1e9f), hi(-1e9f);
            for (const vec3& p : m.points) lo = glm::min(lo, vec2(glm::dot(p, own), p.z)), hi = glm::max(hi, vec2(glm::dot(p, own), p.z));
            const vec2 h = (hi - lo) * 0.5f + kJumpRadius;
            for (float t = 1; t <= kJumpFrames && t < bestT; t += 0.25f) {
                const vec3 d = holdAnchor(m) - (here + n * (kJumpBack * t) + vec3(0, 0, kJumpSideUp * t - 0.5f * kJumpGravity * t * t));
                if (std::abs(glm::dot(d, own)) <= h.x && std::abs(d.z) <= h.y && std::abs(glm::dot(d, hn)) <= kJumpRadius) {
                    best = int32_t(i), bestT = t;
                    break;
                }
            }
        }
        return std::pair{best, bestT};
    };
    // Onto a hold: the launch clip, then the flight to her catch point at the query's hit time (tru.exe 0x710100 keeps
    // the first hold its arc meets).
    const auto jumpTo = [&](int32_t target, float frames, std::string_view launch) {
        const auto& m = col_.markup[size_t(target)];
        s_.climbFrom = s_.pos, s_.climbTo = holdAnchor(m);
        s_.climbYawFrom = s_.yaw;
        s_.climbNormal = holdNormal(m, glm::dot(holdNormal(m, n), n) < -0.5f ? -n : n);
        s_.climbYawTo = yawOf(xy(-s_.climbNormal));
        s_.vel = vec3(0);
        s_.flight = frames / 30.0f;
        s_.climbHold = target;
        play(launch, false);
        s_.climbStance = uint8_t(m.type - 16);
    };

    // Jump (tru.exe player chart, hold idle S571 / lean S583..S587 on the jump button): the stick's quadrant picks the
    // jump query — up q31, right q29, left q28, neutral or down q30.
    if (jumpPressed && held && verb != 2) {
        const std::string launch(stanceClip(stance, "jump_" + std::string(kSide[verb])));
        if (const Target j = jumpTarget(verb); j.hold >= 0) {
            jumpTo(j.hold, j.frames, launch);
            return;
        } else if (j.ledge.markup >= 0) { // onto a ledge: the launch clip, the flight (HangJump), ledgeb_attach_U / _S
            s_.ledge = j.ledge.markup, s_.ledgeSeg = j.ledge.seg, s_.ledgeT = j.ledge.t, s_.ledgeSide = j.ledge.side;
            s_.polePara = false;
            s_.climbHold = -1;
            s_.state = State::HangJump;
            s_.fallStartZ = s_.pos.z;
            play(launch, false);
            launchHangJump((verb == 0 ? kJumpUp : kJumpSideUp) * 30.0f);
            return;
        }
        // nothing that way: the leap still goes (S601..S605), q31 / q28 / q29's launch (74.12 up; 63.21 sideways,
        // 34.77 up a frame)
        s_.climbHold = -1;
        s_.state = State::Jump;
        s_.vel = (verb == 0 ? vec3(0, 0, kJumpUp) : right * (verb == 1 ? kJumpSide : -kJumpSide) + vec3(0, 0, kJumpSideUp)) * 30.0f;
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.4f;
        play(launch, false);
        return;
    }
    if (jumpPressed && (!held || verb == 2)) {
        if (const auto [target, frames] = backTarget(); target >= 0) {
            jumpTo(target, frames, stanceClip(stance, "jump_back"));
            return;
        }
        // Leap away from the wall with q30's launch: 50.57 out, 34.77 up a frame (tru.exe under Wine, l2_cliffclimb hold
        // 1: 1517 units/s out, apex 0.2 s after leaving, g 5100 measured).
        s_.climbHold = -1;
        s_.state = State::Jump;
        s_.vel = (n * kJumpBack + vec3(0, 0, kJumpSideUp)) * 30.0f;
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.4f;
        play(stanceClip(stance, "jump_back"), false);
        return;
    }
    if (dropPressed) {
        s_.climbHold = -1;
        s_.regrabCooldown = 0.4f;
        play(stanceClip(stance, "drop"), false);
        startFall(n * 150.0f);
        return;
    }

    // A finished step ends in the new stance's idle pose (DICE_B_to_B_Up's last frame has the chest within 4 units of
    // DICE_B_Idle's): the next step follows straight on, or she idles. The DICE_<s>_settle_<dir> clips are not for this:
    // they start far off idle (DICE_R_settle_right: chest 211 to her left).
    if (arrived) play(stanceClip(stance, "idle"), true);

    if (held) {
        // The next hold that way: the move query's sweep along the stick (q73 / q80 / q81), then the
        // DICE_<stance>_to_<its stance>_<dir> clip whose root motion fits best. No diagonal fallback: tru.exe under Wine
        // on l2_cliffclimb hold 11 stays put pushing up or right alone and goes to hold 38 (159 right, 160 up) only with
        // up + right held.
        const vec2 sd = glm::normalize(dir);
        const int32_t best = firstHold(vec2(sd.x * kStepSide, sd.y * kStepUp), 0.0f, 0.0f, kStepFrames, kStepRadius, [&](vec3 d) {
            return glm::dot(vec2(glm::dot(d, right), d.z), sd) >= 0; // q73 +0x24: 90 degrees
        }).first;
        if (best < 0 && jumpTarget(verb).any()) { // a far hold or ledge that way: lean toward it until jump is pressed
            if (const auto antic = stanceClip(stance, "antic_" + std::string(kSide[verb])); !antic.empty()) {
                play(antic, true);
                return;
            }
        }
        std::string_view bestClip;
        if (best >= 0) {
            const auto& m = col_.markup[size_t(best)];
            const vec3 d = holdAnchor(m) - here;
            const std::string prefix = "dice_" + std::string(kStance[stance]) + "_to_" + std::string(kStance[m.type - 16]) + "_";
            float bestErr = 1e9f;
            for (const auto k : kDir) {
                const auto name = dice(prefix + std::string(k));
                const anim::Clip* c = name.empty() ? nullptr : anims_.find(name);
                if (!c) continue;
                vec3 e = rotYaw(s_.yaw, rootAt(*c, c->durationSeconds())) - d;
                e -= n * glm::dot(e, n);
                if (glm::length(e) < bestErr) bestClip = name, bestErr = glm::length(e);
            }
        }
        if (!bestClip.empty()) {
            const auto& m = col_.markup[size_t(best)];
            s_.climbWarp = true;
            s_.climbFrom = s_.pos, s_.climbTo = holdAnchor(m);
            s_.climbYawFrom = s_.yaw;
            s_.climbNormal = holdNormal(m, n);
            s_.climbYawTo = yawOf(xy(-s_.climbNormal));
            s_.climbHold = best;
            s_.climbStance = uint8_t(m.type - 16);
            play(bestClip, false);
            return;
        }
        if (verb == 2) { // no hold below: a floor within the step-off clip's drop = step down onto it
            const auto name = stanceClip(stance, "step_off_ground");
            const anim::Clip* c = name.empty() ? nullptr : anims_.find(name);
            if (c) {
                const vec3 end = rotYaw(s_.yaw, rootAt(*c, c->durationSeconds()));
                if (const auto f = col_.groundProbe(s_.pos + vec3(end.x, end.y, 100.0f), 600.0f);
                    f && f->normal.z >= t_.minFloorNormalZ && std::abs(s_.pos.z + end.z - f->point.z) < 200.0f) {
                    s_.climbWarp = true;
                    s_.climbFrom = s_.pos, s_.climbTo = f->point;
                    s_.climbYawFrom = s_.climbYawTo = s_.yaw;
                    play(name, false);
                    return;
                }
            }
        }
        // Nothing that way: reach for it (DICE_<stance>_reach_<dir>, held at its last frame).
        if (const auto reach = stanceClip(stance, "reach_" + std::string(kSide[verb])); !reach.empty()) play(reach, false);
        return;
    }
    play(stanceClip(stance, "idle"), true);
}

} // namespace ou::game
