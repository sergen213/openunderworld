#include "audio/MusicPlayer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <strings.h>

namespace ou::audio {

// One player slot (0x528 bytes at 0xCC5260 + slot * 0x528) with its Mus slot (+0x51C -> 0xCCA9A0 + n * 0x178).
struct MusicPlayer::Slot {
    std::string name;           // +0x000 the loaded track; cleared by 0x594D10 once nothing is loaded
    std::string pending;        // +0x100 requested while the slot fades out; requested again when the fade ends
    bool pendingPlay = false;   // +0x500
    int fadeIn = 0;             // +0x50C fade-in frames for the pending / loaded track
    bool play = false;          // +0x505 started, or starts when loaded
    bool fadingOut = false;     // +0x502
    bool keepLoaded = false;    // +0x503 a Load of the fading track: stay loaded (stopped) when the fade ends
    // Mus slot
    bool loaded = false;        // +0x51C != 0
    Track track;
    std::unique_ptr<MusSequencer> seq;
    bool started = false;       // sequence started (0x591BE0, +0x36) and the stream playing
    VoiceId voice = 0;
    float master = 0;           // +0x38
    float fadeLeft = 0;         // +0x3C seconds
    float fadeTarget = 0;       // +0x40
    bool stopAfter = false;     // +0x44: the fade ends in fadeDone
    bool fadeDone = false;      // +0x35 (also: the stream ended, 0x5950A0)
    int section = -2;           // last logged MusSequencer::section()
    std::vector<float> gains() const { return seq ? seq->stemGains() : std::vector<float>{}; }
};

namespace {
bool same(const std::string& a, std::string_view b) { // __stricmp
    return a.size() == b.size() && !strncasecmp(a.data(), b.data(), a.size());
}
std::string fmt(const char* f, auto... a) {
    char b[512];
    std::snprintf(b, sizeof b, f, a...);
    return b;
}
} // namespace

MusicPlayer::MusicPlayer(Mixer& mixer, Loader loader, Log log) : mixer_(mixer), loader_(std::move(loader)), log_(std::move(log)) {
    for (auto& s : slots_) s = std::make_unique<Slot>();
}

MusicPlayer::~MusicPlayer() {
    for (auto& s : slots_) mixer_.stop(s->voice);
}

void MusicPlayer::log(const std::string& line) const {
    if (log_) log_(line);
}

std::string MusicPlayer::name(int index) const { return slots_[size_t(index)]->name; }

// 0x593950: the stream stops at once, the Mus slot is freed, and 0x583CA0 clears the name and the flags +0x501..+0x506;
// the pending name (+0x100, +0x500, +0x50C) stays.
void MusicPlayer::unload(Slot& s) {
    if (!s.loaded) return;
    mixer_.stop(s.voice);
    s.voice = 0, s.loaded = s.started = false, s.seq.reset(), s.track = {};
    s.master = s.fadeLeft = s.fadeTarget = 0, s.stopAfter = s.fadeDone = false, s.section = -2;
    s.name.clear(), s.play = s.fadingOut = s.keepLoaded = false;
}

// 0x591BE0 restarts the sequence, the master drops to 0 (+0x38 = 0, 0x5864E0), then the stream starts and the master
// fades to 1 over the fade-in frames (0x589370; 0 frames: 1 at once) — 0x593A50 after a load, 0x594D10 (+0x506) once
// the stream is ready, both in the frame of the request for us (our loads are synchronous).
void MusicPlayer::startTrack(Slot& s, int fadeInFrames) {
    mixer_.stop(s.voice);
    if (s.track.mus) s.seq = std::make_unique<MusSequencer>(*s.track.mus);
    s.section = -2, s.started = true, s.fadeDone = false;
    s.master = 0, s.stopAfter = false;
    if (fadeInFrames != 0) s.fadeLeft = float(fadeInFrames) * kFrameSeconds, s.fadeTarget = 1;
    else s.master = 1, s.fadeLeft = 0;
    s.voice = mixer_.playMusic(s.track.pcm, s.master, s.gains());
}

// 0x58BD50: +0x505 = play; the name becomes the slot's track when the load is issued.
bool MusicPlayer::loadTrack(int index, const std::string& n, bool play) {
    Slot& s = *slots_[size_t(index)];
    s.name = n, s.play = play;
    auto t = loader_ ? loader_(n) : std::nullopt;
    if (!t || !t->pcm || !t->pcm->frames()) {
        log(fmt("music slot %d: cannot load '%s'", index, n.c_str()));
        return false; // the name stays: not retried every frame
    }
    s.track = std::move(*t), s.loaded = true, s.started = false;
    if (play) startTrack(s, s.fadeIn);
    log(fmt("music slot %d %s %s%s", index, play ? "plays" : "loads", n.c_str(),
            play ? (s.fadeIn ? fmt(" (fade in %.2f s)", s.fadeIn * kFrameSeconds).c_str() : " (at once)") : ""));
    return true;
}

void MusicPlayer::request(int index, std::string_view n, int fadeOut, int fadeIn, bool play) {
    if (index < 0 || index >= kSlots) return;
    Slot& s = *slots_[size_t(index)];
    if (!n.empty()) {
        if (same(s.name, n)) { // the slot's own track: never restarted
            if (s.fadingOut) {
                if (!play) { // a Load: let the fade finish, keep the track loaded
                    s.keepLoaded = true;
                    return;
                }
                if (fadeIn != 0) s.fadeLeft = float(fadeIn) * kFrameSeconds, s.fadeTarget = 1, s.stopAfter = false; // back up
                else s.master = 1, s.fadeLeft = 0, s.stopAfter = false;
                s.fadingOut = s.keepLoaded = false, s.pending.clear(), s.pendingPlay = false;
                log(fmt("music slot %d %s fades back in (%.2f s)", index, s.name.c_str(), fadeIn * kFrameSeconds));
            }
            if (play && !s.play && s.loaded) { // loaded by a Load: start it
                s.play = true, s.fadeIn = fadeIn;
                startTrack(s, fadeIn);
                log(fmt("music slot %d plays %s (fade in %.2f s)", index, s.name.c_str(), fadeIn * kFrameSeconds));
            }
            return;
        }
        s.pending = n, s.pendingPlay = play, s.fadeIn = fadeIn;
        if (s.loaded) {
            if (fadeOut > 0 && s.play) { // fade the slot's track out first; 0x594D10 requests the pending one after
                s.stopAfter = true, s.fadeLeft = float(fadeOut) * kFrameSeconds, s.fadeTarget = 0;
                s.fadingOut = true, s.keepLoaded = false;
                log(fmt("music slot %d %s fades out (%.2f s), then %s", index, s.name.c_str(), fadeOut * kFrameSeconds,
                        s.pending.c_str()));
                return;
            }
            log(fmt("music slot %d %s stops", index, s.name.c_str()));
            unload(s);
        }
        const std::string next = std::exchange(s.pending, {});
        s.pendingPlay = false;
        loadTrack(index, next, play);
        return;
    }
    // Stop
    if (fadeOut < 1 || !s.loaded || !s.play) {
        if (s.loaded) log(fmt("music slot %d %s stops", index, s.name.c_str()));
        unload(s);
    } else if (!s.fadingOut) {
        s.stopAfter = true, s.fadeLeft = float(fadeOut) * kFrameSeconds, s.fadeTarget = 0;
        s.fadingOut = true, s.keepLoaded = false;
        log(fmt("music slot %d %s fades out (%.2f s)", index, s.name.c_str(), fadeOut * kFrameSeconds));
    }
    s.pending.clear(), s.pendingPlay = false;
}

void MusicPlayer::update(float dt, std::span<uint8_t, 256> vars) {
    // 0x594D10 per player slot.
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = *slots_[size_t(i)];
        if (s.loaded && s.fadeDone) {
            if (s.keepLoaded) { // stays loaded, stopped and silent
                s.fadeDone = s.fadingOut = s.keepLoaded = s.play = false;
                mixer_.stop(s.voice), s.voice = 0, s.started = false;
                log(fmt("music slot %d %s faded out, stays loaded", i, s.name.c_str()));
            } else {
                log(fmt("music slot %d %s faded out", i, s.name.c_str()));
                unload(s);
            }
            continue;
        }
        if (!s.loaded) s.name.clear(); // a load that failed
        if (!s.fadingOut && !s.pending.empty() && !same(s.name, s.pending)) {
            const std::string next = std::exchange(s.pending, {});
            request(i, next, 0, s.fadeIn, s.pendingPlay);
        }
    }
    // 0x595250: master fades by the game frame time (0x58ADE0), then the stream clock and the sequencer (0x5950A0).
    bool fading = false;
    for (int i = 0; i < kSlots; ++i) {
        Slot& s = *slots_[size_t(i)];
        if (!s.loaded) continue;
        if (std::abs(s.fadeLeft) > 0.01f) {
            fading = true;
            if (s.fadeLeft <= dt) s.master = s.fadeTarget, s.fadeLeft = 0;
            else s.master += (s.fadeTarget - s.master) / s.fadeLeft * dt, s.fadeLeft -= dt;
            if (s.fadeLeft < 0.01f && s.stopAfter) s.fadeDone = true;
        }
        if (!s.started) continue;
        const auto pos = mixer_.position(s.voice);
        if (!pos) { // the stream ended (non-looping): 0x5950A0 stops the sequence and flags the slot done
            s.fadeDone = true, s.started = false, s.voice = 0;
            continue;
        }
        if (s.seq) {
            s.seq->updateAt(*pos, vars);
            if (s.seq->section() != s.section) {
                s.section = s.seq->section();
                std::string g;
                for (float x : s.seq->stemGains()) g += fmt(" %.3f", x);
                log(fmt("music slot %d %s beat %.2f: section L%d (vars 7B=%u 7C=%u 7D=%u 0D=%u), stem gains%s", i,
                        s.name.c_str(), s.seq->beats(), s.section, vars[0x7B], vars[0x7C], vars[0x7D], vars[0x0D], g.c_str()));
            }
        }
        mixer_.setMusic(s.voice, s.master, s.gains(), dt);
    }
    // Fade progress twice a second while a master fade runs (the overlap of a crossfade shows here).
    gainLog_ = fading ? gainLog_ + dt : 0;
    if (fading && gainLog_ >= 0.5f - 1e-4f) {
        gainLog_ = 0;
        std::string line = "music masters:";
        for (int i = 0; i < kSlots; ++i)
            if (const Slot& s = *slots_[size_t(i)]; s.loaded && s.started)
                line += fmt(" slot %d %s %.3f%s", i, s.name.c_str(), s.master,
                            s.fadeLeft > 0.01f ? (s.fadeTarget > s.master ? " (in)" : " (out)") : "");
        log(line);
    }
}

void MusicPlayer::reset() {
    for (auto& s : slots_) {
        unload(*s);
        s->name.clear(), s->pending.clear(), s->pendingPlay = s->play = s->fadingOut = s->keepLoaded = false, s->fadeIn = 0;
    }
    gainLog_ = 0;
}

MusicPlayer::SlotView MusicPlayer::slot(int i) const {
    const Slot& s = *slots_[size_t(std::clamp(i, 0, kSlots - 1))];
    return {s.name, s.pending, s.play, s.fadingOut, s.master, s.voice};
}

bool MusicPlayer::idle() const {
    return std::ranges::all_of(slots_, [](const auto& s) { return s->name.empty() && s->pending.empty() && !s->loaded; });
}

} // namespace ou::audio
