// Mus! music scripts. Layout and exe evidence: docs/formats/AUDIO.md §5.
#include "audio/Audio.hpp"

#include <algorithm>
#include <cstring>

namespace ou::audio {
namespace {

uint32_t u32(std::span<const uint8_t> d, size_t o) {
    if (o > d.size() || d.size() - o < 4) throw FormatError("mus: truncated");
    uint32_t v;
    std::memcpy(&v, d.data() + o, 4);
    return v;
}
uint8_t u8(std::span<const uint8_t> d, size_t o) {
    if (o >= d.size()) throw FormatError("mus: truncated");
    return d[o];
}
float f32(std::span<const uint8_t> d, size_t o) {
    const uint32_t v = u32(d, o);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
std::string name20(std::span<const uint8_t> d, size_t o) { // char[0x14], NUL optional
    u32(d, o + 0x10);
    const char* s = reinterpret_cast<const char*>(d.data() + o);
    return std::string(s, strnlen(s, 0x14));
}
MusKeyRange keyRange(std::span<const uint8_t> d, size_t o) {
    return {u8(d, o), u8(d, o + 1), u8(d, o + 2), u8(d, o + 3), u32(d, o + 4)};
}
uint32_t vlq(std::span<const uint8_t> d, size_t& p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        const uint8_t b = u8(d, p++);
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
    throw FormatError("mus: VLQ longer than 4 bytes");
}

// Track events from t0 up to and including the end-of-track meta 0x44 (0x594520 decoding:
// running status, 0xFF meta {cmd, u8 len, data}, 0xF0 {VLQ len, data}).
std::vector<MusEvent> decodeTrack(std::span<const uint8_t> d, size_t t0) {
    std::vector<MusEvent> out;
    uint8_t status = 0;
    for (size_t p = t0;;) {
        MusEvent e;
        e.offset = uint32_t(p - t0);
        e.delta = vlq(d, p);
        if (u8(d, p) & 0x80) status = d[p++];
        e.status = status;
        size_t n;
        if (status == kMusMeta) {
            e.meta = u8(d, p);
            n = u8(d, p + 1);
            p += 2;
        } else if (status == 0xF0) {
            n = vlq(d, p);
        } else if (status >= 0x80 && status < 0xF0) {
            n = (status >> 4) == 0xC || (status >> 4) == 0xD ? 1 : 2;
        } else {
            throw FormatError("mus: bad status byte");
        }
        if (p > d.size() || d.size() - p < n) throw FormatError("mus: truncated event");
        e.data.assign(d.begin() + p, d.begin() + p + n);
        p += n;
        out.push_back(std::move(e));
        if (status == kMusMeta && out.back().meta == 0x44) return out;
    }
}

size_t eventAt(const std::vector<MusEvent>& track, uint32_t offset) {
    const auto it = std::lower_bound(track.begin(), track.end(), offset,
                                     [](const MusEvent& e, uint32_t o) { return e.offset < o; });
    return it != track.end() && it->offset == offset ? size_t(it - track.begin()) : track.size();
}

// Operands of the commands MusSequencer executes must be in range: {min data size, stem byte,
// label byte} (-1 = none).
struct Operands { uint8_t size; int8_t stem, label; };
Operands operands(uint8_t meta) {
    switch (meta) {
    case 0x32: case 0x48: return {1, -1, 0};
    case 0x33: case 0x34: case 0x35: case 0x36: case 0x37: case 0x38: case 0x50: case 0x51:
        return {2, -1, -1};
    case 0x39: case 0x3A: case 0x3B: case 0x3C: case 0x3D: case 0x3E: case 0x3F: case 0x40:
        return {3, -1, 2};
    case 0x60: case 0x65: return {2, 0, -1};
    case 0x62: case 0x66: return {3, 0, -1};
    case 0x64: return {4, 0, -1};
    default: return {0, -1, -1};
    }
}

} // namespace

MusFile parseMus(std::span<const uint8_t> d) {
    if (d.size() < 4 || std::memcmp(d.data(), "Mus!", 4) != 0) throw FormatError("mus: bad magic");
    MusFile f;
    f.header04 = u32(d, 0x04);
    f.version = u32(d, 0x08);
    if (f.version != kMusVersion) throw FormatError("mus: unsupported version");
    f.header20 = u32(d, 0x20);
    const uint32_t nSeq = u32(d, 0x18), nStem = u32(d, 0x1C), nLabel = u32(d, 0x28),
                   nSample = u32(d, 0x30), nTone = u32(d, 0x34), nProgram = u32(d, 0x38);
    // 0x580E20 relocates: sequence table {id, offset} at 0x3C, then tone and program offset
    // tables; the sample records follow the program table.
    size_t p = 0x3C;
    std::vector<std::pair<uint32_t, size_t>> seqs;
    for (uint32_t i = 0; i < nSeq; ++i, p += 8) seqs.emplace_back(u32(d, p), u32(d, p + 4));
    for (uint32_t i = 0; i < nTone; ++i, p += 4) {
        const size_t o = u32(d, p);
        MusTone t{name20(d, o), {}};
        const uint32_t n = u32(d, o + 0x14);
        for (uint32_t k = 0; k < n; ++k) t.layers.push_back(keyRange(d, o + 0x18 + 0x68 * size_t(k) + 0x40));
        f.tones.push_back(std::move(t));
    }
    for (uint32_t i = 0; i < nProgram; ++i, p += 4) {
        const size_t o = u32(d, p);
        MusProgram pr{name20(d, o), u32(d, o + 0x14), u32(d, o + 0x18), {}};
        const uint32_t n = u32(d, o + 0x1C);
        for (uint32_t k = 0; k < n; ++k) pr.zones.push_back(keyRange(d, o + 0x24 + 0xC * size_t(k)));
        f.programs.push_back(std::move(pr));
    }
    for (uint32_t i = 0; i < nSample; ++i, p += 0x34)
        f.samples.push_back({name20(d, p), u32(d, p + 0x14), u32(d, p + 0x1C)});
    for (size_t i = 0, o = u32(d, 0x24); i < nStem; ++i, o += 0x58) {
        MusStem s;
        s.sampleRate = u32(d, o);
        s.channels = u32(d, o + 0x08);
        s.frames = u32(d, o + 0x10);
        s.unknown14 = u32(d, o + 0x14);
        s.msPerBeat = u32(d, o + 0x18);
        s.volume = f32(d, o + 0x1C);
        s.fade = f32(d, o + 0x20);
        s.mute = f32(d, o + 0x34);
        s.pan = f32(d, o + 0x3C);
        f.stems.push_back(s);
    }
    for (size_t i = 0, o = u32(d, 0x2C); i < nLabel; ++i) f.labels.push_back(u32(d, o + 4 * i));

    for (auto [id, o] : seqs) { // 0x588F10: "QSMa", tempo, u16 ppqn, u16, u16 tracks, u16, offsets
        if (u32(d, o) != 0x614D5351) throw FormatError("mus: bad sequence magic");
        MusSequence s;
        s.id = id;
        s.tempo = u32(d, o + 4);
        const uint32_t w = u32(d, o + 8);
        s.ppqn = uint16_t(w);
        s.unknown0a = uint16_t(w >> 16);
        if (s.tempo < 1000 || !s.ppqn) throw FormatError("mus: zero tempo");
        const uint16_t nTrack = uint16_t(u32(d, o + 0xC));
        for (uint16_t t = 0; t < nTrack; ++t) {
            auto ev = decodeTrack(d, size_t(o) + u32(d, o + 0x10 + 4 * size_t(t)));
            for (const auto& e : ev) {
                if (e.status != kMusMeta) continue;
                const Operands op = operands(e.meta);
                if (e.data.size() < op.size) throw FormatError("mus: short meta event");
                if (op.stem >= 0 && e.data[op.stem] >= f.stems.size()) throw FormatError("mus: stem out of range");
                if (op.label >= 0 && (e.data[op.label] >= f.labels.size() ||
                                      eventAt(ev, f.labels[e.data[op.label]]) == ev.size()))
                    throw FormatError("mus: bad label");
            }
            s.tracks.push_back(std::move(ev));
        }
        f.sequences.push_back(std::move(s));
    }
    return f;
}

MusSequencer::MusSequencer(const MusFile& f, size_t sequence)
    : f_(f), seq_(f.sequences.at(sequence)), tracks_(seq_.tracks.size()), stems_(f.stems),
      fades_(f.stems.size()) {}

void MusSequencer::update(float seconds, std::span<uint8_t, 256> v) {
    for (size_t i = 0; i < stems_.size(); ++i) { // 0x5891B0
        Fade& fd = fades_[i];
        if (!fd.on) continue;
        if (seconds <= fd.left) {
            fd.left -= seconds;
            stems_[i].fade += fd.rate * seconds;
        } else {
            stems_[i].fade = fd.target;
            fd.on = false;
        }
    }
    // 0x5950A0 clock without an attached stream: beats += dt * 1000 / (tempo / 1000 ms).
    // ponytail: with the .mul attached the exe derives ticks from the stream instead (AUDIO.md §5 [U]).
    seconds_ += seconds;
    const auto now = uint64_t(seconds_ * 1000.0 / (seq_.tempo / 1000) * seq_.ppqn);

    for (size_t t = 0; t < tracks_.size(); ++t) {
        Track& tr = tracks_[t];
        const auto& ev = seq_.tracks[t];
        while (tr.active && tr.pos < ev.size()) {
            const MusEvent& e = ev[tr.pos];
            if (tr.time + e.delta > now) break;
            tr.time += e.delta;
            ++tr.pos;
            if (e.status != kMusMeta) continue;
            const auto& a = e.data;
            int label = -1;
            switch (e.meta) { // 0x8A7B10 handler table
            case 0x32: case 0x48: label = a[0]; break;
            case 0x33: v[a[1]] = a[0]; break;
            case 0x34: v[a[1]] = v[a[0]]; break;
            case 0x35: v[a[1]] = uint8_t(v[a[1]] + a[0]); break;
            case 0x36: v[a[1]] = uint8_t(v[a[1]] - a[0]); break;
            case 0x37: v[a[1]] |= a[0]; break;
            case 0x38: v[a[1]] &= uint8_t(~a[0]); break;
            case 0x50: v[a[1]] = uint8_t(v[a[1]] * a[0]); break;
            case 0x51: if (a[0]) v[a[1]] = uint8_t(v[a[1]] / a[0]); break;
            case 0x39: if (v[a[0]] == a[1]) label = a[2]; break;
            case 0x3A: if (v[a[0]] != a[1]) label = a[2]; break;
            case 0x3B: if (v[a[0]] < a[1]) label = a[2]; break;
            case 0x3C: if (v[a[0]] > a[1]) label = a[2]; break;
            case 0x3D: if (v[a[0]] <= a[1]) label = a[2]; break;
            case 0x3E: if (v[a[0]] >= a[1]) label = a[2]; break;
            case 0x3F: if (v[a[0]] & a[1]) label = a[2]; break;
            case 0x40: if (!(v[a[0]] & a[1])) label = a[2]; break;
            case 0x44: tr.active = false; break;
            case 0x60: // 0x5865A0: static stem volume, cancels the fade
                stems_[a[0]].volume = a[1] / 127.0f;
                fades_[a[0]].on = false;
                break;
            case 0x62: case 0x66: { // 0x5866B0 / 0x586720 -> 0x586650: fade over a[1] beats (1/32 beats)
                MusStem& s = stems_[a[0]];
                const float secs = float(s.msPerBeat) * a[1] * 0.001f * (e.meta == 0x66 ? 1 / 32.0f : 1.0f);
                const float target = a[2] / 127.0f;
                if (secs < 0.001f) {
                    s.fade = target;
                    fades_[a[0]].on = false;
                } else {
                    fades_[a[0]] = {target, (target - s.fade) / secs, secs, true};
                }
                break;
            }
            case 0x64: stems_[a[0]].msPerBeat = (uint32_t(a[1]) << 16 | a[2] << 8 | a[3]) / 1000; break;
            case 0x65:
                for (const auto& s : f_.sequences)
                    if (s.id == a[1]) stems_[a[0]].msPerBeat = s.tempo / 1000;
                break;
            default: break; // 0xF0 source path, 0xF1 markers, 0x31 label definitions: no-ops
            }
            if (label >= 0) { // jumps end the track's frame (track flag 4)
                tr.pos = eventAt(ev, f_.labels[size_t(label)]);
                break;
            }
        }
    }
}

std::vector<float> MusSequencer::stemGains() const {
    std::vector<float> g;
    for (const auto& s : stems_) g.push_back(s.volume * s.fade * s.mute);
    return g;
}

bool MusSequencer::finished() const {
    return std::none_of(tracks_.begin(), tracks_.end(), [](const Track& t) { return t.active; });
}

} // namespace ou::audio
