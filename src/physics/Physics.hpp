// Physics objects: the collision primitives and physics class of object DRMs, and the rigid-body scene (Jolt
// Physics: hinge doors, uber-object joints, loose bodies landing on the level) stepped on the fixed game tick.
// Layouts, evidence and what is inferred: docs/formats/PHYSICS.md.
// Frame: Z-up game units; model space = the object's model, world = intro transform * model.
#pragma once

#include "cdc/Cdc.hpp"
#include "collision/Collision.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::physics {

using glm::mat3;
using glm::mat4;
using glm::quat;
using glm::vec3;

// ---- object data --------------------------------------------------------------------------------------
enum class PrimKind : uint8_t { Sphere, Box, Capsule };
// One HInfo collision primitive of a model segment (sphere 0x24, box 0x50, capsule 0x40 bytes).
struct Prim {
    PrimKind kind = PrimKind::Box;
    uint8_t head[4] = {};   // the flag bytes (sphere +0, box +0x30, capsule +0x20); head[1] & 1 = solid [I]
    int segment = 0;        // model segment (bone) whose frame `pos`/`rot` are in
    vec3 pos{0};
    quat rot{1, 0, 0, 0};
    vec3 size{0};           // box: full side lengths (tru.exe box inertia 0x4E9100 uses them as such)
    float radius = 0;       // sphere, capsule
    float length = 0;       // capsule, along its local Z [I]
    uint16_t id = 0, mass = 0;
    // What touching it costs (sphere +0x1C, box +0x3E, capsule +0x32, s16; tru.exe 0x48E040 read by the instance-contact
    // handler 0x4923C0): 0 on almost every primitive, the traps' blades / flames / grinders carry it.
    int16_t damage = 0;
    bool solid() const { return head[1] & 1; }
};
// Model marker (dtp Model +0x38 count, +0x3C list, 0x30 bytes): frame of e.g. a joint [I].
struct Marker {
    vec3 pos{0}, rot{0}; // model space; Euler radians, R = Rx * Ry * Rz
    uint32_t kind = 0;   // +0x20; 2 on ca_rb_wall_hpole_door ([I] its joint type: 2 = cylindrical in tru.exe)
    int32_t link = -1;   // +0x24
};
// HInfo slot-2 marker (0x20 bytes): an effect / attach point in its segment's frame, found by id (tru.exe 0x46DA20;
// effect definitions name it with a negative segment, -id).
struct HMarker {
    int segment = 0;
    uint16_t id = 0;
    vec3 pos{0}, rot{0}; // segment frame; Euler radians, R = Rx * Ry * Rz [I] (cdc_Matrix_Build_XYZOrder)
};
// Engagement record (0x8C bytes; tru.exe [Engagement] 0x6F3C60 tested by Lara's scan 0x719520): where Lara can interact
// with an object. The instance answers query 0x53 with its active set {+0 id, +4 -> records, +8 count}.
struct Engagement {
    int kind = 0;       // +0: 0 off, 4 pick up (portables), 5 insert into (receptacles), 8 use (switches)
    int segment = -1;   // +4: frame = that segment's, -1 the instance's; also the insert's script argument (vt 49)
    int shape = 0;      // +0x10: 0 sphere, 1 cylinder (centre = middle), 2 cylinder + facing cone (0x6F3A50)
    vec3 centre{0};     // +0x14, units (the data's centimetres x 3.1605)
    float radius = 0, height = 0; // +0x20 / +0x24, units
    float heading = 0, half = 0;  // +0x28 / +0x2C, radians: the cone about the frame's Z, measured from +X
    std::vector<uint16_t> needs;  // +0x6C -> u16 object ids, +0x70 count: Lara must hold one of them (0x6F3BB0)
};
struct EngagementSet {
    int32_t id = -1; // selectable by instance message 0x1B1 (scripts' UseEngagementProperties) when 0 < id < 10000
    std::vector<Engagement> records;
};
struct ObjectPhysics {
    uint32_t oflags = 0;   // ObjectBase +0; bit 0x80000000 = "physical" object family (tru.exe 0x5BF0E0)
    int physClass = -1;    // physical data class (ObjectBase+0xC8: u16 +2 when u16 +4 == 0xB00B), 34 = rigid body
    // A rigid body's joint limit (physical data +0x2C -> {+4 lo, +8 hi}): prismatic units, hinge degrees.
    struct Limit { float lo = 0, hi = 0; };
    std::optional<Limit> jointLimit;
    // The rigid body joint's sound (record +0x64 SoundPlex*; +0x61 where it plays: 0 the pivot, 1 on the body, 2 between;
    // tru.exe 0x6B6440 -> 0x4F2B00, docs/formats/AUDIO.md §9.16) and whether its limit is on (+0x34 bit 0).
    std::optional<cdc::Pointer> jointSound;
    uint8_t jointSoundAt = 0;
    bool jointLimitOn = false;
    std::vector<mat4> segments; // bind-pose model-space frame of each segment (pivots accumulated down the parents)
    std::vector<Prim> prims;
    std::vector<Marker> markers;
    std::vector<HMarker> hmarkers;
    vec3 boxMin{0}, boxMax{0}; // segment 0's box (dtp segment +0 / +0x10): the model's bounds (tru.exe 0x696B00)
    // Engagement sets: physical record +0x68 (+0x6C count) of anim-graph objects, +0xA8 (+0xAC) of portables (class 50).
    std::vector<EngagementSet> engagements;
    // Receptacles: the model marker a portable attaches at (record +0x3C -> byte; query 0x173 -> 0x6B55D0), -1 none.
    int attachMarker = -1;
    // The rigid body joint's motor (record +0x1C speed per second, +0x20 bound, <= 0 unbounded, +0x34 bit 3 on; tru.exe
    // 0x6B6440 -> 0x4DA160 / 0x4DA190 / 0x4DA0C0), as the uber joints' (formats::UberJoint): ms_rb_press_plate 40 / 400,
    // ms_rb_tomb_lock 0 / 1000 (its script sets the speed, multibody 89).
    struct Motor { float speed = 0, bound = 0; bool on = false; };
    Motor jointMotor;
    // Traversal markup of the model (model DTP +0x54 count, +0x58 -> 0x5C-byte items: +0x28 flags (0 = from the type),
    // +0x2E id, +0x33 type, +0x58 -> polyline {u32 n, pad, float4 points[n]}), points in model space. tru.exe adds them
    // per instance (0x5D0610 -> 0x5CE4F0 with the instance as the markup box's owner, box +0x0C) when the instance's
    // current model has any (0x4D78D0, 0x5BA480, 0x5BA650, 0x5D07E0): push blocks' top-edge ledges, poles, cog rings.
    std::vector<collision::Markup> markup;
    // Item +0x30 (s16): the model segment each item rides. tru.exe places a markup box by its owner's matrix of that
    // segment (0x5C41A0: instance +0x90 + 0x40 * item[+0x30]) and takes the velocity Lara inherits on it from the same
    // matrix now and one frame ago (0x7027B0 -> 0x5D75B0). Parallel to `markup`.
    std::vector<int16_t> markupSegment;
    // Target features of the model (model DTP +0x5C count, +0x60 -> 0x20-byte records {u16 segment, u16 mask, float3
    // position in that segment's frame}; tru.exe's instance queries 0xDE / 0xDF / 0x14C walk them per model). Mask bit
    // kFeatureGrapple = a grapple point (every `*_ei_grapple_*` anchor has one, and so do the manor's ring door, the
    // tomb gate, the snake heads, ...: survey docs/GAMEPLAY.md "Grapple"); enemies carry 0x41 (lock-on points).
    struct Feature { int segment = 0; uint16_t mask = 0; vec3 pos{0}; };
    std::vector<Feature> features;
    // Bind-pose model-space frame of a segment field (tru.exe 0x615930): >= 0 a segment, < 0 the HInfo marker -id
    // (0x46DA20) in its segment's frame; nullopt when there is no such segment / marker.
    std::optional<mat4> frame(int field) const;
};
inline constexpr int kClassRigidBody = 34; // tru.exe 0x6BA570 (0x6D00E0 dispatch); 28 objects, the "*_rb_*" ones
inline constexpr uint16_t kFeatureGrapple = 0x4; // ObjectPhysics::Feature::mask
// Model `model` (ObjectBase +0xA4 list) of an object DRM. Empty without an object section or that model; throws
// cdc::FormatError on bad layouts.
ObjectPhysics loadObjectPhysics(const cdc::Drm& object, uint32_t model = 0);
// The engagement set at `offset` in the object DRM's object section (an uber section's: formats::UberSection /
// UberAction::engagement); nullopt when it lies outside.
std::optional<EngagementSet> loadEngagementSet(const cdc::Drm& object, uint32_t offset);
// Face mask of object collision shapes: our own convention, outside the terrain bits (l4_pool terrain faces are 3) [I];
// mask-0x0F queries see terrain only. Objects the gameplay camera collides with (ObjectBase oflags bit 0x02, tru.exe
// 0x5E94C0; 101 of the 335 solid objects placed in units) also carry kCameraObjectMask (collision::kCameraMask); those
// that block the targeting line of sight (oflags bit 0x08, tru.exe 0x713090) carry kSightObjectMask
// (collision::kSightMask).
inline constexpr uint8_t kObjectMask = 0x10, kCameraObjectMask = 0x20, kSightObjectMask = 0x40;
// Solid primitives of `segment` as one collision shape (faces masked kObjectMask, plus kCameraObjectMask for camera
// colliders and kSightObjectMask for sight blockers) in that segment's frame; nullptr if it has none. Spheres and
// capsules become their bounding boxes.
std::shared_ptr<const collision::CollisionMesh> solidShape(const ObjectPhysics& o, int segment);

// ---- rigid bodies -----------------------------------------------------------------------------------------
struct MassProps {
    float mass = 0;
    vec3 com{0};         // model space
    mat3 inertia{1};     // about com, model axes
};
// Sum of the primitives' masses and inertias with tru.exe's per-shape formulas (box 0x4E9100, sphere 0x4DC770,
// capsule 0x4E9230), shifted to the common centre of mass.
MassProps massProps(const ObjectPhysics& o);

// Jolt simulates in metres: game units / kUnitsPerMetre (tru.exe's bike speed unit, 280 units per metre [O], Vehicle.hpp).
inline constexpr float kUnitsPerMetre = 280.0f;

// A hinge body as of the last step (read-only mirror of the Jolt body).
struct Body {
    float invMass = 0;
    mat3 invInertia{0};   // body (model) axes
    vec3 com{0};          // model space
    vec3 pos{0};          // world centre of mass
    quat rot{1, 0, 0, 0}; // model -> world rotation
    vec3 vel{0}, angVel{0};
    mat4 model() const;   // model -> world
};

// Revolute joint between a body and the world: anchor and axis fixed in both. The motor turns the body toward
// `target` at up to `maxSpeed` rad/s, landing on it in the step that reaches it, with at most `maxTorque`
// (mass * units^2 / s^2) [I].
struct Hinge {
    int body = 0;
    vec3 localAnchor{0}, localAxis{1, 0, 0}; // model space
    vec3 worldAnchor{0}, worldAxis{1, 0, 0}; // where the body is at angle 0
    float lo = -3.14159265f, hi = 3.14159265f; // angle limits, rad
    std::optional<float> target;
    float maxSpeed = 1.0f, maxTorque = 0;
};

// One-degree-of-freedom joint of a body against a fixed frame (tru.exe uber-object joints, 0x4D8000; docs/formats/
// PHYSICS.md "Uber joints"), simulated by Scene as a Jolt slider / hinge. Units are tru.exe's: per 30 Hz frame.
// Prismatic: x in units along `axis`; hinge: x in radians about `axis` through the pivot (not wrapped), `arm` = pivot ->
// centre of mass at x = 0.
struct Joint1 {
    bool hinge = false;
    float x = 0, v = 0;         // coordinate, its velocity per frame
    float mass = 0;             // body mass
    float inertia = 0;          // hinge: about the axis through the pivot
    vec3 axis{0, 0, 1}, arm{0}; // world
    vec3 pivot{0};              // world origin of the joint frame (where the body is posed from)
    float lo = 0, hi = 0;       // limit (x units), when `limited`
    bool limited = false;
    float speed = 0, force = 0; // motor: target velocity per frame, impulse bound per 1/60 s step (<= 0: unbounded)
    bool motor = false;
    float push = 0;             // next step only: velocity per frame imposed from outside (Lara pushing), over gravity and motors
    float pushBound = 0;        // the push's impulse bound per 1/60 s step, as `force` (<= 0: unbounded)
};
// Three-axis translational joint of a body against the world (tru.exe engine joint 10, 0x4E0F40: uber joint type 7,
// the Thor pillars; docs/formats/PHYSICS.md "Uber joints"): the body keeps its rotation and moves along the world's
// X / Y / Z, each axis with its own limit and motor. Simulated by Scene as a translation-only Jolt body with the
// object's collision (it rests on the terrain and the colliders under gravity) on a SixDOF constraint.
// Units as Joint1's: positions in units from where it was made, velocities per 30 Hz frame.
struct Joint3 {
    vec3 x{0}, v{0};
    float mass = 0;                         // 0: the collision primitives'
    const ObjectPhysics* physics = nullptr; // its collision
    mat4 model{1};                          // model -> world at x = 0
    float lo[3] = {}, hi[3] = {};
    bool limited[3] = {};
    float speed[3] = {}, force[3] = {};     // motors: target velocity per frame, impulse bound per 1/60 s step (<= 0: unbounded)
    bool motor[3] = {};
    float friction = 0;                     // joint friction per axis, as a motor bound [I units]
    vec3 push{0};                           // next step only: velocity per frame imposed on X / Y (Lara pushing)
    float pushBound = 0;                    // its impulse bound per 1/60 s step (<= 0: unbounded)
};
// Gravity, units per 30 Hz frame^2 (Lara's, tru.exe 5.72443).
inline constexpr float kJointGravity = 5.72443f;

// A raycast-wheel vehicle (tru.exe Vehicle / Bike: one Havok body, a spring per wheel along a down ray from its
// attachment, 0x792900; docs/GAMEPLAY.md "Motorcycle"), simulated by Scene as a Jolt VehicleConstraint with a
// WheeledVehicleController for the wheels (springs, friction, brakes) and tru.exe's engine as wheel forces, kept
// upright. Model space and game units; kg; torques in N m.
struct VehicleSpec {
    const ObjectPhysics* physics = nullptr; // chassis shape and mass: its primitives
    struct Wheel {
        vec3 attach{0};          // top of the suspension, model space
        float travel = 0;        // ray length below `attach` to the wheel centre at full extension (spring rest)
        float radius = 0, width = 0, mass = 0; // mass: its inertia (solid disc) about the axle
        float frequency = 0, damping = 0; // spring, Hz / damping ratio
        float drive = 0;         // share of the engine torque
        float brake = 0;         // N m at full brake
        float handBrake = 0;     // N m at full hand brake
        float steer = 0;         // max steering angle, rad (0 = fixed)
        float raise = 0;         // the wheel's forces act this fraction of the travel above the contact (roll centre)
        float friction = 0;      // tyre friction peak (Jolt's curves scaled to it; 0 = Jolt's 1.2)
    };
    std::vector<Wheel> wheels;
    float maxTorque = 0, minRpm = 0, maxRpm = 0; // N m, rpm
    std::vector<float> torque; // engine torque / maxTorque at rpm i * maxRpm / (size - 1)
    float ratio = 0;           // engine / wheel angular velocity, forward gear
    float reverseRatio = 0, reverseTorque = 0; // the same in reverse (positive); its torque, N m (no curve)
    float maxPitchRoll = 0;    // tilt limit from the world's up, rad
    float leanRate = 3;        // roll servo: roll rate = leanRate x (VehicleDrive::lean - roll), 1/s
};
// Driver input for one step: forward 0..1 (reverse: in the reverse gear), right -1..1 (of each wheel's max steer),
// brake / handBrake 0..1; lean: the roll the servo holds, rad (+ = the left side down; the roll is from the world's up);
// yawTorque (N m about the body's up, + turns left) and sideForce (N along the body's left) at the centre of mass;
// push: force (N, model axes) at wheel pushWheel's contact (its attachment when off the ground); pinWheel: that wheel's
// contact point is held still horizontally (the body's velocity is corrected before the step); maxYawRate (rad/s, 0 =
// none): the spin about the world's up is clamped to it before the step. rpm (> 0): the engine torque is read at this
// rpm instead of the wheels' (still none once the wheels pass the top rpm); torqueScale multiplies it.
struct VehicleDrive {
    float forward = 0, right = 0, brake = 0, handBrake = 0;
    bool reverse = false;
    float lean = 0, yawTorque = 0, sideForce = 0, maxYawRate = 0;
    int pushWheel = -1, pinWheel = -1;
    vec3 push{0};
    float rpm = 0, torqueScale = 1;
};
struct VehicleState {
    mat4 model{1};       // model -> world
    vec3 vel{0};         // units/s
    uint32_t contacts = 0; // bit per wheel touching the ground
    float wheelAngle = 0;  // last wheel's spin, rad
    float rpm = 0;
};

// Ragdoll of an object (ObjectBase +0x78 flag, +0x7C -> record; tru.exe 0x4F1920 builds it, 0x4F1390 a bone,
// 0x4F0B40 its Havok body and joint; docs/formats/PHYSICS.md "Ragdolls"). One capsule body per bone; offsets and axes
// in the model's (rotation-free) bind frame [I].
struct RagdollBone {
    int segment = 0, endSegment = 0;   // +4 (+6 when >= 0): capsule start; +0x14 (else the start's): its end
    vec3 startOffset{0}, endOffset{0}; // +8, +0x18
    int parent = -1;                   // +0x24: ragdoll bone it hangs from
    float massShare = 0;               // +0x28: % of RagdollDef::mass
    float radius = 0;                  // +0x2C x RagdollDef::radiusScale
    int joint = 0;                     // +0x38: 0 none (the root), 1 hinge, 2 two axes, 3 cone + twist
    vec3 pivot{0}, axis1{0}, axis2{0}; // +0x3C (from the capsule start), +0x48, +0x54
    float limits[5] = {};              // +0x60..+0x70 degrees: axis 1 lo, hi; axis 2 lo, hi; cone
    float friction = 0;                // +0x74 (at least 0.01)
};
struct RagdollDef {
    float mass = 0, radiusScale = 1; // record +4 (kg), +8
    std::vector<RagdollBone> bones;
};
// The object's ragdoll; nullopt when it has none. Throws cdc::FormatError on bad layouts.
std::optional<RagdollDef> loadRagdoll(const cdc::Drm& object);

class Scene {
public:
    Scene();
    ~Scene();
    Scene(Scene&&) noexcept;
    Scene& operator=(Scene&&) noexcept;

    std::vector<Body> bodies;   // hinge bodies, as of the last step
    std::vector<Hinge> hinges;  // limits and motor read by step()
    int addBody(const MassProps& m, const mat4& model);
    // Hinge at model-space `anchor` about model-space `axis`, angle 0 = the body's current pose.
    int addHinge(int body, vec3 anchor, vec3 axis, float lo, float hi);
    float angle(int hinge) const;      // rad, signed about worldAxis
    void setAngle(int hinge, float a); // places the body at `a`, at rest (load, streaming)
    void clearHinges();                // drops bodies and hinges; joints, loose bodies and solids stay

    // Uber joints for the next step, keyed by the caller. A key not passed is dropped; a new one, or one whose x / v
    // changed outside the simulation (a load, a re-creation, a shot), is placed at x moving at v. step() writes x and v
    // back (the pointers are used by that step only). Links: follower = ratio * driver in game units (hinge radians).
    struct JointLink { uint64_t driver = 0, follower = 0; float ratio = 0; };
    void setJoints(std::vector<std::pair<uint64_t, Joint1*>> joints, std::vector<JointLink> links);

    // Three-axis joints for the next step, keyed by the caller as setJoints' (a key not passed is dropped; a new one, or
    // one whose x / v changed outside the simulation, is placed there). step() writes x and v back and clears the push.
    void setJoints3(std::vector<std::pair<uint64_t, Joint3*>> joints);

    // Loose rigid bodies (shape, mass and inertia from the object's primitives), keyed by the caller. A new key starts
    // at `model` (model -> world) moving at `vel` (units/s) / `angVel` (rad/s, world); existing ones keep simulating
    // (`model` and the velocities are ignored); missing keys are dropped.
    // `rail`: the body still rides that joint (setJoints key): the joint's body takes its shape and collides, posed by
    // the joint (`model` = the object's pose at joint coordinate 0); loose(id) is empty then.
    struct Loose { int32_t id = 0; const ObjectPhysics* physics = nullptr; mat4 model{1}; uint64_t rail = 0; vec3 vel{0}, angVel{0}; };
    void setLoose(std::span<const Loose> loose);
    std::optional<mat4> loose(int32_t id) const; // model -> world as of the last step
    // What loose bodies land on: resident unit terrain (static meshes, built once a loose body exists) and the moving
    // object collision (kinematic, following each Dynamic::transform; pass without the loose bodies' own).
    struct Terrain { std::string name; const collision::CollisionMesh* mesh = nullptr; vec3 offset{0}; };
    void setTerrain(std::span<const Terrain> units);
    void setColliders(std::span<const collision::Dynamic> dynamics);
    // Pressure plates: each plate's joint (setJoints key, a prismatic joint whose motor lifts it) is built with `meshes`
    // (world transforms at coordinate 0) as its collision; it collides with loose bodies, vehicles and ragdolls (not
    // the terrain it sits in). Kinematic weights (Lara, push blocks) press `mass` (kg) under gravity onto the plate a
    // ray finds below `at` (from 40 above to 30 below); loose bodies weigh through their contacts. Call before setJoints.
    struct Plate { uint64_t joint = 0; std::span<const collision::Dynamic> meshes; };
    struct Weight { vec3 at{0}; float mass = 0; };
    void setPlates(std::span<const Plate> plates, std::span<const Weight> weights);
    // Lara's proxy (null: none): a `mass` kg capsule standing on `feet`, velocity-driven to follow her each step
    // (translation only, no gravity), so walking into loose bodies and ragdolls shoves them by mass. It exists only
    // while there are loose bodies or ragdolls, and touches nothing else.
    struct Character { vec3 feet{0}; float radius = 0, height = 0, mass = 0; };
    void setCharacter(const Character* c);
    // Water for the next steps (tru.exe 0x6AD070 per water volume): a yaw-rotated box (centre, half extents, (cos, sin)
    // of its yaw) with its surface z; `flow` (user data, point) -> the current's velocity in units/s, null = still.
    // Loose bodies in it get buoyancy at water density 1000 kg/m^3 and drag 0.5 / 0.01 (0x69D350's defaults).
    struct Water {
        vec3 centre{0}, half{0};
        glm::vec2 yaw{1, 0};
        float surface = 0;
        vec3 (*flow)(const void* user, vec3 p) = nullptr;
        const void* user = nullptr;
    };
    void setWater(std::span<const Water> water);

    // Explosion: velocity change `near` .. `edge` (units/s, linear falloff to `radius`) away from `at` on hinge, loose
    // and ragdoll bodies, along the line from `at` raised by half the distance; `elevation` >= 0 (degrees): along the
    // body's heading from `at` at that elevation (Physics.cpp).
    void blast(vec3 at, float radius, float near, float edge, float elevation = -1);
    // Vehicles, keyed by the caller. A new key starts at `model` (model -> world) at rest; existing ones keep simulating
    // with this step's `drive`, moved by `shift` (the caller's frame moved) and given `kick` (units/s, a hop or a hit);
    // missing keys are dropped. Their terrain is setTerrain's and setColliders'.
    struct Vehicle { int32_t id = 0; const VehicleSpec* spec = nullptr; mat4 model{1}; VehicleDrive drive; vec3 shift{0}, kick{0}; };
    void setVehicles(std::span<const Vehicle> vehicles);
    std::optional<VehicleState> vehicle(int32_t id) const; // as of the last step
    // Ragdolls, keyed by the caller (missing keys are dropped). A new key is built from `def` and `bind` (each skeleton
    // segment's bind position, model space) and starts at `frames` (world frame of each bone's start segment) moving at
    // `velocity` (units/s per bone), with `impulse` (mass * units/s) at `hitPoint` on the nearest bone; existing keys
    // keep simulating (the spans are ignored). They collide with the terrain, the colliders and loose bodies.
    struct Ragdoll {
        int32_t id = 0;
        const RagdollDef* def = nullptr;
        std::span<const vec3> bind;
        std::span<const mat4> frames;
        std::span<const vec3> velocity;
        vec3 hitPoint{0}, impulse{0};
    };
    void setRagdolls(std::span<const Ragdoll> ragdolls);
    // World frame of each bone's start segment as of the last step; asleep: every body of it sleeps.
    struct RagdollPose { std::vector<mat4> frames; bool asleep = false; };
    std::optional<RagdollPose> ragdoll(int32_t id) const;
    void step(float dt); // one fixed tick
    // The scene's state for a save: body IDs by entity, constraint zeros and Jolt's recorded state (bodies, contact
    // cache, constraints). restore() drops the scene; as the caller re-creates the same entities they get the saved IDs
    // and zeros, and the next step() applies the recorded state, so the run continues bit for bit. false: not a state
    // this version wrote (the rebuilt scene stands); a scene rebuilt differently skips the recorded state.
    std::string save() const;
    bool restore(std::string_view state);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ou::physics
