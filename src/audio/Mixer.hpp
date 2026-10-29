// Software mixer on an SDL3 audio stream: sound registry (DRM Wave sections by section id),
// voices with volume / pan / pitch / loop, 3D attenuation + panning from a listener, buses,
// and a music slot with crossfades and per-stem gains (.mul music = stereo stem pairs).
// mix() is renderer-style pure code; openDevice() attaches it to the default playback device.
#pragma once

#include "audio/Audio.hpp"

#include <glm/vec3.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

struct SDL_AudioStream;

namespace ou::audio {

using VoiceId = uint32_t; // 0 = invalid

enum class Bus : uint8_t { Sfx, Voice, Music, Count };

struct PlayParams {
    float volume = 1;
    float pan = 0;   // -1 left .. 1 right; added to the 3D pan
    float pitch = 1; // playback-rate multiplier
    bool loop = false; // wraps at Pcm::loopEnd (or the end) to Pcm::loopStart (or 0)
    Bus bus = Bus::Sfx;
    std::optional<glm::vec3> position{}; // world position; nullopt = 2D
    float minDistance = 1, maxDistance = 10000; // FSB4 defaults seen in every TRU bank
};

struct Listener {
    glm::vec3 position{0}, forward{0, 1, 0}, up{0, 0, 1}; // game frame is Z-up
};

// gain = f(distance, minDistance, maxDistance). Default: FMOD-style inverse rolloff,
// min / clamp(d, min, max).
using Attenuation = std::function<float(float, float, float)>;

class Mixer {
public:
    explicit Mixer(uint32_t outputRate = 48000);
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;

    // Opens the default SDL playback device (inits the SDL audio subsystem). False on failure;
    // SDL_GetError() has the reason. mix() works without a device.
    bool openDevice();
    void closeDevice();
    uint32_t outputRate() const { return rate_; }

    // Renders `frames` interleaved stereo float frames into out[0 .. 2*frames) (overwrites).
    void mix(float* out, size_t frames);

    // ---- sounds by id (game: Wave section ids) ----
    void addSound(uint32_t id, std::shared_ptr<const Pcm> pcm);
    size_t addWaves(const cdc::Drm& drm); // every Wave section, keyed by section id; returns count
    bool hasSound(uint32_t id) const;
    std::shared_ptr<const Pcm> sound(uint32_t id) const; // null if unknown

    VoiceId play(uint32_t soundId, const PlayParams& p = {}); // 0 if the id is unknown
    VoiceId play(std::shared_ptr<const Pcm> pcm, const PlayParams& p = {});
    // Voice lines (.mul decoded with decodeMul): Bus::Voice, 2D.
    VoiceId playVoice(std::shared_ptr<const Pcm> pcm, float volume = 1);
    void stop(VoiceId v, float fadeSeconds = 0);
    bool playing(VoiceId v) const;
    void setVolume(VoiceId v, float volume);
    void setPosition(VoiceId v, glm::vec3 position);
    void setPitch(VoiceId v, float pitch);

    // ---- music: one slot; a new track crossfades with the current one ----
    // stemGains: per stereo pair (channels 2k, 2k+1); missing entries = 1.
    void playMusic(std::shared_ptr<const Pcm> pcm, float fadeSeconds = 2, std::vector<float> stemGains = {});
    void setMusicStemGains(std::vector<float> stemGains);
    void stopMusic(float fadeSeconds = 2);

    void setListener(const Listener& l);
    void setBusVolume(Bus b, float volume);
    void setAttenuation(Attenuation a);
    size_t activeVoices() const;

private:
    struct Voice {
        VoiceId id;
        std::shared_ptr<const Pcm> pcm;
        PlayParams params;
        double pos = 0, step = 1; // source frames
        float fade = 1, fadeTarget = 1, fadeStep = 0; // per output frame
        bool stopAtSilence = false, done = false;
        std::vector<float> stemGains{};
    };
    VoiceId start(std::shared_ptr<const Pcm> pcm, const PlayParams& p, float fadeIn); // locked
    Voice* find(VoiceId v);
    const Voice* find(VoiceId v) const;
    void fadeTo(Voice& v, float target, float seconds, bool stopAfter);
    static void sdlCallback(void* self, SDL_AudioStream* s, int additional, int total);

    uint32_t rate_;
    mutable std::mutex mutex_;
    std::vector<Voice> voices_;
    std::unordered_map<uint32_t, std::shared_ptr<const Pcm>> sounds_;
    VoiceId nextId_ = 1, music_ = 0;
    Listener listener_;
    float bus_[size_t(Bus::Count)] = {1, 1, 1};
    Attenuation atten_;
    SDL_AudioStream* stream_ = nullptr;
    std::vector<float> scratch_;
};

} // namespace ou::audio
