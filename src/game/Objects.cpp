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
                uberMotion_[o.uid] = {motion->delta, motion->rotation, motion->duration, 0, false};
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
        if (target.section < 0 || size_t(target.section) >= states.size()) continue;
        states[size_t(target.section)] = target.payload;
    }
    if (const auto motion = uberMotion_.find(uid); motion != uberMotion_.end())
        motion->second.active = std::any_of(states.begin(), states.end(), [](uint32_t state) { return state != 0; });
    return true;
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
        const auto commandIt = commandIndex < data->commands.size() && data->commands[commandIndex].id == command
            ? data->commands.begin() + commandIndex
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
        if (function != 9 || !args.empty()) return false;
        result = std::bit_cast<uint32_t>(health_.value()); // instance property 0x1D (tru.exe 0x4A5200)
        return true;
    });
}

void World::bindMultibody() {
    script_.setMessageNative([](uint32_t, uint32_t, std::string_view payload) {
        // The app already owns one cinematic at a time, and enemy birth/generator state is driven by Enemies.
        return payload == "stopOthers" || payload == "enemyBirthed";
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
        if (!ordinary && !uber) continue;
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
