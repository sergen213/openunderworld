// Lara's close combat and carried objects. Evidence and numbers: docs/GAMEPLAY.md "Close combat", "Carried objects".
//
// [O] lara.drm's player state chart (player data +0x12508; tru.exe conditions 0x8B2838, actions 0x8B0A50):
// - Interact (chart input 32) with melee allowed (state property 1002: the ground and combat families) and the melee
//   target (targeting slot 5, 0x71CAF0(5)) kicks it by its height (condition 0x7D8 = 0x765810: melee_kick_low / _mid /
//   _high / _highest, transitions T36 / T2367..T2370). Without a target and with nothing to interact with (0x591 = 0,
//   T38): melee_kick_move while the stick runs her forward (S1424), else melee_kick_mid. From the sprint (S59, T177):
//   melee_kick_move with its own attack record (S1432: 3 damage). All kick states inherit S1426's record (60, class 2).
// - Interact with a fatality target (slot 6, condition 0x7DA; T1347): melee_thrall_fatality (S738), whose animation
//   event 0x6D at frame 23 posts the hit to it (0x6E6840: record S737, 100 damage, class 16).
// - The blow of a kick is a limb contact: animation event 0xE opens it, 0xD closes it (0x716F30 -> 0x6FFED0 posts 0x8F /
//   0x94 with the limb to Lara and to the portable she holds). Ours [I]: her leading foot's sphere over those frames.
// - Jump while she runs at the melee target in combat (condition 0x7DB, T1405 -> S736): the enemy vault; her clip is
//   the enemy's own hosted_enemy_vault, the record 0 damage, class 1.
// - Target lock + crouch with the stick (T1355 -> S744): combat_roll<front|back|left|right> by the stick's quadrant
//   relative to her; + jump (T1367 -> S751): the combat_jump<b|l|r> leaps (forward and neutral stay the plain jump).
// - Carried portables (S1313 family): chart input 101 puts it down (S1302, the portable's hosted clip slot 4 =
//   hosted_putdown, event 0x4F -> message 0x156), the weapon chart's input 102 throws it (S100 -> S780, slot 5 =
//   hosted_throw, event 0x53 -> message 0x1AF -> 0x6B53A0), Interact with the melee target swings it (S1311, slot 10 =
//   hosted_melee / hosted_melee_front, record 100, class 2). A weight (data +0x18) walks with the
//   portable_encumbered_* clips (S1316..S1322).
// [O] The keys of inputs 101 / 102: Crouch and Grenade. generalbank.drm's action map (DTP 47, GlobalInfo +0x1C, tru.exe
// 0x480BC0) binds action 101 to its control 1 (virtual button 2, Crouch/Roll) and 102 to control 5 (button 7, which the
// Control Configuration names "Grenade (throw held object)"); 32 and 99 to control 2 (button 0, Interact).
#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::game {

const std::vector<std::string_view> kMeleeClipNames = {
    "melee_kick_low",      "melee_kick_mid",      "melee_kick_high",    "melee_kick_highest",  "melee_kick_move",
    "melee_thrall_fatality", "combat_rollfront",  "combat_rollback",    "combat_rollleft",     "combat_rollright",
    "combat_jumpl_takeoff", "combat_jumpl_inair", "combat_jumpr_takeoff", "combat_jumpr_inair", "combat_jumpb_takeoff",
    "combat_jumpb_inair",  "Hit_ThrownFront",     "Hit_LandFront",      "Hit_GetupFront",      "hitReact_thrownBack",
    "hitReact_landBack",   "Hit_getupback",       "portable_encumbered_idle", "portable_encumbered_idle_to_walk",
    "portable_encumbered_walk", "portable_encumbered_walk_to_idle",
};

namespace {
constexpr float kPi = 3.14159265f;
float wrap(float a) { return std::remainder(a, 2 * kPi); }

// [I] numbers of ours (docs/GAMEPLAY.md "Close combat"): the foot's blow sphere, how far her leg reaches the target she
// kicks at (gap between the two capsules), how fast she closes on it before the blow, the gap she stops at.
constexpr float kFootRadius = 70.0f, kKickReach = 190.0f, kApproachSpeed = 1200.0f, kContactGap = 60.0f;
constexpr float kThrownSeconds = 1.5f;   // [O] a thrown portable's weapon time (0x6B5890 case 0x1AF: now + 1500 ms)
constexpr float kDropShove = 300.0f;     // [O value, I unit] message 0x156 with parameter 0: along her heading
constexpr float kThrowGravity = 3.0f;    // [O] units per frame^2 in the launch solve (0x6B53A0: 0xC0400000)
constexpr float kLieSeconds = 1.0f;      // [I] knocked down: how long she lies before getting up by herself
constexpr float kEvadeSpeed = 1200.0f, kEvadeVz = 900.0f; // [I] the evasive leaps' launch (units/s)

template <class T>
T rd(std::span<const uint8_t> d, size_t o) {
    T v{};
    if (o + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + o, sizeof(T));
    return v;
}

// An object's ObjectBase section and its data block (ObjectBase +0xC8).
struct ObjData {
    cdc::Drm drm;
    uint32_t base = 0, section = 0, offset = 0;
};
std::optional<ObjData> objData(const cdc::BigfileSet& bf, std::string_view object) {
    auto bytes = bf.read("pc-w\\" + std::string(object) + ".drm");
    if (!bytes) return std::nullopt;
    ObjData o{cdc::parseDrm(std::move(*bytes))};
    const auto obj = std::ranges::find_if(o.drm.sections, [](const cdc::Section& s) { return s.type == cdc::SectionType::Object; });
    if (obj == o.drm.sections.end() || obj->data.size() < 4) return std::nullopt;
    const uint32_t baseId = rd<uint32_t>(obj->data, 0);
    for (uint32_t i = 0; i < o.drm.sections.size(); ++i) {
        if (o.drm.sections[i].type != cdc::SectionType::DtpData || o.drm.sections[i].id != baseId) continue;
        const auto ptrs = cdc::sectionPointers(o.drm, i);
        const auto it = ptrs.find(0xC8);
        if (it == ptrs.end()) return std::nullopt;
        o.base = i, o.section = it->second.section, o.offset = it->second.offset;
        return o;
    }
    return std::nullopt;
}

// The ObjectBase animation list (+0x54 count, +0x58 {i16 anim, i16, u32 event-list DTP id, char* name}): each clip's
// command events (records with info & 3 == 2: +8 i16 command, +0xA i16 parameter, +2 frame) and its animation id.
struct ClipEvents {
    uint32_t animId = 0;
    std::vector<std::array<int, 3>> commands; // {command, parameter, frame}
};
std::map<std::string, ClipEvents, std::less<>> clipEvents(const ObjData& o) {
    std::map<std::string, ClipEvents, std::less<>> out;
    const auto& s = o.drm.sections[o.base];
    const auto ptrs = cdc::sectionPointers(o.drm, o.base);
    const auto list = ptrs.find(0x58);
    const uint32_t n = rd<uint32_t>(s.data, 0x54);
    if (list == ptrs.end() || n > 8192) return out;
    const auto& ls = o.drm.sections[list->second.section];
    const auto lptrs = list->second.section == o.base ? ptrs : cdc::sectionPointers(o.drm, list->second.section);
    for (uint32_t i = 0; i < n; ++i) {
        const uint32_t e = list->second.offset + 12 * i;
        const auto name = lptrs.find(e + 8);
        if (name == lptrs.end()) continue;
        std::string nm;
        const auto& nd = o.drm.sections[name->second.section].data;
        for (size_t k = name->second.offset; k < nd.size() && nd[k]; ++k) nm += char(nd[k]);
        ClipEvents ce;
        ce.animId = uint32_t(uint16_t(rd<int16_t>(ls.data, e)));
        const uint32_t id = rd<uint32_t>(ls.data, e + 4);
        const auto ev = std::ranges::find_if(o.drm.sections, [&](const cdc::Section& x) { return x.type == cdc::SectionType::DtpData && x.id == id; });
        if (ev != o.drm.sections.end())
            for (size_t at = 4; at + 8 <= ev->data.size();) {
                const auto info = rd<uint16_t>(ev->data, at);
                if (info == 0xFFFF) break;
                if ((info & 3) == 2) ce.commands.push_back({rd<int16_t>(ev->data, at + 8), rd<int16_t>(ev->data, at + 10), rd<uint16_t>(ev->data, at + 2)});
                at += 8 + size_t(info >> 12) * 4;
            }
        out.emplace(std::move(nm), std::move(ce)); // the first entry of a name (the lists repeat some)
    }
    return out;
}

bool ended(const AnimLibrary& lib, const PlayerState& s) {
    const anim::Clip* c = lib.find(s.clip);
    return !c || (!s.loop && s.clipTime >= c->durationSeconds());
}
bool onFeet(State s) {
    return s == State::Idle || s == State::Walk || s == State::Run || s == State::Stop || s == State::Turn || s == State::Land;
}
// Distance from `p` to the vertical capsule axis of a target.
float axisDistance(vec3 p, const Damageable& t) {
    const vec3 base = t.position();
    const float z = std::clamp(p.z, base.z, base.z + t.height());
    return glm::length(p - vec3(base.x, base.y, z));
}
nlohmann::json vecJson(vec3 v) { return {v.x, v.y, v.z}; }
vec3 vecJson(const nlohmann::json& j) { return {j.at(0).get<float>(), j.at(1).get<float>(), j.at(2).get<float>()}; }
} // namespace

// ---- data ---------------------------------------------------------------------------------------
std::optional<MeleeTuning> loadMeleeTuning(const cdc::BigfileSet& bigfile) {
    const auto o = objData(bigfile, "lara");
    if (!o) return std::nullopt;
    MeleeTuning t;
    const std::span<const uint8_t> d = o->drm.sections[o->section].data;
    const auto ptrs = cdc::sectionPointers(o->drm, o->section);
    const auto ptr = [&](uint32_t off) -> std::optional<uint32_t> { // a pointer inside the data block's section
        const auto it = ptrs.find(off);
        if (it == ptrs.end() || it->second.section != o->section) return std::nullopt;
        return it->second.offset;
    };
    // Melee target height classes.
    t.low = rd<float>(d, o->offset + 0x2B4), t.mid = rd<float>(d, o->offset + 0x2B8), t.high = rd<float>(d, o->offset + 0x2BC);
    // The state chart: +0x12508 {u16 states, u16 transitions, -> states[]}; state +0xA property count, +0x14 ->
    // properties {u16 id, u16 type, u32 value}; property 1005 (type 27) -> the attack record.
    const uint32_t chart = o->offset + 0x12508;
    const uint32_t states = rd<uint16_t>(d, chart);
    const auto table = ptr(chart + 4);
    const auto record = [&](uint32_t state) -> std::optional<AttackRecord> {
        const auto st = table && state < states ? ptr(*table + 4 * state) : std::nullopt;
        const auto props = st ? ptr(*st + 0x14) : std::nullopt;
        if (!props) return std::nullopt;
        for (uint32_t k = 0, n = rd<uint16_t>(d, *st + 0xA); k < n; ++k)
            if (rd<uint16_t>(d, *props + 8 * k) == 1005)
                if (const auto r = ptr(*props + 8 * k + 4)) return AttackRecord{rd<float>(d, *r + 4), int(rd<uint32_t>(d, *r + 0xC))};
        return std::nullopt;
    };
    if (const auto r = record(1426)) t.kick = *r;
    if (const auto r = record(1432)) t.sprintKick = *r;
    if (const auto r = record(737)) t.fatality = *r;
    if (const auto r = record(1311)) t.swing = *r;
    if (const auto r = record(736)) t.vault = *r;
    // Targeting slot 5 (+0x474): {flags, sets, -> set {-> order, criteria, flag, -> criterion {u16 type, u16, f32}...}}.
    if (const auto slot = ptr(o->offset + 0x474))
        if (const auto set = ptr(*slot + 8))
            for (uint32_t k = 0, n = std::min(rd<uint32_t>(d, *set + 4), 8u); k < n; ++k)
                if (const auto c = ptr(*set + 0xC + 4 * k)) {
                    const int type = rd<uint16_t>(d, *c);
                    if (type == 1) t.range = rd<float>(d, *c + 4);
                    if (type == 5) t.coneDeg = rd<float>(d, *c + 4);
                }
    // Hit windows from the clips' events.
    const auto events = clipEvents(*o);
    for (auto& [name, w] : t.windows) {
        const auto it = events.find(name);
        if (it == events.end()) continue;
        for (const auto& c : it->second.commands) {
            if (c[0] == 0xE) w.open = c[2];
            if (c[0] == 0xD) w.close = c[2];
            if (c[0] == 0x6D) w.open = w.close = c[2];
        }
    }
    return t;
}

glm::vec3 throwVelocity(glm::vec3 origin, glm::vec3 aim, float speed, float cap) {
    const glm::vec2 d(aim - origin);
    const float dist = glm::length(d);
    if (dist < 1.0f || speed <= 0) return glm::vec3(0, 0, cap) * 30.0f;
    const float frames = dist / speed; // 0x6A6F40: the flight time that covers the distance at `speed`
    float vz = (aim.z - origin.z) / frames + 0.5f * kThrowGravity * frames;
    if (std::abs(vz) > cap) vz = speed; // 0x6A7510: over the cap the vertical speed is the horizontal one
    return glm::vec3(d / frames, vz) * 30.0f;
}

const PortableDef* World::portableDef(const std::string& object) {
    auto it = portableDefs_.find(object);
    if (it == portableDefs_.end()) {
        std::unique_ptr<PortableDef> def;
        const auto* bf = script_.bigfile();
        if (const auto o = bf ? objData(*bf, object) : std::nullopt; o) try {
                const std::span<const uint8_t> d = o->drm.sections[o->section].data;
                if (rd<uint32_t>(d, o->offset + 4) == 0xB00B) { // portable data
                    def = std::make_unique<PortableDef>();
                    def->encumbers = d[o->offset + 0x18] != 0;
                    def->nearDist = rd<float>(d, o->offset + 0x34), def->aimDist = rd<float>(d, o->offset + 0x38);
                    def->nearSpeed = rd<float>(d, o->offset + 0x3C), def->speed = rd<float>(d, o->offset + 0x40);
                    def->lockNearDist = rd<float>(d, o->offset + 0x44), def->lockAimDist = rd<float>(d, o->offset + 0x48);
                    def->lockNearSpeed = rd<float>(d, o->offset + 0x4C), def->lockSpeed = rd<float>(d, o->offset + 0x50);
                    if (const auto* p = objectPhysics(object)) { // its collision's box in its own frame
                        vec3 lo(1e30f), hi(-1e30f);
                        for (int seg = 0; seg < int(p->segments.size()); ++seg)
                            if (const auto shape = physics::solidShape(*p, seg))
                                for (const vec3& v : shape->vertices) {
                                    const vec3 m(p->segments[size_t(seg)] * glm::vec4(v, 1));
                                    lo = glm::min(lo, m), hi = glm::max(hi, m);
                                }
                        if (lo.x <= hi.x) {
                            const vec3 half = (hi - lo) * 0.5f;
                            def->thick = std::max(10.0f, std::min({half.x, half.y, half.z}));
                        }
                    }
                    for (const auto& [name, ce] : clipEvents(*o)) {
                        if (!name.starts_with("hosted_")) continue;
                        const std::string key = "hosted:" + std::to_string(ce.animId);
                        if (!anims_.clips.contains(key))
                            for (const auto& s : o->drm.sections)
                                if (s.type == cdc::SectionType::Animation && s.id == ce.animId) {
                                    anims_.clips.emplace(key, anim::decodeClip(s.data));
                                    break;
                                }
                        if (!anims_.clips.contains(key)) continue;
                        def->hosted.emplace(name, key);
                        auto& ev = def->events[name];
                        for (const auto& c : ce.commands) ev.emplace_back(c[0], c[2]);
                    }
                }
            } catch (const cdc::FormatError& e) {
                warnings.push_back(object + ": " + e.what());
                def.reset();
            }
        it = portableDefs_.emplace(object, std::move(def)).first;
    }
    return it->second.get();
}

// ---- Player: the action states ------------------------------------------------------------------
void Player::act(std::string_view clip, State st, bool mirror) {
    s_.state = st;
    s_.vel = vec3(0);
    s_.ledge = -1;
    s_.clip.clear(), s_.clipMix.clear(), s_.mix = 0; // restart even when the same clip is playing
    play(clip, st == State::Idle, 1, mirror);
}

void Player::launch(vec3 vel, std::string_view clip) {
    startFall(vel);
    if (anims_.find(clip)) play(clip, true);
}

void Player::step(vec3 delta, float dt) { groundMove(delta, dt); }

// Melee: the clip with its root motion, then Idle. Knocked: the clip, sliding on with what is left of the knock
// (PlayerState::vel, horizontal); World::tickMelee chains its clips and stands her up.
void Player::actTick(float dt) {
    const State st = s_.state;
    vec3 d = toWorld(advanceClip(dt));
    if (st == State::Knocked) {
        d += vec3(vec2(s_.vel) * dt, 0);
        s_.vel *= std::exp(-4.0f * dt); // [I] she skids to a stop
    }
    const vec3 keep = s_.vel;
    groundMove(d, dt);
    if (s_.state != st) return; // walked or was thrown off an edge: falling
    s_.vel = st == State::Knocked ? keep : vec3(0);
    if (st == State::Melee && clipDone()) {
        s_.state = State::Idle;
        play("ng_idle", true);
    }
}

// Carrying a weight (PortableDef::encumbers) on her feet: portable_encumbered_idle, _idle_to_walk, _walk (its root
// motion is the speed), _walk_to_idle (chart S1316..S1322). She turns at the walk's rate [I: Tuning::turnRate; the
// _turn45 / _turn90 clips are not used]. No run, sprint, roll or crouch; a jump lets go of it first (World::meleeInput).
bool Player::encumberedMove(vec2 move, bool jumpPressed, float dt) {
    static constexpr std::string_view kIdle = "portable_encumbered_idle", kStart = "portable_encumbered_idle_to_walk",
                                      kWalk = "portable_encumbered_walk", kStop = "portable_encumbered_walk_to_idle";
    const bool mine = s_.clip.starts_with("portable_encumbered");
    if (!s_.encumbered || jumpPressed || !anims_.find(kIdle) || !anims_.find(kWalk)) {
        if (mine) { // let go of it: back to the plain idle
            s_.state = State::Idle;
            play("ng_idle", true);
        }
        return false;
    }
    const float mag = std::min(1.0f, glm::length(move));
    if (mag > 0.1f) {
        const float turn = wrap(std::atan2(move.y, move.x) - s_.yaw), step = t_.turnRate * dt;
        s_.yaw = wrap(s_.yaw + std::clamp(turn, -step, step));
        s_.state = State::Walk;
        if (s_.clip == kWalk) {
        } else if (s_.clip == kStart && !clipDone()) {
        } else if (s_.clip == kStart || !anims_.find(kStart)) play(kWalk, true);
        else play(kStart, false);
    } else {
        s_.state = State::Idle;
        if (s_.clip == kIdle) {
        } else if (s_.clip == kStop && !clipDone()) {
        } else if ((s_.clip == kWalk || s_.clip == kStart) && anims_.find(kStop)) play(kStop, false);
        else play(kIdle, true);
    }
    groundMove(toWorld(advanceClip(dt)), dt);
    return true;
}

// ---- World: targets and blows -------------------------------------------------------------------
bool World::meleeReady() const {
    const PlayerState& s = player_.state();
    return onFeet(s.state) && s.water < 0 && !health_.dead() && combat_.slowMo <= 0 && !fieldCamera_ && !s.grappleUid;
}

Damageable* World::meleeTarget(bool fatality) {
    const PlayerState& s = player_.state();
    const MeleeTuning& mt = anims_.melee;
    const vec3 eye = s.pos + vec3(0, 0, targeting.eyeHeight), fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    Damageable* best = nullptr;
    float bestD = mt.range;
    for (Damageable* e : targets) {
        const auto* enemy = dynamic_cast<const ai::Enemy*>(e);
        if (!e->alive() || e->team() != Team::Enemy || (enemy && enemy->regenerating()) != fatality) continue;
        const float d = axisDistance(eye, *e);
        if (d > bestD) continue;
        const vec3 to = e->position() - s.pos;
        if (!fatality && glm::length(vec2(to)) > 1.0f &&
            glm::dot(glm::normalize(vec2(to)), vec2(fwd)) < std::cos(glm::radians(mt.coneDeg)) - 1e-4f)
            continue;
        const vec3 at = e->aimPoint();
        if (const float len = glm::length(at - eye); len > 1.0f)
            if (const auto hit = col_.raycast(eye, (at - eye) / len, len, collision::kSightMask); hit && hit->t < len - e->radius()) continue;
        best = e, bestD = d;
    }
    return best;
}

// MeleeState::target: an enemy's uid, or -(index + 1) into World::targets for any other target; 0 none.
int32_t World::meleeId(const Damageable* t) const {
    if (!t) return 0;
    if (const auto* e = dynamic_cast<const ai::Enemy*>(t)) return e->uid();
    const auto it = std::ranges::find(targets, t);
    return it == targets.end() ? 0 : -int32_t(it - targets.begin()) - 1;
}
Damageable* World::meleeFind(int32_t id) const {
    if (id < 0) return size_t(-id - 1) < targets.size() ? targets[size_t(-id - 1)] : nullptr;
    if (id > 0)
        for (const auto& e : enemies_.all())
            if (e->uid() == id) return e.get();
    return nullptr;
}

// A blow's sphere: every living enemy whose capsule it touches (`only`: that one whatever the sphere, the target she
// struck at within her reach) takes the attack record's hit, and every uber object whose collider it touches the hit
// message (World::damageObject: the urns break on any damage).
// A breakable in her reach: the nearest uber object with a section that takes damage whose collider is within
// kKickReach of her capsule, in front of her and at her height (0: none), and its top over her feet.
std::pair<int32_t, float> World::meleeObject() const {
    const PlayerState& s = player_.state();
    const vec2 fwd(std::cos(s.yaw), std::sin(s.yaw));
    int32_t best = 0;
    float bestD = player_.tuning().radius + kKickReach, top = 0;
    for (const auto& d : col_.dynamics) {
        if (!d.shape || d.shape->vertices.empty()) continue;
        const auto rt = uberRuntime_.find(d.id);
        const auto data = uberData_.find(d.id);
        if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second) continue;
        bool breakable = false;
        for (size_t i = 0; i < rt->second.sections.size() && i < data->second->sections.size(); ++i)
            breakable |= data->second->sections[i].takesDamage && !rt->second.sections[i].removed;
        if (!breakable) continue;
        vec3 lo(1e30f), hi(-1e30f);
        for (const vec3& v : d.shape->vertices) {
            const vec3 w(d.transform * glm::vec4(v, 1));
            lo = glm::min(lo, w), hi = glm::max(hi, w);
        }
        if (hi.z < s.pos.z || lo.z > s.pos.z + player_.tuning().height) continue;
        const vec2 to = vec2(glm::clamp(s.pos, lo, hi)) - vec2(s.pos), centre = vec2(0.5f * (lo + hi)) - vec2(s.pos);
        const float dist = glm::length(to);
        if (dist >= bestD || (glm::length(centre) > 1.0f && glm::dot(glm::normalize(centre), fwd) < 0.5f)) continue;
        best = d.id, bestD = dist, top = hi.z - s.pos.z;
    }
    return {best, top};
}

bool World::meleeHit(vec3 at, float radius, const AttackRecord& rec, Damageable* only, int32_t object) {
    const PlayerState& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    bool any = false;
    for (Damageable* e : std::vector<Damageable*>(targets)) { // onHit may unregister a target
        if (!e->alive() || e->team() != Team::Enemy) continue;
        if (e != only && axisDistance(at, *e) > radius + e->radius()) continue;
        const vec3 base = e->position();
        Hit h{rec.damage, vec3(base.x, base.y, std::clamp(at.z, base.z, base.z + e->height())), fwd, Team::Player};
        h.damageClass = rec.hitClass, h.melee = rec.hitClass != kFinisherClass;
        const float before = e->health();
        e->onHit(h);
        gainAdrenaline(before - std::max(0.0f, e->health()));
        any = true;
    }
    std::set<int32_t> struck;
    if (object) struck.insert(object);
    for (const auto& d : col_.dynamics) {
        if (!d.shape || !uberRuntime_.contains(d.id) || struck.contains(d.id) || d.shape->vertices.empty()) continue;
        vec3 lo(1e30f), hi(-1e30f);
        for (const vec3& v : d.shape->vertices) {
            const vec3 w(d.transform * glm::vec4(v, 1));
            lo = glm::min(lo, w), hi = glm::max(hi, w);
        }
        if (glm::length(at - glm::clamp(at, lo, hi)) <= radius) struck.insert(d.id);
    }
    for (const int32_t uid : struck) {
        script_.graphHit(uid, uint32_t(rec.hitClass), rec.damage);
        damageObject(uid, rec.damage, rec.hitClass);
        any = true;
    }
    return any;
}

// 0x4D5560 per section in a state and idle: its first transition of trigger 6 / 7 / 8 whose float the length of the
// message's vector A / B / C exceeds. A is an impulse applied at a point (0x4ECFA0: a blow, a shot): `applied`. [I] B /
// C read as a body contact's speed, units/s: `contact` (the urns: 6 over 0 = any blow, 7 over 379.27 = 1.2 m/s).
void World::impulseObject(int32_t uid, float applied, float contact) {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second) return;
    for (size_t i = 0; i < rt->second.sections.size(); ++i) {
        const auto& sec = data->second->sections[i];
        auto& st = rt->second.sections[i];
        if (st.removed || st.transition >= 0 || st.state < 0 || size_t(st.state) >= sec.stateOut.size()) continue;
        for (const uint32_t ti : sec.stateOut[size_t(st.state)]) {
            const auto& t = sec.transitions[ti];
            const float limit = std::bit_cast<float>(t.param);
            if ((t.trigger == 6 && applied > limit) || ((t.trigger == 7 || t.trigger == 8) && contact > limit)) {
                uberBegin(uid, i, ti);
                break;
            }
        }
    }
}

// ---- World: carried portables -------------------------------------------------------------------
// Where the carried portable's origin is: between her hands (bones 55 / 80, the pose of the last tick) while a clip
// that holds it there plays (the weights' walk, the hosted clips); else 60 ahead and 80 up of her root [I: poles,
// keys; their carry pose hosted_idle is not layered].
vec3 World::handsPoint() const {
    const PlayerState& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    const bool inHands = s.clip.starts_with("portable_encumbered") || s.clip.starts_with("hosted:");
    if (inHands && bones_.size() > 80 && anims_.skeleton.bones.size() > 80) {
        const glm::mat4 m = player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll);
        return 0.5f * (vec3(m * bones_[55][3]) + vec3(m * bones_[80][3]));
    }
    return s.pos + fwd * 60.0f + vec3(0, 0, 80);
}

// The carried portable collides with the level [I: tru.exe keeps it on her hand markers; what keeps it out of walls
// there is not traced]: a sphere of its thickness (the smallest half extent of its collision box, at most her own
// radius so it starts inside her capsule) swept from her chest to her hands stops at the first wall, door or other
// object, so it is never posed through one.
vec3 World::heldPoint() const {
    const PlayerState& s = player_.state();
    const vec3 at = handsPoint();
    const auto* held = held_ ? script_.find(held_) : nullptr;
    const PortableDef* def = held ? const_cast<World*>(this)->portableDef(held->name) : nullptr;
    const float r = std::min(def ? def->thick : PortableDef{}.thick, player_.tuning().radius - 5.0f);
    const vec3 chest = s.pos + vec3(0, 0, std::max(at.z - s.pos.z, 0.5f * player_.tuning().height));
    if (glm::length(at - chest) > 1.0f)
        if (const auto hit = col_.sweepSphere(chest, r, at - chest, collision::kSightMask); hit && (hit->dynamic < 0 || col_.dynamics[size_t(hit->dynamic)].id != held_))
            return chest + (at - chest) * std::clamp(hit->t, 0.0f, 1.0f);
    return at;
}

// Carrying on her feet what she holds out in both hands (an encumbering portable: the carved weights): a wall of the
// level its thickness reaches at her hands holds her off it (she cannot walk a weight into a wall) [I]. Gems, keys and
// poles are held 60 ahead of her root, inside her own radius: the sweep is enough for them. Not where she engages a
// receptacle's record (l5_valgrind's cog wheels take the power gem from right by their housing). Objects do not (receptacles are approached with the portable in her hands: the sweep
// above keeps it out of them). Not during the pick-up, put-down, throw and swing clips, whose hands go to the floor.
void World::carryCollide(float dt) {
    const PlayerState& s = player_.state();
    const auto* held = held_ ? script_.find(held_) : nullptr;
    if (!held || s.water >= 0 || !(s.state == State::Idle || s.state == State::Walk || s.state == State::Run)) return;
    const PortableDef* def = portableDef(held->name);
    if (!def || !def->encumbers) return; // at her side, within her capsule's reach: heldPoint's sweep is enough
    const float r = def->thick;
    const vec3 at = handsPoint();
    const auto hit = col_.closestPoint(at, r, collision::kSightMask);
    if (!hit) return;
    if (const Engaged e = engaged(); e.record && e.record->kind == 5) return; // at a receptacle: she sets it in from there
    if ( std::abs(hit->normal.z) > 0.7f || hit->dynamic >= 0) return; // the level's walls; objects only stop the sweep
    const vec2 n(hit->normal);
    if (const float len = glm::length(n); len > 1e-3f && hit->t < r) player_.step(vec3(n / len * (r - hit->t), 0), dt);
}

// Picking `o` up: with its hosted_pickup clip she bends for it and it comes into her hands on the clip's event 0x4E
// (message 0x17B, 0x716F30); swimming, or without the clip, at once.
void World::pickUp(script::Object& o) {
    const PlayerState& s = player_.state();
    const PortableDef* def = portableDef(o.name);
    const std::string* clip = def && meleeReady() ? def->clip("hosted_pickup") : nullptr;
    if (!clip) {
        held_ = o.uid, o.loose = false;
        return;
    }
    const vec3 to = o.pos + o.offset - s.pos;
    if (glm::length(vec2(to)) > 1.0f) player_.face(std::atan2(to.y, to.x));
    player_.act(*clip, State::Melee);
    melee_.move = MeleeState::Move::PickUp, melee_.target = o.uid, melee_.landed = false;
}

// The carried portable leaves her hands as a loose body (stepPhysics makes it, moving at `vel`): from where she holds
// it, moved out along her heading until its collision clears her proxy capsule (a body starting inside it is shoved
// aside). `thrown`: it is a weapon for kThrownSeconds.
void World::releaseHeld(vec3 vel, bool thrown) {
    auto* held = held_ ? const_cast<script::Object*>(script_.find(held_)) : nullptr; // World owns object motion
    if (!held) {
        held_ = 0;
        return;
    }
    const PlayerState& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    float reach = 0;
    if (const auto* p = objectPhysics(held->name))
        for (int seg = 0; seg < int(p->segments.size()); ++seg)
            if (const auto shape = physics::solidShape(*p, seg))
                for (const vec3& v : shape->vertices) reach = std::max(reach, glm::length(vec2(p->segments[seg] * glm::vec4(v, 1))));
    vec3 at = heldPoint();
    const float ahead = glm::dot(vec2(at - s.pos), vec2(fwd)), clear = player_.tuning().radius + reach + 10.0f;
    if (s.water < 0 && ahead < clear) at += fwd * (clear - ahead);
    held->offset = at - held->pos;
    held->loose = true;
    melee_.launch[held->uid] = vel;
    melee_.lastPos[held->uid] = at;
    if (thrown) melee_.thrown[held->uid] = kThrownSeconds;
    held_ = 0;
}

// Loose portables she let go of: their speed from tick to tick. A thrown one is a weapon while its timer runs: the
// first living enemy its centre comes within their radii of takes the swung-portable attack record (100, class 2) [I:
// the thrown body's own hit numbers are not decoded]. Any of them hitting an uber object passes its impulse on
// (impulseObject: the urns break under a falling or thrown weight).
void World::tickThrown(float dt) {
    for (auto it = melee_.lastPos.begin(); it != melee_.lastPos.end();) {
        const int32_t uid = it->first;
        const auto* o = script_.find(uid);
        const auto pose = o && o->loose ? physics_.loose(uid) : std::nullopt;
        if (!pose) {
            if (!o || !o->loose) melee_.thrown.erase(uid), it = melee_.lastPos.erase(it);
            else ++it; // released this tick: its body starts with the next physics step
            continue;
        }
        const vec3 at((*pose)[3]), vel = (at - it->second) / dt;
        it->second = at;
        const float speed = glm::length(vel);
        float reach = 40.0f;
        if (const auto* p = objectPhysics(o->name)) {
            for (int seg = 0; seg < int(p->segments.size()); ++seg)
                if (const auto shape = physics::solidShape(*p, seg))
                    for (const vec3& v : shape->vertices) reach = std::max(reach, glm::length(vec3(p->segments[seg] * glm::vec4(v, 1))));
        }
        const auto th = melee_.thrown.find(uid);
        if (th != melee_.thrown.end()) {
            if (speed > 150.0f) // [I] moving: a body at rest on the floor hurts nobody
                for (Damageable* e : std::vector<Damageable*>(targets)) {
                    if (!e->alive() || e->team() != Team::Enemy || axisDistance(at, *e) > reach + e->radius()) continue;
                    const vec3 base = e->position();
                    Hit h{anims_.melee.swing.damage, vec3(base.x, base.y, std::clamp(at.z, base.z, base.z + e->height())), vel / speed, Team::Player};
                    h.damageClass = anims_.melee.swing.hitClass, h.melee = true;
                    const float before = e->health();
                    e->onHit(h);
                    gainAdrenaline(before - std::max(0.0f, e->health()));
                    th->second = 0; // one blow
                    break;
                }
            if ((th->second -= dt) <= 0) melee_.thrown.erase(th);
        }
        if (speed > 30.0f) {
            std::set<int32_t> struck;
            for (const auto& d : col_.dynamics) {
                if (!d.shape || d.id == uid || !uberRuntime_.contains(d.id) || struck.contains(d.id) || d.shape->vertices.empty()) continue;
                vec3 lo(1e30f), hi(-1e30f);
                for (const vec3& v : d.shape->vertices) {
                    const vec3 w(d.transform * glm::vec4(v, 1));
                    lo = glm::min(lo, w), hi = glm::max(hi, w);
                }
                if (glm::length(at - glm::clamp(at, lo, hi)) <= reach) struck.insert(d.id);
            }
            for (const int32_t hit : struck) impulseObject(hit, 0, speed);
        }
        // At rest for good: no longer tracked.
        if (speed < 1.0f && th == melee_.thrown.end() && !melee_.thrown.contains(uid)) it = melee_.lastPos.erase(it);
        else ++it;
    }
}

void World::throwLara(vec3 vel) {
    const PlayerState& s = player_.state();
    if (health_.dead() || s.water >= 0 || !(onFeet(s.state) || s.state == State::Hit || s.state == State::Melee || s.state == State::Crouch)) return;
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    // The blast is in front of her: thrown back, onto her back (hitReact_thrownBack / _landBack, whose root moves 60
    // backwards [O], then Hit_getupback); behind her: thrown forward onto her front (Hit_ThrownFront / Hit_LandFront, root
    // 215 forwards [O], then Hit_GetupFront).
    const bool front = glm::dot(vec2(vel), vec2(fwd)) <= 0;
    const std::string_view clip = front ? "hitReact_thrownBack" : "Hit_ThrownFront";
    if (!anims_.find(clip)) return;
    if (held_) releaseHeld(vec3(0), false);
    melee_.move = MeleeState::Move::Knocked, melee_.phase = 0, melee_.target = front ? 1 : 0, melee_.landed = false;
    melee_.vel = vec3(vec2(vel), 0);
    player_.act(clip, State::Knocked);
    auto st = player_.state();
    st.vel = melee_.vel;
    player_.restore(st);
}

// ---- World: input and moves ---------------------------------------------------------------------
// Before the player ticks: the input lock of a move, the keys of a carried portable (put down, throw, let go) and the
// combat moves that take the jump / crouch keys (enemy vault, evasive rolls and leaps).
void World::meleeInput(Actions& a) {
    MeleeState& m = melee_;
    const PlayerState& s = player_.state();
    if (m.swallowInteract) {
        if (a.interact) a.interact = false;
        else m.swallowInteract = false;
    }
    const bool dropPress = a.drop && !m.dropHeld, grenadePress = a.grenade && !m.grenadeHeld;
    const bool jumpPress = a.jump && !m.jumpHeld, firePress = (a.fire || a.aim) && !m.fireHeld;
    m.dropHeld = a.drop, m.grenadeHeld = a.grenade, m.jumpHeld = a.jump, m.fireHeld = a.fire || a.aim;
    if (s.state == State::Melee || s.state == State::Knocked) {
        // Knocked down and lying: jump gets her up at once (the controls page's "tap Jump to recover").
        if (s.state == State::Knocked && m.phase == 1 && jumpPress) m.landed = true;
        const vec2 look = a.look;
        a = Actions{}, a.look = look;
        return;
    }
    auto* held = held_ ? const_cast<script::Object*>(script_.find(held_)) : nullptr;
    const PortableDef* def = held ? portableDef(held->name) : nullptr;
    player_.setEncumbered(def && def->encumbers);
    if (held) {
        const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
        const bool ground = meleeReady(), swim = s.water >= 0;
        if (grenadePress || a.grenade) a.grenade = false; // the key throws what she holds, not a grenade
        if (dropPress && (ground || swim)) {
            a.drop = false;
            const std::string* clip = def && ground ? def->clip("hosted_putdown") : nullptr;
            if (clip) {
                m.move = MeleeState::Move::PutDown, m.target = held_, m.landed = false;
                player_.act(*clip, State::Melee);
            } else {
                releaseHeld(vec3(0), false);
            }
        } else if (grenadePress && ground) {
            const std::string* clip = def ? def->clip("hosted_throw") : nullptr;
            m.move = MeleeState::Move::Throw, m.target = held_, m.landed = false;
            if (clip) player_.act(*clip, State::Melee);
            // without the clip (no data) the throw happens at once in tickMelee
        } else if ((jumpPress || firePress) && ground) {
            // [O] message 0x156 with parameter 0: it drops with her velocity plus a shove along her heading
            releaseHeld((s.pos - prev_.pos) / float(kTickSeconds) + fwd * kDropShove, false);
            player_.setEncumbered(false);
        } else if (ground && a.drop) {
            a.drop = false; // carrying: no crouch
        }
        if (def && def->encumbers && ground) a.sprint = false;
        return;
    }
    // Combat moves under a target lock.
    if (!meleeReady() || !combat_.locked || !combat_.target || !combat_.target->alive()) return;
    const vec2 fwd(std::cos(s.yaw), std::sin(s.yaw)), left(-fwd.y, fwd.x);
    const vec2 cf(std::cos(cam_.yaw), std::sin(cam_.yaw)), cr(std::sin(cam_.yaw), -std::cos(cam_.yaw));
    const vec2 stick = cr * a.move.x + cf * a.move.y; // world
    const float along = glm::dot(stick, fwd), side = glm::dot(stick, left);
    const bool moving = glm::length(stick) > 0.3f;
    const int quadrant = !moving ? -1 : std::abs(along) >= std::abs(side) ? (along > 0 ? 0 : 1) : (side > 0 ? 2 : 3); // front, back, left, right
    if (jumpPress && quadrant == 0 && s.state == State::Run) { // the enemy vault: running at the melee target
        auto* e = dynamic_cast<ai::Enemy*>(meleeTarget());
        if (e && e == combat_.target && e->type().laraVault) {
            const std::string key = "vault:" + e->type().name;
            if (!anims_.clips.contains(key)) anims_.clips.emplace(key, *e->type().laraVault);
            if (e->vaulted()) {
                const vec3 to = e->position() - s.pos;
                player_.face(std::atan2(to.y, to.x));
                player_.act(key, State::Melee);
                m.move = MeleeState::Move::Vault, m.target = e->uid(), m.landed = true;
                a.jump = false;
                return;
            }
        }
    }
    if (dropPress && moving) {
        static constexpr std::string_view kRoll[] = {"combat_rollfront", "combat_rollback", "combat_rollleft", "combat_rollright"};
        if (anims_.find(kRoll[quadrant])) {
            player_.act(kRoll[quadrant], State::Melee);
            m.move = MeleeState::Move::Evade, m.phase = 0, m.vel = vec3(0), m.landed = true;
            a.drop = false;
        }
    } else if (jumpPress && quadrant >= 1) {
        static constexpr std::string_view kTake[] = {"", "combat_jumpb_takeoff", "combat_jumpl_takeoff", "combat_jumpr_takeoff"};
        if (anims_.find(kTake[quadrant])) {
            const vec2 dir = quadrant == 1 ? -fwd : quadrant == 2 ? left : -left;
            player_.act(kTake[quadrant], State::Melee);
            m.move = MeleeState::Move::Evade, m.phase = quadrant, m.vel = vec3(dir * kEvadeSpeed, kEvadeVz), m.landed = false;
            a.jump = false;
        }
    }
}

// After the tick's interactions. `pressed`: an interact press that no pickup, portable, switch or grip took.
void World::tickMelee(Actions& a, bool pressed, float dt) {
    MeleeState& m = melee_;
    const MeleeTuning& mt = anims_.melee;
    tickThrown(dt);
    const PlayerState& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    const float frame = float(s.clipTime) * 30.0f;
    // The move in progress.
    if (m.move != MeleeState::Move::None && s.state != State::Melee && s.state != State::Knocked) {
        // Thrown without its clip (no data): at once. Anything else ended (the clip ran out, she fell off an edge).
        if (m.move == MeleeState::Move::Throw && !m.landed && held_ && meleeReady()) {
            const PortableDef none;
            const vec3 origin = s.pos + vec3(0, 0, 0.7f * player_.tuning().height);
            releaseHeld(throwVelocity(origin, origin + fwd * none.aimDist, none.speed, none.speed), true);
        }
        if (m.move == MeleeState::Move::Evade && !m.landed && m.vel != vec3(0) && (s.state == State::Idle || s.state == State::Fall)) {
            // the take-off clip ended: the leap
            static constexpr std::string_view kAir[] = {"", "combat_jumpb_inair", "combat_jumpl_inair", "combat_jumpr_inair"};
            player_.launch(m.vel, kAir[std::clamp(m.phase, 0, 3)]);
        }
        m.move = MeleeState::Move::None, m.target = 0, m.object = 0, m.landed = false, m.sprint = false, m.phase = 0, m.vel = vec3(0);
    }
    switch (m.move) {
    case MeleeState::Move::Kick:
    case MeleeState::Move::Swing: {
        Damageable* e = meleeFind(m.target);
        if (e && !e->alive()) e = nullptr;
        const bool swing = m.move == MeleeState::Move::Swing;
        MeleeTuning::Window w = mt.window(s.clip);
        const auto* heldObj = swing && held_ ? script_.find(held_) : nullptr;
        if (swing) {
            const PortableDef* def = heldObj ? portableDef(heldObj->name) : nullptr;
            for (const char* name : {"hosted_melee", "hosted_melee_front"})
                if (const std::string* c = def ? def->clip(name) : nullptr; c && *c == s.clip)
                    w = {def->frame(name, 0xE).value_or(0), def->frame(name, 0xD).value_or(0)};
        }
        if (e && frame < float(w.open)) { // she closes on the target and keeps facing it before the blow
            const vec3 to = e->position() - s.pos;
            const float flat = glm::length(vec2(to)), gap = flat - e->radius() - player_.tuning().radius;
            if (flat > 1.0f) player_.face(std::atan2(to.y, to.x));
            if (gap > kContactGap && flat > 1.0f) player_.step(vec3(vec2(to) / flat, 0) * std::min(gap - kContactGap, kApproachSpeed * dt), dt);
        }
        if (!m.landed && frame >= float(w.open) && frame <= float(w.close) + 1.0f) {
            vec3 at = s.pos + fwd * 150.0f + vec3(0, 0, 250.0f); // without a skeleton
            float radius = kFootRadius;
            if (swing) {
                at = heldPoint(), radius = 150.0f; // [I] the swung portable
            } else if (bones_.size() == anims_.bind.size() && bones_.size() > 80) {
                // her leading foot: of the two lowest bones of the bind pose (one a side), the one farther forward now
                int foot[2] = {-1, -1};
                for (size_t i = 1; i < anims_.bind.size(); ++i) {
                    const int side = anims_.bind[i].x < 0 ? 0 : 1;
                    if (foot[side] < 0 || anims_.bind[i].z < anims_.bind[size_t(foot[side])].z) foot[side] = int(i);
                }
                if (foot[0] >= 0 && foot[1] >= 0) {
                    const glm::mat4 model = player_.modelMatrix(s.pos, s.yaw, s.pitch, s.roll);
                    const vec3 p0(model * bones_[size_t(foot[0])][3]), p1(model * bones_[size_t(foot[1])][3]);
                    at = glm::dot(p0 - s.pos, fwd) > glm::dot(p1 - s.pos, fwd) ? p0 : p1;
                }
            }
            Damageable* reached = nullptr;
            if (e) {
                const vec3 to = e->position() - s.pos;
                const float gap = glm::length(vec2(to)) - e->radius() - player_.tuning().radius;
                if (gap <= kKickReach && glm::dot(glm::normalize(vec2(to) + vec2(1e-6f)), vec2(fwd)) > 0.5f) reached = e;
            }
            // the breakable she kicks at, still engaged / in reach
            const int32_t object = m.object && (meleeObject().first == m.object || engaged().uid == m.object) ? m.object : 0;
            if (meleeHit(at, radius, swing ? mt.swing : m.sprint ? mt.sprintKick : mt.kick, reached, object)) m.landed = true; // one blow per move
        }
        break;
    }
    case MeleeState::Move::Fatality: {
        auto* e = dynamic_cast<ai::Enemy*>(meleeFind(m.target));
        const MeleeTuning::Window w = mt.window("melee_thrall_fatality");
        if (e && e->alive() && frame < float(w.open)) {
            const vec3 to = e->position() - s.pos;
            const float flat = glm::length(vec2(to)), gap = flat - e->radius() - player_.tuning().radius;
            if (flat > 1.0f) player_.face(std::atan2(to.y, to.x));
            if (gap > kContactGap && flat > 1.0f) player_.step(vec3(vec2(to) / flat, 0) * std::min(gap - kContactGap, kApproachSpeed * dt), dt);
        }
        if (!m.landed && frame >= float(w.open)) {
            m.landed = true;
            if (e && e->alive() && e->regenerating()) { // 0x6E6840: the hit goes to the target, no contact test
                Hit h{mt.fatality.damage, e->aimPoint(), fwd, Team::Player};
                h.damageClass = mt.fatality.hitClass;
                e->onHit(h);
            }
        }
        break;
    }
    case MeleeState::Move::PickUp: {
        auto* o = const_cast<script::Object*>(script_.find(m.target));
        const PortableDef* def = o ? portableDef(o->name) : nullptr;
        const int at = def ? def->frame("hosted_pickup", 0x4E).value_or(0) : 0;
        if (!m.landed && frame >= float(at)) {
            m.landed = true;
            if (o && !held_ && !inserted_.contains(o->uid)) held_ = o->uid, o->loose = false;
        }
        break;
    }
    case MeleeState::Move::PutDown: {
        const auto* o = held_ ? script_.find(held_) : nullptr;
        const PortableDef* def = o ? portableDef(o->name) : nullptr;
        const int at = def ? def->frame("hosted_putdown", 0x4F).value_or(0) : 0;
        if (!m.landed && frame >= float(at)) {
            m.landed = true;
            releaseHeld(vec3(0), false); // [O] 0x156 with parameter 1: her velocity only (she stands)
        }
        break;
    }
    case MeleeState::Move::Throw: {
        const auto* o = held_ ? script_.find(held_) : nullptr;
        const PortableDef* def = o ? portableDef(o->name) : nullptr;
        const int at = def ? def->frame("hosted_throw", 0x53).value_or(0) : 0;
        if (!m.landed && o && frame >= float(at)) {
            m.landed = true;
            // 0x6B53A0: from 0.7 of her height over her feet (0x6EFC40) at the point `aimDist` along her heading (a lock:
            // the target), stopped by what is in the way (0x6F2530); the near speed when that point is closer than
            // `nearDist`.
            const PortableDef none;
            const PortableDef& d = def ? *def : none;
            const bool lock = combat_.locked && combat_.target && combat_.target->alive();
            const vec3 origin = s.pos + vec3(0, 0, 0.7f * player_.tuning().height);
            vec3 aim = lock ? combat_.target->aimPoint() : origin + fwd * d.aimDist;
            if (const float len = glm::length(aim - origin); len > 1.0f)
                if (const auto hit = col_.raycast(origin, (aim - origin) / len, len, collision::kSightMask); hit && (hit->dynamic < 0 || col_.dynamics[size_t(hit->dynamic)].id != held_))
                    aim = hit->point;
            const float nearDist = lock ? d.lockNearDist : d.nearDist, far = lock ? d.lockSpeed : d.speed;
            const float speed = glm::length(aim - origin) < nearDist ? (lock ? d.lockNearSpeed : d.nearSpeed) : far;
            releaseHeld(throwVelocity(origin, aim, speed, far), true);
        }
        break;
    }
    case MeleeState::Move::Knocked: {
        // Thrown (phase 0), the landing clip (1: she lies at its end until jump or kLieSeconds), the get-up (2).
        const bool front = m.target != 0;
        if (!ended(anims_, s)) break;
        if (m.phase == 0) {
            m.phase = 1, m.landed = false, m.vel = vec3(0), m.timer = 0;
            player_.act(front ? "hitReact_landBack" : "Hit_LandFront", State::Knocked);
        } else if (m.phase == 1) {
            if (m.landed || (m.timer += dt) >= kLieSeconds) {
                m.phase = 2;
                // on her back she gets up with Hit_getupback, on her front with Hit_GetupFront [I: by the clip names]
                player_.act(front ? "Hit_getupback" : "Hit_GetupFront", State::Knocked);
            }
        } else {
            m.move = MeleeState::Move::None, m.phase = 0, m.target = 0;
            player_.act("ng_idle", State::Idle);
        }
        break;
    }
    default: break;
    }
    if (m.move != MeleeState::Move::None || !pressed || !meleeReady()) return;

    // A new move from the interact press.
    const auto start = [&](std::string_view clip, MeleeState::Move move, Damageable* e) {
        if (!anims_.find(clip)) return false;
        if (e) {
            const vec3 to = e->position() - s.pos;
            if (glm::length(vec2(to)) > 1.0f) player_.face(std::atan2(to.y, to.x));
        }
        player_.act(clip, State::Melee);
        m.move = move, m.target = meleeId(e), m.object = 0, m.landed = false, m.sprint = false;
        m.swallowInteract = true, a.interact = false;
        return true;
    };
    if (Damageable* down = meleeTarget(true); down && !held_) { // T1347: the finishing move on a regenerating thrall
        start("melee_thrall_fatality", MeleeState::Move::Fatality, down);
        return;
    }
    Damageable* e = meleeTarget();
    if (held_) { // S1311: swing what she carries (at the target, or at the air when nothing takes the press)
        const auto* o = script_.find(held_);
        const PortableDef* def = o ? portableDef(o->name) : nullptr;
        if ((e || !engaged().record) && def)
            for (const char* name : {"hosted_melee", "hosted_melee_front"})
                if (const std::string* c = def->clip(name); c && start(*c, MeleeState::Move::Swing, e)) return;
        return;
    }
    if (s.clip == "sprint_loop") { // T177 (S59 -> S1432): the sprint kick, its own attack record
        if (start("melee_kick_move", MeleeState::Move::Kick, e)) m.sprint = true;
        return;
    }
    if (e) { // T36 / T2367..T2370: by the target's top over her feet
        const auto* enemy = dynamic_cast<const ai::Enemy*>(e);
        const float top = e->position().z - s.pos.z + (enemy && enemy->type().tuning.top > 0 ? enemy->type().tuning.top : e->height());
        start(top < mt.low ? "melee_kick_low" : top < mt.mid ? "melee_kick_mid" : top < mt.high ? "melee_kick_high" : "melee_kick_highest",
              MeleeState::Move::Kick, e);
        return;
    }
    // 0x591: the kind of the engagement record she is in. 3 (the urns' and other breakables' record): chart S1301, whose
    // action 0x72C790 posts 0x158 to the object and starts the kick (0x71EF60): she kicks at it. Any other record: an
    // interaction, no kick at the air.
    const Engaged en = engaged();
    if (en.record && en.record->kind != 3) return;
    // The breakable she kicks at: the engaged one, else one in reach [I: tru.exe needs the record]. She turns to it and
    // kicks by its top [I: the class rule of the melee targets].
    auto [object, top] = meleeObject();
    if (en.record && uberRuntime_.contains(en.uid) && object != en.uid) object = en.uid, top = mt.mid - 1.0f;
    if (object) {
        if (const auto* o = script_.find(object)) {
            const vec3 to = o->pos + o->offset - s.pos;
            if (glm::length(vec2(to)) > 1.0f) player_.face(std::atan2(to.y, to.x));
        }
        if (start(top < mt.low ? "melee_kick_low" : top < mt.mid ? "melee_kick_mid" : top < mt.high ? "melee_kick_high" : "melee_kick_highest",
                  MeleeState::Move::Kick, nullptr))
            m.object = object;
        return;
    }
    start(s.state == State::Run ? "melee_kick_move" : "melee_kick_mid", MeleeState::Move::Kick, nullptr); // T38 / T177
}

// ---- saves --------------------------------------------------------------------------------------
nlohmann::json World::serializeMelee() const {
    const MeleeState& m = melee_;
    nlohmann::json thrown = nlohmann::json::array(), launch = nlohmann::json::array(), last = nlohmann::json::array();
    for (const auto& [uid, t] : m.thrown) thrown.push_back({uid, t});
    for (const auto& [uid, v] : m.launch) launch.push_back({uid, vecJson(v)});
    for (const auto& [uid, p] : m.lastPos) last.push_back({uid, vecJson(p)});
    return {{"move", int(m.move)}, {"target", m.target}, {"object", m.object}, {"landed", m.landed}, {"sprint", m.sprint}, {"phase", m.phase},
            {"vel", vecJson(m.vel)}, {"timer", m.timer}, {"swallow", m.swallowInteract}, {"held", {m.dropHeld, m.grenadeHeld, m.jumpHeld, m.fireHeld}},
            {"thrown", std::move(thrown)}, {"launch", std::move(launch)}, {"lastPos", std::move(last)}};
}

void World::restoreMelee(const nlohmann::json& j) {
    melee_ = {};
    if (!j.is_object()) return; // saves from before: no move in progress
    MeleeState& m = melee_;
    const int move = j.value("move", 0);
    if (move < 0 || move > int(MeleeState::Move::Knocked)) throw std::runtime_error("melee move " + std::to_string(move));
    m.move = MeleeState::Move(move), m.target = j.value("target", 0), m.object = j.value("object", 0), m.landed = j.value("landed", false);
    m.sprint = j.value("sprint", false), m.phase = j.value("phase", 0), m.swallowInteract = j.value("swallow", false);
    m.timer = j.value("timer", 0.0f);
    if (const auto v = j.find("vel"); v != j.end()) m.vel = vecJson(*v);
    if (const auto h = j.find("held"); h != j.end() && h->size() == 4)
        m.dropHeld = h->at(0), m.grenadeHeld = h->at(1), m.jumpHeld = h->at(2), m.fireHeld = h->at(3);
    for (const auto& e : j.value("thrown", nlohmann::json::array())) m.thrown[e.at(0).get<int32_t>()] = e.at(1).get<float>();
    for (const auto& e : j.value("launch", nlohmann::json::array())) m.launch[e.at(0).get<int32_t>()] = vecJson(e.at(1));
    for (const auto& e : j.value("lastPos", nlohmann::json::array())) m.lastPos[e.at(0).get<int32_t>()] = vecJson(e.at(1));
}

} // namespace ou::game
