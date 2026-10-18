// Level collision: decoded terrain collision meshes + markup of a TRU unit DRM, our own BVH and the
// queries the character controller needs. Layouts and evidence: docs/formats/COLLISION.md.
// Coordinates: Z-up game units in the render-terrain frame (the frame ou_formats emits terrain in).
#pragma once

#include "cdc/Cdc.hpp"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ou::collision {

using glm::vec3;

struct Face {
    uint32_t v[3] = {};
    uint8_t flags = 0;        // raw +0x0C; bit 0x40 = skipped by tru.exe face queries, rest unknown
    uint8_t mask = 0xFF;      // raw +0x0D; tru.exe ANDs it with the query mask (observed 3 on terrain)
    uint16_t clientFlags = 0; // raw +0x0E; meaning unknown
    uint8_t material = 0;     // raw +0x10; meaning unknown (surface/sound type?)
    uint16_t group = 0;       // terrain group index in the unit
};

// tru.exe skips faces with flags bit 0x40 and faces whose mask shares no bit with the query mask.
inline bool queryable(const Face& f, uint8_t mask) { return !(f.flags & 0x40) && (f.mask & mask); }

// Markup polyline ("markup2" in tru.exe): ledges, landings, balance beams, ...
struct Markup {
    uint8_t type = 0;   // raw type byte; markupFlags/markupTypeName interpret it
    uint16_t id = 0;
    std::vector<vec3> points;
};
// MUD flag bits tru.exe derives from the type (switch 0x5D6F90 | 0x5D70A0).
uint32_t markupFlags(uint8_t type);
std::string_view markupTypeName(uint8_t type); // "ledgegrab", ..., "unknown"

struct Hit {
    float t = 0;      // raycast/closestPoint: distance; sweeps: fraction of `delta` travelled
    vec3 point{};     // contact point on the surface
    vec3 normal{};    // unit, facing the query (ray origin / swept shape)
    uint32_t face = 0; // terrain: index into faces; dynamic: index into that body's shape faces
    int32_t dynamic = -1; // index into CollisionMesh::dynamics, -1 = terrain
};

class CollisionMesh;
// A moving solid queried together with the terrain (doors, push blocks, physics bodies; src/physics): `shape`
// in its own frame, placed by the rigid `transform` (rotation + translation only). Its faces' masks filter
// it like terrain faces.
struct Dynamic {
    int32_t id = 0; // owner (intro unique id)
    std::shared_ptr<const CollisionMesh> shape;
    glm::mat4 transform{1};
};

class CollisionMesh {
public:
    std::vector<vec3> vertices;
    std::vector<Face> faces;
    std::vector<Markup> markup;
    std::vector<Dynamic> dynamics; // queried with the faces by raycast/sweeps/closestPoint (not by build)

    // (Re)builds the BVH over `faces`. Queries need it; loadLevelCollision calls it.
    void build();

    // Nearest double-sided hit along `dir` (any length) within `maxDist`.
    std::optional<Hit> raycast(vec3 origin, vec3 dir, float maxDist, uint8_t mask = 0xFF) const;
    // First contact of a sphere / capsule (segment a-b, radius r) translated by `delta`.
    // Already-touching shapes hit at t = 0.
    std::optional<Hit> sweepSphere(vec3 center, float radius, vec3 delta, uint8_t mask = 0xFF) const;
    std::optional<Hit> sweepCapsule(vec3 a, vec3 b, float radius, vec3 delta, uint8_t mask = 0xFF) const;
    // Closest surface point within `maxDist`.
    std::optional<Hit> closestPoint(vec3 p, float maxDist, uint8_t mask = 0xFF) const;
    // Floor below `p` (ray straight down, -Z) within `maxDrop`.
    std::optional<Hit> groundProbe(vec3 p, float maxDrop, uint8_t mask = 0xFF) const {
        return raycast(p, {0, 0, -1}, maxDrop, mask);
    }

    vec3 faceNormal(uint32_t face) const; // unit, from winding (v1-v0)x(v2-v0)

private:
    struct Node {
        vec3 lo, hi;
        uint32_t first = 0, count = 0; // count > 0: leaf over order_[first, first+count); else children first, first+1
    };
    template <class Visit> void overlap(vec3 lo, vec3 hi, Visit&& visit) const;
    // Terrain faces only; the public queries add `dynamics`.
    std::optional<Hit> rayFaces(vec3 origin, vec3 dir, float maxDist, uint8_t mask) const;
    std::optional<Hit> sweepFaces(vec3 a, vec3 b, float radius, vec3 delta, uint8_t mask) const;
    std::optional<Hit> closestFaces(vec3 p, float maxDist, uint8_t mask) const;
    // Runs `local(shape, inverse transform)` on every dynamic and keeps the smallest t (hits mapped back).
    template <class Local> std::optional<Hit> withDynamics(std::optional<Hit> best, Local&& local) const;
    std::vector<Node> nodes_;
    std::vector<uint32_t> order_;
};

// Decodes every terrain group collision mesh (the signal/portal group is skipped) and the markup
// polylines of a unit DRM. Throws cdc::FormatError if the DRM has no unit Level root.
// Unsupported parts are reported in `warnings`.
CollisionMesh loadLevelCollision(const cdc::Drm& drm, std::vector<std::string>* warnings = nullptr);

// Appends a box of full side lengths `size`, centred at the origin of `frame` (rigid), as 12 triangles with
// the given face mask. For object collision primitives (src/physics).
void addBox(CollisionMesh& m, const glm::mat4& frame, vec3 size, uint8_t mask = 0xFF);

} // namespace ou::collision
