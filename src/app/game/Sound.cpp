// ou-game sound events. Evidence: docs/formats/AUDIO.md §9.
#include "app/game/Sound.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace ou::app {
namespace {
constexpr uint32_t kLaraDrown = 2315; // lara.drm DTP: ChoiceList of lara_drown_01..08 (AUDIO.md §8.1)
// motorcycle.drm DTP: Overlap of the SV650 engine loops, each under a Reaction on game parameters 2 (rpm, 1500..10000),
// 5 (0..700) and 0 (0..50) (AUDIO.md §9.8). [I] What the Bike class feeds them is not traced: rpm follows the speed.
constexpr uint32_t kBikeEngine = 13589;
std::shared_ptr<const cdc::Drm> load(const cdc::BigfileSet& bf, const std::string& path) {
    auto d = bf.read(path);
    return d ? std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*d))) : nullptr;
}
} // namespace

GameSound::GameSound(const cdc::BigfileSet& bigfile, audio::Mixer& mixer)
    : bf_(bigfile), mixer_(mixer), player_(mixer, [this](uint32_t id) { return bank_.find(id); },
                                           [this](const std::string& path) -> std::shared_ptr<const audio::Mul> {
                                               auto& m = streams_[path];
                                               if (!m)
                                                   if (auto d = bf_.read("pc-w\\audio\\streams\\" + path + ".mul"))
                                                       m = std::make_shared<audio::Mul>(audio::decodeMul(*d));
                                               return m;
                                           }) {
    mixer_.setAttenuation(audio::rolloff); // the game's distance curve (AUDIO.md §9.12), not the Mixer's FMOD default
    for (const char* name : {"pc-w\\lara.drm", "pc-w\\generalbank.drm"})
        try {
            if (auto drm = load(bf_, name)) {
                if (std::string_view(name).ends_with("generalbank.drm")) mixer_.addWaves(*drm); // the menu sounds
                bank_.add(std::move(drm));
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: sound bank %s: %s\n", name, e.what());
        }
    for (uint32_t id = 200; id < 1000; ++id) // generalbank hint/answer lines: single-stream DTPs (AUDIO.md §9.5)
        if (const audio::Plex* p = bank_.find(id); p && p->type == audio::PlexType::Assignment && p->children.size() == 1 &&
                                                   p->children[0].type == audio::PlexType::Stream)
            helpLines_.try_emplace(p->children[0].stream, id);
    try { // sound categories and their DSPs (AUDIO.md §9.15)
        if (auto gsi = load(bf_, "pc-w\\globalsoundinfo.drm")) player_.setCategories(audio::parseSoundCategories(*gsi));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ou-game: sound categories: %s\n", e.what());
    }
}

void GameSound::attach(game::World& world) {
    syncUnits(world), unitsVersion_ = world.unitsVersion();
    world.script().setSoundNative([this, &world](int32_t owner, uint32_t member, cdc::Pointer at, uint32_t fn, int32_t anchor) {
        const auto key = std::make_pair(owner, member);
        if (fn == 5) { // Stop
            if (const auto it = scriptHandles_.find(key); it != scriptHandles_.end()) player_.stop(it->second), scriptHandles_.erase(it);
            return true;
        }
        const auto* o = world.script().find(owner);
        const auto unit = o ? units_.find(o->unit) : units_.end();
        if (unit == units_.end()) return false;
        auto& plex = scriptPlex_[{o->unit, at.section, at.offset}];
        if (!plex) {
            try {
                plex = std::make_unique<audio::Plex>(audio::parsePlex(*unit->second.drm, at.section, at.offset));
            } catch (const std::exception& e) {
                std::fprintf(stderr, "ou-game: script sound of %d: %s\n", owner, e.what());
                return false;
            }
        }
        std::optional<glm::vec3> pos;
        if (anchor)
            if (const auto* a = world.script().find(anchor)) pos = a->pos;
        // Play / Play3D (0x4AE290 / 0x4AE380) leave a sound that still plays on the member alone: scripts call them every
        // tick to keep a loop going (l3_main_bedroom's firetriggervolumes).
        if (const auto it = scriptHandles_.find(key); it != scriptHandles_.end() && player_.active(it->second)) {
            if (pos) player_.setPosition(it->second, *pos);
            return true;
        }
        scriptHandles_[key] = player_.start(*plex, pos);
        std::fprintf(stderr, "ou-game: tick %llu script sound %d (%s) +%u %s%s%u%s%s\n", (unsigned long long)world.ticks(), owner,
                     o->name.c_str(), member, fn == 1 || fn == 3 ? "3D " : "", plex->type == audio::PlexType::Reference ? "DTP " : "node type ",
                     plex->type == audio::PlexType::Reference ? plex->id : uint32_t(plex->type), pos ? " at anchor" : "",
                     scriptHandles_[key] ? "" : " (silent)");
        return true;
    });
    // reverb 1 (0x4AA750 -> 0x582B70): the 13 SFX-reverb floats the member's value points to (AUDIO.md §9.15).
    world.script().setSoundBusy([this](int32_t owner, uint32_t member) { return scriptSoundBusy(owner, member); });
    world.script().setReverbNative([this, &world](const std::string& name, cdc::Pointer at) { reverb(world, name, at); });
    resume(world); // a game loaded before the sound existed (--load)
}

void GameSound::reverb(const game::World& world, const std::string& name, cdc::Pointer at) {
    const auto unit = units_.find(name);
    if (unit == units_.end()) return;
    const cdc::Drm& drm = *unit->second.drm;
    if (at.section >= drm.sections.size() || at.offset + sizeof(audio::SfxReverbProps) > drm.sections[at.section].data.size()) {
        std::fprintf(stderr, "ou-game: reverb in %s: no settings at section %u +%u\n", name.c_str(), at.section, at.offset);
        return;
    }
    audio::SfxReverbProps p;
    std::memcpy(p.data(), drm.sections[at.section].data.data() + at.offset, sizeof p);
    if (p == mixer_.reverb()) return;
    mixer_.setReverb(p);
    std::string values;
    for (float v : p) values += (values.empty() ? "" : ",") + std::to_string(v);
    std::fprintf(stderr, "ou-game: tick %llu reverb %s: room %.0f mB, decay %.2f s, reverb %.0f mB%s [%s]\n",
                 (unsigned long long)world.ticks(), name.c_str(), p[1], p[4], p[8], p[1] <= -10000 ? " (off)" : "", values.c_str());
}

void GameSound::resume(const game::World& world) {
    const auto& a = world.script().audioState();
    if (a.reverbUnit.empty()) return;
    syncUnits(world), unitsVersion_ = world.unitsVersion();
    reverb(world, a.reverbUnit, a.reverbAt);
}

bool GameSound::playHelp(const game::World& world, bool answer) {
    const std::string& unit = world.currentUnit().data.name;
    const std::string condition = world.script().helpCondition();
    if (condition.empty() || unit.size() < 3 || unit[0] != 'l' || !std::isdigit(static_cast<unsigned char>(unit[1]))) {
        std::fprintf(stderr, "ou-game: tick %llu help: no hint condition\n", (unsigned long long)world.ticks());
        return false;
    }
    // sc_<CC>: the level number of the unit name [I]; NN: the condition's number.
    const std::string path = std::string("vo\\lara\\sc_0") + unit[1] + "_" + condition.substr(5, 2) + (answer ? "_answer" : "_hint");
    const auto it = helpLines_.find(path);
    const audio::Plex* p = it == helpLines_.end() ? nullptr : bank_.find(it->second);
    std::fprintf(stderr, "ou-game: tick %llu help %s %s -> %s\n", (unsigned long long)world.ticks(), answer ? "answer" : "hint",
                 condition.c_str(), p ? path.c_str() : "no line");
    if (!p) return false;
    player_.stop(help_);
    help_ = player_.start(*p);
    return true;
}

void GameSound::reset() {
    player_.stopAll();
    mixer_.setReverb(audio::kReverbOff);
    for (auto& [name, u] : units_) u.handles.clear(), u.triggers = audio::SfxMarkerTriggers(u.markers); // Init fires again
    scriptHandles_.clear(), slots_.clear(), jointVoices_.clear();
    drown_ = engine_ = help_ = 0;
    lastVars_ = {};
    ticker_ = {};
    lastCondition_.clear();
}

void GameSound::menuSound(uint32_t dtp) {
    const audio::Plex* p = bank_.find(dtp);
    if (!p || p->type != audio::PlexType::Assignment || p->children.empty() || p->children[0].type != audio::PlexType::Wave) {
        std::fprintf(stderr, "ou-game: menu sound DTP %u: not an Assignment -> Wave\n", dtp);
        return;
    }
    const auto v = mixer_.play(p->children[0].id, {.volume = p->volume, .world = false});
    std::fprintf(stderr, "ou-game: menu sound DTP %u wave %u%s\n", dtp, p->children[0].id, v ? "" : " (silent)");
}

void GameSound::addObject(const fx::ObjectFx& fx) {
    if (fx.drm && eventDrms_.insert(fx.drm.get()).second) bank_.add(fx.drm), mixer_.addWaves(*fx.drm);
}

const audio::Plex* GameSound::objectPlex(const fx::ObjectFx& fx, cdc::Pointer at) {
    auto& slot = eventPlex_[{fx.drm.get(), at.section, at.offset}];
    if (!slot) {
        try {
            slot = std::make_unique<audio::Plex>(audio::parsePlex(*fx.drm, at.section, at.offset));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "ou-game: %s sound record: %s\n", fx.name.c_str(), ex.what());
            slot = std::make_unique<audio::Plex>(); // silence: do not retry
        }
    }
    return slot.get();
}

void GameSound::slotSound(const fx::ObjectFx& fx, uint8_t slot, uint8_t action, int32_t owner, glm::vec3 position, uint8_t material,
                          uint64_t tick) {
    if (!fx.drm || slot >= fx.soundSlots.size() || fx.soundSlots[slot].plex.section == ~0u) return;
    addObject(fx);
    const auto key = std::make_tuple(owner, fx.drm.get(), slot);
    const auto it = slots_.find(key);
    if (action == 0) { // 0x593D50: the playing sound gets its release (vfunc 4)
        if (it != slots_.end()) player_.stop(it->second), slots_.erase(it);
        return;
    }
    if (it != slots_.end() && player_.active(it->second)) {
        if (action == 1) return player_.setPosition(it->second, position);
        player_.stop(it->second, 0);
    }
    const auto h = player_.start(*objectPlex(fx, fx.soundSlots[slot].plex), position, 1, material);
    slots_[key] = h;
    std::fprintf(stderr, "ou-game: tick %llu slot sound %s owner %d slot %u%s\n", (unsigned long long)tick, fx.name.c_str(), owner, slot,
                 h ? "" : " (silent)");
}

void GameSound::ownerGone(int32_t owner) {
    for (auto it = slots_.lower_bound({owner, nullptr, 0}); it != slots_.end() && std::get<0>(it->first) == owner;)
        player_.stop(it->second), it = slots_.erase(it);
}

bool GameSound::scriptSoundBusy(int32_t owner, uint32_t member) const {
    const auto it = scriptHandles_.find({owner, member});
    return it != scriptHandles_.end() && player_.busy(it->second);
}

const audio::Plex* GameSound::jointPlex(const std::string& object, cdc::Pointer at) {
    auto& drm = jointDrms_[object];
    if (!drm) {
        try {
            drm = load(bf_, "pc-w\\" + object + ".drm");
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: joint sound object %s: %s\n", object.c_str(), e.what());
        }
        if (!drm) drm = std::make_shared<const cdc::Drm>(); // missing: silent, not retried
        else if (eventDrms_.insert(drm.get()).second) bank_.add(drm), mixer_.addWaves(*drm);
    }
    if (drm->sections.empty()) return nullptr;
    auto& slot = eventPlex_[{drm.get(), at.section, at.offset}];
    if (!slot) {
        try {
            slot = std::make_unique<audio::Plex>(audio::parsePlex(*drm, at.section, at.offset));
        } catch (const std::exception& ex) {
            std::fprintf(stderr, "ou-game: %s joint sound: %s\n", object.c_str(), ex.what());
            slot = std::make_unique<audio::Plex>(); // silence: do not retry
        }
    }
    return slot->type == audio::PlexType::Silence ? nullptr : slot.get(); // most joint records hold a TimeIn
}

// tru.exe 0x4F2840 per contact: channel 1 (the rolling voice) starts when it has none or its sound ended and gets
// parameters 1 / 2 every frame; channel 0 (impact) starts on a frame with an impact when it has none or it ended, else
// takes the stronger impact (a weaker one sets 0). A contact gone (bodies asleep, 0x4F32E0) releases channel 1
// (0x5527F0(4)); ours releases the impact voice too [I: the exe drops its reference and lets it play out].
void GameSound::jointSounds(const game::World& world) {
    const auto tick = (unsigned long long)world.ticks();
    std::set<std::pair<int32_t, int32_t>> live;
    for (const auto& s : world.jointSounds()) {
        const audio::Plex* p = jointPlex(s.object, s.sound);
        if (!p) continue;
        const auto key = std::make_pair(s.uid, s.joint);
        live.insert(key);
        JointVoice& v = jointVoices_[key];
        if (!player_.active(v.roll)) {
            v.roll = player_.start(*p, s.at);
            std::fprintf(stderr, "ou-game: tick %llu joint sound %s uid %d joint %d start (%.1f units/s, %.1f deg/s)%s\n", tick,
                         s.object.c_str(), s.uid, s.joint, s.linear, s.angular, v.roll ? "" : " (silent)");
        }
        player_.setPosition(v.roll, s.at);
        player_.setParam(v.roll, 1, s.linear), player_.setParam(v.roll, 2, s.angular);
        if (!s.impact) continue;
        if (!player_.active(v.impact)) {
            v.impact = player_.start(*p, s.at), v.impactSpeed = *s.impact;
            player_.setParam(v.impact, 0, *s.impact);
        } else if (std::abs(*s.impact) > std::abs(v.impactSpeed)) {
            player_.setPosition(v.impact, s.at), player_.setParam(v.impact, 0, v.impactSpeed = *s.impact);
        } else {
            player_.setParam(v.impact, 0, v.impactSpeed = 0);
        }
        std::fprintf(stderr, "ou-game: tick %llu joint sound %s uid %d joint %d impact %.1f\n", tick, s.object.c_str(), s.uid, s.joint,
                     *s.impact);
    }
    for (auto it = jointVoices_.begin(); it != jointVoices_.end();) {
        if (live.contains(it->first)) { ++it; continue; }
        player_.stop(it->second.roll), player_.stop(it->second.impact);
        std::fprintf(stderr, "ou-game: tick %llu joint sound uid %d joint %d stop\n", tick, it->first.first, it->first.second);
        it = jointVoices_.erase(it);
    }
}

void GameSound::animSound(const fx::ObjectFx& fx, const fx::Event& e, glm::vec3 position, uint8_t material, uint64_t tick) {
    if (!fx.drm) return;
    addObject(fx);
    const audio::Plex* p = nullptr;
    uint32_t dtp = 0;
    const auto& s = e.sound;
    if (s.kind == 0) {
        p = objectPlex(fx, s.plex);
    } else {
        dtp = s.dtp[0];
        if (s.kind == 3) { // 0x5B2820: rand() % (w0 + w1 + w2) picks one of three DTPs
            const int sum = s.weight[0] + s.weight[1] + s.weight[2];
            if (!sum) return;
            int r = std::uniform_int_distribution<int>(0, sum - 1)(rng_);
            int k = 0;
            while (k < 2 && r >= s.weight[k]) r -= s.weight[k++];
            dtp = s.dtp[k];
        }
        p = bank_.find(dtp);
    }
    if (!p) return;
    // kind 1 (0x5B99C0): volume (u8 / 127), semitones and delay, each ± its variance (0x582C20 / 0x582CA0 / 0x582D50).
    const auto vary = [&](float v, float var) { return var ? v + std::uniform_real_distribution<float>(-var, var)(rng_) : v; };
    const float volume = s.kind == 1 ? std::clamp(vary(s.volume, s.volumeVar), 0.0f, 1.0f) : 1;
    const float semitones = s.kind == 1 ? std::clamp(vary(s.semitones, s.semitonesVar), -60.0f, 60.0f) : 0;
    const double delay = s.kind == 1 ? std::max(0.0f, vary(s.delay, s.delayVar)) : 0;
    const auto h = player_.start(*p, position, volume, material, semitones, delay);
    // Root Assignments wrap a Reference: log the DTP it names.
    if (!dtp && p->type == audio::PlexType::Assignment && !p->children.empty() && p->children[0].type == audio::PlexType::Reference)
        dtp = p->children[0].id;
    std::fprintf(stderr, "ou-game: tick %llu anim sound %s frame %u DTP %u material %u%s", (unsigned long long)tick, fx.name.c_str(),
                 e.frame, dtp, material, h ? "" : " (silent)");
    if (s.kind == 1) std::fprintf(stderr, " volume %.2f semitones %.2f delay %.2f", volume, semitones, delay);
    std::fprintf(stderr, "\n");
}

void GameSound::syncUnits(const game::World& world) {
    std::set<std::string> present;
    for (const auto& u : world.units()) {
        present.insert(u.data.name);
        if (units_.contains(u.data.name) || !u.data.drm) continue;
        try {
            bank_.add(u.data.drm);
            mixer_.addWaves(*u.data.drm);
            auto markers = audio::parseSfxMarkers(*u.data.drm);
            audio::SfxMarkerTriggers triggers(markers);
            UnitSfx& us = units_.emplace(u.data.name, UnitSfx{u.data.drm, std::move(markers), std::move(triggers), {}, {}}).first->second;
            // Enemy chatter: single DTPs whose streams are vo\enemy\<who>\b_<voice>_<category>_<g|s>_NN (AUDIO.md §9.10)
            const auto stream = [](const auto& self, const audio::Plex& p) -> std::string {
                if (p.type == audio::PlexType::Stream) return p.stream;
                for (const auto& c : p.children)
                    if (auto s = self(self, c); !s.empty()) return s;
                return {};
            };
            for (const auto& sec : u.data.drm->sections) {
                if (sec.type != cdc::SectionType::DtpData) continue;
                const audio::Plex* p = bank_.find(sec.id);
                const std::string path = p ? stream(stream, *p) : std::string();
                if (!path.starts_with("vo\\enemy\\")) continue;
                const std::string file = path.substr(path.rfind('\\') + 1);
                const size_t a = file.find('_', file.find('_') + 1), b = a == std::string::npos ? a : file.find('_', a + 1);
                if (b == std::string::npos) continue;
                std::string category = file.substr(a + 1, b - a - 1);
                for (char& c : category) c = char(std::toupper(static_cast<unsigned char>(c)));
                us.barks[category].push_back(sec.id);
            }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ou-game: sfxmarkers of %s: %s\n", u.data.name.c_str(), e.what());
            units_.emplace(u.data.name, UnitSfx{u.data.drm, {}, audio::SfxMarkerTriggers({}), {}, {}});
        }
    }
    for (auto it = units_.begin(); it != units_.end();) { // streamed out: their sounds end
        if (present.contains(it->first)) { ++it; continue; }
        for (const auto& [k, h] : it->second.handles) player_.stop(h);
        it = units_.erase(it);
    }
}

void GameSound::tick(game::World& world) {
    const auto tick = (unsigned long long)world.ticks();
    if (world.unitsVersion() != unitsVersion_) syncUnits(world), unitsVersion_ = world.unitsVersion();
    const glm::vec3 lara = world.player().state().pos;
    // Enemies' slot sounds (owners -(0x10000000 | uid), ou-game's EffectsView) end with the enemy (0x58D740 stops a
    // SoundComponent's sounds when its instance goes).
    for (auto it = slots_.begin(); it != slots_.end();) {
        const int32_t owner = std::get<0>(it->first);
        const bool enemy = owner < 0 && (-owner & 0x10000000), gone = enemy && std::none_of(world.enemies().all().begin(), world.enemies().all().end(),
                                                                   [&](const auto& e) { return (e->uid() & 0x0FFFFFFF) == (-owner & 0x0FFFFFFF); });
        if (gone || !player_.active(it->second)) player_.stop(it->second), it = slots_.erase(it);
        else ++it;
    }
    for (const auto& b : world.enemies().barks) { // a line of that category from the loaded units, at the soldier
        std::vector<uint32_t> lines;
        for (const auto& [name, us] : units_)
            if (const auto it = us.barks.find(b.category); it != us.barks.end()) lines.insert(lines.end(), it->second.begin(), it->second.end());
        if (lines.empty()) continue;
        if (const audio::Plex* p = bank_.find(lines[rng_() % lines.size()])) player_.start(*p, b.pos);
        std::fprintf(stderr, "ou-game: tick %llu enemy %d bark %s\n", tick, b.uid, b.category.c_str());
    }
    for (const auto& u : world.units()) {
        const auto it = units_.find(u.data.name);
        if (it == units_.end()) continue;
        UnitSfx& s = it->second;
        for (const auto& f : s.triggers.update(s.markers, lara - u.offset)) { // 0x594870
            const auto& m = s.markers[f.marker];
            const auto& t = m.triggers[f.trigger];
            auto& vars = world.script().gameValues();
            switch (t.action) {
            case audio::SfxTrigger::Start:
                for (const int32_t r : t.refs) {
                    if (r < 0 || size_t(r) >= m.sounds.size()) continue;
                    auto& h = s.handles[{f.marker, size_t(r)}];
                    if (player_.active(h)) continue; // still playing: not restarted
                    h = player_.start(m.sounds[size_t(r)], m.position + u.offset);
                    std::fprintf(stderr, "ou-game: tick %llu sfxmarker %s #%zu (id %u) start sound %d%s\n", tick, u.data.name.c_str(),
                                 f.marker, m.id, r, h ? "" : " (silent)");
                }
                break;
            case audio::SfxTrigger::End:
                for (const int32_t r : t.refs)
                    if (const auto h = s.handles.find({f.marker, size_t(r)}); h != s.handles.end()) player_.stop(h->second), s.handles.erase(h);
                break;
            case audio::SfxTrigger::SetVariable: vars[t.variable] = t.value; break;
            case audio::SfxTrigger::IncVariable: vars[t.variable] = uint8_t(vars[t.variable] + t.value); break;
            case audio::SfxTrigger::DecVariable: vars[t.variable] = uint8_t(vars[t.variable] - t.value); break;
            default: break;
            }
        }
    }

    // Drowning: the line starts when the air runs out and repeats while she stays under [I] (lara.drm DTPs
    // 1014/1020 reference DTP 2315; the state that plays it is not decoded).
    const float air = world.air();
    if (air <= 0 && !world.health().dead() && !player_.active(drown_))
        if (const audio::Plex* p = bank_.find(kLaraDrown)) {
            drown_ = player_.start(*p, lara);
            std::fprintf(stderr, "ou-game: tick %llu drowning: Lara sound DTP %u\n", tick, kLaraDrown);
        }

    if (const auto* b = world.ridden(); b && b->phase == game::BikePhase::Ride) {
        if (!engine_) {
            if (!bank_.find(kBikeEngine))
                if (auto drm = load(bf_, "pc-w\\motorcycle.drm")) mixer_.addWaves(*drm), bank_.add(std::move(drm));
            if (const audio::Plex* p = bank_.find(kBikeEngine)) engine_ = player_.start(*p, lara);
            std::fprintf(stderr, "ou-game: tick %llu bike engine %s (DTP %u)\n", tick, engine_ ? "start" : "missing", kBikeEngine);
        }
        // [I] rpm 1500 at rest to 10000 at the top speed; parameter 5 = throttle (0..700), 0 = 0.
        const float speed = std::abs(b->speed), rpm = 1500 + 8500 * std::min(1.0f, speed / game::BikeTuning{}.maxSpeed);
        const float throttle = b->throttle * 700;
        player_.setPosition(engine_, lara);
        player_.setParam(engine_, 2, rpm), player_.setParam(engine_, 5, throttle), player_.setParam(engine_, 0, 0);
        if (tick % 60 == 0) std::fprintf(stderr, "ou-game: tick %llu bike engine rpm %.0f throttle %.0f\n", tick, rpm, throttle);
    } else if (engine_) {
        player_.stop(engine_, 0.5f), engine_ = 0;
        std::fprintf(stderr, "ou-game: tick %llu bike engine stop\n", tick);
    }

    jointSounds(world);
    player_.update(game::kTickSeconds);
    for (const auto& l : player_.lines) {
        std::fprintf(stderr, "ou-game: tick %llu voice %s \"%s\"\n", tick, l.path.c_str(), l.subtitle.c_str());
        for (const auto& line : locale::cineSubtitleLines(l.subtitle)) ticker_.push(line);
    }
    player_.lines.clear(), player_.waves.clear();
    ticker_.advance(game::kTickSeconds * 30); // ticker ticks are 1/30 s

    if (auto c = world.script().helpCondition(); c != lastCondition_) {
        std::fprintf(stderr, "ou-game: tick %llu help condition %s\n", tick, c.c_str());
        lastCondition_ = std::move(c);
    }
    auto& vars = world.script().gameValues(); // music variables: written by gamebase natives (and sfxmarkers)
    for (size_t i = 0; i < vars.size(); ++i)
        if (vars[i] != lastVars_[i]) {
            std::fprintf(stderr, "ou-game: tick %llu music var 0x%02zX %u -> %u\n", tick, i, lastVars_[i], vars[i]);
            lastVars_[i] = vars[i];
        }
}

void drawSubtitles(ui::Canvas& canvas, const locale::SubtitleTicker& ticker) {
    ui::Hud::subtitles(canvas, ticker.top(), ticker.bottom());
}

} // namespace ou::app
