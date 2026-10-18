// Skeletons, animation clips (DRM section type 2), pose sampling and CPU skinning for TRU PC.
// Format notes and evidence: docs/formats/ANIMATION.md. Renderer-neutral.
#pragma once

#include "cdc/Cdc.hpp"
#include "scene/Assets.hpp"

#include <algorithm>
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

// ---- blending (docs/formats/ANIMATION.md "Transitions") ------------------------------------------
// Rotation: quaternion slerp of the axis-angle vectors (shortest arc); scale and translation: lerp.
// w = 0 gives `a`, w = 1 gives `b`.
LocalTransform blend(const LocalTransform& a, const LocalTransform& b, float w);
void blendPoses(std::span<const LocalTransform> a, std::span<const LocalTransform> b, float w, std::span<LocalTransform> out);
// Phase sync for looping locomotion: the time in `to` at the same cycle fraction as `fromTime` in `from`.
double syncedStartTime(const Clip& from, double fromTime, const Clip& to);

// Crossfading player. Each play() fades the new clip in over `blendSeconds` (linear weight, as tru.exe's
// anim request: weight += 1/interpFrames per 30 Hz frame) on top of whatever was showing; older layers keep
// playing until a newer one reaches full weight.
class AnimController {
public:
    explicit AnimController(const scene::Skeleton& skeleton);
    // `clip` may be null (bind pose). `startTime` in seconds; looping clips may pass unbounded times.
    void play(const Clip* clip, float blendSeconds, bool loop, float rate = 1, double startTime = 0);
    void update(double dt);                // advances every layer and the fades
    void setTime(double seconds);          // newest layer's time (when the caller owns the clock)
    std::vector<LocalTransform> pose() const;
    float weight() const;                  // newest layer's fade-in weight, 1 = no blend in progress
    size_t layers() const { return layers_.size(); }

private:
    struct Layer {
        const Clip* clip;
        double time;
        float rate, fade, elapsed;
        bool loop;
        float weight() const { return fade <= 0 ? 1.0f : std::min(1.0f, elapsed / fade); }
    };
    std::vector<LocalTransform> sampleLayer(const Layer& l) const;
    const scene::Skeleton* skeleton_;
    std::vector<Layer> layers_; // oldest first
};

// ---- hair (render-model cloth chains, docs/formats/ANIMATION.md "Hair") ----------------------------
struct HairRig {
    struct Particle {
        uint32_t bone = 0;  // skeleton bone (116.. for Lara)
        int32_t pred = -1;  // strand predecessor particle (orientation reference), -1 for roots
        bool pinned = false; // segment flags bit 0: follows the animation rigidly
        uint8_t group = 0;   // chain group of the render model
    };
    struct Link { uint32_t a = 0, b = 0; float rest = 0; }; // particle indices, rest length (units)
    std::vector<Particle> particles;
    std::vector<Link> links;
};
// Particles and distance constraints of every chain group of the skeleton's render model.
HairRig decodeHair(const cdc::Drm& drm, const scene::Skeleton& skeleton);

struct HairCapsule { uint32_t boneA = 0, boneB = 0; glm::vec3 offsetB{0}; float radius = 0; }; // offsetB: in boneB's frame
struct HairGroupParams {
    float gravity = 5.72443f * 900.0f; // units/s^2 (default: Lara's gravity, tru.exe 5.72443 per 30 Hz frame^2)
    float damping = 0.08f;             // velocity fraction lost per tick
    float stiffness = 0.04f;           // pull toward the animated (rigid) position per tick
};
struct HairParams {
    HairGroupParams defaults;
    std::vector<HairGroupParams> groups; // per chain group, overriding `defaults` where present
    float follow = 0.0f;               // fraction of the animated motion carried over without inertia
    int iterations = 4;
    std::vector<HairCapsule> body;     // radii are clamped so no bind-pose particle starts inside
    const HairGroupParams& group(size_t g) const { return g < groups.size() ? groups[g] : defaults; }
};

// Verlet chains in world space on the caller's fixed tick; deterministic for identical inputs.
class HairSim {
public:
    HairSim() = default;
    HairSim(const HairRig& rig, const scene::Skeleton& skeleton, HairParams params);
    bool empty() const { return rig_.particles.empty(); }
    // `bones`: animated model-space matrices (worldMatrices); `toWorld`: model -> world.
    void reset(std::span<const glm::mat4> bones, const glm::mat4& toWorld);
    void step(std::span<const glm::mat4> bones, const glm::mat4& toWorld, float dt);
    // Replaces the hair bones of `bones` (model space) with the simulated ones.
    void apply(std::span<glm::mat4> bones, const glm::mat4& toWorld) const;
    const std::vector<glm::vec3>& positions() const { return pos_; } // world space, per particle
    const HairParams& params() const { return p_; }

private:
    glm::vec3 capsulePoint(const HairCapsule& c, std::span<const glm::mat4> bones, const glm::mat4& toWorld, bool end) const;
    HairRig rig_;
    HairParams p_;
    std::vector<glm::vec3> bind_; // bind position per skeleton bone
    std::vector<glm::vec3> pos_, prev_, target_; // target_: last animated position per particle
    std::vector<int32_t> child_;  // per particle: first particle whose pred it is, -1 at strand tips
};

} // namespace ou::anim
