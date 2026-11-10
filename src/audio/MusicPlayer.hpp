// tru.exe's music player: 8 slots (0xCC5260 + slot * 0x528), each holding one track (.mus script + .mul stream, a
// MusSequencer and a Mixer voice) with its own master gain fade (the slot's Mus slot +0x38..+0x44). Requests follow
// 0x593EB0: a new track in a slot first fades the slot's playing track out, then loads and fades in; tracks in
// different slots fade independently, which is how the scripts crossfade (play in slot 1, stop slot 0). A request for
// the track a slot already holds does not restart it. Layout and evidence: docs/formats/AUDIO.md §9.9.
#pragma once

#include "audio/Audio.hpp"
#include "audio/Mixer.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace ou::audio {

class MusicPlayer {
public:
    static constexpr int kSlots = 8;          // 0xCC5260 .. 0xCC80C4 step 0x528 [O]
    static constexpr float kFrameSeconds = 0.033333335f; // fades are counted in 30 Hz frames (natives: ftol(s * 30))

    struct Track {
        std::shared_ptr<const Pcm> pcm;      // the .mul
        std::shared_ptr<const MusFile> mus;  // null: no .mus (stems at full gain, clock unused)
    };
    // name -> track; nullopt when it cannot be loaded (the exe reports "Cannot load music files for the '%s' music!").
    using Loader = std::function<std::optional<Track>(const std::string& name)>;
    using Log = std::function<void(const std::string&)>;

    MusicPlayer(Mixer& mixer, Loader loader, Log log = {});
    ~MusicPlayer();
    MusicPlayer(const MusicPlayer&) = delete;
    MusicPlayer& operator=(const MusicPlayer&) = delete;

    // 0x593EB0(name, fade-out frames, fade-in frames, play): the `music` natives 1 Load(fade-out s) = (name, out, 0,
    // false), 2 Play(fade-out s, fade-in s) = (name, out, in, true), 3 Stop(fade-out s) = ("", out, 0, true).
    void request(int slot, std::string_view name, int fadeOutFrames, int fadeInFrames, bool play);
    void load(int slot, std::string_view name, float fadeOutSeconds) { request(slot, name, frames(fadeOutSeconds), 0, false); }
    void play(int slot, std::string_view name, float fadeOutSeconds, float fadeInSeconds) {
        request(slot, name, frames(fadeOutSeconds), frames(fadeInSeconds), true);
    }
    void stop(int slot, float fadeOutSeconds) { request(slot, "", frames(fadeOutSeconds), 0, true); }
    // Seconds -> frames as the natives convert them (fmul 30.0, _ftol 0x814D20: truncation).
    static int frames(float seconds) { return seconds > 0 ? int(seconds * 30.0) : 0; }

    // One game update of `seconds` (0x594D10 per slot, then 0x595250: master fades by the game frame time, every
    // started sequencer clocked by its stream's play position). vars: the global music variables (0xCCB560).
    void update(float seconds, std::span<uint8_t, 256> vars);
    // Everything stops at once, pending requests dropped (0x593D10 -> 0x593BE0 on a level change, load, quit).
    void reset();

    struct SlotView {
        std::string track;   // loaded track (+0x000), empty: none
        std::string pending; // waits for the fade-out (+0x100)
        bool playing = false; // started / starts when loaded (+0x505)
        bool fadingOut = false;
        float master = 0;     // Mus slot +0x38
        VoiceId voice = 0;
    };
    SlotView slot(int i) const;
    bool idle() const; // no slot holds, plays or waits for a track

private:
    struct Slot;
    void unload(Slot& s);
    void startTrack(Slot& s, int fadeInFrames); // 0x591BE0 + master 0, then the fade in (0x593A50 / 0x594D10 0x506)
    bool loadTrack(int index, const std::string& name, bool play);
    void log(const std::string& line) const;
    std::string name(int index) const;

    Mixer& mixer_;
    Loader loader_;
    Log log_;
    std::array<std::unique_ptr<Slot>, kSlots> slots_;
    float gainLog_ = 0; // seconds since the last fade-progress log line
};

} // namespace ou::audio
