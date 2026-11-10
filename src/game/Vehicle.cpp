// Lara's motorcycle: tuning and Havok vehicle data from motorcycle.drm, mount / ride / dismount with the original clips,
// the ride simulated by physics::Scene (Jolt vehicle on the level collision), the launch, fall damage, ramming enemies, bike
// state in saves. Numbers and what is inferred: docs/GAMEPLAY.md "Motorcycle".
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
    "motorcycle_jump_inair", "motorcycle_jump_land", "motorcycle_hard_brake", "motorcycle_air_tilt_left",
    "motorcycle_air_tilt_right", "motorcycle_power_skid_left", "motorcycle_power_skid_right", "motorcycle_donut_left",
    "motorcycle_donut_right",
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
    t.stopSpeed = f(0x04) * kMphToUnits;
    t.steerLow = f(0x10) * kDeg, t.steerHigh = f(0x14) * kDeg;
    t.steerRateLow = f(0x18) * kDeg, t.steerRateHigh = f(0x1B4) * kDeg;
    t.steerFraction = f(0x1C);
    t.brakeRamp = f(0x30);
    t.maxTilt = f(0x40) * kDeg;
    // ponytail: first gear only; the record has one (+0x70 = 1), more would need Jolt gears and 0x791D10's shift rpm.
    t.gearRatio = f(0x54);
    t.reverseRatio = f(0x6C), t.reverseSpeed = f(0x74) * kMphToUnits, t.reverseTorque = f(0xBC);
    t.reverseDelay = (f(0x1F8) + 1) / 30;
    t.launchRpm[0] = f(0x88), t.launchRpm[1] = f(0x8C), t.launchCharge = f(0x1C4), t.launchTime = f(0x1C0), t.launchTorque = f(0x0C);
    for (int i = 0; i < 3; ++i) t.rpm[i] = f(0x78 + 4 * uint32_t(i)), t.torque[i] = f(0x90 + 4 * uint32_t(i));
    t.springHz = f(0xA4), t.springDamping = f(0xA8);
    t.forceRaise = f(0x28);
    t.damageA = f(0x184), t.damageB = f(0x188), t.damageLethal = f(0x18C);
    t.fallMin = f(0x190), t.fallMid = f(0x194), t.fallMax = f(0x198);
    // Lean, air control, stunts (0x793680, 0x790E40, 0x7918B0, 0x790920). Forces in the record's game units (kg units
    // per 30 Hz frame^2) -> N: / (280 / 30^2).
    constexpr float kForce = 280.0f / 900.0f;
    t.leanMax = f(0xB0) * kDeg, t.leanSpeed = f(0xB8);
    t.airYaw = f(0x44), t.airSide = f(0x48);
    t.stuntFlags = read<uint8_t>(rs, rec->offset + 0x52);
    t.handBrakeRamp = f(0x38);
    t.burnoutCharge = f(0x34), t.burnoutSpin = f(0x144) / (30.0f * 60.0f / (2 * kPi)) * 30.0f;
    t.slideSpeed = f(0xE0) * kMphToUnits;
    t.slideLean = f(0xE4) * kDeg, t.slideBrakeRamp = f(0x1B8);
    t.slidePush = vec3(f(0x1D4), -f(0x1D8), f(0xC4)) / kForce;
    t.slideIn = f(0x1E4) / 30, t.slideHold = f(0x1EC) / 30, t.slideOut = f(0x1F4) / 30;
    t.donutPush = f(0xEC) / kForce, t.donutIn = f(0xF4) / 30, t.donutLean = f(0x114) * kDeg;
    // Wheels (0x6AB960): up to four definitions; each names the model's wheel sphere (HInfo head[3]) and the marker
    // at its suspension top; 0x7953F0 keeps the sphere's x, y at the marker's z and the sphere's radius.
    auto body = std::make_shared<physics::ObjectPhysics>(physics::loadObjectPhysics(drm));
    for (uint32_t i = 0; i < 4; ++i) {
        const auto w = ptrAt(drm, data->section, data->offset + 0x90 + 4 * i);
        if (!w) continue;
        const cdc::Section& wsec = drm.sections[w->section];
        const auto g = [&](uint32_t off) { return read<float>(wsec, w->offset + off); };
        const uint8_t sphereId = read<uint8_t>(wsec, w->offset + 0x22), markerId = read<uint8_t>(wsec, w->offset + 0x23);
        const auto sphere = std::ranges::find_if(body->prims, [&](const physics::Prim& p) {
            return p.kind == physics::PrimKind::Sphere && p.head[2] != 2 && p.head[3] == sphereId;
        });
        const auto marker = std::ranges::find(body->hmarkers, markerId, &physics::HMarker::id);
        if (sphere == body->prims.end() || marker == body->hmarkers.end()) throw cdc::FormatError("motorcycle: wheel sphere / marker missing");
        BikeTuning::Wheel wh;
        wh.rest = g(0x00), wh.friction = g(0x08), wh.brake = g(0x0C), wh.slide = g(0x10), wh.drive = g(0x14), wh.springScale = g(0x18);
        wh.flags = read<uint32_t>(wsec, w->offset + 0x1C);
        wh.sphere = sphereId;
        const auto seg = [&](int s) { return s >= 0 && size_t(s) < body->segments.size() ? body->segments[size_t(s)] : glm::mat4(1); };
        const vec3 sp = vec3(seg(sphere->segment) * glm::vec4(sphere->pos, 1)), mk = vec3(seg(marker->segment) * glm::vec4(marker->pos, 1));
        wh.attach = vec3(sp.x, sp.y, mk.z);
        wh.radius = sphere->radius, wh.mass = sphere->mass;
        t.wheels.push_back(wh);
    }
    if (t.wheels.empty()) throw cdc::FormatError("motorcycle: no wheels");
    // The chassis: the other primitives (the wheel rays stand in for the wheel spheres) [I].
    std::erase_if(body->prims, [&](const physics::Prim& p) {
        return p.kind == physics::PrimKind::Sphere && p.head[2] != 2 && std::ranges::any_of(t.wheels, [&](const BikeTuning::Wheel& w) { return w.sphere == p.head[3]; });
    });
    // physics::Scene's vehicle (docs/formats/PHYSICS.md "Vehicles"). Ratio (0x78F860): top rpm at top speed, the
    // wheels' drive-weighted radius; rpm = 30 * 60 / (2 pi) * wheel rad/frame * ratio.
    physics::VehicleSpec& v = t.vehicle;
    t.body = body, v.physics = body.get();
    float driveRadius = 0, mass = 0;
    for (const auto& wh : t.wheels) driveRadius += wh.drive * wh.radius;
    for (const auto& p : body->prims) mass += p.mass; // the chassis (wheel spheres already gone)
    v.ratio = t.rpm[2] * (2 * kPi / 60) * driveRadius / t.maxSpeed;
    v.reverseRatio = t.rpm[2] * (2 * kPi / 60) * driveRadius / t.reverseSpeed * t.gearRatio / t.reverseRatio;
    v.minRpm = t.rpm[0], v.maxRpm = t.rpm[2], v.reverseTorque = t.reverseTorque;
    const auto torqueAt = [&](float r) { // 0x791D10: Lagrange quadratic, rpm clamped to [min, max]
        const float* x = t.rpm;
        r = std::clamp(r, x[0], x[2]);
        return t.torque[0] * (r - x[1]) * (r - x[2]) / ((x[0] - x[1]) * (x[0] - x[2])) +
               t.torque[1] * (r - x[0]) * (r - x[2]) / ((x[1] - x[0]) * (x[1] - x[2])) +
               t.torque[2] * (r - x[0]) * (r - x[1]) / ((x[2] - x[0]) * (x[2] - x[1]));
    };
    for (int i = 0; i <= 18; ++i) v.torque.push_back(torqueAt(t.rpm[2] * float(i) / 18));
    v.maxTorque = *std::ranges::max_element(v.torque);
    for (float& x : v.torque) x /= v.maxTorque;
    v.maxPitchRoll = t.maxTilt;
    for (const auto& wh : t.wheels) {
        physics::VehicleSpec::Wheel w;
        w.attach = wh.attach, w.travel = wh.rest, w.radius = wh.radius, w.mass = wh.mass; // ray: rest + radius to the contact
        w.frequency = t.springHz * wh.springScale, w.damping = t.springDamping;
        w.drive = wh.drive;
        // [I] brake: +0x0C as a velocity change per 30 Hz frame of the wheel's mass share -> N m; hand brake: +0x10 alike
        const float brakeScale = 900.0f * (mass / float(t.wheels.size())) * wh.radius / (physics::kUnitsPerMetre * physics::kUnitsPerMetre);
        w.brake = wh.brake * brakeScale;
        w.handBrake = t.stuntFlags & 4 ? wh.slide * brakeScale : 0.0f;
        w.steer = wh.flags & 1 ? t.steerLow : 0.0f;
        w.friction = wh.friction;
        w.raise = t.forceRaise;
        v.wheels.push_back(w);
    }
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
    const float rear = b.prevRearWheel + wrap(b.rearWheel - b.prevRearWheel) * a;
    local[5].rot = vec3(wrap(wheel), 0, 0), local[8].rot = vec3(wrap(rear), 0, 0); // axles along model X
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

// Sit a standing bike on the ground: both wheel contacts (axle x, y; probes from 160 above) set its height and pitch.
void World::settleBike(Bike& b) {
    const BikeTuning& t = anims_.bike;
    const vec3 p = b.pos + bikeOffset(b);
    const auto ground = [&](float my) -> std::optional<float> {
        const vec3 at = p + rotYaw(b.yaw, vec3(0, my, 0));
        const auto h = col_.groundProbe(vec3(at.x, at.y, p.z + 160), 330);
        return h && h->normal.z >= 0.5f ? std::optional(h->point.z) : std::nullopt;
    };
    const auto zf = ground(t.frontAxle.y), zr = ground(t.rearAxle.y);
    if (!zf || !zr) return;
    b.pitch = std::atan2(*zf - *zr, std::max(1.0f, t.wheelbase()));
    b.pos.z = *zr + (*zf - *zr) * t.rearAxle.y / (t.rearAxle.y - t.frontAxle.y) - bikeOffset(b).z;
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
    return player_.modelMatrix(p, b.prevYaw + wrap(b.yaw - b.prevYaw) * a, glm::mix(b.prevPitch, b.pitch, a), glm::mix(b.prevRoll, b.roll, a));
}

void World::syncVehicles() {
    for (const auto& u : units_)
        for (const auto& v : u.data.vehicles)
            if (!bikes_.contains(v.uid)) {
                Bike b;
                b.uid = v.uid, b.unit = u.data.name, b.pos = v.pos, b.yaw = v.yaw;
                b.prevPos = v.pos + u.offset, b.prevYaw = b.yaw;
                settleBike(b); // the intro stands level; its slope pitches it
                bikes_.emplace(v.uid, std::move(b));
            }
    // A level load moved the stream (units at new offsets): no interpolation from the old frame.
    for (auto& [uid, b] : bikes_) b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevRoll = b.roll;
}

// `instance` 26 with a motorcycle as its receiver (0x4A4140 places the instance at its argument's transform): the parked
// bike stands at the target object, or at its marker, heading as an intro there would (the frame's -Y, as findSpawn),
// on the ground under it. l5_valgrind's 235_val_shaft end handler (7403 vt 60) moves bike 7229 to marker object 7404
// beside where the cutscene leaves Lara, when she rode through its plane [O bytecode]. While she sits on it the
// placement waits until something other than her own dismount takes her off it (the cutscene's end).
void World::placeBike(int32_t uid, int32_t target, int32_t marker) {
    const auto it = bikes_.find(uid);
    const auto* o = script_.find(target);
    if (it == bikes_.end() || !o) return;
    if (riding_ == uid) {
        // She sits on it: the handler ran with the cutscene's start (World sends both cinematic events at once), and
        // the stream's end stands her on foot a little later. The placement waits for that (tickVehicles), not longer
        // than the cutscene can take [I: 120 s].
        if (player_.state().state == State::Vehicle) {
            bikePlace_ = {uid, target, marker}, bikePlaceTick_ = ticks_;
            return;
        }
        riding_ = 0;
    }
    Bike& b = it->second;
    vec3 at = o->pos;
    float yaw = o->rot.z - kPi / 2;
    if (const auto m = marker >= 0 ? markerFrame(*o, marker) : std::nullopt) at = vec3((*m)[3]), yaw = std::atan2(-(*m)[1].y, -(*m)[1].x);
    // The marker may hang over the floor (7404: 230 over it); the original's vehicle body falls onto its wheels. Ours
    // parks it on the ground found under the point [I: up to 1000 down].
    if (const auto h = col_.groundProbe(at + vec3(0, 0, 160), 1160); h && h->normal.z >= 0.5f) at.z = h->point.z;
    b.pos = at - bikeOffset(b), b.yaw = yaw, b.pitch = b.roll = 0;
    b.phase = BikePhase::Parked, b.speed = 0, b.steer = 0, b.vel = vec3(0), b.air = false, b.simWorld.reset();
    settleBike(b);
    b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevRoll = b.roll;
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
    b.phase = BikePhase::Ride, b.t = 0, b.speed = 0, b.steer = 0, b.air = false, b.brakeT = anims_.bike.brakeRamp;
    combat_.drawn = combat_.redraw = false, combat_.drawT = 0;
    PlayerState s = player_.state();
    s.state = State::Vehicle;
    s.pos = b.pos + bikeOffset(b), s.yaw = b.yaw, s.pitch = b.pitch, s.vel = vec3(0);
    s.ledge = -1, s.water = -1, s.grappleUid = 0;
    s.clip = "motorcycle:motorcycle_idle_foot_down", s.loop = true, s.clipTime = 0, s.clipRate = 1;
    s.fallStartZ = s.pos.z, s.fallHeight = 0;
    player_.restore(s);
    // the camera behind her (the original's ch4 start after 180_mex_intro looks up the trail) [I]
    cam_.yaw = b.yaw, cam_.pitch = CameraState{}.pitch;
    updateCamera({}, 0);
    prevCam_ = cam_;
}

// Ramming [I]: a living enemy whose body capsule reaches into a sphere around the front wheel while the bike is faster
// than ramSpeed takes ramDamage per unit/s; the bike loses 30 % of its velocity (a kick in the next physics step); one
// hit per contact. Enemies are not Jolt bodies: the bike does not bounce off them.
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
        const float speed = glm::length(vec2(b.vel));
        if (rammed_.contains(d) || speed < t.ramSpeed) continue;
        rammed_[d] = ticks_;
        d->onHit({speed * t.ramDamage, q + vec3(0, 0, d->height() * 0.5f), heading(b.yaw), Team::Player, false});
        b.kick -= 0.3f * b.vel, b.speed *= 0.7f, b.vel *= 0.7f;
    }
}

// The rider's input for the bike's next physics step (World::stepPhysics simulates it, readBikes takes the result).
// Throttle / brake / reverse follow 0x790E40's states: back while rolling forward faster than stopSpeed (and throttle
// while rolling back) brakes, back below it reverses; with no input under stopSpeed the brakes hold it; above it the
// bike coasts; standing, the reverse gear takes after the brake has been held reverseDelay. The brakes bite over
// brakeRamp (0x792900); the hand brake over handBrakeRamp, without drive. The hand brake is the jump key: tru.exe's
// "Vehicle Brake" (button 22, Space by default); the bike has no hop (none in 0x790E40's states or the record).
// Stunts (0x790E40 states 7 / 11 / 10): throttle + hand brake = burnout (no drive, the rear wheel spins up); with the
// stick past half (0x790920 / 0x7918B0) and below slideSpeed it becomes a donut; brake or hand brake with the stick
// past half above slideSpeed is a power slide. Lean and air control: 0x793680 (BikeTuning).
void World::driveBike(Bike& b, const Actions& a, float dt) {
    const BikeTuning& t = anims_.bike;
    if (b.airT >= 0) b.airT += dt;
    if (b.landT >= 0) b.landT += dt;
    const float throttle = std::max(0.0f, a.move.y), back = std::max(0.0f, -a.move.y), v = b.speed, stick = -a.move.x;
    const bool hand = a.jump, turning = std::abs(stick) > 0.5f, braking = (back > 0 && v > t.stopSpeed) || hand;
    // Steering (tru.exe 0x793680): the wheel limit and its rate blend from the low- to the high-speed values over
    // [steerFraction, 1] * maxSpeed; Jolt turns the front wheel to it (+ steer = left).
    const float lo = t.steerFraction * t.maxSpeed;
    const float f = std::clamp((std::abs(v) - lo) / std::max(1.0f, t.maxSpeed - lo), 0.0f, 1.0f);
    const float limit = glm::mix(t.steerLow, t.steerHigh, f), rate = glm::mix(t.steerRateLow, t.steerRateHigh, f);
    b.steer += std::clamp(stick * limit - b.steer, -rate * dt, rate * dt);
    const float steerFrac = std::clamp(b.steer / std::max(limit, 1e-3f), -1.0f, 1.0f);

    const BikeStunt was = b.stunt;
    const bool burnout = throttle > 0 && hand && (t.stuntFlags & 1);
    switch (b.stunt) {
    case BikeStunt::None:
        b.stunt = burnout && turning && std::abs(v) < t.stopSpeed ? BikeStunt::Donut
                  : burnout && !turning                          ? BikeStunt::Burnout
                  : braking && turning && v > t.slideSpeed       ? BikeStunt::PowerSlide
                                                                 : BikeStunt::None;
        break;
    case BikeStunt::Burnout: b.stunt = !burnout ? BikeStunt::None : turning ? BikeStunt::Donut : BikeStunt::Burnout; break;
    case BikeStunt::Donut: b.stunt = burnout && turning ? BikeStunt::Donut : BikeStunt::None; break;
    case BikeStunt::PowerSlide:
        b.stunt = burnout && turning && v < t.slideSpeed ? BikeStunt::Donut
                  : braking && stick != 0 && v >= t.stopSpeed ? BikeStunt::PowerSlide
                                                              : BikeStunt::None;
        break;
    }
    if (b.air) b.stunt = BikeStunt::None;
    b.stuntT = b.stunt == was ? b.stuntT + dt : 0.0f;
    if (turning) b.stuntDir = stick > 0 ? 1 : -1;

    physics::VehicleDrive d;
    const auto ramp = [](float x, float over) { return std::min(1.0f, x / std::max(over, 1e-3f)); };
    const int rear = int(std::ranges::find_if(t.wheels, [](const BikeTuning::Wheel& w) { return !(w.flags & 1); }) - t.wheels.begin());
    const float out = -float(b.stuntDir); // model +X is the bike's left; the rear swings out of the turn [I sign]
    // Launch (0x793680 bits 23 / 22): revving against the brake charges it; the brake let go with the throttle held
    // fires it, the throttle let go first drops it.
    const bool revving = throttle > 0 && hand;
    if (revving) {
        if (b.launchCharge <= 0) b.launchRpm = std::max(t.rpm[2] * std::abs(v) / t.maxSpeed, t.launchRpm[0]); // +0x774: not below the engine's now
        b.launchCharge = std::min(b.launchCharge + dt, t.launchCharge), b.launchT = -1;
        b.launchRpm = std::max(b.launchRpm, std::min(glm::mix(t.launchRpm[0], t.launchRpm[1], b.launchCharge / std::max(t.launchCharge, 1e-3f)), t.launchRpm[1]));
    } else if (b.launchT < 0 && b.launchCharge > 0) {
        if (throttle > 0 && !b.air) b.launchT = 0;
        else b.launchCharge = 0;
    }
    // 0x790E40 state 3: the counter runs while only the brake is held at a standstill and is cleared when the reverse
    // gear takes, so a hold cut short keeps what it counted (Wine: 0.14 s after an earlier brake to a stop, else 0.25 s).
    if (back > 0 && throttle == 0 && !hand && std::abs(v) < t.stopSpeed) b.reverseT += dt;
    else if (b.reverseT > t.reverseDelay || throttle > 0 || std::abs(v) >= t.stopSpeed) b.reverseT = 0;
    bool brakes = false;
    switch (b.stunt) {
    case BikeStunt::None:
        if (hand) d.handBrake = ramp(b.brakeT, t.handBrakeRamp), brakes = true;
        else if (throttle > 0 && v > -t.stopSpeed) d.forward = throttle;
        else if (back > 0 && (v <= -t.stopSpeed || b.reverseT > t.reverseDelay)) d.forward = back, d.reverse = true;
        else if (throttle > 0 || back > 0 || std::abs(v) < t.stopSpeed) {
            d.brake = ramp(b.brakeT, t.brakeRamp) * (throttle + back > 0 ? throttle + back : 1.0f);
            brakes = true;
        }
        if (b.launchT >= 0) {
            if (b.launchT >= t.launchTime || d.forward <= 0 || d.reverse) b.launchT = -1, b.launchCharge = 0;
            else d.rpm = b.launchRpm, d.torqueScale = t.launchTorque * std::min(1.0f, b.launchCharge / std::max(t.launchCharge, 1e-3f)), b.launchT += dt;
        }
        d.lean = b.air ? 0.0f : steerFrac * std::min(1.0f, std::abs(v) / std::max(1.0f, t.leanSpeed * t.maxSpeed)) * t.leanMax;
        break;
    case BikeStunt::Burnout: // state 7: no drive; the hand brake holds it (0x792900 bit 25)
        d.handBrake = ramp(b.stuntT, t.handBrakeRamp);
        break;
    case BikeStunt::PowerSlide: { // state 11: brakes over slideBrakeRamp; the rear kicked out (0x7918B0 envelope)
        d.brake = ramp(b.stuntT, t.slideBrakeRamp);
        const float s = b.stuntT, e = s < t.slideIn ? s / t.slideIn : s < t.slideIn + t.slideHold ? 1.0f
                                                                       : 1.0f - std::min(1.0f, (s - t.slideIn - t.slideHold) / t.slideOut);
        d.pushWheel = rear, d.push = e * vec3(out * t.slidePush.x, t.slidePush.y, t.slidePush.z);
        d.lean = float(b.stuntDir) * t.slideLean, d.maxYawRate = t.stuntYawRate; // 0x793680 bit 17: +0xE4 into the turn
        break;
    }
    case BikeStunt::Donut: // state 10: hand brake, the rear pushed round the front (0x790920)
        d.handBrake = ramp(b.stuntT, t.handBrakeRamp);
        d.pushWheel = rear, d.push = vec3(out * t.donutPush * ramp(b.stuntT, t.donutIn), 0, 0);
        d.pinWheel = rear == 0 ? 1 : 0; // the "Donut Anchor Body" (0x78F520) holds it: here the front contact [I]
        d.lean = float(b.stuntDir) * t.donutLean, d.maxYawRate = t.stuntYawRate; // bit 18: +0x114
        break;
    }
    b.brakeT = brakes ? b.brakeT + dt : 0.0f;
    if (b.stunt != BikeStunt::None && !revving) b.launchT = -1, b.launchCharge = 0;
    if (b.air) { // air control (0x793680 with no wheel on the ground)
        const float k = steerFrac * v / t.maxSpeed;
        d.yawTorque = t.airYaw * k, d.sideForce = t.airSide * k;
    }
    d.right = -b.steer / std::max(t.steerLow, 1e-3f);
    b.drive = d, b.throttle = d.forward;
}

// The ridden bike for physics::Scene (its ride phase only; mounting, dismounting and parked bikes stand where they are).
std::vector<physics::Scene::Vehicle> World::bikeVehicles() {
    std::vector<physics::Scene::Vehicle> out;
    const auto it = bikes_.find(riding_);
    if (!riding_ || it == bikes_.end() || it->second.phase != BikePhase::Ride || !anims_.bike.vehicle.physics) {
        for (auto& [uid, b] : bikes_) b.simWorld.reset();
        return out;
    }
    Bike& b = it->second;
    if (!b.simWorld) settleBike(b); // a new body starts on the ground, not in a slope
    const vec3 p = b.pos + bikeOffset(b);
    out.push_back({b.uid, &anims_.bike.vehicle, player_.modelMatrix(p, b.yaw, b.pitch, b.roll), b.drive, b.simWorld ? p - *b.simWorld : vec3(0), b.kick});
    b.kick = vec3(0);
    return out;
}

// After the physics step: the ridden bike's pose, speed and wheel contacts; take-off, landing (fall damage), ramming;
// Lara sits on it.
void World::readBikes(float dt) {
    const auto it = bikes_.find(riding_);
    if (!riding_ || it == bikes_.end() || it->second.phase != BikePhase::Ride) return;
    const auto st = physics_.vehicle(riding_);
    if (!st) return;
    Bike& b = it->second;
    const BikeTuning& t = anims_.bike;
    const vec3 p = vec3(st->model[3]), fwd = -vec3(st->model[1]);
    b.yaw = std::atan2(fwd.y, fwd.x), b.pitch = std::asin(std::clamp(fwd.z, -1.0f, 1.0f));
    b.roll = std::asin(std::clamp(-st->model[0].z / std::max(std::cos(b.pitch), 1e-3f), -1.0f, 1.0f)); // model +X = left
    b.vel = st->vel, b.speed = glm::dot(st->vel, fwd);
    b.rpm = std::clamp(t.rpm[2] * std::abs(b.speed) / t.maxSpeed, t.rpm[0], t.rpm[2]); // 0x791D10: from the wheel speed
    const bool air = st->contacts == 0;
    if (air && !b.air) b.airStartZ = p.z, b.airT = 0;
    if (air && b.airStartZ - p.z >= t.fallMax && !health_.dead()) damageLara(t.damageLethal, p, kDamageAbsolute); // in-air lethal check
    if (!air && b.air) { // landing damage (tru.exe 0x793680 state 2, fall = take-off z - landing z, damage type 8)
        b.landT = 0, b.airT = -1;
        const float fall = b.airStartZ - p.z;
        if (fall > t.fallMin && !health_.dead()) {
            const float dmg = fall > t.fallMid
                                  ? (t.damageB - t.damageA) + std::clamp((fall - t.fallMid) / (t.fallMax - t.fallMid), 0.0f, 1.0f) * (t.damageLethal - t.damageB)
                                  : std::clamp((fall - t.fallMin) / (t.fallMid - t.fallMin), 0.0f, 1.0f) * (t.damageB - t.damageA);
            damageLara(dmg, p, kDamageAbsolute);
        }
    }
    b.air = air;
    b.wheel = wrap(b.wheel + (air ? 0.0f : b.speed) / t.wheelRadius * dt);
    const float spin = b.stunt == BikeStunt::Burnout ? t.burnoutSpin * std::min(1.0f, b.stuntT / std::max(t.burnoutCharge, 1e-3f)) : 0.0f;
    b.rearWheel = wrap(b.rearWheel + ((air ? 0.0f : b.speed) / t.wheelRadius + spin) * dt);
    b.pos = p - bikeOffset(b);
    const LoadedUnit& cur = currentUnit(); // re-home to the unit Lara is in (its neighbours may stream out)
    if (b.unit != cur.data.name) b.pos = p - cur.offset, b.unit = cur.data.name;
    b.simWorld = p;
    ramEnemies(b);
    PlayerState s = player_.state();
    s.pos = p, s.yaw = b.yaw, s.pitch = b.pitch, s.roll = b.roll, s.fallStartZ = p.z;
    player_.restore(s);
}

bool World::tickVehicles(const Actions& a, float dt) {
    const bool interactPressed = a.interact && !bikeInteractHeld_;
    bikeInteractHeld_ = a.interact;
    for (auto& [uid, b] : bikes_)
        b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevRoll = b.roll, b.prevSteer = b.steer,
        b.prevWheel = b.wheel, b.prevRearWheel = b.rearWheel;
    const BikeTuning& t = anims_.bike;
    auto it = bikes_.find(riding_);
    if (riding_ && (it == bikes_.end() || player_.state().state != State::Vehicle)) { // died, respawned, loaded, a cutscene's end
        if (it != bikes_.end()) it->second.phase = BikePhase::Parked, it->second.speed = 0, it->second.air = false;
        const int32_t was = std::exchange(riding_, 0);
        if (const auto p = std::exchange(bikePlace_, std::nullopt); p && (*p)[0] == was && ticks_ - bikePlaceTick_ < 120 * 60)
            placeBike((*p)[0], (*p)[1], (*p)[2]); // instance 26 on the bike while she sat on it
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
        combat_.drawn = combat_.redraw = false, combat_.drawT = 0; // [I] guns away to mount; drawn again once riding (Combat.cpp)
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
    s.vel = vec3(0), s.ledge = -1, s.water = -1, s.grappleUid = 0, s.pitch = 0, s.roll = 0;
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
        if (b.t >= end) b.phase = BikePhase::Ride, b.t = 0, b.brakeT = t.brakeRamp; // parked: brakes on [I]
        break;
    }
    case BikePhase::Ride: {
        driveBike(b, a, dt); // input; the step and Lara's seat follow in stepPhysics / readBikes
        if (interactPressed && !b.air && std::abs(b.speed) < t.dismountSpeed)
            b.phase = BikePhase::Dismount, b.t = 0, b.speed = 0, b.vel = vec3(0), b.roll = b.prevRoll = 0, b.stunt = BikeStunt::None, bikePlace_.reset();
        s.pos = b.pos + bikeOffset(b), s.yaw = b.yaw, s.pitch = b.pitch, s.roll = b.roll;
        const float v = b.speed, steer = a.move.x;
        const double next = s.clipTime + dt;
        const auto side = [&](bool left, std::string_view l, std::string_view r) { playClip(left ? l : r, true, next); };
        if (b.air && b.airT >= 0 && b.airT < 0.4f) playClip("motorcycle:motorcycle_jump", false, next);
        else if (b.air && std::abs(steer) > 0.5f) side(steer < 0, "motorcycle:motorcycle_air_tilt_left", "motorcycle:motorcycle_air_tilt_right");
        else if (b.air) playClip("motorcycle:motorcycle_jump_inair", true, next);
        else if (b.landT >= 0 && b.landT < 0.6f) playClip("motorcycle:motorcycle_jump_land", false, next);
        else if (b.stunt == BikeStunt::Donut) side(b.stuntDir > 0, "motorcycle:motorcycle_donut_left", "motorcycle:motorcycle_donut_right");
        else if (b.stunt == BikeStunt::PowerSlide) side(b.stuntDir > 0, "motorcycle:motorcycle_power_skid_left", "motorcycle:motorcycle_power_skid_right");
        else if (b.stunt == BikeStunt::Burnout) playClip("motorcycle:motorcycle_accelerate_loop", true, next); // [I] clip
        else if (a.jump && v > 300) playClip("motorcycle:motorcycle_hard_brake", true, next);
        else if (v < -60) side(steer < 0, "motorcycle:motorcycle_reverse_left", "motorcycle:motorcycle_reverse_right");
        else if (v < 60 && a.move.y <= 0) playClip("motorcycle:motorcycle_idle_foot_down", true, next);
        else if (a.move.y < 0 && v > 300) playClip("motorcycle:motorcycle_braking", true, next);
        else if (std::abs(steer) > 0.5f && v > 300) side(steer < 0, "motorcycle:motorcycle_lean_left", "motorcycle:motorcycle_lean_right");
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

bool World::ridingBike() const {
    const auto it = bikes_.find(riding_);
    return riding_ && it != bikes_.end() && it->second.phase == BikePhase::Ride;
}

nlohmann::json World::serializeVehicles() const {
    nlohmann::json bikes = nlohmann::json::array();
    for (const auto& [uid, b] : bikes_)
        bikes.push_back({{"uid", uid}, {"unit", b.unit}, {"pos", {b.pos.x, b.pos.y, b.pos.z}}, {"yaw", b.yaw}, {"pitch", b.pitch},
                         {"roll", b.roll}, {"speed", b.speed}, {"steer", b.steer}, {"wheel", b.wheel}, {"rearWheel", b.rearWheel}, {"air", b.air},
                         {"stunt", int(b.stunt)}, {"stuntT", b.stuntT}, {"stuntDir", b.stuntDir},
                         {"vel", {b.vel.x, b.vel.y, b.vel.z}}, {"airStartZ", b.airStartZ - bikeOffset(b).z},
                         {"kick", {b.kick.x, b.kick.y, b.kick.z}}, {"brakeT", b.brakeT}, {"airT", b.airT}, {"landT", b.landT},
                         {"launchCharge", b.launchCharge}, {"launchT", b.launchT}, {"launchRpm", b.launchRpm}, {"reverseT", b.reverseT},
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
        b.yaw = e.at("yaw").get<float>(), b.pitch = e.value("pitch", 0.0f), b.roll = e.value("roll", 0.0f);
        b.speed = e.value("speed", 0.0f), b.steer = e.value("steer", 0.0f), b.wheel = e.value("wheel", 0.0f);
        b.rearWheel = e.value("rearWheel", b.wheel);
        b.stunt = BikeStunt(std::clamp(e.value("stunt", 0), 0, int(BikeStunt::Donut)));
        b.stuntT = e.value("stuntT", 0.0f), b.stuntDir = int8_t(e.value("stuntDir", 0));
        b.air = e.value("air", false);
        if (e.contains("vel")) b.vel = v3(e["vel"]);
        if (e.contains("kick")) b.kick = v3(e["kick"]);
        b.brakeT = e.value("brakeT", 0.0f), b.airT = e.value("airT", -1.0f), b.landT = e.value("landT", -1.0f);
        b.launchCharge = e.value("launchCharge", 0.0f), b.launchT = e.value("launchT", -1.0f), b.launchRpm = e.value("launchRpm", 0.0f);
        b.reverseT = e.value("reverseT", 0.0f);
        const std::string phase = e.value("phase", std::string("parked"));
        b.phase = BikePhase::Parked;
        for (int p = 0; p < 4; ++p)
            if (bikePhaseName(BikePhase(p)) == phase) b.phase = BikePhase(p);
        b.t = e.value("t", 0.0);
        b.airStartZ = e.value("airStartZ", b.pos.z) + bikeOffset(b).z;
        b.prevPos = b.pos + bikeOffset(b), b.prevYaw = b.yaw, b.prevPitch = b.pitch, b.prevRoll = b.roll, b.prevSteer = b.steer;
        b.prevWheel = b.wheel, b.prevRearWheel = b.rearWheel;
        bikes_[b.uid] = std::move(b);
    }
    riding_ = j.value("riding", 0);
    bikeMounts = j.value("mounts", 0);
    if (riding_ && !bikes_.contains(riding_)) throw std::runtime_error("vehicle state: ridden bike " + std::to_string(riding_) + " unknown");
    bikeInteractHeld_ = true;
}

} // namespace ou::game
