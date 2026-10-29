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

// Material particle (def +0x168 material or +0x16C list {u32 n, materials[n]}: 0x543F70 takes the material path when
// either is set, the material's shaders then draw the sprite). Reduced to what the composite pixel shaders do (EFFECTS.md
// "Material particles"): a colour texture times the vertex colour, a soft depth fade, or a screen-space refraction.
struct ParticleMaterial {
    uint32_t id = 0;
    uint32_t texture = 0;     // first material texture that is not a normal map (category != 3); 0 = white
    uint32_t normal = 0;      // category-3 texture: refraction offsets
    bool additive = false;    // material blend mode 5 (SRCALPHA/ONE); else 2-4 SRCALPHA/INVSRCALPHA
    bool distort = false;     // samples BackBufferTexture (global class 9): drawn as refraction of the scene
    float distortScale = 0;   // screen-uv offset per unit of (normal.xy * mask alpha)
    float softScale = 0, softBias = 0; // DepthBufferTexture (class 10): alpha *= sat((scene - z - (1 - a) bias) * scale)
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
    std::vector<ParticleMaterial> materials; // +0x168 / +0x16C (wins over the textures)
    bool model() const { return flags4 & 0x80; } // model particles (casings, debris): drawn as small sprites [I]
};

// Class 2 glow (GenericFXObject +0x1C / +0x20, 0x58 bytes): creator tru.exe 0x536B70 -> 0x536070, update 0x535FA0,
// draw 0x536280 (prim type 0x82, drawn from the 0xAE4D04 list by 0x548BF0). Times in 30 Hz frames.
struct GlowDef {
    uint8_t flags = 0;               // +0x00: 1 birth-enabled, 2 rotation jitter, 4 ignores instance fade,
                                     // 0x10 random colour phase, 0x20 fixed at its start position; >> 6 == 1: alpha
    int8_t kind = 1;                 // +0x01: 1 one sprite at `segment` (every record); >= 2 beams (0x5361E0): not drawn
    int16_t segment = 0, segment2 = 0; // +0x02 / +0x04 (-126 = the instance origin)
    std::vector<uint32_t> colours;   // +0x08 -> {u32 n, rgba[n]}; none = 0x80808080
    uint8_t id = 0;                  // +0x0C prim id (stop commands 0x15 / 0x1A / 0x18 / 0x1B)
    uint8_t lifeVar = 0;             // +0x0F +- period
    int16_t period = 30;             // +0x10 colour cycle period (colours >= 2)
    float facing = 1;                // +0x12 / 180: drawn while dot(axis, to camera) > 1 - facing
    float width = 0, height = 0;     // +0x14 / +0x16 start half-size [I: world units]
    float endWidth = -1, endHeight = -1; // +0x18 / +0x1A size at the end of `life` (< 0 = constant)
    int32_t life = -1;               // +0x1C; <= 0 forever (until stopped)
    uint16_t fadeIn = 0, fadeOut = 0; // +0x20 / +0x22
    int16_t depthBias = 0;           // +0x24 view-depth offset (z >= 20 after it)
    int8_t facingMode = 0;           // +0x26 low nibble, signed: +-1/2/3 = local +-X/Y/Z of axisSegment (or the
                                     // axisSegment -> axisSegment2 vector when they differ), 4 distance [U], 0 none
    int16_t axisSegment = 0, axisSegment2 = 0; // +0x28 / +0x2A
    float rot0 = 0, rotRange = 0, spin = 0, spinRange = 0; // +0x2C.. degrees, degrees per 30 Hz frame [I]
    uint8_t texRate = 0;             // +0x3C texture-list frames per step (0 = one random frame)
    uint8_t condition = 0;           // +0x3D: 1 / 2 draw only when / unless 0x5EA390 (a camera mode [U]) is set
    uint32_t texture = 0;            // +0x40; 0 = kGlowTexture
    std::vector<uint32_t> textures;  // +0x44 -> {u32 n, ids[n]}
    glm::vec3 offset{0};             // +0x48 s16 x3 in the segment's frame
    uint16_t fadeFar = 0, fadeNear = 0; // +0x4E / +0x50 camera-distance fade band
    bool alphaBlend() const { return (flags >> 6) == 1; } // draw mode 2 (alpha fades); else 1 (RGB fades: additive) [I]
};
// Glows without a texture draw particle.drm model 0's first animated texture (0x52B0B0(0x8AA77C, 0, 0)).
inline constexpr uint32_t kGlowTexture = 130;

// Class 6 effect light (GenericFXObject +0x3C / +0x40, 0x24 bytes; tru.exe 0x546710 -> 0x5C35D0).
struct LightDef {
    uint8_t kind = 0;                // +0x00 (1 = timed light prim)
    int16_t life = 0;                // +0x02 timed prim life (0 = the +0x16 duration applies)
    int16_t segment = 0;             // +0x06
    glm::vec3 colour{0}, colour2{0}; // +0x0A / +0x0E rgb / 255; colour2 = end colour [I]
    bool birth = false;              // +0x11 & 1
    float radius = 0, radius2 = 0;   // +0x12 / +0x14 (radius2 0 = constant) [I]
    int16_t duration = -1;           // +0x16, -1 forever
    int16_t fadeIn = 0, fadeOut = 0; // +0x18 / +0x1A [I]
    std::optional<scene::Light> dtp; // +0x20 light DTP: type, cone, falloff curve, flicker
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
    std::vector<GlowDef> glows;          // class 2
    std::vector<LightDef> lights;        // class 6
    struct MapEntry { uint8_t id = 0; int8_t cls = -1; uint8_t index = 0; };
    std::vector<MapEntry> effectMap;     // +0x04 / +0x08 (commands 0x1C / 0x2F look effects up by id)
    std::vector<Event> events;           // object section +4 list
    // Animation list (ObjectBase +0x54 / +0x58 {i16 anim, i16, u32 event-list DTP, char* name}) -> that clip's
    // event list (DTP rtype 19: u32 header, then records as `events`). Only clips with an event list.
    std::map<std::string, std::vector<Event>> clips;
    std::vector<glm::mat4> segments;     // bind frames of model 0's segments, object frame
    std::map<int, Marker> markers;       // HInfo marker id -> marker
    std::vector<uint32_t> textureIds() const; // every texture a drawn particle or glow references (kGlowTexture apart)
    bool usesGlowTexture() const;             // a glow without a texture of its own
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

// Unit FX point sets (unit data +0x120 count, +0x124 -> 0x70-byte records; EFFECTS.md "FX point sets"): fires, heat
// haze, smoke placed in the level without an object. Each runs an event list (DTP rtype 34: u32 GenericFXObject DTP id,
// then event records) at birth with its two points as segments 1 and 2 (line emitters run between them).
struct PointSet {
    int32_t id = 0;                     // record +0x44
    std::string name;                   // +0x40, e.g. "FXPointSet50069", "fx_heat_haze50001"
    std::shared_ptr<const ObjectFx> fx; // the list's GenericFXObject (bundled in the unit DRM)
    std::vector<Event> events;          // +0x58 list
    std::vector<glm::mat4> segments;    // 0 = the record's matrix, then the +0x64 points (0x50 bytes, matrix first)
    float distance = 0;                 // +0x5C [U: cull distance]
};
std::vector<PointSet> parsePointSets(const cdc::Drm& unit, std::vector<std::string>* warnings = nullptr);

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
    // A unit FX point set's birth records (as startBirth) with its points as the segments; `world` = the unit offset.
    int startPointSet(const PointSet& set, const glm::mat4& world, int32_t owner);
    // `birth`: run from the birth list, where glows / lights also need their record's birth bit.
    bool runCommand(const std::shared_ptr<const ObjectFx>& fx, const Command& c, const glm::mat4& world, int32_t owner,
                    std::span<const glm::mat4> bones = {}, bool birth = false);
    // Animation events: the records of `events` (an fx->clips list) at 30 Hz frames in [from, to) that run on
    // `material` (Event::runsOn). Returns the commands that started something.
    int runEvents(const std::shared_ptr<const ObjectFx>& fx, const std::vector<Event>& events, const glm::mat4& world,
                  int32_t owner, uint8_t material, float from, float to, std::span<const glm::mat4> bones = {});
    // A one-shot instance playing `events` from frame 0 at a fixed `world` as the game ticks (bullet hits, weapon
    // fire: tru.exe births the hit object, whose clip events filter on its material, 0x6B7030).
    void play(std::shared_ptr<const ObjectFx> fx, const std::vector<Event>& events, const glm::mat4& world, int32_t owner,
              uint8_t material);
    void stopOwner(int32_t owner);  // emitters stop, live particles finish, glows fade out, lights end
    bool hasOwner(int32_t owner) const;
    void clear();
    void tick(const glm::vec3& camera); // one 60 Hz game tick
    // Camera-facing sprites (particles and glows), back to front, batched by texture and blend.
    void build(const render::Camera& camera, float aspect, std::vector<render::FxVertex>& vertices,
               std::vector<render::FxBatch>& batches) const;
    // Live effect lights at their current colour / radius.
    struct LiveLight {
        glm::vec3 pos{0}, dir{0}, colour{0}; // colour: rgb x fades, 1 = full
        float radius = 0;
        const scene::Light* dtp = nullptr;   // the record's light DTP (type, cone, falloff, flicker) when it has one
        int32_t owner = 0;
        bool forever = false;                // no duration: a fixture (birth lights of placed objects)
    };
    void lights(std::vector<LiveLight>& out) const; // replaces out's contents
    size_t emitterCount() const { return emitters_.size(); }
    size_t particleCount() const { return particles_.size(); }
    size_t glowCount() const { return glows_.size(); }
    uint64_t checksum() const; // positions + ages, for determinism tests

private:
    struct Glow {
        std::shared_ptr<const ObjectFx> fx;
        const GlowDef* def = nullptr;
        glm::vec3 pos{0};
        glm::vec3 axisFrom{0}, axis{0};      // facing test (facingMode != 0)
        int32_t owner = 0, age = 0, left = -1; // ticks; left -1 forever
        int32_t fadeOut = -1;                // ticks left of a stop fade (-1 not stopping)
        float scaleW = 1, scaleH = 1, growW = 0, growH = 0, rot = 0, spin = 0;
        int16_t period = 30;
        uint16_t phase = 0, frame = 0;
    };
    struct FxLight {
        std::shared_ptr<const ObjectFx> fx;
        const LightDef* def = nullptr;
        glm::vec3 pos{0}, dir{0, 1, 0};
        int32_t owner = 0, age = 0, life = -1; // ticks, -1 forever
        int32_t fadeOut = -1;
    };
    bool startGlow(const std::shared_ptr<const ObjectFx>& fx, int def, const glm::mat4& world, int32_t owner,
                   int16_t segment, std::span<const glm::mat4> bones);
    bool startLight(const std::shared_ptr<const ObjectFx>& fx, int def, const glm::mat4& world, int32_t owner,
                    int16_t segment, std::span<const glm::mat4> bones);
    void stopId(int32_t owner, int id, bool now); // commands 0x15 / 0x1A (fade) and 0x18 / 0x1B (now); id < 0 all
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
    std::vector<Glow> glows_;
    std::vector<FxLight> lights_;
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
    // A unit's FX point sets (parsed once per unit name) with their textures loaded from the unit DRM.
    const std::vector<PointSet>& pointSets(const cdc::Drm& unit, const std::string& unitName,
                                           std::vector<std::string>* warnings = nullptr);
    const std::vector<scene::TextureData>& textures() const { return textures_; }

private:
    struct Entry { std::shared_ptr<const ObjectFx> fx; uint32_t flags = 0; std::vector<uint8_t> data; };
    void loadTextures(const cdc::Drm& drm, const std::string& name, const std::vector<uint32_t>& ids,
                      std::vector<std::string>* warnings);
    const cdc::BigfileSet& bigfile_;
    std::map<std::string, Entry> objects_;
    std::map<std::string, std::vector<PointSet>> pointSets_;
    std::vector<scene::TextureData> textures_;
    std::set<uint32_t> textureIds_;
};
inline constexpr uint32_t kObjectNoBirthEffects = 0x01000000;

} // namespace ou::fx
