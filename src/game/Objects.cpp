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
            uberStates_[o.uid].assign(data->sectionModels.size(), 0);
            if (const auto motion = uberMotion(*drm->second, o.name))
                uberMotion_[o.uid] = {motion->delta, motion->rotation, motion->duration, 0, false, motion->clip};
        }
    }
    const auto [it, inserted] = uberData_.emplace(o.uid, std::move(data));
    (void)inserted;
    return it->second ? &*it->second : nullptr;
}

bool World::sendUberCommand(int32_t uid, uint32_t commandIndex) {
    const auto* object = script_.find(uid);
    if (!object) return false;
    const auto* data = prepareUber(*object);
    if (!data || commandIndex >= data->commands.size()) return false;
    auto& states = uberStates_[uid];
    for (const auto& target : data->commands[commandIndex].targets) {
        if (target.type == 0 || target.section < 0 || size_t(target.section) >= states.size()) continue;
        states[size_t(target.section)] = target.payload;
    }
    if (const auto motion = uberMotion_.find(uid); motion != uberMotion_.end())
        motion->second.active = std::any_of(states.begin(), states.end(), [](uint32_t state) { return state != 0; });
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
    script_.setUberNative([this](int32_t uid, uint32_t function, uint32_t command, uint32_t commandIndex,
                                 std::span<const script::vm::Value>, uint32_t& result) {
        const auto* object = script_.find(uid);
        if (!object) return false;
        const auto* data = prepareUber(*object);
        if (!data) return false;
        auto& states = uberStates_[uid];
        if (function == 5) return true; // initialize the command-driven section state machine
        if (function == 2) {
            result = std::any_of(states.begin(), states.end(), [](uint32_t state) { return state != 0; });
            return true;
        }
        if (function != 1) return false;
        // The command member is {uber object id, command word}: the word names its table entry [I: by ordinal and
        // id when they agree, else as the table index, else as the id].
        const auto commandIt = commandIndex < data->commands.size() && data->commands[commandIndex].id == command
            ? data->commands.begin() + commandIndex
            : command < data->commands.size() ? data->commands.begin() + command
            : std::find_if(data->commands.begin(), data->commands.end(), [&](const auto& c) { return c.id == command; });
        if (commandIt == data->commands.end()) return false;
        if (!sendUberCommand(uid, uint32_t(commandIt - data->commands.begin()))) return false;
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
        if (!ordinary && !uber && !o.spline) continue; // spline movers' collision follows Object::body (pose below)
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
}

void World::tickObjects(float dt) {
    // Objects came or went, or the merged collision was rebuilt (streaming drops its dynamics).
    if (script_.objects().size() != syncedObjects_ || col_.dynamics.size() != colliders_.size()) syncObjects();
    for (auto& [uid, motion] : uberMotion_) {
        const float target = motion.active ? motion.duration : 0.0f;
        motion.t = std::clamp(motion.t + (target > motion.t ? dt : -dt), 0.0f, motion.duration);
    }
    for (const auto& c : colliders_) // lever use drives its hinge; a raised door limit drives the door to it [I]
        if (c.hinge >= 0) {
            const auto* o = script_.find(c.uid);
            auto& hinge = physics_.hinges[c.hinge];
            const auto states = uberStates_.find(c.uid);
            const bool uberOpen = states != uberStates_.end() &&
                                  std::any_of(states->second.begin(), states->second.end(), [](uint32_t state) { return state != 0; });
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

} // namespace ou::game
