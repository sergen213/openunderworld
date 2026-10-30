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

} // namespace

const physics::ObjectPhysics* World::objectPhysics(const std::string& name) {
    auto it = objectPhysics_.find(name);
    if (it == objectPhysics_.end()) {
        std::shared_ptr<const physics::ObjectPhysics> p;
        if (const auto* bf = script_.bigfile())
            if (auto bytes = bf->read("pc-w\\" + name + ".drm")) try {
                    p = std::make_shared<physics::ObjectPhysics>(physics::loadObjectPhysics(cdc::parseDrm(std::move(*bytes))));
                } catch (const cdc::FormatError& e) {
                    warnings.push_back(name + ": " + e.what());
                }
        it = objectPhysics_.emplace(name, std::move(p)).first;
    }
    return it->second.get();
}

// HInfo marker `id` of an object in the world: intro transform * its segment's bind frame * the marker frame (0x46DF20).
std::optional<glm::mat4> World::markerFrame(const script::Object& o, int id) {
    const physics::ObjectPhysics* p = objectPhysics(o.name);
    if (!p) return std::nullopt;
    for (const auto& m : p->hmarkers)
        if (m.id == id) {
            const glm::mat4 segment = size_t(m.segment) < p->segments.size() ? p->segments[size_t(m.segment)] : glm::mat4(1);
            return glm::translate(glm::mat4(1), o.offset) * introModel(o) * segment * glm::translate(glm::mat4(1), m.pos) *
                   glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                   glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1});
        }
    return std::nullopt;
}

namespace {

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
    if (!it->second) return nullptr;
    uberStates_[o.uid].assign(it->second->sections.size(), 0xFFFFFFFFu);
    if (const auto saved = uberSaved_.find(o.uid); saved != uberSaved_.end()) { // a load: as saved, no events
        auto& rt = uberRuntime_[o.uid];
        const auto& j = saved->second;
        const auto& s = j.at("sections");
        if (s.size() == rt.sections.size()) {
            for (size_t i = 0; i < s.size(); ++i) {
                auto& sec = rt.sections[i];
                sec.state = s[i].at(0), sec.transition = s[i].at(1), sec.time = s[i].at(2), sec.length = s[i].at(3);
                if (sec.state >= 0) uberStates_[o.uid][i] = uint32_t(sec.state);
            }
            rt.fired = j.at("fired"), rt.commanded = j.at("commanded");
            if (const auto m = uberMotion_.find(o.uid); m != uberMotion_.end() && j.contains("motion"))
                m->second.t = j["motion"].at(0), m->second.active = j["motion"].at(1);
            for (const auto& e : j.value("joints", nlohmann::json::array())) // state: x, v, limit, motor
                if (auto joint = makeUberJoint(o.uid, e.at(0).get<int32_t>())) {
                    joint->x = e.at(1), joint->v = e.at(2), joint->lo = e.at(3), joint->hi = e.at(4), joint->limited = e.at(5);
                    joint->speed = e.at(6), joint->force = e.at(7), joint->motor = e.at(8);
                    rt.joints[e.at(0).get<int32_t>()] = *joint;
                }
            for (const auto& e : j.value("links", nlohmann::json::array())) rt.links.insert(e.get<int32_t>());
            for (const auto& e : j.value("watches", nlohmann::json::array()))
                rt.watches.push_back({e.at(0).get<int32_t>(), e.at(1).get<int32_t>(), e.at(2).get<int32_t>(),
                                      e.at(3).get<uint32_t>(), e.at(4).get<bool>()});
            uberSaved_.erase(saved);
            return &*it->second;
        }
        uberSaved_.erase(saved); // the object's data changed shape: start it afresh
    }
    for (size_t i = 0; i < it->second->sections.size(); ++i) // sections start in their default state (0x4D7550 flag 1 -> 0x4D7440)
        if (!it->second->sections[i].stateOut.empty()) uberEnter(o.uid, i, it->second->sections[i].defaultState);
    return &*it->second;
}

nlohmann::json World::serializeObjects() const {
    nlohmann::json uber = nlohmann::json::object();
    for (const auto& [uid, j] : uberSaved_) uber[std::to_string(uid)] = j;
    for (const auto& [uid, rt] : uberRuntime_) {
        if (const auto d = uberData_.find(uid); d == uberData_.end() || !d->second) continue;
        nlohmann::json sections = nlohmann::json::array();
        for (const auto& s : rt.sections) sections.push_back({s.state, s.transition, s.time, s.length});
        nlohmann::json& e = uber[std::to_string(uid)] = {{"sections", std::move(sections)}, {"fired", rt.fired}, {"commanded", rt.commanded}};
        if (const auto m = uberMotion_.find(uid); m != uberMotion_.end()) e["motion"] = {m->second.t, m->second.active};
        nlohmann::json joints = nlohmann::json::array(), watches = nlohmann::json::array();
        for (const auto& [index, jt] : rt.joints)
            joints.push_back({index, jt.x, jt.v, jt.lo, jt.hi, jt.limited, jt.speed, jt.force, jt.motor});
        for (const auto& w : rt.watches) watches.push_back({w.section, w.joint, w.range, w.mode, w.inside});
        e["joints"] = std::move(joints), e["links"] = rt.links, e["watches"] = std::move(watches);
    }
    return {{"uber", std::move(uber)}, {"held", held_}};
}

void World::restoreObjects(const nlohmann::json& j) {
    std::map<int32_t, nlohmann::json> saved;
    int32_t held = 0;
    if (j.is_object()) {
        try {
            const nlohmann::json uber = j.value("uber", nlohmann::json::object());
            for (const auto& [key, e] : uber.items()) {
                const auto& s = e.at("sections");
                for (const auto& sec : s) // validate the shape now: prepareUber reads it later (older saves: 5 entries)
                    if (!sec.is_array() || sec.size() < 4 || !sec[0].is_number_integer() || !sec[1].is_number_integer() ||
                        !sec[2].is_number() || !sec[3].is_number())
                        throw std::runtime_error("bad uber section of " + key);
                (void)e.at("fired").get<uint64_t>(), (void)e.at("commanded").get<bool>();
                if (e.contains("motion")) (void)e["motion"].at(0).get<float>(), (void)e["motion"].at(1).get<bool>();
                for (const auto& jt : e.value("joints", nlohmann::json::array())) {
                    (void)jt.at(0).get<int32_t>();
                    for (int k : {1, 2, 3, 4, 6, 7}) (void)jt.at(k).get<float>();
                    (void)jt.at(5).get<bool>(), (void)jt.at(8).get<bool>();
                }
                (void)e.value("links", nlohmann::json::array()).get<std::vector<int32_t>>();
                for (const auto& w : e.value("watches", nlohmann::json::array())) {
                    for (int k : {0, 1, 2}) (void)w.at(k).get<int32_t>();
                    (void)w.at(3).get<uint32_t>(), (void)w.at(4).get<bool>();
                }
                saved[std::stoi(key)] = e;
            }
            held = j.value("held", 0);
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error(std::string("objects: ") + e.what());
        } catch (const std::logic_error& e) {
            throw std::runtime_error(std::string("objects: ") + e.what());
        }
    }
    uberData_.clear(), uberRuntime_.clear(), uberStates_.clear(), uberMotion_.clear(), uberEvents_.clear();
    uberSaved_ = std::move(saved);
    held_ = held, fallSpeed_.clear();
    syncedObjects_ = ~size_t(0); // prepare the loaded uber objects on the next tick
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

// Enter state `state` (0x4D7440): runs its entry actions (0x4D5C60), finishes the running transition (event 4), then
// the state's events (1).
void World::uberEnter(int32_t uid, size_t section, uint32_t state) {
    auto& rt = uberRuntime_[uid];
    auto& s = rt.sections[section];
    if (s.state == int32_t(state)) return;
    const int32_t finished = s.transition;
    s.state = int32_t(state);
    for (const auto& a : uberData_.at(uid)->sections[section].stateActions[state]) uberAction(uid, section, a);
    s.transition = -1, s.time = 0, s.length = 0;
    uberStates_[uid][section] = state;
    if (finished >= 0) uberFire(uid, section, 4, uint32_t(finished));
    uberFire(uid, section, 1, state);
}

// A state's physics entry actions (0x4D86A0); the others (animations, flags) are not modelled here.
void World::uberAction(int32_t uid, size_t section, const formats::UberAction& a) {
    auto& rt = uberRuntime_[uid];
    const auto& data = *uberData_.at(uid);
    const auto joint = rt.joints.find(a.joint);
    const formats::UberParam* p = a.param >= 0 ? &data.params[size_t(a.param)] : nullptr;
    const float unit = joint != rt.joints.end() && joint->second.hinge ? glm::radians(1.0f) : 1.0f; // records: degrees
    switch (a.type) {
    case 9: // create (0x4D8000; an existing joint stays as it is); links need both joints (0x4D7ED0)
        if (a.joint < 0 || rt.joints.contains(a.joint) || rt.links.contains(a.joint)) break;
        if (const auto& j = data.joints[size_t(a.joint)]; j.type == 11) {
            if (rt.joints.contains(j.link1) && rt.joints.contains(j.link2)) rt.links.insert(a.joint);
        } else if (auto made = makeUberJoint(uid, a.joint)) {
            rt.joints[a.joint] = *made;
        }
        break;
    case 10: // remove (0x4D6220): its watches go quiet (0x4937A0 skips missing joints)
        rt.joints.erase(a.joint), rt.links.erase(a.joint);
        break;
    case 12: // limit (0x4D6310 -> 0x4D4510)
        if (joint != rt.joints.end() && p) joint->second.lo = p->a * unit, joint->second.hi = p->b * unit, joint->second.limited = p->flag;
        break;
    case 13: // motor (0x4D6350 -> 0x4D2B00 / 0x4D29F0): speed per second -> per frame
        if (joint != rt.joints.end() && p) joint->second.speed = p->a * unit / 30.0f, joint->second.force = p->b, joint->second.motor = p->flag;
        break;
    case 0x14: // watch (0x4D7380 -> 0x49A5C0); ponytail: a repeated registration is kept once
    case 0x15: { // unwatch (0x493A80)
        const auto same = [&](const UberWatch& w) {
            return w.section == int32_t(section) && w.joint == a.joint && w.range == a.param && w.mode == a.value;
        };
        if (a.type == 0x15) std::erase_if(rt.watches, same);
        else if (a.joint >= 0 && a.param >= 0 && std::ranges::none_of(rt.watches, same))
            rt.watches.push_back({int32_t(section), a.joint, a.param, a.value, true});
        break;
    }
    default: break;
    }
}

// Joint frame (object model `model`'s marker, its Z the axis) and body (section bodyB's model: mass, centre of mass,
// inertia) in the world, at rest at coordinate 0. Types 0 (hinge) and 2 (prismatic); others are not simulated.
std::optional<physics::Joint1> World::makeUberJoint(int32_t uid, int32_t index) {
    const auto* o = script_.find(uid);
    const auto data = uberData_.find(uid);
    const auto drm = o ? objectDrms_.find(o->name) : objectDrms_.end();
    if (!o || data == uberData_.end() || !data->second || index < 0 || size_t(index) >= data->second->joints.size() ||
        drm == objectDrms_.end() || !drm->second)
        return std::nullopt;
    const auto& u = *data->second;
    const auto& j = u.joints[size_t(index)];
    if ((j.type != 0 && j.type != 2) || j.bodyB < 0) return std::nullopt;
    const auto model = [&](uint32_t m) -> const physics::ObjectPhysics* {
        auto it = modelPhysics_.find({o->name, m});
        if (it == modelPhysics_.end()) {
            std::shared_ptr<const physics::ObjectPhysics> p;
            try { p = std::make_shared<physics::ObjectPhysics>(physics::loadObjectPhysics(*drm->second, m)); } catch (const cdc::FormatError&) {}
            it = modelPhysics_.emplace(std::pair{o->name, m}, std::move(p)).first;
        }
        return it->second.get();
    };
    const auto* frame = model(j.model);
    const auto* body = model(u.sections[size_t(j.bodyB)].model);
    if (!frame || !body || j.marker >= frame->markers.size()) return std::nullopt;
    const auto& mk = frame->markers[j.marker];
    const vec3 axis = glm::mat3(glm::rotate(glm::mat4(1), mk.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), mk.rot.y, {0, 1, 0}) *
                                glm::rotate(glm::mat4(1), mk.rot.z, {0, 0, 1})) * vec3(0, 0, 1);
    const auto mp = physics::massProps(*body);
    const glm::mat3 intro(introModel(*o));
    const vec3 d = mp.com - mk.pos; // model space
    physics::Joint1 out;
    out.hinge = j.type == 0;
    out.mass = mp.mass;
    out.axis = glm::normalize(intro * axis);
    out.arm = intro * d;
    out.pivot = vec3(introModel(*o) * glm::vec4(mk.pos, 1));
    out.inertia = glm::dot(axis, mp.inertia * axis) + mp.mass * (glm::dot(d, d) - glm::dot(d, axis) * glm::dot(d, axis));
    const float unit = out.hinge ? glm::radians(1.0f) : 1.0f;
    out.limited = j.flags & 1, out.lo = j.lo * unit, out.hi = j.hi * unit;
    out.motor = j.flags & 2, out.speed = j.speed * unit / 30.0f, out.force = j.force;
    return out;
}

// The simulated joint whose body is the section using model 0 (the object's collision), -1 none. Not while the object
// has a travel clip, or moves as a loose / carried body (its script offset then poses it).
int32_t World::uberModelJoint(int32_t uid) const {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    const auto* o = script_.find(uid);
    if (!o || o->loose || o->offset != vec3(0) || rt == uberRuntime_.end() || data == uberData_.end() || !data->second ||
        uberMotion_.contains(uid))
        return -1;
    for (const auto& [index, j] : rt->second.joints)
        if (const int32_t body = data->second->joints[size_t(index)].bodyB; body >= 0 && data->second->sections[size_t(body)].model == 0)
            return index;
    return -1;
}

// World motion of model 0 riding its joint (tomb doors, levers, pushed towers): the translation along / rotation
// about the joint axis.
std::optional<glm::mat4> World::uberJointPose(int32_t uid) const {
    const int32_t index = uberModelJoint(uid);
    if (index < 0) return std::nullopt;
    const auto& j = uberRuntime_.at(uid).joints.at(index);
    if (!j.hinge) return glm::translate(glm::mat4(1), j.axis * j.x);
    return glm::translate(glm::mat4(1), j.pivot) * glm::mat4_cast(glm::angleAxis(j.x, j.axis)) * glm::translate(glm::mat4(1), -j.pivot);
}

// Lara pushes / pulls an uber object whose model 0 rides a level prismatic joint (l7 Thor towers 8398-8400, "pu":
// pushed out into their watched range they lock, "block_pushed"). As push blocks (src/script): interact held within
// reach of its collision grips it, the stick moves it along the joint axis at the push-block speed and she follows
// (World::tick). [I] Lara's push overrides the joint's motor and gravity (tru.exe: Lara's push animation drives the
// body); ponytail: no push clips, the axis must be within 45 degrees of her facing.
void World::pushUberJoints(const Actions& a) {
    constexpr float kReach = 120, kSpeed = 120; // units, units per second (Script.cpp kBlockSpeed)
    uberGrip_ = false, uberPush_ = {};
    if (!a.interact) return;
    const auto& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    for (auto& [uid, rt] : uberRuntime_) {
        const int32_t index = uberModelJoint(uid);
        if (index < 0) continue;
        auto& j = rt.joints.at(index);
        if (j.hinge || std::abs(j.axis.z) > 0.3f) continue;
        vec3 lo(1e30f), hi(-1e30f); // world box of its collision
        for (const auto& d : col_.dynamics)
            if (d.id == uid)
                for (const vec3& v : d.shape->vertices) {
                    const vec3 w(d.transform * glm::vec4(v, 1));
                    lo = glm::min(lo, w), hi = glm::max(hi, w);
                }
        const vec3 near = glm::clamp(s.pos, lo, hi);
        if (lo.x > hi.x || glm::length(vec2(near - s.pos)) > kReach || s.pos.z < lo.z - 50 || s.pos.z > hi.z) continue;
        uberGrip_ = true;
        const float along = glm::dot(fwd, j.axis) * (a.move.y > 0 ? 1.0f : -1.0f);
        if (std::abs(a.move.y) > 0.5f && std::abs(along) > 0.7071f)
            j.push = (along > 0 ? kSpeed : -kSpeed) / 30.0f, uberPush_ = {uid, index, j.x};
        return;
    }
}

// One physics step of an object's joints (links: the second joint follows the first), then its range watches
// (0x4937A0): a value entering [lo, hi) sends trigger 14, leaving it 15, to the watching section's current state
// (0x4D5B10 -> 0x4D5A50; ignored during a transition). Mode 0 watches the position (units / degrees), 1 the velocity
// per second.
void World::tickUberJoints(int32_t uid) {
    auto& rt = uberRuntime_[uid];
    const auto& data = *uberData_.at(uid);
    std::map<int32_t, std::vector<std::pair<physics::Joint1*, float>>> linked;
    std::set<int32_t> followers;
    for (const int32_t l : rt.links) {
        const auto& lk = data.joints[size_t(l)];
        const auto a = rt.joints.find(lk.link1), b = rt.joints.find(lk.link2);
        if (a == rt.joints.end() || b == rt.joints.end()) continue;
        // 0x4D7ED0: degrees per unit when a prismatic joint drives a hinge
        const float ratio = !a->second.hinge && b->second.hinge ? glm::radians(lk.ratio) : lk.ratio;
        linked[lk.link1].push_back({&b->second, ratio}), followers.insert(lk.link2);
    }
    constexpr float kFrames = kTickSeconds * 30.0f;
    for (auto& [index, j] : rt.joints)
        if (!followers.contains(index)) physics::stepJoint(j, linked[index], kFrames);
    for (auto& w : rt.watches) {
        const auto j = rt.joints.find(w.joint);
        if (j == rt.joints.end() || w.mode > 1) continue;
        const float unit = j->second.hinge ? glm::degrees(1.0f) : 1.0f;
        const float value = (w.mode == 0 ? j->second.x : j->second.v * 30.0f) * unit;
        const auto& range = data.params[size_t(w.range)];
        const bool in = range.a <= value && value < range.b;
        if (in == w.inside) continue;
        w.inside = in;
        const auto& s = rt.sections[size_t(w.section)];
        const auto& sec = data.sections[size_t(w.section)];
        if (s.transition >= 0 || s.state < 0 || size_t(s.state) >= sec.stateOut.size()) continue;
        for (const uint32_t ti : sec.stateOut[size_t(s.state)]) {
            const auto& t = sec.transitions[ti];
            if (t.trigger == (in ? 14u : 15u) && t.param == w.mode && t.joint == w.joint && t.range == w.range) {
                uberBegin(uid, size_t(w.section), ti);
                break;
            }
        }
    }
}

// Start transition `transition` (0x4D4A00): leaves the state (event 2), starts the transition (3). With flags 1 + 2
// and an animation it lasts the clip, else it ends on the next update.
void World::uberBegin(int32_t uid, size_t section, uint32_t transition) {
    const auto& data = *uberData_.at(uid);
    const auto& t = data.sections[section].transitions[transition];
    auto& rt = uberRuntime_[uid];
    auto& s = rt.sections[section];
    const int32_t left = s.state;
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
                // Triggers 14 / 15 come from the joint simulation (tickUberJoints).
                const bool go = t.trigger == 3 || (t.trigger == 1 && s.time > std::bit_cast<float>(t.param) - 0.001f) ||
                                (t.trigger == 9 && uberHolds(uid, t.conditions));
                if (go) { uberBegin(uid, i, ti); break; }
            }
        }
        tickUberJoints(uid);
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

// A bullet's impulse on the object's watched joints, at their centre of mass (tru.exe: Havok applies it to the struck
// body; 0x4D5B10 then sees the joint leave its range): l1_kraken_room gear 32's column (section 12, braked by a 79
// motor) slides out and takes trigger 15 before switch 57 turns the gear. ponytail: the struck section is not resolved
// (every watched joint takes the impulse) and kShotImpulse [I] is not tru.exe's bullet value.
void World::uberShot(int32_t uid, vec3 dir) {
    constexpr float kShotImpulse = 3000.0f; // mass * units per frame
    const auto rt = uberRuntime_.find(uid);
    if (rt == uberRuntime_.end()) return;
    for (auto& [index, j] : rt->second.joints) {
        if (std::ranges::none_of(rt->second.watches, [&](const UberWatch& w) { return w.joint == index; })) continue;
        const vec3 d = glm::length(dir) > 0 ? glm::normalize(dir) : j.axis;
        if (j.effectiveMass() <= 0) continue;
        j.v += (j.hinge ? glm::dot(glm::cross(j.arm, d), j.axis) : glm::dot(d, j.axis)) * kShotImpulse / j.effectiveMass();
    }
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
        if (function == 0) { // query (0x4AB6A0): its condition list on the current section states
            result = command < data->queries.size() && uberHolds(uid, data->queries[command]);
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
// 27 GuardNode(unitref, noderef, r1, r2) (0x777210 -> 0x61F7A0): a guard zone at planner node {record, node} of the
// unit (0x5C8EA0), inner r1, outer max(r1, r2) (Script passes the noderef as record << 16 | node).
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
        if (function == 27 && args.size() == 4) {
            enemies_.guard(uid, args[1].bits >> 16, args[1].bits & 0xFFFF, float(int32_t(args[2].bits)), float(int32_t(args[3].bits)));
            return true;
        }
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
        // Every uber object runs its section components from spawn (0x4D3DA0), as the l7 Thor towers' joints extend.
        const formats::UberObjectData* uber = prepareUber(o);
        if (!ordinary && !uber && !o.spline && !lateCollider(o)) continue; // spline movers' collision follows Object::body (pose below)
        if (o.kind == script::Kind::Switch && !uber &&
            (!o.script || !o.script->program->mainFunction(o.script->typeId))) continue;
        const physics::ObjectPhysics* physics = objectPhysics(o.name);
        if (!physics) continue;
        const physics::ObjectPhysics& p = *physics;
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
        } else if (const auto joint = uberJointPose(c.uid)) {
            model = glm::inverse(c.intro) * *joint * c.intro;
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
        // Its collision's base 40 units above her feet (as carried, the base may sit lower than its origin): it falls
        // from there onto what is below (a raised plate top included).
        float base = 0;
        for (const auto& d : col_.dynamics)
            if (d.id == held->uid)
                for (const vec3& v : d.shape->vertices) base = std::min(base, vec3(d.transform * glm::vec4(v, 1)).z - (held->pos.z + held->offset.z));
        held->offset = s.pos + fwd * 90.0f + vec3(0, 0, 40 - base) - held->pos;
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
