// Skeletons, animation clips (DRM section type 2), pose sampling and CPU skinning for TRU PC.
// Format notes and evidence: docs/formats/ANIMATION.md. Renderer-neutral.
#pragma once

#include "cdc/Cdc.hpp"
#include "scene/Assets.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ou::anim {

// Anim time unit: 100 per 30 Hz frame (tru.exe feeds the anim graph timeMult * 100).
inline constexpr double kTimeUnitsPerSecond = 3000.0;

// One skeleton per dtp Model section (a DTPData section whose resource relocation at +0x64 names a
// render model). Bones = the model's segments followed by the render model's hair/cloth chain
// segments (attached rigidly; no simulation). Problems become warnings.
std::vector<scene::Skeleton> decodeSkeletons(const cdc::Drm& drm, std::vector<std::string>& warnings);

// Channel order inside a segment, as in tru.exe's per-segment component mask.
enum Channel : uint8_t { RotX, RotY, RotZ, ScaleX, ScaleY, ScaleZ, TransX, TransY, TransZ, kChannels };

struct Clip {
    uint32_t id = 0;            // animID = the type-2 section id
    uint16_t keyCount = 0;
    uint16_t timePerKey = 0;    // kTimeUnitsPerSecond units
    // tracks[segment][channel]: keyCount values, or empty = default (rot 0, scale 1, trans = bind pivot).
    // Rotations are axis-angle vectors (direction = axis, length = angle in radians).
    std::vector<std::array<std::vector<float>, kChannels>> tracks;
    double durationSeconds() const { return double(keyCount) * timePerKey / kTimeUnitsPerSecond; }
};

// Decodes a type-2 section payload. Throws cdc::FormatError on malformed or unsupported data.
Clip decodeClip(std::span<const uint8_t> sectionData);

// The object's animation list (Object DTP +0x54 count, +0x58 entries {i16 animID, i16, u32, char* name}).
struct AnimEntry {
    uint32_t animId = 0;
    std::string name;
};
std::vector<AnimEntry> animList(const cdc::Drm& drm);

struct LocalTransform {
    glm::vec3 rot{0.0f};        // axis-angle vector
    glm::vec3 scale{1.0f};
    glm::vec3 trans{0.0f};
};

// Bind pose: no rotation, translation = pivot.
std::vector<LocalTransform> bindPose(const scene::Skeleton& skeleton);
// Samples `clip` at `seconds` the way tru.exe does: value = lerp(key[k-1], key[k], frac) with
// k = floor(t / timePerKey); k-1 wraps to the last key when looping, else clamps to key 0.
std::vector<LocalTransform> sample(const Clip& clip, const scene::Skeleton& skeleton, double seconds, bool loop);

glm::mat3 rotationFromVector(glm::vec3 axisAngle);
// Model-space bone matrices: world[i] = world[parent] * T(trans) * R(rot) * S(scale).
std::vector<glm::mat4> worldMatrices(const scene::Skeleton& skeleton, std::span<const LocalTransform> local);
// Bind position of every bone (sum of pivots).
std::vector<glm::vec3> bindPositions(const scene::Skeleton& skeleton);
// Skin matrices: world[i] * T(-bindPosition[i]).
std::vector<glm::mat4> skinPalette(const scene::Skeleton& skeleton, std::span<const glm::mat4> world);
// Linear blend skinning of mesh.vertices (bind pose) into `out` (resized to match).
void skinVertices(const scene::MeshData& mesh, std::span<const glm::mat4> palette, std::vector<scene::Vertex>& out);

} // namespace ou::anim
