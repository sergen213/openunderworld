// Free climbing on climbable rock (lara.drm DICE_* clips): attach from the ground or the air,
// climb with the stick, top out, jump off or drop. Rock is climbable only where the level places free-climb
// holds (markup types 16-20, docs/GAMEPLAY.md "Free climbing"); speeds and reach are ours [I], the settle
// clips have no root motion. The hold nearest the chest sets the hand stance (B / L / LW / R / RW); moving
// to another hold plays DICE_<from>_to_<to>_<dir> once.
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
constexpr float kSpeed = 240.0f;   // units/s along the wall [I]
constexpr float kReach = 90.0f;    // probe past the capsule radius [I]
constexpr float kHoldReach = 350.0f; // chest (root + 300) to hold centroid; holds sit 100-250 apart [I]

// Hand stance per hold markup type 16..20 [I]: each hold is a triangle whose points 1 and 2 sit, relative to point 0,
// (right, up) = 16: (-67, 144) (69, 174) both hands high; 17: (-75, 174) (41, 16) left high; 18: (-134, 143) (136, 16)
// left high, wide; 19: (-72, 14) (70, 143) right high; 20: (-96, 20) (125, 169) right high, wide (means over all 336
// holds in 8 units, 2026-10-29) -> DICE stances B, L, LW, R, RW.
constexpr std::array<std::string_view, 5> kStance = {"b", "l", "lw", "r", "rw"};
// Transition directions, clockwise from up in 30-degree steps [I: the 12 suffixes lara.drm uses].
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

// Free-climb holds [O]: markup types 16-20 (MUD 0x8000 + one of 0x4000..0x400), 3-point triangles 20-35
// units off the rock. tru.exe disables 0x8000 markup unless it has exactly 3 points (0x5D6F00) and places
// Lara at point 0 - 158.025 z when attaching to one (0x707C60). A hold counts when it is within reach of the
// chest and on this wall (near the plane through `wall` with normal `n`), not one behind a thin wall; the
// nearest one is returned (markup index), -1 if none.
int32_t nearestHold(const collision::CollisionMesh& col, vec3 chest, vec3 wall, vec3 n) {
    int32_t best = -1;
    float bestD = kHoldReach;
    for (size_t i = 0; i < col.markup.size(); ++i) {
        const auto& m = col.markup[i];
        if (m.type < 16 || m.type > 20 || m.points.size() != 3) continue;
        const vec3 c = (m.points[0] + m.points[1] + m.points[2]) / 3.0f;
        if (const float d = glm::length(c - chest); std::abs(glm::dot(c - wall, n)) < 120.0f && d < bestD) best = int32_t(i), bestD = d;
    }
    return best;
}
} // namespace

// DICE_<from>_to_<to>_<dir> for a move along `dir` (x right, y up on the wall), as lara.drm names it; the nearest
// existing direction within 60 degrees when that exact one is missing; "" if none.
std::string_view diceTransition(uint8_t from, uint8_t to, vec2 dir) {
    const float a = std::atan2(dir.x, dir.y); // clockwise from up
    const int k = (int(std::lround(a / (3.14159265f / 6))) + 12) % 12;
    const std::string prefix = "dice_" + std::string(kStance[from]) + "_to_" + std::string(kStance[to]) + "_";
    for (const int off : {0, 1, -1, 2, -2})
        if (const auto n = dice(prefix + std::string(kDir[size_t((k + off + 12) % 12)])); !n.empty()) return n;
    return {};
}

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
    const auto h = col_.raycast(s_.pos + vec3(0, 0, 300), fwd, t_.wallOffset + kReach);
    if (!h || h->dynamic >= 0) return false;
    if (std::abs(h->normal.z) > 0.35f || glm::dot(fwd, -h->normal) < 0.6f) return false;
    const int32_t hold = nearestHold(col_, s_.pos + vec3(0, 0, 300), h->point, h->normal);
    if (hold < 0) return false;
    s_.climbFace = h->face;
    s_.climbNormal = h->normal;
    s_.climbHold = hold;
    s_.climbStance = uint8_t(col_.markup[size_t(hold)].type - 16);
    s_.yaw = yawOf(xy(-h->normal));
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.state = State::Climb;
    vec3 p = h->point + h->normal * t_.wallOffset;
    p.z = s_.pos.z; // the attach clip supplies the rise
    s_.pos = p;
    play(stanceClip(s_.climbStance, fromAir ? "air_attach" : "ground_attach"), false);
    return true;
}

void Player::climb(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.climbFace == UINT32_MAX || s_.climbFace >= col_.faces.size()) { startFall(s_.vel); return; }
    const vec3 n = s_.climbNormal;
    const vec3 fwd = -n; // into the wall
    const uint8_t stance = s_.climbStance;
    const bool attaching = s_.clip == stanceClip(stance, "air_attach") || s_.clip == stanceClip(stance, "ground_attach");
    const bool moving = s_.clip.find("_to_") != std::string::npos && !clipDone(); // a hold-to-hold transition
    if (!attaching && jumpPressed) { // leap away from the wall
        s_.climbFace = UINT32_MAX;
        s_.state = State::Jump;
        s_.vel = n * 700.0f + vec3(0, 0, 1400.0f);
        s_.fallStartZ = s_.pos.z;
        s_.regrabCooldown = 0.4f;
        play(stanceClip(stance, "jump_back"), false);
        return;
    }
    if (!attaching && dropPressed) {
        s_.climbFace = UINT32_MAX;
        s_.regrabCooldown = 0.4f;
        play(stanceClip(stance, "drop"), false);
        startFall(n * 150.0f);
        return;
    }

    const vec3 prev = s_.pos;
    vec3 delta(0);
    // The stick is a world-space horizontal wish (as on the ground): its component into the wall
    // climbs up (away = down), its component along the wall moves sideways.
    const vec3 right = glm::normalize(glm::cross(fwd, vec3(0, 0, 1))); // Lara's right (was her left before 2026-10-29)
    const vec3 wish(move.x, move.y, 0);
    const float side = glm::dot(wish, right), up = glm::dot(wish, fwd);
    if (attaching) {
        delta = toWorld(advanceClip(dt));
        if (clipDone()) play(stanceClip(stance, "idle"), true);
    } else {
        delta = right * (side * kSpeed * dt) + vec3(0, 0, up * kSpeed * dt);
        if (moving)
            ; // the transition plays out while she moves [I]
        else if (std::abs(up) >= std::abs(side) && std::abs(up) > 0.1f)
            play(stanceClip(stance, up > 0 ? "settle_up" : "settle_down"), true);
        else if (std::abs(side) > 0.1f)
            play(stanceClip(stance, side > 0 ? "settle_right" : "settle_left"), true);
        else
            play(stanceClip(stance, "idle"), true);
        advanceClip(dt); // the settle/transition clips' root motion is not applied; kSpeed moves the root
    }
    s_.pos = slide(s_.pos, delta, t_.stepUp, true);

    // Stay on the rock: probe back into the wall at chest height (at the feet the probe grazes the floor), follow
    // it (also around corners) while holds are in reach, or top out.
    const vec3 chest = s_.pos + vec3(0, 0, 300);
    const auto h = col_.raycast(chest + n * 20.0f, -n, t_.wallOffset + kReach + 40.0f);
    if (const int32_t hold = h && h->dynamic < 0 && std::abs(h->normal.z) <= 0.35f ? nearestHold(col_, chest, h->point, h->normal) : -1;
        hold >= 0) {
        s_.climbFace = h->face;
        s_.climbNormal = h->normal;
        s_.yaw = yawOf(xy(-h->normal));
        const vec3 p = h->point + h->normal * t_.wallOffset;
        s_.pos.x = p.x, s_.pos.y = p.y; // pinned at capsule distance from the rock
        if (hold != s_.climbHold && !attaching) { // reached the next hold: its stance, via the matching transition [I]
            const auto to = uint8_t(col_.markup[size_t(hold)].type - 16);
            if (const auto clip = diceTransition(stance, to, {side, up}); !clip.empty() && glm::length(vec2(side, up)) > 0.1f)
                play(clip, false);
            s_.climbHold = hold;
            s_.climbStance = to;
        }
        return;
    }
    // No holds here: a standable floor above the feet, just into the wall = climb onto it (not the floor at the
    // wall's foot, found through the rock when the holds end sideways).
    if (const auto top = col_.groundProbe(s_.pos + fwd * (t_.wallOffset + 100.0f) + vec3(0, 0, 450), 550.0f);
        top && top->normal.z >= t_.minFloorNormalZ && top->point.z > prev.z + 10.0f) {
        s_.pos = top->point + n * t_.wallOffset;
        s_.climbFace = UINT32_MAX;
        s_.climbHold = -1;
        s_.state = State::Idle;
        play("ng_idle", true);
        return;
    }
    s_.pos = prev; // the rock ended: stop at the edge instead of falling off it
}

} // namespace ou::game
