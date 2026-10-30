// TR8 SoundPlex sound graphs (DTP data), sfxmarker ambient emitters and a player that runs them on the Mixer.
// Layouts and evidence: docs/formats/AUDIO.md §9.
#pragma once

#include "audio/Mixer.hpp"

#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace ou::audio {

// Node type = tru.exe SoundPlexCollection::Create (0x5920B0) switch case.
enum class PlexType : uint8_t {
    Silence = 0, Reference, Wave, Stream, Assignment, Envelope, ChoiceList, MaterialList, Selector, Reaction,
    Sequence, Overlap, Effect, Oscillator,
};
struct Plex {
    PlexType type = PlexType::Silence;
    uint32_t id = 0;        // Reference: DTP id; Wave: Wave section id
    std::string stream;     // Stream: .mul path without extension ("vo\\lara\\sc_01_05_hint")
    float volume = 1, volumeVar = 0; // Assignment: volume ± variance; Stream: u16 +6 percent / 100
    float delay = 0, delayVar = 0;   // Assignment start delay (s); Silence: duration in `delay`
    float maxDistance = 0;  // Assignment float [14] when > 0 [I]
    uint8_t mode = 0;       // ChoiceList pick: 0 random, 1 random without repeat, 2 shuffle, 3 in order (0x587920)
    std::vector<Plex> children;
    std::vector<std::pair<float, float>> delays; // Overlap per-child start delay {value, variance}
    // MaterialList (0x583960) / Selector (0x583490): children are References to DTP ids, picked by key (surface
    // material; Selector: 0xFFFFFFFF = default entry).
    std::vector<uint32_t> keys;
    // Envelope (0x590CE0 / 0x587A90 / 0x587B80): curves of {time s, value} keys from key 0, linear between keys; param 0
    // = volume [I: shape]. Reaching key loopEnd while not released jumps back to loopStart (loopStart == loopEnd: holds
    // there = sustain); loopEnd without loopStart only marks the release point. Stopping releases: `release` curves
    // (u16 +2 == 0) go on from loopEnd (else the last key) towards the following keys; an envelope without such a curve
    // or a sustain stops at once. A volume curve past its last key with value 0 ends the voice.
    struct Curve {
        uint16_t param = 0;
        bool release = false;
        int32_t loopStart = -1, loopEnd = -1; // header ints, overridden by key flags bit 0 / bit 1
        std::vector<std::pair<float, float>> keys;
        float value(float t, float releasedAt, bool* ended = nullptr) const; // at t s after the envelope started
    };
    std::vector<Curve> curves;
    // Reaction (0x590F30 / 0x592640 / 0x587FF0): game parameters (SoundPlayer::setParam) mapped onto the child's
    // volume (output 0) / pitch in semitones (output 1): x in [inMin, inMax] -> normalised -> piecewise-linear curve ->
    // outMin + (outMax - outMin) * y; `add` combines additively (map bit 0x100), else multiplies.
    struct Map {
        uint8_t input = 0, output = 0;
        bool add = false, random = false; // random: input byte 0xFF, a uniform pick in [outMin, outMax]
        bool absolute = true, negate = false; // map bits 12-13: 0 negative inputs use |x|, 2 always negated
        bool zeroBelow = false, zeroAbove = false; // map bits 0x4000 / 0x8000: 0 outside [inMin, inMax]
        float inMin = 0, inMax = 1, outMin = 0, outMax = 1;
        std::vector<std::pair<float, float>> keys;
    };
    std::vector<Map> maps;
    mutable uint32_t counter = 0; // ChoiceList state (last pick / shuffle mask), as data +0x10 in the exe
};
// Node {i32 type, ptr data} at `offset` of drm.sections[section]. Oscillator (an FMOD tone DSP, 0x587C60) parses
// without data: no retail graph reached from markers or animation events uses it. Throws FormatError.
Plex parsePlex(const cdc::Drm& drm, uint32_t section, uint32_t offset);

// DTP id -> SoundPlex graph at offset 0 of that DTP section, parsed on first use.
class PlexBank {
public:
    void add(std::shared_ptr<const cdc::Drm> drm); // indexes its DTP sections (first DRM added wins an id)
    const Plex* find(uint32_t dtpId);              // null if unknown or not a SoundPlex
private:
    std::map<uint32_t, std::pair<std::shared_ptr<const cdc::Drm>, uint32_t>> sections_;
    std::map<uint32_t, std::unique_ptr<Plex>> parsed_;
};

// ---- sfxmarker (unit data +0xAC count / +0xB0 pointers, 0x3C bytes each; trigger update 0x594A80) ----
struct SfxTrigger {
    enum Condition : uint8_t { Init = 0, Enter = 1, Exit = 2 };
    enum Action : uint8_t { Start = 0, End = 1, SetVariable = 2, IncVariable = 3, DecVariable = 4 };
    uint8_t condition = Init, action = Start;
    float radius = 0;
    bool always = false;        // perimeter bAlways: may fire more than once
    std::vector<int32_t> refs;  // Start/End: indices into SfxMarker::sounds
    uint8_t variable = 0, value = 0; // music variable actions
};
struct SfxMarker {
    glm::vec3 position{0}; // unit frame
    uint32_t id = 0;       // +0x20
    std::vector<Plex> sounds;
    std::vector<SfxTrigger> triggers;
};
std::vector<SfxMarker> parseSfxMarkers(const cdc::Drm& unit); // Throws FormatError

// Perimeter state machine of every trigger. update() returns the triggers that fire this frame.
class SfxMarkerTriggers {
public:
    explicit SfxMarkerTriggers(const std::vector<SfxMarker>& markers);
    struct Fired { size_t marker, trigger; };
    std::vector<Fired> update(const std::vector<SfxMarker>& markers, glm::vec3 player); // player in the unit frame
private:
    struct State { bool armed = false, inside = false; uint8_t count = 0; };
    std::vector<std::vector<State>> state_;
};

// 3D distance rolloff, tru.exe 0x5850E0 -> 0x585070 with globalsoundinfo.drm's object data +0x28 {f32 max distance 3160.56,
// f32 min distance 158.03, f32 curve 1.0}: f = (max - d) / (max - min) clamped to [0, 1], gain = (f² - f)·curve + f (= f²).
// An Assignment's distance [14] replaces the max (0x5850E0: the emitter's +0 when > 0). The listener is halfway between
// the camera and Lara (0x57FD50 with globalsoundinfo +0x20 = 0.5; pan weight +0x24 = 0).
inline constexpr float kSoundMinDistance = 158.028f, kSoundMaxDistance = 3160.56f, kSoundListenerToPlayer = 0.5f;
float rolloff(float distance, float minDistance, float maxDistance); // Mixer::setAttenuation for the game's sounds

// ---- player ----
class SoundPlayer {
public:
    using Resolve = std::function<const Plex*(uint32_t dtpId)>;
    using LoadStream = std::function<std::shared_ptr<const Mul>(const std::string& path)>; // null = missing
    SoundPlayer(Mixer& mixer, Resolve resolve, LoadStream stream) : mixer_(mixer), resolve_(std::move(resolve)),
                                                                 stream_(std::move(stream)) {}
    using Handle = uint32_t; // 0 = nothing started
    // `material`: the surface material MaterialList nodes pick by (sound instance +0x44, AUDIO.md §9.7).
    Handle start(const Plex& p, std::optional<glm::vec3> position = {}, float volume = 1, uint16_t material = 0);
    void stop(Handle h, float fadeSeconds = 0.2f);
    void stopAll(float fadeSeconds = 0); // every sound, pending parts too (leaving play: tru.exe 0x593D10)
    bool active(Handle h) const; // a voice still plays or a delayed/sequenced part is pending
    void setParam(Handle h, size_t index, float value); // Reaction input (game parameter) of a running sound
    void setPosition(Handle h, glm::vec3 position);     // a 3D sound's emitter moved
    void update(double seconds);
    // Started streams (voice lines): path plus its English subtitle text, oldest first; cleared by the caller.
    struct Line { std::string path, subtitle; };
    std::vector<Line> lines;
    std::vector<std::string> waves; // "0x<id>" of every wave started, for logs; cleared by the caller
private:
    // An Envelope / Reaction above a voice and when it started; also an Assignment with a max distance (innermost wins).
    struct Mod { const Plex* plex; double start; };
    using Mods = std::vector<Mod>;
    struct Pending { double at; const Plex* plex; float volume; size_t index; Mods mods; };
    struct Voice { VoiceId id; float volume; Mods mods; };
    struct Instance {
        std::optional<glm::vec3> position;
        uint16_t material = 0;
        std::array<float, 16> params{};  // Reaction inputs (sound instance +0x28)
        std::vector<Voice> voices;
        std::vector<Pending> pending;
        double released = -1; // stop() time while release envelopes play out
    };
    bool apply(Instance& in, Voice& v); // the voice's Envelope / Reaction volume and pitch; false when an envelope ended
    float mapValue(const Plex::Map& m, float x);
    double fire(Instance& in, const Plex& p, float volume, double at, const Mods& mods, int depth = 0); // returns the duration
    float vary(float v, float var);
    Mixer& mixer_;
    Resolve resolve_;
    LoadStream stream_;
    std::map<Handle, Instance> instances_;
    Handle next_ = 1;
    double now_ = 0;
    std::mt19937 rng_{0x50D}; // fixed seed: replays pick the same variations
};

} // namespace ou::audio
