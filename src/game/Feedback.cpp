#include "game/Feedback.hpp"

#include "game/Game.hpp" // World::bindFeedback
#include "script/Vm.hpp" // vm::Value

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>

#include "cdc/Cdc.hpp"

namespace ou::game {
namespace {
template <class T> T rd(std::span<const uint8_t> d, size_t o) {
    if (o + sizeof(T) > d.size()) throw std::runtime_error("padshock: truncated");
    T v;
    std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}
constexpr float kFrames = 1.0f / 4096.0f / 30.0f; // 0x481C70: duration * 0.00024414062 * 0.033333335 s
} // namespace

std::vector<Padshock> parsePadshockLib(std::span<const uint8_t> d) {
    std::vector<Padshock> out;
    const uint32_t n = rd<uint32_t>(d, 4);
    if (n > 4096) throw std::runtime_error("padshock: bad count");
    for (uint32_t i = 0; i < n; ++i) {
        const size_t o = rd<uint32_t>(d, 0xC + 8 * i);
        Padshock p;
        for (size_t k = o + 8; k < o + 0x48 && rd<char>(d, k); ++k) p.name += rd<char>(d, k);
        const int32_t channels = rd<int32_t>(d, o + 0x48);
        p.loop = rd<int32_t>(d, o + 0x4C) == 1, p.loopStart = rd<float>(d, o + 0x50), p.loopEnd = rd<float>(d, o + 0x54);
        if (channels < 0 || channels > 8) throw std::runtime_error("padshock: bad channel count");
        size_t c = o + 0x60;
        for (int32_t k = 0; k < channels; ++k) {
            const int32_t keys = rd<int32_t>(d, c);
            if (keys < 0 || keys > 4096) throw std::runtime_error("padshock: bad key count");
            Padshock::Channel ch{rd<int32_t>(d, c + 4), rd<float>(d, c + 12), rd<float>(d, c + 16), {}};
            for (int32_t j = 0; j < keys; ++j) ch.keys.push_back({rd<float>(d, c + 20 + 8 * j), rd<float>(d, c + 24 + 8 * j)});
            p.channels.push_back(std::move(ch));
            c += 4 * (2 * size_t(keys) + 5);
        }
        out.push_back(std::move(p));
    }
    return out;
}

void Rumble::shock0(int on, int duration) { shockTime_[0] = float(duration) * kFrames, shockLevel_[0] = on ? 1.0f : 0.0f; }
void Rumble::shock1(int level, int duration) { shockTime_[1] = float(duration) * kFrames, shockLevel_[1] = float(level) / 255.0f; }

uint32_t Rumble::play(std::string_view name, float rate, float scale) {
    const auto seq = std::find_if(lib_.begin(), lib_.end(), [&](const Padshock& p) {
        return p.name.size() == name.size() && std::equal(name.begin(), name.end(), p.name.begin(), [](char a, char b) {
                   return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); // __stricmp
               });
    });
    if (seq == lib_.end()) return 0;
    auto free = std::find_if(slots_.begin(), slots_.end(), [](const Slot& s) { return !s.active; });
    Slot& s = free != slots_.end() ? *free : slots_[0]; // equal priorities: the first slot
    s = {&*seq, true, false, 0, rate, scale, nextHandle_};
    nextHandle_ += 2;
    return s.handle;
}

Rumble::Slot* Rumble::find(uint32_t h) {
    for (auto& s : slots_)
        if (s.active && s.handle == h) return &s;
    return nullptr;
}
const Rumble::Slot* Rumble::find(uint32_t h) const { return const_cast<Rumble*>(this)->find(h); }

void Rumble::stop(uint32_t h, bool release) {
    if (Slot* s = find(h)) {
        if (!release) *s = {};
        else if (s->seq->loop) s->released = true;
    }
}
bool Rumble::playing(uint32_t h) const { return find(h); }
bool Rumble::looping(uint32_t h) const {
    const Slot* s = find(h);
    return s && s->seq->loop;
}
void Rumble::setRate(uint32_t h, float rate) {
    if (Slot* s = find(h)) s->rate = rate;
}
void Rumble::setScale(uint32_t h, float scale) {
    if (Slot* s = find(h)) s->scale = scale;
}

float Rumble::eval(Slot& s, size_t channel) {
    if (!s.active || channel >= s.seq->channels.size()) return 0;
    const auto& c = s.seq->channels[channel];
    // The keys around `time`: the last at or before it, the first after it; none after it ends the sequence.
    size_t next = 0;
    while (next < c.keys.size() && c.keys[next].x <= s.time) ++next;
    if (next >= c.keys.size()) {
        s = {};
        return 0;
    }
    const glm::vec2 b = c.keys[next], a = c.keys[next ? next - 1 : 0];
    if (c.interpolation == 2) return s.scale * a.y;
    if (c.interpolation != 1) return 0;
    const float f = b.x > a.x ? (s.time - a.x) / (b.x - a.x) : 0.0f;
    return s.scale * std::clamp((a.y + f * (b.y - a.y)) * c.scale + c.offset, 0.0f, 1.0f);
}

void Rumble::tick(float dt) {
    for (Slot& s : slots_) // 0x481160
        if (s.active) {
            s.time += s.rate * dt;
            if (s.seq->loop && !s.released && s.time > s.seq->loopEnd && s.seq->loopEnd > s.seq->loopStart)
                while (s.time > s.seq->loopEnd) s.time -= s.seq->loopEnd - s.seq->loopStart;
        }
    std::array<float, 2> sum{};
    for (Slot& s : slots_) {
        if (!s.active) continue;
        // 0x481030: channels with an interpolation go to the motors in order (both XInput motors take either kind).
        size_t motor = 0;
        for (size_t c = 0; s.seq && c < s.seq->channels.size() && motor < sum.size(); ++c)
            if (s.seq->channels[c].interpolation) sum[motor++] += eval(s, c);
    }
    for (size_t m = 0; m < 2; ++m) // the shocks: channel 0 (type 2) -> motor 0, channel 1 (type 1) -> motor 1
        if (shockTime_[m] > 0) {
            if ((shockTime_[m] -= dt) <= 0) shockTime_[m] = 0, shockLevel_[m] = 0;
            sum[m] += shockLevel_[m];
        }
    for (size_t m = 0; m < 2; ++m) motors_[m] = std::min(sum[m], 1.0f);
}

std::map<uint32_t, ShakeData> parseCameraShakes(std::span<const uint8_t> bytes) {
    const cdc::Drm drm = cdc::parseDrm({bytes.begin(), bytes.end()});
    std::map<uint32_t, ShakeData> out;
    for (const auto& s : drm.sections) {
        if (s.type != cdc::SectionType::DtpData || s.data.size() != 0x3E0) continue; // the shakes; the key table is
        ShakeData& d = out[s.id];                                                     // the ObjectBase's
        for (size_t c = 0; c < d.channels.size(); ++c) {
            auto& ch = d.channels[c];
            std::memcpy(ch.envelope.data(), s.data.data() + 0x8C * c, ch.envelope.size());
            ch.amplitude = rd<float>(s.data, 0x8C * c + 0x80), ch.period = rd<float>(s.data, 0x8C * c + 0x84);
            ch.random = rd<float>(s.data, 0x8C * c + 0x88);
        }
        d.duration = rd<float>(s.data, 0x3D4), d.radius = rd<float>(s.data, 0x3D8), d.positional = rd<uint32_t>(s.data, 0x3DC) != 0;
    }
    return out;
}

bool CameraShake::start(uint32_t id, float duration) {
    const auto it = lib_.find(id);
    if (it == lib_.end() || !(duration > 0)) return false;
    // 0x5E86F0: a free slot or one whose shake ran out.
    const auto s = std::find_if(slots_.begin(), slots_.end(), [](const Slot& x) { return !x.data || x.time >= x.duration; });
    if (s == slots_.end()) return false;
    *s = {&it->second, id, 0, duration, {}};
    return true;
}

void CameraShake::stop(uint32_t id) {
    for (Slot& s : slots_)
        if (s.data && s.id == id) s = {};
}

bool CameraShake::active() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const Slot& s) { return s.data; });
}

// value = amplitude * envelope(tn) * sin(2 pi t / period) * (1 - r + 2 r u), r = randomness / 100, u uniform in [0, 1],
// period >= 0.01 s; envelope: the 128-byte table at tn in 0..1, linear between entries, / 255.
float CameraShake::channel(const ShakeData::Channel& c, float t, float tn, float scale) {
    const float r = c.random * 0.01f;
    const float x = std::clamp(tn, 0.0f, 1.0f) * 127.0f;
    const int i = int(x), j = std::min(i + 1, 127);
    const float env = float(int((float(c.envelope[size_t(j)]) - float(c.envelope[size_t(i)])) * (x - float(i)) +
                                float(c.envelope[size_t(i)])) & 0xFF) / 255.0f;
    rng_ = rng_ * 1664525u + 1013904223u;
    const float u = float((rng_ >> 9) & 0x7FFFFF) / 8388607.0f;
    const float period = std::max(c.period, 0.01f);
    return c.amplitude * scale * env * std::sin(t / period * 6.2831854820251465f) * (1 - r + 2 * r * u);
}

void CameraShake::tick(float dt) {
    prev_ = now_, now_ = {};
    for (Slot& s : slots_) {
        if (!s.data) continue;
        if (s.time >= s.duration) { // script shakes (0x84 set) free their slot; none loops (0x87)
            s = {};
            continue;
        }
        const float t = s.time, tn = t / s.duration;
        s.time += dt;
        const auto& c = s.data->channels;
        constexpr float kUnits = 3.1605000495910645f, kDeg = 0.01745329238474369f;
        s.out.move = {channel(c[0], t, tn, kUnits), channel(c[1], t, tn, kUnits), channel(c[2], t, tn, kUnits)};
        const float x = channel(c[4], t, tn, kDeg), y = channel(c[5], t, tn, kDeg), z = channel(c[3], t, tn, kDeg); // 0x5F3190 order
        s.out.turn = {x, y, z};
        s.out.fov = channel(c[6], t, tn, kDeg);
        // Intensity 1: positional falloff (0x5EB460) only applies to shakes started at a point (animation events).
        now_.move += s.out.move, now_.turn += s.out.turn, now_.fov += s.out.fov;
    }
}

CameraShake::Offset CameraShake::offset(float alpha) const {
    return {glm::mix(prev_.move, now_.move, alpha), glm::mix(prev_.turn, now_.turn, alpha), prev_.fov + (now_.fov - prev_.fov) * alpha};
}

// ---- script natives ------------------------------------------------------------------------------------

void World::bindFeedback() {
    script_.setFeedbackNative([this](bool shake, uint64_t key, const script::Member& m, uint32_t value, uint32_t function,
                                     std::span<const script::vm::Value> args, uint32_t& result) {
        const auto f = [&](size_t i) { return std::bit_cast<float>(m.initWords[i]); };
        const auto arg = [&] { return args.size() == 1 ? std::bit_cast<float>(args[0].bits) : 0.0f; };
        if (shake) { // camerashake: the value is the shake's DTP id (camerashakes.drm, DAT_00C36F28 resource table)
            if (function == 0) return cameraShake.start(value, arg()), true; // Start(seconds) 0x778600 -> 0x5EE8C0
            if (function == 1) return cameraShake.stop(value), true;          // Stop 0x778620 -> 0x5E8750
            return false;
        }
        // padshock {char* name, f32 library, rate, scale | on, duration 0, level 1, duration 1} (NtPadshockInst 0x4AC800
        // +0x10..+0x28); durations / 4096 frames. Play (0x4AA540): "shock" / "shock0" / "shock1" set the shocks
        // (0x481C70 / 0x481CC0 / 0x481CF0), any other name plays that library sequence at the rate and scale.
        const auto i = [](float v) { return int(v); }; // _ftol
        const auto is = [&](std::string_view s) {
            return m.initText.size() == s.size() &&
                   std::equal(s.begin(), s.end(), m.initText.begin(), [](char a, char b) { return a == std::tolower(uint8_t(b)); });
        };
        uint32_t& handle = padshocks_[key];
        switch (function) {
        case 1: case 3: // Play; 3 also returns an NtPadshockSequence object [U: none modelled], 2 only that object
            if (is("shock")) rumble.shock0(i(f(2)), i(f(3))), rumble.shock1(i(f(4)), i(f(5)));
            else if (is("shock0")) rumble.shock0(i(f(2)), i(f(3)));
            else if (is("shock1")) rumble.shock1(i(f(4)), i(f(5)));
            else handle = rumble.play(m.initText, f(1), f(2));
            return true;
        case 4: case 5: rumble.stop(handle, function == 4); return true; // 0x483130: 4 lets a loop finish
        case 6: result = rumble.playing(handle); return true;
        case 7: result = rumble.looping(handle); return true;
        case 8: rumble.setRate(handle, arg()); return true;
        case 9: rumble.setScale(handle, arg()); return true;
        case 10: case 11: return true; // debug prints
        default: return false;
        }
    });
}

} // namespace ou::game
