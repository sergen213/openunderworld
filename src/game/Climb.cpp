// Free climbing on climbable rock (lara.drm DICE_* clips): attach from the ground or the air, move hold to hold with
// the stick, leap up to a ledge, step off at the foot, jump off or drop. Rock is climbable only where the level places
// free-climb holds (markup types 16-20, docs/GAMEPLAY.md "Free climbing"). Lara's root sits at a hold; each move plays
// DICE_<from>_to_<to>_<dir>, whose root motion is the step to the next hold, ending in the new stance's idle.
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
constexpr float kStepError = 90.0f;    // next hold within this of a transition clip's root motion (grid step 158) [I]

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
} // namespace

// Attach to the free-climb holds on a wall ahead of Lara (chest-height probe, facing it, nearly vertical): the hold
// where the stance's attach clip would put her (ground: its root motion from her feet; air: where she is), which that
// clip then carries her to.
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
    play(stanceClip(s_.climbStance, fromAir ? "air_attach" : "ground_attach"), false);
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
    // Attach / transition / step-off clips carry her to their target (warpStep); the idle / reach clips have
    // no root motion: she stays at her hold.
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
        if (!arrived) play(stanceClip(s_.climbStance, "idle"), true); // attached
    } else {
        advanceClip(dt);
    }

    const vec3 n = s_.climbNormal;
    const uint8_t stance = s_.climbStance;
    if (jumpPressed) { // leap away from the wall
        s_.climbHold = -1;
        s_.state = State::Jump;
        s_.vel = n * 700.0f + vec3(0, 0, 1400.0f);
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
    // they start far off idle (DICE_R_settle_right: chest 211 to her left), the swing after a DICE jump lands.
    if (arrived) play(stanceClip(stance, "idle"), true);

    // `stick`: the raw stick (x right, y up the rock), whichever way the camera looks.
    const vec3 right = glm::normalize(glm::cross(-n, vec3(0, 0, 1)));
    const vec2 dir = stick;
    const size_t verb = std::abs(dir.y) >= std::abs(dir.x) ? (dir.y > 0 ? 0 : 2) : (dir.x > 0 ? 1 : 3);
    static constexpr std::array<std::string_view, 4> kSide = {"up", "right", "down", "left"};
    if (glm::length(dir) > 0.3f) {
        // The next hold that way: of the holds on this rock within one or two grid steps and 50 degrees of the stick that
        // a DICE_<stance>_to_<its stance>_<dir> clip's root motion reaches, the one nearest the stick's direction (a
        // closer clip fit breaks near-ties: 90 units of misfit weigh as ~17 degrees).
        const vec3 here = holdAnchor(col_.markup[size_t(s_.climbHold)]);
        int32_t best = -1;
        std::string_view bestClip;
        float bestScore = 1e9f;
        for (size_t i = 0; i < col_.markup.size(); ++i) {
            const auto& m = col_.markup[i];
            if (!isHold(m) || int32_t(i) == s_.climbHold) continue;
            const vec3 d = holdAnchor(m) - here;
            const float len = glm::length(d);
            if (len < 50.0f || len > 450.0f || std::abs(glm::dot(d, n)) > 150.0f || glm::dot(holdNormal(m, n), n) < 0.5f) continue;
            const vec2 dw(glm::dot(d, right), d.z);
            const float cosine = glm::length(dw) < 40.0f ? -1.0f : glm::dot(glm::normalize(dw), glm::normalize(dir));
            if (cosine < 0.64f) continue; // 50 degrees
            const std::string prefix = "dice_" + std::string(kStance[stance]) + "_to_" + std::string(kStance[m.type - 16]) + "_";
            for (const auto k : kDir) {
                const auto name = dice(prefix + std::string(k));
                const anim::Clip* c = name.empty() ? nullptr : anims_.find(name);
                if (!c) continue;
                vec3 e = rotYaw(s_.yaw, rootAt(*c, c->durationSeconds())) - d;
                e -= n * glm::dot(e, n);
                const float err = glm::length(e), score = 1.0f - cosine + err * 0.0005f;
                if (err < kStepError && score < bestScore) best = int32_t(i), bestClip = name, bestScore = score;
            }
        }
        if (best >= 0) {
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
        if (verb == 0) { // no hold above: a ledge within a leap = jump up and catch it (ledgeb_attach_U)
            const float hands = s_.pos.z + 300.0f;
            int bestLi = -1, bestSeg = 0;
            float bestT = 0, bestSide = 1, bestD = 200.0f;
            for (size_t li = 0; li < col_.markup.size(); ++li) {
                const auto& m = col_.markup[li];
                if (m.type != 1) continue;
                for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
                    const vec3 p0 = m.points[size_t(si)], p1 = m.points[size_t(si) + 1];
                    const float len = glm::length(xy(p1 - p0));
                    if (len < 1e-2f) continue;
                    const vec2 sd = xy(p1 - p0) / len, sn(-sd.y, sd.x);
                    if (std::abs(glm::dot(sn, xy(n))) < 0.7f) continue; // the ledge runs along this rock
                    const float t = std::clamp(glm::dot(xy(s_.pos - p0), sd), 0.0f, len);
                    const vec3 q = p0 + vec3(sd * t, (p1.z - p0.z) * t / len);
                    const float horiz = glm::length(xy(q - s_.pos)), rise = q.z - hands;
                    if (horiz < bestD && rise > -50.0f && rise < 450.0f)
                        bestD = horiz, bestLi = int(li), bestSeg = si, bestT = t, bestSide = glm::dot(sn, xy(n)) > 0 ? 1.0f : -1.0f;
                }
            }
            if (bestLi >= 0) {
                const vec3 delta = attachPose(bestLi, bestSeg, bestT, bestSide).first - s_.pos;
                const float g = t_.gravity, vz = std::sqrt(2 * g * (std::max(delta.z, 0.0f) + t_.hangJumpApex));
                const float time = (vz + std::sqrt(vz * vz - 2 * g * delta.z)) / g;
                s_.vel = vec3(xy(delta) / time, vz);
                s_.flight = time;
                s_.ledge = bestLi, s_.ledgeSeg = bestSeg, s_.ledgeT = bestT, s_.ledgeSide = bestSide;
                s_.climbHold = -1;
                s_.state = State::HangJump;
                s_.fallStartZ = s_.pos.z;
                play(stanceClip(stance, "jump_up"), false);
                return;
            }
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
