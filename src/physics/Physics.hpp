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
#include <vector>

namespace ou::physics {

using glm::mat3;
using glm::mat4;
using glm::quat;
using glm::vec3;

// ---- object data --------------------------------------------------------------------------------------
enum class PrimKind : uint8_t { Sphere, Box, Capsule };
// One HInfo collision primitive of a model segment (sphere 0x20, box 0x50, capsule 0x40 bytes).
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
struct ObjectPhysics {
    uint32_t oflags = 0;   // ObjectBase +0; bit 0x80000000 = "physical" object family (tru.exe 0x5BF0E0)
    int physClass = -1;    // physical data class (ObjectBase+0xC8: u16 +2 when u16 +4 == 0xB00B), 34 = rigid body
    std::vector<mat4> segments; // bind-pose model-space frame of each segment (pivots accumulated down the parents)
    std::vector<Prim> prims;
    std::vector<Marker> markers;
    std::vector<HMarker> hmarkers;
    // Bind-pose model-space frame of a segment field (tru.exe 0x615930): >= 0 a segment, < 0 the HInfo marker -id
    // (0x46DA20) in its segment's frame; nullopt when there is no such segment / marker.
    std::optional<mat4> frame(int field) const;
};
inline constexpr int kClassRigidBody = 34; // tru.exe 0x6BA570 (0x6D00E0 dispatch); 28 objects, the "*_rb_*" ones
// Model `model` (ObjectBase +0xA4 list) of an object DRM. Empty without an object section or that model; throws
// cdc::FormatError on bad layouts.
ObjectPhysics loadObjectPhysics(const cdc::Drm& object, uint32_t model = 0);
// Face mask of object collision shapes: our own convention, outside the terrain bits (l4_pool terrain faces are 3) [I];
// mask-0x0F queries see terrain only. Objects the gameplay camera collides with (ObjectBase oflags bit 0x02, tru.exe
// 0x5E94C0; 101 of the 335 solid objects placed in units) also carry kCameraObjectMask (collision::kCameraMask).
inline constexpr uint8_t kObjectMask = 0x10, kCameraObjectMask = 0x20;
// Solid primitives of `segment` as one collision shape (faces masked kObjectMask, plus kCameraObjectMask for camera
// colliders) in that segment's frame; nullptr if it has none. Spheres and capsules become their bounding boxes.
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
};
// Gravity, units per 30 Hz frame^2 (Lara's, tru.exe 5.72443).
inline constexpr float kJointGravity = 5.72443f;

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

    // Loose rigid bodies (shape, mass and inertia from the object's primitives), keyed by the caller. A new key starts
    // at `model` (model -> world) at rest; existing ones keep simulating (`model` is ignored); missing keys are dropped.
    // `rail`: the body still rides that joint (setJoints key): the joint's body takes its shape and collides, posed by
    // the joint (`model` = the object's pose at joint coordinate 0); loose(id) is empty then.
    struct Loose { int32_t id = 0; const ObjectPhysics* physics = nullptr; mat4 model{1}; uint64_t rail = 0; };
    void setLoose(std::span<const Loose> loose);
    std::optional<mat4> loose(int32_t id) const; // model -> world as of the last step
    // What loose bodies land on: resident unit terrain (static meshes, built once a loose body exists) and the moving
    // object collision (kinematic, following each Dynamic::transform; pass without the loose bodies' own).
    struct Terrain { std::string name; const collision::CollisionMesh* mesh = nullptr; vec3 offset{0}; };
    void setTerrain(std::span<const Terrain> units);
    void setColliders(std::span<const collision::Dynamic> dynamics);

    // Explosion: velocity change `near` .. `edge` (units/s, linear falloff to `radius`) away from `at` on hinge and
    // loose bodies.
    void blast(vec3 at, float radius, float near, float edge);
    void step(float dt); // one fixed tick

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ou::physics
