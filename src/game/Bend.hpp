// bend_* plants (kelp, sea fans, jungle broadleaf, bamboo): tru.exe's "Bendie" controller and the unit wind that drives
// it, on the fixed 60 Hz tick. Each bend bone is a damped 2D spring; its state becomes the bone's bend vector (the
// 4th skin-palette row the bend VS reads as c88; scene::MeshData::bend). Evidence: docs/GAMEPLAY.md "Bend plants".
// Frame: Z-up game units; positions in the world frame (unit frame + stream offset).
#pragma once

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace ou::game {

// Object data (ObjectBase +0xC8, class 57 / 0xB00B) of a bend_* object [O 0x48FD70, 0x499B40].
struct BendType {
    bool physical = false; // +0x08: has a physics phantom; Lara pushes it by contact (0x490590), else by distance (0x490450)
    struct Bone {          // +0x24 -> 0x48-byte records, count +0x28
        int segment = 0;   // +0x3C s16
        float mass = 1, linear = 0, quadratic = 0, omega = 0, damping = 0; // +0x08 .. +0x18
        float push = 0, pushRadius = 0, pushPad = 0, wind = 0;            // +0x1C .. +0x28
        glm::vec3 centre{0};       // the segment's first HInfo box: centre (model space)
        glm::vec3 lo{0}, hi{0};    // its corners centre -/+ size / 2 (min / max)
        glm::vec2 inv2Size{0};     // 1 / (2 size.xy)
    };
    std::vector<Bone> bones;
    int segments = 0; // bend vectors per instance (one per model segment)
};
// nullptr when `object` is no bend object or a bone has no box (tru.exe marks it "can't bend": never updated).
std::shared_ptr<const BendType> loadBendType(const cdc::Drm& object);

struct BendIntro {
    int32_t uid = 0;
    glm::vec3 pos{0}, rot{0}; // unit frame, Euler radians (formats::Intro)
    std::shared_ptr<const BendType> type;
};

// A unit's global wind (formats::Unit::wind; tru.exe wind source type 1: retargeted by 0x53C3F0, evaluated by 0x52E460).
class Wind {
public:
    explicit Wind(std::vector<formats::Unit::Wind> records = {});
    void tick();                      // one 60 Hz tick (the wind clock DAT_008A5074 counts them)
    glm::vec2 at(glm::vec3 pos) const; // world-frame wind at `pos` (z = 0)

private:
    struct Channel { float cur = 0, target = 0, rate = 0; };
    struct Source {
        formats::Unit::Wind rec;
        int countdown = 0; // ticks until the next targets
        Channel strength, heading, wavelength;
        float frequency = 0; // radians per tick
    };
    float random(float lo, float hi); // 0x52E750 with its own LCG (DAT_00A038E0)
    std::vector<Source> sources_;
    uint32_t seed_ = 0, ticks_ = 0;
};

class Bends {
public:
    struct Unit {
        std::string name;
        glm::vec3 offset{0};
        const std::vector<BendIntro>* intros = nullptr;
        const std::vector<formats::Unit::Wind>* wind = nullptr;
    };
    // Instances of `units` (kept states survive; unloaded units' plants and winds go).
    void sync(std::span<const Unit> units);
    // One 60 Hz tick: wind, Lara's push (her position now and a tick ago), the springs.
    void tick(glm::vec3 lara, glm::vec3 laraPrev);
    // Bend vectors of intro `uid`, one per model segment: (a.x, a.y, b.x, b.y), s = a * v.xy + b; empty if none.
    std::span<const glm::vec4> bend(int32_t uid) const;
    size_t size() const { return instances_.size(); }

private:
    struct Bone { glm::vec2 x{0}, v{0}, force{0}; glm::vec3 pos{0}; };
    struct Instance {
        int32_t uid = 0;
        std::shared_ptr<const BendType> type;
        std::string unit;
        glm::vec3 pos{0};
        glm::quat toLocal{1, 0, 0, 0}; // world -> instance axes
        std::vector<Bone> bones;
        std::vector<glm::vec4> out;
    };
    std::vector<Instance> instances_;
    std::unordered_map<int32_t, size_t> index_;
    std::map<std::string, Wind> winds_;
};

} // namespace ou::game
