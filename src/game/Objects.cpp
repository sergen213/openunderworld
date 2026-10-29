// World <-> src/physics: collision of scripted objects (doors, push blocks) and rigid-body hinge doors.
// Evidence and inferences: docs/formats/PHYSICS.md.
#include "game/Game.hpp"
#include "script/Vm.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>

namespace ou::game {
namespace {

// [I] Opening angle of a rigid-body hinge door. tru.exe couples it to the lever's joint every tick (l4_ballcourt lever
// 6502 fn 2/3: door limit = lever angle + 2 degrees); we do not run that loop and swing the door fully open instead.
constexpr float kDoorOpen = 80.0f * 3.14159265f / 180.0f;

glm::mat4 introModel(const script::Object& o) { // T * Rx * Ry * Rz, as the intros (formats::introMatrix)
    return glm::translate(glm::mat4(1), o.pos) * glm::rotate(glm::mat4(1), o.rot.x, {1, 0, 0}) *
           glm::rotate(glm::mat4(1), o.rot.y, {0, 1, 0}) * glm::rotate(glm::mat4(1), o.rot.z, {0, 0, 1});
}

struct UberMotionSample {
    vec3 delta{0}, rotation{0};
    float duration = 1;
    std::string clip; // animation-list name
};

std::optional<UberMotionSample> uberMotion(const cdc::Drm& drm, std::string_view name) {
    const auto list = anim::animList(drm);
    const anim::AnimEntry* entry = nullptr;
    for (const auto& e : list) {
        const bool travel = name.find("elevator") != std::string_view::npos ? e.name == "falling"
            : e.name.find("opening") != std::string::npos || e.name.find("raising") != std::string::npos;
        if (travel) {
            entry = &e;
            break;
        }
    }
    if (!entry) return std::nullopt;
    for (const auto& s : drm.sections)
        if (s.type == cdc::SectionType::Animation && s.id == entry->animId) {
            const auto clip = anim::decodeClip(s.data);
            UberMotionSample motion;
            motion.clip = entry->name;
            motion.duration = float(clip.durationSeconds());
            motion.delta = rootTranslation(clip, clip.durationSeconds(), false) - rootTranslation(clip, 0, false);
            for (const auto& track : clip.tracks) {
                const vec3 first(track[anim::TransX].empty() ? 0 : track[anim::TransX].front(),
                                 track[anim::TransY].empty() ? 0 : track[anim::TransY].front(),
                                 track[anim::TransZ].empty() ? 0 : track[anim::TransZ].front());
                const vec3 last(track[anim::TransX].empty() ? 0 : track[anim::TransX].back(),
                                track[anim::TransY].empty() ? 0 : track[anim::TransY].back(),
                                track[anim::TransZ].empty() ? 0 : track[anim::TransZ].back());
                if (glm::length(last - first) > glm::length(motion.delta)) motion.delta = last - first;
                const vec3 firstRot(track[anim::RotX].empty() ? 0 : track[anim::RotX].front(),
                                    track[anim::RotY].empty() ? 0 : track[anim::RotY].front(),
                                    track[anim::RotZ].empty() ? 0 : track[anim::RotZ].front());
                const vec3 lastRot(track[anim::RotX].empty() ? 0 : track[anim::RotX].back(),
                                   track[anim::RotY].empty() ? 0 : track[anim::RotY].back(),
                                   track[anim::RotZ].empty() ? 0 : track[anim::RotZ].back());
                if (glm::length(lastRot - firstRot) > glm::length(motion.rotation)) motion.rotation = lastRot - firstRot;
            }
            if ((glm::length(motion.delta) > 1e-3f || glm::length(motion.rotation) > 1e-4f) && motion.duration > 0) return motion;
        }
    return std::nullopt;
}

// Carryable objects: native base `portablebase` (l1_kraken_entrance weights ms_pt_weight_c, base 3937 vt 20).
bool portable(const script::Object& o) {
    if (!o.script) return false;
    for (const auto* t = o.script->program->type(o.script->typeId); t; t = o.script->program->type(t->superId))
        if (!t->nativeName.empty()) return t->nativeName == "portablebase";
    return false;
}

// Scripted objects that gain collision after their constructor / a pull: pressure plates and loose rigid bodies.
bool lateCollider(const script::Object& o) { return (o.joint && o.joint->prismatic) || o.loose; }
size_t lateColliders(const script::Runtime& rt) {
    return size_t(std::count_if(rt.objects().begin(), rt.objects().end(), lateCollider));
}

} // namespace

const formats::UberObjectData* World::prepareUber(const script::Object& o) {
    if (!script_.bigfile()) return nullptr;
    if (const auto it = uberData_.find(o.uid); it != uberData_.end()) return it->second ? &*it->second : nullptr;
    auto drm = objectDrms_.find(o.name);
    if (drm == objectDrms_.end()) {
        std::shared_ptr<const cdc::Drm> parsed;
        if (const auto* bf = script_.bigfile())
            if (auto bytes = bf->read("pc-w\\" + o.name + ".drm")) parsed = std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)));
        drm = objectDrms_.emplace(o.name, std::move(parsed)).first;
    }
    std::optional<formats::UberObjectData> data;
    if (drm->second) {
        try { data = formats::loadUberObject(*drm->second); } catch (const cdc::FormatError&) { return nullptr; }
        if (data) {
            if (const auto motion = uberMotion(*drm->second, o.name))
                uberMotion_[o.uid] = {motion->delta, motion->rotation, motion->duration, 0, false, motion->clip};
            auto& rt = uberRuntime_[o.uid];
            rt.sections.assign(data->sections.size(), {});
            // Transition animations: the object's animation list by index (transition +0x2A).
            for (const auto& e : anim::animList(*drm->second)) {
                float length = 0;
                for (const auto& sec : drm->second->sections)
                    if (sec.type == cdc::SectionType::Animation && sec.id == e.animId) length = float(anim::decodeClip(sec.data).durationSeconds());
                rt.animLength.push_back(length);
            }
        }
    }
    const auto [it, inserted] = uberData_.emplace(o.uid, std::move(data));
    (void)inserted;
    if (it->second) { // sections start in their default state (0x4D7550 flag 1 -> 0x4D7440)
        uberStates_[o.uid].assign(it->second->sections.size(), 0xFFFFFFFFu);
        for (size_t i = 0; i < it->second->sections.size(); ++i)
            if (!it->second->sections[i].stateOut.empty()) uberEnter(o.uid, i, it->second->sections[i].defaultState);
    }
    return it->second ? &*it->second : nullptr;
}

bool World::uberHolds(int32_t uid, const std::vector<formats::UberCondition>& conditions) const {
    const auto rt = uberRuntime_.find(uid);
    if (rt == uberRuntime_.end() || conditions.empty()) return false;
    for (const auto& c : conditions) { // 0x4D1360
        const bool present = c.section >= 0 && size_t(c.section) < rt->second.sections.size();
        if (c.kind == 1 ? !present || rt->second.sections[size_t(c.section)].state != int32_t(c.value) : c.kind != 2 || present)
            return false;
    }
    return true;
}

// Events of section `section` doing `kind` with `value` (0x4D3F60 / 0x4D3FF0 / 0x4D4080 / 0x4D4110 -> 0x4D3EC0):
// queued for the script's vt 54 (uberobjectevent members compare the event index).
void World::uberFire(int32_t uid, size_t section, uint32_t kind, uint32_t value) {
    const auto data = uberData_.find(uid);
    if (data == uberData_.end() || !data->second) return;
    auto& rt = uberRuntime_[uid];
    const auto& events = data->second->events;
    for (size_t i = 0; i < events.size(); ++i) {
        const auto& e = events[i];
        if (e.section != int32_t(section) || e.kind != kind || e.value != value) continue;
        if (e.once && i < 64 && (rt.fired >> i & 1)) continue;
        if (!std::all_of(e.conditions.begin(), e.conditions.end(), [&](const auto& c) { return uberHolds(uid, c); })) continue;
        if (e.once && i < 64) rt.fired |= uint64_t(1) << i;
        uberEvents_.push_back({uid, uint32_t(i)});
    }
}

// Enter state `state` (0x4D7440): finishes the running transition (event 4), then the state's events (1).
void World::uberEnter(int32_t uid, size_t section, uint32_t state) {
    auto& rt = uberRuntime_[uid];
    auto& s = rt.sections[section];
    if (s.state == int32_t(state)) return;
    const int32_t finished = s.transition;
    s.state = int32_t(state), s.transition = -1, s.time = 0, s.length = 0;
    if (finished < 0) s.carried = false; // set directly (command type 1, default state)
    uberStates_[uid][section] = state;
    if (finished >= 0) uberFire(uid, section, 4, uint32_t(finished));
    uberFire(uid, section, 1, state);
}

// Start transition `transition` (0x4D4A00): leaves the state (event 2), starts the transition (3). With flags 1 + 2
// and an animation it lasts the clip, else it ends on the next update.
void World::uberBegin(int32_t uid, size_t section, uint32_t transition) {
    const auto& data = *uberData_.at(uid);
    const auto& t = data.sections[section].transitions[transition];
    auto& rt = uberRuntime_[uid];
    auto& s = rt.sections[section];
    const int32_t left = s.state;
    const auto& actions = data.sections[section].stateActions;
    s.carried = left >= 0 && size_t(left) < actions.size() && std::ranges::find(actions[size_t(left)], 13u) != actions[size_t(left)].end();
    s.state = -1, s.transition = int32_t(transition), s.time = 0;
    s.length = (t.flags & 3) == 3 && t.anim >= 0 && size_t(t.anim) < rt.animLength.size() ? rt.animLength[size_t(t.anim)] : 0.0f;
    uberStates_[uid][section] = 0xFFFFFFFFu;
    if (left >= 0) uberFire(uid, section, 2, uint32_t(left));
    uberFire(uid, section, 3, transition);
}

// Section updates (0x4D7550): a running transition ends into its target state; otherwise the state's automatic
// transitions (0x4D31E0: 1 after `param` seconds, 3 always, 9 when its condition list holds) start. Then the queued
// events run the uber scripts' vt 54.
void World::tickUber(float dt) {
    for (auto& [uid, rt] : uberRuntime_) {
        const auto data = uberData_.find(uid);
        if (data == uberData_.end() || !data->second) continue;
        for (size_t i = 0; i < rt.sections.size(); ++i) {
            auto& s = rt.sections[i];
            const auto& sec = data->second->sections[i];
            if (s.transition >= 0) {
                if ((s.time += dt) >= s.length) uberEnter(uid, i, sec.transitions[size_t(s.transition)].to);
                continue;
            }
            if (s.state < 0 || size_t(s.state) >= sec.stateOut.size()) continue;
            s.time += dt;
            for (const uint32_t ti : sec.stateOut[size_t(s.state)]) {
                const auto& t = sec.transitions[ti];
                // ponytail: probability (flags 0x10, tru.exe's LCG 0x8A9788) ignored; no state machine uses it on the route.
                // ponytail: triggers 15 / 14 come from the section's physics joint reaching / leaving a limit (action 0x14
                // registers 0x4D5B10 -> 0x4D5A50 with the physics system). We have no uber joints: 15 fires after 1 s in a
                // state that drives its joint (action 13), 14 after 1 s when the state was left by a driving one (the
                // joint still moving: kraken entrance locks, l4 ramps); Lara-moved joints (levers) never fire them.
                const auto& actions = sec.stateActions[size_t(s.state)];
                const bool drives = std::ranges::find(actions, 13u) != actions.end();
                const bool go = t.trigger == 3 || (t.trigger == 1 && s.time > std::bit_cast<float>(t.param) - 0.001f) ||
                                (t.trigger == 9 && uberHolds(uid, t.conditions)) ||
                                (s.time >= 1.0f && ((t.trigger == 14 && s.carried) || (t.trigger == 15 && drives)));
                if (go) { uberBegin(uid, i, ti); break; }
            }
        }
    }
    for (size_t i = 0; i < uberEvents_.size(); ++i) { // handlers may send commands that queue more events
        const auto [uid, event] = uberEvents_[i];
        script_.uberEvent(uid, event);
    }
    uberEvents_.clear();
}

// One command table entry (0x4D77A0 -> 0x4D7680 per target): 1 sets the section's state, 2 starts the state's
// transition whose trigger is the payload, 3 / 4 the one with trigger 0xD / 0x10 and that parameter.
bool World::sendUberCommand(int32_t uid, uint32_t commandIndex) {
    const auto* object = script_.find(uid);
    if (!object) return false;
    const auto* data = prepareUber(*object);
    if (!data || commandIndex >= data->commands.size()) return false;
    auto& rt = uberRuntime_[uid];
    for (const auto& target : data->commands[commandIndex].targets) {
        if (target.type == 0 || target.section < 0 || size_t(target.section) >= rt.sections.size()) continue;
        const size_t i = size_t(target.section);
        const auto& sec = data->sections[i];
        if (target.type == 1) {
            if (target.payload < sec.stateOut.size()) uberEnter(uid, i, target.payload), rt.commanded = true;
            continue;
        }
        const auto& s = rt.sections[i];
        if (s.transition >= 0 || s.state < 0 || size_t(s.state) >= sec.stateOut.size()) continue; // busy (0x4D4BE0)
        for (const uint32_t ti : sec.stateOut[size_t(s.state)]) {
            const auto& t = sec.transitions[ti];
            const bool match = target.type == 2 ? t.trigger == target.payload
                                                : t.trigger == (target.type == 3 ? 0xDu : 0x10u) && t.param == target.payload;
            if (match) {
                uberBegin(uid, i, ti), rt.commanded = true;
                break;
            }
        }
    }
    if (const auto motion = uberMotion_.find(uid); motion != uberMotion_.end() && rt.commanded) motion->second.active = true;
    return true;
}

std::vector<World::ObjectClip> World::objectClips() const {
    std::vector<ObjectClip> out;
    for (const auto& o : script_.objects()) {
        if (o.clips && o.clipNames && o.clipNames->size() == 2 && o.state != 0)
            out.push_back({o.uid, o.name, (*o.clipNames)[o.state == 1 ? 0 : 1], o.t, (*o.clips)[o.state == 1 ? 0 : 1].durationSeconds(),
                           o.state == 2, introModel(o)});
        else if (const auto m = uberMotion_.find(o.uid); m != uberMotion_.end() && m->second.active && m->second.t < m->second.duration)
            out.push_back({o.uid, o.name, m->second.clip, m->second.t, m->second.duration, false, introModel(o)});
    }
    return out;
}

std::vector<glm::mat4> World::objectBones(int32_t uid) const {
    const auto* o = script_.find(uid);
    if (!o || !o->clips || !o->skeleton || o->state == 0) return {};
    return anim::worldMatrices(*o->skeleton, anim::sample((*o->clips)[o->state == 1 ? 0 : 1], *o->skeleton, o->t, o->state == 2));
}

void World::bindUber() {
    script_.setUberNative([this](int32_t uid, uint32_t function, uint32_t command, uint32_t,
                                 std::span<const script::vm::Value>, uint32_t& result) {
        const auto* object = script_.find(uid);
        if (!object) return false;
        const auto* data = prepareUber(*object);
        if (!data) return false;
        if (function == 5) return true; // initialize the command-driven section state machine
        if (function == 2) { // [I] some section away from its default state
            const auto& rt = uberRuntime_[uid];
            result = false;
            for (size_t i = 0; i < rt.sections.size(); ++i)
                result |= rt.sections[i].state != int32_t(data->sections[i].defaultState);
            return true;
        }
        if (function != 1) return false;
        // The command member is {uber object id, command word}; the word indexes the command table (0x4D77A0).
        if (command >= data->commands.size() || !sendUberCommand(uid, command)) return false;
        result = 1;
        return true;
    });
}

void World::bindTerrain() {
    script_.setTerrainNative([this](int32_t, uint16_t streamUnit, uint32_t model, uint32_t function,
                                    std::span<const script::vm::Value> args, uint32_t& result) {
        LoadedUnit* addressed = nullptr;
        for (auto& candidate : units_) {
            for (const auto& source : units_) {
                const auto portal = std::find_if(source.data.portals.begin(), source.data.portals.end(), [&](const auto& p) {
                    return p.toStreamId == int16_t(streamUnit) && p.toUnit == candidate.data.name;
                });
                if (portal != source.data.portals.end()) { addressed = &candidate; break; }
            }
            if (addressed) break;
        }
        if (!addressed && units_.size() == 1) addressed = &units_[0];
        if (!addressed) return false;
        if (function == 3) {
            const auto enabled = addressed->data.col.terrainGroupEnabled(model);
            if (!enabled) return false;
            result = *enabled;
            return true;
        }
        if (function != 2 || args.size() != 1 || !addressed->data.col.setTerrainGroupEnabled(model, args[0].bits != 0)) return false;
        rebuildCollision();
        return true;
    });
}

bool World::lightEnabled(std::string_view unit, uint32_t lightId) const {
    const auto it = lightStates_.find({std::string(unit), lightId});
    return it == lightStates_.end() || it->second;
}

void World::bindLightHealth() {
    script_.setLightNative([this](int32_t uid, uint32_t lightId, uint32_t function,
                                  std::span<const script::vm::Value> args, uint32_t& result) {
        const auto* object = script_.find(uid);
        if (!object) return false;
        const auto key = std::make_pair(object->unit, lightId);
        if (function == 2) {
            result = lightEnabled(key.first, key.second);
            return true;
        }
        if (function != 1 || args.size() != 1) return false;
        const bool enabled = args[0].bits != 0;
        auto [it, inserted] = lightStates_.try_emplace(key, true);
        if (it->second != enabled) {
            it->second = enabled;
            ++lightsVersion_;
        }
        return true;
    });
    script_.setHealthNative([this](uint32_t function, std::span<const script::vm::Value> args, uint32_t& result) {
        if (function == 9 && args.empty()) {
            result = std::bit_cast<uint32_t>(health_.value()); // instance property 0x1D (tru.exe 0x4A5200)
            return true;
        }
        // fn 1 (0x4A50F0): INSTANCE_Post(0xD, value), scripts pass 12345 to kill [I: absolute damage];
        // fn 2 (0x4A50D0 -> 0x5D3C90): hit message 0x11C with float(amount) and the type argument [I: same type ids].
        if (function == 1 && args.size() == 1) return health_.damage(float(int32_t(args[0].bits)), kDamageAbsolute), true;
        if (function == 3 && args.size() == 1) return health_.heal(float(int32_t(args[0].bits))), true; // message 5 (0x6F4EF0)
        if (function == 2 && (args.size() == 1 || args.size() == 2))
            return health_.damage(float(int32_t(args[0].bits)), args.size() == 2 ? int(args[1].bits) : kDamageLight), true;
        return false;
    });
}

// markup SetEnabled (fn 0) / IsEnabled (fn 1): tru.exe 0x4A56B0 sets/clears bit 31 of the markup box flags, which the
// traversal queries skip; 0x4A5720 reports it (the owner-instance checks of 0x5BE2F0 are not modelled).
void World::bindMarkup() {
    script_.setMarkupNative([this](const std::string& unit, uint32_t id, uint32_t function,
                                   std::span<const script::vm::Value> args, uint32_t& result) {
        const auto key = std::make_pair(unit, uint16_t(id));
        if (function == 1) {
            result = markupEnabled(unit, key.second);
            return true;
        }
        const bool enabled = (args[0].bits & 0xFF) != 0;
        if (enabled ? !disabledMarkup_.erase(key) : !disabledMarkup_.insert(key).second) return true; // unchanged
        for (const auto& u : units_)
            if (u.data.name == unit)
                for (size_t i = 0; i < u.data.col.markup.size(); ++i)
                    if (u.data.col.markup[i].id == key.second)
                        col_.markup[u.markupBase + i].type = enabled ? u.data.col.markup[i].type : kMarkupDisabled;
        return true;
    });
}

// enemybase (tru.exe bind 0x7B4060): 16/17 Set/Get the AI behaviour index (0x776F10 -> 0x62DE00 selects the
// behaviour list of the enemy data; 0x776F40 reads it back), 22 selects entry n of the current target's list and
// posts 0xE1 (0x777010 -> 0x63AC40), 26 picks the MoveNav animation variant (0x7771E0, field 0x37C0 read by
// 0x61B0C0). Ours keep the values for Get; our AI has no behaviour lists or variants [U: their effect on it].
// 27 GuardNode(unitref, node, r1, r2) (0x777210 -> 0x61F7A0, mercs with radii 2500/4000): our AI guards no node [U].
// 18 sets an AI byte (+0xC34, 0x776F70, then 0x63F7F0) [U]; kept like 16.
// generatorbase (0x7B4B60) and health 1/2 on an enemy's own script go to ai::Enemies.
void World::bindEnemies() {
    script_.setEnemyNative([this](int32_t uid, std::string_view type, uint32_t function,
                                  std::span<const script::vm::Value> args, uint32_t& result) {
        if (type == "generatorbase") return enemies_.generatorNative(uid, function, result);
        if (type == "health") {
            if ((function != 1 || args.size() != 1) && (function != 2 || args.empty() || args.size() > 2)) return false;
            enemies_.damage(uid, float(int32_t(args[0].bits))); // [I] absolute, like Lara's; absent enemy: no-op
            return true;
        }
        if ((function == 16 || function == 18 || function == 22 || function == 26) && args.size() == 1) {
            enemyValues_[{uid, function}] = args[0].bits;
            return true;
        }
        if (function == 27 && args.size() == 4) return true;
        if (function == 17 && args.empty()) {
            const auto it = enemyValues_.find({uid, 16});
            result = it == enemyValues_.end() ? 0 : it->second;
            return true;
        }
        return false;
    });
}

void World::bindMultibody() {
    script_.setMessageNative([](uint32_t, uint32_t, std::string_view) {
        // Send (0x4A2E70) posts to the channel's native subscribers; a post nobody consumes is a no-op in the exe too.
        // Ours: the app owns one cinematic at a time ("stopOthers") and Enemies drives birth/generator state
        // ("enemyBirthed"); UI/HUD channel subscribers are not modelled [U].
        return true;
    });
    script_.setMultibodyNative([this](int32_t uid, uint32_t function, std::span<const script::vm::Value> args,
                                      uint32_t& result) {
        int index = -1;
        for (const auto& c : colliders_)
            if (c.uid == uid && c.hinge >= 0) { index = c.hinge; break; }
        if (function == 2) return true; // Enable: bodies are already active once their hinge exists
        if (function != 77 && function != 78 && function != 79) return false;
        if (args.empty() || args[0].bits != 0) return false; // one hinge per current rigid-body object
        if (function == 77) {
            if (args.size() != 4 || args[1].bits < 1 || args[1].bits > 3) return false;
            const float lo = glm::radians(std::bit_cast<float>(args[2].bits));
            const float hi = glm::radians(std::bit_cast<float>(args[3].bits));
            if (args[1].bits != 3) {
                if (index >= 0) physics_.hinges[index].lo = lo;
                else multibodyLo_[uid] = lo;
            }
            if (args[1].bits != 2) {
                if (index >= 0) physics_.hinges[index].hi = hi;
                else multibodyHi_[uid] = hi;
            }
            return true;
        }
        if (function == 78) {
            result = std::bit_cast<uint32_t>(index >= 0 ? glm::degrees(physics_.angle(index)) : 0.0f);
            return true;
        }
        if (args.size() != 5) return false; // joint, limit, drive parameters
        if (index >= 0) physics_.hinges[index].maxTorque = 1e11f;
        return true;
    });
}

void World::syncObjects() {
    std::map<int32_t, float> angles; // doors that stay loaded keep their swing
    for (const auto& c : colliders_)
        if (c.hinge >= 0) angles[c.uid] = physics_.angle(c.hinge);
    colliders_.clear();
    col_.dynamics.clear();
    physics_ = {};
    for (const auto& o : script_.objects()) {
        const bool ordinary = o.kind == script::Kind::Door || o.kind == script::Kind::Block || o.kind == script::Kind::Switch;
        const formats::UberObjectData* uber = (ordinary || o.name.starts_with("ms_") || o.name.starts_with("sa_uo_") ||
                                               o.name.starts_with("ca_")) ? prepareUber(o) : nullptr;
        if (!ordinary && !uber && !o.spline && !lateCollider(o)) continue; // spline movers' collision follows Object::body (pose below)
        if (o.kind == script::Kind::Switch && !uber &&
            (!o.script || !o.script->program->mainFunction(o.script->typeId))) continue;
        auto it = objectPhysics_.find(o.name);
        if (it == objectPhysics_.end()) {
            std::shared_ptr<const physics::ObjectPhysics> p;
            if (const auto* bf = script_.bigfile())
                if (auto bytes = bf->read("pc-w\\" + o.name + ".drm")) try {
                        p = std::make_shared<physics::ObjectPhysics>(physics::loadObjectPhysics(cdc::parseDrm(std::move(*bytes))));
                    } catch (const cdc::FormatError& e) {
                        warnings.push_back(o.name + ": " + e.what());
                    }
            it = objectPhysics_.emplace(o.name, std::move(p)).first;
        }
        if (!it->second) continue;
        const physics::ObjectPhysics& p = *it->second;
        const glm::mat4 intro = introModel(o);
        int hinge = -1;
        // [I] Rigid doors (class 34) and joint levers (class 6) swing about the X axis of their first model marker.
        if ((o.kind == script::Kind::Door || o.kind == script::Kind::Switch) && !o.clips &&
            (o.kind == script::Kind::Switch || p.physClass == physics::kClassRigidBody) && !p.markers.empty()) {
            if (const auto mp = physics::massProps(p); mp.mass > 0) {
                const auto& m = p.markers[0];
                const glm::mat3 r(glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                                  glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1}));
                hinge = physics_.addHinge(physics_.addBody(mp, intro), m.pos, r * glm::vec3(1, 0, 0), 0, kDoorOpen + 0.02f);
                if (const auto lo = multibodyLo_.find(o.uid); lo != multibodyLo_.end()) physics_.hinges[hinge].lo = lo->second;
                if (const auto hi = multibodyHi_.find(o.uid); hi != multibodyHi_.end()) physics_.hinges[hinge].hi = hi->second;
                physics_.hinges[hinge].maxTorque = 1e11f; // [I] a motor that always wins against gravity
                if (const auto a = angles.find(o.uid); a != angles.end()) physics_.setAngle(hinge, a->second);
            }
        }
        for (int seg = 0; seg < int(p.segments.size()); ++seg)
            if (auto shape = physics::solidShape(p, seg)) {
                col_.dynamics.push_back({o.uid, std::move(shape), intro * p.segments[seg]});
                colliders_.push_back({o.uid, seg, hinge, intro, p.segments[seg]});
            }
    }
    syncedObjects_ = script_.objects().size();
    syncedPlates_ = lateColliders(script_);
}

void World::tickObjects(float dt) {
    // Objects came or went, or the merged collision was rebuilt (streaming drops its dynamics).
    if (script_.objects().size() != syncedObjects_ || col_.dynamics.size() != colliders_.size() ||
        lateColliders(script_) != syncedPlates_)
        syncObjects();
    tickUber(dt);
    for (auto& [uid, motion] : uberMotion_) {
        const float target = motion.active ? motion.duration : 0.0f;
        motion.t = std::clamp(motion.t + (target > motion.t ? dt : -dt), 0.0f, motion.duration);
    }
    for (const auto& c : colliders_) // lever use drives its hinge; a raised door limit drives the door to it [I]
        if (c.hinge >= 0) {
            const auto* o = script_.find(c.uid);
            auto& hinge = physics_.hinges[c.hinge];
            const auto rt = uberRuntime_.find(c.uid);
            const bool uberOpen = rt != uberRuntime_.end() && rt->second.commanded;
            if (uberOpen)
                hinge.target = kDoorOpen;
            else if (o && o->kind == script::Kind::Door && o->state != 2 && hinge.hi > glm::radians(2.01f))
                hinge.target = hinge.hi;
            else
                hinge.target = o && o->state == 2 ? (o->kind == script::Kind::Switch ? kDoorOpen * 0.5f : kDoorOpen) : 0.0f;
        }
    physics_.step(dt);
    // Lara rides the dynamic she stands on (platforms, a door swinging under her).
    const vec3 feet = player_.state().pos;
    const auto ground = col_.groundProbe(feet + vec3(0, 0, 20), 60);
    const int under = ground ? ground->dynamic : -1;
    const glm::mat4 before = under >= 0 ? col_.dynamics[under].transform : glm::mat4(1);
    tickLoose(dt);
    tickPlates(dt);
    for (size_t i = 0; i < colliders_.size(); ++i) {
        const auto& c = colliders_[i];
        const auto* o = script_.find(c.uid);
        if (!o) continue;
        glm::mat4 model(1); // model-space rigid motion of the segment
        if (c.hinge >= 0) {
            model = glm::inverse(c.intro) * physics_.bodies[physics_.hinges[c.hinge].body].model();
            script_.setBody(c.uid, model);
        } else if (const auto motion = uberMotion_.find(c.uid); motion != uberMotion_.end()) {
            const float alpha = motion->second.t / motion->second.duration;
            model = glm::translate(glm::mat4(1), motion->second.delta * alpha) *
                    glm::mat4(anim::rotationFromVector(motion->second.rotation * alpha));
            script_.setBody(c.uid, model);
        } else if (const auto palette = script_.pose(*o); palette && !palette->empty()) {
            model = (*palette)[std::min<size_t>(c.segment, palette->size() - 1)];
        }
        col_.dynamics[i].transform = c.intro * model * c.frame;
    }
    if (under >= 0) {
        const vec3 moved = vec3(col_.dynamics[under].transform * glm::inverse(before) * glm::vec4(feet, 1)) - feet;
        if (glm::dot(moved, moved) > 1e-6f) player_.carry(moved);
    }
}

// Portable objects (heavy weights): interact next to one picks it up, she carries it in front of her chest, interact
// again sets it down in front of her feet, where it falls as a loose body (it weighs on pressure plates). [I] No carry
// clips or walk-speed change; the script's vt 20 pick-up callback (sets its "touched" sticky) is not sent.
void World::carryPortables(bool pressed) {
    const auto& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    auto* held = held_ ? const_cast<script::Object*>(script_.find(held_)) : nullptr; // World owns object motion
    if (held_ && !held) held_ = 0;
    if (held && pressed) {
        held->offset = s.pos + fwd * 90.0f + vec3(0, 0, 20) - held->pos;
        held->loose = true;
        held_ = 0;
        return;
    }
    if (held) {
        held->offset = s.pos + fwd * 60.0f + vec3(0, 0, 80) - held->pos;
        return;
    }
    if (!pressed || s.state != State::Idle) return;
    for (const auto& o : script_.objects()) {
        const vec3 at = o.pos + o.offset;
        if (portable(o) && glm::length(vec2(at - s.pos)) < 150.0f && std::abs(at.z - s.pos.z) < 100.0f) {
            held_ = o.uid;
            fallSpeed_.erase(o.uid);
            const_cast<script::Object&>(o).loose = false;
            return;
        }
    }
}

// Loose rigid bodies (Object::loose) fall under gravity until their collision rests on what is below (terrain, a
// plate: they follow a sinking plate down). [I] No tumbling or sliding; tru.exe's Havok body does both.
void World::tickLoose(float dt) {
    for (const auto& o : script_.objects()) {
        if (!o.loose) continue;
        vec3 lo(1e30f), hi(-1e30f);
        for (const auto& d : col_.dynamics)
            if (d.id == o.uid)
                for (const vec3& v : d.shape->vertices) {
                    const vec3 p(d.transform * glm::vec4(v, 1));
                    lo = glm::min(lo, p), hi = glm::max(hi, p);
                }
        if (lo.x > hi.x) lo = hi = o.pos + o.offset + vec3(0, 0, 1); // no collision: its origin is its base
        const vec3 base((lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, lo.z - 1.0f);
        const auto ground = col_.groundProbe(base, 20000.0f);
        float& v = fallSpeed_[o.uid];
        const float gap = ground ? ground->t : 0.0f;
        if (gap <= 0.0f) { v = 0; continue; }
        v += Tuning{}.gravity * dt;
        const float drop = std::min(gap, v * dt);
        if (drop >= gap) v = 0;
        const_cast<script::Object*>(&o)->offset.z -= drop; // World owns object motion as for pulled blocks
    }
}

// Pressure plates (prismatic joints, Runtime::Object::Joint): weight on the plate — Lara standing on it, a push block
// resting on it, a loose rigid body landed on it — drives the joint down to its lowest range; unloaded it rises back to 0. Entering a range runs the
// plate's vt 34 (pressed / released ids). [I] The original simulates the joint and the weights in Havok; ours moves at
// a fixed speed, and only Lara, push blocks and loose bodies weigh (other physics props do not).
void World::tickPlates(float dt) {
    constexpr float kSpeed = 120.0f; // joint units (model z) per second
    const auto box = [&](int32_t uid) { // world AABB of an object's collision
        vec3 lo(1e30f), hi(-1e30f);
        for (const auto& d : col_.dynamics)
            if (d.id == uid)
                for (const vec3& v : d.shape->vertices) {
                    const vec3 p(d.transform * glm::vec4(v, 1));
                    lo = glm::min(lo, p), hi = glm::max(hi, p);
                }
        return std::pair(lo, hi);
    };
    const vec3 feet = player_.state().pos;
    for (const auto& o : script_.objects()) {
        if (!o.joint || !o.joint->prismatic || o.joint->ranges.empty()) continue;
        const auto [lo, hi] = box(o.uid);
        const auto on = [&](vec3 c, float bottom) { // over the plate's footprint, resting on its top
            return c.x > lo.x && c.x < hi.x && c.y > lo.y && c.y < hi.y && bottom > hi.z - 30.0f && bottom < hi.z + 40.0f;
        };
        bool loaded = on(feet, feet.z);
        for (const auto& b : script_.objects())
            if ((b.kind == script::Kind::Block || b.loose) && !loaded) {
                const auto [blo, bhi] = box(b.uid);
                if (blo.x <= bhi.x) loaded = on((blo + bhi) * 0.5f, blo.z);
                else if (b.loose) loaded = on(b.pos + b.offset, b.pos.z + b.offset.z); // no collision: origin = base
            }
        float bottom = 0;
        for (const auto& r : o.joint->ranges) bottom = std::min(bottom, r.lo);
        const float x = o.joint->position, target = loaded ? bottom : 0.0f;
        if (x == target) continue;
        const float next = target < x ? std::max(target, x - kSpeed * dt) : std::min(target, x + kSpeed * dt);
        const int32_t uid = o.uid; // setJointPosition may run scripts
        script_.setBody(uid, glm::translate(glm::mat4(1), vec3(0, 0, next)));
        script_.setJointPosition(uid, next);
    }
}

} // namespace ou::game
