// Physics joint sounds (tru.exe's joint-sound manager, 0x4F2B00 / 0x4F3540; docs/formats/AUDIO.md §9.16): a joint
// whose record names a SoundPlex keeps one voice while its bodies are awake, fed the joint's speed as Reaction
// parameters, and starts an impact voice when it reaches a limit.
#pragma once

#include "cdc/Cdc.hpp"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace ou::game {

// One joint with a sound whose bodies are awake this tick (0x4F3120 makes a contact for it, 0x4F22C0 fills it).
struct JointSound {
    int32_t uid = 0;       // the object's intro uid
    int32_t joint = -1;    // uber joint (UberObjectData::joints index); -1 the rigid body's own joint (class 34)
    std::string object;    // the object DRM holding `sound`
    cdc::Pointer sound;    // SoundPlex node
    glm::vec3 at{0};       // where it plays (0x4F1E10)
    float linear = 0;      // Reaction parameter 1 (instance +0x2C): speed along the axis, units/s, signed
    float angular = 0;     // parameter 2 (+0x30): speed about the axis, degrees/s, signed
    std::optional<float> impact; // parameter 0 (+0x28): the joint reached a limit this tick at this speed (units/s or
                                 // degrees/s, signed; 0x4E76A0 -> 0x48E1C0 -> 0x4F2E40 -> 0x4F2200)
};

// Limit state of one joint axis (Havok constraint +0xE4: 0 inside, 1 at the lower, 2 at the upper limit; 0x4E76A0).
// The impact fires on the step that finds the joint at a limit after being inside, with the speed it arrived at.
struct LimitHit {
    uint8_t state = 0;
    // `x` the coordinate after the step, `lo` / `hi` the limit, `tolerance` how close counts as at it [I: Jolt's limits
    // stop the body within a little of them], `speed` the speed it moved at before the step.
    std::optional<float> update(float x, float lo, float hi, float tolerance, float speed) {
        if (x > lo + tolerance && x < hi - tolerance) {
            state = 0;
            return std::nullopt;
        }
        if (state) return std::nullopt;
        state = x >= hi - tolerance ? 2 : 1;
        return speed;
    }
};

// Whether a body the game moves without the physics engine (class-34 joints driven by `multibody` motors, press
// plates) is awake: moving, or still for less than `sleepAfter` seconds [I: Jolt's 0.5 s sleep time stands for Havok's
// deactivation timer, body +0x108].
struct WakeTimer {
    float still = 1e9f;
    bool update(bool moving, float dt, float sleepAfter = 0.5f) {
        still = moving ? 0.0f : still + dt;
        return still < sleepAfter;
    }
};

// tru.exe's units for the Reaction inputs from a joint velocity per 30 Hz frame (Joint1::v): prismatic units/s; hinge
// radians -> degrees/s (0x4F22C0 multiplies the angular speed by 57.2958).
inline float jointLinearSpeed(float vPerFrame) { return vPerFrame * 30.0f; }
inline float jointAngularSpeed(float radPerFrame) { return radPerFrame * 30.0f * 57.2957795f; }

} // namespace ou::game
