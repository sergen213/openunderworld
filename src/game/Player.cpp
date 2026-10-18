// Lara's controller: state machine, root-motion locomotion, capsule collide-and-slide, ledge hang.
// Numbers and their provenance: docs/GAMEPLAY.md.
#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kSkin = 1.0f; // gap kept to surfaces after a sweep, units

float wrapAngle(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
float approachAngle(float from, float to, float maxStep) {
    const float d = wrapAngle(to - from);
    return wrapAngle(from + std::clamp(d, -maxStep, maxStep));
}
vec2 xy(vec3 v) { return {v.x, v.y}; }

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

} // namespace

std::string_view stateName(State s) {
    static constexpr std::string_view k[] = {"idle", "walk", "run", "stop", "turn", "jumpprep", "jump",
                                             "fall", "land", "hang", "shimmy", "climbup", "swimsplash",
                                             "swimtread", "swimsurface", "swimdive", "swimunder", "surfacing",
                                             "swimclimbout", "dead"};
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

glm::mat4 Player::modelMatrix(vec3 pos, float yaw) const {
    // Model -Y is forward: rotate by yaw + 90 degrees about Z.
    return glm::rotate(glm::translate(glm::mat4(1), pos), yaw + kPi / 2, vec3(0, 0, 1));
}

vec3 Player::forward() const { return {std::cos(s_.yaw), std::sin(s_.yaw), 0}; }

vec3 Player::toWorld(vec3 m) const {
    const float c = std::cos(s_.yaw + kPi / 2), s = std::sin(s_.yaw + kPi / 2);
    return {c * m.x - s * m.y, s * m.x + c * m.y, m.z};
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

// Collide-and-slide of a capsule [pos + bottom + r, pos + height - r]. `horizontalNormals` flattens
// contact normals (grounded movement: walls only push sideways; the floor is handled by the probe).
vec3 Player::slide(vec3 pos, vec3 delta, float bottom, bool horizontalNormals) const {
    for (int iter = 0; iter < 4 && glm::dot(delta, delta) > 1e-8f; ++iter) {
        const vec3 a = pos + vec3(0, 0, bottom + t_.radius), b = pos + vec3(0, 0, t_.height - t_.radius);
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

void Player::groundMove(vec3 delta, float dt) {
    delta.z = 0;
    vec3 p = slide(s_.pos, delta, t_.stepUp, true);
    const auto g = col_.groundProbe(p + vec3(0, 0, t_.stepUp), t_.stepUp + t_.stepDown);
    if (g && g->normal.z >= t_.minFloorNormalZ) {
        p.z = g->point.z;
        s_.pos = p;
        return;
    }
    s_.pos = p; // walked off an edge (or onto a slope too steep to stand on)
    s_.vel = vec3(xy(delta) / dt, 0);
    s_.state = State::Fall;
    s_.fallStartZ = s_.pos.z;
    play("NG_InAir_Feet_Forward_stage3", true);
}

void Player::land(vec2 move) {
    const float speed = glm::length(xy(s_.vel));
    s_.vel = vec3(0);
    s_.fallHeight = s_.fallStartZ - s_.pos.z;
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
        const auto hit = col_.sweepCapsule(p + vec3(0, 0, t_.radius), p + vec3(0, 0, t_.height - t_.radius), t_.radius, d);
        if (!hit || (hit->t == 0 && glm::dot(d, hit->normal) >= 0)) { p += d; break; }
        const vec3 n = hit->normal;
        p += d * hit->t + n * kSkin;
        if (n.z >= t_.minFloorNormalZ && s_.vel.z <= 0) { // landed on a floor
            if (auto g = col_.groundProbe(p + vec3(0, 0, t_.radius), t_.radius + kSkin + 2)) p.z = g->point.z;
            s_.pos = p;
            land(move);
            return;
        }
        const vec3 rest = d * (1 - hit->t);
        d = rest - n * std::min(0.0f, glm::dot(rest, n));
        s_.vel -= n * std::min(0.0f, glm::dot(s_.vel, n));
    }
    s_.pos = p;
    if (s_.vel.z <= 0) {
        if (auto g = col_.groundProbe(p + vec3(0, 0, t_.radius), t_.radius + kSkin + 2);
            g && g->normal.z >= t_.minFloorNormalZ) {
            s_.pos.z = g->point.z;
            land(move);
            return;
        }
        if (tryGrab(prevPos)) return;
    }
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

void Player::placeOnLedge() {
    const auto& m = col_.markup[size_t(s_.ledge)];
    const vec3 p0 = m.points[size_t(s_.ledgeSeg)], p1 = m.points[size_t(s_.ledgeSeg) + 1];
    const float len = glm::length(xy(p1 - p0));
    const vec2 dir = xy(p1 - p0) / len;
    const vec2 out = vec2(-dir.y, dir.x) * s_.ledgeSide;
    s_.yaw = std::atan2(-out.y, -out.x);
    const vec3 q = glm::mix(p0, p1, s_.ledgeT / len);
    s_.pos = q - toWorld(anims_.hangHandOffset);
}

void Player::tick(vec2 move, const Actions& a, float dt) {
    const bool jumpPressed = a.jump && !jumpHeld_, dropPressed = a.drop && !dropHeld_;
    jumpHeld_ = a.jump;
    dropHeld_ = a.drop;
    s_.regrabCooldown = std::max(0.0f, s_.regrabCooldown - dt);
    s_.fallHeight = 0;
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
        break;
    }
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
        play(running ? "NG_InAir_Feet_Forward_stage1" : "NG_InAir_Feet_Forward_stage1_standing", false);
        break;
    }
    case State::Jump:
    case State::Fall:
        advanceClip(dt);
        if (s_.state == State::Jump && clipDone())
            play(s_.clip == "NG_InAir_Feet_Forward_stage2" ? "NG_InAir_Feet_Forward_stage3" : "NG_InAir_Feet_Forward_stage2",
                 s_.clip == "NG_InAir_Feet_Forward_stage2");
        airMove(dt, move);
        if ((s_.state == State::Jump || s_.state == State::Fall) && s_.vel.z <= 0) enterWater();
        break;
    case State::Hang:
    case State::Shimmy: {
        const vec2 right(std::sin(s_.yaw), -std::cos(s_.yaw));
        const float lateral = glm::dot(move, right);
        if (dropPressed) {
            s_.state = State::Fall;
            s_.fallStartZ = s_.pos.z;
            s_.pos -= forward() * 20.0f; // off the wall
            s_.ledge = -1;
            s_.regrabCooldown = 0.4f;
            play("NG_InAir_Feet_Forward_stage3", true);
            break;
        }
        if (jumpPressed) {
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
            break;
        }
        if (s_.state == State::Hang && s_.clip == "ledgeb_attach_M" && !clipDone()) { advanceClip(dt); break; }
        if (std::abs(lateral) > 0.3f) {
            s_.state = State::Shimmy;
            play("ledgeb_right", true, lateral > 0 ? 1.0f : -1.0f);
        } else {
            s_.state = State::Hang;
            play("ledgeb_idle", true);
        }
        const vec3 d = toWorld(advanceClip(dt));
        if (s_.state == State::Shimmy) {
            // Move along the polyline by the root motion's component along the ledge.
            const auto& m = col_.markup[size_t(s_.ledge)];
            float along = s_.ledgeT;
            vec3 p0 = m.points[size_t(s_.ledgeSeg)], p1 = m.points[size_t(s_.ledgeSeg) + 1];
            float len = glm::length(xy(p1 - p0));
            along += glm::dot(xy(d), xy(p1 - p0) / len);
            if (along > len && size_t(s_.ledgeSeg) + 2 < m.points.size()) {
                along -= len;
                ++s_.ledgeSeg;
            } else if (along < 0 && s_.ledgeSeg > 0) {
                --s_.ledgeSeg;
                p0 = m.points[size_t(s_.ledgeSeg)], p1 = m.points[size_t(s_.ledgeSeg) + 1];
                along += glm::length(xy(p1 - p0));
            }
            p0 = m.points[size_t(s_.ledgeSeg)], p1 = m.points[size_t(s_.ledgeSeg) + 1];
            len = glm::length(xy(p1 - p0));
            s_.ledgeT = std::clamp(along, 0.0f, len); // ends of the polyline stop the shimmy
        }
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
                s_.state = State::Fall;
                s_.fallStartZ = s_.pos.z;
                s_.vel = vec3(0);
                play("NG_InAir_Feet_Forward_stage3", true);
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
    case State::Dead: // the death clip plays out where she fell; World respawns her (Combat.cpp)
        advanceClip(dt);
        break;
    }
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
        if (jumpPressed && tryClimbOut()) break;
        if (dropPressed) { s_.state = State::SwimDive; play("swim_surfacedive", false); break; }
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
        // pitch turns towards the wish at turnRate. [I] the original's under-water control scheme.
        const float h = wantsMove ? mag : 0.0f, v = float(a.jump) - float(a.drop);
        if (h > 0 || v != 0) {
            if (wantsMove) s_.yaw = approachAngle(s_.yaw, target, t_.turnRate * dt);
            const float wish = std::atan2(v, h);
            s_.pitch += std::clamp(wish - s_.pitch, -t_.turnRate * dt, t_.turnRate * dt);
            const vec3 dir = forward() * std::cos(s_.pitch) + vec3(0, 0, std::sin(s_.pitch));
            s_.pos = swimSlide(s_.pos, dir * (anims_.swimSpeed * std::min(1.0f, std::hypot(h, v)) * dt));
            play(s_.pitch > 0.35f ? "swim_sub_up" : s_.pitch < -0.35f ? "swim_sub_down" : "swim_sub", true);
        } else {
            s_.pitch += std::clamp(-s_.pitch, -t_.turnRate * dt, t_.turnRate * dt);
            play("swim_submerged_idle", true);
        }
        advanceClip(dt);
        if (s_.pos.z >= surf) {
            s_.pitch = 0;
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
