// Enemy types from object DRMs (skeleton, role clips, enemy data) and enemy / generator intros of the loaded
// units. Layouts and evidence: docs/GAMEPLAY.md "Enemies".
#include "game/ai/Enemy.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"
#include "game/Game.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace ou::game::ai {

namespace {

constexpr float kPi = 3.14159265f;
constexpr uint32_t kEnemyDataMagic = 0xDAF00018; // tru.exe 0x63C190 refuses enemy data without it
constexpr uint32_t kEnemyFamily = 0x400000;     // ObjectBase oflags2 (+4) bit: "enemy" family (0x5BF0E0)

template <class T> T read(const cdc::Section& s, uint32_t off) {
    if (off + sizeof(T) > s.data.size()) throw cdc::FormatError("enemy data: read past the section end");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}

std::optional<cdc::Drm> readObject(const cdc::BigfileSet& bf, const std::string& name) {
    auto bytes = bf.read("pc-w\\" + name + ".drm");
    if (!bytes) return std::nullopt;
    return cdc::parseDrm(std::move(*bytes));
}

uint32_t oflags2(const cdc::Drm& d) {
    const auto base = formats::objectBaseSection(d);
    return base ? read<uint32_t>(d.sections[*base], 4) : 0;
}

// Relocated pointer stored at `off` in section `section`.
std::optional<cdc::Pointer> ptrAt(const cdc::Drm& d, uint32_t section, uint32_t off) {
    const auto p = cdc::sectionPointers(d, section);
    const auto it = p.find(off);
    return it == p.end() ? std::nullopt : std::optional(it->second);
}

float deg(int16_t d) { return float(d) * kPi / 180.0f; }

// Senses record (layout: Senses in Enemy.hpp).
Senses readSenses(const cdc::Section& s, uint32_t o) {
    const auto cone = [&](uint32_t c) { return Cone{float(read<int16_t>(s, c)), deg(read<int16_t>(s, c + 2)), deg(read<int16_t>(s, c + 4))}; };
    Senses r;
    r.body = {cone(o + 4), cone(o + 0xA)};
    const auto pick = [&](uint32_t k) { const int16_t v = read<int16_t>(s, o + 0x10 + k); return v >= 0 ? v : read<int16_t>(s, o + 4 + k); };
    r.eye = {float(pick(0)), deg(pick(2)), deg(pick(4))};
    r.ray = read<uint8_t>(s, o + 0x44) == 0;
    r.loseTrack = read<float>(s, o + 0x20);
    return r;
}
} // namespace

// Clip names tried per role, first match wins: cats / thralls / spiders, soldiers (poacher, merc), bats. Fire, reload
// and death clips come from the enemy data records instead.
const std::array<std::vector<std::string_view>, size_t(Role::Count)> kRoleCandidates = {{
    {"passive_idle", "idle_passive", "idle"},                                         // Idle
    {"attack_idle", "idle_aggresive", "idle_combat_stand", "swim_forward_medium"},   // Alert
    {"walk_forward_2", "walk", "combat_walk_forward", "bat_fly_passive", "swim_forward_slow"}, // Walk
    {"run_forward_2", "run", "combat_run_forward", "bat_fly_aggressive", "swim_forward_fast", "swim_forward_medium"}, // Run
    {"simple_attack_static1", "rifle_bash", "attack_bite", "simple_attack", "slash_attack", "slash", "bite_2", "burst_attack"}, // Attack
    {"hitreact_medium_front", "hitreact_medium", "hit_front", "hit_react_light_front", "hit_react_medium", "hit"}, // Hit
    {"death_static", "death_shot", "death", "damage_front_death"},                    // Death
    {"spawn_from_pool"},                                                              // Spawn
}};

bool isEnemyObject(const cdc::BigfileSet& bigfile, const std::string& name) {
    const auto d = readObject(bigfile, name);
    return d && (oflags2(*d) & kEnemyFamily);
}

std::shared_ptr<const EnemyType> loadEnemyType(const cdc::BigfileSet& bigfile, const std::string& name) {
    const auto drm = readObject(bigfile, name);
    if (!drm) throw std::runtime_error("pc-w\\" + name + ".drm not in bigfile");
    const auto base = formats::objectBaseSection(*drm);
    if (!base || !(oflags2(*drm) & kEnemyFamily)) throw std::runtime_error(name + ": not an enemy object");
    auto t = std::make_shared<EnemyType>();
    t->name = name;
    for (const auto& s : drm->sections)
        if (s.type == cdc::SectionType::Object) t->objectId = uint16_t(s.id); // object section id = objectlist id
    // Enemy data: ObjectBase +0xC8 (the instance+0x50 data tru.exe 0x63C190 reads).
    const auto data = ptrAt(*drm, *base, 0xC8);
    if (!data) throw cdc::FormatError(name + ": ObjectBase +0xC8 has no enemy data");
    const cdc::Section& ed = drm->sections[data->section];
    const uint32_t o = data->offset;
    if (read<uint32_t>(ed, o) != kEnemyDataMagic) throw cdc::FormatError(name + ": enemy data magic mismatch");
    t->enemyClass = int32_t(read<uint32_t>(ed, o + 4));
    t->behaviour = read<uint8_t>(ed, o + 0xC);
    t->tuning.radius = read<float>(ed, o + 0x2C);
    // Component list (+0x20, next +0x1C): each field is read from the first node where it is non-null (getters
    // 0x614400..0x614940).
    const auto field = [&](uint32_t off) -> std::optional<cdc::Pointer> {
        auto node = ptrAt(*drm, data->section, o + 0x20);
        for (int hop = 0; node && hop < 16; ++hop, node = ptrAt(*drm, node->section, node->offset + 0x1C))
            if (const auto p = ptrAt(*drm, node->section, node->offset + off)) return p;
        return std::nullopt;
    };
    // Hit points: node +0x3C health block {easy, normal, hard} floats; tru.exe 0x63C190 -> 0x620CC0 -> 0x6E77B0 picks by
    // difficulty (non-positive -> normal) into the health object at comp+0x2C64. We play Normal.
    if (const auto h = field(0x3C)) {
        t->tuning.health = read<float>(drm->sections[h->section], h->offset + 4);
        // [I] block +0xC (cats 80, thralls 0), copied to comp+0x2C6C next to the health object: read as the damage it
        // takes to stagger (EnemyHealth::m_damageSinceMove, TRLAU-menu-hook) — unconfirmed.
        t->tuning.staggerDamage = read<float>(drm->sections[h->section], h->offset + 0xC);
    }
    if (const auto s = field(0x30)) t->tuning.senses = t->tuning.combatSenses = readSenses(drm->sections[s->section], s->offset);
    if (const auto s = field(0x38)) t->tuning.combatSenses = readSenses(drm->sections[s->section], s->offset);
    if (const auto w = field(0x20)) t->weapon = read<uint16_t>(drm->sections[w->section], w->offset + 2); // 0x614400 -> 0x614950 attaches it

    // Clip refs {s16 type, s16 idx}: type 1 -> ObjectBase +0x60 table (count +0x5C, 0x24-byte entries), entry +4 -> s16
    // animation-list index (FUN_00626F10 / FUN_004325d0). Type 2 (anim requests) is not resolved: "".
    const auto list = anim::animList(*drm);
    const auto table = ptrAt(*drm, *base, 0x60);
    const uint32_t tableCount = read<uint32_t>(drm->sections[*base], 0x5C);
    const auto clipRef = [&](const cdc::Section& s, uint32_t off) -> std::string {
        const int16_t type = read<int16_t>(s, off), idx = read<int16_t>(s, off + 2);
        if (type != 1 || idx < 0 || uint32_t(idx) >= tableCount || !table) return {};
        const auto e = ptrAt(*drm, table->section, table->offset + 0x24 * uint32_t(idx) + 4);
        if (!e) return {};
        const int16_t a = read<int16_t>(drm->sections[e->section], e->offset);
        return a >= 0 && size_t(a) < list.size() ? list[size_t(a)].name : std::string();
    };
    for (uint32_t off = 0x68; off <= 0x9C; off += 4) {
        const auto p = field(off);
        if (!p) continue;
        const cdc::Section& s = drm->sections[p->section];
        const uint32_t r = p->offset;
        t->tuning.attacks.push_back({off, clipRef(s, r + 0x78), clipRef(s, r + 0x7C), clipRef(s, r + 0x84),
                                     read<float>(s, r + 0x154), int(read<uint32_t>(s, r + 0x158)), read<int16_t>(s, r + 0x13E),
                                     read<int16_t>(s, r + 0x144), read<int16_t>(s, r + 0x13A), float(read<uint32_t>(s, r + 0x34)) / 1000.0f,
                                     deg(read<uint8_t>(s, r + 0x1F0))});
    }
    if (const auto p = field(0x64)) {
        const cdc::Section& s = drm->sections[p->section];
        const uint32_t n = std::min(read<uint32_t>(s, p->offset + 0xC8), 32u);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t e = p->offset + 0xCC + 0x14 * k;
            t->tuning.deaths.push_back({clipRef(s, e), int(read<uint32_t>(s, e + 0xC)), read<uint8_t>(s, e + 0x10)});
        }
    }

    // Model-0 skeleton and the role clips.
    uint32_t model = 0;
    for (const uint32_t id : formats::objectRenderModels(*drm))
        if ((model = id)) break;
    std::vector<std::string> warnings;
    for (auto& sk : anim::decodeSkeletons(*drm, warnings))
        if (model && sk.renderModelId == model) t->skeleton = std::move(sk);
    if (t->skeleton.bones.empty()) throw cdc::FormatError(name + ": no skeleton for render model " + std::to_string(model));
    float top = 0;
    for (const vec3 p : anim::bindPositions(t->skeleton)) top = std::max(top, p.z);
    if (top > t->tuning.radius) t->tuning.height = top; // [I] capsule up to the highest bind-pose bone

    const auto loadClip = [&](std::string_view want) {
        if (want.empty() || t->clips.count(want)) return !want.empty();
        const auto e = std::find_if(list.begin(), list.end(), [&](const anim::AnimEntry& x) { return x.name == want; });
        if (e == list.end()) return false;
        const auto s = std::find_if(drm->sections.begin(), drm->sections.end(), [&](const cdc::Section& x) {
            return x.type == cdc::SectionType::Animation && x.id == e->animId;
        });
        if (s == drm->sections.end()) return false;
        t->clips.emplace(std::string(want), anim::decodeClip(s->data));
        return true;
    };
    for (size_t r = 0; r < size_t(Role::Count); ++r)
        for (const auto want : kRoleCandidates[r])
            if (loadClip(want)) {
                t->roles[r] = std::string(want);
                break;
            }
    for (const auto& a : t->tuning.attacks)
        for (const auto* c : {&a.clip, &a.shotClip, &a.followClip}) loadClip(*c);
    for (const auto& d : t->tuning.deaths) loadClip(d.clip);

    // Melee damage per blow: the record whose clip is our attack clip (panther simple_attack_static1 -> +0x88, 50);
    // [I] a single melee record otherwise (bat +0x80 attach_bite_loop while we play attack_bite). No record (thralls):
    // the [I] 120 default.
    std::vector<const Attack*> melee;
    for (const auto& a : t->tuning.attacks)
        if (!a.ranged() && !a.clip.empty()) melee.push_back(&a);
    const auto m = std::find_if(melee.begin(), melee.end(), [&](const Attack* a) { return a->clip == t->roles[size_t(Role::Attack)]; });
    if (const Attack* a = m != melee.end() ? *m : melee.size() == 1 ? melee[0] : nullptr) {
        t->tuning.attackDamage = a->damage;
        t->tuning.attackType = a->type == 2 ? kDamageHeavy : kDamageLight;
    }
    if (t->flyer()) { // [I] bites from just outside its radius, one per 1.5 s (the latch record's 10 s cooldown is unused)
        t->tuning.attackRange = t->tuning.radius + 150.0f;
        t->tuning.attackReach = t->tuning.radius + 250.0f;
        t->tuning.attackCooldown = 1.5f;
    }
    if (t->aquatic()) {
        // Sharks have no attack records: their clip set supplies the locomotion and the unrecorded melee damage
        // remains the inferred 120 default. Use the actual fast-swim root cycle rather than the bat's speed.
        const anim::Clip* run = t->clip(Role::Run);
        const double seconds = run ? run->durationSeconds() : 0;
        if (run && seconds > 0) {
            const vec3 cycle = rootTranslation(*run, seconds, true) - rootTranslation(*run, 0, true);
            if (const float distance = glm::length(cycle); distance > 1e-3f) t->tuning.flySpeed = distance / float(seconds);
        }
        loadClip("death_float_loop");
    }

    // Death clips that end lying down: the last frame's median bone height under a third of the bind pose's (the
    // median, as helper bones such as the dropped rifle fly far off).
    const auto median = [](std::vector<float> z) {
        std::nth_element(z.begin(), z.begin() + long(z.size() / 2), z.end());
        return z.empty() ? 0.0f : z[z.size() / 2];
    };
    std::vector<float> bind;
    for (const vec3 p : anim::bindPositions(t->skeleton)) bind.push_back(p.z);
    for (const auto& [n, c] : t->clips) {
        if (n.find("death") == std::string::npos && n.find("hitreact_thrown") == std::string::npos) continue;
        std::vector<float> z;
        for (const auto& b : anim::worldMatrices(t->skeleton, anim::sample(c, t->skeleton, c.durationSeconds(), false))) z.push_back(b[3].z);
        if (median(z) < median(bind) / 3) t->lyingDeaths.insert(n);
    }
    return t;
}

std::shared_ptr<const EnemyType> Enemies::typeOf(const std::string& name) {
    if (const auto it = types_.find(name); it != types_.end()) return it->second;
    auto& t = types_[name]; // tried once: nullptr = not spawnable
    if (!bigfile_) return nullptr;
    try {
        t = loadEnemyType(*bigfile_, name);
        if (!t->spawnable()) {
            warnings.push_back("enemy " + name + ": no idle/run/death clip with an attack clip or fire record; not spawned");
            t = nullptr;
        }
    } catch (const std::exception& e) {
        warnings.push_back("enemy " + name + ": " + e.what());
    }
    return t;
}

void Enemy::restore(const EnemyState& s) {
    s_ = prev_ = s;
    updatePose(0, true);
}

void Enemies::setUnits(const std::vector<script::UnitRef>& units) {
    const auto loaded = [&](const std::string& n) {
        return std::any_of(units.begin(), units.end(), [&](const script::UnitRef& u) { return u.name == n; });
    };
    const size_t before = enemies_.size();
    std::erase_if(enemies_, [&](const auto& e) {
        if (e->unit.empty() || loaded(e->unit)) return false;
        EnemyState s = e->state();
        s.pos -= offsets_[e->unit], s.lastSeen -= offsets_[e->unit];
        stash_[e->uid()] = s; // generator spawns: uid = their generator's, re-spawned with it below
        return true;
    });
    std::erase_if(generators_, [&](const Generator& g) {
        if (loaded(g.unit)) return false;
        if (g.fired) firedGenerators_.insert(g.uid);
        return true;
    });
    if (enemies_.size() != before) ++version_, sync();
    std::erase_if(units_, [&](const std::string& n) { return !loaded(n); });
    if (!bigfile_) return;
    for (const auto& u : units) {
        if (!u.drm || std::find(units_.begin(), units_.end(), u.name) != units_.end()) continue;
        units_.push_back(u.name);
        offsets_[u.name] = u.offset;
        if (objectNames_.empty()) {
            const auto list = bigfile_->read("\\tr8\\pc-w\\objectlist.txt");
            if (!list) throw cdc::FormatError("\\tr8\\pc-w\\objectlist.txt not in bigfile");
            objectNames_ = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        }
        for (const auto& in : formats::parseUnit(*u.drm).intros) {
            const auto n = objectNames_.find(in.objectId);
            if (n == objectNames_.end()) continue;
            const vec3 pos = vec3(in.position[0], in.position[1], in.position[2]) + u.offset;
            const float yaw = in.rotation[2] - kPi / 2; // intro rotation z is the model yaw (as findSpawn)
            if (n->second.ends_with("_generator")) {
                // [O] generator ObjectBase +0xDE u16 = the spawned object (mayan_thrall_pool_generator -> 600 mayan_thrall)
                auto& spawned = generatorObjects_[n->second];
                if (spawned.empty())
                    if (const auto d = readObject(*bigfile_, n->second))
                        if (const auto b = formats::objectBaseSection(*d))
                            if (const auto it = objectNames_.find(read<uint16_t>(d->sections[*b], 0xDE)); it != objectNames_.end())
                                spawned = it->second;
                if (spawned.empty()) continue;
                const bool fired = firedGenerators_.contains(in.uniqueId);
                generators_.push_back({in.uniqueId, u.name, spawned, pos, yaw, 2528.45f, fired});
                if (const auto s = stash_.find(in.uniqueId); fired && s != stash_.end())
                    if (auto type = typeOf(spawned)) {
                        auto& e = add(std::move(type), in.uniqueId, pos, yaw);
                        e.unit = u.name, e.generator = in.uniqueId;
                        EnemyState st = s->second;
                        st.pos += u.offset, st.lastSeen += u.offset;
                        e.restore(st);
                    }
                continue;
            }
            auto& isEnemy = enemyObjects_.try_emplace(n->second, -1).first->second;
            if (isEnemy < 0) isEnemy = isEnemyObject(*bigfile_, n->second);
            if (!isEnemy) continue;
            auto type = typeOf(n->second);
            if (!type) continue;
            auto& e = add(std::move(type), in.uniqueId, pos, yaw);
            e.unit = u.name;
            if (const auto s = stash_.find(in.uniqueId); s != stash_.end()) {
                EnemyState st = s->second;
                st.pos += u.offset, st.lastSeen += u.offset;
                e.restore(st);
            }
        }
    }
}

namespace {
nlohmann::json vec(vec3 v) { return {v.x, v.y, v.z}; }
vec3 vec(const nlohmann::json& j) { return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }

nlohmann::json toJson(const EnemyState& s) {
    return {{"mode", int(s.mode)}, {"pos", vec(s.pos)}, {"yaw", s.yaw}, {"vz", s.vz}, {"clip", s.clip}, {"clipTime", s.clipTime},
            {"loop", s.loop}, {"health", s.health}, {"modeTime", s.modeTime}, {"unseenTime", s.unseenTime},
            {"cooldown", s.cooldown}, {"blowLanded", s.blowLanded}, {"damageSinceReaction", s.damageSinceReaction},
            {"lastSeen", vec(s.lastSeen)}, {"fire", s.fire}, {"shotsLeft", s.shotsLeft}, {"shotTimer", s.shotTimer},
            {"fireCooldown", s.fireCooldown}, {"rng", s.rng}, {"tip", s.tip}};
}

EnemyState stateFromJson(const nlohmann::json& j) {
    EnemyState s;
    const int mode = j.at("mode").get<int>();
    if (mode < 0 || mode > int(Mode::Reload)) throw std::runtime_error("enemy mode " + std::to_string(mode));
    s.mode = Mode(mode);
    s.pos = vec(j.at("pos")), s.yaw = j.at("yaw"), s.vz = j.at("vz"), s.clip = j.at("clip"), s.clipTime = j.at("clipTime");
    s.loop = j.at("loop"), s.health = j.at("health"), s.modeTime = j.at("modeTime"), s.unseenTime = j.at("unseenTime");
    s.cooldown = j.at("cooldown"), s.blowLanded = j.at("blowLanded"), s.damageSinceReaction = j.at("damageSinceReaction");
    s.lastSeen = vec(j.at("lastSeen")), s.fire = j.at("fire"), s.shotsLeft = j.at("shotsLeft"), s.shotTimer = j.at("shotTimer");
    s.fireCooldown = j.at("fireCooldown"), s.rng = j.at("rng"), s.tip = j.at("tip");
    return s;
}
} // namespace

nlohmann::json Enemies::serialize() const {
    std::map<int32_t, EnemyState> states = stash_; // streamed out; loaded ones overwrite below
    std::set<int32_t> fired = firedGenerators_;
    for (const auto& e : enemies_) {
        if (e->unit.empty()) continue; // added by hand (tests), not from a unit
        EnemyState s = e->state();
        const auto off = offsets_.find(e->unit);
        const vec3 o = off == offsets_.end() ? vec3(0) : off->second;
        s.pos -= o, s.lastSeen -= o;
        states[e->uid()] = s;
    }
    for (const auto& g : generators_)
        if (g.fired) fired.insert(g.uid);
    nlohmann::json st = nlohmann::json::object();
    for (const auto& [uid, s] : states) st[std::to_string(uid)] = toJson(s);
    return {{"states", st}, {"firedGenerators", fired}, {"deadAtCheckpoint", deadAtCheckpoint_}};
}

void Enemies::restore(const nlohmann::json& j) {
    std::map<int32_t, EnemyState> states;
    std::set<int32_t> fired, dead;
    try {
        for (const auto& [uid, s] : j.at("states").items()) states[std::stoi(uid)] = stateFromJson(s);
        fired = j.at("firedGenerators").get<std::set<int32_t>>();
        dead = j.at("deadAtCheckpoint").get<std::set<int32_t>>();
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("enemy state: ") + e.what());
    }
    std::erase_if(enemies_, [](const auto& e) { return !e->unit.empty(); });
    generators_.clear();
    units_.clear(); // setUnits() spawns every loaded unit again, from the restored stash
    stash_ = std::move(states), firedGenerators_ = std::move(fired), deadAtCheckpoint_ = std::move(dead);
    ++version_;
    sync();
}

} // namespace ou::game::ai
