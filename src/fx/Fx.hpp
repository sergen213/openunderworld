// Original particle effects (cdc GenericFXObject): parser, deterministic 60 Hz simulation, camera-facing sprites.
// Layouts, evidence labels and what is still missing: docs/formats/EFFECTS.md. Renderer-neutral.
#pragma once

#include "cdc/Cdc.hpp"
#include "render/Renderer.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace ou::fx {

inline constexpr int16_t kNoSegment = -127; // "none" in every segment field
inline constexpr size_t kParticleDefSize = 0x17C;
// One colour/size key of a particle definition (emitter +0x148 + 12 * k, tru.exe 0x543F70).
struct Key {
    uint32_t rgba = 0;    // bytes R,G,B,A; 0x80 = 1.0; 0 on a middle key = interpolated from its neighbours
    float size = -1;      // sprite half-width in world units; < 0 = no size key (interpolated)
    int16_t sizeVar = 0;  // +- random per particle
    float time = 0;       // fraction of the particle's life, 0..1
    uint8_t blend = 0;    // def +0x11C + k, low 3 bits (fx::Blend mapping in blendFor)
    float yScale = 1;     // def +0xE0 + 4k: height / width
};

// 0x17C-byte particle emitter definition (GenericFXObject +0x10 array).
struct ParticleDef {
    float size0 = 0;
    uint8_t flags4 = 0, flags5 = 0;
    int8_t mode = 0;                       // +0x06: 0 free particles, 1 attached, 2 one persistent particle
    int16_t segment = 0, segment2 = kNoSegment;
    float radiusMax = 0, radiusMin = 0;    // +0x10 / +0x6C spawn shell radius
    glm::vec3 velocity{0}, velocityRandom{0}, accel{0}, offset{0}; // +0x14 / +0x20 / +0x2C / +0x4C, per 30 Hz frame
    uint32_t colour0 = 0, colourLast = 0;  // +0x38 / +0x3C
    int16_t duration = -1;                 // +0x44 emitter life in 30 Hz frames; -1 forever, 0 one burst
    uint16_t life = 1;                     // +0x46 particle life in 30 Hz frames
    uint8_t lifeVar = 0;                   // +0x7C
    float rate = 0;                        // +0x48 particles per 30 Hz frame (< 0: one per -rate frames)
    uint16_t fadeFar = 0, fadeNear = 0;    // +0x5A / +0x5C camera-distance fade-in band
    float screenFadeStart = 0, screenFadeEnd = 0; // +0x60 / +0x64 on-screen size fade (NDC half-size / 2)
    uint8_t shape = 0;                     // +0x68 spawn shape
    float maxDistance = 0, minDistance = 0; // +0x74 / +0x78 emit only while the camera is in range (0 = off)
    int16_t radialSegment = kNoSegment, velocitySegment = kNoSegment, offsetSegment = kNoSegment; // +0x94/+0x96/+0xAA
    uint8_t animRate = 0;                  // +0xB1 texture frame rate (0 random frame per particle)
    int8_t animStart = 0;                  // +0x91
    std::vector<Key> keys;                 // 2..4, times ascending
    float rot0 = 0, rotRange = 0, spin = 0, spinRange = 0; // degrees, degrees per 30 Hz frame
    uint32_t texture = 0;                  // +0x110 texture section id, 0 = none
    int16_t depthBias = 0;                 // +0x15E view-depth offset (< 0 = toward the camera)
    std::vector<uint32_t> textures;        // +0x114 -> {u32 n, ids[n]} (wins over +0x110)
    uint8_t atlasCols = 0, atlasRows = 0, atlasFrames = 0, atlasFirst = 0; // +0x174..+0x177
    bool model() const { return flags4 & 0x80; } // model particles (casings, debris): drawn as small sprites [I]
};

// Effect command (8 bytes at +8 of an event record, tru.exe 0x547B40): u16 type, then 3 u16 arguments.
struct Command { uint16_t type = 0, a1 = 0, a2 = 0, a3 = 0; };
// Object / animation event record {u16 info, u16 frame, u8 event, ...; command}, 8 + (info >> 12) * 4 bytes
// (tru.exe 0x5BA000). info & 3 kind (1 effect callback), & 4 disabled, & 0x3F0 surface-material filter (0 any,
// else the instance material, tru.exe 0x5BBFD0 / instance +0x186), & 0xC00 variant (matched against 0).
struct Event {
    uint16_t info = 0, frame = 0; // frame: 30 Hz animation frame (anim lists)
    uint8_t event = 0;
    Command cmd;
    bool effect() const { return (info & 3) == 1 && !(info & 4); } // effect callback, not disabled
    uint8_t material() const { return uint8_t((info & 0x3F0) >> 4); }
    // Anim-list record that runs on an instance standing on / hit at `material`.
    bool runsOn(uint8_t mat) const { return effect() && !(info & 0xC00) && (!material() || material() == mat); }
};

struct ObjectFx {
    struct Marker { int segment = 0; glm::mat4 local{1}; }; // HInfo marker in its segment's frame
    std::vector<ParticleDef> particles;
    std::vector<Event> events;           // object section +4 list
    // Animation list (ObjectBase +0x54 / +0x58 {i16 anim, i16, u32 event-list DTP, char* name}) -> that clip's
    // event list (DTP rtype 19: u32 header, then records as `events`). Only clips with an event list.
    std::map<std::string, std::vector<Event>> clips;
    std::vector<glm::mat4> segments;     // bind frames of model 0's segments, object frame
    std::map<int, Marker> markers;       // HInfo marker id -> marker
    std::vector<uint32_t> textureIds() const; // every texture a drawn particle references
    // Object-frame transform of a definition's segment field: >= 0 a segment, -id an HInfo marker (tru.exe 0x46DA20);
    // unknown ones are the root (as 0x46DA20's fallback). `bones` = posed segment frames (object frame); empty = bind.
    glm::mat4 frame(int16_t segment, std::span<const glm::mat4> bones = {}) const;
    const std::vector<Event>* clip(const std::string& name) const {
        const auto it = clips.find(name);
        return it == clips.end() ? nullptr : &it->second;
    }
};
// Object section +4 (event list) and +8 (GenericFXObject). nullopt when the DRM has neither.
std::optional<ObjectFx> parseObjectFx(const cdc::Drm& object, std::vector<std::string>* warnings = nullptr);
// Decodes one particle definition from raw bytes (texture list resolved by the caller); for tests.
ParticleDef parseParticleDef(std::span<const uint8_t> def);

enum class Blend : uint8_t { Alpha, Additive, Subtract, Multiply };
Blend blendFor(uint8_t keyBlend); // tru.exe 0x52AE60: 1 -> mode 4, 2 -> 5, 3 -> 0, 4 -> 6, 5 -> 7, else 5

class System {
public:
    explicit System(uint32_t seed = 0x3039) : seed_(seed) {}
    // Starts particle definition `def` of `fx` at `world` (object matrix). `owner` groups emitters (intro uid).
    // `bones`: posed segment frames in the object frame (empty = bind pose). Returns false for out-of-range defs.
    bool start(std::shared_ptr<const ObjectFx> fx, int def, const glm::mat4& world, int32_t owner,
               int16_t segment = 0, int16_t segment2 = 0, std::span<const glm::mat4> bones = {});
    // Runs an instance's birth effects: enabled event-0 records of fx->events (tru.exe 0x5BA480 -> 0x5BA000).
    int startBirth(const std::shared_ptr<const ObjectFx>& fx, const glm::mat4& world, int32_t owner);
    bool runCommand(const std::shared_ptr<const ObjectFx>& fx, const Command& c, const glm::mat4& world, int32_t owner,
                    std::span<const glm::mat4> bones = {});
    // Animation events: the records of `events` (an fx->clips list) at 30 Hz frames in [from, to) that run on
    // `material` (Event::runsOn). Returns the commands that started something.
    int runEvents(const std::shared_ptr<const ObjectFx>& fx, const std::vector<Event>& events, const glm::mat4& world,
                  int32_t owner, uint8_t material, float from, float to, std::span<const glm::mat4> bones = {});
    // A one-shot instance playing `events` from frame 0 at a fixed `world` as the game ticks (bullet hits, weapon
    // fire: tru.exe births the hit object, whose clip events filter on its material, 0x6B7030).
    void play(std::shared_ptr<const ObjectFx> fx, const std::vector<Event>& events, const glm::mat4& world, int32_t owner,
              uint8_t material);
    void stopOwner(int32_t owner);  // emitters stop, live particles finish
    bool hasOwner(int32_t owner) const;
    void clear();
    void tick(const glm::vec3& camera); // one 60 Hz game tick
    // Camera-facing sprites, back to front, batched by texture and blend.
    void build(const render::Camera& camera, float aspect, std::vector<render::FxVertex>& vertices,
               std::vector<render::FxBatch>& batches) const;
    size_t emitterCount() const { return emitters_.size(); }
    size_t particleCount() const { return particles_.size(); }
    uint64_t checksum() const; // positions + ages, for determinism tests

private:
    struct Emitter {
        std::shared_ptr<const ObjectFx> fx;
        const ParticleDef* def = nullptr;
        glm::vec3 origin{0}, origin2{0};
        glm::mat3 velocityFrame{1}, offsetFrame{1}; // def +0x96 / +0xAA frames (identity when "none")
        int32_t owner = 0;
        int32_t ticksLeft = -1; // -1 forever
        float accum = 0;
        bool burst = false, stopped = false;
        uint32_t persistent = 0; // mode 2: id + 1 of its particle
    };
    struct Particle {
        const ParticleDef* def = nullptr;
        std::shared_ptr<const ObjectFx> fx; // keeps def alive
        glm::vec3 pos{0}, vel{0}, acc{0};
        float sizes[4] = {};
        int32_t age = 0, life = 1; // ticks
        float rot = 0, spin = 0;   // radians, radians per tick
        uint16_t frame = 0;
        uint32_t id = 0, emitter = 0;
    };
    struct Sequence {
        std::shared_ptr<const ObjectFx> fx;
        const std::vector<Event>* events = nullptr; // inside *fx
        glm::mat4 world{1};
        int32_t owner = 0, age = 0; // ticks
        uint8_t material = 0;
    };
    uint32_t rand15(); // tru.exe LCG 0x41C64E6D / 0x3039, bits 16..30
    float rand01() { return float(rand15()) / 32768.0f; }
    float randSigned() { return float(int(rand15() & 0x1FFF) - 0xFFF) / 4096.0f; }
    void spawn(Emitter& e, const glm::vec3& camera);
    uint32_t seed_;
    uint32_t nextId_ = 1;
    std::vector<Emitter> emitters_;
    std::vector<Particle> particles_;
    std::vector<Sequence> sequences_;
};

// Object DRM effects by object name, loaded once, with the decoded textures their particles use.
class Library {
public:
    explicit Library(const cdc::BigfileSet& bigfile) : bigfile_(bigfile) {}
    // nullptr when pc-w\<name>.drm is missing or has neither an event list nor a GenericFXObject.
    std::shared_ptr<const ObjectFx> get(const std::string& objectName, std::vector<std::string>* warnings = nullptr);
    // ObjectBase oflags (0 before get()); birth effects are skipped for bit 0x01000000 (tru.exe 0x5BA480).
    uint32_t flags(const std::string& objectName) const;
    // u16 at `off` of the object data block (ObjectBase +0xC8), 0 when absent (loads the object like get()).
    uint16_t dataU16(const std::string& objectName, uint32_t off);
    const std::vector<scene::TextureData>& textures() const { return textures_; }

private:
    struct Entry { std::shared_ptr<const ObjectFx> fx; uint32_t flags = 0; std::vector<uint8_t> data; };
    void loadTextures(const cdc::Drm& drm, const std::string& name, const std::vector<uint32_t>& ids,
                      std::vector<std::string>* warnings);
    const cdc::BigfileSet& bigfile_;
    std::map<std::string, Entry> objects_;
    std::vector<scene::TextureData> textures_;
    std::set<uint32_t> textureIds_;
};
inline constexpr uint32_t kObjectNoBirthEffects = 0x01000000;

} // namespace ou::fx
