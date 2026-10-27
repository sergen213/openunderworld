// Lara's controller: state machine, root-motion locomotion, capsule collide-and-slide, ledges, beams, poles.
// Numbers and their provenance: docs/GAMEPLAY.md.
#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace ou::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSkin = 1.0f;       // gap kept to surfaces after a sweep, units
constexpr float kEdgePush = 200.0f; // units/s: a capsule resting on an edge corner slides off at least this fast [I]

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
float approachAngle(float from, float to, float maxStep) {
    const float d = wrapAngle(to - from);
    return wrapAngle(from + std::clamp(d, -maxStep, maxStep));
}
vec2 xy(vec3 v) { return {v.x, v.y}; }
float yawOf(vec2 d) { return std::atan2(d.y, d.x); }
// Model-space vector -> world for heading `yaw` (model -Y = forward).
vec3 rotYaw(float yaw, vec3 m) {
    const float c = std::cos(yaw + kPi / 2), s = std::sin(yaw + kPi / 2);
    return {c * m.x - s * m.y, s * m.x + c * m.y, m.z};
}

// Root segment channels [first, first + 3) of `c` at unbounded time `t` (see rootTranslation).
vec3 rootChannels(const anim::Clip& c, double t, bool loop, int first) {
    if (c.tracks.empty() || c.keyCount == 0 || c.timePerKey == 0) return vec3(0);
    const auto& tr = c.tracks[0];
    const int n = c.keyCount;
    auto key = [&](int i) {
        vec3 v(0);
        for (int a = 0; a < 3; ++a)
            if (!tr[size_t(first + a)].empty()) v[a] = tr[size_t(first + a)][size_t(i)];
        return v;
    };
    const double dur = c.durationSeconds();
    double cycles = 0;
    if (loop) {
        cycles = std::floor(t / dur);
        t -= cycles * dur;
    } else {
        t = std::clamp(t, 0.0, dur);
    }
    const vec3 first0 = key(0), last = key(n - 1);
    // Cycle displacement: n key intervals per loop (the seam interval runs last -> first), each the average step.
    const vec3 cycle = n > 1 ? (last - first0) * (float(n) / float(n - 1)) : vec3(0);
    const double u = t * anim::kTimeUnitsPerSecond / c.timePerKey;
    const int k = int(std::floor(u));
    if (k >= n) return last + cycle * float(cycles);
    const vec3 b = key(k), a = k > 0 ? key(k - 1) : loop ? last - cycle : first0;
    return glm::mix(a, b, float(u - k)) + cycle * float(cycles);
}
// Root yaw (segment 0 axis-angle Z; turn clips rotate about Z only) at unbounded time `t`.
float rootYaw(const anim::Clip& c, double t, bool loop) { return rootChannels(c, t, loop, anim::RotX).z; }

// ---- markup polylines (x/y geometry; z interpolated along the segment) ----
float segLen(const collision::Markup& m, int seg) { return glm::length(xy(m.points[size_t(seg) + 1] - m.points[size_t(seg)])); }
vec2 segDir(const collision::Markup& m, int seg) {
    const vec2 d = xy(m.points[size_t(seg) + 1] - m.points[size_t(seg)]);
    const float l = glm::length(d);
    return l > 1e-4f ? d / l : vec2(1, 0);
}
vec3 pointOn(const collision::Markup& m, int seg, float t) {
    const float len = segLen(m, seg);
    return glm::mix(m.points[size_t(seg)], m.points[size_t(seg) + 1], len > 1e-4f ? t / len : 0.0f);
}
struct OnLine {
    int seg = -1;
    float t = 0, u = 0; // clamped / unclamped distance along the segment
    vec3 q{0};
    float dist = 1e30f; // x/y
};
OnLine closestOn(const collision::Markup& m, vec3 p) {
    OnLine best;
    for (size_t s = 0; s + 1 < m.points.size(); ++s) {
        const float len = segLen(m, int(s));
        if (len < 1e-2f) continue;
        const float u = glm::dot(xy(p - m.points[s]), segDir(m, int(s)));
        const float t = std::clamp(u, 0.0f, len);
        const vec3 q = pointOn(m, int(s), t);
        const float d = glm::length(xy(p - q));
        if (d < best.dist) best = {int(s), t, u, q, d};
    }
    return best;
}
// Moves (seg, t) by `ds` along the polyline, crossing joints; true if it stopped at an end.
bool moveAlong(const collision::Markup& m, int& seg, float& t, float ds) {
    t += ds;
    while (t > segLen(m, seg) && size_t(seg) + 2 < m.points.size()) t -= segLen(m, seg++);
    while (t < 0 && seg > 0) t += segLen(m, --seg);
    const float len = segLen(m, seg);
    const bool end = t < 0 || t > len;
    t = std::clamp(t, 0.0f, len);
    return end;
}
bool isPole(const collision::Markup& m) { return m.type == 4 || m.type == 6; } // pole, horizpole (MUD 0x8000000 / 0x2000000)
// The hang side of a ledge (out = segment normal * side) drops more than 200 within 240 units of the line:
// somewhere to hang. Ledge lines can sit well inside the collision top (l4_pool 1253 by up to ~180), and
// platform-edge ledges must not be hung from the platform side.
bool openBelow(const collision::CollisionMesh& col, const collision::Markup& m, int seg, float t, float side) {
    const vec3 q = pointOn(m, seg, t);
    const vec2 dir = segDir(m, seg), out = vec2(-dir.y, dir.x) * side;
    for (float k = 40; k <= 240; k += 40) {
        const auto g = col.groundProbe(vec3(xy(q) + out * k, q.z + 50), 250);
        if (!g || g->point.z <= q.z - 200) return true;
    }
    return false;
}

} // namespace

std::string_view stateName(State s) {
    static constexpr std::string_view k[] = {"idle", "walk", "run", "stop", "turn", "jumpprep", "jump",
                                             "fall", "land", "hang", "shimmy", "climbup", "swimsplash",
                                             "swimtread", "swimsurface", "swimdive", "swimunder", "surfacing",
                                             "swimclimbout", "dead", "crouch", "roll", "slide", "beam", "droptohang",
                                             "hangjump", "polehang", "climb", "grapplethrow", "grappleswing", "hit"};
    return size_t(s) < std::size(k) ? k[size_t(s)] : "unknown";
}

const anim::Clip* AnimLibrary::find(std::string_view name) const {
    const auto it = clips.find(name);
    return it == clips.end() ? nullptr : &it->second;
}

std::optional<vec3> AnimLibrary::handsAt(const anim::Clip& clip, double t) const {
    if (skeleton.bones.size() <= 80) return std::nullopt;
    auto local = anim::sample(clip, skeleton, t, false);
    local[0] = anim::bindPose(skeleton)[0]; // as rendered: root motion lives in the player position
    const auto w = anim::worldMatrices(skeleton, local);
    return (vec3(w[55][3]) + vec3(w[80][3])) * 0.5f;
}

vec3 rootTranslation(const anim::Clip& c, double t, bool loop) { return rootChannels(c, t, loop, anim::TransX); }

Player::Player(const collision::CollisionMesh& col, const AnimLibrary& anims, Tuning tuning)
    : col_(col), anims_(anims), t_(tuning) {}

glm::mat4 Player::modelMatrix(vec3 pos, float yaw, float pitch) const {
    // Model -Y is forward: rotate by yaw + 90 degrees about Z; pitch raises -Y towards +Z (about model X).
    const glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1), pos), yaw + kPi / 2, vec3(0, 0, 1));
    return pitch == 0 ? m : glm::rotate(m, -pitch, vec3(1, 0, 0));
}

vec3 Player::forward() const { return {std::cos(s_.yaw), std::sin(s_.yaw), 0}; }

vec3 Player::toWorld(vec3 m) const { return rotYaw(s_.yaw, m); }

float Player::capHeight() const {
    return s_.state == State::Crouch || s_.state == State::Roll ? t_.crouchHeight : t_.height;
}

void Player::play(std::string_view clip, bool loop, float rate) {
    if (s_.clip == clip && s_.loop == loop && s_.clipRate == rate) return;
    // A new clip starts at the original request's start frame (clipBlend); swapping between the looping
    // gaits of one blend space keeps the stride phase instead (walk and run line up at phase 0).
    double t = 0;
    if (s_.clip != clip) {
        const auto gait = [](std::string_view n) {
            return n == "ng_walk" || n == "ng_run" ? 1 : n == "swim_sub" || n == "swim_sub_up" || n == "swim_sub_down" ? 2 : 0;
        };
        const anim::Clip *from = anims_.find(s_.clip), *to = anims_.find(clip);
        t = from && to && s_.loop && loop && gait(clip) && gait(clip) == gait(s_.clip)
                ? anim::syncedStartTime(*from, s_.clipTime, *to)
                : clipBlend(s_.clip, clip).startSeconds;
    }
    s_.clip = clip;
    s_.loop = loop;
    s_.clipRate = rate;
    s_.clipTime = t;
}

bool Player::clipDone() const {
    const anim::Clip* c = anims_.find(s_.clip);
    return !c || (!s_.loop && s_.clipTime >= c->durationSeconds());
}

vec3 Player::advanceClip(float dt) {
    const anim::Clip* c = anims_.find(s_.clip);
    if (!c) return vec3(0);
    const double t0 = s_.clipTime;
    double t1 = t0 + double(s_.clipRate) * dt;
    if (!s_.loop) t1 = std::clamp(t1, 0.0, c->durationSeconds());
    s_.clipTime = t1;
    return rootTranslation(*c, t1, s_.loop) - rootTranslation(*c, t0, s_.loop);
}

// Clips that turn the root (drop_to_ledgeb, bbeam_walk_turn180, ledgeb_jump_back): the yaw carries the root's
// Z rotation (the pose is rendered with the root at bind), and the root translation is in the frame the clip
// started in (yaw minus the rotation so far).
vec3 Player::advanceRoot(float dt) {
    const anim::Clip* c = anims_.find(s_.clip);
    const double t0 = s_.clipTime;
    const vec3 d = advanceClip(dt);
    if (!c) return vec3(0);
    const float base = s_.yaw - rootYaw(*c, t0, s_.loop);
    s_.yaw = wrapAngle(base + rootYaw(*c, s_.clipTime, s_.loop));
    return rotYaw(base, d);
}

void Player::spawn(vec3 feet, float yaw) {
    s_ = PlayerState{};
    s_.pos = feet;
    s_.yaw = yaw;
    if (auto g = col_.groundProbe(feet + vec3(0, 0, t_.stepUp), t_.stepUp + 1000.0f)) s_.pos.z = g->point.z;
    s_.fallStartZ = s_.pos.z;
    play("ng_idle", true);
}

void Player::die(std::string_view clip) {
    s_.state = State::Dead;
    s_.vel = vec3(0);
    play(clip, false);
}

void Player::hitReact(std::string_view clip) {
    s_.state = State::Hit;
    s_.vel = vec3(0);
    s_.clip.clear(); // restart even when the same reaction is playing
    play(clip, false);
}

// Collide-and-slide of a capsule [pos + bottom + r, pos + height - r]. `horizontalNormals` flattens
// contact normals (grounded movement: walls only push sideways; the floor is handled by the probe).
vec3 Player::slide(vec3 pos, vec3 delta, float bottom, bool horizontalNormals) const {
    for (int iter = 0; iter < 4 && glm::dot(delta, delta) > 1e-8f; ++iter) {
        const vec3 a = pos + vec3(0, 0, bottom + t_.radius), b = pos + vec3(0, 0, capHeight() - t_.radius);
        const auto hit = col_.sweepCapsule(a, b, t_.radius, delta);
        vec3 n = hit ? hit->normal : vec3(0);
        if (horizontalNormals) n.z = 0;
        const float nl = glm::length(n);
        // ponytail: an already-touching contact we are moving away from is ignored (the sweep reports
        // touching faces at t = 0); a full depenetration pass would handle overlapping starts.
        if (!hit || nl < 1e-4f || (hit->t == 0 && glm::dot(delta, n) >= 0)) {
            pos += delta;
            break;
        }
        n /= nl;
        pos += delta * hit->t + n * kSkin;
        const vec3 rest = delta * (1 - hit->t);
        delta = rest - n * std::min(0.0f, glm::dot(rest, n));
    }
    return pos;
}

// A floor hit under `p` supports standing: walkable slope, or a steep sliver/seam with a walkable face
// within 20 units of its height 60 units to a side (floor bumps; ragnarok_room's 0.6-normal strips).
// ponytail: four fixed probes; a footprint sweep would catch narrower walkable bits.
bool Player::standable(vec3 p, const collision::Hit& g) const {
    if (g.normal.z >= t_.minFloorNormalZ) return true;
    for (const vec2 o : {vec2(60, 0), vec2(-60, 0), vec2(0, 60), vec2(0, -60)}) {
        const auto h = col_.groundProbe(vec3(p.x + o.x, p.y + o.y, g.point.z + t_.stepUp), t_.stepUp + 20);
        if (h && h->normal.z >= t_.minFloorNormalZ && std::abs(h->point.z - g.point.z) <= 20) return true;
    }
    return false;
}

void Player::groundMove(vec3 delta, float dt) {
    delta.z = 0;
    vec3 p = slide(s_.pos, delta, t_.stepUp, true);
    const auto g = col_.groundProbe(p + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown);
    if (g && standable(p, *g)) {
        p.z = g->point.z;
        s_.pos = p;
        return;
    }
    s_.pos = p; // walked off an edge, or onto a slope too steep to stand on
    const vec3 vel(xy(delta) / dt, 0);
    if (g && g->normal.z >= t_.slideMinNormalZ) {
        s_.pos.z = g->point.z;
        startSlide(vel);
    } else {
        startFall(vel);
    }
}

void Player::startFall(vec3 vel) {
    s_.state = State::Fall;
    s_.vel = vel;
    s_.ledge = -1;
    s_.fallStartZ = s_.pos.z;
    play("NG_InAir_Feet_Forward_stage3", true);
}

void Player::startSlide(vec3 vel) {
    s_.state = State::Slide;
    s_.vel = vec3(xy(vel), 0);
    s_.ledge = -1;
    s_.fallStartZ = s_.pos.z;
    play("slope_slidefront", true);
}

void Player::land(vec2 move) {
    s_.fallHeight = s_.fallStartZ - s_.pos.z;
    if (tryBeam("bbeam_jumpland", true)) return;
    const float speed = glm::length(xy(s_.vel));
    s_.vel = vec3(0);
    if (speed > 600.0f && glm::length(move) > 0.5f) {
        s_.state = State::Run;
        play("ng_running_land", false);
    } else {
        s_.state = State::Land;
        play("ng_generic_land_forwards", false);
    }
}

void Player::airMove(float dt, vec2 move) {
    const vec3 prevPos = s_.pos;
    // tru.exe integrator 0x5DB370: pos += v t + a t^2 / 2; v += a t (constant acceleration, exact for any t).
    // No terminal velocity: the exe never clamps zVel.
    vec3 d = s_.vel * dt + vec3(0, 0, -0.5f * t_.gravity * dt * dt);
    s_.vel.z -= t_.gravity * dt;
    vec3 p = s_.pos;
    // Same sweep loop as slide(), but contacts also clip the velocity (ceilings stop the rise, walls the run-up).
    for (int iter = 0; iter < 4 && glm::dot(d, d) > 1e-8f; ++iter) {
        const auto hit = col_.sweepCapsule(p + vec3(0, 0, t_.radius), p + vec3(0, 0, capHeight() - t_.radius), t_.radius, d);
        if (!hit || (hit->t == 0 && glm::dot(d, hit->normal) >= 0)) { p += d; break; }
        const vec3 n = hit->normal;
        p += d * hit->t + n * kSkin;
        if (n.z >= t_.slideMinNormalZ && s_.vel.z <= 0) {
            // Floor-like contact: land only with a floor under the root. A capsule resting on an edge's corner
            // has none (walking slowly off a ledge used to alternate Fall/Land there): it slides off instead.
            if (auto g = col_.groundProbe(p + vec3(0, 0, t_.radius), t_.radius + kSkin + 2)) {
                s_.pos = vec3(p.x, p.y, g->point.z);
                if (standable(p, *g)) { land(move); return; }
                if (g->normal.z >= t_.slideMinNormalZ) { startSlide(s_.vel); return; }
            }
            if (n.z >= t_.minFloorNormalZ && glm::length(xy(n)) > 1e-4f) {
                const vec2 out = glm::normalize(xy(n));
                const float vo = glm::dot(xy(s_.vel), out);
                if (vo < kEdgePush) s_.vel += vec3(out * (kEdgePush - vo), 0);
            }
        }
        const vec3 rest = d * (1 - hit->t);
        d = rest - n * std::min(0.0f, glm::dot(rest, n));
        s_.vel -= n * std::min(0.0f, glm::dot(s_.vel, n));
    }
    s_.pos = p;
    if (s_.vel.z > 0) return;
    // Falling through a balance beam's line (beams can be thinner than the capsule): stand on it.
    for (const auto& m : col_.markup) {
        if (m.type != 14) continue;
        const OnLine o = closestOn(m, p);
        if (o.seg >= 0 && o.dist <= t_.beamAttach && prevPos.z >= o.q.z - 1 && p.z <= o.q.z + 1) {
            s_.pos = o.q;
            if (tryBeam("bbeam_jumpland", true)) { s_.fallHeight = s_.fallStartZ - s_.pos.z; return; }
            s_.pos = p; // not attachable (cooldown, beside an end): keep falling
        }
    }
    if (auto g = col_.groundProbe(p + vec3(0, 0, t_.radius), t_.radius + kSkin + 2); g && standable(p, *g)) {
        s_.pos.z = g->point.z;
        land(move);
        return;
    }
    if (tryGrab(prevPos)) return;
    tryGrabPole(prevPos);
    tryClimb(true); // ledges and poles win over a rock face
}

// Ledge grab: markup type 1 ("ledgegrab") polylines. The hands (hang pose) must pass the ledge height
// while falling, within grabReach horizontally, with Lara facing the ledge.
bool Player::tryGrab(vec3 prevPos) {
    if (s_.regrabCooldown > 0 || s_.vel.z < t_.grabMinVz || s_.vel.z > t_.grabMaxVz) return false;
    const vec3 hand = s_.pos + toWorld(anims_.hangHandOffset), prevHand = prevPos + toWorld(anims_.hangHandOffset);
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        for (size_t si = 0; si + 1 < m.points.size(); ++si) {
            const vec3 p0 = m.points[si], p1 = m.points[si + 1];
            const vec2 seg = xy(p1 - p0);
            const float len2 = glm::dot(seg, seg);
            if (len2 < 1e-4f) continue;
            const float u = std::clamp(glm::dot(xy(hand - p0), seg) / len2, 0.0f, 1.0f);
            const vec3 q = glm::mix(p0, p1, u);
            if (glm::length(xy(hand - q)) > t_.grabReach) continue;
            if (hand.z > q.z + 10.0f || prevHand.z < q.z - t_.grabWindow || hand.z < q.z - t_.grabWindow) continue;
            // Lara's side of the ledge: the wall is the other side; she must be facing it.
            const vec2 dir = seg / std::sqrt(len2);
            vec2 out(-dir.y, dir.x);
            float side = 1;
            if (glm::dot(xy(s_.pos - q), out) < 0) side = -1, out = -out;
            if (glm::dot(xy(forward()), -out) < 0.5f) continue;
            s_.ledge = int(li);
            s_.ledgeSeg = int(si);
            s_.ledgeT = u * std::sqrt(len2);
            s_.ledgeSide = side;
            s_.vel = vec3(0);
            s_.state = State::Hang;
            play("ledgeb_attach_M", false);
            placeOnLedge();
            return true;
        }
    }
    return false;
}

// Horizontal poles (markup 4 "pole", 6 "horizpole"): falling hands (pole hang pose, Lara turned square to the
// pole) pass the pole axis within poleReach, same height window as ledges.
bool Player::tryGrabPole(vec3 prevPos) {
    if (s_.regrabCooldown > 0 || s_.vel.z < t_.grabMinVz || s_.vel.z > t_.grabMaxVz) return false;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (!isPole(m)) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 dir = segDir(m, si), n(-dir.y, dir.x);
            const float c = glm::dot(xy(forward()), n);
            if (std::abs(c) < 0.5f) continue; // not facing across the pole
            const float side = c > 0 ? 1.0f : -1.0f, yaw = yawOf(n * side);
            const vec3 hand = s_.pos + rotYaw(yaw, anims_.poleHandOffset), prevHand = prevPos + rotYaw(yaw, anims_.poleHandOffset);
            const float t = std::clamp(glm::dot(xy(hand - m.points[size_t(si)]), dir), 0.0f, segLen(m, si));
            const vec3 q = pointOn(m, si, t);
            if (glm::length(xy(hand - q)) > t_.poleReach) continue;
            if (hand.z > q.z + 10.0f || prevHand.z < q.z - t_.grabWindow || hand.z < q.z - t_.grabWindow) continue;
            s_.ledge = int(li), s_.ledgeSeg = si, s_.ledgeT = t, s_.ledgeSide = side;
            s_.vel = vec3(0);
            s_.state = State::PoleHang;
            play("Hpole_perp_attach_M", false);
            placeOnLedge();
            return true;
        }
    }
    return false;
}

// Root and heading for hanging at (markup, seg, t, side). Ledges: facing the wall, hands (hang pose) on the
// ledge; poles: facing along the polyline normal * side, hands (pole pose) on the axis.
std::pair<vec3, float> Player::attachPose(int markup, int seg, float t, float side) const {
    const auto& m = col_.markup[size_t(markup)];
    const vec2 out = vec2(-segDir(m, seg).y, segDir(m, seg).x) * side;
    const bool pole = isPole(m);
    const float yaw = yawOf(pole ? out : -out);
    return {pointOn(m, seg, t) - rotYaw(yaw, pole ? anims_.poleHandOffset : anims_.hangHandOffset), yaw};
}

void Player::placeOnLedge() {
    std::tie(s_.pos, s_.yaw) = attachPose(s_.ledge, s_.ledgeSeg, s_.ledgeT, s_.ledgeSide);
}

// Balance beam (markup 14): the root within beamAttach of the line (and of its height), on the beam's extent;
// from the ground only when heading along it.
bool Player::tryBeam(std::string_view clip, bool anyHeading) {
    if (s_.regrabCooldown > 0) return false;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 14) continue;
        const OnLine o = closestOn(m, s_.pos);
        if (o.seg < 0 || o.dist > t_.beamAttach || std::abs(s_.pos.z - o.q.z) > t_.beamAttach) continue;
        if (o.u < -t_.beamAttach || o.u > segLen(m, o.seg) + t_.beamAttach) continue;
        const vec2 dir = segDir(m, o.seg);
        const float c = glm::dot(xy(forward()), dir);
        if (!anyHeading && std::abs(c) < 0.7f) continue;
        s_.ledge = int(li), s_.ledgeSeg = o.seg, s_.ledgeT = o.t, s_.ledgeSide = c >= 0 ? 1.0f : -1.0f;
        s_.pos = o.q;
        s_.yaw = yawOf(dir * s_.ledgeSide);
        s_.vel = vec3(0);
        s_.state = State::Beam;
        play(clip, false);
        return true;
    }
    return false;
}

// Standing near a ledge (markup 1) at floor height whose far side (away from Lara) drops more than 200 within
// 240 units of the line (ledge lines can sit well inside the collision top: l4_pool 1253 by up to ~180):
// drop_to_ledgeb, which starts facing the drop and turns half round into the hang. Placed so the clip's root
// ends on the hang pose.
bool Player::tryDropToHang() {
    int best = -1;
    OnLine bestOn;
    float bestSide = 1;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        const OnLine o = closestOn(m, s_.pos);
        if (o.seg < 0 || o.dist > 200.0f || o.dist >= bestOn.dist || std::abs(o.q.z - s_.pos.z) > 40.0f) continue;
        const vec2 dir = segDir(m, o.seg), n(-dir.y, dir.x);
        const float side = glm::dot(xy(s_.pos - o.q), n) > 0 ? -1.0f : 1.0f; // hang on the side away from Lara
        if (!openBelow(col_, m, o.seg, o.t, side)) continue;
        best = int(li), bestOn = o, bestSide = side;
    }
    if (best < 0) return false;
    s_.ledge = best, s_.ledgeSeg = bestOn.seg, s_.ledgeT = bestOn.t, s_.ledgeSide = bestSide;
    const auto [hang, hangYaw] = attachPose(s_.ledge, s_.ledgeSeg, s_.ledgeT, bestSide);
    s_.state = State::DropToHang;
    s_.vel = vec3(0);
    play("drop_to_ledgeb", false);
    s_.pos = hang, s_.yaw = hangYaw;
    if (const anim::Clip* c = anims_.find(s_.clip)) {
        const double end = c->durationSeconds();
        const float base = hangYaw - rootYaw(*c, end, false);
        s_.yaw = wrapAngle(base + rootYaw(*c, s_.clipTime, false));
        s_.pos = hang - rotYaw(base, rootTranslation(*c, end, false) - rootTranslation(*c, s_.clipTime, false));
    }
    return true;
}

// Walkable top behind the hanging ledge (within 150 above it).
bool Player::canClimbUp() const {
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 q = pointOn(m, s_.ledgeSeg, s_.ledgeT);
    const auto g = col_.groundProbe(vec3(xy(q) + xy(forward()) * 100.0f, q.z + 300), 400);
    return g && g->normal.z >= t_.minFloorNormalZ && g->point.z > q.z - 30 && g->point.z < q.z + 150;
}

// Targeted jump from a hang (kind 0 up, +-1 right/left, 2 back) to another ledge, or from a pole (kind 3,
// fling forward) to another pole. The target is the attach point in the jump's reach box nearest to a typical
// jump end (400 up / 400 aside / 700 back and 100 down / 1264 ahead: l4_traprooms' pole spacing) [I]; the flight
// is ballistic (exe gravity), rising hangJumpApex above the higher end, timed so the root arrives exactly on
// the target's hang pose (as the exe's jump-to-target solver 0x73EDC0 aims the remaining flight time).
bool Player::startHangJump(int kind) {
    const bool fling = kind == 3;
    const vec3 hands = s_.pos + toWorld(fling ? anims_.poleHandOffset : anims_.hangHandOffset);
    const vec2 f = xy(forward()), r(std::sin(s_.yaw), -std::cos(s_.yaw));
    const vec2 newF = kind == 2 ? -f : f;
    const vec3 ideal = hands + (kind == 0   ? vec3(0, 0, 400)
                                : kind == 2 ? vec3(-f * 700.0f, -100)
                                : fling     ? vec3(f * 1264.0f, 0)
                                            : vec3(r * (400.0f * float(kind)), 0));
    int bestLi = -1, bestSeg = 0;
    float bestT = 0, bestSide = 1, bestDist = 1e30f;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if ((fling ? !isPole(m) : m.type != 1) || int(li) == s_.ledge) continue; // along the same one: shimmy
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 dir = segDir(m, si), n(-dir.y, dir.x);
            if (std::abs(glm::dot(n, newF)) < 0.7f) continue; // the wall / pole must face the new heading
            const float side = glm::dot(n, fling ? newF : -newF) > 0 ? 1.0f : -1.0f;
            const float t = std::clamp(glm::dot(xy(ideal - m.points[size_t(si)]), dir), 0.0f, segLen(m, si));
            const vec3 rel = pointOn(m, si, t) - hands;
            const float lat = glm::dot(xy(rel), r), fw = glm::dot(xy(rel), f), up = rel.z;
            bool ok = false;
            switch (kind) {
            case 0: ok = std::abs(lat) <= 100 && std::abs(fw) <= 100 && up >= 150 && up <= t_.hangJumpUp; break;
            case 1:
            case -1: ok = lat * float(kind) >= 150 && lat * float(kind) <= t_.hangJumpSide && std::abs(fw) <= 150 && std::abs(up) <= 400; break;
            case 2: ok = fw <= -200 && fw >= -t_.hangJumpBack && std::abs(lat) <= 200 && up >= -700 && up <= 200; break;
            default: ok = fw >= 300 && fw <= t_.poleFlingReach && std::abs(lat) <= 300 && std::abs(up) <= 500; break;
            }
            const float score = glm::distance(pointOn(m, si, t), ideal);
            if (ok && score < bestDist && (fling || openBelow(col_, m, si, t, side)))
                bestDist = score, bestLi = int(li), bestSeg = si, bestT = t, bestSide = side;
        }
    }
    if (bestLi < 0) return false;
    const vec3 delta = attachPose(bestLi, bestSeg, bestT, bestSide).first - s_.pos;
    const float g = t_.gravity, vz = std::sqrt(2 * g * (std::max(delta.z, 0.0f) + t_.hangJumpApex));
    const float time = (vz + std::sqrt(vz * vz - 2 * g * delta.z)) / g;
    s_.vel = vec3(xy(delta) / time, vz);
    s_.flight = time;
    s_.ledge = bestLi, s_.ledgeSeg = bestSeg, s_.ledgeT = bestT, s_.ledgeSide = bestSide;
    s_.state = State::HangJump;
    s_.fallStartZ = s_.pos.z;
    // [I] no mirrored clip exists for jumping left; ledgeb_jump_right plays for both sides.
    play(fling ? "hpole_perp_fling" : kind == 0 ? "ledgeb_jump_up" : kind == 2 ? "ledgeb_jump_back" : "ledgeb_jump_right", false);
    return true;
}

// Jump assist (tru.exe 0x73EDC0 retargets in-air velocity at markup with MUD flags 0x20830000: jump landing,
// rail slide, perch, balance beam): of the jump-landing (2) and beam (14) polylines, the point nearest to the
// natural landing at its height, if within assistRadius, becomes the landing; the horizontal velocity is set
// so the ballistic flight (same vertical speed) ends there.
void Player::assistJump() {
    const vec2 v = xy(s_.vel);
    if (glm::length(v) < 1) return;
    const float g = t_.gravity, vz = s_.vel.z;
    const auto timeAt = [&](float z) {
        const float disc = vz * vz - 2 * g * (z - s_.pos.z);
        return disc < 0 ? -1.0f : (vz + std::sqrt(disc)) / g;
    };
    float bestMiss = t_.assistRadius, bestTime = 0;
    vec3 target(0);
    for (const auto& m : col_.markup) {
        if (m.type != 2 && m.type != 14) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            float time = timeAt(0.5f * (m.points[size_t(si)].z + m.points[size_t(si) + 1].z));
            if (time < 0.15f) continue;
            const float t = std::clamp(glm::dot(xy(s_.pos) + v * time - xy(m.points[size_t(si)]), segDir(m, si)), 0.0f, segLen(m, si));
            const vec3 q = pointOn(m, si, t);
            time = timeAt(q.z);
            if (time < 0.15f) continue;
            const float miss = glm::length(xy(q) - (xy(s_.pos) + v * time));
            if (miss < bestMiss) bestMiss = miss, bestTime = time, target = q;
        }
    }
    if (bestTime > 0) s_.vel = vec3((xy(target) - xy(s_.pos)) / bestTime, vz);
}

void Player::tick(vec2 move, const Actions& a, float dt) {
    const bool jumpPressed = a.jump && !jumpHeld_, dropPressed = a.drop && !dropHeld_;
    jumpHeld_ = a.jump;
    dropHeld_ = a.drop;
    s_.fallHeight = 0;
    s_.regrabCooldown = std::max(0.0f, s_.regrabCooldown - dt);
    const float mag = std::min(1.0f, glm::length(move));
    const float target = mag > 0.1f ? std::atan2(move.y, move.x) : s_.yaw;
    const bool wantsMove = mag > 0.1f, wantsWalk = a.walk || mag < 0.5f;

    switch (s_.state) {
    case State::Idle:
    case State::Walk:
    case State::Run:
    case State::Stop:
    case State::Turn:
    case State::Land: {
        if (enterWater()) break; // standing in water deeper than swimDepth
        if (jumpPressed && s_.state != State::Land) {
            s_.runJump = s_.state == State::Run;
            s_.state = State::JumpPrep; // launch velocity is set when the take-off clip ends
            play(s_.runJump ? "ng_running_jump_forwards" : "ng_standing_jump", false);
            break;
        }
        // Drop (crouch button): at an edge drop to hang, running roll, else crouch while held.
        if (dropPressed && s_.state != State::Run && tryDropToHang()) break;
        if (dropPressed && s_.state == State::Run) { s_.state = State::Roll; play("ng_roll_front", false); break; }
        if (a.drop && s_.state != State::Run && s_.state != State::Land) {
            s_.state = State::Crouch;
            play("crouch_idleToCrouch", false);
            break;
        }
        const float diff = std::abs(wrapAngle(target - s_.yaw));
        switch (s_.state) {
        case State::Idle:
            if (!wantsMove) break;
            if (diff > 1.75f) { s_.state = State::Turn; play("ng_turn_center", true); }
            else if (wantsWalk) { s_.state = State::Walk; play("ng_walk", true); }
            else { s_.state = State::Run; play("ng_run_start", false); }
            break;
        case State::Turn: // yaw follows the turn clip's root rotation (applied after the clip advances)
            if (!wantsMove) { s_.state = State::Idle; play("ng_idle", true); }
            else if (diff < 0.2f) { s_.state = State::Idle; play("ng_idle", true); }
            break;
        case State::Walk:
            s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            if (!wantsMove) { s_.state = State::Idle; play("ng_idle", true); }
            else if (!wantsWalk) { s_.state = State::Run; play("ng_run", true); }
            break;
        case State::Run:
            s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            if (!wantsMove) { s_.state = State::Stop; play("ng_running_stop", false); }
            else if (wantsWalk) { s_.state = State::Walk; play("ng_walk", true); }
            else if ((s_.clip == "ng_run_start" || s_.clip == "ng_running_land") && clipDone()) play("ng_run", true);
            break;
        case State::Stop:
            if (wantsMove && !wantsWalk) { s_.state = State::Run; play("ng_run", true); }
            else if (clipDone()) { s_.state = State::Idle; play("ng_idle", true); }
            break;
        case State::Land:
            if (clipDone()) { s_.state = State::Idle; play("ng_idle", true); }
            break;
        default: break;
        }
        const double t0 = s_.clipTime;
        const vec3 d = advanceClip(dt);
        if (s_.state == State::Turn) {
            // ng_turn_center: root Z rotation 0 -> -1.57 rad (a quarter turn right) over its keys; played
            // either way, the rotation magnitude per tick is the turn rate. Without the clip: turnRate.
            const float rest = wrapAngle(target - s_.yaw);
            const anim::Clip* c = anims_.find(s_.clip);
            const float step = c ? std::abs(rootYaw(*c, s_.clipTime, s_.loop) - rootYaw(*c, t0, s_.loop)) : t_.turnRate * dt;
            s_.yaw = wrapAngle(s_.yaw + std::copysign(std::min(step, std::abs(rest)), rest));
        }
        groundMove(toWorld(d), dt);
        if (s_.state == State::Idle || s_.state == State::Walk || s_.state == State::Run || s_.state == State::Stop)
            tryBeam("bbeam_walk_attach", false);
        // Pushing into a climbable rock face starts climbing (a jump keeps its normal arc).
        if (wantsMove && glm::dot(move, xy(forward())) > 0.5f) tryClimb(false);
        break;
    }
    case State::Crouch: {
        const bool headroom = !col_.sweepCapsule(s_.pos + vec3(0, 0, t_.stepUp + t_.radius), s_.pos + vec3(0, 0, t_.crouchHeight - t_.radius),
                                                 t_.radius, vec3(0, 0, t_.height - t_.crouchHeight));
        if (!a.drop && headroom) { s_.state = State::Idle; play("ng_idle", true); break; }
        if (wantsMove) {
            s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            play("Crouch_Front", true);
        } else if (s_.clip != "crouch_idleToCrouch" || clipDone()) {
            play("crouch_Idle", true);
        }
        groundMove(toWorld(advanceClip(dt)), dt);
        break;
    }
    case State::Roll:
        groundMove(toWorld(advanceClip(dt)), dt);
        if (s_.state == State::Roll && clipDone()) {
            if (wantsMove && !wantsWalk) { s_.state = State::Run; play("ng_run", true); }
            else { s_.state = State::Idle; play("ng_idle", true); }
        }
        break;
    case State::Slide: {
        // Down the fall line of a floor steeper than minFloorNormalZ: accelerate to slideMaxSpeed [I]; jump leaps
        // off forward; a standable floor ends it, nothing below falls.
        advanceClip(dt);
        s_.fallStartZ = s_.pos.z; // sliding is not falling (no fall damage on the run-out)
        if (jumpPressed) {
            s_.state = State::Jump;
            s_.vel = vec3(xy(s_.vel), t_.jumpVz);
            play("NG_InAir_Feet_Forward_stage1", false);
            break;
        }
        const auto g = col_.groundProbe(s_.pos + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown);
        if (!g) { startFall(s_.vel); break; }
        if (standable(s_.pos, *g)) { land(move); break; }
        const float dl = glm::length(xy(g->normal));
        if (dl < 1e-4f) { land(move); break; }
        const vec2 dir = xy(g->normal) / dl; // the normal leans downhill
        s_.yaw = yawOf(dir);
        const float speed = std::clamp(glm::dot(xy(s_.vel), dir) + t_.slideAccel * dt, 0.0f, t_.slideMaxSpeed);
        s_.vel = vec3(dir * speed, 0);
        vec3 p = slide(s_.pos, s_.vel * dt, t_.stepUp, true);
        const auto g2 = col_.groundProbe(p + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown + speed * dt * 3);
        s_.pos = p;
        if (!g2) { startFall(s_.vel); break; }
        s_.pos.z = g2->point.z;
        break;
    }
    case State::Beam:
        beam(move, jumpPressed, dropPressed, dt);
        break;
    case State::Climb:
        climb(move, jumpPressed, dropPressed, dt);
        break;
    case State::GrappleThrow:
    case State::GrappleSwing:
        grapple(move, a, jumpPressed, dropPressed, dt);
        break;
    case State::DropToHang:
        s_.pos += advanceRoot(dt); // full root motion and turn, no collision (as ClimbUp)
        if (clipDone()) {
            s_.state = State::Hang;
            play("ledgeb_idle", true);
            placeOnLedge();
        }
        break;
    case State::JumpPrep: {
        const bool running = s_.runJump;
        const vec3 d = toWorld(advanceClip(dt));
        if (running) groundMove(d, dt);
        if (s_.state != State::JumpPrep || !clipDone()) break; // fell off during the take-off, or still crouching
        const float h = running ? t_.runSpeed : wantsMove ? t_.standJumpSpeed : 0.0f;
        if (wantsMove && !running) s_.yaw = target;
        s_.vel = forward() * h + vec3(0, 0, running ? t_.runJumpVz : t_.jumpVz);
        s_.state = State::Jump;
        s_.fallStartZ = s_.pos.z;
        assistJump();
        play(running ? "NG_InAir_Feet_Forward_stage1" : "NG_InAir_Feet_Forward_stage1_standing", false);
        break;
    }
    case State::Jump:
    case State::Fall:
        if (s_.clip == "ledgeb_jump_back") advanceRoot(dt); // its half turn; air clips have no root motion
        else advanceClip(dt);
        if (s_.state == State::Jump && clipDone())
            play(s_.clip == "NG_InAir_Feet_Forward_stage2" ? "NG_InAir_Feet_Forward_stage3" : "NG_InAir_Feet_Forward_stage2",
                 s_.clip == "NG_InAir_Feet_Forward_stage2");
        airMove(dt, move);
        if ((s_.state == State::Jump || s_.state == State::Fall) && s_.vel.z <= 0) enterWater();
        break;
    case State::HangJump: {
        if (s_.ledge < 0) { startFall(s_.vel); break; } // target lost (loaded mid-flight)
        if (s_.clip == "ledgeb_jump_back") advanceRoot(dt);
        else advanceClip(dt);
        const float step = std::min(dt, s_.flight);
        s_.pos += s_.vel * step + vec3(0, 0, -0.5f * t_.gravity * step * step);
        s_.vel.z -= t_.gravity * step;
        s_.flight -= step;
        if (s_.flight > 0) break;
        const bool pole = isPole(col_.markup[size_t(s_.ledge)]);
        const std::string_view attach = pole                              ? "Hpole_perp_attach_M"
                                        : s_.clip == "ledgeb_jump_up"   ? "ledgeb_attach_U"
                                        : s_.clip == "ledgeb_jump_back" ? "ledgeb_attach_F"
                                                                         : "ledgeb_attach_S";
        s_.vel = vec3(0);
        s_.state = pole ? State::PoleHang : State::Hang;
        play(attach, false);
        placeOnLedge();
        break;
    }
    case State::Hang:
    case State::Shimmy: {
        const vec2 right(std::sin(s_.yaw), -std::cos(s_.yaw));
        const float lateral = glm::dot(move, right);
        if (dropPressed) {
            s_.pos -= forward() * 20.0f; // off the wall
            s_.regrabCooldown = 0.4f;
            startFall(vec3(0));
            break;
        }
        if (jumpPressed) {
            const float fw = glm::dot(move, xy(forward()));
            const int kind = std::abs(lateral) > 0.5f ? (lateral > 0 ? 1 : -1) : fw < -0.5f ? 2 : 0;
            const auto climb = [&] {
                s_.state = State::ClimbUp;
                play("ledgeb_climbup_fast", false);
                // Start where the clip expects the ledge: its hands (at the start time) on the ledge in x/y, and
                // its root at the clip end on the ledge top (end - start rise); nothing to snap at the end.
                const anim::Clip* c = anims_.find(s_.clip);
                if (const auto h = c ? anims_.handsAt(*c, s_.clipTime) : std::nullopt) {
                    const float rise = rootTranslation(*c, c->durationSeconds(), false).z - rootTranslation(*c, s_.clipTime, false).z;
                    const vec3 hang = anims_.hangHandOffset;
                    s_.pos += toWorld({hang.x - h->x, hang.y - h->y, hang.z - rise});
                }
            };
            if (kind == 0 && canClimbUp()) climb();
            else if (startHangJump(kind)) {}
            else if (kind == 2) { // nothing behind: leap off backwards (ledgeb_jump_back turns her half round)
                s_.vel = -forward() * t_.standJumpSpeed + vec3(0, 0, t_.jumpVz);
                s_.ledge = -1;
                s_.regrabCooldown = 0.3f;
                s_.state = State::Jump;
                s_.fallStartZ = s_.pos.z;
                play("ledgeb_jump_back", false);
            } else if (kind == 0) climb();
            break;
        }
        if (s_.state == State::Hang && s_.clip.starts_with("ledgeb_attach") && !clipDone()) { advanceClip(dt); break; }
        if (std::abs(lateral) > 0.3f) {
            s_.state = State::Shimmy;
            play("ledgeb_right", true, lateral > 0 ? 1.0f : -1.0f);
        } else {
            s_.state = State::Hang;
            play("ledgeb_idle", true);
        }
        const vec3 d = toWorld(advanceClip(dt));
        if (s_.state == State::Shimmy) {
            // Move along the polyline by the root motion's component along the ledge; its ends stop the shimmy.
            const auto& m = col_.markup[size_t(s_.ledge)];
            moveAlong(m, s_.ledgeSeg, s_.ledgeT, glm::dot(xy(d), segDir(m, s_.ledgeSeg)));
        }
        placeOnLedge();
        break;
    }
    case State::PoleHang: {
        if (dropPressed) {
            s_.regrabCooldown = 0.4f;
            startFall(vec3(0));
            break;
        }
        if (jumpPressed) { // fling: to the next pole in reach, else a free leap forward [I] (run-jump speeds)
            if (!startHangJump(3)) {
                s_.ledge = -1;
                s_.regrabCooldown = 0.3f;
                s_.vel = forward() * t_.runSpeed + vec3(0, 0, t_.runJumpVz);
                s_.state = State::Jump;
                s_.fallStartZ = s_.pos.z;
                play("hpole_perp_fling", false);
            }
            break;
        }
        if (s_.clip == "Hpole_perp_attach_M" && !clipDone()) { advanceClip(dt); placeOnLedge(); break; }
        const bool swinging = s_.clip == "HPole_StartSwing" || s_.clip == "HPole_swing";
        if (glm::dot(move, xy(forward())) > 0.5f) {
            if (!swinging) play("HPole_StartSwing", false);
            else if (s_.clip == "HPole_StartSwing" && clipDone()) play("HPole_swing", true);
        } else if (swinging) {
            play("Hpole_swingstopA", false);
        } else if (s_.clip != "Hpole_swingstopA" || clipDone()) {
            play("HPole_idleHangPerp", true);
        }
        advanceClip(dt);
        placeOnLedge();
        break;
    }
    case State::ClimbUp:
        s_.pos += toWorld(advanceClip(dt)); // full root motion, no collision: the clip lifts her over the edge
        if (clipDone()) {
            s_.ledge = -1;
            if (auto g = col_.groundProbe(s_.pos + vec3(0, 0, t_.stepUp + 100.0f), t_.stepUp + 100.0f + t_.stepDown + 100.0f)) {
                s_.pos.z = g->point.z;
                s_.state = State::Idle;
                play("ng_idle", true);
            } else {
                startFall(vec3(0));
            }
        }
        break;
    case State::SwimSplash:
    case State::SwimTread:
    case State::SwimSurface:
    case State::SwimDive:
    case State::SwimUnder:
    case State::Surfacing:
    case State::SwimClimbOut:
        swim(move, a, jumpPressed, dropPressed, dt);
        break;
    case State::Hit: // hit reaction clip with its root motion, then back to idle / the swim state
        if (s_.water >= 0) {
            s_.pos = swimSlide(s_.pos, toWorld(advanceClip(dt)));
            if (clipDone()) {
                s_.state = s_.pos.z < surfaceZ() - 1 ? State::SwimUnder : State::SwimTread;
                play(s_.state == State::SwimUnder ? "swim_submerged_idle" : "swim_surfaceidle", true);
            }
        } else {
            groundMove(toWorld(advanceClip(dt)), dt);
            if (s_.state == State::Hit && clipDone()) { s_.state = State::Idle; play("ng_idle", true); }
        }
        break;
    case State::Dead: // the death clip plays out where she fell; World respawns her (Combat.cpp)
        advanceClip(dt);
        break;
    }
}

// Balance beam: the stick along the walking direction walks (bbeam_walk root motion projected on the line),
// against it turns round (bbeam_walk_turn180); joints of the polyline are followed. At an end: another beam
// starting there takes over, a floor beyond is stepped onto, else she stops. Jump = standing jump (assisted
// back onto the beam), drop = falls off to her right [I].
void Player::beam(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    const auto& m = col_.markup[size_t(s_.ledge)];
    if (jumpPressed) {
        s_.ledge = -1;
        s_.runJump = false;
        s_.state = State::JumpPrep;
        play("ng_standing_jump", false);
        return;
    }
    if (dropPressed) {
        const vec2 right(std::sin(s_.yaw), -std::cos(s_.yaw));
        s_.regrabCooldown = 0.4f;
        startFall(vec3(right * 200.0f, 0));
        return;
    }
    if (s_.clip == "bbeam_walk_turn180") {
        if (!clipDone()) advanceRoot(dt);
        if (!clipDone()) return;
        s_.ledgeSide = -s_.ledgeSide;
        play("bbeam_idle", true);
    }
    const vec2 dir = segDir(m, s_.ledgeSeg) * s_.ledgeSide;
    const float along = glm::length(move) > 0.1f ? glm::dot(glm::normalize(move), dir) : 0.0f;
    if (along < -0.5f) {
        play("bbeam_walk_turn180", false);
        return;
    }
    if (along > 0.3f) play("bbeam_walk", true);
    else if ((s_.clip != "bbeam_walk_attach" && s_.clip != "bbeam_jumpland") || clipDone()) play("bbeam_idle", true);
    const float ds = -advanceClip(dt).y; // model -Y = forward along the beam
    const bool end = moveAlong(m, s_.ledgeSeg, s_.ledgeT, ds * s_.ledgeSide) && ds > 0;
    s_.pos = pointOn(m, s_.ledgeSeg, s_.ledgeT);
    const vec2 d = segDir(m, s_.ledgeSeg) * s_.ledgeSide;
    s_.yaw = yawOf(d);
    if (!end) return;
    for (size_t j = 0; j < col_.markup.size(); ++j) {
        const auto& o = col_.markup[j];
        if (o.type != 14 || int(j) == s_.ledge || o.points.size() < 2) continue;
        for (const bool first : {true, false}) {
            const vec3 e = first ? o.points.front() : o.points.back();
            if (glm::distance(e, s_.pos) > 60.0f) continue;
            s_.ledge = int(j);
            s_.ledgeSeg = first ? 0 : int(o.points.size()) - 2;
            s_.ledgeT = first ? 0 : segLen(o, s_.ledgeSeg);
            s_.ledgeSide = first ? 1.0f : -1.0f; // walk away from the joint
            s_.pos = e;
            s_.yaw = yawOf(segDir(o, s_.ledgeSeg) * s_.ledgeSide);
            return;
        }
    }
    if (auto g = col_.groundProbe(s_.pos + vec3(d * 80.0f, t_.stepUp), t_.stepUp + t_.stepDown); g && standable(s_.pos + vec3(d * 80.0f, 0), *g)) {
        s_.ledge = -1;
        s_.regrabCooldown = 0.5f; // no re-attach while stepping off the end
        s_.state = State::Walk;
        play("ng_walk", true);
        return;
    }
    play("bbeam_idle", true);
}

// ---- water ----------------------------------------------------------------------------------
// tru.exe 0x710A20: the player's probe box against every water volume box; of the overlapping volumes the
// one with the highest surface wins (the exe first compares a priority short we do not have).
int Player::waterAt(vec3 feet) const {
    const vec3 c = feet + vec3(0, 0, t_.waterProbeCenterZ), h = t_.waterProbeHalf;
    int best = -1;
    for (size_t i = 0; i < col_.markup.size(); ++i) {
        const auto& m = col_.markup[i];
        if (m.type != 10 || m.points.size() != 2) continue;
        if (glm::any(glm::greaterThan(c - h, m.points[1])) || glm::any(glm::lessThan(c + h, m.points[0]))) continue;
        if (best < 0 || m.points[1].z > col_.markup[size_t(best)].points[1].z) best = int(i);
    }
    return best;
}

float Player::surfaceZ() const { return col_.markup[size_t(s_.water)].points[1].z; }

bool Player::enterWater() {
    const int w = waterAt(s_.pos);
    if (w < 0) return false;
    const float surf = col_.markup[size_t(w)].points[1].z;
    if (s_.pos.z > surf) return false;
    if (auto floor = col_.groundProbe(vec3(s_.pos.x, s_.pos.y, surf), t_.swimDepth); floor) return false; // shallow: walk
    const bool airborne = s_.state == State::Jump || s_.state == State::Fall;
    s_.water = w;
    s_.pitch = 0;
    s_.ledge = -1;
    if (airborne) { // keep the plunge velocity; water drag stops it
        s_.state = State::SwimSplash;
        play("swim_jump_land", false);
    } else {
        s_.vel = vec3(0);
        s_.state = State::Surfacing;
        play("swim_sub_up", true);
    }
    return true;
}

// Sphere (swimRadius) around the swimming root, slid along contacts; never above the water surface.
vec3 Player::swimSlide(vec3 pos, vec3 delta) const {
    for (int iter = 0; iter < 4 && glm::dot(delta, delta) > 1e-8f; ++iter) {
        const auto hit = col_.sweepSphere(pos, t_.swimRadius, delta);
        if (!hit || (hit->t == 0 && glm::dot(delta, hit->normal) >= 0)) { pos += delta; break; }
        pos += delta * hit->t + hit->normal * kSkin;
        const vec3 rest = delta * (1 - hit->t);
        delta = rest - hit->normal * std::min(0.0f, glm::dot(rest, hit->normal));
    }
    pos.z = std::min(pos.z, surfaceZ());
    return pos;
}

// At the surface, facing a wall whose top is 50..400 above the water: swim_climbout. Its root rises 172.8
// and moves 308.7 forward; the start is placed so its hands (at the start time) reach the edge and its
// root ends on the top.
bool Player::tryClimbOut() {
    const anim::Clip* c = anims_.find("swim_climbout");
    if (!c) return false;
    const float surf = surfaceZ(), reach = 300.0f;
    const auto wall = col_.sweepSphere(s_.pos, t_.swimRadius, forward() * reach);
    if (!wall || std::abs(wall->normal.z) > 0.5f || glm::dot(xy(wall->normal), xy(forward())) > -0.5f) return false;
    const vec3 edge = wall->point;
    const auto top = col_.groundProbe(vec3(xy(edge) + xy(forward()) * 50.0f, surf + 400.0f), 400.0f);
    if (!top || top->normal.z < t_.minFloorNormalZ || top->point.z - surf < 50.0f) return false;
    play("swim_climbout", false);
    const vec3 hands = anims_.handsAt(*c, s_.clipTime).value_or(vec3(0, -205.4f, 164.1f));
    const float rise = rootTranslation(*c, c->durationSeconds(), false).z - rootTranslation(*c, s_.clipTime, false).z;
    s_.pos = vec3(xy(edge) - xy(forward()) * -hands.y, top->point.z - rise);
    s_.state = State::SwimClimbOut;
    s_.vel = vec3(0);
    return true;
}

void Player::swim(vec2 move, const Actions& a, bool jumpPressed, bool dropPressed, float dt) {
    const float mag = std::min(1.0f, glm::length(move));
    const bool wantsMove = mag > 0.1f;
    const float target = wantsMove ? std::atan2(move.y, move.x) : s_.yaw;
    auto standUp = [&](float floorZ) {
        s_.pos.z = floorZ;
        s_.water = -1;
        s_.pitch = 0;
        s_.vel = vec3(0);
        s_.state = State::Idle;
        s_.fallStartZ = floorZ;
        play("ng_idle", true);
    };
    const float surf = surfaceZ();
    switch (s_.state) {
    case State::SwimSplash: { // tru.exe SwimSplashDownState; drag constant [I]
        advanceClip(dt);
        s_.vel *= std::exp(-3.0f * dt);
        s_.pos = swimSlide(s_.pos, s_.vel * dt);
        if (clipDone() || glm::length(s_.vel) < 50.0f) {
            s_.vel = vec3(0);
            s_.state = s_.pos.z < surf - 1 ? State::Surfacing : State::SwimTread;
            play(s_.state == State::Surfacing ? "swim_sub_up" : "swim_surfaceidle", true);
        }
        break;
    }
    case State::Surfacing: // rise straight up at swim speed; drop takes control under water
        advanceClip(dt);
        s_.pos = swimSlide(s_.pos, vec3(0, 0, anims_.swimSpeed * dt));
        if (s_.pos.z >= surf) { s_.state = State::SwimTread; play("swim_surfaceidle", true); }
        else if (dropPressed) { s_.state = State::SwimUnder; }
        break;
    case State::SwimTread:
    case State::SwimSurface: {
        s_.pos.z = surf;
        s_.pitch += std::clamp(-s_.pitch, -t_.turnRate * dt, t_.turnRate * dt); // level out after surfacing
        if (jumpPressed && tryClimbOut()) break;
        if (dropPressed) { s_.state = State::SwimDive; s_.pitch = 0; play("swim_surfacedive", false); break; }
        if (wantsMove) {
            s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            if (s_.state == State::SwimTread) { s_.state = State::SwimSurface; play("swim_surface", true); }
        } else if (s_.state == State::SwimSurface) {
            s_.state = State::SwimTread;
            play("swim_surfaceidle", true);
        }
        advanceClip(dt);
        if (s_.state == State::SwimSurface) s_.pos = swimSlide(s_.pos, forward() * (anims_.swimSpeed * mag * dt));
        s_.pos.z = surf;
        // Shallow water: stand on the floor.
        if (auto floor = col_.groundProbe(s_.pos, t_.swimDepth); floor && floor->normal.z >= t_.minFloorNormalZ) standUp(floor->point.z);
        break;
    }
    case State::SwimDive: // swim_surfacedive root motion: 112 forward, 667 down
        s_.pos = swimSlide(s_.pos, toWorld(advanceClip(dt)));
        if (clipDone()) { s_.state = State::SwimUnder; s_.pitch = 0; play("swim_submerged_idle", true); }
        break;
    case State::SwimUnder: {
        // Stick swims horizontally (camera-relative), jump / drop held swim up / down; the swim direction's
        // pitch turns towards the wish at turnRate. [I] the original's under-water control scheme. The model is
        // pitched by the swim pitch (Player::modelMatrix), so the level stroke swim_sub plays at every pitch:
        // its body is level (8.8 deg), swim_sub_up / _down tilt it +35 / -39 deg themselves and would double it.
        const float h = wantsMove ? mag : 0.0f, v = float(a.jump) - float(a.drop);
        if (h > 0 || v != 0) {
            if (wantsMove) s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            const float wish = std::atan2(v, h);
            s_.pitch += std::clamp(wish - s_.pitch, -t_.turnRate * dt, t_.turnRate * dt);
            const vec3 dir = forward() * std::cos(s_.pitch) + vec3(0, 0, std::sin(s_.pitch));
            s_.pos = swimSlide(s_.pos, dir * (anims_.swimSpeed * std::min(1.0f, std::hypot(h, v)) * dt));
            play("swim_sub", true);
        } else {
            s_.pitch += std::clamp(-s_.pitch, -t_.turnRate * dt, t_.turnRate * dt);
            play("swim_submerged_idle", true);
        }
        advanceClip(dt);
        if (s_.pos.z >= surf) {
            s_.state = State::SwimTread;
            play("swim_surfaceidle", true);
        }
        break;
    }
    case State::SwimClimbOut: // full root motion, no collision (as ClimbUp)
        s_.pos += toWorld(advanceClip(dt));
        if (clipDone()) {
            if (auto g = col_.groundProbe(s_.pos + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown)) standUp(g->point.z);
            else standUp(s_.pos.z);
        }
        break;
    default: break;
    }
}

} // namespace ou::game
