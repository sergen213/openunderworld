// Lara's motorcycle (objectlist `motorcycle`, tru.exe classes Bike : Vehicle, VehicleCamera): data types; the
// World side (mount / ride / dismount, terrain following, ramming, saves) is in Vehicle.cpp. Evidence, numbers and
// what is inferred: docs/GAMEPLAY.md "Motorcycle".
#pragma once

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"
#include "physics/Physics.hpp"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::game {

// A `motorcycle` intro: unit frame; yaw = heading (intro rot.z - 90 degrees, as findSpawn).
struct VehicleIntro {
    int32_t uid = 0;
    glm::vec3 pos{0};
    float yaw = 0;
};

// Bike tuning. [V-data] motorcycle.drm vehicle data (ObjectBase +0xC8: u16 +4 0xB00B, class 0x40, +8 = 1 Bike, +0x90 wheel
// list) and its default tuning record (+0x10 -> 0x20C bytes, tru.exe Bike +0x4C4); the model's wheel spheres / markers;
// the skeleton's axle bones. What is inferred is marked [I].
struct BikeTuning {
    float maxSpeed = 5008.0f;       // record +0x00 40 mph -> units/s (280 units = 1 m, tru.exe 0x105B30C)
    float stopSpeed = 250.4f;       // +0x04 2 mph: slower = stopped (brake holds, back = reverse; 0x790E40)
    float steerFraction = 0.3f;     // +0x1C: the steering limits blend over [steerFraction, 1] * maxSpeed
    float steerLow = 0.5236f, steerHigh = 0.1745f;       // +0x10 / +0x14: front wheel limit, 30 / 10 degrees
    float steerRateLow = 1.658f, steerRateHigh = 1.309f; // +0x18 / +0x1B4: 95 / 75 degrees/s
    float brakeRamp = 1.25f;        // +0x30: seconds for the brakes to bite fully (0x792900 states 3 / 5)
    float maxTilt = 0.7418f;        // +0x40 42.5 degrees: the body's tilt limit (0x795080 constraint)
    // Engine (0x791D10): torque (N m) quadratic through (rpm, torque) at +0x78 / +0x90, +0x7C / +0x94, +0x80 / +0x98,
    // none above the top rpm; one gear (+0x70 count 1, ratio +0x54); rpm = top rpm at top speed (0x78F860).
    float rpm[3] = {1000, 10000, 18000}, torque[3] = {30, 20, 20};
    float reverseSpeed = 1877.6f;   // +0x74 15 mph, reverse ratio +0x6C 4 against gear +0x54 6: rpm top at 10 mph
    float gearRatio = 6, reverseRatio = 4, reverseTorque = 20; // +0x54, +0x6C; +0xBC N m in reverse (0x791D10)
    float springHz = 1.8f, springDamping = 0.35f; // +0xA4 / +0xA8, times each wheel's +0x18 (0x792900)
    float forceRaise = 0.7f;        // +0x28: wheel forces act 0.7 x (attach -> contact) above the contact (0x792900)
    struct Wheel {                  // vehicle data +0x90 list (0x28 bytes) + the model's HInfo
        float rest = 0;             // +0x00 suspension ray beyond the radius (spring rest), units
        float friction = 0;         // +0x08 [I: the tyres' friction peak]
        float brake = 0;            // +0x0C brake friction [I: units/frame^2 per mass share]
        float drive = 0;            // +0x14 share of the engine torque
        float springScale = 1;      // +0x18 spring frequency factor
        uint32_t flags = 0;         // +0x1C bit 0 steered
        uint8_t sphere = 0;         // +0x22
        glm::vec3 attach{0};        // sphere (+0x22 = its head[3]) x, y at marker (+0x23) z, model space
        float radius = 0, mass = 0; // the sphere's
    };
    std::vector<Wheel> wheels;
    std::shared_ptr<const physics::ObjectPhysics> body; // motorcycle.drm model 0: chassis primitives and mass
    physics::VehicleSpec vehicle;   // the above for physics::Scene (wheels, engine; vehicle.physics = body)
    // Fall damage (tru.exe 0x793680): drop below the take-off height (units) and health damage (kDamageAbsolute).
    float fallMin = 3792.67f, fallMid = 4819.85f, fallMax = 4977.88f; // +0x190 / +0x194 / +0x198
    float damageA = 5, damageB = 100, damageLethal = 1650;           // +0x184 / +0x188 / +0x18C
    glm::vec3 frontAxle{0, -206.6f, 104.1f}, rearAxle{0, 256.8f, 98.8f}; // model space, bones 5 / 8 of the skeleton
    float wheelRadius = 104.0f;     // axle height in the bind pose
    // [I] (no hop in the record or 0x790E40's states):
    float hopSpeed = 1100;                           // jump: vertical velocity added, units/s
    float mountReach = 250, dismountSpeed = 200;     // units, units/s
    float ramSpeed = 800, ramDamage = 0.5f;          // units/s; health per unit/s of speed
    float wheelbase() const { return frontAxle.y < rearAxle.y ? rearAxle.y - frontAxle.y : frontAxle.y - rearAxle.y; }
};
// Reads the default tuning record and the axles of pc-w\motorcycle.drm. Throws cdc::FormatError.
BikeTuning loadBikeTuning(const cdc::Drm& motorcycle);
// Lara's riding clips (motorcycle.drm animation list); AnimLibrary keeps them as "motorcycle:<name>".
extern const std::vector<std::string_view> kBikeClipNames;

enum class BikePhase : uint8_t { Parked, Mount, Ride, Dismount };
std::string_view bikePhaseName(BikePhase p);

struct Bike {
    int32_t uid = 0;
    std::string unit;     // unit whose frame `pos` is in (the intro's, then wherever it was last ridden)
    glm::vec3 pos{0};     // ground point under the model origin, `unit` frame
    float yaw = 0, pitch = 0; // heading (model -Y forward), nose-up pitch
    float speed = 0;      // along the heading, units/s; < 0 reversing
    float steer = 0;      // front wheel angle, rad (+ = left)
    float wheel = 0;      // wheel spin, rad
    glm::vec3 vel{0};     // body velocity, units/s
    bool air = false;     // no wheel on the ground
    float airStartZ = 0;  // world z at take-off (fall damage)
    float hopT = -1, landT = -1; // seconds since the hop / the landing, -1 = none
    float brakeT = 0;     // seconds the brakes have been on (BikeTuning::brakeRamp)
    float rpm = 0, throttle = 0; // engine, as of the last step
    physics::VehicleDrive drive; // input for the next physics step
    glm::vec3 kick{0};    // velocity added in the next physics step (hop, ram)
    std::optional<glm::vec3> simWorld; // world origin after the last physics step; nullopt: no Jolt body (parked, new)
    BikePhase phase = BikePhase::Parked;
    double t = 0;         // seconds into the mount / dismount clip
    // previous tick (world frame) for interpolation
    glm::vec3 prevPos{0};
    float prevYaw = 0, prevPitch = 0, prevSteer = 0, prevWheel = 0;
};

// The bike skeleton's local pose: wheels (bones 5, 8) spun, handlebars (bone 3) turned by the interpolated steer.
std::vector<anim::LocalTransform> bikePose(const scene::Skeleton& skeleton, const Bike& b, double alpha);

} // namespace ou::game
