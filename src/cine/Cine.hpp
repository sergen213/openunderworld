// In-engine cutscenes: the CINE stream inside pc-w\cinstream\<name>.mul (cdc::CinematicHandlerImpl,
// tru.exe 0x456580 header, 0x457100 frames), its camera / actor channels and their playback.
// Format notes, evidence labels and the verification: docs/formats/CINEMATICS.md.
#pragma once

#include "cdc/Cdc.hpp"
#include "locale/Locale.hpp"
#include "scene/Assets.hpp"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ou::cine {

inline constexpr double kFps = 30.0; // frames per second (the debug overlay prints frame / 30 as mm:ss)

// Concatenated payloads of the cinematic (type 1) packets of a MultiplexStream .mul.
std::vector<uint8_t> demux(std::span<const uint8_t> mul);

// ---- header (0x456580) ---------------------------------------------------------------------------------
// Channels are numbered in header order; every channel holds one float per frame.
struct Marker {                    // area marker: 9 transform channels (scale xyz, rotation xyz, translation xyz)
    uint32_t unit = 0;             // stream unit id
    int32_t intro = 0;             // its intro unique id (object co_cinetest_marker)
    uint32_t channel = 0;
};
struct Light {                    // instance light entry, 6 u32 (0x454BD0 light loop at 0x4551B0)
    uint32_t bone = 0;            // instance bone it rides on: light matrix = bone matrix * Rx(180 deg)
    uint32_t type = 0;            // 0 directional, 1 point, 2 spot (-> render light types 2, 0, 1); survey: all spots
    uint32_t flag = 0, unk3 = 0, unk4 = 0; // [U] (0/1, 1/2, 512/1024/2048)
    uint32_t table = 0;           // light DTP template: globalaniminfo ObjectBase +0xC8 -> +0x14 + 4 table (20 ids)
};
struct Instance {
    int32_t intro = -1;            // unit intro unique id; -1 = the player (Lara)
    int32_t alt = -1;              // header +4: when not -1 the exe binds another instance (0x4D12D0) [U]; -1 in all 53
    uint32_t bones = 0;
    uint32_t channel = 0;          // 3 instance channels, then 14 per bone (boneChannel)
    std::vector<glm::mat4> boneFix; // per bone, applied after the animated matrix (translation zeroed by the exe)
    std::vector<Light> lights;    // 10 channels each from lightChannel (LightPose)
    uint32_t lightChannel = 0;
    uint32_t vars = 0, varChannel = 0;     // script-visible float channels (0x9CDFA4 callback)
    // Optional tail: where the instance stands after the cinematic (0x455D90 restores it).
    std::optional<glm::vec3> endPos, endRot;
    uint32_t endUnit = 0;
    uint32_t boneChannel(uint32_t bone) const { return channel + 3 + 14 * bone; }
};
// Per-bone channels relative to boneChannel(): 0-8 transform, 9 flags (bit 0 parent-scale compensation,
// bit 1 parent = bone 0, bit 2 on bone 0: move the instance to the handler's unit, 0x5B3A20), 10-13 the bone's
// skin-palette 4th row (Player::boneRows).
inline constexpr uint32_t kBoneFlags = 9;
struct Event {                     // rising edge of the channel sends `id` to the header's event target
    uint32_t id = 0, channel = 0;
};
struct Header {
    uint32_t version = 0;          // 9
    std::string unitName;          // as stored ("L3_main_bedroom")
    uint32_t unitId = 0;
    std::vector<Marker> markers;
    std::vector<Instance> instances;
    std::vector<uint32_t> cameras; // 16 channels each, see Camera
    uint32_t eventTarget = 0;
    std::vector<Event> events;
    uint32_t channels = 0;         // running total, as +0xCC
    size_t framesOffset = 0;       // first frame
};
// Throws cdc::FormatError on truncation, a bad magic or a header size mismatch.
Header parseHeader(std::span<const uint8_t> cine);

// ---- frames (0x457100) ---------------------------------------------------------------------------------
// Sequential decoder: {u32 size, i32 frame (-1 ends)}, per channel a run-length coded float, u32 subtitle
// length + bytes, padding to 16. Voice-line streams (0 channels) end at a frame boundary without -1.
class Decoder {
public:
    Decoder(std::span<const uint8_t> cine, const Header& h);
    // Decodes the next frame; false at the end. Throws cdc::FormatError on truncation / inconsistency.
    bool next();
    int frame() const { return frame_; }                       // last decoded frame, -1 before the first
    std::span<const float> values() const { return values_; }  // its channel values
    std::string_view subtitleBlob() const { return subtitle_; }
    size_t offset() const { return p_; }                       // bytes consumed
    bool terminated() const { return terminated_; }            // stopped at a -1 frame

private:
    struct Run { uint32_t left = 0; bool animated = false; };
    std::span<const uint8_t> b_;
    size_t p_ = 0;
    int frame_ = -1;
    bool terminated_ = false, done_ = false;
    std::vector<Run> runs_;
    std::vector<float> values_;
    std::string_view subtitle_;
};

// Whole-stream scan: frame count and every subtitle entry (all languages).
struct Summary {
    Header header;
    int frames = 0;
    std::vector<locale::CineSubtitle> subtitles;
    // After the -1 frame the muxer pads with more {12, -1, 0, 0} frames: true when that is all that follows.
    bool cleanEnd = false;
};
Summary scan(std::span<const uint8_t> cine);

// ---- playback ------------------------------------------------------------------------------------------
// Camera of the active camera (channel base c): c+0..8 transform, c+9 horizontal FOV (radians), c+10 flags
// (bit 0 cut, bit 2 active camera, bit 3 light-region probe, bit 6 depth of field), c+11..13 depth of field,
// c+14 time scale.
struct CameraPose {
    glm::vec3 position{0}, forward{0, 1, 0}, up{0, 0, 1};
    float horizontalFov = 1.0f; // radians
    bool cut = false;           // the current frame starts a new shot
    // c+10 bit 6: 0x4547D0 hands globalaniminfo's cinematic filter (ObjectBase +0xC8 -> +0x68) with depthOfField(c+11,
    // c+12, c+13) applied to the render device. Only 045_med_kraken, 110_tha_yachtexit and 150_man_pinned set it.
    bool dof = false;
    float focus = 0, fStop = 0, focalLength = 0; // c+11..13 (focus 60..4827, f-stop 1..14.8, focal 25..600)
    // tru.exe 0x455910: vertical FOV = 2 atan(tan(h / 2) / aspect).
    float verticalFov(float aspect) const;
};

// A light of an instance (10 channels from Instance::lightChannel + 10 i; 0x454BD0 at 0x4551B0). Flags and masks
// come from the frame floor(pos) value, the rest is interpolated.
struct LightPose {
    bool on = false;            // round(ch5) bit 0: the exe creates the scene light, else destroys it
    uint8_t type = 1;           // render light type (Light::type 0 -> 2 directional, 1 -> 0 point, 2 -> 1 spot)
    glm::mat4 matrix{1.0f};     // bone matrix * Rx(180 deg) (quaternion (1, 0, 0, 0)): position [3], spot axis [2]
    glm::vec3 colour{1.0f};     // ch0-2 (PCRenderLight 0x24)
    float intensity = 1;        // ch8 (PCRenderLight 0x20)
    float coneInner = 0, coneOuter = 0; // ch3, ch3 + ch9: SceneLight 0x54 -> PCRenderLight 0x34, full angles (radians)
    uint32_t mask = 0, shadowMask = 0;  // round(ch6) | 0x83000000 (SceneLight +0x14), round(ch7) | 0x83000000 (+0x18)
    bool shadowMaskOn = false;  // ch7 != 0: SceneLight 0x58 -> PCRenderLight +0xC5 bit 2
    bool bit2 = false;          // round(ch5) bit 2: PCRenderLight 0x58 -> +0xC5 bit 0 [U]
};

// tru.exe 0x4545C0: the depth of field the cinematic camera writes into its filter resource (EFFECTS.md "Screen
// filters": ColorDOF blur +0x1300, range +0x1310 / +0x1314, overlay alpha curve +0x1200 over that range).
struct DepthOfField {
    float blur = 0, near = 0, far = 0;
    std::array<uint8_t, 256> curve{};
};
DepthOfField depthOfField(float focus, float fStop, float focalLength);

// The frame clock: tru.exe runs the frame position off the stream's playback clock. `audioSeconds` = how far the
// mixer has played the cinematic's sound track (nullopt: no track, or it ended); between the mixer's updates the clock
// runs on by wall time, at most kSlack past the last audio position. Never runs backwards.
class Clock {
public:
    static constexpr double kSlack = 0.1;
    double update(double dt, std::optional<double> audioSeconds);
    double seconds() const { return t_; }

private:
    double t_ = 0, audio_ = -1, since_ = 0;
};

// Playback in the exe's way: the frame position runs at 30 per second; channel values are interpolated
// between frame floor(pos) and floor(pos) + 1 (rotations by quaternion slerp), except across a camera cut.
class Player {
public:
    explicit Player(std::vector<uint8_t> cine); // throws cdc::FormatError
    const Header& header() const { return header_; }
    // Advances to frame position `pos` (>= the last one). Returns false once the stream has ended.
    bool seek(double pos);
    double position() const { return pos_; }
    int frame() const { return int(pos_ + 1e-6); }
    bool finished() const { return finished_; }
    // Scene origin: world = origin * cinematic space; the caller sets it from the area marker (originFromMarker).
    glm::mat4 origin{1.0f};
    // Interpolated values.
    float value(uint32_t channel) const;
    float raw(uint32_t channel) const { return cur_[channel]; } // current frame, not interpolated
    glm::quat rotation(uint32_t channel) const;   // 3 Euler channels, R = Rz * Ry * Rx
    glm::mat4 transform(uint32_t channel) const;  // 9 channels: T * R * S
    CameraPose camera() const;                    // in the world (origin applied)
    // World matrices of instance `inst`'s skeleton: animated bones as 0x454BD0, the skeleton's remaining bones
    // rigidly on their parents. Use with anim::skinPalette.
    std::vector<glm::mat4> bones(const Instance& inst, const scene::Skeleton& skeleton) const;
    // Lights of `inst` posed on `bones` (as returned by bones()).
    std::vector<LightPose> lights(const Instance& inst, std::span<const glm::mat4> bones) const;
    // Per stream bone the 4th skin-palette row (channels 10-13, interpolated): 0x454BD0 passes them to 0x5B60F0
    // (x wetness, y dirt as GAMEPLAY.md "Wetness"; Lara's face bones 22-30 animate z / w).
    std::vector<glm::vec4> boneRows(const Instance& inst) const;
    // Subtitle entries of the frames decoded by the last seek().
    const std::vector<locale::CineSubtitle>& newSubtitles() const { return subs_; }
    // origin = markerIntro * inverse(marker channel transform) (0x457100).
    glm::mat4 originFromMarker(const Marker& m, const glm::mat4& markerIntroWorld) const;

private:
    bool cutAhead() const;
    std::vector<uint8_t> bytes_;
    Header header_;
    Decoder dec_;
    std::vector<float> prev_, cur_;
    double pos_ = 0;
    float frac_ = 0;
    bool finished_ = false, fresh_ = true;
    std::vector<locale::CineSubtitle> subs_;
};

// ---- binding --------------------------------------------------------------------------------------------
// Cinematic stream names of a unit: Level+0x70 -> level data, +0x1DC count, +0x288 char* table (0x4A4D10).
std::vector<std::string> unitCinematics(const cdc::Drm& unit);
// Script section id of native type "cinematic" (cinematic_object.drm, game/script/game/cinematic) [O]. A
// cinematic_object intro script holds one such member {u32 index into unitCinematics, u32 stream unit} [O].
inline constexpr uint32_t kCinematicType = 3521;
// Stream name played by the intro whose script section is `scriptId`, or empty.
std::string introCinematic(const cdc::Drm& unit, uint32_t scriptId);

} // namespace ou::cine
