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
// Surface swim turning: at most this / her speed rad/s (tru.exe under Wine, l1_seatop sea: 117.5 deg/s while she lags
// the stick at full swim speed 1348.6 = 2765 units/s^2; faster when slow) [O-runtime; the 1 / speed form [I]].
constexpr float kSwimTurnAccel = 2765.0f;
// Poles, tru.exe under Wine (2026-11-06, docs/GAMEPLAY.md "Poles"): the catch hands over to the idle
// hang after kPoleAttachIdle; a fling leaves the pole kPoleFlingDelay after the press (0.64 s twice, 0.31 / 0.81 s
// without a target: [I] the phase rule); q33's take-off (73.72 forward, 52.10 up a 30 Hz frame, lara.drm query
// table); a targeted fling reaches its target kFlingFrames (30 Hz) after leaving; across -> along turns in 0.30 s.
constexpr float kPoleAttachIdle = 0.83f, kPoleFlingDelay = 0.64f; // attach: clip time (Hpole_perp_attach_M starts at frame 4)
constexpr float kFlingForward = 73.72f * 30.0f, kFlingUp = 52.10f * 30.0f, kFlingFrames = 23.5f;
constexpr float kPerpToParaRate = 1.5f / 0.30f;
// Standing on a pole: the root kPoleTopRise over its axis (hpole_to_perch 638.4 up from the hang, hands 619.4 over its
// root; Wine l4_traprooms 1336: 19.9) [O]; landing on it from the air within kPoleTopLand of the axis [I].
constexpr float kPoleTopRise = 19.0f, kPoleTopLand = 40.0f;
constexpr float kSkin = 1.0f;       // gap kept to surfaces after a sweep, units
constexpr float kEdgePush = 200.0f; // units/s: a capsule resting on an edge corner slides off at least this fast [I]
// Balance beams off balance (docs/GAMEPLAY.md "Balance beams"). The about-to-fall states' balance tuning [O]: lara.drm
// objdata +0xF88 (the record 0x72A430 picks for state property 0x209 = 0x191, chart S895 / S896): tolerance 15 and fall
// angle 100 degrees, wiggle filter (smoothing 0.35, period 10 frames, penalty 1), grace 20 frames, not-improving 30,
// unbalanced 45, recovery 2 and full-lean 10 frames, flags 3 (bit 0: re-aim the sway on recovery, bit 1: the side
// holds once leaning); its sway rates are 0 (no sway). Objdata +0xE94 bit 3 is set: the fall flag needs a full lean.
// Lean target = 2 x the weight + 2 x the unbalanced time (objdata +0xE78 / +0xE7C = 2, 2) [O]; ours divides that time
// by 30 frames and moves the lean 0.1 a frame (the record's +0x28) [I: the exe's spring 0x7431F1..0x7437D5 is not
// reproduced]. Frames are tru.exe's 30 Hz ones (gameTracker_timeMult).
struct BalanceTuning {
    float tol, fall, smooth, period, penalty, grace, notImprove, unbal, recover, full, leanRate, leanTime;
    bool sideLock;
};
constexpr BalanceTuning kAboutFall{0.2617994f, 1.7453293f, 0.35f, 10, 1, 20, 30, 45, 2, 10, 0.1f, 30, true};
constexpr float kBeamTipFrames = 45; // ours [I]: the stick held across the beam this long tips her (the record's 45)
// Falling off: tru.exe's fall states (S890 / S893, action 416 = 0x76D4D0) run query 64 from her root moved 47.4075
// to the fall side (prop 912 = 452: +90 degrees, 453: -90) [O]; q64: 0.32 out + 0.32 up a frame, falling, gravity
// 5.72443, cone 90 degrees, from frame 1 [O-data]; a ledge (markup flag 610) found -> bbeam_fall_<side> then the
// ledge hang (S526 / S527), none -> bbeam_fall_<side>_no_ledge then the fall (S894 -> S1012) [O-data].
constexpr float kBeamFallOffset = 47.4075f, kBeamFallQueryV = 0.32f;
constexpr float kBeamCatchFrames = 10, kBeamCatchReach = 200; // ours [I]: catches the clip can carry her to

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
// tru.exe's jump-to-target solver 0x73EDC0: the velocity that carries the root by `delta` in `time` under gravity g.
vec3 jumpVelocity(vec3 delta, float time, float g) { return vec3(xy(delta), delta.z + 0.5f * g * time * time) / time; }

bool swimSprintClip(std::string_view c) { return c == "swim_surface_sprint_forward" || c == "swim_submerged_sprint"; }
bool swimExhaustedClip(std::string_view c) { return c == "swim_surface_exhausted" || c == "swim_submerged_exhausted"; }
// Take-off clips of hang leaps whose root motion (and turn) plays before the launch (the wall-hug ones: lara.drm
// S272 / S273 / S274 / S1258, Traverse.cpp Player::wallHugJump).
bool hangTakeOff(std::string_view c) {
    return c == "ledgeb_jump_back" || c == "ledgeb_jump_right" || c == "ledgeb_jump_right_from_antic" || c == "ladder_jump_left" ||
           c == "ladder_jump_right" || c == "Wall_Hug_Jump_Forward" || c == "Wall_Hug_jump_left" || c == "Wall_hug_Jump_Up_Twist";
}

} // namespace

std::string_view stateName(State s) {
    static constexpr std::string_view k[] = {"idle", "walk", "run", "stop", "turn", "jumpprep", "jump",
                                             "fall", "land", "hang", "shimmy", "climbup", "swimsplash",
                                             "swimtread", "swimsurface", "swimdive", "swimunder", "surfacing",
                                             "swimclimbout", "dead", "crouch", "roll", "slide", "beam", "droptohang",
                                             "hangjump", "polehang", "climb", "ladder", "perch", "pillar", "grapplethrow",
                                             "grappleswing", "hit", "railslide", "grapplepull", "vehicle", "grapplerappel",
                                             "use", "block", "wallhug", "melee", "knocked"};
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

glm::mat4 Player::modelMatrix(vec3 pos, float yaw, float pitch, float roll) const {
    // Model -Y is forward: rotate by yaw + 90 degrees about Z; pitch raises -Y towards +Z (about model X); roll lowers +X
    // (about model Y).
    glm::mat4 m = glm::rotate(glm::translate(glm::mat4(1), pos), yaw + kPi / 2, vec3(0, 0, 1));
    if (pitch != 0) m = glm::rotate(m, -pitch, vec3(1, 0, 0));
    return roll == 0 ? m : glm::rotate(m, roll, vec3(0, 1, 0));
}

vec3 Player::forward() const { return {std::cos(s_.yaw), std::sin(s_.yaw), 0}; }

vec3 Player::toWorld(vec3 m) const { return rotYaw(s_.yaw, m); }

float Player::capHeight() const {
    return s_.state == State::Crouch || s_.state == State::Roll ? t_.crouchHeight : t_.capsuleTop;
}

void Player::play(std::string_view clip, bool loop, float rate, bool mirror) {
    if (s_.clip == clip && s_.loop == loop && s_.clipRate == rate && s_.mirror == mirror) return;
    // A new clip starts at the original request's start frame (clipBlend); swapping between the looping
    // gaits of one blend space keeps the stride phase instead (walk and run line up at phase 0).
    double t = 0;
    if (s_.clip != clip) {
        const auto gait = [](std::string_view n) {
            return n == "ng_walk" || n == "ng_run" || n == "sprint_loop" ? 1 : n == "swim_sub" || n == "swim_sub_up" || n == "swim_sub_down" ? 2 : 0;
        };
        const anim::Clip *from = anims_.find(s_.clip), *to = anims_.find(clip);
        t = from && to && s_.loop && loop && gait(clip) && gait(clip) == gait(s_.clip)
                ? anim::syncedStartTime(*from, s_.clipTime, *to)
                : clipBlend(s_.clip, clip).startSeconds;
    }
    s_.clip = clip;
    s_.loop = loop;
    s_.clipRate = rate;
    s_.mirror = mirror;
    s_.clipTime = t;
    s_.clipMix.clear();
    s_.mix = 0;
}

void Player::playMix(std::string_view clip, std::string_view other, float mix) {
    play(clip, false);
    if (mix <= 0 || !anims_.find(other)) return;
    s_.clipMix = other;
    s_.mix = std::min(mix, 1.0f);
}

// Root motion of the clip between t0 and t1, its blend-space partner phase-synced and weighted in.
static vec3 rootDelta(const AnimLibrary& anims, const PlayerState& s, const anim::Clip& c, double t0, double t1) {
    vec3 d = rootTranslation(c, t1, s.loop) - rootTranslation(c, t0, s.loop);
    if (const anim::Clip* m = s.mix > 0 ? anims.find(s.clipMix) : nullptr) {
        const double k = m->durationSeconds() / c.durationSeconds();
        d = glm::mix(d, rootTranslation(*m, t1 * k, s.loop) - rootTranslation(*m, t0 * k, s.loop), s.mix);
    }
    if (s.mirror) d.x = -d.x; // tru.exe 0x441FB0: a mirrored request's root translation x negated
    return d;
}

vec3 Player::rootLeft() const {
    const anim::Clip* c = anims_.find(s_.clip);
    return c ? rootDelta(anims_, s_, *c, s_.clipTime, c->durationSeconds()) : vec3(0);
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
    return rootDelta(anims_, s_, *c, t0, t1);
}

// Clips that turn the root (drop_to_ledgeb, bbeam_walk_turn180, ledgeb_jump_back): the yaw carries the root's
// Z rotation (the pose is rendered with the root at bind), and the root translation is in the frame the clip
// started in (yaw minus the rotation so far).
vec3 Player::advanceRoot(float dt) {
    const anim::Clip* c = anims_.find(s_.clip);
    const double t0 = s_.clipTime;
    const vec3 d = advanceClip(dt);
    if (!c) return vec3(0);
    // A mirrored request turns the other way: tru.exe negates the root quaternion's y / z (0x4428A0), i.e. the yaw.
    const float k = s_.mirror ? -1.0f : 1.0f;
    const float base = s_.yaw - k * rootYaw(*c, t0, s_.loop);
    s_.yaw = wrapAngle(base + k * rootYaw(*c, s_.clipTime, s_.loop));
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

void Player::hitReact(std::string_view clip, bool mirror) {
    s_.state = State::Hit;
    s_.vel = vec3(0);
    s_.clip.clear(); // restart even when the same reaction is playing
    play(clip, false, 1, mirror);
}

bool Player::canInteract() const {
    using enum State;
    const State s = s_.state;
    return s_.water < 0 && (s == Idle || s == Walk || s == Run || s == Stop || s == Turn || s == Land || s == Crouch);
}

bool Player::use(std::string_view clip, vec3 pos, float yaw) {
    if (!canInteract() || !anims_.find(clip)) return false;
    s_.state = State::Use;
    s_.vel = vec3(0), s_.pos = pos, s_.yaw = yaw;
    s_.clip.clear(), s_.clipMix.clear(), s_.mix = 0;
    play(clip, false);
    return true;
}

// [O] lara.drm requests 60 obj_block_grab, 61 obj_block_idle, 62 / 58 obj_block_front/left/right/back, 1452
// obj_block_release; the clips have no root motion. [I] front = pushing, back = pulling; the sideways clips are unused.
void Player::grip(vec3 axis, int moving) {
    if (s_.state != State::Block) {
        if (axis == vec3(0) || !canInteract() || !anims_.find("obj_block_grab")) return;
        s_.state = State::Block;
        s_.vel = vec3(0), s_.yaw = std::atan2(axis.y, axis.x);
        s_.clip.clear(), s_.clipMix.clear(), s_.mix = 0;
        play("obj_block_grab", false);
        return;
    }
    if (axis == vec3(0)) { // let go
        s_.state = State::Use;
        s_.clip.clear();
        play("obj_block_release", false);
        return;
    }
    s_.yaw = std::atan2(axis.y, axis.x);
    if (s_.clip == "obj_block_grab" && !clipDone()) return;
    play(moving > 0 ? "obj_block_front" : moving < 0 ? "obj_block_back" : "obj_block_idle", true);
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
    if (wading() && anims_.find("swim_land_wading")) { // into class-0x100 water
        s_.state = State::Land;
        play("swim_land_wading", false);
    } else if (speed > 600.0f && glm::length(move) > 0.5f) {
        s_.state = State::Run;
        play("ng_running_land", false);
    } else {
        s_.state = State::Land;
        play("ng_generic_land_forwards", false);
    }
}

// tru.exe 0x5D3860 classes the water under her by floor depth (surface - feet on the floor): class 0x100 = wading.
bool Player::wading() const {
    const int w = waterAt(s_.pos);
    if (w < 0 || !anims_.find("wading_idle") || !anims_.find("wading_walk")) return false;
    const float d = waterSurface(col_.markup[size_t(w)], vec2(s_.pos), waveSeconds_) - s_.pos.z;
    return d >= t_.wadeDepth && d < t_.swimDepth;
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
    // Falling through a pole's top (its axis + kPoleTopRise) within kPoleTopLand of it: back on top, square to it [I]
    // (tru.exe lands pole-top jumps through its air states' jump-target conditions, not ported).
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (!isPole(m)) continue;
        const OnLine o = closestOn(m, p);
        const float top = o.q.z + kPoleTopRise;
        if (o.seg < 0 || o.dist > kPoleTopLand || prevPos.z < top - 1 || p.z > top + 1) continue;
        const vec2 dir = segDir(m, o.seg), n(-dir.y, dir.x);
        const float side = glm::dot(xy(forward()), n) >= 0 ? 1.0f : -1.0f;
        s_.pos = vec3(xy(o.q), top), s_.yaw = yawOf(n * side), s_.vel = vec3(0);
        s_.ledge = int(li), s_.ledgeSeg = o.seg, s_.ledgeT = o.t, s_.ledgeSide = side, s_.polePara = false;
        s_.fallHeight = s_.fallStartZ - s_.pos.z;
        s_.state = State::Perch;
        play("hpole_perch_ready", true);
        return;
    }
    if (auto g = col_.groundProbe(p + vec3(0, 0, t_.radius), t_.radius + kSkin + 2); g && standable(p, *g)) {
        s_.pos.z = g->point.z;
        land(move);
        return;
    }
    if (tryGrab(prevPos)) return;
    tryGrabPole(prevPos);
    if (tryLadder(true)) return;
    if (tryPerch(true)) return;
    if (!tryClimb(true)) tryRail(true); // ledges, poles, ladders, perches and rock faces win over a rail
}

// Ledge grab: markup type 1 ("ledgegrab") polylines. The hands (hang pose) must pass the ledge height while falling,
// with Lara facing the ledge, from grabReach in front of its line to anywhere over it while her root is still on her
// side: pressed square against the wall under a ledge at its face, the capsule (radius 79) holds the root 79 short of
// the line and the hands, 141.7 ahead, reach 63 over it (a running jump into l2_cliffclimb's ledge 911 missed it).
bool Player::tryGrab(vec3 prevPos) {
    // Faster than grabMinVz (up to twice) only a saving grab holds [I]; the vertical window grows to one tick's fall.
    if (s_.regrabCooldown > 0 || s_.vel.z < 2 * t_.grabMinVz || s_.vel.z > t_.grabMaxVz) return false;
    const bool saving = s_.vel.z < t_.grabMinVz;
    const vec3 hand = s_.pos + toWorld(anims_.hangHandOffset), prevHand = prevPos + toWorld(anims_.hangHandOffset);
    const float window = std::max(t_.grabWindow, prevHand.z - hand.z);
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
            if (hand.z > q.z + 10.0f || prevHand.z < q.z - window || hand.z < q.z - window) continue;
            // Lara's side of the ledge: the wall is the other side; she must be facing it.
            const vec2 dir = seg / std::sqrt(len2);
            vec2 out(-dir.y, dir.x);
            float side = 1;
            if (glm::dot(xy(s_.pos - q), out) < 0) side = -1, out = -out;
            const float over = glm::dot(xy(hand - q), -out); // hands past the line (> 0) or short of it
            if (over < -t_.grabReach || std::abs(glm::dot(xy(hand - q), dir)) > t_.grabReach) continue;
            if (glm::dot(xy(forward()), -out) < 0.5f) continue;
            s_.ledge = int(li);
            s_.ledgeSeg = int(si);
            s_.ledgeT = u * std::sqrt(len2);
            s_.ledgeSide = side;
            s_.vel = vec3(0);
            s_.state = State::Hang;
            s_.savingGrab = saving ? t_.savingGrabSeconds : 0.0f;
            play(saving ? "ledgeb_bad_attach_M" : "ledgeb_attach_M", false);
            placeOnLedge();
            return true;
        }
    }
    return false;
}

// Horizontal poles (markup 4 "pole", 6 "horizpole"): the hands (pole hang pose, Lara turned square to the
// pole) within poleReach of the pole axis, rising or falling inside the zVel window, from grabWindow below the
// axis to poleGrabAbove above it.
bool Player::tryGrabPole(vec3 prevPos) {
    if (s_.regrabCooldown > 0 || s_.vel.z < t_.grabMinVz || s_.vel.z > t_.grabMaxVz) return false;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (!isPole(m)) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 dir = segDir(m, si), n(-dir.y, dir.x);
            const float c = glm::dot(xy(forward()), n);
            // Facing across the pole: the perpendicular hang; along it (within 60 degrees): the parallel one
            // (Hpole_para_attach_* of request 1337 [O]; the 30 / 60 degree split is [I]).
            const bool para = std::abs(c) < 0.5f;
            const float side = para ? (glm::dot(xy(forward()), dir) > 0 ? 1.0f : -1.0f) : c > 0 ? 1.0f : -1.0f;
            const float yaw = yawOf(para ? dir * side : n * side);
            const vec3 off = para ? anims_.poleParaHandOffset : anims_.poleHandOffset;
            const vec3 hand = s_.pos + rotYaw(yaw, off), prevHand = prevPos + rotYaw(yaw, off);
            const float window = std::max(t_.grabWindow, prevHand.z - hand.z);
            const float t = std::clamp(glm::dot(xy(hand - m.points[size_t(si)]), dir), 0.0f, segLen(m, si));
            const vec3 q = pointOn(m, si, t);
            if (glm::length(xy(hand - q)) > t_.poleReach) continue;
            if (hand.z > q.z + t_.poleGrabAbove || hand.z < q.z - window) continue;
            s_.ledge = int(li), s_.ledgeSeg = si, s_.ledgeT = t, s_.ledgeSide = side, s_.polePara = para;
            s_.vel = vec3(0);
            s_.state = State::PoleHang;
            play(para ? "Hpole_para_attach_S" : "Hpole_perp_attach_M", false);
            placeOnLedge();
            return true;
        }
    }
    return false;
}

// The leap from a jump onto a pole in reach: tru.exe under Wine (l4_traprooms 1336, a standing and a running jump)
// flies her root in a straight line, no gravity, onto the hang in 11 frames at 60 Hz (0.183 s; 1718 and 1135 units/s
// for 315 and 208 out) [O]. The reach (kPoleLeapReach out, 0..kPoleLeapRise up to the hang root) is [I]: both leaps
// started within it.
constexpr float kPoleLeapTime = 11.0f / 60.0f, kPoleLeapReach = 330.0f, kPoleLeapRise = 250.0f;
bool Player::tryPoleLeap() {
    if (s_.regrabCooldown > 0) return false;
    const vec2 f = xy(forward());
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (!isPole(m)) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 dir = segDir(m, si), n(-dir.y, dir.x);
            const float c = glm::dot(f, n);
            const bool para = std::abs(c) < 0.5f; // as tryGrabPole
            const float side = para ? (glm::dot(f, dir) > 0 ? 1.0f : -1.0f) : c > 0 ? 1.0f : -1.0f;
            const float t = std::clamp(glm::dot(xy(s_.pos - m.points[size_t(si)]), dir), 0.0f, segLen(m, si));
            const vec3 d = attachPose(int(li), si, t, side, para).first - s_.pos;
            if (glm::length(xy(d)) > kPoleLeapReach || d.z < 0 || d.z > kPoleLeapRise) continue;
            if (glm::length(xy(d)) > 1 && glm::dot(glm::normalize(xy(d)), f) < 0.5f) continue; // ahead of her
            s_.ledge = int(li), s_.ledgeSeg = si, s_.ledgeT = t, s_.ledgeSide = side, s_.polePara = para;
            s_.vel = d / kPoleLeapTime;
            s_.flight = kPoleLeapTime;
            s_.state = State::HangJump;
            return true;
        }
    }
    return false;
}

// Root and heading for hanging at (markup, seg, t, side). Ledges: facing the wall, hands (hang pose) on the
// ledge; poles: facing along the polyline normal * side (perpendicular hang) or along the polyline * side
// (parallel hang, `para`), hands (that pole pose) on the axis.
std::pair<vec3, float> Player::attachPose(int markup, int seg, float t, float side, bool para) const {
    const auto& m = col_.markup[size_t(markup)];
    const vec2 dir = segDir(m, seg), out = vec2(-dir.y, dir.x) * side;
    const bool pole = isPole(m);
    para = pole && para;
    const float yaw = yawOf(para ? dir * side : pole ? out : -out);
    const vec3 off = para ? anims_.poleParaHandOffset : pole ? anims_.poleHandOffset : anims_.hangHandOffset;
    return {pointOn(m, seg, t) - rotYaw(yaw, off), yaw};
}

void Player::placeOnLedge() {
    std::tie(s_.pos, s_.yaw) = attachPose(s_.ledge, s_.ledgeSeg, s_.ledgeT, s_.ledgeSide, s_.polePara);
}

// Ledge corners. lara.drm has a corner set for the hang (requests 1030..1046 [O]): ledgeb_concave_right_phase1 (0.27 s,
// a 45 degree turn to the right, ending with the hands 38 in front of the root: ledgeb_concave_idle's pose) and _phase2
// (another 43 degrees, ending in ledgeb_idle's pose), ledgeb_convex_right_phase1 (0.6 s, 206 to the right and 45 degrees
// to the left, hands 212 out: ledgeb_convex_idle) and _phase2 (37 degrees more), the corner idles and their leans
// ledgeb_<concave|convex>_jump_up_antic / _jump_back_antic (loops, requests 1036..1039) [O: clip poses]. Ledge polylines
// turn by ~90 degrees at their corners (openunderworld-work/ladderleans/survey.txt: 740 joints over 45 degrees in the
// units, 573 of them 85..95) [O-data]. Ours [I: the hang group's requests are not decoded, so which state plays which]:
// shimmying onto a joint turning more than 45 degrees plays phase1 into the corner pose (her hands on the joint, facing
// between the two walls), concave where the next segment turns toward her side, convex where it turns away; there the
// corner idle plays, the stick up / back leans, the stick on along the ledge plays phase2 onto the next segment (back the
// way she came: phase2 mirrored onto the first); left = the right clips mirrored. Jumping at a corner is the hang's.
namespace {
constexpr float kCornerTurn = 0.7853982f; // 45 degrees
float jointTurn(const collision::Markup& m, int seg) { // the polyline's turn from seg to seg + 1
    const vec2 a = segDir(m, seg), b = segDir(m, seg + 1);
    return std::atan2(a.x * b.y - a.y * b.x, glm::dot(a, b));
}
} // namespace

bool Player::startHangCorner(int seg, float along, float lateral) {
    const auto& m = col_.markup[size_t(s_.ledge)];
    const int joint = along > 0 ? seg : seg - 1, to = along > 0 ? seg + 1 : seg - 1;
    if (joint < 0 || size_t(joint) + 2 >= m.points.size() || std::abs(jointTurn(m, joint)) <= kCornerTurn) return false;
    const vec2 dir = segDir(m, seg), out = vec2(-dir.y, dir.x) * s_.ledgeSide;
    const bool concave = glm::dot(segDir(m, to) * (along > 0 ? 1.0f : -1.0f), out) > 0; // the next wall comes round to her
    const bool mirror = lateral < 0;
    s_.ledgeSeg = seg;
    s_.ledgeT = along > 0 ? segLen(m, seg) : 0.0f;
    const float yawA = attachPose(s_.ledge, seg, s_.ledgeT, s_.ledgeSide).second;
    const float yawB = attachPose(s_.ledge, to, along > 0 ? 0.0f : segLen(m, to), s_.ledgeSide).second;
    const float mid = yawA + wrapAngle(yawB - yawA) * 0.5f;
    const anim::Clip* idle = anims_.find(concave ? "ledgeb_concave_idle" : "ledgeb_convex_idle");
    vec3 hands = idle ? anims_.handsAt(*idle, 0).value_or(anims_.hangHandOffset) : anims_.hangHandOffset;
    if (mirror) hands.x = -hands.x;
    s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw;
    s_.climbTo = pointOn(m, seg, s_.ledgeT) - rotYaw(mid, hands);
    s_.climbYawTo = s_.yaw + wrapAngle(mid - s_.yaw);
    s_.state = State::Hang;
    play(concave ? "ledgeb_concave_right_phase1" : "ledgeb_convex_right_phase1", false, 1, mirror);
    return true;
}

bool Player::hangCorner(float lateral, float fw, bool jumpPressed, float dt) {
    const std::string c = s_.clip;
    if (!c.starts_with("ledgeb_concave") && !c.starts_with("ledgeb_convex")) return false;
    const std::string kind = c.starts_with("ledgeb_concave") ? "concave" : "convex";
    if (c.ends_with("_phase1")) {
        if (warpStep(dt)) play("ledgeb_" + kind + "_idle", true, 1, s_.mirror);
        return true;
    }
    if (c.ends_with("_phase2")) {
        if (warpStep(dt)) {
            play("ledgeb_idle", true);
            placeOnLedge();
        }
        return true;
    }
    if (jumpPressed) return false;
    const auto& m = col_.markup[size_t(s_.ledge)];
    const int seg = s_.ledgeSeg;
    const bool atEnd = s_.ledgeT > 0.5f * segLen(m, seg);
    if (std::abs(lateral) > 0.3f) { // round onto the next segment, or back onto this one
        const bool onward = lateral * (s_.mirror ? -1.0f : 1.0f) > 0;
        const int dst = onward ? (atEnd ? seg + 1 : seg - 1) : seg;
        const float len = segLen(m, dst);
        const bool fromStart = onward ? atEnd : !atEnd; // the joint is dst's start
        const bool mirror = lateral < 0;
        const std::string phase2 = "ledgeb_" + kind + "_right_phase2";
        const anim::Clip* p2 = anims_.find(phase2);
        vec3 mv = p2 ? rootTranslation(*p2, p2->durationSeconds(), false) - rootTranslation(*p2, 0, false) : vec3(0);
        if (mirror) mv.x = -mv.x;
        const float yawDst = attachPose(s_.ledge, dst, fromStart ? 0.0f : len, s_.ledgeSide).second;
        const vec3 hands = s_.pos + rotYaw(s_.yaw, mv) + rotYaw(yawDst, anims_.hangHandOffset); // where the clip ends her
        const float d = std::abs(glm::dot(xy(hands - pointOn(m, dst, fromStart ? 0.0f : len)), segDir(m, dst)));
        const float t = std::clamp(fromStart ? d : len - d, std::min(1.0f, len), std::max(len - 1.0f, 0.0f));
        s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw;
        std::tie(s_.climbTo, s_.climbYawTo) = attachPose(s_.ledge, dst, t, s_.ledgeSide);
        s_.climbYawTo = s_.yaw + wrapAngle(s_.climbYawTo - s_.yaw);
        s_.ledgeSeg = dst, s_.ledgeT = t;
        play(phase2, false, 1, mirror);
        return true;
    }
    play("ledgeb_" + kind + (fw > 0.5f ? "_jump_up_antic" : fw < -0.5f ? "_jump_back_antic" : "_idle"), true, 1, s_.mirror);
    advanceClip(dt);
    return true;
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
        s_.balance = {};
        play(clip, false);
        return true;
    }
    return false;
}

// Standing within 250 [I] of a ledge (markup 1; a hurdle_onto leaves her 208 past it) at floor height whose far side
// (away from Lara) drops more than 200 within 240 units of the line (ledge lines can sit well inside the collision
// top: l4_pool 1253 by up to ~180):
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
        if (o.seg < 0 || o.dist > 250.0f || o.dist >= bestOn.dist || std::abs(o.q.z - s_.pos.z) > 40.0f) continue;
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

// The hang's clearance class, tru.exe 0x74E290 (run on the attached ledgegrab by 0x76D010 in the segment frame of
// 0x737D60; result in player data +0xEDC, read by the chart's PSCCF_Markup_ClearanceResult) with lara.drm player data
// [O: decomp]: a ray 252 (+0x6F0) above the ledge from 100 (+0x6E8) on her side to 139 (+0x6EA) over the top hitting
// something = the shelf test 0x74E160 (0x74BB50 == 1: 8, the wall-hug shelf; == 2: 11, wall too close), else 1; no
// hit: the first surface down from 252 over the point 139 over the top 30 below to 40 above the ledge (0x72BAE0), then
// the room above 69.5 over the top from 252 up to +0x10 = 512.01: all of it = 2 (ledgeb_climbup_fast), from +0x14 =
// 260 = 5 (onto the top crouching), less = 1; no top = 1 (a beam narrower than 139, a top that drops away).
// The chart (S530 T558 / T573 / T563, S439): 2 climbs up, 11 jumps up, anything else goes on to S439 (5 crouching,
// 8 the wall hug, a balance beam as the jump target bbeam_ledgeu_climbup). Classes 0 / 12 (0x7375C0's check of what
// is under her) are not modelled [I].
int Player::hangClearance() const {
    constexpr float kRayZ = 252.0f, kBack = 100.0f, kOver = 139.0f, kStand = 512.0101f, kCrouch = 260.0f;
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 q = pointOn(m, s_.ledgeSeg, s_.ledgeT);
    const vec3 top = forward();
    if (col_.raycast(q - top * kBack + vec3(0, 0, kRayZ), top, kBack + kOver)) {
        const vec2 d = segDir(m, s_.ledgeSeg);
        if (glm::dot(vec2(-d.y, d.x), xy(top)) > 0.7f && shelfAt(s_.ledge, s_.ledgeSeg, s_.ledgeT)) return 8;
        return 1; // [I] 0x74BB50's 2 (wall within 88.5) as 1: no up jump without a target
    }
    const auto floor = col_.raycast(q + top * kOver + vec3(0, 0, kRayZ), vec3(0, 0, -1), kRayZ + 30.0f);
    const float dz = floor ? floor->point.z - q.z : -30.0f;
    if (!(dz > -30.0f && dz < 40.0f)) return 1;
    const vec3 above = q + top * (kOver * 0.5f) + vec3(0, 0, kRayZ);
    const auto roof = col_.raycast(above, vec3(0, 0, 1), kStand - kRayZ);
    const float room = roof ? kRayZ + roof->t : kStand;
    return room >= kStand ? 2 : room < kCrouch ? 1 : 5;
}

// Hanging from a ledge with a balance beam (markup 14) as the jump target: the chart's S439 T698 (jump target type
// 625 = beam) -> S882 -> S885 bbeam_ledgeu_climbup (request 184) -> the beam idle [O-data]. Lara's root does not move
// in the clip: it stands on the beam line from the start, the hands 105 behind it and 28.6 to her side, where the
// ledge is (the clip's last pose is bbeam_idle's) [O: clip]. Which beam the hang-up query picks: the one along the
// ledge, its line within 100 over the top and 60 of its height [I] (l3_chapel: ledges 63 apart round 63-wide beams).
bool Player::tryHangToBeam() {
    const auto& lm = col_.markup[size_t(s_.ledge)];
    const vec3 q = pointOn(lm, s_.ledgeSeg, s_.ledgeT);
    const anim::Clip* c = anims_.find("bbeam_ledgeu_climbup");
    if (!c) return false;
    const auto hands = anims_.handsAt(*c, clipBlend("", "bbeam_ledgeu_climbup").startSeconds);
    if (!hands) return false;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 14) continue;
        const OnLine o = closestOn(m, q);
        if (o.seg < 0 || o.dist > 100.0f || std::abs(o.q.z - q.z) > 60.0f || glm::dot(xy(o.q - q), xy(forward())) < 0) continue;
        const vec2 dir = segDir(m, o.seg);
        for (const float side : {1.0f, -1.0f}) {
            const float yaw = yawOf(dir * side);
            const vec3 h = rotYaw(yaw, *hands);
            if (glm::dot(xy(h), xy(q - o.q)) <= 0) continue; // her hands on the ledge's side of the beam
            // root on the line where the hands reach the ledge point
            int seg = o.seg;
            float t = o.t;
            moveAlong(m, seg, t, -glm::dot(xy(h), dir));
            s_.ledge = int(li), s_.ledgeSeg = seg, s_.ledgeT = t, s_.ledgeSide = side;
            s_.pos = pointOn(m, seg, t);
            s_.yaw = yaw;
            s_.vel = vec3(0);
            s_.state = State::Beam;
            s_.balance = {};
            play("bbeam_ledgeu_climbup", false);
            return true;
        }
    }
    return false;
}

// ---- obstacles: ledges (markup 1) met from the ground (docs/GAMEPLAY.md "Obstacles") ----
// lara.drm's obstacle anim requests are blend spaces (object DTP +0x60, 0x24-byte records; tru.exe 0x4428A0 syncs
// their clips by phase) whose clips are named by height and depth in cm, 1 cm = 3.16050 units: the exe's own
// 158.02501 / 316.05002 / 474.075 / 632.1 (0x707C60, 0x72CCD0, 0x7396E0, our probe of the clips' root motion).
//   127 hurdle_ground_attach_low_50 / _low_100, 128 _mid_100 / _mid_150, 129 _high_150 / _high_200: no root motion; the
//       mid / high ones end with the hands 103 ahead on an edge that high (323 / 477 / 480 / 642), the low ones hop
//   132 hurdle_low_onto / _100 (rise 162.7 / 316.1), 133 hurdle_onto (317.3): onto the top
//   130 hurdle_low_over / _over_low_long_50 / _low_over_100 / _over_low_long_100 (640 / 860 / 635 / 907 forward),
//       131 hurdle_over / _over_long_100 (594 / 722): over a thin obstacle back to the same floor
//   134 hurdle_to_ledgeb_low_25 / _65 / _100, 135 _mid_*: over the top (79 / 205 / 316 deep: 332 / 459 / 571
//       forward) and down its far side into a hang, turning half round
//   139 hurdle_down_low_50 / _100, 141 _mid_100 / _mid_150, 140 _mid_150 / _high_200: down 158..632 off a ledge
//   147 / 149 / 148 hurdle_ground_attach_*_cancel: back off the attach (low / mid / high), 107-110 back
// Selection: lara.drm's player state chart (decoded, docs/GAMEPLAY.md "Obstacles"; tool openunderworld-work/hurdles/sc.py):
// - the ground locomotion states (chart S724 / S730) run markup query 96 (state property 0x21D); its winning ledge is
//   classed by the ledge markup table (objdata +0x121C0, evaluated by 0x728650): the floor 94.8 out from the ledge on
//   her side 154.87..312.90 / 312.90..474.08 / 474.08..635.27 below it = class 5 / 6 / 7 (up), the same drop beyond a
//   ledge she stands on = 8 / 9 / 10 (down), each also needing the clearance test 0x727BF0. On the ground, class 5 goes
//   straight into S958 (no attach clip: low onto / over / far-side hang), 6 only bumps (wall_collide_mid, not played),
//   8 / 9 / 10 play the down clips (S996 / S995 / S994); from the jump states (S289, running jump S775) class 6 / 7 play
//   the mid / high attach (S959 / S957; 5 again S958): mid / high need the jump.
// - the hurdle parent S954 runs action 0x201 = 0x73DFF0 on entry, which classes what is past the ledge (player +0x6C,
//   condition 0x1D1): see hurdleClass. 0 over (S971 / S981), 1 onto (S974 / S980), 2 onto the far side into a hang
//   (S966 / S983, the jump target a ledge); unclassed (-1) falls to onto. (3 / 4 / 5 = the same with flair clips when
//   0x72F360 finds room around her; not played.)
// - anim target 0x273 (0x73DD70, state property 0x19A): the root is drawn to the ledge point + 86.94 out (property
//   0x119 of S954) at her own height (0x11A = 0); down (S997): on the line, heading the normal + 180 (0x11C). We measure
//   from the face under the line (l4_pool 1253's line sits up to ~180 back: on the line she sank into the rock).
// - cancel: stick back (conditions 0x192 MoveDir 411) during the attach (S959 -> S961, request 149 / 148 / 147).
namespace {
constexpr float kCm50 = 158.02501f, kCm100 = 316.05002f, kCm150 = 474.075f, kCm200 = 632.1f;
// ledge markup class table (objdata +0x121C0): class 5..7 / 8..10 bands of the floor 94.815 out below the ledge
constexpr float kBand0 = 154.86726f, kBand1 = 312.89508f, kBand2 = 474.08347f, kBand3 = 635.27185f;
constexpr float kFloorOut = 94.815f, kAttachOut = 86.94f; // 0x728650; S954 property 0x119
float span(float v, float a, float b) { return std::clamp((v - a) / (b - a), 0.0f, 1.0f); }

// What is past the ledge point q for a hurdle along f (tru.exe 0x73DFF0, tuning objdata +0x530): rays along f from 6.32
// behind q to 474.08 past it at 237.04 and then 31.61 above q: a hit within 237.04 = -1, beyond = 1 (onto); else a
// down ray 347 + 6.32 past q from 37.9 above to 53.7 below it: a top there = 1; none = 2 when a ledge along the top's far
// end can be hung from (query 100 in the original; ours the nearest ledge within 200 of where the top ends) unless a
// floor lies within 632 below 158 past it (then 0), else 0 (over).
struct Past { int cls = 1; int li = -1, seg = 0; float t = 0, side = 1, depth = 0; };
Past hurdleClass(const collision::CollisionMesh& col, vec3 q, vec2 f) {
    Past p;
    for (const float z : {237.04f, 31.606f})
        if (const auto hit = col.raycast(vec3(xy(q) - f * 6.321f, q.z + z), vec3(f, 0), kBand2 + 6.321f)) {
            p.cls = hit->t < 237.04f ? -1 : 1;
            return p;
        }
    if (col.raycast(vec3(xy(q) + f * 353.321f, q.z + 37.926f), vec3(0, 0, -1), 91.642f)) return p; // onto
    float k = 20;
    for (; k < 353.0f; k += 20)
        if (!col.raycast(vec3(xy(q) + f * k, q.z + 37.926f), vec3(0, 0, -1), 91.642f)) break;
    p.depth = k - 20;
    const vec3 far = q + vec3(f * p.depth, 0);
    float best = 200.0f;
    p.cls = 0;
    for (size_t fi = 0; fi < col.markup.size(); ++fi) {
        const auto& fm = col.markup[fi];
        if (fm.type != 1) continue;
        const OnLine fo = closestOn(fm, far);
        if (fo.seg < 0 || fo.dist >= best || std::abs(fo.q.z - q.z) > 40.0f || glm::dot(xy(fo.q - q), f) < 20.0f) continue;
        const vec2 n(-segDir(fm, fo.seg).y, segDir(fm, fo.seg).x);
        if (std::abs(glm::dot(n, f)) < 0.7f) continue;
        const float side = glm::dot(n, f) > 0 ? 1.0f : -1.0f;
        best = fo.dist;
        const bool floor = bool(col.raycast(fo.q + vec3(n * side * 158.0f, 6.321f), vec3(0, 0, -1), kCm200 + 12.642f));
        p.cls = floor ? 0 : 2, p.li = int(fi), p.seg = fo.seg, p.t = fo.t, p.side = side;
    }
    return p;
}
// Ledge class test 0 (0x727BF0; tuning +0x6E8 100, +0x6EA 139, +0x6F0 252, +0x10 height 512), its segments in the
// ledge's frame (out = away from the top, up): over the top 252 above the ledge from 100 out to 139 onto it (both ways)
// and a standing column 69.5 onto the top from 31.6 to 543.6 above it must be clear. Its first segment (100 out,
// 252 below to 252 above) would cross the floor under every low ledge as we read the frame: not applied [U].
bool ledgeClear(const collision::CollisionMesh& col, vec3 q, vec2 out) {
    const auto p = [&](float y, float z) { return q + vec3(out * y, z); };
    const auto clear = [&](vec3 a, vec3 b) { const vec3 d = b - a; return !col.raycast(a, glm::normalize(d), glm::length(d)); };
    return clear(p(100, 252), p(-139, 252)) && clear(p(-139, 252), p(100, 252)) && clear(p(-69.5f, 31.605f), p(-69.5f, 512.0f + 31.605f));
}
// Face under the ledge point q seen from `from` along -out at 20 below the top (q when nothing is hit or it is behind).
vec3 faceEdge(const collision::CollisionMesh& col, vec3 from, vec3 q, vec2 out) {
    const float toLine = glm::dot(xy(from - q), out);
    if (const auto hit = col.raycast(vec3(xy(from), q.z - 20), vec3(-out, 0), std::max(toLine, 0.0f) + 10); hit && hit->t < toLine)
        return vec3(xy(from) - out * hit->t, q.z);
    return q;
}
} // namespace

bool Player::tryHurdle(bool jumped) {
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        const OnLine o = closestOn(m, s_.pos);
        if (o.seg < 0 || o.u < 0 || o.u > segLen(m, o.seg) || o.dist > t_.radius + 120.0f + 200.0f) continue;
        vec2 out = vec2(-segDir(m, o.seg).y, segDir(m, o.seg).x);
        if (glm::dot(xy(s_.pos - o.q), out) < 0) out = -out; // Lara's side
        if (glm::dot(xy(forward()), -out) < 0.6f) continue;
        const vec3 edge = faceEdge(col_, s_.pos, o.q, out);
        const float reach = glm::dot(xy(s_.pos - edge), out);
        if (reach < -1.0f || reach > t_.radius + (jumped ? 120.0f : 40.0f)) continue; // [U] query 96's reach
        const auto below = col_.groundProbe(vec3(xy(edge) + out * kFloorOut, o.q.z + 60), kBand3 + 200.0f);
        const float h = o.q.z - (below ? below->point.z : s_.pos.z);
        const int band = h <= kBand0 ? -1 : h <= kBand1 ? 0 : h <= kBand2 ? 1 : h <= kBand3 ? 2 : -1;
        if (band < 0 || (band > 0 && !jumped) || !anims_.find("hurdle_low_onto") || !ledgeClear(col_, o.q, out)) continue; // 6 / 7 only from the jump states
        const Past past = hurdleClass(col_, o.q, xy(forward()));
        PlayerState::Hurdle hu;
        hu.edge = edge, hu.out = out, hu.h = h, hu.endZ = o.q.z;
        if (const auto top = col_.groundProbe(vec3(xy(edge) - out * 40.0f, o.q.z + 60), 100); top && std::abs(top->point.z - o.q.z) <= 40)
            hu.endZ = top->point.z;
        // blend weights: requests' axes (state properties 0x0E6 / 0x0E7: 158.1..316.2 / 316.2..474.1 / 474.1..632.2)
        const float hw = band == 0 ? span(h, kCm50, kCm100) : band == 1 ? span(h, kCm100, kCm150) : span(h, kCm150, kCm200);
        if (past.cls == 2) {
            hu.next = PlayerState::Hurdle::ToLedge, hu.farLedge = past.li, hu.farSeg = past.seg, hu.farT = past.t, hu.farSide = past.side;
            hu.alt = past.depth > 205.4f; // depth axis 79.06 / 205.4 / 316.2 (S977 properties 0x0E5..0x0E7)
            hu.mix = hu.alt ? span(past.depth, 205.4f, kCm100) : span(past.depth, 79.06f, 205.4f);
        } else if (past.cls == 0) {
            hu.next = PlayerState::Hurdle::Over, hu.endZ = s_.pos.z;
            // depth axis 316.2..632.2 (S971 properties 0x0E6 / 0x0E7; its height axis 0x0E9 / 0x0EA 158.1..316.2): [U]
            // which distance; ours from her root to the top's far end
            const float dw = span(reach + past.depth + kAttachOut, kCm100, kCm200);
            hu.alt = band == 0 && dw >= 0.5f;
            hu.mix = band == 0 ? hw : dw;
        } else {
            hu.next = PlayerState::Hurdle::Onto;
        }
        // Low onto: where the request's root motion ends must be on the top; tru.exe under Wine (the prologue slab, 34
        // degrees off square) started it moved ~50 along the ledge, away from the slab's corner: the nearest start up to
        // 100 along it whose landing is on the top (0, 25, -25, ... [I] the step).
        float along = 0;
        if (band == 0 && hu.next == PlayerState::Hurdle::Onto) {
            const anim::Clip *ca = anims_.find("hurdle_low_onto"), *cb = anims_.find("hurdle_low_onto_100");
            if (!ca || !cb) continue;
            const double t0 = clipBlend("", "hurdle_low_onto").startSeconds;
            const vec3 tr = glm::mix(rootTranslation(*ca, ca->durationSeconds(), false) - rootTranslation(*ca, t0, false),
                                     rootTranslation(*cb, cb->durationSeconds(), false) - rootTranslation(*cb, t0 / ca->durationSeconds() * cb->durationSeconds(), false), hw);
            const vec2 land = xy(s_.pos) + out * (kAttachOut - reach) + xy(rotYaw(s_.yaw, tr));
            const auto supported = [&](float a) {
                if (o.u + a < 0 || o.u + a > segLen(m, o.seg)) return false;
                const auto g = col_.groundProbe(vec3(land + segDir(m, o.seg) * a, o.q.z + 100), 140);
                return g && g->normal.z >= t_.minFloorNormalZ && std::abs(g->point.z - o.q.z) <= 40;
            };
            while (!supported(along) && along > -100.0f) along = along > 0 ? -along : 25.0f - along;
            if (!supported(along)) continue;
            if (const auto g = col_.groundProbe(vec3(land + segDir(m, o.seg) * along, o.q.z + 100), 140)) hu.endZ = g->point.z;
        }
        s_.pos += vec3(segDir(m, o.seg) * along, 0);
        hu.edge += vec3(segDir(m, o.seg) * along, 0);
        s_.ledge = int(li), s_.ledgeSeg = o.seg, s_.ledgeT = std::clamp(o.t + along, 0.0f, segLen(m, o.seg)), s_.ledgeSide = 1;
        s_.vel = vec3(0);
        s_.state = State::ClimbUp;
        s_.hurdle = hu;
        if (band == 0) { // S958: no attach clip
            s_.hurdle.draw = true, s_.hurdle.drawOff = out * (kAttachOut - reach);
            s_.mix = hw;
            startHurdleFollowUp(true);
        } else if (band == 1) {
            playMix("hurdle_ground_attach_mid_100", "hurdle_ground_attach_mid_150", hw);
        } else {
            playMix("hurdle_ground_attach_high_150", "hurdle_ground_attach_high_200", hw);
        }
        return true;
    }
    return false;
}

// Down off an obstacle (ledge class 8 / 9 / 10: chart S724 -> S996 / S995 / S994, no other condition): walking or
// running at the edge of a ledge's top she stands on (the class test: ledge less than 6.33 above her feet), facing it
// within 53 degrees, the floor 94.8 out past the ledge 154.87..312.90 / ..474.08 / ..635.27 below it: request 139 / 141 /
// 140, weighted 158.1..316.2 / 316.2..474.1 / 474.1..632.2 (S996 properties 0x0E6 / 0x0E7), heading the ledge normal +
// 180 from the line (S997 property 0x11C, 0x119 = 0); the drop scaled to the floor. Trigger distance: the top ending
// within radius + 40 ahead [U: query 96's reach].
bool Player::tryHurdleDown() {
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        const OnLine o = closestOn(m, s_.pos);
        if (o.seg < 0 || o.u < 0 || o.u > segLen(m, o.seg) || o.dist > 400.0f || std::abs(o.q.z - s_.pos.z) > 40.0f) continue;
        vec2 out = vec2(-segDir(m, o.seg).y, segDir(m, o.seg).x);
        if (glm::dot(xy(forward()), out) < 0) out = -out;
        if (glm::dot(xy(forward()), out) < 0.6f) continue;
        // the ledge ahead of her (she stands on its top side; a ledge she has just climbed over is behind her)
        if (glm::dot(xy(o.q - s_.pos), out) < -20.0f) continue;
        float k = 0; // where the top ends ahead
        for (; k <= t_.radius + 40.0f; k += 10.0f) {
            const auto g = col_.groundProbe(vec3(xy(s_.pos) + out * k, s_.pos.z + 60), 100);
            if (!g || g->normal.z < t_.minFloorNormalZ || std::abs(g->point.z - s_.pos.z) > 40.0f) break;
        }
        if (k > t_.radius + 40.0f || std::abs(glm::dot(xy(s_.pos) + out * k - xy(o.q), out)) > 200.0f) continue; // no edge near / not at this ledge
        if (o.q.z - s_.pos.z >= 6.3281126f || !ledgeClear(col_, o.q, out)) continue; // 0x7260B0 kind 2, test 0
        const auto below = col_.groundProbe(vec3(xy(s_.pos) + out * (k + kFloorOut), s_.pos.z + 60), kBand3 + 120.0f);
        if (!below || below->normal.z < t_.minFloorNormalZ) continue;
        const float d = o.q.z - below->point.z;
        if (d <= kBand0 || d > kBand3) continue;
        const std::string_view a = d <= kBand1 ? "hurdle_down_low_50" : d <= kBand2 ? "hurdle_down_mid_100" : "hurdle_down_mid_150";
        const std::string_view b = d <= kBand1 ? "hurdle_down_low_100" : d <= kBand2 ? "hurdle_down_mid_150" : "hurdle_down_high_200";
        const float w = d <= kBand1 ? span(d, kCm50, kCm100) : d <= kBand2 ? span(d, kCm100, kCm150) : span(d, kCm150, kCm200);
        const anim::Clip *ca = anims_.find(a), *cb = anims_.find(b);
        if (!ca || !cb) continue;
        const vec2 land = xy(s_.pos) + out * -glm::mix(rootTranslation(*ca, ca->durationSeconds(), false).y, rootTranslation(*cb, cb->durationSeconds(), false).y, w);
        const auto g = col_.groundProbe(vec3(land, s_.pos.z + 60), kBand3 + 120.0f);
        if (!g || g->normal.z < t_.minFloorNormalZ || std::abs(g->point.z - below->point.z) > t_.stepUp) continue;
        s_.hurdle = {};
        s_.hurdle.next = PlayerState::Hurdle::Down;
        s_.hurdle.edge = g->point;
        s_.hurdle.out = -out;
        s_.hurdle.h = -d;
        s_.hurdle.endZ = g->point.z;
        s_.yaw = yawOf(out);
        s_.vel = vec3(0);
        s_.state = State::ClimbUp;
        playMix(a, b, w);
        return true;
    }
    return false;
}

void Player::startHurdleFollowUp(bool low) {
    const auto& hu = s_.hurdle;
    switch (hu.next) {
    case PlayerState::Hurdle::Onto:
        if (low) playMix("hurdle_low_onto", "hurdle_low_onto_100", s_.mix);
        else play("hurdle_onto", false);
        break;
    case PlayerState::Hurdle::Over:
        if (!low) playMix("hurdle_over", "hurdle_over_long_100", hu.mix);
        else if (hu.alt) playMix("hurdle_over_low_long_50", "hurdle_over_low_long_100", hu.mix);
        else playMix("hurdle_low_over", "hurdle_low_over_100", hu.mix);
        break;
    default: { // ToLedge: the 25 / 65 or 65 / 100 cm deep pair
        const std::string base = low ? "hurdle_to_ledgeb_low_" : "hurdle_to_ledgeb_mid_";
        playMix(base + (hu.alt ? "65" : "25"), base + (hu.alt ? "100" : "65"), hu.mix);
        // Turning half round into the hang: start at the heading that ends on the hang's.
        const anim::Clip* c = anims_.find(s_.clip);
        const float hangYaw = attachPose(hu.farLedge, hu.farSeg, hu.farT, hu.farSide).second;
        if (c) s_.yaw = wrapAngle(hangYaw - (rootYaw(*c, c->durationSeconds(), false) - rootYaw(*c, s_.clipTime, false)));
        break;
    }
    }
}

bool Player::hurdleTick(vec2 move, float dt) {
    auto& hu = s_.hurdle;
    const bool low = s_.clip.find("_low_") != std::string::npos;
    const auto hands = [&]() -> std::optional<vec3> { // blended hands now (model space, root removed)
        const anim::Clip *c = anims_.find(s_.clip), *mc = s_.mix > 0 ? anims_.find(s_.clipMix) : nullptr;
        const auto h = c ? anims_.handsAt(*c, s_.clipTime) : std::nullopt;
        const auto hm = mc && c ? anims_.handsAt(*mc, s_.clipTime / c->durationSeconds() * mc->durationSeconds()) : std::nullopt;
        return h && hm ? std::optional<vec3>(glm::mix(*h, *hm, s_.mix)) : h;
    };
    if (s_.clip.starts_with("hurdle_ground_attach") && !s_.clip.ends_with("_cancel")) {
        const bool high = s_.clip.find("_high_") != std::string::npos;
        if (glm::dot(move, xy(forward())) < -0.5f) { // pulled back: cancel (requests 147 / 149 / 148)
            const float w = s_.mix;
            if (low) playMix("hurdle_ground_attach_low_50_cancel", "hurdle_ground_attach_low_100_cancel", w);
            else if (high) playMix("hurdle_ground_attach_mid_150_cancel", "hurdle_ground_attach_high_200_cancel", w);
            else playMix("hurdle_ground_attach_mid_100_cancel", "hurdle_ground_attach_mid_150_cancel", w);
            return false;
        }
        const anim::Clip* a = anims_.find(s_.clip);
        if (!low) { // square up: root to the edge + 86.94 out (anim target 0x273, S954 property 0x119), over the attach clip
            const float f = a ? float(std::min(1.0, dt / std::max(double(dt), a->durationSeconds() - s_.clipTime))) : 1.0f;
            s_.pos += vec3((xy(hu.edge) + hu.out * kAttachOut - xy(s_.pos)) * f, 0);
            s_.yaw = wrapAngle(s_.yaw + wrapAngle(yawOf(-hu.out) - s_.yaw) * f);
        }
        advanceClip(dt);
        if (!clipDone()) return false;
        const auto end = hands();
        startHurdleFollowUp(low);
        // Mid / high: moved so the hands stay where the attach left them (on the edge; the follow-up starts mid-clip).
        if (!low)
            if (const auto start = hands(); start && end) s_.pos += toWorld(*end - *start);
        return false;
    }
    if (s_.clip.ends_with("_cancel")) {
        s_.pos += toWorld(advanceClip(dt));
        return clipDone();
    }
    if (hu.next == PlayerState::Hurdle::ToLedge && hu.farLedge >= 0) {
        // Root motion and half turn, drawn onto the hang pose at the far ledge by the clip end.
        s_.pos += advanceRoot(dt);
        const anim::Clip* c = anims_.find(s_.clip);
        const auto [hang, hangYaw] = attachPose(hu.farLedge, hu.farSeg, hu.farT, hu.farSide);
        if (c) {
            const float base = s_.yaw - rootYaw(*c, s_.clipTime, false);
            const double left = c->durationSeconds() - s_.clipTime;
            const float f = left <= dt ? 1.0f : float(dt / left);
            s_.pos += (hang - (s_.pos + rotYaw(base, rootLeft()))) * f;
        }
        if (!clipDone()) return false;
        s_.ledge = hu.farLedge, s_.ledgeSeg = hu.farSeg, s_.ledgeT = hu.farT, s_.ledgeSide = hu.farSide;
        s_.state = State::Hang;
        play("ledgeb_idle", true);
        placeOnLedge();
        return false;
    }
    // Onto / down: root motion, the rise / drop scaled to end on the top / the floor below. Over: the clip's own arc
    // (the request's blend clears 50..100 cm; mid / high start raised onto the edge), drawn down onto her floor.
    const float zLeft = rootLeft().z;
    vec3 d = toWorld(advanceClip(dt));
    if (const anim::Clip* c = hu.draw ? anims_.find(s_.clip) : nullptr) d += vec3(hu.drawOff * float(dt / c->durationSeconds()), 0);
    if (hu.next != PlayerState::Hurdle::Over) {
        if (std::abs(zLeft) > 1.0f) d.z *= (hu.endZ - s_.pos.z) / zLeft;
    } else if (const anim::Clip* c = anims_.find(s_.clip)) {
        const double left = c->durationSeconds() - s_.clipTime;
        d.z += (hu.endZ - (s_.pos.z + zLeft)) * (left <= 0 ? 1.0f : std::min(1.0f, float(dt / (left + dt))));
    }
    s_.pos += d;
    return clipDone();
}

// Targeted jump from a hang (kind 0 up, +-1 right/left, 2 back) to another ledge: the hang states' jump queries
// (lara.drm; tru.exe runs them in the lean states S434 / S456 / S457 [O: Wine, l3_main_bedroom]), from the attach point
// (+0xCC = 1: player data +0xCA0, the ledge point she hangs from [I]) along her facing f [O-data]:
// - q10 up: from 79 out of the wall (-79 along f; its target type 3 there too), 75.85 up a 30 Hz frame, coming down
//   (+0x2C 0x3890), cone 65 degrees about f, record 0x2A9EC, no class table;
// - q7 / q8 to the sides: from 474.1 under it and 63.21 out, target type 3 from the attach point, type 1 474.08 under
//   it; 70.80 aside + 34.77 up a frame, cone 45 degrees about that side, +0x28 553 (the hit at least that far from the
//   attach point: record 0x2AA4C's kind 1), with the class table (+0x2C 0x3850), record 0x2AA4C;
// - q9 back: the same starts, 57.33 out + 42.54 up a frame, cone 90 degrees about -f, record 0x2A9EC (class table).
// ledgeOnArc runs them; the first ledge met is the target. Sideways and back, the take-off clip plays first with its
// root motion (tru.exe under Wine: ledgeb_jump_right / _back carry her 122 aside + 107 up / 62 out + 32 up and turn her
// before she leaves); launchHangJump() then aims the flight. (Pole flings: Player::poleFling.)
Player::JumpQuery Player::hangJumpQuery(int kind) const {
    const vec3 at = pointOn(col_.markup[size_t(s_.ledge)], s_.ledgeSeg, s_.ledgeT);
    const vec2 f = xy(forward()), r(std::sin(s_.yaw), -std::cos(s_.yaw));
    JumpQuery q;
    q.base = at, q.attach = 1, q.hang = at, q.root = at - vec3(0, 0, 474.083f);
    q.start = at + vec3(-f * 63.2111f, -474.083f);
    q.record = JumpRecord::HangLadder;
    if (kind == 0) {
        q.start = q.hang = q.root = at + vec3(-f * 79.0139f, 0);
        q.v = vec3(0, 0, 75.85f), q.dir = f, q.cone = glm::radians(65.0f), q.falling = true;
    } else if (kind == 2) {
        q.v = vec3(-f * 57.33f, 42.54f), q.dir = -f, q.cone = glm::radians(90.0f), q.classify = true;
    } else {
        q.v = vec3(r * (70.80f * float(kind)), 34.77f), q.dir = r * float(kind), q.cone = glm::radians(45.0f);
        q.minDist = 553, q.classify = true, q.record = JumpRecord::Side;
    }
    return q;
}

Player::JumpTarget Player::hangJumpTarget(int kind) const { return ledgeOnArc(hangJumpQuery(kind)); }

bool Player::startHangJump(int kind) {
    const JumpTarget j = hangJumpTarget(kind);
    if (j.markup < 0) return false;
    s_.ledge = j.markup, s_.ledgeSeg = j.seg, s_.ledgeT = j.t, s_.ledgeSide = j.side, s_.polePara = false;
    s_.state = State::HangJump;
    s_.fallStartZ = s_.pos.z;
    // Left: ledgeb_jump_right mirrored (lara.drm request 979 has the mirror bit, 1009 is the same clip unmirrored) [O].
    play(kind == 0 ? "ledgeb_jump_up" : kind == 2 ? "ledgeb_jump_back" : "ledgeb_jump_right", false, 1, kind == -1);
    s_.vel = vec3(0); // not launched yet (HangJump: the take-off clip first)
    if (kind == 0) launchHangJump();
    return true;
}

namespace {
// Times in [0, frames] where |r0 + r1 t + r2 t^2| has a local minimum, at most two (tru.exe 0x6F9940's cubic
// (r . r' = 0), its roots clamped to the frame range) [O].
int arcMinima(vec3 r0, vec3 r1, vec3 r2, float frames, float out[2]) {
    const float c0 = glm::dot(r0, r1), c1 = glm::dot(r1, r1) + 2 * glm::dot(r0, r2), c2 = 3 * glm::dot(r1, r2),
                c3 = 2 * glm::dot(r2, r2);
    const auto f = [&](float t) { return ((c3 * t + c2) * t + c1) * t + c0; };
    float cuts[4] = {0, 0, 0, 0}; // 0, f's turning points inside the range, frames
    int n = 1;
    if (const float a = 3 * c3, b = 2 * c2; std::abs(a) > 1e-9f) { // f' = a t^2 + b t + c1
        if (const float disc = b * b - 4 * a * c1; disc > 0) {
            const float s = std::sqrt(disc), t0 = (-b - s) / (2 * a), t1 = (-b + s) / (2 * a);
            for (float t : {std::min(t0, t1), std::max(t0, t1)})
                if (t > 0 && t < frames) cuts[n++] = t;
        }
    } else if (std::abs(b) > 1e-9f && -c1 / b > 0 && -c1 / b < frames) {
        cuts[n++] = -c1 / b;
    }
    cuts[n++] = frames;
    int found = 0;
    for (int i = 0; i + 1 < n && found < 2; ++i) {
        float lo = cuts[i], hi = cuts[i + 1];
        if (!(f(lo) < 0 && f(hi) >= 0)) continue;
        for (int k = 0; k < 40; ++k) {
            const float mid = 0.5f * (lo + hi);
            (f(mid) < 0 ? lo : hi) = mid;
        }
        out[found++] = hi;
    }
    if (found < 2 && f(frames) < 0) out[found++] = frames; // still closing at the last frame: clamped there
    return found;
}

// lara.drm's default markup record (player data +0x584; ledges, MUD 0x40000000): target types 3 / 1, the capsule
// offset from the query's arc point (12.64 behind her heading, 219.03 down), height, radius and end insets [O].
constexpr float kCapsuleBack = 12.64f, kCapsuleDown = 219.03f, kCapsuleHeight = 252.84f, kCapsuleRadius = 316.06f;
constexpr float kLedgeInset = 79.0f;
constexpr float kQueryG = 5.72443f;
constexpr float kLedgeSide = -1; // a ledge's hang side: right of its direction (attachPose side -1) [O-data]
// Lara's jump queries name a class record at +0x94, so 0x7296C0 runs it (0x729670 -> 0x729540) on every swept candidate
// instead of the default checks: a candidate counts when one of the record's entries passes (0x729540 returns at the
// first). An entry is gated by the candidate markup's type (0x723330: entry flags 0x10, bit 0 = not in the list) and,
// while she is attached to one (query state +0x280 = the candidate of player data +0xC94), by the attached markup's
// type (0x723380: flags 0x04, bit 3 = not in the list); then all its tests must hold (0x729420 -> 0x728F00). A test
// is {kind, point source a1, a2, value, flags: bit 0 the value given, else the query's own (kind 1: +0x28, 13: +0x20,
// 22: +0x24 = query state +0x50 / +0x4C / +0x54), bit 1 negated, bit 2 active} [O: decomp]. Point sources (0x723AB0):
// 0 her root, 4 / 31 the hit (the ledge point), 9 / 10 the query's heading / right, 13 the target type's arc at the
// hit time, 24 the hit less the query start and linear motion, 28 the query's base point, 33 the target markup's point
// (holds). Kinds: 1 horizontal distance a1..a2 < v; 4 z(a2) - z(a1) <= v; 10 the ledge record's capsule round source
// 13 holds the hit (0x725B50 -> 0x6FCB10); 11 a moving markup's angle (static: passes, 0x6EC2E0); 12 the arc's vertical
// speed at the hit < v (no value: only for +0x2C bit 0x80); 13 hit time < v; 14 / 18 / 19 / 20 the ledge class (0x728A80:
// not 3 / 11 / 12 / 13, = 6, = 5, 8..10); 21 the wall side (0x725D00 -> 0x6FC6B0); 22 the cone (0x6FC530); 31 the hit
// at most player data +0xC18 = 1583.44 under her (sources 27 / 32: her z + the record's +0x68, the hit less it; the
// ledge record's is 0); 33 for classes 5 / 6 the class table's +0xA4 value > v; 34 source a1 along direction a2 < v
// (0x7256F0 0x22); 36 horizontal distance < v + the drop when the hit is below her, applied only while neither she
// (query state +0x30, her base's velocity) nor the markup moves faster than 3.1605 a frame (0x725F40) [O: decomp].
struct ClassTest { uint8_t kind, a1, a2, flags; float value; };
struct ClassEntry { uint8_t flags; std::span<const uint8_t> types, attach; std::span<const ClassTest> tests; };
constexpr uint8_t kHoldTypes[] = {16, 17, 18, 19, 20}, kHoldTypes15[] = {16, 17, 18, 19, 20, 15};
constexpr uint8_t kT14[] = {14}, kT15[] = {15}, kT14_15_2_13[] = {14, 15, 2, 13}, kT2_13[] = {2, 13}, kT4_1[] = {4, 1};
constexpr uint8_t kT14_15[] = {14, 15};
constexpr ClassTest kAngle{11, 0, 0, 4, 0}, kCapsule{10, 13, 4, 4, 0}, kCone{22, 0, 0, 4, 0}, kWall{21, 0, 0, 4, 0};
constexpr ClassTest kAfterMin{13, 0, 0, 6, 0}, kVzOff{12, 0, 0, 0, 0}, kVzDown{12, 0, 0, 5, 1}, kDepth{31, 27, 32, 4, 0};
constexpr ClassTest kClass{14, 0, 0, 4, 0}, kNot6{18, 0, 0, 6, 0}, kNot5{19, 0, 0, 6, 0}, kNotDown{20, 0, 0, 6, 0};
constexpr ClassTest kHurdleA4{33, 0, 0, 5, -375}, kRise700{4, 0, 4, 5, 700}, kFar{1, 28, 31, 6, 3};
// lara.drm 0x2AB8C (q2 / q26 / q43 and q1, q6, q20, q94, q102..q104) [O-data]
constexpr ClassTest kG0[] = {kAngle, kCapsule, kCone, kWall, kAfterMin, kVzDown, kDepth, {36, 0, 4, 5, 1700}};
constexpr ClassTest kG1[] = {kRise700, {36, 0, 4, 5, 1581}, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth,
                             kClass, kHurdleA4};
constexpr ClassTest kG2[] = {kAngle, {36, 0, 4, 5, 1581}, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass,
                             kNotDown, kNot5, kHurdleA4};
constexpr ClassTest kG3[] = {kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass, kHurdleA4};
constexpr ClassTest kG4[] = {{34, 24, 9, 5, 632}, {34, 24, 9, 7, -150}, {34, 24, 10, 7, -316}, {34, 24, 10, 5, 316},
                             {4, 0, 31, 5, 885}, {4, 0, 31, 7, 300}, kWall, kCone, kClass, kNotDown, kNot5};
constexpr ClassEntry kGround[] = {{0x12, kT14, {}, kG0}, {0x12, kT15, {}, kG1}, {0x13, kT14_15_2_13, {}, kG2},
                                  {0x12, kT2_13, {}, kG3}, {0x12, kT4_1, {}, kG4}};
// lara.drm 0x2AA4C (q7 / q8 hang sides, q28 / q29 DICE sides, q30, q38) [O-data]
constexpr ClassTest kS0[] = {kFar, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzDown};
constexpr ClassTest kS1[] = {kRise700, kFar, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass, kNot6,
                             kHurdleA4};
constexpr ClassTest kS2[] = {kFar, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass, kHurdleA4, kNot6,
                             kNotDown};
constexpr ClassTest kS3[] = {{1, 28, 4, 6, 360}, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth};
constexpr ClassEntry kSide[] = {{0x12, kT14, {}, kS0}, {0x1F, {}, kHoldTypes, kS1}, {0x1F, {}, kHoldTypes15, kS2},
                                {0x06, {}, kHoldTypes, kS3}};
// lara.drm 0x2A9EC (q9 / q10 hang back / up, q22 / q23 ladder, q32 / q33 poles and 17 more) [O-data]
constexpr ClassTest kH0[] = {kAngle, kCapsule, kCone, kWall, kAfterMin, kVzDown, kDepth};
constexpr ClassTest kH1[] = {kRise700, kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass, kHurdleA4};
constexpr ClassTest kH2[] = {kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass, kHurdleA4, kNot6,
                             kNotDown};
constexpr ClassEntry kHangLadder[] = {{0x12, kT14, {}, kH0}, {0x12, kT15, {}, kH1}, {0x13, kT14_15, {}, kH2}};
// lara.drm 0x2AE2C (q31, the DICE jump up): entry 0 {0x16: holds, attached to a hold; the hit within 120 of the hold
// point, ...} (holds: Climb.cpp's sweep) and entry 1 {0x1B: not holds; its attach list not gated (no 0x04)}, so from
// a hold it meets ledges too [O-data; tru.exe under Wine, l2_cliffclimb hold 5 -> ledge 45]
constexpr ClassTest kD1[] = {kAngle, kCapsule, kCone, kWall, kAfterMin, kVzOff, kDepth, kClass};
constexpr ClassEntry kDiceUp[] = {{0x1B, kHoldTypes, kHoldTypes, kD1}};
std::span<const ClassEntry> classRecord(int r) { // Player::JumpRecord: 1 Ground, 2 Side, 3 HangLadder, 4 DiceUp
    switch (r) {
    case 1: return kGround;
    case 2: return kSide;
    case 3: return kHangLadder;
    case 4: return kDiceUp;
    default: return {};
    }
}
// 0x723330 / 0x723380: `type` in `list`, or not in it with `exclude`
bool listed(std::span<const uint8_t> list, int type, bool exclude) {
    return (std::find(list.begin(), list.end(), uint8_t(type)) != list.end()) != exclude;
}
constexpr float kPlayerDepth = 1583.4387f; // player data +0xC18 (kind 31) [O-data]
// The ledge class table (objdata +0x121C0, 0x728650; only for queries with +0x2C bit 0x40): the floor 94.815 out of the
// face below the ledge point (+0xA8) in bands: 0..154.87 down class 11; 154.87..312.90 / ..474.08 / ..635.27 down 5 /
// 6 / 7, or 8 / 9 / 10 when she stands on the ledge (kind 2: the point < 6.33 over her, facing out of its face), each
// with the clearance test 0 (0x727BF0, ledgeClear); later entries need a moving markup (class 0, entry 7), the player
// chart's test 1 (0x74BB50, class 4, entry 8) or the value of 0x726420 (+0xA4) [O-data, decomp]. Ours: 4 for the rest
// [I: test 1 not decoded; the Wine standing jumps that caught 782..907 ledges need a class kind 14 admits, which a
// fall to class 12 (floor over 632.1 down) or 3 would not].
int ledgeClassOf(const collision::CollisionMesh& col, vec3 h, vec2 out, vec3 self, vec2 dir) {
    const auto floor = col.groundProbe(vec3(xy(h) + out * kFloorOut, h.z + 31.605f), kBand3 + 400.0f);
    if (!floor) return 4;
    const float d = floor->point.z - h.z;
    const bool onIt = h.z - self.z < 6.3281126f && glm::dot(dir, out) > -0.1f;
    const int band = d > -kBand0 && d < 0 ? 0 : d > -kBand1 && d < -kBand0 ? 1 : d > -kBand2 && d < -kBand1 ? 2
                     : d > -kBand3 && d < -kBand2 ? 3 : -1;
    if (band < 0 || !ledgeClear(col, h, out)) return 4;
    return band == 0 ? 11 : (onIt ? 7 : 4) + band;
}
constexpr float kSweepPad = 424.5f;
// A running jump's box catch (record 0x2AB8C entry 4) reaches a ledge up to this far (horizontally, root at take-off ->
// the ledge point) [O: tru.exe under Wine, l4_temple ledge 1204 (782 over the floor): caught from 1222.9, not from
// 1243.4; I: what in tru.exe sets it]
constexpr float kRunJumpReach = 1233.0f;
} // namespace

// Jump queries meeting ledges [O: tru.exe 0x70F300 sweep, 0x70EC60 checks, 0x6FCB10 capsule; lara.drm player data
// +0x584]. For each of the ledge record's target types (3, 1) the arc from that type's start (the query's +0x8C entry
// for it) gives the times of closest approach to each segment's line (0x6F9940: at most two minima, clamped to the
// frame range); the nearest segment point there (the polyline's first / last ends inset 79), or, past an end, the
// arc's closest approach to that end, is the hit. It counts when (0x70EC60): later than the query's minimum time
// (+0x20); for queries with +0x2C bit 0x80 the arc is coming down (vz < 0.05 a frame, 0x6EC3D0); the point lies in the
// record's capsule (0x6FCB10: upright, centred 219.03 under and 12.64 behind the QUERY'S OWN arc point at that time —
// the arc from its start, not the target type's — half-height 126.42, radius 316.06 round its axis and end points);
// it is within the query's cone (+0x24) of its heading (0x6FC530); and the arc's horizontal motion (or, standing
// still, the way to the point) is not away from the ledge's wall (0x6FC6B0 MUD 0x40000000: dot with the ledge's
// outward normal, 0x737D60, <= 0.2618) [O]. A ledge hangs to the right of its polyline's direction (kLedgeSide: every
// ledge with a wall under one side and its top behind it in l2 / l3 / l4 / l7 units is wound so) [O-data], so from
// behind (its top) the way to it points out of its face and is refused: l3_main_hall ledge 944 hangs toward -x and the
// standing jump under Wine that catches nothing was made on its +x side, 113 behind its line. Ours adds that the hang
// has room under it (openBelow) and keeps the earliest hit [I: 0x710100 keeps the first of its list]. Queries naming
// a class record (`record`) take its entries instead of 0x70EC60's checks (above the anonymous namespace's tables).
Player::JumpTarget Player::ledgeOnArc(const JumpQuery& q) const {
    const vec3 a2(0, 0, -0.5f * kQueryG), dir3(q.dir, 0);
    const vec2 right(q.dir.y, -q.dir.x);
    const auto arcAt = [&](vec3 from, float t) { return from + q.v * t + a2 * (t * t); };
    JumpTarget best;
    best.frames = 1e9f;
    const auto capsule = [&](vec3 c, vec3 h) {
        constexpr float hh = 0.5f * kCapsuleHeight;
        c -= dir3 * kCapsuleBack + vec3(0, 0, kCapsuleDown);
        const float d = h.z > c.z + hh ? glm::distance(h, c + vec3(0, 0, hh))
                        : h.z < c.z - hh ? glm::distance(h, c - vec3(0, 0, hh))
                                         : glm::length(xy(h - c));
        return d <= kCapsuleRadius;
    };
    const std::span<const ClassEntry> record = classRecord(int(q.record));
    // from: the target type's arc start the hit time t belongs to
    const auto check = [&](int li, int si, float t, vec3 h, vec3 from) {
        // +0x2C bit 0x80, the arc coming down (0x6EC3D0): a default check (0x70EC60), not in the class record path
        // (0x7296C0) [O: decomp]; ours keeps it for both to time the re-aimed flight (the record path's box test holds
        // at every time, its first hit is the take-off) [I]
        if (h.z < q.minZ || t >= best.frames || (q.falling && q.v.z - kQueryG * t >= 0.05f)) return;
        int entry = -1;
        const auto& m = col_.markup[size_t(li)];
        const vec2 sd = segDir(m, si), n(-sd.y, sd.x), toHit = xy(h - q.start);
        constexpr float side = kLedgeSide;
        const auto cone = [&](float c) { return glm::length(toHit) <= 1e-3f || glm::dot(glm::normalize(toHit), q.dir) >= std::cos(c); };
        const auto wallSide = [&] {
            vec2 motion = xy(q.v);
            if (glm::length(motion) < 1e-3f) motion = glm::length(toHit) > 1e-3f ? toHit : q.dir;
            return glm::dot(glm::normalize(motion), n * side) <= 0.2617994f;
        };
        if (record.empty()) { // the default checks (0x70EC60)
            if (t <= q.minT || !capsule(arcAt(q.start, t), h) || !cone(q.cone) ||
                !wallSide())
                return;
        } else {
            int cls = -1; // the ledge class, worked out on first use (0x728A80)
            const auto ledgeClass = [&] {
                if (cls < 0) cls = q.classify ? ledgeClassOf(col_, h, n * side, s_.pos, q.dir) : 0;
                return cls;
            };
            const auto point = [&](uint8_t src) {
                switch (src) {
                case 0: return s_.pos; // her root (her instance's position)
                case 13: return arcAt(from, t);
                case 24: return h - (q.start + q.v * t);
                case 28: return q.base;
                default: return h; // 4, 31 (33: a hold's point; ledges are no holds)
                }
            };
            const auto pass = [&](const ClassTest& c) {
                if (!(c.flags & 4)) return true;
                const bool given = c.flags & 1;
                bool r = true, neg = c.flags & 2;
                switch (c.kind) {
                case 1: r = glm::length(xy(point(c.a2) - point(c.a1))) < (given ? c.value : q.minDist); break;
                case 4: r = point(c.a2).z - point(c.a1).z <= c.value; break;
                case 10: r = capsule(arcAt(from, t), h); break;
                case 12:
                    if (!given && !q.falling) break;
                    if (!given) neg = true; // 0x728F00 forces the negation with -1
                    r = q.v.z - kQueryG * t < (given ? c.value : -1.0f);
                    break;
                case 13: r = t < (given ? c.value : q.minT); break;
                case 14: r = ledgeClass() != 3 && ledgeClass() < 11; break;
                case 18: r = ledgeClass() == 6; break;
                case 19: r = ledgeClass() == 5; break;
                case 20: r = ledgeClass() >= 8 && ledgeClass() <= 10; break;
                case 21: r = wallSide(); break;
                case 22: r = cone(given ? c.value : q.cone); break;
                case 3: r = std::abs(point(c.a2).z - point(c.a1).z) <= c.value; break;
                case 16: r = ledgeClass() >= 5 && ledgeClass() <= 11; break;
                case 25: { // 0x725980: the angle between her motion (15) and the way from the arc's linear point to the
                    // hit (23), both flattened (the instance's up axis), under v degrees; either one nil passes
                    const vec2 a = xy(q.v), b = xy(h - (q.start + q.v * t));
                    r = glm::length(a) < 1e-4f || glm::length(b) < 1e-4f ||
                        glm::dot(glm::normalize(a), glm::normalize(b)) > std::cos(glm::radians(c.value));
                    break;
                }
                case 27: r = false; break; // her base moving (squared speed > 1): the static world
                case 31: r = h.z - s_.pos.z > -(given ? c.value : kPlayerDepth); break;
                case 34: r = glm::dot(xy(point(c.a1)), c.a2 == 9 ? q.dir : right) < c.value; break;
                case 36: { // the markups and her base are static here
                    const vec3 a = point(c.a1), b = point(c.a2);
                    r = glm::length(xy(b - a)) < c.value + std::max(0.0f, a.z - b.z);
                    break;
                }
                default: break; // 11 (a static markup), 33 ([U]: 0x726420's value)
                }
                return r != neg;
            };
            const auto hit = std::find_if(record.begin(), record.end(), [&](const ClassEntry& e) {
                if ((e.flags & 0x10) && !listed(e.types, m.type, e.flags & 1)) return false;
                if ((e.flags & 4) && q.attach != 0 && !listed(e.attach, q.attach, e.flags & 8)) return false;
                return std::all_of(e.tests.begin(), e.tests.end(), pass);
            });
            if (hit == record.end()) return;
            entry = int(hit - record.begin());
        }
        const float along = glm::length(xy(h - m.points[size_t(si)]));
        if (openBelow(col_, m, si, along, side)) best = {li, si, along, side, t, entry};
    };
    // The class path's candidates: the markup sweep-and-prune pass over x / y (0x70F300 -> 0x463DE0, axes mask 3) of
    // the query's start -> start + its whole-range displacement against each ledge segment's box grown by kSweepPad.
    const vec3 end = arcAt(q.start, q.frames);
    const vec2 sLo = glm::min(xy(q.start), xy(end)), sHi = glm::max(xy(q.start), xy(end));
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1 || int(li) == s_.ledge) continue;
        const int last = int(m.points.size()) - 2;
        for (int si = 0; si <= last; ++si) {
            const vec3 a = m.points[size_t(si)], b = m.points[size_t(si) + 1];
            const float len = glm::distance(a, b);
            if (len < 1e-2f || segLen(m, si) < 1e-2f) continue;
            if (!record.empty() && (glm::any(glm::greaterThan(glm::min(xy(a), xy(b)) - kSweepPad, sHi)) ||
                              glm::any(glm::lessThan(glm::max(xy(a), xy(b)) + kSweepPad, sLo))))
                continue;
            const vec3 u = (b - a) / len;
            const float lo = si == 0 ? kLedgeInset : 0, hi = len - (si == last ? kLedgeInset : 0);
            const auto perp = [&](vec3 w) { return w - u * glm::dot(w, u); };
            for (const vec3& from : {q.hang, q.root}) {
                float ts[2];
                const int nt = arcMinima(perp(from - a), perp(q.v), perp(a2), q.frames, ts);
                for (int k = 0; k < nt; ++k) {
                    const float s = glm::dot(arcAt(from, ts[k]) - a, u);
                    if (lo <= hi && s >= lo && s <= hi) { check(int(li), si, ts[k], a + u * s, from); continue; }
                    const vec3 e = a + u * (lo > hi ? 0.5f * len : std::clamp(s, lo, hi)); // past an end: that end
                    float te[2];
                    const int ne = arcMinima(from - e, q.v, a2, q.frames, te);
                    for (int j = 0; j < ne; ++j) check(int(li), si, te[j], e, from);
                }
            }
        }
    }
    if (best.markup < 0) best.frames = 0;
    return best;
}

// A standing jump at a high ledge in front of her [O: tru.exe under Wine, l4_temple, Lara teleported to rest facing
// the ledge, then the jump key; openunderworld-work/ledgecatch, runjump]: S248 runs q2, whose class record (above,
// ledgeOnArc's `record`) took ledges 782 and 830 over her feet from 100..424 out of their line but none from 425
// (no candidate reaches the class tests there), 892 and 907 ones from 100 but not from 125 / 200 (above the box's 885:
// only the capsule round the hands' apex, root + 707.27, takes them), a 1233 one from nowhere. A hit makes S278 go to
// S289, whose ledge class (0x728650: the floor 94.8 out below it) picks S535 for classes 0 / 2 / 12 and the hurdle
// attaches for 5 / 6 / 7 (S958 / S959 / S957: ledges up to the bands' top, 635.27, are hurdled: a 610 one
// hurdle_ground_attach_high) [O-data]. S535 is no ballistic arc: her root rushes straight, at a constant speed
// whatever the distance, for 11 ticks of 60 Hz to 113.8 out of the line and 604.2 under it, then blends into the hang
// (root 161 out, 506 under) over 0.15 s. Ours: q2's class hit above kBand3, the hang clear, a straight 11/60 s leap
// onto the hang pose (HangJump with the standing air clip), then the attach [I: one leg instead of the rush and blend].
bool Player::standJumpToLedge() {
    constexpr float kStandLeap = 11.0f / 60.0f, kHangTarget = 505.69f;
    if (s_.regrabCooldown > 0) return false;
    JumpQuery q; // q2: 48.04 up a 30 Hz frame from 300 over the root, cone 120 degrees, coming down
    q.start = s_.pos + vec3(0, 0, 300.0f), q.hang = s_.pos + vec3(0, 0, kHangTarget), q.root = s_.pos;
    q.v = vec3(0, 0, 48.04f), q.dir = xy(forward()), q.cone = glm::radians(120.0f), q.falling = true, q.base = s_.pos;
    q.record = JumpRecord::Ground, q.classify = true; // record 0x2AB8C, +0x2C 0x18D0
    q.minZ = s_.pos.z + kBand3;
    const JumpTarget best = ledgeOnArc(q);
    if (best.markup < 0) return false;
    s_.ledge = best.markup, s_.ledgeSeg = best.seg, s_.ledgeT = best.t, s_.ledgeSide = best.side, s_.polePara = false;
    s_.vel = (attachPose(best.markup, best.seg, best.t, best.side).first - s_.pos) / kStandLeap;
    s_.flight = kStandLeap;
    s_.state = State::HangJump;
    return true;
}

// The flight onto the HangJump target, as tru.exe's jump-to-target solver 0x73EDC0 aims it: v = (target - pos -
// g t^2 / 2) / t, the root arriving exactly on the target's hang pose after t. The take-off vertical speed is the jump
// query's (q10 up 75.85, q7 / q8 sideways 34.77, q9 back 42.54 a frame) and t when that rise and fall reaches the
// target's height (rising for a target above, else falling) [I: tru.exe times it by the query arc's first box hit];
// a target above that arc rises hangJumpApex above the higher end instead [I].
void Player::launchHangJump(float vz) {
    const vec3 goal = col_.markup[size_t(s_.ledge)].type == 9 ? ladderCatch(s_.ledge, s_.ledgeSeg)
                                                              : attachPose(s_.ledge, s_.ledgeSeg, s_.ledgeT, s_.ledgeSide).first;
    const vec3 delta = goal - s_.pos;
    const float g = t_.gravity;
    const float vq = vz > 0                         ? vz
                     : s_.clip == "ledgeb_jump_up"   ? t_.hangJumpUpVz
                     : s_.clip == "ledgeb_jump_back" ? t_.hangJumpBackVz
                     : s_.clip.starts_with("ledgeb_jump_right") ? t_.hangJumpSideVz
                     : s_.clip.starts_with("ladder_jump_")      ? t_.ladderJumpVz
                     : s_.clip == "Wall_hug_Jump_Up_Twist"      ? t_.wallHugTwistVz
                     : s_.clip.starts_with("Wall_Hug_")         ? t_.wallHugJumpVz : 0.0f;
    float time = -1;
    vz = vq;
    if (const float disc = vq * vq - 2 * g * delta.z; vq > 0 && disc >= 0)
        time = (delta.z > 0 ? vq - std::sqrt(disc) : vq + std::sqrt(disc)) / g;
    if (time <= 1e-3f) {
        vz = std::sqrt(2 * g * (std::max(delta.z, 0.0f) + t_.hangJumpApex));
        time = (vz + std::sqrt(vz * vz - 2 * g * delta.z)) / g;
    }
    s_.vel = vec3(xy(delta) / time, vz);
    s_.flight = time;
}

// Hanging from a horizontal pole (markup 4 / 6), perpendicular (facing across it) or parallel (facing along it,
// polePara). lara.drm anim requests [O]: idles 1356 HPole_idleHangPerp / 194 hpole_IdleHangPara; turns 1351
// Hpole_PerptoPara, 1350 Hpole_ParatoPerp, 1352 Hpole_Perpturn180, 1349 Hpole_Paraturn180; hand over hand 1338
// HPole_ParaForward_trans, 1342 HPole_ParaForward, 1340 / 1341 HPole_ParaForward_to_idle_L_hand / _R_hand; swing 1354
// HPole_StartSwing (= phase requests 1312 + 1311), 1355 HPole_swing (= phases 1320 / 1319 / 1318 / 1317: 7 / 5 / 19 /
// 10 frames), 1330 Hpole_startswing_endA, 1333 / 1332 / 1331 Hpole_swingstopA / B / C, 1329 Hpole_swingreverseA; flings
// 1308 / 1309; drops 1361 / 1362. The state chart picks them from the stick's orientation against the hang's (its
// conditions 0x195 / 0x196, values 450-453: two perpendicular, two parallel); the swing is these clips alone (no
// physics conditions on the swing states). Which stick angle selects which, and the stop clip by the phase the loop
// is released in (the clip whose first pose matches that phase's end: B after phase 2, C after 3, A after 4), are [I].
void Player::pole(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (s_.ledge < 0 || size_t(s_.ledge) >= col_.markup.size()) { startFall(s_.vel); return; }
    const auto& m = col_.markup[size_t(s_.ledge)];
    const std::string clip = s_.clip;
    const bool attaching = clip == "Hpole_perp_attach_M" || clip == "Hpole_para_attach_S";
    const bool turning = clip == "Hpole_PerptoPara" || clip == "Hpole_ParatoPerp" || clip == "Hpole_Paraturn180" ||
                         clip == "Hpole_swingreverseA";
    if (dropPressed && !turning) {
        const bool para = s_.polePara;
        s_.regrabCooldown = 0.4f;
        startFall(vec3(0));
        play(para ? "Hpole_detach_dropPara" : "Hpole_detach_dropPerp", false);
        return;
    }
    const bool flinging = clip == "hpole_perp_fling" || clip == "hpole_para_fling";
    if (jumpPressed && !turning && !attaching && !flinging) {
        if ((clip == "HPole_idleHangPerp" || clip == "hpole_IdleHangPara") && glm::length(move) <= 0.5f) { // onto the top
            s_.state = State::Perch;
            play(s_.polePara ? "hpole_para_to_perch" : "hpole_to_perch", false);
            return;
        }
        // the fling clip on the pole, then the leap (poleFling)
        play(s_.polePara ? "hpole_para_fling" : "hpole_perp_fling", false);
        s_.flight = 0; // time on the pole since the press
        return;
    }
    if (flinging) { // tru.exe under Wine: she leaves the pole kPoleFlingDelay after the press, the clip carrying her
        s_.pos += toWorld(advanceClip(dt));
        s_.flight += dt;
        if (s_.flight >= kPoleFlingDelay) poleFling();
        return;
    }
    // tru.exe under Wine: the catch hands over to the idle hang (input taken again) 0.62-0.85 s after it (three jump
    // grabs on l4_traprooms 1336); the 1.5 s attach clip is cut there.
    if (attaching && s_.clipTime < kPoleAttachIdle && !clipDone()) { advanceClip(dt); placeOnLedge(); return; }
    if (attaching) play(s_.polePara ? "hpole_IdleHangPara" : "HPole_idleHangPerp", true);
    const vec3 perpOff = anims_.poleHandOffset, paraOff = anims_.poleParaHandOffset;
    if (turning) { // the clip's root turn from climbYawFrom; the hands stay on the axis, their offset blending pose to pose
        const anim::Clip* c = anims_.find(clip);
        advanceClip(dt);
        const double dur = c ? c->durationSeconds() : 0;
        const float u = dur > 0 ? float(std::min(1.0, s_.clipTime / dur)) : 1.0f, k = s_.mirror ? -1.0f : 1.0f;
        s_.yaw = wrapAngle(s_.climbYawFrom + (c ? k * (rootYaw(*c, s_.clipTime, false) - rootYaw(*c, 0, false)) : 0.0f));
        const vec3 to = s_.polePara ? paraOff : perpOff;
        const vec3 from = clip == "Hpole_PerptoPara" ? perpOff : clip == "Hpole_ParatoPerp" ? paraOff : to;
        s_.pos = pointOn(m, s_.ledgeSeg, s_.ledgeT) - rotYaw(s_.yaw, glm::mix(from, to, u));
        if (!clipDone()) return;
        placeOnLedge();
        if (clip == "Hpole_swingreverseA") play("HPole_swing", true);
        else play(s_.polePara ? "hpole_IdleHangPara" : "HPole_idleHangPerp", true);
        return;
    }
    // Turn into the hang (para, side): the clip turns the root; mirrored when it turns the other way (half turns: as is).
    const auto turn = [&](std::string_view c, bool para, float side) {
        const float from = s_.yaw, to = attachPose(s_.ledge, s_.ledgeSeg, s_.ledgeT, side, para).second;
        const anim::Clip* cl = anims_.find(c);
        const float by = cl ? rootYaw(*cl, cl->durationSeconds(), false) - rootYaw(*cl, 0, false) : 0.0f;
        const float want = wrapAngle(to - from);
        s_.polePara = para, s_.ledgeSide = side;
        s_.climbYawFrom = from, s_.climbYawTo = to;
        // tru.exe under Wine (l3_main_hall 948): across -> along turns her in 0.30 s, not Hpole_PerptoPara's 1.5 s
        play(c, false, c == "Hpole_PerptoPara" ? kPerpToParaRate : 1.0f, std::abs(std::abs(by) - kPi) > 0.3f && (want > 0) != (by > 0));
    };
    const vec2 dir = segDir(m, s_.ledgeSeg), n(-dir.y, dir.x), f = xy(forward());
    const float mag = glm::length(move);
    const float fwd = mag > 0.5f ? glm::dot(move, f) / mag : 0.0f;
    const float along = mag > 0.5f ? glm::dot(move, dir) / mag : 0.0f, across = mag > 0.5f ? glm::dot(move, n) / mag : 0.0f;
    if (!s_.polePara) {
        if (clip == "HPole_StartSwing" || clip == "HPole_swing") {
            const anim::Clip* c = anims_.find(clip);
            const double t0 = s_.clipTime;
            advanceClip(dt);
            const double t1 = s_.clipTime;
            if (clip == "HPole_StartSwing") {
                constexpr double kPhase1 = 0.7; // HPole_StartSwing_Phase1: 21 frames; its end pose starts Hpole_startswing_endA
                if (fwd <= 0.7f && t0 < kPhase1 && t1 >= kPhase1) play("Hpole_startswing_endA", false);
                else if (clipDone()) play(fwd > 0.7f ? "HPole_swing" : "Hpole_swingstopA", fwd > 0.7f);
            } else if (c) {
                const double D = c->durationSeconds(), p0 = std::fmod(t0, D), p1 = p0 + (t1 - t0);
                if (fwd < -0.7f && p1 >= D) turn("Hpole_swingreverseA", false, -s_.ledgeSide);
                else if (fwd <= 0.7f)
                    for (const auto& [b, stop] : {std::pair{12 / 30.0, "Hpole_swingstopB"}, {31 / 30.0, "Hpole_swingstopC"}, {D, "Hpole_swingstopA"}})
                        if (p0 < b && p1 >= b) { play(stop, false); break; }
            } else if (fwd <= 0.7f) {
                play("Hpole_swingstopA", false);
            }
            placeOnLedge();
            return;
        }
        const bool settling = clip == "Hpole_swingstopA" || clip == "Hpole_swingstopB" || clip == "Hpole_swingstopC" ||
                              clip == "Hpole_startswing_endA";
        if (settling && !clipDone()) {
        } else if (fwd > 0.7f) {
            play("HPole_StartSwing", false);
        } else if (std::abs(along) > 0.7f) {
            turn("Hpole_PerptoPara", true, along > 0 ? 1.0f : -1.0f);
            return;
        } else {
            play("HPole_idleHangPerp", true);
        }
        advanceClip(dt);
        placeOnLedge();
        return;
    }
    const bool moving = clip == "HPole_ParaForward_trans" || clip == "HPole_ParaForward";
    const bool stopping = clip == "HPole_ParaForward_to_idle_L_hand" || clip == "HPole_ParaForward_to_idle_R_hand" ||
                          clip == "Hpole_para_attach_S";
    // The stop leads with the hand that is behind: the left one in the first half of the hand-over-hand cycle [I].
    const auto stop = [&] {
        const anim::Clip* c = anims_.find(s_.clip);
        const bool firstHalf = !c || s_.clip != "HPole_ParaForward" || std::fmod(s_.clipTime, c->durationSeconds()) < c->durationSeconds() / 2;
        play(firstHalf ? "HPole_ParaForward_to_idle_L_hand" : "HPole_ParaForward_to_idle_R_hand", false);
    };
    int endSeg = s_.ledgeSeg;
    float endT = s_.ledgeT;
    const bool atEnd = moveAlong(m, endSeg, endT, glm::dot(f, dir) > 0 ? 1.0f : -1.0f); // facing the pole's end
    if (fwd > 0.7f && !atEnd) {
        if (clip == "HPole_ParaForward_trans" && clipDone()) play("HPole_ParaForward", true);
        else if (!moving) play("HPole_ParaForward_trans", false);
    } else if (moving) {
        stop();
    } else if (stopping && !clipDone()) {
    } else if (fwd < -0.7f) {
        turn("Hpole_Paraturn180", true, -s_.ledgeSide);
        return;
    } else if (std::abs(across) > 0.7f) {
        turn("Hpole_ParatoPerp", false, across > 0 ? 1.0f : -1.0f);
        return;
    } else {
        play("hpole_IdleHangPara", true);
    }
    const vec3 d = toWorld(advanceClip(dt));
    // Hand over hand along the pole by the root motion's component along it; an end of the pole stops her.
    if (moveAlong(m, s_.ledgeSeg, s_.ledgeT, glm::dot(xy(d), dir)) && (s_.clip == "HPole_ParaForward_trans" || s_.clip == "HPole_ParaForward"))
        stop();
    placeOnLedge();
}

// The leap off a pole after the fling clip (and the pole-top jumps forward / back, q33 / q34). tru.exe under Wine flies
// her onto what lies ahead, re-aimed by its solver 0x73EDC0 (v = (target - pos - g t^2 / 2) / t) [O]: l4_traprooms
// 1336 -> pole 1337 (1264 ahead, same height; presses 1.6 s and 2.8 s into the swing, identical flights): caught 242
// short of it and 357 below, 23.5 frames after leaving, 52.10 up a 30 Hz frame; l3_main_hall pole 948 -> ledge 946
// (1052 ahead, 152 under the axis, the edge of a platform): she lands on the edge 23.0 frames after leaving, (35.9
// forward, 74.1 up) a frame. Ours: the nearest pole or ledge ahead (300..poleFlingReach out, +-300 aside, +-500 height,
// crossing the heading), reached kFlingFrames after leaving [I: one time fits both samples within half a frame; the
// fling query q32's sweep does not]: a pole's hang (HangJump; the root on the hang pose, so the catch has no jump [I]),
// a ledge with a floor on top: landing on it one capsule radius in from the edge (Jump, [I]: tru.exe touches down 3
// short of the edge and steps on), a ledge without one: its hang [I]. Nothing ahead: q33 as it is (73.72 forward,
// 52.10 up a frame) [O].
void Player::poleFling(float dirSign) {
    const vec3 from = pointOn(col_.markup[size_t(s_.ledge)], s_.ledgeSeg, s_.ledgeT);
    const vec2 f = xy(forward()) * dirSign;
    int bestLi = -1, bestSeg = 0;
    float bestT = 0, bestSide = 1, bestFw = 1e30f;
    std::optional<vec3> landAt;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        const bool pole = isPole(m);
        if ((!pole && m.type != 1) || int(li) == s_.ledge) continue;
        for (int si = 0; size_t(si) + 1 < m.points.size(); ++si) {
            const vec2 dir = segDir(m, si), n(-dir.y, dir.x);
            if (std::abs(glm::dot(n, f)) < 0.7f) continue; // it must cross the heading
            const float t = std::clamp(glm::dot(xy(from - m.points[size_t(si)]), dir), 0.0f, segLen(m, si));
            const vec3 p = pointOn(m, si, t), rel = p - from;
            const float fw = glm::dot(xy(rel), f), lat = glm::dot(xy(rel), vec2(f.y, -f.x));
            if (fw < 300 || fw > t_.poleFlingReach || std::abs(lat) > 300 || std::abs(rel.z) > 500 || fw >= bestFw) continue;
            // poles: hang facing on; ledges: her side of the wall (out = n * side towards her)
            const float side = (glm::dot(n, f) > 0) == pole ? 1.0f : -1.0f;
            std::optional<vec3> land;
            if (!pole) {
                const vec3 in = p - vec3(n * (side * t_.radius), 0);
                if (const auto g = col_.groundProbe(in + vec3(0, 0, 100), 150);
                    g && g->normal.z >= t_.minFloorNormalZ && std::abs(g->point.z - p.z) < 50)
                    land = vec3(xy(in), g->point.z);
                else if (!openBelow(col_, m, si, t, side)) continue;
            }
            bestFw = fw, bestLi = int(li), bestSeg = si, bestT = t, bestSide = side, landAt = land;
        }
    }
    s_.regrabCooldown = 0.3f;
    s_.fallStartZ = s_.pos.z;
    if (bestLi < 0) {
        s_.ledge = -1;
        s_.vel = vec3(f * kFlingForward, kFlingUp);
        s_.state = State::Jump;
        return;
    }
    const float time = kFlingFrames / 30.0f;
    if (landAt) {
        s_.ledge = -1;
        s_.vel = jumpVelocity(*landAt - s_.pos, time, t_.gravity);
        s_.state = State::Jump;
        return;
    }
    s_.vel = jumpVelocity(attachPose(bestLi, bestSeg, bestT, bestSide).first - s_.pos, time, t_.gravity);
    s_.flight = time;
    s_.ledge = bestLi, s_.ledgeSeg = bestSeg, s_.ledgeT = bestT, s_.ledgeSide = bestSide, s_.polePara = false;
    s_.state = State::HangJump;
}

// On top of a pole (State::Perch with a pole as `ledge`), lara.drm's state chart [O]: from the idle hang, jump with the
// stick neither along nor across (T1231 / T1235) climbs onto it (hpole_to_perch, along it hpole_para_to_perch, which
// turns her square to it); on top (S668 hpole_perch_ready) the stick across leans into the jump (S689 / S688 loop
// hpole_perch_Jump_forward_Antic / _back_Antic) and jump leaps forward / back (q33 / q34: 73.72 out + 52.10 up a 30 Hz
// frame, aimed as the fling), jump with the stick along it leaps that way (hpole_perch_Jump_left / _right,
// q69 / q70: 45 out + 70 up after the clip's quarter turn), jump alone straight up (hpole_perch_Jump_up, q35 53.73 up);
// drop lowers her back onto the hang (hpole_perch_fall_to_hpole). The stick bands (45 degrees) are [I]; the chart's top
// run along the pole (hpole_perch_to_top_run onto balance-beam states) and its clearance check are not ported.
// Wine (l4_traprooms 1336): her root ends 19.9 over the axis (hpole_to_perch: 638.4 up from the hang, 619.4 hands).
void Player::poleTop(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    const std::string clip = s_.clip;
    if (clip == "hpole_to_perch" || clip == "hpole_para_to_perch" || clip == "hpole_perch_fall_to_hpole") {
        s_.pos += advanceRoot(dt);
        if (!clipDone()) return;
        s_.polePara = false;
        if (clip != "hpole_perch_fall_to_hpole") {
            const vec2 dir = segDir(col_.markup[size_t(s_.ledge)], s_.ledgeSeg);
            s_.ledgeSide = glm::dot(xy(forward()), vec2(-dir.y, dir.x)) >= 0 ? 1.0f : -1.0f; // the hang under her facing
            play("hpole_perch_ready", true);
            return;
        }
        s_.state = State::PoleHang;
        play("HPole_idleHangPerp", true);
        placeOnLedge();
        return;
    }
    if (dropPressed) {
        play("hpole_perch_fall_to_hpole", false);
        return;
    }
    const float mag = glm::length(move), a = mag > 0.5f ? wrapAngle(yawOf(move) - s_.yaw) : 0.0f;
    const bool fwd = mag > 0.5f && std::abs(a) < kPi / 4, back = mag > 0.5f && std::abs(a) > 3 * kPi / 4;
    if (jumpPressed) {
        s_.regrabCooldown = 0.3f;
        s_.fallStartZ = s_.pos.z;
        if (fwd || back) {
            play(fwd ? "hpole_perch_Jump_forward" : "hpole_perch_Jump_back", false);
            poleFling(fwd ? 1.0f : -1.0f);
            return;
        }
        s_.ledge = -1;
        s_.state = State::Jump;
        if (mag > 0.5f) {
            s_.vel = vec3(move / mag * (45.0f * 30.0f), 70.0f * 30.0f);
            play(a > 0 ? "hpole_perch_Jump_left" : "hpole_perch_Jump_right", false);
        } else {
            s_.vel = vec3(0, 0, 53.73f * 30.0f);
            play("hpole_perch_Jump_up", false);
        }
        return;
    }
    play(fwd ? "hpole_perch_Jump_forward_Antic" : back ? "hpole_perch_Jump_back_Antic" : "hpole_perch_ready", true);
    advanceClip(dt);
}

// Ground jumps at a ledge: the take-off state's jump query (lara.drm state chart: S248 standing q2 48.04 up, S245 /
// S247 with the stick q43 40 along + 50 up, S249 running q26 90.25 forward + 42.54 up a 30 Hz frame, frames 1..40; q2
// / q26 start 300 over the root, q43 at it; q2 only while coming down (+0x2C bit 0x80); cones 120 / 90 / 90 degrees;
// the ledge's target types start 505.69 over the root (3, the hands) and at it (1)) [O]; ledgeOnArc tests its hits with
// the queries' shared class record (`record` 0x2AB8C) and 0x73EDC0 re-aims the flight at the first, a ledge (target type
// offset 505.69) at its hang [O], timed by the hit frame [I]. Running (tru.exe under Wine, l4_temple ledge 1204, 782
// over the floor): close in (take-offs 204..614) S535's straight rush, 11 ticks onto the hang; farther (649..1222.9)
// a re-aimed arc, launch ~59 up a frame, 10.5 frames in the air for 781..1218; from 1243.4 nothing [O]. Ours: a hit by
// the record's capsule entry (2) rushes, by its box (4) arcs, rising to the hang and arriving at the arc's top (10.5
// frames for 1204), the box no farther than kRunJumpReach [I: tru.exe's chart keys S535 / S296 on cond 0x2BA and the
// ledge class; ledges in the hurdle bands (5..7) go to hurdle states there, which ours does not model]. Only ledges
// kQueryStep or more over the root count [I: lower ones are landed on, vaulted or caught as they are]. Under Wine, a
// standing jump on the far side of l3_main_hall's 817-high ledge 944 rose its plain 201.6 (the wall-side test refuses
// it) [O]. Returns false (no ledge met).
bool Player::aimGroundJump(vec2 query, float up, bool falling, float cone, bool running) {
    constexpr float kQueryStep = 158.0f, kHangTarget = 505.69f, kRush = 11.0f / 60.0f;
    const vec2 f = xy(forward());
    JumpQuery q;
    q.start = s_.pos + vec3(0, 0, up), q.hang = s_.pos + vec3(0, 0, kHangTarget), q.root = s_.pos;
    q.v = vec3(f * query.x, query.y), q.dir = f, q.cone = cone, q.falling = falling, q.minZ = s_.pos.z + kQueryStep;
    q.record = JumpRecord::Ground, q.classify = true, q.base = s_.pos; // q2 / q26 / q43: record 0x2AB8C, +0x2C bit 0x40
    const JumpTarget j = ledgeOnArc(q);
    if (j.markup < 0) return false;
    const auto& m = col_.markup[size_t(j.markup)];
    const vec3 hit = pointOn(m, j.seg, j.t), goal = attachPose(j.markup, j.seg, j.t, j.side).first;
    if (running) {
        const vec2 sd = segDir(m, j.seg), out = vec2(-sd.y, sd.x) * j.side;
        const int cls = ledgeClassOf(col_, hit, out, s_.pos, f);
        if (j.entry == 2 && (cls < 5 || cls > 7)) { // the capsule entry: S535's straight rush onto the hang
            s_.ledge = j.markup, s_.ledgeSeg = j.seg, s_.ledgeT = j.t, s_.ledgeSide = j.side, s_.polePara = false;
            s_.vel = (goal - s_.pos) / kRush;
            s_.flight = kRush;
            s_.state = State::HangJump;
            return true;
        }
        // the box entry's reach (measured on a ledge over the hurdle bands; S289 sends classes 5..7 to the hurdle states)
        if (j.entry == 4 && (cls < 5 || cls > 7) && glm::length(xy(hit - s_.pos)) > kRunJumpReach) return false;
    }
    // 0x73EDC0 re-aims the flight at the hang; a running jump's box catch rises to it, arriving at the top [I]
    const vec3 d = goal - s_.pos;
    const float time = running && j.entry == 4 && d.z > 0 ? std::sqrt(2 * d.z / t_.gravity) : j.frames / 30.0f;
    s_.vel = jumpVelocity(d, time, t_.gravity);
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
    // Sprint stamina (tru.exe under Wine, l4_trailstart; swimming the same, l2_cliffclimb sea): sprinting spends it
    // second for second, anything else but the exhaustion clips refills it as fast.
    if (s_.clip == "sprint_loop" || swimSprintClip(s_.clip)) s_.sprintUsed += dt;
    else if (s_.clip != "ng_sprint_to_exhaust" && s_.clip != "ng_run_exhaust" && !swimExhaustedClip(s_.clip))
        s_.sprintUsed = std::max(0.0f, s_.sprintUsed - dt);
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
        if (encumberedMove(move, jumpPressed, dt)) break; // carrying a weight (Melee.cpp)
        const float diff = std::abs(wrapAngle(target - s_.yaw));
        if (jumpPressed && s_.state != State::Land) {
            if (tryHurdle(true) || tryPerch(false)) break; // a low ledge ahead / a perch
            // tru.exe (Wine): the stick behind her turns her round into a running jump; to a side, a standing one.
            s_.runJump = s_.state == State::Run || (wantsMove && diff > 1.75f);
            if (s_.runJump && s_.state != State::Run) s_.yaw = target;
            s_.state = State::JumpPrep; // launch velocity is set when the take-off clip ends
            play(s_.runJump ? "ng_running_jump_forwards" : "ng_standing_jump", false);
            break;
        }
        // Drop (crouch button): at an edge drop to hang, running roll, else crouch while held.
        if (dropPressed && s_.state != State::Run && tryDropToHang()) break;
        if (dropPressed && s_.state == State::Run) {
            s_.state = State::Roll;
            play(s_.clip == "sprint_loop" ? "ng_roll_front_sprinting" : "ng_roll_front", false);
            break;
        }
        if (a.drop && s_.state != State::Run && s_.state != State::Land) {
            s_.state = State::Crouch;
            play("crouch_idleToCrouch", false);
            break;
        }
        // Wading (class 0x100): wading_idle, and wading_walk at any stick [I: the original's wading graph not decoded;
        // wading_walk_low and the wading turns / jumps unused]. Leaving it, the on-foot clips come back.
        const bool wade = s_.state != State::Land && wading();
        if (!wade && s_.clip.starts_with("wading_")) play(s_.state == State::Walk ? "ng_walk" : "ng_idle", true);
        if (wade) {
            if (wantsMove) {
                s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
                s_.state = State::Walk;
                play("wading_walk", true);
            } else {
                s_.state = State::Idle;
                play("wading_idle", true);
            }
        } else
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
            else if (s_.clip == "sprint_loop") { // sprint_loop root motion 1927 units/s (tru.exe: ~1860 on a curve)
                if (s_.sprintUsed >= t_.sprintSeconds) play("ng_sprint_to_exhaust", false);
                else if (!a.sprint) play("ng_run", true);
            } else if (s_.clip == "ng_sprint_to_exhaust") {
                if (clipDone()) play("ng_run_exhaust", false);
            } else if (s_.clip == "ng_run_exhaust") {
                if (clipDone()) play("ng_run", true);
            } else if (a.sprint && s_.sprintUsed <= t_.sprintSeconds - t_.sprintResume && (s_.clip == "ng_run" || clipDone()))
                play("sprint_loop", true);
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
        const bool grounded = s_.state == State::Idle || s_.state == State::Walk || s_.state == State::Run || s_.state == State::Stop;
        if (grounded && wantsMove && tryWallHug(move)) break;
        if (grounded && wantsMove && glm::dot(move, xy(forward())) > 0.5f * mag &&
            (tryHurdle(false) || ((s_.state == State::Walk || s_.state == State::Run) && tryHurdleDown())))
            break;
        // Pushing into a climbable rock face starts climbing (a jump keeps its normal arc).
        if (wantsMove && glm::dot(move, xy(forward())) > 0.5f && !tryLadder(false)) tryClimb(false);
        if ((s_.state == State::Idle || s_.state == State::Walk || s_.state == State::Run || s_.state == State::Stop) && tryRail(false)) break;
        break;
    }
    case State::Crouch: {
        const bool headroom = !col_.sweepCapsule(s_.pos + vec3(0, 0, t_.stepUp + t_.radius), s_.pos + vec3(0, 0, t_.crouchHeight - t_.radius),
                                                 t_.radius, vec3(0, 0, t_.capsuleTop - t_.crouchHeight));
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
        // Down the fall line of a floor steeper than minFloorNormalZ, accelerated by gravity along it without friction
        // or cap (tru.exe, Wine: l3_main_bedroom roof, normal z 0.45: along-slope speed 51 -> 69 -> 91 units per
        // 30 Hz frame, ~4.7k units/s^2 = g sin 63 deg); jump leaps off forward; a standable floor ends it, nothing
        // below falls.
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
        const float nz = std::max(g->normal.z, 1e-3f), along = std::max(0.0f, glm::dot(xy(s_.vel), dir)) / nz;
        const float speed = (along + t_.gravity * std::sqrt(1 - nz * nz) * dt) * nz; // horizontal part of the along-slope speed
        s_.vel = vec3(dir * speed, -speed * std::sqrt(1 - nz * nz) / nz); // z: the descent, carried into a fall
        vec3 p = slide(s_.pos, vec3(xy(s_.vel) * dt, 0), t_.stepUp, true);
        const auto g2 = col_.groundProbe(p + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown + speed * dt * 3);
        s_.pos = p;
        if (!g2) { startFall(s_.vel); break; }
        s_.pos.z = g2->point.z;
        break;
    }
    case State::RailSlide:
        rail(jumpPressed, dropPressed, dt);
        break;
    case State::Beam:
        beam(move, jumpPressed, dropPressed, dt);
        break;
    case State::Climb:
        climb(glm::dot(move, move) > 0 ? a.move : vec2(0), jumpPressed, dropPressed, dt); // the stick as on screen: up = up the rock
        break;
    case State::Ladder:
        ladder(move, jumpPressed, dropPressed, dt);
        break;
    case State::Pillar:
        pillar(move, a.move, jumpPressed, dropPressed, dt);
        break;
    case State::Perch:
        if (s_.ledge >= 0 && size_t(s_.ledge) < col_.markup.size() && isPole(col_.markup[size_t(s_.ledge)])) poleTop(move, jumpPressed, dropPressed, dt);
        else perch(move, jumpPressed, dropPressed, dt);
        break;
    case State::WallHug:
        wallHug(move, jumpPressed, dropPressed, dt);
        break;
    case State::GrappleThrow:
    case State::GrapplePull:
    case State::GrappleSwing:
    case State::GrappleRappel:
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
        if (s_.state != State::JumpPrep) break; // fell off during the take-off
        if (!clipDone() && (running || !wantsMove || s_.clipTime < t_.standMoveTakeoff)) break; // still crouching
        const float h = running ? t_.runJumpSpeed : wantsMove ? t_.standJumpSpeed : 0.0f;
        if (wantsMove && !running) s_.yaw = target;
        s_.vel = forward() * h + vec3(0, 0, running ? t_.runJumpVz : wantsMove ? t_.standMoveJumpVz : t_.jumpVz);
        s_.state = State::Jump;
        s_.fallStartZ = s_.pos.z;
        // q26 running, q43 with the stick, q2 standing (lara.drm query table) [O]
        const bool aimed = running    ? aimGroundJump({90.25f, 42.54f}, 300.0f, false, glm::radians(90.0f), true)
                           : wantsMove ? aimGroundJump({40.0f, 50.0f}, 0.0f, false, glm::radians(90.0f))
                                       : aimGroundJump({0.0f, 48.04f}, 300.0f, true, glm::radians(120.0f));
        if (!aimed) assistJump();
        play(running ? "NG_InAir_Feet_Forward_stage1" : "NG_InAir_Feet_Forward_stage1_standing", false);
        if (s_.state == State::HangJump) break; // the running jump's straight rush (aimGroundJump)
        if (!running && !wantsMove && standJumpToLedge()) break;
        tryPoleLeap();
        break;
    }
    case State::Jump:
    case State::Fall:
        // A free leap off a hang: the take-off clip carries her (root motion, turn), then the stored velocity launches.
        if (s_.state == State::Jump && hangTakeOff(s_.clip) && !clipDone()) {
            s_.pos += advanceRoot(dt);
            break;
        }
        // the half / quarter turns of a jump back from a ledge / ladder / pillar / pole top; air clips have no root motion
        if (s_.clip == "ledgeb_jump_back" || s_.clip == "ladder_jump_back" || s_.clip == "pillar_jump_back" ||
            s_.clip == "hpole_perch_Jump_back" || s_.clip == "hpole_perch_Jump_left" || s_.clip == "hpole_perch_Jump_right")
            advanceRoot(dt);
        else advanceClip(dt);
        if (s_.state == State::Jump && clipDone())
            play(s_.clip == "NG_InAir_Feet_Forward_stage2" ? "NG_InAir_Feet_Forward_stage3" : "NG_InAir_Feet_Forward_stage2",
                 s_.clip == "NG_InAir_Feet_Forward_stage2");
        else if (s_.state == State::Fall && clipDone()) play("NG_InAir_Feet_Forward_stage3", true); // after a let-go clip
        if (s_.state == State::Jump && s_.clip.starts_with("NG_InAir") && tryPoleLeap()) break;
        airMove(dt, move);
        if ((s_.state == State::Jump || s_.state == State::Fall) && s_.vel.z <= 0) enterWater();
        break;
    case State::HangJump: {
        if (s_.ledge < 0) { startFall(s_.vel); break; } // target lost (loaded mid-flight)
        const bool takeOff = hangTakeOff(s_.clip);
        if (takeOff && s_.vel == vec3(0)) { // the take-off clip's root motion (and turn), then the launch
            s_.pos += advanceRoot(dt);
            if (clipDone()) launchHangJump();
            break;
        }
        if (takeOff || s_.clip == "hpole_perch_Jump_back" || s_.clip == "ladder_jump_back") advanceRoot(dt);
        else advanceClip(dt);
        const float step = std::min(dt, s_.flight);
        // tryPoleLeap, the running jump's rush, the DICE jump up onto a ledge: no gravity
        const bool straight = s_.clip.starts_with("NG_InAir") || (s_.clip.starts_with("DICE_") && s_.clip.ends_with("jump_up"));
        s_.pos += s_.vel * step + vec3(0, 0, straight ? 0.0f : -0.5f * t_.gravity * step * step);
        if (!straight) s_.vel.z -= t_.gravity * step;
        s_.flight -= step;
        if (s_.flight > 0) break;
        if (col_.markup[size_t(s_.ledge)].type == 9) { catchLadder(); break; } // a ladder jump onto a ladder
        const bool pole = isPole(col_.markup[size_t(s_.ledge)]);
        const std::string_view attach = pole ? (s_.polePara ? "Hpole_para_attach_S" : "Hpole_perp_attach_M")
                                        : s_.clip.ends_with("jump_up") || s_.clip == "Wall_hug_Jump_Up_Twist" ? "ledgeb_attach_U" // also DICE_<s>_jump_up (Climb.cpp)
                                        : s_.clip == "ledgeb_jump_back" || s_.clip == "ladder_jump_back" ? "ledgeb_attach_F"
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
        if (s_.savingGrab > 0) { // one hand on the ledge: jump pulls up, else she slips off when the time runs out
            s_.savingGrab = std::max(0.0f, s_.savingGrab - dt);
            if (jumpPressed) s_.savingGrab = 0, play("ledgeb_bad_attach_recover", false);
            else if (s_.savingGrab <= 0) {
                s_.pos -= forward() * 20.0f;
                s_.regrabCooldown = 0.4f;
                startFall(vec3(0));
                break;
            }
            if (!clipDone()) advanceClip(dt);
            placeOnLedge();
            break;
        }
        if (dropPressed) {
            s_.pos -= forward() * 20.0f; // off the wall
            s_.regrabCooldown = 0.4f;
            startFall(vec3(0));
            break;
        }
        if (hangCorner(lateral, glm::dot(move, xy(forward())), jumpPressed, dt)) break;
        // Leaning into a jump while the stick points that way (lara.drm hang group: S435 up with the up-jump query q10,
        // S449 / S450 sideways at the ledge's end (LookaheadResult 642) with q7 / q8, S436 / S457 back with q9 / q110 [O-data]):
        // ledgeb_idle_to_jump_<way>_antic, the ledgeb_jump_<way>_antic loop, released ledgeb_jump_<way>_antic_to_idle
        // (left: the right ones mirrored). ledgeb_jump_back starts in the back lean's pose and ledgeb_jump_right_from_antic
        // in the side one's [O: clip poses]. tru.exe under Wine (l3_main_bedroom ledge 973, a climbable top): the stick held
        // up keeps her in S434 (no climb), right in S457 -> S449, until released (back to S444) [O]; which clip each
        // lean state's request plays is [I: by name] (the group's requests are not decoded).
        const float fw = glm::dot(move, xy(forward()));
        const auto& lm = col_.markup[size_t(s_.ledge)];
        const float along = glm::dot(right, segDir(lm, s_.ledgeSeg)) * lateral; // > 0: toward the polyline's end
        const bool atEnd = along > 0 ? size_t(s_.ledgeSeg) + 2 >= lm.points.size() && s_.ledgeT >= segLen(lm, s_.ledgeSeg) - 1.0f
                                     : s_.ledgeSeg == 0 && s_.ledgeT <= 1.0f;
        const std::string_view way = std::abs(lateral) > 0.5f ? (atEnd ? "right" : "")
                                     : fw < -0.5f           ? "back"
                                     : fw > 0.5f            ? "up" : "";
        const bool leaning = s_.clip.starts_with("ledgeb_idle_to_jump_") ||
                             (s_.clip.starts_with("ledgeb_jump_") && s_.clip.ends_with("_antic") && !s_.clip.ends_with("from_antic"));
        if (jumpPressed) {
            const int kind = std::abs(lateral) > 0.5f ? (lateral > 0 ? 1 : -1) : fw < -0.5f ? 2 : 0;
            const bool fromLean = leaning && kind != 0 && kind != 2; // the side lean's own take-off
            const auto climb = [&](std::string_view clip) {
                s_.state = State::ClimbUp;
                play(clip, false);
                // Start where the clip expects the ledge: its hands (at the start time) on the ledge in x/y, and
                // its root at the clip end on the ledge top (end - start rise); nothing to snap at the end.
                const anim::Clip* c = anims_.find(s_.clip);
                if (const auto h = c ? anims_.handsAt(*c, s_.clipTime) : std::nullopt) {
                    const float rise = rootTranslation(*c, c->durationSeconds(), false).z - rootTranslation(*c, s_.clipTime, false).z;
                    const vec3 hang = anims_.hangHandOffset;
                    s_.pos += toWorld({hang.x - h->x, hang.y - h->y, hang.z - rise});
                }
            };
            const int cls = kind == 0 ? hangClearance() : 0;
            if (cls == 2) climb("ledgeb_climbup_fast");
            else if (cls == 5) climb("ledgeb_jumpuptocrouch"); // S439 T615 -> S455 -> crouch_Idle [O-data; clip by name I]
            else if (cls == 8) startHangToWallHug();          // S439 T697 -> S1251 bbeam_flip_foward -> Wall_Hug_Idle
            else if (kind == 0 && tryHangToBeam()) {
            } else if (startHangJump(kind)) {
                if (fromLean) play("ledgeb_jump_right_from_antic", false, 1, kind == -1);
            } else if (kind != 0) { // nothing there: a free leap, launched when the take-off clip ends (State::Jump)
                const vec2 dir = kind == 2 ? -xy(forward()) : glm::vec2(right * float(kind));
                s_.vel = vec3(dir * (kind == 2 ? t_.hangLeapBack : t_.hangLeapSide), t_.hangLeapVz);
                s_.ledge = -1;
                s_.regrabCooldown = 0.55f; // take-off clip (0.23 s) + 0.3
                s_.state = State::Jump;
                s_.fallStartZ = s_.pos.z;
                play(kind == 2 ? "ledgeb_jump_back" : fromLean ? "ledgeb_jump_right_from_antic" : "ledgeb_jump_right", false, 1, kind == -1);
            } // else: no top to climb onto, no beam and nothing to jump to: she stays hanging (the chart's S439 has no
              // way on for class 1) [I: what tru.exe shows]
            break;
        }
        if (s_.state == State::Hang && (s_.clip.starts_with("ledgeb_attach") || s_.clip.starts_with("ledgeb_bad_attach") ||
                                        s_.clip.ends_with("_antic_to_idle")) && !clipDone()) {
            advanceClip(dt);
            break;
        }
        if (!way.empty()) {
            const std::string in = "ledgeb_idle_to_jump_" + std::string(way) + "_antic", loop = "ledgeb_jump_" + std::string(way) + "_antic";
            const bool mirror = way == "right" && lateral < 0;
            s_.state = State::Hang;
            if (s_.clip == in && clipDone()) play(loop, true, 1, mirror);
            else if (s_.clip != in && s_.clip != loop) play(in, false, 1, mirror);
            advanceClip(dt);
            placeOnLedge();
            break;
        }
        if (leaning) { // released: back to the idle hang
            const std::string w = s_.clip.substr(s_.clip.starts_with("ledgeb_idle_to_jump_") ? 20 : 12);
            play("ledgeb_jump_" + w.substr(0, w.find('_')) + "_antic_to_idle", false, 1, s_.mirror);
            advanceClip(dt);
            placeOnLedge();
            break;
        }
        if (std::abs(lateral) > 0.3f) {
            s_.state = State::Shimmy;
            play("ledgeb_right", true, lateral > 0 ? 1.0f : -1.0f);
        } else {
            s_.state = State::Hang;
            play("ledgeb_idle", true);
        }
        const vec3 d = toWorld(advanceClip(dt));
        if (s_.state == State::Shimmy) {
            // Move along the polyline by the root motion's component along the ledge; its ends stop the shimmy, a corner
            // joint takes her round it (startHangCorner).
            const float ds = glm::dot(xy(d), segDir(lm, s_.ledgeSeg));
            if (((ds > 0 && s_.ledgeT + ds > segLen(lm, s_.ledgeSeg)) || (ds < 0 && s_.ledgeT + ds < 0)) &&
                startHangCorner(s_.ledgeSeg, ds, lateral))
                break;
            moveAlong(lm, s_.ledgeSeg, s_.ledgeT, ds);
        }
        placeOnLedge();
        break;
    }
    case State::PoleHang:
        pole(move, jumpPressed, dropPressed, dt);
        break;
    case State::ClimbUp: {
        // Full root motion, no collision: the clip lifts her over the edge (ledgeb_climbup_fast placed by climb()).
        if (s_.clip.starts_with("hurdle_")) {
            if (!hurdleTick(move, dt)) break;
        } else {
            s_.pos += toWorld(advanceClip(dt));
            if (!clipDone()) break;
        }
        const bool hurdle = s_.clip.starts_with("hurdle_");
        s_.ledge = -1;
        if (auto g = col_.groundProbe(s_.pos + vec3(0, 0, t_.stepUp + 100.0f), t_.stepUp + 100.0f + t_.stepDown + 100.0f)) {
            s_.pos.z = g->point.z;
            // tru.exe (Wine): running on, she runs off the hurdle without stopping.
            if (hurdle && wantsMove && !wantsWalk) { s_.state = State::Run; play("ng_run", true); }
            else if (s_.clip == "ledgeb_jumpuptocrouch") { s_.state = State::Crouch; play("crouch_Idle", true); }
            else { s_.state = State::Idle; play("ng_idle", true); }
        } else {
            startFall(vec3(0));
        }
        break;
    }
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
    case State::Vehicle: // World drives her from the bike (Vehicle.cpp) and does not tick the player
        break;
    case State::Melee: // close combat / carried-object clips (Melee.cpp)
    case State::Knocked:
        actTick(dt);
        break;
    case State::Use: // in place, input locked (the interaction clips have no root motion)
        advanceClip(dt);
        if (clipDone()) { s_.state = State::Idle; play("ng_idle", true); }
        break;
    case State::Block: // World moves her with the block (Script.cpp carried) and drives the clips through grip()
        advanceClip(dt);
        break;
    }
}

// Balance beam: the stick along the walking direction walks (bbeam_walk root motion projected on the line),
// against it turns round (bbeam_walk_turn180); joints of the polyline are followed. At an end: another beam
// starting there takes over, a floor beyond is stepped onto, else she stops. Jump = standing jump (assisted
// back onto the beam), drop = falls off to her right [I]. The stick held across the beam tips her into the
// about-to-fall state (beamAboutFall), which can end in a fall off it (startBeamFall).
void Player::beam(vec2 move, bool jumpPressed, bool dropPressed, float dt) {
    if (beamFalling(dt)) return;
    const auto& m = col_.markup[size_t(s_.ledge)];
    if (s_.clip == "bbeam_ledgeu_climbup" && !clipDone()) { // up from the hang (tryHangToBeam): no root motion, no input
        advanceClip(dt);
        return;
    }
    if (s_.balance.aboutFall) {
        beamAboutFall(move, jumpPressed, dt);
        return;
    }
    if (jumpPressed) {
        s_.balance = {};
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
        s_.balance.build = 0;
        if (!clipDone()) advanceRoot(dt);
        if (!clipDone()) return;
        s_.ledgeSide = -s_.ledgeSide;
        play("bbeam_idle", true);
    }
    const vec2 dir = segDir(m, s_.ledgeSeg) * s_.ledgeSide;
    const float along = glm::length(move) > 0.1f ? glm::dot(glm::normalize(move), dir) : 0.0f;
    if (along < -0.5f) {
        s_.balance.build = 0;
        play("bbeam_walk_turn180", false);
        return;
    }
    // Off balance with the stick across the beam [I: the weight and the threshold]: walking blends in
    // bbeam_unbalance_walk_left / _right (lara.drm anim request 192: the walk's blend space with them [O]); standing she
    // wobbles (bbeam_idle_wobble_fwd, requests 158 / 159 [O]). Held across like that (beyond 30 degrees off the line) for
    // kBeamTipFrames she tips into the about-to-fall state [I: tru.exe's walk enters it (chart S934 -> S897) on
    // conditions of kind 1000 that are not decoded; its walk balance tuning (objdata +0xE98: a fall angle of 180
    // degrees) never sets the fall flag].
    const float side = glm::length(move) > 0.1f ? glm::dot(glm::normalize(move), vec2(dir.y, -dir.x)) : 0.0f;
    const bool landing = (s_.clip == "bbeam_walk_attach" || s_.clip == "bbeam_jumpland") && !clipDone();
    s_.balance.build = !landing && std::abs(side) > 0.5f ? s_.balance.build + dt * 30.0f : 0.0f;
    if (s_.balance.build >= kBeamTipFrames) {
        auto& b = s_.balance;
        b = {};
        b.aboutFall = true;
        b.grace = kAboutFall.grace;
        b.period = (2.0f - kAboutFall.smooth) * kAboutFall.period; // 0x742330: +0x58 = (2 - rec[7]) * rec[8]
        b.stickYaw = yawOf(move);
        play("bbeam_aboutfall_center", true);
        return;
    }
    if (along > 0.3f) {
        play("bbeam_walk", true);
        s_.clipMix = std::abs(side) > 0.3f ? (side > 0 ? "bbeam_unbalance_walk_right" : "bbeam_unbalance_walk_left") : "";
        s_.mix = s_.clipMix.empty() ? 0.0f : 1.0f;
    } else if (!landing) {
        play(std::abs(side) > 0.5f ? "bbeam_idle_wobble_fwd" : "bbeam_idle", true);
    }
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

// About to fall (chart S895 / S896, bbeam_aboutfall_center blended with _left / _right: request 177 [O]): tru.exe's
// balance controller update 0x742770 on the about-to-fall tuning (kAboutFall), in 30 Hz frames [O]. Its error is the
// stick's heading against hers (the record has no sway); with the stick released the last stick heading holds
// (+0x5C). Within the tolerance she is balanced; beyond it the weight (error - tolerance) / (fall angle - tolerance)
// leans her to that side (stick to her left: left), the unbalanced time runs and, with the side lock (flag 2), the stick
// swung over to the other side counts as balanced. Balanced for more than kAboutFall.recover frames after leaning she
// has recovered [O], and goes back to the beam idle (ours: the chart leaves on the fall flag clear, which it is from
// entry [I]). The fall flag: a full lean (|lean| >= 0.99, held kAboutFall.full frames while the stick is pushed) and
// then the error past the fall angle, not improving for more than kAboutFall.notImprove frames, or leaning longer than
// kAboutFall.unbal frames [O]; she falls to her side (startBeamFall). A quick stick wiggle (flips less than
// kAboutFall.period frames apart on average) narrows the tolerance and shortens the limits [O]. Jump = the beam's jump
// (T1540 / T1541 [O-data]).
void Player::beamAboutFall(vec2 move, bool jumpPressed, float dt) {
    auto& b = s_.balance;
    const BalanceTuning& k = kAboutFall;
    if (jumpPressed) {
        s_.balance = {};
        s_.ledge = -1;
        s_.runJump = false;
        s_.state = State::JumpPrep;
        play("ng_standing_jump", false);
        return;
    }
    const float f = dt * 30.0f, mag = glm::length(move);
    b.clock += f;
    const bool grace = b.grace > 0;
    if (grace) b.grace -= f;
    else b.notImprove += f;
    if (mag > 0.1f) b.stickYaw = yawOf(move);
    const float err = wrapAngle(b.stickYaw - s_.yaw), off = std::abs(err);
    float lean = 0;
    int8_t side = 0;
    bool recovered = false;
    if (!grace) {
        if (off < std::abs(b.ref) || b.notImprove < b.unbal) b.notImprove = 0;
        const float d = wrapAngle(err - b.ref);
        bool flip = false;
        if (d < -0.1f) flip = b.osc == 1, b.osc = 2, b.ref = err;
        else if (d > 0.1f) flip = b.osc == 2, b.osc = 1, b.ref = err;
        const float since = b.clock - b.lastFlip;
        if (flip) b.period = b.period * k.smooth + (1 - k.smooth) * since, b.lastFlip = b.clock;
        float tol = k.tol, fall = k.fall, unbalLimit = k.unbal, fullLimit = k.full;
        if (b.period < k.period && since < k.period) {
            const float pen = (k.period - b.period) / k.period * k.penalty;
            unbalLimit *= 1 - pen, fullLimit *= 1 - pen;
            tol = std::clamp(tol - pen, k.tol * 0.25f, k.tol);
            fall = std::clamp(fall - pen, tol, k.fall);
        }
        if (off <= tol) {
            b.bal += f;
        } else {
            lean = std::clamp((off - tol) / (fall > tol ? fall - tol : kPi), 0.0f, 1.0f);
            if (err > tol) side = 1, lean = -lean;
            else if (err < -tol) side = 2;
            if (k.sideLock && fall > off && b.side != 0 && side != b.side) lean = 0, b.bal += f, side = b.side;
            else b.bal = 0;
            b.unbal += f;
            const bool partial = std::abs(b.blend) < 0.99f;
            bool held = true;
            if (!partial) {
                b.full += f;
                if (mag > 0.1f && b.full < fullLimit) held = false;
            }
            if (fall < off || k.notImprove < b.notImprove || unbalLimit < b.unbal) b.fall = !partial && held;
        }
    } else {
        b.bal += f;
    }
    if (b.bal > 0) {
        if (b.bal > k.recover && b.unbal > 0) b.unbal = 0, b.side = 0, b.full = 0, b.fall = false, recovered = true;
        b.notImprove = 0;
    }
    if (side != 0) b.side = side;
    b.lean = lean;
    const float sign = lean < 0 ? -1.0f : lean > 0 ? 1.0f : 0.0f;
    const float target = std::clamp(2 * lean + 2 * sign * b.unbal / k.leanTime, -1.0f, 1.0f);
    b.blend += std::clamp(target - b.blend, -k.leanRate * f, k.leanRate * f);
    if (b.fall) {
        startBeamFall(b.side == 1 || (b.side == 0 && b.blend < 0));
        return;
    }
    if (recovered) {
        s_.balance = {};
        play("bbeam_idle", true);
        return;
    }
    play("bbeam_aboutfall_center", true);
    s_.clipMix = b.blend < 0 ? "bbeam_aboutfall_left" : "bbeam_aboutfall_right";
    s_.mix = anims_.find(s_.clipMix) ? std::abs(b.blend) : 0.0f;
    advanceClip(dt);
}

// Falling off a beam to her left / right: query 64 from her root moved kBeamFallOffset to that side (falling, cone 90
// degrees about the side, the ledges' capsule round its arc [O]) looks for a ledge (markup 1) to catch: one whose hang
// faces that side with room under it, met within kBeamCatchFrames and kBeamCatchReach of her [I: q64's class record
// 0x31C40 is not decoded; ours: the hang on her side instead of the wall-side test, which a fall from the top refuses].
// Caught: bbeam_fall_<side> (0.33 s; ends in the ledge hang pose facing the beam, the hands 146.6 in front of the root,
// 25.6 to her side of the line and 12.9 over it [O: clip]) carries her onto the hang, then the hang idle; none:
// bbeam_fall_<side>_no_ledge (172 aside, 458 down, facing on) then the fall.
void Player::startBeamFall(bool left) {
    const vec2 fw = xy(forward()), out = left ? vec2(-fw.y, fw.x) : vec2(fw.y, -fw.x);
    const vec3 start = s_.pos + vec3(out * kBeamFallOffset, 0), v(out * kBeamFallQueryV, kBeamFallQueryV);
    int bestLi = -1, bestSeg = 0;
    float bestT = 0, bestSide = 1, bestFrame = 1e9f;
    for (size_t li = 0; li < col_.markup.size(); ++li) {
        const auto& m = col_.markup[li];
        if (m.type != 1) continue;
        for (size_t si = 0; si + 1 < m.points.size(); ++si) {
            const vec3 p0 = m.points[si], p1 = m.points[si + 1];
            const float len = glm::length(p1 - p0);
            if (len < 1e-3f) continue;
            const vec2 sd = segDir(m, int(si)), n(-sd.y, sd.x);
            const float side = glm::dot(n, out) >= 0 ? 1.0f : -1.0f;
            for (float t = 1.001f; t <= kBeamCatchFrames && t < bestFrame; t += 0.25f) {
                const vec3 a = start + v * t - vec3(0, 0, 0.5f * kQueryG * t * t);
                const vec3 c = a - vec3(out * kCapsuleBack, kCapsuleDown);
                const float along = std::clamp(glm::dot(c - p0, p1 - p0) / len, 0.0f, len);
                const vec3 h = p0 + (p1 - p0) * (along / len);
                constexpr float hh = 0.5f * kCapsuleHeight;
                const float d = h.z > c.z + hh ? glm::distance(h, c + vec3(0, 0, hh))
                                : h.z < c.z - hh ? glm::distance(h, c - vec3(0, 0, hh))
                                                 : glm::length(xy(h - c));
                if (d > kCapsuleRadius) continue;
                const vec2 to = xy(h - s_.pos);
                if (glm::dot(to, out) < -10.0f || glm::length(to) > kBeamCatchReach || h.z > s_.pos.z + 50.0f) break;
                if (!openBelow(col_, m, int(si), along, side)) break;
                bestLi = int(li), bestSeg = int(si), bestT = along, bestSide = side, bestFrame = t;
                break;
            }
        }
    }
    s_.balance = {};
    s_.vel = vec3(0);
    s_.fallStartZ = s_.pos.z;
    s_.climbFrom = s_.pos, s_.climbYawFrom = s_.yaw;
    if (bestLi >= 0) {
        s_.ledge = bestLi, s_.ledgeSeg = bestSeg, s_.ledgeT = bestT, s_.ledgeSide = bestSide, s_.polePara = false;
        const auto [hang, hangYaw] = attachPose(bestLi, bestSeg, bestT, bestSide);
        s_.climbTo = hang, s_.climbYawTo = s_.yaw + wrapAngle(hangYaw - s_.yaw);
        play(left ? "bbeam_fall_left" : "bbeam_fall_right", false);
    } else {
        s_.ledge = -1;
        s_.climbTo = s_.pos, s_.climbYawTo = s_.yaw;
        play(left ? "bbeam_fall_left_no_ledge" : "bbeam_fall_right_no_ledge", false);
        if (const anim::Clip* c = anims_.find(s_.clip)) // the request starts past frame 0: no jump at the start
            s_.climbFrom -= rotYaw(s_.yaw, rootTranslation(*c, s_.clipTime, false) - rootTranslation(*c, 0, false));
    }
}

bool Player::beamFalling(float dt) {
    if (s_.clip == "bbeam_fall_left" || s_.clip == "bbeam_fall_right") {
        if (warpStep(dt)) {
            s_.state = State::Hang;
            s_.savingGrab = 0;
            play("ledgeb_idle", true);
            placeOnLedge();
        }
        return true;
    }
    if (s_.clip != "bbeam_fall_left_no_ledge" && s_.clip != "bbeam_fall_right_no_ledge") return false;
    // its root motion off the beam (no collision but the floor under her), then the fall with the clip's last speed
    const anim::Clip* c = anims_.find(s_.clip);
    advanceClip(dt);
    const auto root = [&](double t) { return c ? rootTranslation(*c, t, false) - rootTranslation(*c, 0, false) : vec3(0); };
    const vec3 prev = s_.pos;
    s_.pos = s_.climbFrom + rotYaw(s_.climbYawFrom, root(s_.clipTime));
    if (prev.z > s_.pos.z) {
        if (const auto g = col_.groundProbe(vec3(xy(s_.pos), prev.z + 1), prev.z - s_.pos.z + 1)) {
            s_.pos.z = g->point.z;
            s_.vel = vec3(0);
            land(vec2(0));
            return true;
        }
    }
    if (!clipDone()) return true;
    const double end = c ? c->durationSeconds() : 0;
    const vec3 last = c ? (root(end) - root(std::max(0.0, end - 1.0 / 30))) * 30.0f : vec3(0);
    s_.regrabCooldown = 0.2f; // ours [I]: not back onto the beam she fell off
    startFall(rotYaw(s_.climbYawFrom, last));
    return true;
}

// ---- water ----------------------------------------------------------------------------------
// tru.exe 0x710A20: the player's probe box against every water volume box; of the overlapping volumes the
// one with the highest surface wins (the exe first compares a priority short we do not have).
int Player::waterAt(vec3 feet) const {
    const vec3 c = feet + vec3(0, 0, t_.waterProbeCenterZ), h = t_.waterProbeHalf;
    int best = -1;
    for (size_t i = 0; i < col_.markup.size(); ++i) {
        const auto& m = col_.markup[i];
        if (m.type != 10 || !insideWater(m, c, h)) continue;
        if (best < 0 || m.points[1].z > col_.markup[size_t(best)].points[1].z) best = int(i);
    }
    return best;
}

// The swimming root's level at the surface: the wave surface less swimSurfaceDepth.
float Player::surfaceZ() const {
    return waterSurface(col_.markup[size_t(s_.water)], vec2(s_.pos), waveSeconds_) - t_.swimSurfaceDepth;
}

bool Player::enterWater() {
    const int w = waterAt(s_.pos);
    if (w < 0) return false;
    const float surf = waterSurface(col_.markup[size_t(w)], vec2(s_.pos), waveSeconds_);
    if (s_.pos.z > surf) return false;
    // Shallow (a floor within swimDepth under the surface): walk. Feet already deeper than that are swimming (placed under
    // l7_arctic_sea's ice by 275_hel_explosion, where the probe from the surface would hit the ice).
    if (s_.pos.z > surf - t_.swimDepth)
        if (auto floor = col_.groundProbe(vec3(s_.pos.x, s_.pos.y, surf), t_.swimDepth); floor) return false;
    const bool airborne = s_.state == State::Jump || s_.state == State::Fall;
    s_.water = w;
    s_.pitch = 0;
    s_.ledge = -1;
    if (airborne) { // keep the plunge velocity; water drag stops it
        s_.state = State::SwimSplash;
        play("swim_jump_land", false);
    } else if (s_.pos.z <= surf - t_.swimDepth) { // placed deep (275_hel_explosion's end): stays under, swimming
        s_.vel = vec3(0);
        s_.state = State::SwimUnder;
        play("swim_submerged_idle", true);
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
    const float surf = surfaceZ() + t_.swimSurfaceDepth, reach = 300.0f;
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
        if (wading() && anims_.find("swim_to_wade")) s_.state = State::Land, play("swim_to_wade", false); // [I] its use
    };
    const float surf = surfaceZ();
    // Stroke of a swim that moves: at the surface swim_surface_fast_forward, under water swim_sub; with Shift held the
    // sprint stroke at swimSprintFactor (tru.exe under Wine, l2_cliffclimb sea: 53.94 vs 44.95 units per frame at the
    // surface and under water; request 896 / 934 clips, the surface pose fits swim_surface_sprint_forward). The surface
    // strokes are lara.drm's 3-way surface blends, request 881 (normal) and 896 (sprint): tru.exe's surface-swim pose
    // fits swim_surface_fast_forward at body rms 10.7 vs swim_sub 30.0 (docs/GAMEPLAY.md). The sprint stamina is the
    // run sprint's: after sprintSeconds the exhaustion clip plays once at normal speed, then she sprints again once
    // sprintResume is refilled (Wine: 9.95 s sprint, then 2.13 s normal / 0.95 s sprint while Shift stays held).
    // Returns the speed factor.
    auto stroke = [&](bool surface) {
        const std::string_view normal = surface ? "swim_surface_fast_forward" : "swim_sub",
                               fast = surface ? "swim_surface_sprint_forward" : "swim_submerged_sprint";
        if (swimSprintClip(s_.clip)) {
            if (s_.sprintUsed >= t_.sprintSeconds) play(surface ? "swim_surface_exhausted" : "swim_submerged_exhausted", false);
            else play(a.sprint ? fast : normal, true);
        } else if (swimExhaustedClip(s_.clip)) {
            if (clipDone()) play(normal, true);
        } else {
            play(a.sprint && s_.sprintUsed <= t_.sprintSeconds - t_.sprintResume ? fast : normal, true);
        }
        return swimSprintClip(s_.clip) ? t_.swimSprintFactor : 1.0f;
    };
    // Swim velocity: towards `dir * swimSpeed * wish` at swimAccel, slowing at swimDecel (tru.exe ramps).
    auto swimTowards = [&](vec3 dir, float wish) {
        const float speed = glm::length(s_.vel), want = t_.swimSpeed * wish;
        const float next = want > speed ? std::min(want, speed + t_.swimAccel * dt) : std::max(want, speed - t_.swimDecel * dt);
        if (wish <= 0 && speed > 1e-4f) dir = s_.vel / speed; // coasting keeps its heading
        s_.vel = dir * next;
        // plus the volume's current (0x755A00 adds 0x6ACB60 to the swim velocity it steers to [I: added straight on])
        s_.pos = swimSlide(s_.pos, (s_.vel + waterFlow(col_.markup[size_t(s_.water)], s_.pos)) * dt);
    };
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
        s_.pos = swimSlide(s_.pos, vec3(0, 0, t_.swimSpeed * dt));
        if (s_.pos.z >= surf) { s_.state = State::SwimTread; play("swim_surfaceidle", true); }
        else if (dropPressed) { s_.state = State::SwimUnder; }
        break;
    case State::SwimTread:
    case State::SwimSurface: {
        s_.pos.z = surf;
        s_.pitch += std::clamp(-s_.pitch, -t_.turnRate * dt, t_.turnRate * dt); // level out after surfacing
        if (jumpPressed && tryClimbOut()) break;
        if (dropPressed) { s_.state = State::SwimDive; s_.pitch = 0; play("swim_surfacedive", false); break; }
        float speed = 1;
        const float horiz = glm::length(xy(s_.vel));
        if (wantsMove) {
            s_.yaw = approachAngle(s_.yaw, target, std::min(t_.turnRate, kSwimTurnAccel / std::max(horiz, 1.0f)) * dt);
            // tru.exe has one SwimState for surface and under water; the surface strokes are its surface blends (stroke).
            s_.state = State::SwimSurface;
            speed = stroke(true);
        } else if (s_.state == State::SwimSurface) {
            s_.state = State::SwimTread;
            play("swim_surfaceidle", true);
        }
        advanceClip(dt);
        // The velocity steers toward her heading x the wished speed as a vector, at swimAccel (swimDecel when slowing).
        const vec3 want = forward() * (t_.swimSpeed * (s_.state == State::SwimSurface ? mag * speed : 0.0f));
        const vec3 gap = want - s_.vel;
        const float step = (glm::length(want) >= horiz ? t_.swimAccel : t_.swimDecel) * dt, len = glm::length(gap);
        s_.vel = len <= step ? want : s_.vel + gap * (step / len);
        s_.pos = swimSlide(s_.pos, (s_.vel + waterFlow(col_.markup[size_t(s_.water)], s_.pos)) * dt);
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
            const float k = stroke(false);
            swimTowards(forward() * std::cos(s_.pitch) + vec3(0, 0, std::sin(s_.pitch)), std::min(1.0f, std::hypot(h, v)) * k);
        } else {
            s_.pitch += std::clamp(-s_.pitch, -t_.turnRate * dt, t_.turnRate * dt);
            swimTowards(forward(), 0.0f);
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
