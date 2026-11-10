// Lara's limb locks (feet on what she stands on, hands on the ledge she hangs from) and her head look-at: the pose
// passes World::updatePose runs after the clips and the weapon layers. Visual only: the controller, collision and root
// motion never read them. Each is a pure function of the pose and the world it is given (no state of its own), so the
// same game state shows the same pose (replays, saves). The original's rules and where ours differs:
// docs/GAMEPLAY.md "Animation layers and IK".
#pragma once

#include "anim/Anim.hpp"
#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

namespace ou::game {

// One limb lock as tru.exe's cdc::GenericLimbLockController (constructor 0x445160, record parser 0x443C60) reads it
// from the player object data: lara.drm object data +0x5C8 u16 count (30), +0x5CC records of 0x4C bytes [O].
struct LimbLock {
    uint32_t chain = 0;     // +0x00 IK chain of the instance's IK system (instance +0x2E0, 0x120-byte chains)
    uint32_t type = 0;      // +0x04 target callback (controller +0x14C, chosen at 0x6EFDA0): 1 the floor under a foot
                            //       (0x73E6F0), 7 the ledge under a hand (0x737790), 9 the bike's grips and pegs
                            //       (0x741490), 0xB wading (0x7406E0), 5 / 6 / 8 / 10 others, 0 / 2 none
    uint32_t group = 0;     // +0x08
    uint16_t limb = 0;      // +0x0C 0x30 / 0x31 left / right foot, 0x40 / 0x41 left / right hand (0x6F1A60)
    uint16_t limb2 = 0;     // +0x0E
    float up = 0, down = 0; // +0x14 / +0x18 the probe: from `up` over the effector to `down` under it (0x438F60)
    glm::vec3 offset{0};    // +0x1C; z = how far off the hit the effector is held, along the hit's normal
    uint32_t framesIn = 2, framesOut = 3; // +0x28 / +0x2C blend in / out in 30 Hz frames (controller +0x10E / +0x10F)
    int16_t ignoreSegment = 0, segment = 0; // +0x34 / +0x36: the effector segment when > 0 (else the chain's last)
    uint32_t flags = 0;     // +0x40: 1 and 2 = on from the start (0x443C60), 8 = object hits count, 0x10 = the
                            //        effector turns to the hit's normal (0x443B50)
};

struct IkTuning {
    // Records 13 / 15 (the floor under the left / right foot) and 4 / 7 (the ledge under the left / right hand) of
    // every outfit's lara.drm; loadIkTuning replaces the list with the file's 30.
    std::vector<LimbLock> locks{
        {12, 1, 0, 0x30, 0, 158.025f, 158.025f, {0, 0, 26.864f}, 2, 3, 6, 6, 0x1F},
        {13, 1, 0, 0x31, 0, 158.025f, 158.025f, {0, 0, 26.864f}, 2, 3, 11, 11, 0x1F},
        {14, 7, 0, 0x40, 0x42, 0, 0, {0, 0, 0}, 2, 3, 0, 0, 1},
        {15, 7, 0, 0x41, 0x43, 0, 0, {0, 0, 0}, 2, 3, 0, 0, 1},
    };
    // Effector bones. Feet: the foot records' +0x36 [O]. Hands: the last arm segment before the fingers, the bones
    // the hang pose is fitted by (AnimLibrary::hangHandOffset) [I: the IK chains' own effector segments are not decoded].
    std::array<int, 2> foot{6, 11}, hand{55, 80}; // left, right
    int pelvis = 1;                // object data +0x168 [O]
    // Head look-at (0x6FD080) [O]: head / neck segments +0x1B0 / +0x1A8; the head turns at most +0xCD4 degrees aside
    // and +0xCD8 up / down of the neck; a change over +0xB68 degrees restarts the +0xB6C-frame interpolation; a
    // scripted look-at point is sighted from +0xCC8 units short of it.
    int head = 17, neck = 16;
    float lookYaw = glm::radians(60.0f), lookPitch = glm::radians(15.0f);
    float lookRestart = glm::radians(30.0f), lookFrames = 3, lookSightGap = 60;
    float ledgeEndGap = 20.0f;     // 0x737790: a hand comes no nearer than this to the end of the ledge's polyline [O]
    const LimbLock* lock(uint32_t type, uint16_t limb) const; // the first record of that type for that limb
};
// From lara.drm's object data; nullopt when the file or the data is missing.
std::optional<IkTuning> loadIkTuning(const cdc::BigfileSet& bigfile);

// What a pass did, for tests, the self-test probes and the HUD-less debugging of poses (world space).
struct FootPlant {
    std::array<bool, 2> planted{};      // the probe found a floor for the foot
    std::array<glm::vec3, 2> ground{};  // where
    std::array<float, 2> error{};       // the ankle's height after the pass minus the height the pass wanted for it
    float pelvis = 0;                   // how far the pelvis was lowered
};
// Feet on the floor under them. `toWorld`: Lara's model matrix; the root plane is the horizontal plane through its
// origin (her feet on level ground). Each foot is probed from `up` over its ankle to `down` under it (the record's
// numbers); a floor steeper than `minNormalZ` or no floor leaves the foot to the clip. The ankle keeps the clip's
// height over the root plane, now over the floor found; the pelvis comes down by what the lower foot needs; the foot
// turns with the floor (flag 0x10). A foot the clip has lifted is held less: fully while the floor is within the
// nearer half of the probe under its ankle, not at all at the probe's end. `weight` 0..1 scales it all.
FootPlant plantFeet(const scene::Skeleton& skeleton, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
                    const glm::mat4& toWorld, const collision::CollisionMesh& col, const IkTuning& tuning,
                    float minNormalZ, float weight = 1.0f);

// Hands on the ledge. `line`: the ledge's polyline (world). Each hand goes to the point of the polyline nearest to
// it (at least ledgeEndGap from the polyline's ends when it is that long), keeping where the clip has it along the
// ledge. Returns each hand's distance from the polyline before the pass.
std::array<float, 2> lockHands(const scene::Skeleton& skeleton, std::span<anim::LocalTransform> local,
                               std::span<glm::mat4> world, const glm::mat4& toWorld, std::span<const glm::vec3> line,
                               const IkTuning& tuning, float weight = 1.0f);

// Head towards `target` (world), within the yaw / pitch limits about the neck's facing. Returns the yaw (+ = her
// left) and pitch (+ = up) the head was given relative to the neck, radians, before `weight`.
glm::vec2 lookAt(const scene::Skeleton& skeleton, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
                 const glm::mat4& toWorld, glm::vec3 target, const IkTuning& tuning, float weight = 1.0f);

// One arm to another target: the arm ending in `hand` is swung at its shoulder by the turn from `from` to `to` as the
// shoulder sees them (world points), at most `maxAngle`. For the second target of a two-gun weapon: the weapon layer
// has both arms on the primary target (`from`), the left then goes over to the second (`to`). Returns the angle turned.
float aimArm(const scene::Skeleton& skeleton, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
             const glm::mat4& toWorld, int hand, glm::vec3 from, glm::vec3 to, float maxAngle, float weight = 1.0f);

} // namespace ou::game
