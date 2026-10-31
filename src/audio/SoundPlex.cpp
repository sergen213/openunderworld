// SoundPlex graphs, sfxmarkers and their player. Evidence: docs/formats/AUDIO.md §9.
#include "audio/SoundPlex.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <glm/geometric.hpp>

namespace ou::audio {
namespace {

// Pointer-following reader over the DTP sections of one DRM (relocation maps cached per section).
struct Reader {
    const cdc::Drm& drm;
    std::map<uint32_t, std::unordered_map<uint32_t, cdc::Pointer>> maps;
    const cdc::Section& sec(uint32_t s) const { return drm.sections.at(s); }
    uint32_t u32(cdc::Pointer p) const {
        const auto d = sec(p.section).data;
        if (p.offset + 4 > d.size()) throw FormatError("soundplex: read past section end");
        uint32_t v;
        std::memcpy(&v, d.data() + p.offset, 4);
        return v;
    }
    float f32(cdc::Pointer p) const { const uint32_t v = u32(p); float f; std::memcpy(&f, &v, 4); return f; }
    uint8_t u8(cdc::Pointer p) const { return uint8_t(u32({p.section, p.offset & ~3u}) >> 8 * (p.offset & 3)); }
    std::optional<cdc::Pointer> ptr(cdc::Pointer p) {
        auto it = maps.find(p.section);
        if (it == maps.end()) it = maps.emplace(p.section, cdc::sectionPointers(drm, p.section)).first;
        const auto q = it->second.find(p.offset);
        if (q == it->second.end()) return std::nullopt;
        return q->second;
    }
    cdc::Pointer need(cdc::Pointer p) {
        if (auto q = ptr(p)) return *q;
        throw FormatError("soundplex: missing pointer at section " + std::to_string(p.section) + " +" + std::to_string(p.offset));
    }
    std::string str(cdc::Pointer p) const {
        const auto d = sec(p.section).data;
        if (p.offset >= d.size()) throw FormatError("soundplex: string past section end");
        const char* c = reinterpret_cast<const char*>(d.data() + p.offset);
        return {c, strnlen(c, d.size() - p.offset)};
    }
    static cdc::Pointer at(cdc::Pointer p, uint32_t add) { return {p.section, p.offset + add}; }

    // {count, children**} at d: the child node records.
    std::vector<Plex> list(cdc::Pointer d, int depth) {
        std::vector<Plex> out;
        const uint32_t n = u32(d);
        if (n > 1024) throw FormatError("soundplex: child count " + std::to_string(n));
        if (!n) return out;
        const auto arr = need(at(d, 4));
        for (uint32_t k = 0; k < n; ++k) out.push_back(node(need(at(arr, 4 * k)), depth + 1));
        return out;
    }

    Plex node(cdc::Pointer p, int depth = 0) {
        if (depth > 32) throw FormatError("soundplex: nesting too deep");
        Plex x;
        const uint32_t type = u32(p);
        if (type > uint32_t(PlexType::Oscillator)) throw FormatError("soundplex: node type " + std::to_string(type));
        x.type = PlexType(type);
        const auto d = ptr(at(p, 4));
        if (!d) return x; // null data: the exe creates nothing
        switch (x.type) {
        case PlexType::Silence: x.delay = f32(*d); break; // SoundPlexTimeIn(*(float*)data)
        case PlexType::Reference: x.id = u32(*d); break;
        case PlexType::Wave: x.id = u32(*d); break;      // Wave_Find(data->id) (0x591220)
        case PlexType::Stream:                            // {char* path, u8, u8, u16 volume%} [I volume]
            x.stream = str(need(*d));
            x.volume = float(u32(at(*d, 4)) >> 16) / 100.0f;
            break;
        case PlexType::Assignment: // 0x590970: [0] child, [1..2] delay, [3..4] volume, [14] distance [I]
            x.delay = f32(at(*d, 4)), x.delayVar = f32(at(*d, 8));
            x.volume = f32(at(*d, 12)), x.volumeVar = f32(at(*d, 16));
            x.maxDistance = std::max(0.0f, f32(at(*d, 56)));
            [[fallthrough]];
        case PlexType::Envelope:
        case PlexType::Reaction:
        case PlexType::Effect: // single child at data +0 (0x590CE0, 0x590F30, 0x58DEF0)
            if (const auto c = ptr(*d)) x.children.push_back(node(*c, depth + 1));
            break;
        case PlexType::ChoiceList: // {n, Plex** sounds, Plex* data, i8 mode, i8, u32 counter} (0x587920)
            x.children = list(*d, depth);
            x.mode = u8(at(*d, 12));
            break;
        case PlexType::Sequence: // {n, Plex**, ...} children in order (0x58E070)
            x.children = list(*d, depth);
            break;
        case PlexType::Overlap: // {n, Plex**, ..., +0x14 delays?, +0x18 {value, variance}[n]} (0x58B570)
            x.children = list(*d, depth);
            if (u32(at(*d, 0x14)))
                if (const auto dl = ptr(at(*d, 0x18)))
                    for (size_t k = 0; k < x.children.size(); ++k)
                        x.delays.emplace_back(f32(at(*dl, uint32_t(8 * k))), f32(at(*dl, uint32_t(8 * k + 4))));
            break;
        case PlexType::MaterialList: // {f32, u32 n, {u32 DTP, u16 material, u16}[n]} (0x583960)
        case PlexType::Selector: {   // {u32 n, {u32 DTP, u32 key}[n]} (0x583490)
            const uint32_t base = x.type == PlexType::MaterialList ? 4 : 0, n = u32(at(*d, base));
            if (n > 1024) throw FormatError("soundplex: list count " + std::to_string(n));
            for (uint32_t k = 0; k < n; ++k) {
                Plex ref;
                ref.type = PlexType::Reference, ref.id = u32(at(*d, base + 4 + 8 * k));
                x.children.push_back(ref);
                const uint32_t key = u32(at(*d, base + 8 + 8 * k));
                x.keys.push_back(x.type == PlexType::MaterialList ? key & 0xFFFF : key);
            }
            break;
        }
        default: break; // Oscillator
        }
        if (x.type == PlexType::Envelope) { // {Plex*, u32 n, Curve** } Curve {u16 param, u16, Keys*}; Keys {i32 loop start,
            const uint32_t n = u32(at(*d, 4));  // i32 loop end, u32 count, {f32 time, f32 value, u32 flags}[count]}
            if (n > 64) throw FormatError("soundplex: envelope curves " + std::to_string(n));
            const auto arr = n ? ptr(at(*d, 8)) : std::nullopt;
            for (uint32_t k = 0; arr && k < n; ++k) {
                const auto c = ptr(at(*arr, 4 * k));
                const auto keys = c ? ptr(at(*c, 4)) : std::nullopt;
                if (!keys) continue;
                Plex::Curve cv;
                const uint32_t head = u32(*c);
                cv.param = uint16_t(head), cv.release = (head >> 16) == 0;
                cv.loopStart = int32_t(u32(*keys)), cv.loopEnd = int32_t(u32(at(*keys, 4)));
                const uint32_t count = u32(at(*keys, 8));
                if (count > 1024) throw FormatError("soundplex: envelope keys");
                for (uint32_t i = 0; i < count; ++i) {
                    cv.keys.emplace_back(f32(at(*keys, 12 + 12 * i)), f32(at(*keys, 16 + 12 * i)));
                    const uint32_t flags = u32(at(*keys, 20 + 12 * i)); // 0x590CE0: bit 0 loop start, bit 1 loop end here
                    if (flags & 1) cv.loopStart = int32_t(i);
                    if (flags & 2) cv.loopEnd = int32_t(i);
                }
                if (cv.loopStart >= int32_t(count)) cv.loopStart = -1;
                if (cv.loopEnd >= int32_t(count) || cv.loopEnd < cv.loopStart) cv.loopEnd = -1, cv.loopStart = -1;
                x.curves.push_back(std::move(cv));
            }
        }
        if (x.type == PlexType::Reaction) { // {Plex*, u8 flags, u8 n, Map*}; Map 0x1C {u16 map, u16 flags, f32 in min / max,
            const uint32_t n = u8(at(*d, 5));   // f32 out min / max, u32 n, {f32 x, f32 y}*}
            const auto arr = n ? ptr(at(*d, 8)) : std::nullopt;
            for (uint32_t k = 0; arr && k < n; ++k) {
                const auto r = at(*arr, 0x1C * k);
                const uint32_t bits = u32(r);
                if (bits >> 16 & 1) continue; // disabled
                Plex::Map m;
                m.random = (bits & 0xFF) == 0xFF;
                m.input = uint8_t(bits & 0xF), m.output = uint8_t(bits >> 8 & 0xF), m.add = m.output == 1;
                m.absolute = (bits >> 12 & 3) == 0;
                if ((bits >> 12 & 3) == 2) m.absolute = false, m.negate = true;
                m.zeroBelow = bits & 0x4000, m.zeroAbove = bits & 0x8000;
                m.inMin = f32(at(r, 4)), m.inMax = f32(at(r, 8)), m.outMin = f32(at(r, 12)), m.outMax = f32(at(r, 16));
                const uint32_t count = u32(at(r, 20));
                if (count > 1024) throw FormatError("soundplex: reaction keys");
                if (const auto keys = count ? ptr(at(r, 24)) : std::nullopt)
                    for (uint32_t i = 0; i < count; ++i) m.keys.emplace_back(f32(at(*keys, 8 * i)), f32(at(*keys, 8 * i + 4)));
                x.maps.push_back(std::move(m));
            }
        }
        return x;
    }
};

} // namespace

Plex parsePlex(const cdc::Drm& drm, uint32_t section, uint32_t offset) {
    Reader r{drm, {}};
    return r.node({section, offset});
}

void PlexBank::add(std::shared_ptr<const cdc::Drm> drm) {
    for (uint32_t i = 0; i < drm->sections.size(); ++i)
        if (drm->sections[i].type == cdc::SectionType::DtpData && sections_.try_emplace(drm->sections[i].id, drm, i).second)
            // find() caches misses: an id asked for before its DRM was added must be looked up again (the motorcycle
            // engine DTP 13589 stayed missing, and ou-game re-read motorcycle.drm every tick, 35 ms, while riding).
            if (const auto it = parsed_.find(drm->sections[i].id); it != parsed_.end() && !it->second) parsed_.erase(it);
}

const Plex* PlexBank::find(uint32_t id) {
    if (const auto it = parsed_.find(id); it != parsed_.end()) return it->second.get();
    auto& slot = parsed_[id];
    if (const auto it = sections_.find(id); it != sections_.end()) {
        try {
            slot = std::make_unique<Plex>(parsePlex(*it->second.first, it->second.second, 0));
        } catch (const FormatError&) { /* not a SoundPlex DTP */ }
    }
    return slot.get();
}

std::vector<SfxMarker> parseSfxMarkers(const cdc::Drm& unit) {
    std::vector<SfxMarker> out;
    if (unit.sections.empty()) return out;
    Reader r{unit, {}};
    const auto ud = r.ptr({0, 0x74}); // Level +0x74: unit data
    if (!ud) return out;
    const uint32_t n = r.u32(Reader::at(*ud, 0xAC));
    if (!n) return out;
    const auto arr = r.need(Reader::at(*ud, 0xB0));
    for (uint32_t i = 0; i < n; ++i) {
        const auto m = r.need(Reader::at(arr, 4 * i));
        SfxMarker s;
        s.position = {r.f32(m), r.f32(Reader::at(m, 4)), r.f32(Reader::at(m, 8))};
        s.id = r.u32(Reader::at(m, 0x20));
        const uint32_t sounds = r.u32(Reader::at(m, 0x2C)), triggers = r.u32(Reader::at(m, 0x30));
        if (sounds > 256 || triggers > 256) throw FormatError("sfxmarker: counts");
        if (sounds) {
            const auto sd = r.need(Reader::at(m, 0x28));
            for (uint32_t k = 0; k < sounds; ++k) {
                const auto p = r.ptr(Reader::at(sd, 4 * k));
                s.sounds.push_back(p ? r.node(*p) : Plex{});
            }
        }
        if (triggers) {
            const auto tr = r.need(Reader::at(m, 0x38));
            for (uint32_t k = 0; k < triggers; ++k) { // 0x18 bytes: {u32 bits, perimeter {radius, invader, flags}, action}
                const auto t = Reader::at(tr, 0x18 * k);
                const uint32_t bits = r.u32(t);
                SfxTrigger g;
                g.condition = uint8_t(bits >> 1 & 3), g.action = uint8_t(bits >> 3 & 15);
                g.radius = r.f32(Reader::at(t, 4));
                g.always = r.u32(Reader::at(t, 0xC)) & 1;
                const uint32_t a = r.u32(Reader::at(t, 0x10));
                if (g.action == SfxTrigger::Start || g.action == SfxTrigger::End) {
                    const uint32_t refs = a >> 16;
                    if (refs > 256) throw FormatError("sfxmarker: sound refs");
                    if (refs) {
                        const auto rp = r.need(Reader::at(t, 0x14));
                        for (uint32_t j = 0; j < refs; ++j) g.refs.push_back(int32_t(r.u32(Reader::at(rp, 4 * j))));
                    }
                } else {
                    g.variable = uint8_t(a), g.value = uint8_t(a >> 8);
                }
                s.triggers.push_back(std::move(g));
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

SfxMarkerTriggers::SfxMarkerTriggers(const std::vector<SfxMarker>& markers) {
    for (const auto& m : markers) state_.emplace_back(m.triggers.size());
}

std::vector<SfxMarkerTriggers::Fired> SfxMarkerTriggers::update(const std::vector<SfxMarker>& markers, glm::vec3 player) {
    std::vector<Fired> out;
    for (size_t i = 0; i < markers.size() && i < state_.size(); ++i)
        for (size_t k = 0; k < markers[i].triggers.size(); ++k) {
            const SfxTrigger& t = markers[i].triggers[k];
            State& s = state_[i][k];
            auto test = [&] { // 0x586C70: true when the inside flag changed
                const bool inside = glm::distance(markers[i].position, player) <= t.radius;
                const bool changed = inside != s.inside;
                s.inside = inside;
                return changed;
            };
            auto fire = [&] { if (++s.count >= 64) s.count = 1; out.push_back({i, k}); };
            if (!s.armed) {
                s.armed = true;
                if (t.condition == SfxTrigger::Init) { out.push_back({i, k}); continue; }
                test();
                if (s.inside && t.condition == SfxTrigger::Enter) fire();
            } else if (t.condition != SfxTrigger::Init && (t.always || s.count == 0) && test() &&
                       s.inside != (t.condition == SfxTrigger::Exit)) {
                fire();
            }
        }
    return out;
}

float rolloff(float d, float mn, float mx) { // 0x585070 (curve 1)
    if (d >= mx) return 0;
    if (d <= mn || mx <= mn) return 1;
    const float f = (mx - d) / (mx - mn);
    return f * f;
}

float SoundPlayer::vary(float v, float var) { // 0x582D50: v ± var, clamped at 0
    if (var == 0) return v;
    return std::max(0.0f, v + std::uniform_real_distribution<float>(-var, var)(rng_));
}

double SoundPlayer::fire(Instance& in, const Plex& p, float volume, double at, const Mods& mods, int depth) {
    if (depth > 32) return 0;
    PlayParams pp{.volume = volume, .position = in.position, .minDistance = kSoundMinDistance, .maxDistance = kSoundMaxDistance};
    for (const Mod& m : mods)
        if (m.plex->type == PlexType::Assignment) pp.maxDistance = m.plex->maxDistance; // innermost wins
    auto play = [&](VoiceId v) {
        if (!v) return;
        in.voices.push_back({v, volume * (p.type == PlexType::Stream ? p.volume : 1), mods});
        apply(in, in.voices.back());
    };
    switch (p.type) {
    case PlexType::Silence: return p.delay;
    case PlexType::Reference:
        if (const Plex* q = resolve_ ? resolve_(p.id) : nullptr) return fire(in, *q, volume, at, mods, depth + 1);
        return 0;
    case PlexType::Wave: {
        const auto pcm = mixer_.sound(p.id);
        if (!pcm || !pcm->sampleRate) return 0;
        pp.loop = pcm->loopEnd > 0; // Wave sections with loop points loop until stopped [I]
        play(mixer_.play(p.id, pp));
        waves.push_back(std::to_string(p.id));
        return pp.loop ? 1e9 : double(pcm->frames()) / pcm->sampleRate;
    }
    case PlexType::Stream: {
        const auto mul = stream_ ? stream_(p.stream) : nullptr;
        if (!mul || !mul->pcm.sampleRate) return 0;
        pp.volume *= p.volume, pp.bus = Bus::Voice;
        play(mixer_.play(std::make_shared<Pcm>(mul->pcm), pp));
        std::string text;
        for (const auto& s : mul->subtitles) text += std::string(s.text(0));
        lines.push_back({p.stream, std::move(text)});
        return double(mul->pcm.frames()) / mul->pcm.sampleRate;
    }
    case PlexType::Assignment: {
        if (p.children.empty()) return 0;
        const float v = volume * vary(p.volume, p.volumeVar), d = vary(p.delay, p.delayVar);
        Mods m = mods;
        if (p.maxDistance > 0) m.push_back({&p, at});
        if (d > 0) { // SoundPlexTimeIn before the child
            in.pending.push_back({at + d, &p.children[0], v, SIZE_MAX, m});
            return d;
        }
        return fire(in, p.children[0], v, at, m, depth + 1);
    }
    case PlexType::Envelope: case PlexType::Reaction: {
        if (p.children.empty()) return 0;
        Mods m = mods;
        if (!p.curves.empty() || !p.maps.empty()) m.push_back({&p, at});
        return fire(in, p.children[0], volume, at, m, depth + 1);
    }
    case PlexType::Effect:
        return p.children.empty() ? 0 : fire(in, p.children[0], volume, at, mods, depth + 1);
    case PlexType::MaterialList: case PlexType::Selector: { // the entry for the key, else the default (material 0 / key -1)
        const uint32_t want = p.type == PlexType::MaterialList ? in.material : 0, def = p.type == PlexType::MaterialList ? 0 : ~0u;
        size_t pick = SIZE_MAX;
        for (size_t k = 0; k < p.keys.size(); ++k) {
            if (p.keys[k] == want) { pick = k; break; }
            if (p.keys[k] == def && pick == SIZE_MAX) pick = k;
        }
        return pick == SIZE_MAX ? 0 : fire(in, p.children[pick], volume, at, mods, depth + 1);
    }
    case PlexType::ChoiceList: {
        const uint32_t n = uint32_t(p.children.size());
        if (!n) return 0;
        uint32_t pick = 0;
        auto rnd = [&](uint32_t m) { return std::uniform_int_distribution<uint32_t>(0, m - 1)(rng_); };
        switch (n == 1 ? 3 : (p.mode == 2 && n > 32 ? 1 : p.mode)) {
        case 1: pick = rnd(n); if (pick == p.counter) pick = (pick + 1) % n; p.counter = pick; break;
        case 2: { // shuffle: every child once before repeating
            std::vector<uint32_t> left;
            for (uint32_t k = 0; k < n; ++k) if (!(p.counter >> k & 1)) left.push_back(k);
            if (left.empty()) { p.counter = 0; for (uint32_t k = 0; k < n; ++k) left.push_back(k); }
            pick = left[rnd(uint32_t(left.size()))];
            p.counter = left.size() == 1 ? 0 : p.counter | 1u << pick;
            break;
        }
        case 3: pick = p.counter % n; p.counter = pick + 1 == n ? 0 : pick + 1; break;
        default: pick = rnd(n); break;
        }
        return fire(in, p.children[pick], volume, at, mods, depth + 1);
    }
    case PlexType::Sequence: {
        if (p.children.empty()) return 0;
        const double d = fire(in, p.children[0], volume, at, mods, depth + 1);
        if (p.children.size() > 1) in.pending.push_back({at + d, &p, volume, 1, mods});
        return d; // [I] the rest follows; total length unknown up front
    }
    case PlexType::Overlap: {
        double longest = 0;
        for (size_t k = 0; k < p.children.size(); ++k) {
            const float d = k < p.delays.size() ? vary(p.delays[k].first, p.delays[k].second) : 0;
            if (d > 0) in.pending.push_back({at + d, &p.children[k], volume, SIZE_MAX, mods}), longest = std::max(longest, double(d));
            else longest = std::max(longest, fire(in, p.children[k], volume, at, mods, depth + 1));
        }
        return longest;
    }
    default: return 0;
    }
}

// 0x587FF0: the input normalised into [inMin, inMax], through the piecewise-linear keys (0x587F10), scaled to the output.
float SoundPlayer::mapValue(const Plex::Map& m, float x) {
    if (m.random) return std::uniform_real_distribution<float>(m.outMin, m.outMax)(rng_);
    if ((m.absolute && x < 0) || m.negate) x = -x;
    if (x < 0 || (x < m.inMin && m.zeroBelow) || (x > m.inMax && m.zeroAbove)) return 0;
    float u = x <= m.inMin ? 0 : x >= m.inMax ? 1 : (x - m.inMin) / (m.inMax - m.inMin);
    if (m.keys.size() >= 2) {
        size_t k = 0;
        while (k + 2 < m.keys.size() && m.keys[k + 1].first <= u) ++k;
        const auto a = m.keys[k], b = m.keys[k + 1];
        const float f = u <= a.first ? 0 : u >= b.first || b.first <= a.first ? 1 : (u - a.first) / (b.first - a.first);
        u = a.second + (b.second - a.second) * f;
    }
    return m.outMin + (m.outMax - m.outMin) * u;
}

float Plex::Curve::value(float t, float releasedAt, bool* ended) const {
    if (ended) *ended = false;
    if (keys.empty()) return 1;
    bool past = false;
    // From value v0 at key j's time through keys j+1.. (0x587A90 moves linearly from the current value to the next key).
    const auto walk = [&](float u, size_t j, float v0) {
        float a = keys[j].first, va = v0;
        for (size_t k = j + 1; k < keys.size(); ++k) {
            const float b = keys[k].first;
            if (u < b) return b > a ? va + (keys[k].second - va) * (u - a) / (b - a) : keys[k].second;
            a = b, va = keys[k].second;
        }
        past = true;
        return va;
    };
    const auto sustain = [&](float s) {
        float u = keys[0].first + s;
        if (loopStart >= 0 && u >= keys[size_t(loopEnd)].first) {
            if (loopStart == loopEnd) return keys[size_t(loopEnd)].second;
            const float a = keys[size_t(loopStart)].first, b = keys[size_t(loopEnd)].first;
            u = b > a ? a + std::fmod(u - b, b - a) : b; // [I] the jump back continues from loop start's value
        }
        return walk(u, 0, keys[0].second);
    };
    // [I] a released curve past its last key ends the voice even when that key is not 0 (else a looping wave would
    // play on); sustain curves without the release flag leave their loop the same way.
    const bool released = releasedAt >= 0 && t >= releasedAt && (release || loopStart >= 0);
    float v;
    if (released) {
        const size_t j = loopEnd >= 0 ? size_t(loopEnd) : keys.size() - 1;
        const float v0 = sustain(releasedAt);
        past = false;
        v = walk(keys[j].first + (t - releasedAt), j, v0);
    } else {
        v = sustain(t);
    }
    if (ended) *ended = past && param == 0 && (released || keys.back().second <= 0);
    return v;
}

// Envelope volume curves (param 0, Plex::Curve::value) at the node's age; one ending ends the voice (0x587A90). Reaction
// maps: output 0 multiplies the volume, output 1 adds semitones.
bool SoundPlayer::apply(Instance& in, Voice& v) {
    float gain = 1, semitones = 0;
    bool pitch = false;
    for (const Mod& mod : v.mods) {
        for (const Plex::Curve& c : mod.plex->curves) {
            if (c.param != 0) continue;
            bool ended = false;
            const float g = c.value(float(now_ - mod.start), in.released >= 0 ? float(std::max(0.0, in.released - mod.start)) : -1, &ended);
            if (ended) return false;
            gain *= std::max(0.0f, g);
        }
        for (const Plex::Map& m : mod.plex->maps) {
            const float y = mapValue(m, in.params[m.input]);
            if (m.output == 0) gain *= y;
            else if (m.output == 1) semitones += y, pitch = true;
        }
    }
    mixer_.setVolume(v.id, v.volume * gain);
    if (pitch) mixer_.setPitch(v.id, std::exp2(semitones / 12.0f));
    return true;
}

SoundPlayer::Handle SoundPlayer::start(const Plex& p, std::optional<glm::vec3> position, float volume, uint16_t material) {
    const Handle h = next_++;
    Instance& in = instances_[h];
    in.position = position, in.material = material;
    fire(in, p, volume, now_, {});
    if (in.voices.empty() && in.pending.empty()) { instances_.erase(h); return 0; }
    return h;
}

// 0x587B80: stopping an Envelope with a sustain or a release curve plays its release instead of cutting the voice;
// pending parts never start. A second stop (or one without such an envelope) cuts.
void SoundPlayer::stop(Handle h, float fade) {
    const auto it = instances_.find(h);
    if (it == instances_.end()) return;
    Instance& in = it->second;
    const auto releases = [](const Voice& v) {
        return std::any_of(v.mods.begin(), v.mods.end(), [](const Mod& m) {
            return std::any_of(m.plex->curves.begin(), m.plex->curves.end(), [](const Plex::Curve& c) { return c.param == 0 && (c.release || c.loopStart >= 0); });
        });
    };
    if (in.released < 0) {
        in.released = now_, in.pending.clear();
        std::erase_if(in.voices, [&](const Voice& v) { return releases(v) ? false : (mixer_.stop(v.id, fade), true); });
        if (!in.voices.empty()) return;
    }
    for (const Voice& v : in.voices) mixer_.stop(v.id, fade);
    instances_.erase(it);
}

void SoundPlayer::stopAll(float fade) {
    for (const auto& [h, in] : instances_)
        for (const Voice& v : in.voices) mixer_.stop(v.id, fade);
    instances_.clear(), lines.clear(), waves.clear();
}

bool SoundPlayer::active(Handle h) const { return instances_.contains(h); }

void SoundPlayer::setParam(Handle h, size_t index, float value) {
    if (const auto it = instances_.find(h); it != instances_.end() && index < it->second.params.size()) it->second.params[index] = value;
}

void SoundPlayer::setPosition(Handle h, glm::vec3 position) {
    const auto it = instances_.find(h);
    if (it == instances_.end() || !it->second.position) return;
    it->second.position = position;
    for (const Voice& v : it->second.voices) mixer_.setPosition(v.id, position);
}

void SoundPlayer::update(double seconds) {
    now_ += seconds;
    for (auto it = instances_.begin(); it != instances_.end();) {
        Instance& in = it->second;
        for (bool again = true; again;) { // fired parts may queue more due parts
            again = false;
            for (size_t i = 0; i < in.pending.size(); ++i)
                if (in.pending[i].at <= now_) {
                    const Pending p = in.pending[i];
                    in.pending.erase(in.pending.begin() + std::ptrdiff_t(i));
                    if (p.index == SIZE_MAX) fire(in, *p.plex, p.volume, p.at, p.mods);
                    else { // Sequence step
                        const double d = fire(in, p.plex->children[p.index], p.volume, p.at, p.mods);
                        if (p.index + 1 < p.plex->children.size()) in.pending.push_back({p.at + d, p.plex, p.volume, p.index + 1, p.mods});
                    }
                    again = true;
                    break;
                }
        }
        std::erase_if(in.voices, [&](Voice& v) {
            if (!mixer_.playing(v.id)) return true;
            if (v.mods.empty() || apply(in, v)) return false;
            mixer_.stop(v.id, 0.02f);
            return true;
        });
        it = in.voices.empty() && in.pending.empty() ? instances_.erase(it) : std::next(it);
    }
}

} // namespace ou::audio
