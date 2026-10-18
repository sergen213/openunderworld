#include "audio/Mixer.hpp"

#include <SDL3/SDL.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace ou::audio {

Mixer::Mixer(uint32_t outputRate) : rate_(outputRate) {}

Mixer::~Mixer() { closeDevice(); }

bool Mixer::openDevice() {
    if (stream_) return true;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    const SDL_AudioSpec spec{SDL_AUDIO_F32, 2, int(rate_)};
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, sdlCallback, this);
    if (!stream_ || !SDL_ResumeAudioStreamDevice(stream_)) {
        closeDevice();
        return false;
    }
    return true;
}

void Mixer::closeDevice() {
    if (!stream_) return;
    SDL_DestroyAudioStream(stream_); // also closes the device it opened
    stream_ = nullptr;
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

void Mixer::sdlCallback(void* self, SDL_AudioStream* s, int additional, int) {
    auto* m = static_cast<Mixer*>(self);
    const size_t frames = size_t(std::max(additional, 0)) / (2 * sizeof(float));
    if (!frames) return;
    if (m->scratch_.size() < frames * 2) m->scratch_.resize(frames * 2);
    m->mix(m->scratch_.data(), frames);
    SDL_PutAudioStreamData(s, m->scratch_.data(), int(frames * 2 * sizeof(float)));
}

void Mixer::mix(float* out, size_t frames) {
    std::fill(out, out + frames * 2, 0.f);
    std::lock_guard lock(mutex_);
    glm::vec3 right = glm::cross(listener_.forward, listener_.up);
    const float rl = glm::length(right);
    right = rl > 1e-6f ? right / rl : glm::vec3(1, 0, 0);

    for (Voice& v : voices_) {
        const Pcm& p = *v.pcm;
        const size_t n = p.frames(), ch = p.channels;
        if (!n || v.done) {
            v.done = true;
            continue;
        }
        // Per-channel stereo gains (header layout or default), times music stem gains.
        float gl[12], gr[12];
        for (size_t c = 0; c < ch && c < 12; ++c) {
            gl[c] = c < p.volLeft.size() ? p.volLeft[c] : (ch == 1 || c % 2 == 0) ? 1.f : 0.f;
            gr[c] = c < p.volRight.size() ? p.volRight[c] : (ch == 1 || c % 2 == 1) ? 1.f : 0.f;
            const float stem = c / 2 < v.stemGains.size() ? v.stemGains[c / 2] : 1.f;
            gl[c] *= stem;
            gr[c] *= stem;
        }
        const size_t chUsed = std::min<size_t>(ch, 12);
        float g = v.params.volume * bus_[size_t(v.params.bus)];
        float pan = v.params.pan;
        if (v.params.position) {
            const glm::vec3 rel = *v.params.position - listener_.position;
            const float d = glm::length(rel);
            g *= atten_ ? atten_(d, v.params.minDistance, v.params.maxDistance)
                        : v.params.minDistance / std::clamp(d, std::max(v.params.minDistance, 1e-6f),
                                                            std::max(v.params.minDistance, v.params.maxDistance));
            if (d > 1e-4f) pan += glm::dot(rel / d, right);
        }
        pan = std::clamp(pan, -1.f, 1.f);
        float pl, pr;
        if (ch == 1) { // equal power
            const float a = (pan + 1) * std::numbers::pi_v<float> / 4;
            pl = std::cos(a);
            pr = std::sin(a);
        } else { // balance
            pl = std::min(1.f, 1 - pan);
            pr = std::min(1.f, 1 + pan);
        }
        pl *= g;
        pr *= g;
        v.step = double(p.sampleRate) * v.params.pitch / rate_;
        // Looping voices wrap at loopEnd (or the end) back to loopStart (or 0).
        const size_t end = v.params.loop && p.loopEnd > 0 && size_t(p.loopEnd) < n ? size_t(p.loopEnd) : n;
        const size_t loopStart = p.loopStart >= 0 && size_t(p.loopStart) < end ? size_t(p.loopStart) : 0;
        const int16_t* s = p.samples.data();

        for (size_t i = 0; i < frames; ++i) {
            if (v.pos >= double(end)) {
                if (!v.params.loop) {
                    v.done = true;
                    break;
                }
                v.pos = loopStart + std::fmod(v.pos - double(end), double(end - loopStart));
            }
            const size_t i0 = size_t(v.pos);
            const size_t i1 = i0 + 1 < end ? i0 + 1 : v.params.loop ? loopStart : i0;
            const float t = float(v.pos - double(i0));
            float L = 0, R = 0;
            for (size_t c = 0; c < chUsed; ++c) {
                const float a = s[i0 * ch + c], b = s[i1 * ch + c];
                const float x = (a + (b - a) * t) * (1.f / 32768.f);
                L += x * gl[c];
                R += x * gr[c];
            }
            out[2 * i] += L * pl * v.fade;
            out[2 * i + 1] += R * pr * v.fade;
            if (v.fadeStep != 0) {
                v.fade += v.fadeStep;
                if ((v.fadeStep > 0) == (v.fade >= v.fadeTarget)) {
                    v.fade = v.fadeTarget;
                    v.fadeStep = 0;
                    if (v.stopAtSilence && v.fade <= 0) {
                        v.done = true;
                        break;
                    }
                }
            }
            v.pos += v.step;
        }
    }
    std::erase_if(voices_, [](const Voice& v) { return v.done; });
}

void Mixer::addSound(uint32_t id, std::shared_ptr<const Pcm> pcm) {
    std::lock_guard lock(mutex_);
    sounds_[id] = std::move(pcm);
}

size_t Mixer::addWaves(const cdc::Drm& drm) {
    size_t count = 0;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Wave) {
            addSound(s.id, std::make_shared<const Pcm>(decodeWave(s.data)));
            ++count;
        }
    return count;
}

bool Mixer::hasSound(uint32_t id) const {
    std::lock_guard lock(mutex_);
    return sounds_.contains(id);
}

VoiceId Mixer::start(std::shared_ptr<const Pcm> pcm, const PlayParams& p, float fadeIn) {
    if (!pcm || !pcm->frames()) return 0;
    Voice v{.id = nextId_++, .pcm = std::move(pcm), .params = p};
    if (nextId_ == 0) nextId_ = 1;
    if (fadeIn > 0) {
        v.fade = 0;
        fadeTo(v, 1, fadeIn, false);
    }
    voices_.push_back(std::move(v));
    return voices_.back().id;
}

VoiceId Mixer::play(uint32_t soundId, const PlayParams& p) {
    std::lock_guard lock(mutex_);
    const auto it = sounds_.find(soundId);
    return it == sounds_.end() ? 0 : start(it->second, p, 0);
}

VoiceId Mixer::play(std::shared_ptr<const Pcm> pcm, const PlayParams& p) {
    std::lock_guard lock(mutex_);
    return start(std::move(pcm), p, 0);
}

VoiceId Mixer::playVoice(std::shared_ptr<const Pcm> pcm, float volume) {
    return play(std::move(pcm), PlayParams{.volume = volume, .bus = Bus::Voice});
}

Mixer::Voice* Mixer::find(VoiceId id) {
    for (Voice& v : voices_)
        if (v.id == id && !v.done) return &v;
    return nullptr;
}
const Mixer::Voice* Mixer::find(VoiceId id) const { return const_cast<Mixer*>(this)->find(id); }

void Mixer::fadeTo(Voice& v, float target, float seconds, bool stopAfter) {
    v.fadeTarget = target;
    v.stopAtSilence = stopAfter;
    if (seconds <= 0 || v.fade == target) {
        v.fade = target;
        v.fadeStep = 0;
        if (stopAfter && target <= 0) v.done = true;
        return;
    }
    v.fadeStep = (target - v.fade) / (seconds * float(rate_));
}

void Mixer::stop(VoiceId id, float fadeSeconds) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(id)) fadeTo(*v, 0, fadeSeconds, true);
}

bool Mixer::playing(VoiceId id) const {
    std::lock_guard lock(mutex_);
    return find(id) != nullptr;
}

void Mixer::setVolume(VoiceId id, float volume) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(id)) v->params.volume = volume;
}

void Mixer::setPosition(VoiceId id, glm::vec3 position) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(id)) v->params.position = position;
}

void Mixer::setPitch(VoiceId id, float pitch) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(id)) v->params.pitch = pitch;
}

void Mixer::playMusic(std::shared_ptr<const Pcm> pcm, float fadeSeconds, std::vector<float> stemGains) {
    std::lock_guard lock(mutex_);
    if (Voice* old = find(music_)) fadeTo(*old, 0, fadeSeconds, true);
    const bool loop = pcm && pcm->loopStart >= 0;
    music_ = start(std::move(pcm), PlayParams{.loop = loop, .bus = Bus::Music}, fadeSeconds);
    if (Voice* v = find(music_)) v->stemGains = std::move(stemGains);
}

void Mixer::setMusicStemGains(std::vector<float> stemGains) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(music_)) v->stemGains = std::move(stemGains);
}

void Mixer::stopMusic(float fadeSeconds) {
    std::lock_guard lock(mutex_);
    if (Voice* v = find(music_)) fadeTo(*v, 0, fadeSeconds, true);
    music_ = 0;
}

void Mixer::setListener(const Listener& l) {
    std::lock_guard lock(mutex_);
    listener_ = l;
}

void Mixer::setBusVolume(Bus b, float volume) {
    std::lock_guard lock(mutex_);
    bus_[size_t(b)] = volume;
}

void Mixer::setAttenuation(Attenuation a) {
    std::lock_guard lock(mutex_);
    atten_ = std::move(a);
}

size_t Mixer::activeVoices() const {
    std::lock_guard lock(mutex_);
    return size_t(std::count_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.done; }));
}

} // namespace ou::audio
