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
    uint8_t flags = 0;        // raw +0x0C adjacencyFlags (TRLAU IndexedFace); bit 0x40 = skipped by tru.exe face queries
    uint8_t mask = 0xFF;      // raw +0x0D; tru.exe ANDs it with the query mask (observed 3 on terrain)
    // raw +0x0E. tru.exe's ground check 0x5DCE20 reads it through 0x5D3AF0 [O]: 0x0004 / 0x0040 slide surfaces
    // (slide kinds 2 / 1), 0x0020 never too steep to stand on, 0x0100 stood on however steep. 0x0080: no wall jump
    // (game/WallJump.cpp). Other bits unknown.
    uint16_t clientFlags = 0;
    uint8_t material = 0;     // raw +0x10; meaning unknown (surface/sound type?)
    uint16_t group = 0;       // terrain group index in the unit
};

// tru.exe skips faces with flags bit 0x40 and faces whose mask shares no bit with the query mask.
inline bool queryable(const Face& f, uint8_t mask) { return !(f.flags & 0x40) && (f.mask & mask); }
// Query mask of the gameplay camera's collision. Terrain: tru.exe's collider queries faces with mask 2 (0x5EB9B0 ->
// 0x5EAF60 -> 0x462E10) and skips terrain groups whose TerrainGroupExtra flags have 0x200 (0x5EB9B0 / 0x5EC360; its
// rays 0x493330 -> filter 0x48EF50 skip 0x300), so loadLevelCollision clears bit 2 of those groups' faces (21 903
// faces in 76 groups over the 82 units; every other terrain face has mask 3 or 0). Objects: only those with ObjectBase
// oflags bit 0x02 (0x5E94C0 / 0x5E91D0 / 0x5E8940), which physics::solidShape marks with bit 0x20.
// ponytail: the rays' extra 0x100-only skip (5 groups with faces, e.g. l2_norsehub 2) is not modelled.
inline constexpr uint8_t kCameraMask = 0x22;
// Query mask of tru.exe's line-of-sight ray for targeting (0x713200 -> 0x713090: ray flags 0xC5AC1983 -> 0x4935F0 ->
// 0x493330): terrain faces with mask bit 1 (flags & 0x20000002 -> query mask 1), skipping terrain groups whose
// TerrainGroupExtra flags have 0x8, 0x20000, 0x40000 or 0x80000 (filter 0x48EF50 for those ray flags), so
// loadLevelCollision gives the other mask-1 faces bit 0x04 (raw face masks are only 0, 1 and 3 over the 82 units);
// objects only with ObjectBase oflags bit 0x08 (0x713090 ignores a hit instance without it), which
// physics::solidShape marks with bit 0x40.
inline constexpr uint8_t kSightMask = 0x44;

// Level camera record (unit data +0x208 count, +0x20C 0x70-byte records; tru.exe 0x606xxx): where the gameplay camera
// is drawn while Lara is on a markup whose camera commands select it (command 0x12, COLLISION.md "Camera records").
struct CameraRecord {
    uint32_t type = 0;                 // +0x4C: 0 nearest spline point to Lara, 1 / 2 along her markup, 3 by markup in `markups`
    std::vector<vec3> pos, tangent;    // +0x44 count, +0x48 0x20-byte Hermite points {position, tangent} (unit frame)
    std::vector<uint16_t> markups;     // +0x50 count, +0x54 markup items (as markup ids)
    float angleSpeed = 0, distanceSpeed = 0, radius = 0; // +0x58 deg/s, +0x5C units/s, +0x60
    bool distance = false, inside = false;               // +0x6C drive the distance too, +0x6D no collision within radius
    std::vector<std::pair<uint32_t, float>> forces;      // +0x64 count, +0x68 0x44-byte force records {type, k}
};

// Markup polyline ("markup2" in tru.exe): ledges, landings, balance beams, ...
struct Markup {
    uint8_t type = 0;   // raw type byte; markupFlags/markupTypeName interpret it
    uint16_t id = 0;
    std::vector<vec3> points;
    std::optional<CameraRecord> camera; // item +0x64 / +0x68 camera commands: the record command 0x12 selects
    uint16_t intro = 0; // unit item +0x4A -> MarkUp +0x2C: tru.exe's "introID" of a markup box (0x5C41A0's debug text)
};
// MUD flag bits tru.exe derives from the type (switch 0x5D6F90 | 0x5D70A0).
uint32_t markupFlags(uint8_t type);
std::string_view markupTypeName(uint8_t type); // "ledgegrab", ..., "unknown"

inline constexpr uint16_t kClientSlide = 0x0044;      // slope-slide surface (0x04 kind 2, 0x40 kind 1)
inline constexpr uint16_t kClientNeverSteep = 0x0020; // exempt from the steep-floor test
inline constexpr uint16_t kClientStandSteep = 0x0100; // stood on although steep

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

    // NtTerrainGroup identity is the unit model-list index; group 0 has no model/extra record.
    std::optional<uint32_t> terrainGroupForModel(uint32_t modelIndex) const;
    std::optional<bool> terrainGroupEnabled(uint32_t modelIndex) const;
    bool setTerrainGroupEnabled(uint32_t modelIndex, bool enabled);

    // Merged mesh of several built meshes (World streaming: one per loaded unit) without rebuilding a BVH over all of
    // them: appendPart adds `part`'s vertices moved by `offset`, its faces and terrain groups, and its own BVH (bounds
    // moved; a part without a current BVH is built first); finishParts puts a small top level over the parts' BVHs.
    // Queries need finishParts after the last appendPart. clearParts empties the geometry (not markup or dynamics).
    void clearParts();
    void appendPart(const CollisionMesh& part, vec3 offset);
    void finishParts();

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
    bool queryable(const Face& f, uint8_t mask) const;
    friend CollisionMesh loadLevelCollision(const cdc::Drm& drm, std::vector<std::string>* warnings);
    std::vector<Node> nodes_;
    std::vector<uint32_t> order_;
    uint32_t root_ = 0;              // nodes_ index of the root (build: 0; finishParts: the top level's)
    std::vector<uint32_t> partRoots_; // appendPart: each part's root in nodes_
    std::vector<uint32_t> groupModels_; // terrain group -> unit model-list index; UINT32_MAX = no identity
    std::vector<bool> groupEnabled_;
};

// Decodes every terrain group collision mesh (the signal/portal group is skipped) and the markup
// polylines of a unit DRM. Throws cdc::FormatError if the DRM has no unit Level root.
// Unsupported parts are reported in `warnings`.
CollisionMesh loadLevelCollision(const cdc::Drm& drm, std::vector<std::string>* warnings = nullptr);

// Appends a box of full side lengths `size`, centred at the origin of `frame` (rigid), as 12 triangles with
// the given face mask. For object collision primitives (src/physics).
void addBox(CollisionMesh& m, const glm::mat4& frame, vec3 size, uint8_t mask = 0xFF);

} // namespace ou::collision
