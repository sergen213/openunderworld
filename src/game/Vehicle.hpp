// Lara's motorcycle (objectlist `motorcycle`, tru.exe classes Bike : Vehicle, VehicleCamera): data types; the
// World side (mount / ride / dismount, terrain following, ramming, saves) is in Vehicle.cpp. Evidence, numbers and
// what is inferred: docs/GAMEPLAY.md "Motorcycle".
#pragma once

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"

#include <glm/glm.hpp>

#include <cstdint>
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

// Bike tuning. [V-data] motorcycle.drm vehicle data (ObjectBase +0xC8: u16 +4 0xB00B, class 0x40, +8 = 1 Bike) and its
// default tuning record (+0x10 -> 0x20C bytes, tru.exe Bike +0x4C4); the skeleton's axle bones. The rest is [I].
struct BikeTuning {
    float maxSpeed = 5008.0f;       // record +0x00 40 mph -> units/s (280 units = 1 m, tru.exe 0x105B30C)
    float steerFraction = 0.3f;     // +0x1C: the steering limits blend over [steerFraction, 1] * maxSpeed
    float steerLow = 0.5236f, steerHigh = 0.1745f;       // +0x10 / +0x14: front wheel limit, 30 / 10 degrees
    float steerRateLow = 1.658f, steerRateHigh = 1.309f; // +0x18 / +0x1B4: 95 / 75 degrees/s
    // Fall damage (tru.exe 0x793680): drop below the take-off height (units) and health damage (kDamageAbsolute).
    float fallMin = 3792.67f, fallMid = 4819.85f, fallMax = 4977.88f; // +0x190 / +0x194 / +0x198
    float damageA = 5, damageB = 100, damageLethal = 1650;           // +0x184 / +0x188 / +0x18C
    glm::vec3 frontAxle{0, -206.6f, 104.1f}, rearAxle{0, 256.8f, 98.8f}; // model space, bones 5 / 8 of the skeleton
    float wheelRadius = 104.0f;     // axle height in the bind pose
    // [I] (the original drives a Havok vehicle with a gear/torque model, not reproduced):
    float accel = 2400, brake = 5000, coast = 600;   // units/s^2
    float reverseSpeed = 700, reverseAccel = 900;    // units/s, units/s^2
    float hopSpeed = 1100;                           // jump: vertical take-off speed, units/s
    float gravity = 5.72443f * 900.0f;               // Lara's (tru.exe 5.72443 units per 30 Hz frame^2)
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
    glm::vec3 vel{0};     // airborne velocity
    bool air = false;
    float airStartZ = 0;  // world z at take-off (fall damage)
    float hopT = -1, landT = -1; // seconds since the hop / the landing, -1 = none
    BikePhase phase = BikePhase::Parked;
    double t = 0;         // seconds into the mount / dismount clip
    // previous tick (world frame) for interpolation
    glm::vec3 prevPos{0};
    float prevYaw = 0, prevPitch = 0, prevSteer = 0, prevWheel = 0;
};

// The bike skeleton's local pose: wheels (bones 5, 8) spun, handlebars (bone 3) turned by the interpolated steer.
std::vector<anim::LocalTransform> bikePose(const scene::Skeleton& skeleton, const Bike& b, double alpha);

} // namespace ou::game
