// Enemy types from object DRMs (skeleton, role clips, enemy data) and enemy / generator intros of the loaded
// units. Layouts and evidence: docs/GAMEPLAY.md "Enemies".
#include "game/ai/Enemy.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"

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

} // namespace

// Cats (cat_panther, panther_thrall, tiger), thralls, spiders / tarantulas. Bats (flight clips only) and soldiers
// (guns) get no Run / Attack and are not spawned.
const std::array<std::vector<std::string_view>, size_t(Role::Count)> kRoleCandidates = {{
    {"passive_idle", "idle"},                                       // Idle
    {"attack_idle", "idle_aggresive"},                              // Alert
    {"walk_forward_2", "walk"},                                     // Walk
    {"run_forward_2", "run"},                                       // Run
    {"simple_attack_static1", "simple_attack", "slash_attack", "slash"}, // Attack
    {"hitreact_medium_front", "hitreact_medium", "hit_front", "hit_react_light_front", "hit_react_medium"}, // Hit
    {"death_static", "death", "damage_front_death"},                // Death
    {"spawn_from_pool"},                                            // Spawn
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
    t->tuning.radius = read<float>(ed, o + 0x2C);
    // Hit points: the first node of the enemy data's list (+0x20, next +0x1C) with a health block at +0x3C,
    // {easy, normal, hard} floats; tru.exe 0x63C190 -> 0x620CC0 -> 0x6E77B0 picks by difficulty (non-positive ->
    // normal) into the health object at comp+0x2C64. We play Normal.
    auto node = ptrAt(*drm, data->section, o + 0x20);
    for (int hop = 0; node && hop < 16; ++hop, node = ptrAt(*drm, node->section, node->offset + 0x1C))
        if (const auto h = ptrAt(*drm, node->section, node->offset + 0x3C)) {
            t->tuning.health = read<float>(drm->sections[h->section], h->offset + 4);
            // [I] block +0xC (cats 80, thralls 0), copied to comp+0x2C6C next to the health object: read as the
            // damage it takes to stagger (EnemyHealth::m_damageSinceMove, TRLAU-menu-hook) — unconfirmed.
            t->tuning.staggerDamage = read<float>(drm->sections[h->section], h->offset + 0xC);
            break;
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

    const auto list = anim::animList(*drm);
    for (size_t r = 0; r < size_t(Role::Count); ++r)
        for (const auto want : kRoleCandidates[r]) {
            const auto e = std::find_if(list.begin(), list.end(), [&](const anim::AnimEntry& x) { return x.name == want; });
            if (e == list.end()) continue;
            const auto s = std::find_if(drm->sections.begin(), drm->sections.end(), [&](const cdc::Section& x) {
                return x.type == cdc::SectionType::Animation && x.id == e->animId;
            });
            if (s == drm->sections.end()) continue;
            t->clips.emplace(std::string(want), anim::decodeClip(s->data));
            t->roles[r] = std::string(want);
            break;
        }
    return t;
}

std::shared_ptr<const EnemyType> Enemies::typeOf(const std::string& name) {
    if (const auto it = types_.find(name); it != types_.end()) return it->second;
    auto& t = types_[name]; // tried once: nullptr = not spawnable
    if (!bigfile_) return nullptr;
    try {
        t = loadEnemyType(*bigfile_, name);
        if (!t->melee()) {
            warnings.push_back("enemy " + name + ": no idle/run/attack/death clip set (ranged and flying enemies are not implemented); not spawned");
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
        if (!e->generator) { // ponytail: generator spawns are not kept; their generator stays fired
            EnemyState s = e->state();
            s.pos -= offsets_[e->unit], s.lastSeen -= offsets_[e->unit];
            stash_[e->uid()] = s;
        }
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
                if (!spawned.empty())
                    generators_.push_back({in.uniqueId, u.name, spawned, pos, yaw, 2528.45f, firedGenerators_.contains(in.uniqueId)});
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

} // namespace ou::game::ai
