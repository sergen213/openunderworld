// Physics objects: the collision primitives and physics class of object DRMs, and a small deterministic
// rigid-body solver (hinges with limits and motors, kinematic movers) stepped on the fixed game tick.
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
};
inline constexpr int kClassRigidBody = 34; // tru.exe 0x6BA570 (0x6D00E0 dispatch); 28 objects, the "*_rb_*" ones
// Model `model` (ObjectBase +0xA4 list) of an object DRM. Empty without an object section or that model; throws
// cdc::FormatError on bad layouts.
ObjectPhysics loadObjectPhysics(const cdc::Drm& object, uint32_t model = 0);
// Face mask of object collision shapes: our own convention, outside the terrain bits (l4_pool terrain faces are 3) [I];
// mask-0x0F queries see terrain only.
inline constexpr uint8_t kObjectMask = 0x10;
// Solid primitives of `segment` as one collision shape (faces masked kObjectMask) in that segment's frame; nullptr if it
// has none. Spheres and capsules become their bounding boxes.
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

struct Body {
    float invMass = 0;
    mat3 invInertia{0};   // body (model) axes
    vec3 com{0};          // model space
    vec3 pos{0};          // world centre of mass
    quat rot{1, 0, 0, 0}; // model -> world rotation
    vec3 vel{0}, angVel{0};
    mat4 model() const;   // model -> world
};

// Revolute joint between a body and the world: anchor and axis fixed in both.
struct Hinge {
    int body = 0;
    vec3 localAnchor{0}, localAxis{1, 0, 0}, localRef{0, 1, 0}; // model space; ref perpendicular to the axis
    vec3 worldAnchor{0}, worldAxis{1, 0, 0}, worldRef{0, 1, 0}; // where the body is at angle 0
    float lo = -3.14159265f, hi = 3.14159265f; // angle limits, rad
    std::optional<float> target;   // motor drives the angle here (rad), at most `maxSpeed`, with `maxTorque`
    float maxSpeed = 1.0f, gain = 4.0f, maxTorque = 0;
};

// Kinematic mover: follows `points` (world) back and forth at `speed`; not affected by bodies.
struct Mover {
    std::vector<vec3> points;
    float speed = 100; // units/s
    float s = 0;       // distance travelled along the ping-pong path
    vec3 position() const;
};

// One-degree-of-freedom joint of a body against a fixed frame, simulated in its own coordinate (tru.exe uber-object
// joints, 0x4D8000; docs/formats/PHYSICS.md "Uber joints"). Units are tru.exe's: per 30 Hz frame. Prismatic: x in
// units along `axis`; hinge: x in radians about `axis` through the pivot, `arm` = pivot -> centre of mass at x = 0.
struct Joint1 {
    bool hinge = false;
    float x = 0, v = 0;         // coordinate, its velocity per frame
    float mass = 0;             // body mass
    float inertia = 0;          // hinge: about the axis through the pivot
    vec3 axis{0, 0, 1}, arm{0}; // world
    vec3 pivot{0};              // world origin of the joint frame (where the body is posed from)
    float lo = 0, hi = 0;       // limit (x units), when `limited`
    bool limited = false;
    float speed = 0, force = 0; // motor: target velocity per frame, impulse bound per step (<= 0: unbounded)
    bool motor = false;
    float push = 0;             // this step only: velocity per frame imposed from outside (Lara pushing), over gravity and motors
    float effectiveMass() const { return hinge ? inertia : mass; }
    float gravityForce(float x) const; // generalized gravity force (torque) at coordinate x, per frame^2
};
// Gravity on uber joints, units per frame^2 (Lara's, tru.exe 5.72443 at 30 Hz).
inline constexpr float kJointGravity = 5.72443f;
// Advances `j` and the joints linked to it (joint = ratio * j.x) by `frames` 30 Hz frames: gravity, then each motor
// row (impulse clamped to its bound), then the position, clamped to every limit (inelastic). Deterministic.
void stepJoint(Joint1& j, std::span<const std::pair<Joint1*, float>> linked, float frames);

class Scene {
public:
    vec3 gravity{0, 0, -5.72443f * 900.0f}; // tru.exe gravity, 5.72443 units per 30 Hz frame^2
    float angularDamping = 0.5f;            // 1/s [I]
    int iterations = 16;
    std::vector<Body> bodies;
    std::vector<Hinge> hinges;
    std::vector<Mover> movers;

    int addBody(const MassProps& m, const mat4& model);
    // Hinge at model-space `anchor` about model-space `axis`, angle 0 = the body's current pose.
    int addHinge(int body, vec3 anchor, vec3 axis, float lo, float hi);
    float angle(int hinge) const;   // rad, signed about worldAxis
    void setAngle(int hinge, float a); // places the body at `a`, at rest (load, streaming)
    void step(float dt);            // one fixed tick
};

} // namespace ou::physics
