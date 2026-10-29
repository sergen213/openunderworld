// Lara's motorcycle: tuning from motorcycle.drm, mount / ride / dismount with the original clips, terrain following on
// two wheel contacts, hops and ramps, fall damage, ramming enemies, bike state in saves. Numbers and what is inferred:
// docs/GAMEPLAY.md "Motorcycle".
#include "game/Game.hpp"

#include "formats/Formats.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace ou::game {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;
constexpr float kMphToUnits = 280.0f / 2.237f; // tru.exe speed (0x105B30C): v * 30/280 m/s * 2.237 mph; 280 units = 1 m
// Collision proxies [I]: three spheres / a capsule between the axles for walls (high enough to roll up steps the wheel
// probes find), a sphere at axle height in the air.
constexpr float kCapsuleRadius = 80.0f, kCapsuleHeight = 200.0f, kAirRadius = 100.0f;
constexpr float kProbeUp = 160.0f, kProbeDown = 170.0f; // wheel ground probe window around the current height
constexpr float kGroundNormalZ = 0.5f;                  // flatter = ground for the wheels (~60 degrees)
constexpr float kMaxPitch = 0.75f;                      // steeper climbs stop the bike (~43 degrees) [I]

constexpr std::string_view kMount = "motorcycle:motorcycle_mount_cycle", kDismount = "motorcycle:motorcycle_dismount_cycle";

float wrap(float a) {
    a = std::fmod(a + kPi, 2 * kPi);
    return (a < 0 ? a + 2 * kPi : a) - kPi;
}
vec3 heading(float yaw) { return {std::cos(yaw), std::sin(yaw), 0}; }
// Model-space vector -> world for heading `yaw` (model -Y = forward), as Player.
vec3 rotYaw(float yaw, vec3 m) {
    const float c = std::cos(yaw + kPi / 2), s = std::sin(yaw + kPi / 2);
    return {c * m.x - s * m.y, s * m.x + c * m.y, m.z};
}
vec3 rotZ(float a, vec3 v) { return {std::cos(a) * v.x - std::sin(a) * v.y, std::sin(a) * v.x + std::cos(a) * v.y, v.z}; }

// Root yaw (segment 0 axis-angle Z) with the keys unwrapped: the mount/dismount clips turn through +-180 degrees,
// where the stored axis-angle flips sign. Same key lag as rootTranslation / anim::sample.
float clipYaw(const anim::Clip& c, double t) {
    if (c.tracks.empty() || c.timePerKey == 0) return 0;
    const auto& k = c.tracks[0][anim::RotZ];
    if (k.empty()) return 0;
    std::vector<float> u(k.begin(), k.end());
    for (size_t i = 1; i < u.size(); ++i) u[i] = u[i - 1] + wrap(k[i] - k[i - 1]);
    const double x = std::clamp(t, 0.0, c.durationSeconds()) * anim::kTimeUnitsPerSecond / c.timePerKey;
    const int i = int(std::floor(x));
    if (i >= int(u.size())) return u.back();
    const float a = i > 0 ? u[size_t(i) - 1] : u[0];
    return a + (u[size_t(i)] - a) * float(x - i);
}

template <class T> T read(const cdc::Section& s, uint32_t off) {
    if (off + sizeof(T) > s.data.size()) throw cdc::FormatError("motorcycle data: read past section end");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}
std::optional<cdc::Pointer> ptrAt(const cdc::Drm& d, uint32_t section, uint32_t off) {
    const auto p = cdc::sectionPointers(d, section);
    const auto it = p.find(off);
    return it == p.end() ? std::nullopt : std::optional(it->second);
}
} // namespace

const std::vector<std::string_view> kBikeClipNames = {
    "motorcycle_mount_cycle", "motorcycle_dismount_cycle", "motorcycle_idle_foot_down", "motorcycle_ride_slow",
    "motorcycle_ride_fast", "motorcycle_accelerate_loop", "motorcycle_braking", "motorcycle_lean_left",
    "motorcycle_lean_right", "motorcycle_reverse_left", "motorcycle_reverse_right", "motorcycle_jump",
    "motorcycle_jump_inair", "motorcycle_jump_land",
};

std::string_view bikePhaseName(BikePhase p) {
    static constexpr std::string_view k[] = {"parked", "mount", "ride", "dismount"};
    return size_t(p) < std::size(k) ? k[size_t(p)] : "unknown";
}

BikeTuning loadBikeTuning(const cdc::Drm& drm) {
    const auto base = formats::objectBaseSection(drm);
    if (!base) throw cdc::FormatError("motorcycle: no object section");
    const auto data = ptrAt(drm, *base, 0xC8);
    if (!data) throw cdc::FormatError("motorcycle: ObjectBase +0xC8 has no vehicle data");
    const cdc::Section& ds = drm.sections[data->section];
    if (read<uint16_t>(ds, data->offset + 4) != 0xB00B || read<uint32_t>(ds, data->offset + 8) != 1)
        throw cdc::FormatError("motorcycle: vehicle data is not a Bike (0xB00B, type 1)");
    const auto rec = ptrAt(drm, data->section, data->offset + 0x10); // tru.exe 0x6AB960 -> Bike(+0x4C4)
    if (!rec) throw cdc::FormatError("motorcycle: no default tuning record");
    const cdc::Section& rs = drm.sections[rec->section];
    const auto f = [&](uint32_t off) { return read<float>(rs, rec->offset + off); };
    BikeTuning t;
    t.maxSpeed = f(0x00) * kMphToUnits;
    t.steerLow = f(0x10) * kDeg, t.steerHigh = f(0x14) * kDeg;
    t.steerRateLow = f(0x18) * kDeg, t.steerRateHigh = f(0x1B4) * kDeg;
    t.steerFraction = f(0x1C);
    t.damageA = f(0x184), t.damageB = f(0x188), t.damageLethal = f(0x18C);
    t.fallMin = f(0x190), t.fallMid = f(0x194), t.fallMax = f(0x198);
    std::vector<std::string> warnings;
    for (const auto& sk : anim::decodeSkeletons(drm, warnings)) {
        if (sk.bones.size() <= 8) continue;
        const auto at = anim::bindPositions(sk);
        t.frontAxle = at[5], t.rearAxle = at[8];
        t.wheelRadius = 0.5f * (at[5].z + at[8].z);
        break;
    }
    return t;
}

std::vector<anim::LocalTransform> bikePose(const scene::Skeleton& skeleton, const Bike& b, double alpha) {
    auto local = anim::bindPose(skeleton);
    if (local.size() <= 8) return local;
    const float a = float(alpha);
    const float wheel = b.prevWheel + wrap(b.wheel - b.prevWheel) * a, steer = glm::mix(b.prevSteer, b.steer, a);
    local[5].rot = local[8].rot = vec3(wrap(wheel), 0, 0); // axles along model X
    // Handlebars + fork (bone 3) about the steering head axis (bone 2 -> front axle 5, raked back) [I].
    const vec3 axis = glm::normalize(vec3(0, 94.4f, 129.9f));
    local[3].rot = axis * steer;
    return local;
}

vec3 World::bikeOffset(const Bike& b) const {
    for (const auto& u : units_)
        if (u.data.name == b.unit) return u.offset;
    return vec3(0);
}

const Bike* World::ridden() const {
    const auto it = bikes_.find(riding_);
    return riding_ && it != bikes_.end() ? &it->second : nullptr;
}

std::optional<glm::mat4> World::bikeMatrix(int32_t uid, double alpha) const {
    const auto it = bikes_.find(uid);
    if (it == bikes_.end() || std::none_of(units_.begin(), units_.end(), [&](const LoadedUnit& u) { return u.data.name == it->second.unit; }))
        return std::nullopt;
    if (const auto* o = script_.find(uid); o && o->hidden && riding_ != uid) return std::nullopt; // instance 12 / 60 (7528)
    const Bike& b = it->second;
    const float a = float(alpha);
    const vec3 p = glm::mix(b.prevPos, b.pos + bikeOffset(b), a);
    return player_.modelMatrix(p, b.prevYaw + wrap(b.yaw - b.prevYaw) * a, glm::mix(b.prevPitch, b.pitch, a));
}

void World::syncVehicles() {
    for (const auto& u : units_)
        for (const auto& v : u.data.vehicles)
            if (!bikes_.contains(v.uid)) {
                Bike b;
                b.uid = v.uid, b.unit = u.data.name, b.pos = v.pos, b.yaw = v.yaw;
                b.prevPos = v.pos + u.offset, b.prevYaw = b.yaw;
                bikes_.emplace(v.uid, std::move(b));
            }
    // A level load moved the stream (units at new offsets): no interpolation from the old frame.
    for (auto& [uid, b] : bikes_) b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch;
}

// larabase 52 (0x777DD0 posts message 0x85 to the bike and to Lara): she sits on it at once, riding (the original's ch4
// and ch5 starts after their intro cinematics); the bike's unit becomes the current one (l5_valgrind_tunnel's 7229).
void World::mountBike(int32_t uid) {
    const auto it = bikes_.find(uid);
    if (it == bikes_.end() || !bikeMatrix(uid, 1)) {
        warnings.push_back("mount: bike " + std::to_string(uid) + " not loaded");
        return;
    }
    if (const auto old = bikes_.find(riding_); riding_ != uid && old != bikes_.end()) old->second.phase = BikePhase::Parked, old->second.speed = 0;
    Bike& b = it->second;
    recentre(b.unit);
    riding_ = uid, ++bikeMounts;
    b.phase = BikePhase::Ride, b.t = 0, b.speed = 0, b.steer = 0, b.air = false;
    combat_.drawn = combat_.redraw = false, combat_.drawT = 0;
    PlayerState s = player_.state();
    s.state = State::Vehicle;
    s.pos = b.pos + bikeOffset(b), s.yaw = b.yaw, s.pitch = b.pitch, s.vel = vec3(0);
    s.ledge = -1, s.water = -1, s.grappleUid = 0;
    s.clip = "motorcycle:motorcycle_idle_foot_down", s.loop = true, s.clipTime = 0, s.clipRate = 1;
    s.fallStartZ = s.pos.z, s.fallHeight = 0;
    player_.restore(s);
}

// Ramming [I]: a living enemy whose body capsule reaches into a sphere around the front wheel while the bike is faster
// than ramSpeed takes ramDamage per unit/s; the bike loses 30 % of its speed; one hit per contact.
void World::ramEnemies(Bike& b) {
    const BikeTuning& t = anims_.bike;
    std::erase_if(rammed_, [&](const auto& kv) { return std::find(targets.begin(), targets.end(), kv.first) == targets.end(); });
    const vec3 p = b.pos + bikeOffset(b), front = p + rotYaw(b.yaw, vec3(0, t.frontAxle.y, t.wheelRadius));
    for (Damageable* d : targets) {
        if (!d->alive() || d->team() != Team::Enemy) continue;
        const vec3 q = d->position();
        const float reach = d->radius() + 160.0f;
        const bool touching = glm::length(vec2(front - q)) < reach && front.z > q.z - 100 && front.z < q.z + d->height();
        if (!touching) {
            rammed_.erase(d);
            continue;
        }
        const float speed = glm::length(b.air ? vec2(b.vel) : vec2(heading(b.yaw) * b.speed));
        if (rammed_.contains(d) || speed < t.ramSpeed) continue;
        rammed_[d] = ticks_;
        d->onHit({speed * t.ramDamage, q + vec3(0, 0, d->height() * 0.5f), heading(b.yaw), Team::Player, false});
        b.speed *= 0.7f, b.vel *= 0.7f;
    }
}

void World::driveBike(Bike& b, const Actions& a, bool jumpPressed, float dt) {
    const BikeTuning& t = anims_.bike;
    const vec3 off = bikeOffset(b);
    vec3 p = b.pos + off;
    const float wb = std::max(1.0f, t.wheelbase()), frontY = t.frontAxle.y, rearY = t.rearAxle.y;
    const float originFrac = rearY / (rearY - frontY); // model origin between the contacts (0 rear .. 1 front)
    const auto ground = [&](vec3 at) -> std::optional<float> {
        const auto h = col_.groundProbe(vec3(at.x, at.y, p.z + kProbeUp), kProbeUp + kProbeDown);
        if (!h || h->normal.z < kGroundNormalZ) return std::nullopt;
        return h->point.z;
    };
    const auto contacts = [&](vec3 at, float yaw) {
        return std::pair(ground(at + rotYaw(yaw, vec3(0, frontY, 0))), ground(at + rotYaw(yaw, vec3(0, rearY, 0))));
    };
    const auto takeOff = [&](float extraVz) {
        b.air = true;
        b.vel = heading(b.yaw) * (b.speed * std::cos(b.pitch)) + vec3(0, 0, b.speed * std::sin(b.pitch) + extraVz);
        b.airStartZ = p.z;
    };
    if (b.hopT >= 0) b.hopT += dt;
    if (b.landT >= 0) b.landT += dt;

    if (!b.air) {
        const float throttle = std::max(0.0f, a.move.y), back = std::max(0.0f, -a.move.y);
        float& v = b.speed;
        if (throttle > 0 && v >= 0) v += t.accel * throttle * std::max(0.0f, 1 - (v / t.maxSpeed) * (v / t.maxSpeed)) * dt;
        else if (throttle > 0) v = std::min(0.0f, v + t.brake * throttle * dt);
        else if (back > 0 && v > 0) v = std::max(0.0f, v - t.brake * back * dt);
        else if (back > 0) v = std::max(-t.reverseSpeed, v - t.reverseAccel * back * dt);
        else v = v > 0 ? std::max(0.0f, v - t.coast * dt) : std::min(0.0f, v + t.coast * dt);
        // Steering (tru.exe 0x793680): the wheel limit and its rate blend from the low- to the high-speed values over
        // [steerFraction, 1] * maxSpeed; the heading follows the bicycle model [I].
        const float lo = t.steerFraction * t.maxSpeed;
        const float f = std::clamp((std::abs(v) - lo) / std::max(1.0f, t.maxSpeed - lo), 0.0f, 1.0f);
        const float limit = glm::mix(t.steerLow, t.steerHigh, f), rate = glm::mix(t.steerRateLow, t.steerRateHigh, f);
        b.steer += std::clamp(-a.move.x * limit - b.steer, -rate * dt, rate * dt);
        b.yaw = wrap(b.yaw + v / wb * std::tan(b.steer) * dt);
        // Walls: sweep the axle capsule (contacts it moves away from or already touches are left to the push-out),
        // slide along what it hits, then push the body spheres out of walls; the speed keeps its part along the wall.
        const auto alongWall = [&](vec3 n) {
            const vec3 vv = heading(b.yaw) * v;
            if (const float into = glm::dot(vv, n); into < 0) v = glm::dot(vv - n * into, heading(b.yaw));
        };
        vec3 delta = heading(b.yaw) * (v * dt);
        for (int i = 0; i < 3 && glm::length(delta) > 1e-3f; ++i) {
            const vec3 A = p + rotYaw(b.yaw, vec3(0, rearY * 0.8f, kCapsuleHeight)), B = p + rotYaw(b.yaw, vec3(0, frontY * 0.8f, kCapsuleHeight));
            const auto h = col_.sweepCapsule(A, B, kCapsuleRadius, delta);
            if (!h || h->t <= 0 || h->normal.z >= kGroundNormalZ || glm::dot(delta, h->normal) >= 0) {
                p += delta;
                break;
            }
            const vec3 n = glm::normalize(vec3(h->normal.x, h->normal.y, 0) + vec3(1e-6f, 0, 0));
            p += delta * std::max(0.0f, h->t - 0.01f);
            vec3 rest = delta * (1 - h->t);
            rest -= n * std::min(0.0f, glm::dot(rest, n));
            delta = rest;
            alongWall(n);
        }
        for (int it = 0; it < 3; ++it) {
            bool pushed = false;
            for (const float my : {rearY * 0.8f, 0.0f, frontY * 0.8f}) {
                const vec3 c = p + rotYaw(b.yaw, vec3(0, my, kCapsuleHeight));
                const auto h = col_.closestPoint(c, kCapsuleRadius);
                if (!h || h->normal.z >= kGroundNormalZ) continue; // floors are the wheels' business
                vec3 d = c - h->point;
                const float depth = kCapsuleRadius - glm::length(d);
                d.z = 0;
                if (depth <= 0 || glm::length(d) < 1e-3f) continue;
                const vec3 n = glm::normalize(d);
                p += n * (depth + 0.5f);
                alongWall(n);
                pushed = true;
            }
            if (!pushed) break;
        }
        const auto [zf, zr] = contacts(p, b.yaw);
        if (zf && zr) {
            const float pitch = std::atan2(*zf - *zr, wb);
            if (pitch > kMaxPitch && v > 0) { // too steep to climb: stop where it was
                p = b.pos + off;
                v = 0;
            } else {
                p.z = *zr + (*zf - *zr) * originFrac;
                b.pitch = pitch;
            }
        } else {
            takeOff(0); // ramp edge or drop: fly with the slope's velocity
        }
        if (!b.air && jumpPressed) {
            takeOff(t.hopSpeed); // hop [I]
            b.hopT = 0;
        }
    } else {
        b.vel.z -= t.gravity * dt;
        const vec3 d = b.vel * dt;
        const auto h = col_.sweepSphere(p + vec3(0, 0, t.wheelRadius), kAirRadius, d);
        if (!h) {
            p += d;
        } else {
            p += d * std::max(0.0f, h->t - 0.01f);
            if (h->normal.z < kGroundNormalZ || b.vel.z > 0) b.vel -= h->normal * std::min(0.0f, glm::dot(b.vel, h->normal));
        }
        const vec2 hv(b.vel);
        const float pitchTarget = std::clamp(std::atan2(b.vel.z, std::max(1.0f, glm::length(hv))), -0.6f, 0.6f);
        b.pitch += std::clamp(pitchTarget - b.pitch, -1.5f * dt, 1.5f * dt); // [I]
        const float drop = b.airStartZ - p.z;
        if (drop >= t.fallMax && !health_.dead()) damageLara(t.damageLethal, p, kDamageAbsolute); // in-air lethal check
        const auto [zf, zr] = contacts(p, b.yaw);
        const bool touch = h && h->normal.z >= kGroundNormalZ && b.vel.z <= 0;
        if (b.vel.z <= 0 && zf && zr && (touch || p.z - std::max(*zf, *zr) < 5.0f)) {
            p.z = *zr + (*zf - *zr) * originFrac;
            b.pitch = std::atan2(*zf - *zr, wb);
            b.air = false;
            b.speed = glm::dot(vec3(hv, 0), heading(b.yaw));
            b.landT = 0, b.hopT = -1;
            // Landing damage (tru.exe 0x793680 state 2, fall = take-off z - landing z, damage type 8).
            const float fall = b.airStartZ - p.z;
            if (fall > t.fallMin && !health_.dead()) {
                const float dmg = fall > t.fallMid
                                      ? (t.damageB - t.damageA) + std::clamp((fall - t.fallMid) / (t.fallMax - t.fallMid), 0.0f, 1.0f) * (t.damageLethal - t.damageB)
                                      : std::clamp((fall - t.fallMin) / (t.fallMid - t.fallMin), 0.0f, 1.0f) * (t.damageB - t.damageA);
                damageLara(dmg, p, kDamageAbsolute);
            }
        }
    }
    b.wheel = wrap(b.wheel + (b.air ? 0.0f : b.speed) / t.wheelRadius * dt);
    b.pos = p - off;
    const LoadedUnit& cur = currentUnit(); // re-home to the unit Lara is in (its neighbours may stream out)
    if (b.unit != cur.data.name) b.pos = p - cur.offset, b.unit = cur.data.name;
}

bool World::tickVehicles(const Actions& a, float dt) {
    const bool interactPressed = a.interact && !bikeInteractHeld_, jumpPressed = a.jump && !bikeJumpHeld_;
    bikeInteractHeld_ = a.interact, bikeJumpHeld_ = a.jump;
    for (auto& [uid, b] : bikes_)
        b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevSteer = b.steer, b.prevWheel = b.wheel;
    const BikeTuning& t = anims_.bike;
    auto it = bikes_.find(riding_);
    if (riding_ && (it == bikes_.end() || player_.state().state != State::Vehicle)) { // died, respawned, loaded
        if (it != bikes_.end()) it->second.phase = BikePhase::Parked, it->second.speed = 0, it->second.air = false;
        riding_ = 0;
        return false;
    }
    PlayerState s = player_.state();
    if (!riding_) {
        if (!interactPressed || health_.dead() || units_.empty()) return false;
        if (s.state != State::Idle && s.state != State::Walk && s.state != State::Run && s.state != State::Stop && s.state != State::Turn)
            return false;
        const anim::Clip* mount = anims_.find(kMount);
        if (!mount) return false;
        // Mount spot: where motorcycle_mount_cycle starts relative to the seat it ends on (Lara's root = the bike's).
        const double end = mount->durationSeconds();
        const vec3 spot = rotZ(-clipYaw(*mount, end), -rootTranslation(*mount, end, false));
        float best = t.mountReach;
        for (auto& [uid, b] : bikes_) {
            if (b.phase != BikePhase::Parked || !bikeMatrix(uid, 1)) continue;
            const vec3 at = b.pos + bikeOffset(b) + rotYaw(b.yaw, spot);
            const float d = glm::length(vec2(at - s.pos));
            if (d < best && std::abs(at.z - s.pos.z) < 150.0f) best = d, it = bikes_.find(uid);
        }
        if (best >= t.mountReach) return false;
        riding_ = it->first;
        it->second.phase = BikePhase::Mount, it->second.t = 0, it->second.speed = 0, it->second.steer = 0;
        combat_.drawn = combat_.redraw = false, combat_.drawT = 0; // [I] weapons go away on the bike
        ++bikeMounts;
        s.clip.clear(); // the mount clip starts now
    }
    Bike& b = it->second;
    const vec3 bp = b.pos + bikeOffset(b);
    const auto playClip = [&](std::string_view name, bool loop, double time) {
        if (s.clip != name || s.loop != loop) s.clip = std::string(name), s.loop = loop, s.clipTime = time;
        else s.clipTime = time;
        s.clipRate = 1;
    };
    s.state = State::Vehicle;
    s.vel = vec3(0), s.ledge = -1, s.water = -1, s.grappleUid = 0, s.pitch = 0;
    switch (b.phase) {
    case BikePhase::Mount: {
        const anim::Clip& c = *anims_.find(kMount);
        b.t += dt;
        const double end = c.durationSeconds(), tt = std::min(b.t, end);
        const float endYaw = clipYaw(c, end);
        s.pos = bp + rotYaw(b.yaw, rotZ(-endYaw, rootTranslation(c, tt, false) - rootTranslation(c, end, false)));
        s.yaw = wrap(b.yaw + clipYaw(c, tt) - endYaw);
        playClip(kMount, false, tt);
        s.pitch = b.pitch * float(tt / end); // [I] from standing to the bike's slope
        if (b.t >= end) b.phase = BikePhase::Ride, b.t = 0;
        break;
    }
    case BikePhase::Ride: {
        driveBike(b, a, jumpPressed, dt);
        ramEnemies(b);
        if (interactPressed && !b.air && std::abs(b.speed) < t.dismountSpeed) b.phase = BikePhase::Dismount, b.t = 0, b.speed = 0;
        s.pos = b.pos + bikeOffset(b), s.yaw = b.yaw, s.pitch = b.pitch;
        const float v = b.speed, steer = a.move.x;
        const double next = s.clipTime + dt;
        if (b.air) playClip(b.hopT >= 0 && b.hopT < 0.4f ? "motorcycle:motorcycle_jump" : "motorcycle:motorcycle_jump_inair", b.hopT >= 0.4f || b.hopT < 0, next);
        else if (b.landT >= 0 && b.landT < 0.6f) playClip("motorcycle:motorcycle_jump_land", false, next);
        else if (v < -60) playClip(steer < 0 ? "motorcycle:motorcycle_reverse_left" : "motorcycle:motorcycle_reverse_right", true, next);
        else if (v < 60 && a.move.y <= 0) playClip("motorcycle:motorcycle_idle_foot_down", true, next);
        else if (a.move.y < 0 && v > 300) playClip("motorcycle:motorcycle_braking", true, next);
        else if (std::abs(steer) > 0.5f && v > 300) playClip(steer < 0 ? "motorcycle:motorcycle_lean_left" : "motorcycle:motorcycle_lean_right", true, next);
        else if (a.move.y > 0 && v < 0.3f * t.maxSpeed) playClip("motorcycle:motorcycle_accelerate_loop", true, next);
        else playClip(v > 0.6f * t.maxSpeed ? "motorcycle:motorcycle_ride_fast" : "motorcycle:motorcycle_ride_slow", true, next);
        break;
    }
    case BikePhase::Dismount: {
        const anim::Clip* c = anims_.find(kDismount);
        b.t += dt;
        const double end = c ? c->durationSeconds() : 0, tt = std::min(b.t, end);
        s.pos = bp + (c ? rotYaw(b.yaw, rootTranslation(*c, tt, false)) : vec3(0));
        s.yaw = wrap(b.yaw + (c ? clipYaw(*c, tt) : 0.0f));
        playClip(kDismount, false, tt);
        s.pitch = b.pitch * (1 - float(end > 0 ? tt / end : 1.0));
        if (b.t >= end) { // on her feet beside the bike
            if (const auto g = col_.groundProbe(s.pos + vec3(0, 0, 150), 400)) s.pos.z = g->point.z;
            s.state = State::Idle, s.clip = "ng_idle", s.loop = true, s.clipTime = 0;
            b.phase = BikePhase::Parked, b.steer = 0;
            riding_ = 0;
        }
        break;
    }
    case BikePhase::Parked: break;
    }
    s.fallStartZ = s.pos.z, s.fallHeight = 0;
    player_.restore(s);
    return true;
}

nlohmann::json World::serializeVehicles() const {
    nlohmann::json bikes = nlohmann::json::array();
    for (const auto& [uid, b] : bikes_)
        bikes.push_back({{"uid", uid}, {"unit", b.unit}, {"pos", {b.pos.x, b.pos.y, b.pos.z}}, {"yaw", b.yaw}, {"pitch", b.pitch},
                         {"speed", b.speed}, {"steer", b.steer}, {"wheel", b.wheel}, {"air", b.air},
                         {"vel", {b.vel.x, b.vel.y, b.vel.z}}, {"airStartZ", b.airStartZ - bikeOffset(b).z},
                         {"phase", std::string(bikePhaseName(b.phase))}, {"t", b.t}});
    return {{"riding", riding_}, {"mounts", bikeMounts}, {"bikes", bikes}};
}

void World::restoreVehicles(const nlohmann::json& j) {
    if (!j.is_object()) throw std::runtime_error("vehicle state is not an object");
    const auto v3 = [](const nlohmann::json& a) { return vec3(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()); };
    for (const auto& e : j.at("bikes")) {
        Bike b;
        b.uid = e.at("uid").get<int32_t>();
        b.unit = e.at("unit").get<std::string>();
        b.pos = v3(e.at("pos"));
        b.yaw = e.at("yaw").get<float>(), b.pitch = e.value("pitch", 0.0f);
        b.speed = e.value("speed", 0.0f), b.steer = e.value("steer", 0.0f), b.wheel = e.value("wheel", 0.0f);
        b.air = e.value("air", false);
        if (e.contains("vel")) b.vel = v3(e["vel"]);
        const std::string phase = e.value("phase", std::string("parked"));
        b.phase = BikePhase::Parked;
        for (int p = 0; p < 4; ++p)
            if (bikePhaseName(BikePhase(p)) == phase) b.phase = BikePhase(p);
        b.t = e.value("t", 0.0);
        b.airStartZ = e.value("airStartZ", b.pos.z) + bikeOffset(b).z;
        b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevSteer = b.steer, b.prevWheel = b.wheel;
        bikes_[b.uid] = std::move(b);
    }
    riding_ = j.value("riding", 0);
    bikeMounts = j.value("mounts", 0);
    if (riding_ && !bikes_.contains(riding_)) throw std::runtime_error("vehicle state: ridden bike " + std::to_string(riding_) + " unknown");
    bikeInteractHeld_ = bikeJumpHeld_ = true;
}

} // namespace ou::game
