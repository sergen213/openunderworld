// SoundPlex graphs, sfxmarkers and their player. Evidence: docs/formats/AUDIO.md §9.
#include "audio/SoundPlex.hpp"

#include <algorithm>
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
        default: break; // MaterialList, Selector, Oscillator: unused by the retail graphs we play
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
        if (drm->sections[i].type == cdc::SectionType::DtpData) sections_.try_emplace(drm->sections[i].id, drm, i);
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

float SoundPlayer::vary(float v, float var) { // 0x582D50: v ± var, clamped at 0
    if (var == 0) return v;
    return std::max(0.0f, v + std::uniform_real_distribution<float>(-var, var)(rng_));
}

double SoundPlayer::fire(Instance& in, const Plex& p, float volume, double at, int depth) {
    if (depth > 32) return 0;
    PlayParams pp{.volume = volume, .position = in.position};
    switch (p.type) {
    case PlexType::Silence: return p.delay;
    case PlexType::Reference:
        if (const Plex* q = resolve_ ? resolve_(p.id) : nullptr) return fire(in, *q, volume, at, depth + 1);
        return 0;
    case PlexType::Wave: {
        const auto pcm = mixer_.sound(p.id);
        if (!pcm || !pcm->sampleRate) return 0;
        pp.loop = pcm->loopEnd > 0; // Wave sections with loop points loop until stopped [I]
        if (const VoiceId v = mixer_.play(p.id, pp)) in.voices.push_back(v);
        waves.push_back(std::to_string(p.id));
        return pp.loop ? 1e9 : double(pcm->frames()) / pcm->sampleRate;
    }
    case PlexType::Stream: {
        const auto mul = stream_ ? stream_(p.stream) : nullptr;
        if (!mul || !mul->pcm.sampleRate) return 0;
        pp.volume *= p.volume, pp.bus = Bus::Voice;
        if (const VoiceId v = mixer_.play(std::make_shared<Pcm>(mul->pcm), pp)) in.voices.push_back(v);
        std::string text;
        for (const auto& s : mul->subtitles) text += std::string(s.text(0));
        lines.push_back({p.stream, std::move(text)});
        return double(mul->pcm.frames()) / mul->pcm.sampleRate;
    }
    case PlexType::Assignment: {
        if (p.children.empty()) return 0;
        const float v = volume * vary(p.volume, p.volumeVar), d = vary(p.delay, p.delayVar);
        if (p.maxDistance > 0 && in.position) pp.maxDistance = p.maxDistance;
        if (d > 0) { // SoundPlexTimeIn before the child
            in.pending.push_back({at + d, &p.children[0], v, SIZE_MAX});
            return d;
        }
        return fire(in, p.children[0], v, at, depth + 1);
    }
    case PlexType::Envelope: case PlexType::Reaction: case PlexType::Effect:
        return p.children.empty() ? 0 : fire(in, p.children[0], volume, at, depth + 1);
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
        return fire(in, p.children[pick], volume, at, depth + 1);
    }
    case PlexType::Sequence: {
        if (p.children.empty()) return 0;
        const double d = fire(in, p.children[0], volume, at, depth + 1);
        if (p.children.size() > 1) in.pending.push_back({at + d, &p, volume, 1});
        return d; // [I] the rest follows; total length unknown up front
    }
    case PlexType::Overlap: {
        double longest = 0;
        for (size_t k = 0; k < p.children.size(); ++k) {
            const float d = k < p.delays.size() ? vary(p.delays[k].first, p.delays[k].second) : 0;
            if (d > 0) in.pending.push_back({at + d, &p.children[k], volume, SIZE_MAX}), longest = std::max(longest, double(d));
            else longest = std::max(longest, fire(in, p.children[k], volume, at, depth + 1));
        }
        return longest;
    }
    default: return 0;
    }
}

SoundPlayer::Handle SoundPlayer::start(const Plex& p, std::optional<glm::vec3> position, float volume) {
    const Handle h = next_++;
    Instance& in = instances_[h];
    in.position = position;
    fire(in, p, volume, now_);
    if (in.voices.empty() && in.pending.empty()) { instances_.erase(h); return 0; }
    return h;
}

void SoundPlayer::stop(Handle h, float fade) {
    const auto it = instances_.find(h);
    if (it == instances_.end()) return;
    for (const VoiceId v : it->second.voices) mixer_.stop(v, fade);
    instances_.erase(it);
}

bool SoundPlayer::active(Handle h) const { return instances_.contains(h); }

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
                    if (p.index == SIZE_MAX) fire(in, *p.plex, p.volume, p.at);
                    else { // Sequence step
                        const double d = fire(in, p.plex->children[p.index], p.volume, p.at);
                        if (p.index + 1 < p.plex->children.size()) in.pending.push_back({p.at + d, p.plex, p.volume, p.index + 1});
                    }
                    again = true;
                    break;
                }
        }
        std::erase_if(in.voices, [&](VoiceId v) { return !mixer_.playing(v); });
        it = in.voices.empty() && in.pending.empty() ? instances_.erase(it) : std::next(it);
    }
}

} // namespace ou::audio
