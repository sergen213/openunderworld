// World <-> src/physics: collision of scripted objects (doors, push blocks) and rigid-body hinge doors.
// Evidence and inferences: docs/formats/PHYSICS.md.
#include "game/Game.hpp"
#include "script/Vm.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <bit>
#include <chrono>

namespace ou::game {
namespace {
// A rigid body (class 34) whose first model marker is a cylindrical joint (kind 2: l4_ballcourt's ca_rb_wall_hpole_door):
// it slides along the marker's Z within its `multibody` 77 limit 1 (units) instead of swinging [O decomp 0x4A5C40 /
// 0x4A5D90: limit 1 of joint type 2 is linear; I: marker kind = joint type].
bool cylinderDoor(const physics::ObjectPhysics& p) {
    return p.physClass == physics::kClassRigidBody && !p.markers.empty() && p.markers[0].kind == 2;
}

// [I] Opening angle of a rigid-body hinge door. tru.exe couples it to the lever's joint every tick (l4_ballcourt lever
// 6502 fn 2/3: door limit = lever angle + 2 degrees); we do not run that loop and swing the door fully open instead.
constexpr float kDoorOpen = 80.0f * 3.14159265f / 180.0f;

glm::mat4 introModel(const script::Object& o) { // T * Rx * Ry * Rz, as the intros (formats::introMatrix)
    return glm::translate(glm::mat4(1), o.pos) * glm::rotate(glm::mat4(1), o.rot.x, {1, 0, 0}) *
           glm::rotate(glm::mat4(1), o.rot.y, {0, 1, 0}) * glm::rotate(glm::mat4(1), o.rot.z, {0, 0, 1});
}

// Scene keys beside the intro uids: Lara's ragdoll; chunk piece k of enemy `uid` as a loose body.
constexpr int32_t kLaraRagdoll = -1;
int32_t pieceId(int32_t uid, size_t k) { return -(uid * 64 + int32_t(k) + 1); }

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
            auto& rt = uberRuntime_[o.uid];
            rt.sections.assign(data->sections.size(), {});
            for (size_t i = 0; i < rt.sections.size(); ++i)
                rt.sections[i].health = data->sections[i].health, rt.sections[i].engagement = data->sections[i].engagement;
            // The anim references' data (transition +0x28/+0x2A, actions 2 / 3): clips, patterns, model skeletons.
            auto& anims = uberAnims_[o.name];
            if (!anims) {
                auto set = std::make_shared<UberAnims>();
                for (const auto& e : anim::animList(*drm->second)) {
                    auto& clip = set->clips.emplace_back();
                    set->names.push_back(e.name);
                    for (const auto& sec : drm->second->sections)
                        if (sec.type == cdc::SectionType::Animation && sec.id == e.animId) {
                            try { clip = anim::decodeClip(sec.data); } catch (const cdc::FormatError&) {}
                            break;
                        }
                }
                set->patterns = anim::animPatterns(*drm->second);
                const auto models = formats::objectRenderModels(*drm->second);
                std::vector<std::string> warnings;
                for (auto& sk : anim::decodeSkeletons(*drm->second, warnings)) {
                    const auto shared = std::make_shared<const scene::Skeleton>(std::move(sk));
                    for (uint32_t m = 0; m < models.size(); ++m)
                        if (models[m] && models[m] == shared->renderModelId) set->skeletons[m] = shared;
                }
                anims = std::move(set);
            }
            rt.anims = anims;
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
                if (s[i].size() >= 6) sec.health = s[i].at(4), sec.removed = s[i].at(5);
                if (s[i].size() >= 7) sec.engagement = s[i].at(6); // older saves: the section's starting set
                if (s[i].size() >= 11) {
                    sec.clip = s[i].at(7), sec.clipTime = s[i].at(8), sec.rate = s[i].at(9), sec.loop = s[i].at(10);
                    if (!rt.anims || sec.clip >= int32_t(rt.anims->clips.size())) sec.clip = -1;
                    if (s[i].size() >= 12) sec.model = s[i].at(11);
                    else if (sec.state >= 0 && size_t(sec.state) < it->second->sections[i].stateActions.size())
                        for (const auto& a : it->second->sections[i].stateActions[size_t(sec.state)]) // 0.37 saves: the
                            if (a.type == 1) uberAction(o.uid, i, a);                                     // state's model
                } else if (sec.state >= 0 && size_t(sec.state) < it->second->sections[i].stateActions.size()) {
                    for (const auto& a : it->second->sections[i].stateActions[size_t(sec.state)]) // older saves: the state's
                        if (a.type == 1 || a.type == 2 || a.type == 3) uberAction(o.uid, i, a);  // own clip and model
                }
                if (sec.state >= 0) uberStates_[o.uid][i] = uint32_t(sec.state);
            }
            rt.fired = j.at("fired"), rt.commanded = j.at("commanded"), rt.intangible = j.value("intangible", false);
            rt.grappleOff = j.value("grappleOff", false);
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
        for (const auto& s : rt.sections)
            sections.push_back({s.state, s.transition, s.time, s.length, s.health, s.removed, s.engagement, s.clip, s.clipTime, s.rate, s.loop, s.model});
        nlohmann::json& e = uber[std::to_string(uid)] = {{"sections", std::move(sections)}, {"fired", rt.fired}, {"commanded", rt.commanded},
                                                         {"intangible", rt.intangible}, {"grappleOff", rt.grappleOff}};
        nlohmann::json joints = nlohmann::json::array(), watches = nlohmann::json::array();
        for (const auto& [index, jt] : rt.joints)
            joints.push_back({index, jt.x, jt.v, jt.lo, jt.hi, jt.limited, jt.speed, jt.force, jt.motor});
        for (const auto& w : rt.watches) watches.push_back({w.section, w.joint, w.range, w.mode, w.inside});
        e["joints"] = std::move(joints), e["links"] = rt.links, e["watches"] = std::move(watches);
    }
    // The physics scene (src/physics Scene::save, hex): a load continues the simulation bit for bit. Older saves lack it.
    std::string physics;
    for (const unsigned char c : physics_.save()) physics += "0123456789abcdef"[c >> 4], physics += "0123456789abcdef"[c & 15];
    nlohmann::json inserted = nlohmann::json::array(), slides = nlohmann::json::array();
    nlohmann::json insertedSections = nlohmann::json::array(); // uber receptacles' sections (0.36)
    for (const auto& [uid, in] : inserted_) {
        inserted.push_back({uid, in.receptacle});
        if (in.section >= 0) insertedSections.push_back({uid, in.section});
    }
    for (const auto& [uid, x] : slides_) slides.push_back({uid, x});
    return {{"uber", std::move(uber)}, {"held", held_}, {"inserted", std::move(inserted)}, {"slides", std::move(slides)},
            {"insertedSections", std::move(insertedSections)}, {"placed", placed_}, {"breakables", serializeBreakables()},
            {"physics", {{"version", 1}, {"state", std::move(physics)}}}};
}

void World::restoreObjects(const nlohmann::json& j) {
    std::map<int32_t, nlohmann::json> saved;
    int32_t held = 0;
    std::map<int32_t, Inserted> inserted;
    std::map<int32_t, float> slides;
    std::set<int32_t> placed;
    std::string physicsState;
    if (j.is_object()) {
        try {
            const nlohmann::json uber = j.value("uber", nlohmann::json::object());
            for (const auto& [key, e] : uber.items()) {
                const auto& s = e.at("sections");
                for (const auto& sec : s) // validate the shape now: prepareUber reads it later (older saves: 4 / 5 entries)
                    if (!sec.is_array() || sec.size() < 4 || !sec[0].is_number_integer() || !sec[1].is_number_integer() ||
                        !sec[2].is_number() || !sec[3].is_number() || (sec.size() >= 6 && (!sec[4].is_number() || !sec[5].is_boolean())) ||
                        (sec.size() >= 7 && !sec[6].is_number_integer()) ||
                        (sec.size() >= 11 && (!sec[7].is_number_integer() || !sec[8].is_number() || !sec[9].is_number() || !sec[10].is_boolean())) ||
                        (sec.size() >= 12 && !sec[11].is_number_integer()))
                        throw std::runtime_error("bad uber section of " + key);
                (void)e.at("fired").get<uint64_t>(), (void)e.at("commanded").get<bool>(), (void)e.value("intangible", false);
                (void)e.value("grappleOff", false);
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
            for (const auto& e : j.value("inserted", nlohmann::json::array())) { // older saves: none
                inserted[e.at(0).get<int32_t>()] = {e.at(1).get<int32_t>()};
                if (e.size() >= 5) slides[e.at(1).get<int32_t>()] = e.at(4).get<float>(); // 0.34.0: the slide rode along
            }
            for (const auto& e : j.value("insertedSections", nlohmann::json::array()))
                if (const auto it = inserted.find(e.at(0).get<int32_t>()); it != inserted.end()) it->second.section = e.at(1).get<int>();
            for (const auto& e : j.value("slides", nlohmann::json::array())) slides[e.at(0).get<int32_t>()] = e.at(1).get<float>();
            placed = j.value("placed", std::set<int32_t>{});
            restoreBreakables(j.value("breakables", nlohmann::json::array())); // older saves: every breakable whole
            if (const auto p = j.find("physics"); p != j.end() && p->value("version", 0) == 1) {
                const std::string hex = p->at("state").get<std::string>();
                if (hex.size() % 2) throw std::runtime_error("odd physics state");
                const auto nibble = [](char c) {
                    if (c >= '0' && c <= '9') return c - '0';
                    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                    throw std::runtime_error("bad physics state");
                };
                for (size_t i = 0; i < hex.size(); i += 2) physicsState.push_back(char(nibble(hex[i]) << 4 | nibble(hex[i + 1])));
            }
        } catch (const nlohmann::json::exception& e) {
            throw std::runtime_error(std::string("objects: ") + e.what());
        } catch (const std::logic_error& e) {
            throw std::runtime_error(std::string("objects: ") + e.what());
        }
    } else {
        restoreBreakables({});
    }
    uberData_.clear(), uberRuntime_.clear(), uberStates_.clear(), uberEvents_.clear(), plates_.clear();
    uberSaved_ = std::move(saved);
    held_ = held;
    inserted_ = std::move(inserted);
    slides_ = std::move(slides);
    // Saves from before placements: what is inserted now was placed (or Lara put it there).
    placed_ = j.is_object() && j.contains("placed") ? std::move(placed) : std::set<int32_t>{};
    if (!(j.is_object() && j.contains("placed")))
        for (const auto& [uid, in] : inserted_) placed_.insert(uid);
    // The saved scene's IDs and recorded state (applied once its bodies exist again); older saves: loose bodies start
    // again from their saved offset and rotation.
    if (physicsState.empty() || !physics_.restore(physicsState)) physics_.setLoose({});
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

// An anim reference on the section instance's anim component (instance +0x280 -> +0x14; 0x444BA0): kind 0 builds a
// one-animation pattern (0x4321D0: the animation-list index, rate 1), kind 1 indexes the object's patterns and plays
// its first animation at its rate (0x438C40, pattern +0x14 / 4096). Then the caller sets the loop bit: transitions and
// action 2 clear it (0x438BC0: once, the last frame holds), action 3 sets it and restarts at 0 (0x438B50). Index 0xFFFF
// plays nothing. [I] Patterns of several animations blend them by weight; every pattern in the data has one.
float World::uberPlay(int32_t uid, size_t section, uint32_t kind, uint32_t index, bool loop) {
    auto& rt = uberRuntime_[uid];
    if (!rt.anims || section >= rt.sections.size() || index == 0xFFFF) return 0;
    int32_t clip = -1;
    float rate = 1;
    if (kind == 0 && index < rt.anims->clips.size()) clip = int32_t(index);
    else if (kind == 1 && index < rt.anims->patterns.size() && !rt.anims->patterns[index].anims.empty() &&
             rt.anims->patterns[index].anims[0] < rt.anims->clips.size())
        clip = rt.anims->patterns[index].anims[0], rate = rt.anims->patterns[index].rate;
    if (clip < 0 || rate <= 0 || !rt.anims->clips[size_t(clip)].keyCount) return 0;
    auto& s = rt.sections[section];
    s.clip = clip, s.clipTime = 0, s.rate = rate, s.loop = loop;
    return float(rt.anims->clips[size_t(clip)].durationSeconds()) / rate;
}

// A state's entry actions (0x4D86A0).
void World::uberAction(int32_t uid, size_t section, const formats::UberAction& a) {
    auto& rt = uberRuntime_[uid];
    const auto& data = *uberData_.at(uid);
    const auto joint = rt.joints.find(a.joint);
    const formats::UberParam* p = a.param >= 0 ? &data.params[size_t(a.param)] : nullptr;
    const float unit = joint != rt.joints.end() && joint->second.hinge ? glm::radians(1.0f) : 1.0f; // records: degrees
    switch (a.type) {
    case 1: // set the instance's model (+0x234 = the value's low byte)
        rt.sections[section].model = int32_t(a.value & 0xFF);
        break;
    case 2: // play once (value: u16 kind, u16 index; uberPlay)
    case 3: // play looping
        uberPlay(uid, section, a.value & 0xFFFF, a.value >> 16, a.type == 3);
        break;
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
    case 5: // instance +0x18 |= 0x20000000: collision callbacks skip it (0x48D590, 0x697970) [O]; ours drops its collider
        rt.intangible = true, syncedObjects_ = ~size_t(0);
        break;
    case 0x19: // instance +0x1C &= ~2 (0x4D86A0): a grapple target again
    case 0x1A: // instance +0x1C |= 2: 0x6ECCA0 (the grapple target filter of 0x7131A0) refuses it [O]. The grapple handles'
               // volumes switch them: l2_towerconnector 2038 entered -> 2042 command 1 -> state 1 (0x1A), left -> state 2
        rt.grappleOff = a.type == 0x1A;
        break;
    case 28: // 0x6D5D90 -> 0x6D54F0: the section's removal record (all in the data: one fade-out over 0 s) runs, then the
             // instance goes (0x6D5E20 -> 0x4D6480). ponytail: removed at once; record entries of type 1 (a clip first) and
             // fade times > 0 do not occur.
        rt.sections[section].removed = true, syncedObjects_ = ~size_t(0);
        break;
    case 0x13: // 0x4D3B50: gives birth to the record's object at the section's marker (formats::UberBirth). Ours: the
               // treasure of a reward urn (UnitData::births; a pickup from now on), whatever broke the urn.
        if (a.birth >= 0) {
            born.insert(formats::bornUid(uid, size_t(a.birth)));
            // An effect object with an explosion record (sh_ei_exploading_barrel -> squib_exploding_barrel) goes off
            // where it is born [I: at the object's origin; the record's marker is not placed].
            if (const auto* o = script_.find(uid); o && size_t(a.birth) < data.births.size())
                birthBlast(data.births[size_t(a.birth)].object, o->pos + o->offset);
        }
        break;
    case 0x1D: // 0x6D5EC0: section +0xD4 = the action's set, where Lara engages the section (query 0x53, 0x6D59C0)
        rt.sections[section].engagement = a.engagement;
        break;
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
// moves as a carried / free loose body (its script offset then poses it), or is a pressure plate:
// the l4_traprooms empty thrones' prismatic uber joint 0 is the `multibody` joint their scripts watch (one Havok
// constraint [I]); the plate's Jolt body (plates_, tickJoints) carries the weight on it and poses it.
int32_t World::uberModelJoint(int32_t uid) const {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    const auto* o = script_.find(uid);
    if (!o || o->offset != vec3(0) || rt == uberRuntime_.end() || data == uberData_.end() || !data->second ||
        plates_.contains(uid))
        return -1;
    for (const auto& [index, j] : rt->second.joints)
        if (const int32_t body = data->second->joints[size_t(index)].bodyB; body >= 0 && data->second->sections[size_t(body)].model == 0)
            return index;
    return -1;
}

namespace {
// World motion of a body riding simulated joint `j`: the translation along / rotation about its axis.
glm::mat4 jointMotion(const physics::Joint1& j) {
    if (!j.hinge) return glm::translate(glm::mat4(1), j.axis * j.x);
    return glm::translate(glm::mat4(1), j.pivot) * glm::mat4_cast(glm::angleAxis(j.x, j.axis)) * glm::translate(glm::mat4(1), -j.pivot);
}
} // namespace

// World motion of model 0 riding its joint (tomb doors, levers, pushed towers).
std::optional<glm::mat4> World::uberJointPose(int32_t uid) const {
    const int32_t index = uberModelJoint(uid);
    if (index < 0) return std::nullopt;
    return jointMotion(uberRuntime_.at(uid).joints.at(index));
}

// [I] A joint whose other body is a moving section (ms_rb_grapple_snake_head joint 2) is posed as if that body stood.
std::optional<glm::mat4> World::uberSectionBody(int32_t uid, int32_t section, bool* removed) const {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    const auto* o = script_.find(uid);
    if (removed) *removed = rt != uberRuntime_.end() && section >= 0 && size_t(section) < rt->second.sections.size() &&
                            rt->second.sections[size_t(section)].removed;
    if (!o || rt == uberRuntime_.end() || data == uberData_.end() || !data->second) return std::nullopt;
    for (const auto& [index, j] : rt->second.joints)
        if (data->second->joints[size_t(index)].bodyB == section) {
            const glm::mat4 intro = introModel(*o);
            return glm::inverse(intro) * jointMotion(j) * intro;
        }
    return std::nullopt;
}

// The section instance's skeleton (its current model's, state action 1) at its anim component's clip, world matrices in
// model space; null skeleton while it stands in the bind pose.
std::pair<const scene::Skeleton*, std::vector<glm::mat4>> World::uberSectionMatrices(int32_t uid, int32_t section) const {
    const auto rt = uberRuntime_.find(uid);
    if (rt == uberRuntime_.end() || !rt->second.anims || section < 0 || size_t(section) >= rt->second.sections.size()) return {};
    const auto& s = rt->second.sections[size_t(section)];
    const auto sk = rt->second.anims->skeletons.find(uint32_t(uberSectionModel(uid, section)));
    if (s.clip < 0 || sk == rt->second.anims->skeletons.end()) return {};
    const auto& skeleton = *sk->second;
    return {&skeleton, anim::worldMatrices(skeleton, anim::sample(rt->second.anims->clips[size_t(s.clip)], skeleton, s.clipTime, s.loop))};
}

std::optional<std::vector<glm::mat4>> World::uberSectionPose(int32_t uid, int32_t section) const {
    const auto [skeleton, world] = uberSectionMatrices(uid, section);
    if (!skeleton) return std::nullopt;
    return anim::skinPalette(*skeleton, world);
}

int32_t World::uberSectionModel(int32_t uid, int32_t section, int32_t* record) const {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (record) *record = -1;
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second || section < 0 ||
        size_t(section) >= rt->second.sections.size() || size_t(section) >= data->second->sections.size())
        return -1;
    const int32_t own = int32_t(data->second->sections[size_t(section)].model);
    if (record) *record = own;
    const int32_t now = rt->second.sections[size_t(section)].model;
    return now >= 0 ? now : own;
}

// The object's own mesh is its first section on model 0 (formats::loadLevelScene).
std::set<int32_t> World::switchedObjects() const {
    std::set<int32_t> out;
    for (const auto& [uid, rt] : uberRuntime_)
        if (const auto d = uberData_.find(uid); d != uberData_.end() && d->second)
            for (size_t i = 0; i < rt.sections.size() && i < d->second->sections.size(); ++i)
                if (d->second->sections[i].model == 0) {
                    if (rt.sections[i].model > 0) out.insert(uid);
                    break;
                }
    return out;
}

// Lara's side of a switch (docs/GAMEPLAY.md "Interactions"). [O] The switch's DRM carries her clip (Object::laraClip,
// AnimLibrary "use:<id>"), without root motion; the use handler still runs when the switch's own clip ends (src/script).
// [I] She stands useDistance in front of it on her side of the switch's local horizontal axis she is most along, facing
// it. Switches without a clip of their own (l4_ballcourt pole lever 6502) play the button press as she stands.
void World::useSwitchClip(const script::Object& o) {
    const auto& s = player_.state();
    const std::string own = "use:" + std::to_string(o.laraClip);
    if (!o.laraClip || !anims_.find(own)) {
        player_.use("use:2441", s.pos, s.yaw);
        return;
    }
    const vec3 lx(std::cos(o.rot.z), std::sin(o.rot.z), 0), ly(-lx.y, lx.x, 0), d = s.pos - o.pos;
    vec3 out = std::abs(glm::dot(d, lx)) > std::abs(glm::dot(d, ly)) ? lx : ly;
    if (glm::dot(d, out) < 0) out = -out;
    const float distance = o.useDistance > 20 && o.useDistance < 500 ? o.useDistance : 135.0f; // [I] the wall switch's
    player_.use(own, vec3(vec2(o.pos + out * distance), s.pos.z), std::atan2(-out.y, -out.x));
}

// Lara pushes / pulls an uber object whose model 0 rides a level prismatic joint (l7 Thor towers 8398-8400, "pu":
// pushed out into their watched range they lock, "block_pushed"). As push blocks (src/script): interact held within
// reach of its collision grips it, the stick moves it along the joint axis at the push-block speed and she follows
// (World::tick). [I] Lara's push overrides the joint's motor and gravity (tru.exe: Lara's push animation drives the
// body); she plays the push-block clips (Player::grip) facing the axis, which must be within 45 degrees of her facing.
void World::pushUberJoints(const Actions& a) {
    constexpr float kReach = 120, kSpeed = 120; // units, units per second (Script.cpp kBlockSpeed)
    uberGrip_ = vec3(0), uberPush_ = {};
    if (pushUberHandle(a, kSpeed) || !a.interact) return;
    const auto& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    for (auto& [uid, rt] : uberRuntime_) {
        const int32_t index = uberModelJoint(uid);
        if (index < 0) continue;
        if (const auto* o = script_.find(uid); o && o->kind == script::Kind::Block) continue; // src/script moves those
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
        uberGrip_ = glm::normalize(vec3(j.axis.x, j.axis.y, 0));
        if (glm::dot(fwd, uberGrip_) < 0) uberGrip_ = -uberGrip_;
        const float along = glm::dot(fwd, j.axis) * (a.move.y > 0 ? 1.0f : -1.0f);
        if (std::abs(a.move.y) > 0.5f && std::abs(along) > 0.7071f)
            j.push = (along > 0 ? kSpeed : -kSpeed) / 30.0f, uberPush_ = {uid, index, j.x};
        return;
    }
}

// Uber section `section`'s first transition out of its state with trigger `trigger` and parameter `param` starts
// (0x6D4790 / 0x6D4810: an idle section, not removed). [I] The transition's condition check (0x4D0E10) is not run.
bool World::uberTrigger(int32_t uid, size_t section, uint32_t trigger, uint32_t param) {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second || section >= rt->second.sections.size()) return false;
    const auto& s = rt->second.sections[section];
    const auto& sec = data->second->sections[section];
    if (s.removed || s.transition >= 0 || s.state < 0 || size_t(s.state) >= sec.stateOut.size()) return false;
    for (const uint32_t ti : sec.stateOut[size_t(s.state)])
        if (sec.transitions[ti].trigger == trigger && sec.transitions[ti].param == param) {
            uberBegin(uid, section, ti);
            return true;
        }
    return false;
}

// The simulated hinge / prismatic joint moving uber section `section`: the one whose body B it is, else (following
// fixed joints, type 8, from their body B to body A) the one moving the section it is fixed to; -1 none. ca_uo_calendar:
// the handles (section 1) are fixed (joint 6) to the ring (section 5), which turns on hinge joint 2.
int32_t World::uberSectionJoint(int32_t uid, int32_t section) const {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second) return -1;
    const auto& joints = data->second->joints;
    for (int depth = 0; depth < 8 && section >= 0; ++depth) {
        for (const auto& [index, j] : rt->second.joints)
            if (joints[size_t(index)].bodyB == section) return index;
        int32_t next = -1;
        for (const auto& j : joints)
            if (j.type == 8 && j.bodyB == section) next = j.bodyA;
        section = next;
    }
    return -1;
}

// Lara's push / pull records (kind 1: ca_uo_calendar(_b)'s push handles, section 1 set 528 records at segments 10 / 20,
// section 0's set 540 once the headdress is in). tru.exe: interact inside one puts her into her PushPull state, whose
// entry posts INSTANCE message 0x158 to the engaged instance (0x701310 with value 1; another state posts it with 3); the
// uber section (0x6D5AC0 case 0x158: Lara's engaged record must be in its current set, section idle) starts the
// transition with trigger 20 and parameter = the record's kind (0x6D4790; calendar section 1: 3 -> 1); letting go posts
// 0x14F (0x701350): trigger 21 (0x6D4810; section 1: 1 -> 5, which locks the ring's hinge, then back to 3 after 1.5 s).
// While she grips, the stick pushes / pulls the joint moving the section ([I] at the push-block speed along her
// facing: a hinge turns so that her grip point moves at it, a prismatic joint slides along its horizontal axis), and
// she moves with it; she faces along the push (Player::grip). [I] The PushPull proxy body (0x7358E0 "PushPull Proxy")
// is not simulated, nor the record kind 3 (0x6D5AC0 also posts game-tracker message 0x1D for it). True: Lara grips one.
bool World::pushUberHandle(const Actions& a, float speed) {
    const auto& s = player_.state();
    if (uberHandle_.uid && (!a.interact || held_ || (s.state != State::Block && !player_.canInteract()))) {
        uberTrigger(uberHandle_.uid, size_t(uberHandle_.section), 21, uint32_t(uberHandle_.kind));
        uberHandle_ = {};
    }
    if (!uberHandle_.uid && a.interact && !interactHeld_ && !held_ && !script_.holding() && player_.canInteract()) {
        const Engaged e = engaged();
        const auto* o = e.uid ? script_.find(e.uid) : nullptr;
        if (e.record && e.section >= 0 && e.record->kind == 1 && o && o->kind != script::Kind::Block) { // src/script pushes blocks
            uberHandle_ = {e.uid, e.section, e.record->kind};
            uberTrigger(e.uid, size_t(e.section), 20, uint32_t(e.record->kind));
        }
    }
    if (!uberHandle_.uid) return false;
    const int32_t index = uberSectionJoint(uberHandle_.uid, uberHandle_.section);
    auto rt = uberRuntime_.find(uberHandle_.uid);
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    if (index < 0 || rt == uberRuntime_.end()) {
        uberGrip_ = fwd; // gripped, nothing moves
        return true;
    }
    auto& j = rt->second.joints.at(index);
    vec3 along = j.hinge ? glm::cross(j.axis, s.pos - j.pivot) : j.axis; // her grip point's motion per unit of the joint
    along.z = 0;
    const float rate = glm::length(along);
    if (rate < 1e-3f) {
        uberGrip_ = fwd;
        return true;
    }
    uberGrip_ = along / rate;
    if (glm::dot(fwd, uberGrip_) < 0) uberGrip_ = -uberGrip_;
    if (std::abs(a.move.y) > 0.5f) {
        const float sign = (glm::dot(uberGrip_, along) > 0 ? 1.0f : -1.0f) * (a.move.y > 0 ? 1.0f : -1.0f);
        j.push = sign * speed / rate / 30.0f;
        uberPush_ = {uberHandle_.uid, index, j.x};
    }
    return true;
}

// Range watches (0x4937A0) on the joint positions of the last physics step (World::stepPhysics): a value entering
// [lo, hi) sends trigger 14, leaving it 15, to the watching section's current state (0x4D5B10 -> 0x4D5A50; ignored
// during a transition). Mode 0 watches the position (units / degrees), 1 the velocity per second.
void World::uberWatches(int32_t uid) {
    auto& rt = uberRuntime_[uid];
    const auto& data = *uberData_.at(uid);
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

// Start transition `transition` (0x4D4A00): leaves the state (event 2), starts the transition (3). With flags 1 its
// animation plays once on the section instance; with flags 2 too it lasts the clip, else it ends on the next update.
// [U] instance +0x18 bit 0 suppresses the animation; never seen set on a section.
void World::uberBegin(int32_t uid, size_t section, uint32_t transition) {
    const auto& data = *uberData_.at(uid);
    const auto& t = data.sections[section].transitions[transition];
    auto& rt = uberRuntime_[uid];
    auto& s = rt.sections[section];
    const int32_t left = s.state;
    s.state = -1, s.transition = int32_t(transition), s.time = 0, s.length = 0;
    if ((t.flags & 1) && t.anim >= 0) {
        const float length = uberPlay(uid, section, t.animKind, uint32_t(t.anim), false);
        if (t.flags & 2) s.length = length;
    }
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
            if (s.clip >= 0 && rt.anims) { // its anim component: a looping clip wraps, one played once holds its last frame
                const float d = float(rt.anims->clips[size_t(s.clip)].durationSeconds());
                s.clipTime += dt * s.rate;
                s.clipTime = s.loop && d > 0 ? std::fmod(s.clipTime, d) : std::min(s.clipTime, d);
            }
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
                                (t.trigger == 9 && uberHolds(uid, t.conditions)) ||
                                (t.trigger == 18 && sec.takesDamage && s.health <= std::bit_cast<float>(t.param) + 0.001f); // 0x6D49D0
                if (go) { uberBegin(uid, i, ti); break; }
            }
        }
    }
    const auto physicsStart = std::chrono::steady_clock::now();
    stepPhysics(dt);
    tickTimes.physicsMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - physicsStart).count();
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
        const float m = j.hinge ? j.inertia : j.mass; // the joint's one degree of freedom: impulse at the centre of mass
        if (m <= 0) continue;
        j.v += (j.hinge ? glm::dot(glm::cross(j.arm, d), j.axis) : glm::dot(d, j.axis)) * kShotImpulse / m; // Scene picks it up
    }
}

// 0x6D4690, per section with +0x52 bit 0: health -= damage (floored at 0); an idle section in a state then starts its
// first transition of trigger 17 whose parameter the damage exceeds, or 19 whose parameter is the damage class. The
// hit message reaches every instance the area hits (0x69F1E0) or the bullet (World::fire) strike. [I] every section of
// the object takes it (the urns and barrels have one).
void World::damageObject(int32_t uid, float damage, int damageClass) {
    if (hitBreakable(uid, damageClass, true)) return; // class 30: its hit count (0x6D3160)
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second) return;
    for (size_t i = 0; i < rt->second.sections.size(); ++i) {
        const auto& sec = data->second->sections[i];
        auto& s = rt->second.sections[i];
        if (!sec.takesDamage || s.removed) continue;
        s.health = std::max(0.0f, s.health - damage);
        if (s.transition >= 0 || s.state < 0 || size_t(s.state) >= sec.stateOut.size()) continue;
        for (const uint32_t ti : sec.stateOut[size_t(s.state)]) {
            const auto& t = sec.transitions[ti];
            if ((t.trigger == 17 && damage > std::bit_cast<float>(t.param)) || (t.trigger == 19 && uint32_t(damageClass) == t.param)) {
                uberBegin(uid, i, ti);
                break;
            }
        }
    }
}

std::set<int32_t> World::removedObjects() const {
    std::set<int32_t> out;
    for (const auto& [uid, rt] : uberRuntime_)
        if (const auto d = uberData_.find(uid); d != uberData_.end() && d->second)
            for (size_t i = 0; i < rt.sections.size(); ++i)
                if (rt.sections[i].removed && d->second->sections[i].model == 0) out.insert(uid);
    return out;
}

std::vector<World::ObjectClip> World::objectClips() const {
    std::vector<ObjectClip> out;
    for (const auto& o : script_.objects()) {
        if (o.clips && o.clipNames && o.clipNames->size() == 2 && o.state != 0)
            out.push_back({o.uid, o.name, (*o.clipNames)[o.state == 1 ? 0 : 1], o.t, (*o.clips)[o.state == 1 ? 0 : 1].durationSeconds(),
                           o.state == 2, introModel(o)});
        else if (const auto rt = uberRuntime_.find(o.uid); rt != uberRuntime_.end()) {
            // every section instance's anim component running a clip (transitions, state actions 2 / 3; tru.exe runs
            // each instance's animation events, 0x5BA0D0): its events (fx_smash: the urn's shards and sound), in the
            // section's frame (its joint's motion).
            for (size_t i = 0; i < rt->second.sections.size(); ++i) {
                const auto& s = rt->second.sections[i];
                if (s.clip < 0 || !rt->second.anims) continue;
                const double d = rt->second.anims->clips[size_t(s.clip)].durationSeconds();
                if (!s.loop && s.clipTime >= d) continue;
                out.push_back({o.uid, o.name, rt->second.anims->names[size_t(s.clip)], s.clipTime, d, s.loop,
                               introModel(o) * uberSectionBody(o.uid, int32_t(i)).value_or(glm::mat4(1)), int(i)});
            }
        } else if (const auto& g = o.graph; g && g->clip >= 0 && size_t(g->clip) < g->set->names.size()) // anim-graph state /
            out.push_back({o.uid, o.name, g->set->names[size_t(g->clip)], g->t,           // transition clips (fixtures' on / off)
                           g->set->clips[size_t(g->clip)].durationSeconds(), g->loop, introModel(o)});
    }
    return out;
}

std::vector<glm::mat4> World::objectBones(int32_t uid, int section) const {
    if (section >= 0) return uberSectionMatrices(uid, section).second;
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

// ---- scripted water (tru.exe watervolumebase natives -> messages 0x1B3 / 0x1B4, handler 0x6AC340) ----------------
std::pair<collision::Markup*, const LoadedUnit*> World::waterVolume(int32_t uid) {
    for (const auto& u : units_)
        for (size_t i = 0; i < u.data.col.markup.size(); ++i)
            if (const auto& m = u.data.col.markup[i]; m.type == 10 && m.id == uint16_t(uid) && m.points.size() > kWaterIds)
                return {&col_.markup[u.markupBase + i], &u};
    return {nullptr, nullptr};
}

void World::applyWaterState(collision::Markup& m, vec3 offset) const {
    const auto it = waterStates_.find(int32_t(m.id));
    if (it == waterStates_.end()) return;
    const WaterState& s = it->second;
    m.points[1].z = s.surface + offset.z;
    m.points[kWaterSource] = s.sourceAt + offset, m.points[kWaterSink] = s.sinkAt + offset;
    m.points[kWaterFlow].x = s.strength;
    m.points[kWaterIds] = vec3(s.source, s.sink, 0);
}

// Script lengths are 3.1605 units (0x7782D0 / 0x778370: int(v x 3.1605 + 0.5)); GetDepth divides by it (x 0.31640562).
void World::bindWater() {
    script_.setWaterNative([this](int32_t uid, std::string_view type, uint32_t function, std::span<const int32_t> a,
                                  uint32_t& result) { return waterNative(uid, type, function, a, result); });
    // Player-sensor anchors (script::Runtime::tickSensors): an object's origin or one of its HInfo markers.
    script_.setAnchorAt([this](int32_t uid, int32_t marker) -> std::optional<vec3> {
        const script::Object* o = script_.find(uid);
        if (!o) return std::nullopt;
        if (marker >= 0)
            if (const auto m = markerFrame(*o, marker)) return vec3((*m)[3]);
        return vec3((glm::translate(glm::mat4(1), o->offset) * introModel(*o))[3]);
    });
}

bool World::waterNative(int32_t uid, std::string_view type, uint32_t function, std::span<const int32_t> a, uint32_t& result) {
    // 6 (0x777A70: INSTANCE_Query(Lara, 0xF8, instance) -> 0x709FD0 case 0xF8): the largest contact force between her
    // and the instance's body (her contact list, -(entry +0x30) where the body matches), rounded; 0 without contact.
    // l4_traprooms crushers 5885-5890 kill her above 30. [I] Lara is no rigid body here: how deep (units) its
    // collision reaches into her capsule, which only grows while it pins her against something.
    if (type == "larabase" && function == 6) {
        const vec3 feet = player_.state().pos;
        const float r = player_.tuning().radius, height = player_.tuning().height;
        float depth = 0;
        for (const float h : {r, height * 0.5f, height - r})
            if (const auto hit = col_.closestPoint(feet + vec3(0, 0, h), r, physics::kObjectMask);
                hit && hit->dynamic >= 0 && col_.dynamics[size_t(hit->dynamic)].id == uid)
                depth = std::max(depth, r - hit->t);
        result = uint32_t(int32_t(std::lround(depth)));
        return true;
    }
    if (type == "larabase") { // 3 (0x777780): Lara is in volume `uid` (her probe box's volume) with its surface over her feet [O Wine: the goo bed kills, the walkways in the same box do not]
        const auto [m, u] = waterVolume(uid);
        const int w = player_.waterAt(player_.state().pos);
        result = m && w >= 0 && &col_.markup[size_t(w)] == m &&
                 waterSurface(*m, vec2(player_.state().pos), double(ticks_) * kTickSeconds) > player_.state().pos.z;
        return true;
    }
    {
        const auto [m, u] = waterVolume(uid);
        if (!m) return false;
        const collision::Markup* data = nullptr;
        for (const auto& d : u->data.col.markup)
            if (d.type == 10 && d.id == m->id) data = &d;
        const float bottom = data->points[0].z, top = data->points[1].z;
        auto [it, fresh] = waterStates_.try_emplace(uid);
        WaterState& s = it->second;
        if (fresh) {
            s.surface = s.target = top;
            s.source = int16_t(data->points[kWaterIds].x), s.sink = int16_t(data->points[kWaterIds].y);
            s.strength = data->points[kWaterFlow].x;
            s.sourceAt = data->points[kWaterSource], s.sinkAt = data->points[kWaterSink];
        }
        const auto markerOf = [&](int32_t intro) -> std::optional<uint16_t> { // the intro's +0x52 (0x7781D0)
            for (const auto& i : u->data.markers)
                if (i.uid == intro) return i.marker;
            return std::nullopt;
        };
        const auto markerAt = [&](int32_t marker) -> std::optional<vec3> { // the last intro naming it (0x69D690)
            std::optional<vec3> at;
            for (const auto& i : u->data.markers)
                if (i.marker == marker) at = i.pos;
            return at;
        };
        const auto length = [](int32_t v) { return int32_t(float(v) * 3.1605f + 0.5f); };
        const auto level = [&](float target, float speed) { // 0x1B4: the new rate keeps or turns the motion (0x6AC340)
            const float old = s.rate, diff = target - s.surface;
            s.target = target;
            if (speed == 0) s.rate = diff * 100;
            else if ((speed + old) * diff > 0) s.rate = speed + old;
            else if (diff * speed > 0) s.rate = speed;
            else if (!(old * diff > 0)) s.rate = -speed;
        };
        switch (function) {
        case 0: { // SetFlow(source, sink, strength) -> 0x1B3 when both instances are intros of the unit
            const auto from = markerOf(a[0]), to = markerOf(a[1]);
            if (!from || !to || a.size() < 3) break;
            s.source = int16_t(*from), s.sink = int16_t(*to);
            if (a[2] >= 0) s.strength = float(a[2]);
            if (const auto p = markerAt(s.source)) s.sourceAt = *p;
            if (const auto p = markerAt(s.sink)) s.sinkAt = *p;
            break;
        }
        case 1: { // SetLevel(depth, speed) -> 0x1B4 mode 2: bottom + depth, at most the box top
            if (a.size() < 2) return false;
            int32_t depth = std::max(0, length(a[0]));
            if (bottom + float(depth) > top) depth = int32_t(top - bottom);
            level(bottom + float(depth), float(length(a[1])));
            break;
        }
        case 2: { // SetLevelToMarker(instance, speed) -> 0x1B4 mode 3: the z of the last intro with its marker id
            if (a.size() < 2) return false;
            if (const auto id = markerOf(a[0]))
                if (const auto p = markerAt(*id)) level(p->z, float(length(a[1])));
            break;
        }
        case 3: // SetFlowStrength -> 0x1B3 (-1, -1, strength)
            if (!a.empty() && a[0] >= 0) s.strength = float(a[0]);
            break;
        case 4: result = std::bit_cast<uint32_t>((s.surface - bottom) * 0.31640562f); break;
        default: return false;
        }
        applyWaterState(*m, u->offset);
        return true;
    }
}

// 0x6C10D0: the surface moves rate per 30 Hz frame toward the target, landing on it.
void World::tickWaterStates(float dt) {
    for (auto& [uid, s] : waterStates_) {
        if (s.surface == s.target) continue;
        const float diff = s.target - s.surface, step = s.rate * dt * 30.0f;
        if ((diff > 0 && diff > step) || (diff < 0 && diff < step)) s.surface += step;
        else s.surface = s.target, s.rate = 0;
        if (const auto [m, u] = waterVolume(uid); m) applyWaterState(*m, u->offset);
    }
}

// enemybase (tru.exe bind 0x7B4060): 16/17 Set/Get the AI behaviour index (0x776F10 -> 0x62DE00 selects the
// behaviour list of the enemy data; 0x776F40 reads it back), 22 selects patrol path n of the intro data (0x777010 ->
// 0x63AC40; ai::Enemies::selectPatrol), 26 picks the MoveNav animation variant (0x7771E0, field 0x37C0 read by
// 0x61B0C0). Ours keep the values for Get; our AI has no behaviour lists or variants [U: the effect of 16 / 26].
// 27 GuardNode(unitref, noderef, r1, r2) (0x777210 -> 0x61F7A0): a guard zone at planner node {record, node} of the
// unit (0x5C8EA0), inner r1, outer max(r1, r2) (Script passes the noderef as record << 16 | node).
// 18 sets an AI byte (+0xC34, 0x776F70, then 0x63F7F0) [U]; kept like 16.
// generatorbase (0x7B4B60) and health 1/2 on an enemy's own script go to ai::Enemies.
void World::bindEnemies() {
    // Dormant enemies (formats::dormant) spawn when a script births them (instanceref Birth), before their constructor.
    enemies_.born = [this](int32_t uid) { return script_.born(uid); };
    script_.onBirth = [this](int32_t uid) { enemies_.birth(uid); };
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
            if (function == 22) enemies_.selectPatrol(uid, int32_t(args[0].bits)); // 0x63AC40: patrol path n
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
        // 4 (0x4A7070): 1 while an atom's body is still fixed (body +0x15C bit 0, cleared by Enable 0x4A6F70 -> 0x4D8FD0).
        // Ours: fixed unless simulated, on a hinge or loose [I] (l4_temple floor 5234 polls statue head 5236).
        auto* o = const_cast<script::Object*>(script_.find(uid)); // World owns object motion
        if (function == 4 && args.empty()) {
            result = index < 0 && !(o && o->loose);
            return true;
        }
        // 59 (0x4A93E0): INSTANCE_Post(0x35, joint) breaks a joint (l5_valaskjalf falling bridge 6974 breaks 0, then 1).
        // [I] Ours has one body per object: the first break lets it fall as a loose body.
        if (function == 59 && args.size() == 1) {
            if (o && !o->loose) o->loose = true;
            return true;
        }
        // 89 (0x4AA250): motor `args[1]` (1..3) of joint `args[0]` takes target speed args[2] (joint +0x108 + 8 * motor;
        // 0x4DA160). l1_baldrs_tomb locks 69 / 70 engage at +10 and disengage at -10; tickJoints moves the joint.
        if (function == 89 && args.size() == 3 && args[0].bits == 0 && args[1].bits >= 1 && args[1].bits <= 3) {
            if (!o) return false;
            (o->joint ? *o->joint : o->joint.emplace(script::Object::Joint{})).speed = std::bit_cast<float>(args[2].bits);
            return true;
        }
        // 75 (0x4A9AB0 -> 0x4D9F50): limit `args[1]` (1..3) of joint `args[0]` on / off (joint +0x174 bit 2 << (limit - 1);
        // 0x4E7550 tests only within the joint's [lo, hi] while it is set). Ours have one degree of freedom: limit 1 off
        // frees the hinge (+-pi) and lets a motor run past the joint limit. l5_valaskjalf hpoles 6984 take it off when the
        // jammed gear 6985 turns (vt 54). Limits 2 / 3 index degrees of freedom ours lack [I: no effect].
        if (function == 75 && args.size() == 3) {
            if (args[1].bits - 1u >= 3u) return true; // "Limit index out of range"
            if (args[0].bits != 0 || args[1].bits != 1) return true;
            const bool on = (args[2].bits & 0xFF) != 0;
            if (on) multibodyLimitOff_.erase(uid);
            else multibodyLimitOff_.insert(uid);
            if (index >= 0) {
                const auto lo = multibodyLo_.find(uid), hi = multibodyHi_.find(uid);
                physics_.hinges[index].lo = on ? (lo != multibodyLo_.end() ? lo->second : 0.0f) : -glm::pi<float>();
                physics_.hinges[index].hi = on ? (hi != multibodyHi_.end() ? hi->second : kDoorOpen + 0.02f) : glm::pi<float>();
            }
            return true;
        }
        if (function != 77 && function != 78 && function != 79) return false;
        if (args.empty() || args[0].bits != 0) return false; // one hinge per current rigid-body object
        if (function == 77) {
            if (args.size() != 4 || args[1].bits < 1 || args[1].bits > 3) return false;
            const float lo = glm::radians(std::bit_cast<float>(args[2].bits));
            const float hi = glm::radians(std::bit_cast<float>(args[3].bits));
            // Kept per object too: a rebuilt hinge (syncObjects) and a load (serializeRuntime) take them again.
            if (args[1].bits != 3) {
                if (index >= 0) physics_.hinges[index].lo = lo;
                multibodyLo_[uid] = lo;
            }
            if (args[1].bits != 2) {
                if (index >= 0) physics_.hinges[index].hi = hi;
                multibodyHi_[uid] = hi;
            }
            return true;
        }
        if (function == 78) {
            // GetJointValue (0x79B710 -> 0x4A9BB0 -> 0x4A5D90) by the joint's type (+0x178): a hinge (1) answers limit 1
            // in degrees; a cylindrical joint (2) limit 1 as its linear position in units (0x4DA460, no conversion) and
            // limit 2 its angle in degrees [O decomp]. The pole-hole levers' joints are cylindrical (marker kind 2): l4_ballcourt
            // 6502 reads its slide (args 0, 1) and passes twice it to door 6505's limit 1 (vt 54), a linear limit on the
            // door's own cylindrical joint (0x4A5C40: limit 1 of type 2 taken as is) — the door rises twice the lever's drop.
            float value = index >= 0 ? glm::degrees(physics_.angle(index)) : 0.0f;
            if (const auto sl = slides_.find(uid); index < 0 && sl != slides_.end()) value = sl->second;
            result = std::bit_cast<uint32_t>(value);
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
    shotOnly_.clear(), shotOnlyLocal_.clear();
    physics_.clearHinges(); // uber joints and loose bodies keep simulating
    for (const auto& o : script_.objects()) {
        const bool ordinary = o.kind == script::Kind::Door || o.kind == script::Kind::Block || o.kind == script::Kind::Switch;
        // Every uber object runs its section components from spawn (0x4D3DA0), as the l7 Thor towers' joints extend.
        const formats::UberObjectData* uber = prepareUber(o);
        // Anim graphs with hit-gated transitions (sh_ei_combat_crate_*) collide so Lara's shots reach them (World::fire).
        const bool shootable = o.graph && o.graph->set->graph &&
                               std::ranges::any_of(o.graph->set->graph->transitions, [](const auto& t) { return !t.events.empty(); });
        if (!ordinary && !uber && !o.spline && !lateCollider(o) && !shootable) { // spline movers' collision follows Object::body (pose below)
            // Anim graphs that leave a state on the damage they took (stimulus 0x800 without event ids, posted by
            // Runtime::tickGraph) and do not collide otherwise: their shapes are met by shots only (World::shotRay), at
            // their intro frame [I: l1_kraken_room's chain links hang on platform 27's markers 21 / 20 (uberobjectbase 2
            // + instance 23), which do not move while the links exist].
            if (o.graph && o.graph->set->graph &&
                std::ranges::any_of(o.graph->set->graph->transitions, [](const auto& t) { return (t.mask & formats::kStimulusEvent) != 0; }))
                // ObjectBase oflags bit 0x40 [I: set on the links and on the hit-reacting combat crates, clear on the
                // squib_explosion_detector spheres (38 in l1_kraken_room, 927 units wide), which shots must pass]
                if (const physics::ObjectPhysics* sp = objectPhysics(o.name); sp && (sp->oflags & 0x40))
                    for (int seg = 0; seg < int(sp->segments.size()); ++seg)
                        if (auto shape = physics::solidShape(*sp, seg))
                            shotOnly_.push_back({o.uid, std::move(shape), introModel(o) * sp->segments[seg]}), shotOnlyLocal_.push_back(sp->segments[seg]);
            continue;
        }
        if (o.kind == script::Kind::Switch && !uber &&
            (!o.script || !o.script->program->mainFunction(o.script->typeId))) continue;
        if (const auto rt = uberRuntime_.find(o.uid); rt != uberRuntime_.end() && rt->second.intangible) continue;
        const glm::mat4 intro = introModel(o);
        // Uber sections (0x4D0E50): every section instance is born with its record's model (0x5BA810 with the model
        // index, then 0x496CB0 builds that model's collision), so the sections other than the one drawn as the
        // object's own mesh (its first on model 0, below) collide with their model's solid segments, posed by their
        // joint and clip each tick (tickObjects): lowering ramps, the shadow door's halves, the pole bars.
        if (uber) {
            size_t main = uber->sections.size();
            for (size_t k = 0; k < uber->sections.size(); ++k)
                if (uber->sections[k].model == 0) { main = k; break; }
            for (size_t k = 0; k < uber->sections.size(); ++k) {
                if (k == main) continue;
                const physics::ObjectPhysics* sp = uberSectionPhysics(o, k);
                if (!sp) continue;
                for (int seg = 0; seg < int(sp->segments.size()); ++seg)
                    if (auto shape = physics::solidShape(*sp, seg)) {
                        col_.dynamics.push_back({o.uid, std::move(shape), intro * sp->segments[seg]});
                        colliders_.push_back({o.uid, seg, -1, intro, sp->segments[seg], int(k)});
                    }
            }
        }
        const physics::ObjectPhysics* physics = objectPhysics(o.name);
        if (!physics) continue;
        const physics::ObjectPhysics& p = *physics;
        int hinge = -1;
        // [I] Rigid doors (class 34) and joint levers (class 6) swing about the X axis of their first model marker.
        // A receptacle holding an inserted portable slides on its joint instead (tickInserted).
        const bool holding = slides_.contains(o.uid) || script_.receptacleGraph(o.name) ||
                             std::ranges::any_of(inserted_, [&](const auto& e) { return e.second.receptacle == o.uid; });
        if ((o.kind == script::Kind::Door || o.kind == script::Kind::Switch) && !o.clips && !holding && !cylinderDoor(p) &&
            (o.kind == script::Kind::Switch || p.physClass == physics::kClassRigidBody) && !p.markers.empty()) {
            if (const auto mp = physics::massProps(p); mp.mass > 0) {
                const auto& m = p.markers[0];
                const glm::mat3 r(glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                                  glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1}));
                hinge = physics_.addHinge(physics_.addBody(mp, intro), m.pos, r * glm::vec3(1, 0, 0), 0, kDoorOpen + 0.02f);
                if (const auto lo = multibodyLo_.find(o.uid); lo != multibodyLo_.end()) physics_.hinges[hinge].lo = lo->second;
                if (const auto hi = multibodyHi_.find(o.uid); hi != multibodyHi_.end()) physics_.hinges[hinge].hi = hi->second;
                if (multibodyLimitOff_.contains(o.uid)) physics_.hinges[hinge].lo = -glm::pi<float>(), physics_.hinges[hinge].hi = glm::pi<float>();
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
    breakableColliders(); // class-30 breakables: their pieces' collision while whole (Breakables.cpp)
    syncedObjects_ = script_.objects().size();
    syncedPlates_ = lateColliders(script_);
    poseShotOnly();
}

// An anchored object (script::Runtime::anchor) sits in the frame of its host's HInfo marker: the marker's segment as
// the host's section on that model is posed now (its clip; the bind pose without one), under the host's intro. [I] the
// marker is looked up on the host's model 0 (uberobjectbase 2's section argument is an unset section id in the data).
void World::poseShotOnly() {
    for (size_t i = 0; i < shotOnly_.size(); ++i) {
        const script::Object* o = script_.find(shotOnly_[i].id);
        if (!o) continue;
        glm::mat4 base = introModel(*o);
        if (const auto a = script_.anchor(o->uid))
            if (const script::Object* host = script_.find(a->first))
                if (const physics::ObjectPhysics* hp = objectPhysics(host->name))
                    for (const auto& m : hp->hmarkers) {
                        if (m.id != a->second) continue;
                        glm::mat4 segment = size_t(m.segment) < hp->segments.size() ? hp->segments[size_t(m.segment)] : glm::mat4(1);
                        if (const auto data = uberData_.find(host->uid); data != uberData_.end() && data->second)
                            for (size_t k = 0; k < data->second->sections.size(); ++k)
                                if (data->second->sections[k].model == 0) {
                                    if (const auto posed = uberSectionMatrices(host->uid, int32_t(k)).second; size_t(m.segment) < posed.size())
                                        segment = posed[size_t(m.segment)];
                                    break;
                                }
                        base = glm::translate(glm::mat4(1), host->offset) * introModel(*host) * segment * glm::translate(glm::mat4(1), m.pos) *
                               glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                               glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1});
                        break;
                    }
        shotOnly_[i].transform = base * shotOnlyLocal_[i];
    }
}

void World::tickObjects(float dt) {
    tickBreakables(dt); // contacts, waits and breaks first: a break drops its colliders before anything queries them
    // Objects came or went, or the merged collision was rebuilt (streaming drops its dynamics).
    if (script_.objects().size() != syncedObjects_ || col_.dynamics.size() != colliders_.size() ||
        lateColliders(script_) != syncedPlates_)
        syncObjects();
    tickUber(dt);
    if (!shotOnly_.empty()) poseShotOnly();
    // The section drawn as model 0 (the object's own mesh, its collision segments) under its clip: doors, gates,
    // elevators and drawbridges open / travel by their transition's anim reference (ms_ani_tombdoor doors_opening,
    // sa_uo_elevator_adrenaline falling), the bones carrying their collision segments.
    for (const auto& [uid, rt] : uberRuntime_) {
        const auto data = uberData_.find(uid);
        if (data == uberData_.end() || !data->second) continue;
        std::vector<glm::mat4> palette;
        for (size_t i = 0; i < data->second->sections.size(); ++i)
            if (data->second->sections[i].model == 0) {
                if (auto p = uberSectionModel(uid, int32_t(i)) == 0 ? uberSectionPose(uid, int32_t(i)) : std::nullopt) palette = std::move(*p);
                break;
            }
        if (const auto* o = script_.find(uid); o && o->animPose != palette) script_.setAnimPose(uid, std::move(palette));
    }
    // Lara rides the dynamic she stands on (platforms, a door swinging under her); gripping a block, its push carries her.
    const vec3 feet = player_.state().pos;
    const auto ground = col_.groundProbe(feet + vec3(0, 0, 20), 60);
    const int under = ground && player_.state().state != State::Block ? ground->dynamic : -1;
    const glm::mat4 before = under >= 0 ? col_.dynamics[under].transform : glm::mat4(1);
    tickInserted(dt);
    tickJoints(dt);
    collectJointSounds(dt);
    for (size_t i = 0; i < colliders_.size(); ++i) {
        const auto& c = colliders_[i];
        const auto* o = script_.find(c.uid);
        if (!o) continue;
        if (c.section >= 0) { // an uber section's model: its joint's motion, its clip's bone for the segment
            bool removed = false;
            glm::mat4 model = uberSectionBody(c.uid, c.section, &removed).value_or(glm::mat4(1));
            if (const auto palette = uberSectionPose(c.uid, c.section); palette && !palette->empty())
                model = model * (*palette)[std::min<size_t>(size_t(c.segment), palette->size() - 1)];
            col_.dynamics[i].transform = removed ? glm::translate(glm::mat4(1), vec3(0, 0, -1e7f)) // action 28: gone
                                                 : c.intro * model * c.frame;
            continue;
        }
        glm::mat4 model(1); // model-space rigid motion of the segment
        if (c.hinge >= 0) {
            model = glm::inverse(c.intro) * physics_.bodies[physics_.hinges[c.hinge].body].model();
            script_.setBody(c.uid, model);
        } else if (const auto* op = objectPhysics(o->name);
                   op && o->kind == script::Kind::Door && !o->clips && !plates_.contains(c.uid) && cylinderDoor(*op)) {
            // Pinned within its limit: Havok holds the body there (record limit 0..0 on until the script moves it) [I: at once]
            float& at = doorSlides_[c.uid];
            const auto lo = multibodyLo_.find(c.uid), hi = multibodyHi_.find(c.uid);
            if (!multibodyLimitOff_.contains(c.uid))
                at = std::clamp(at, lo != multibodyLo_.end() ? glm::degrees(lo->second) : 0.0f,
                                hi != multibodyHi_.end() ? glm::degrees(hi->second) : 0.0f);
            const auto& m = op->markers[0];
            const glm::mat4 r = glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                                glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1});
            model = glm::translate(glm::mat4(1), vec3(r[2]) * at);
            script_.setBody(c.uid, model);
        } else if (const auto joint = uberJointPose(c.uid)) {
            model = glm::inverse(c.intro) * *joint * c.intro;
            script_.setBody(c.uid, model);
        } else if (const auto palette = script_.pose(*o); palette && !palette->empty()) {
            model = (*palette)[std::min<size_t>(c.segment, palette->size() - 1)];
        }
        col_.dynamics[i].transform = c.intro * model * c.frame;
    }
    tickObjectMarkup();
    if (under >= 0) {
        const vec3 moved = vec3(col_.dynamics[under].transform * glm::inverse(before) * glm::vec4(feet, 1)) - feet;
        if (glm::dot(moved, moved) > 1e-6f) player_.carry(moved);
    }
}

// Portable objects (heavy weights, poles, keys, gems): interact next to one picks it up, she carries it in front of her
// chest, interact again inserts it into a receptacle she engages (insertHeld) or sets it down in front of her feet, where
// it falls as a loose body (it weighs on pressure plates). Empty-handed, interact takes an inserted portable she engages
// back out (removeInserted). Standing, or swimming: l7_arctic_sea's statue arms, gem holes and thor gears lie under
// water (engagement spheres of radius 632-948 there [O data]; tru.exe's swim states' carry clips are not traced [I]).
// [I] No carry clips or walk-speed change; the script's vt 20 pick-up callback (sets its "touched" sticky) is not sent.
bool World::carryReady() const {
    const State s = player_.state().state;
    return s == State::Idle || s == State::SwimTread || s == State::SwimSurface || s == State::SwimUnder;
}

void World::carryPortables(bool pressed) {
    const auto& s = player_.state();
    const vec3 fwd(std::cos(s.yaw), std::sin(s.yaw), 0);
    placePortables();
    auto* held = held_ ? const_cast<script::Object*>(script_.find(held_)) : nullptr; // World owns object motion
    if (held_ && !held) held_ = 0;
    const Engaged engagedNow = pressed && carryReady() ? engaged() : Engaged{};
    if (held && pressed && insertHeld(*held, engagedNow)) return;
    if (held && pressed) {
        // Its collision's base 40 units above her feet (as carried, the base may sit lower than its origin), clear of
        // her proxy capsule (a set-down inside it was shoved away and rolled off a plate): it falls from there onto what
        // is below (a raised plate top included).
        float base = 0, reach = 0;
        for (const auto& d : col_.dynamics)
            if (d.id == held->uid)
                for (const vec3& v : d.shape->vertices) base = std::min(base, vec3(d.transform * glm::vec4(v, 1)).z - (held->pos.z + held->offset.z));
        if (const auto* p = objectPhysics(held->name))
            for (int seg = 0; seg < int(p->segments.size()); ++seg)
                if (const auto shape = physics::solidShape(*p, seg))
                    for (const vec3& v : shape->vertices) reach = std::max(reach, glm::length(vec2(p->segments[seg] * glm::vec4(v, 1))));
        held->offset = s.pos + fwd * std::max(90.0f, player_.tuning().radius + reach) + vec3(0, 0, 40 - base) - held->pos;
        held->loose = true;
        held_ = 0;
        return;
    }
    if (held) {
        held->offset = s.pos + fwd * 60.0f + vec3(0, 0, 80) - held->pos;
        held->body = glm::mat4(1);
        return;
    }
    if (!pressed || !carryReady()) return;
    if (removeInserted(engagedNow)) return;
    // Her engaged record is a loose portable's pick-up record (kind 4: the poles' sphere r 869 around marker 20, the
    // keys' and gems' around their origin). Portables without records: [I] within 150 / 100 of her root.
    if (engagedNow.record && engagedNow.record->kind == 4 && engagedNow.section < 0)
        if (auto* o = const_cast<script::Object*>(script_.find(engagedNow.uid)); o && portable(*o) && !inserted_.contains(o->uid)) {
            held_ = o->uid;
            o->loose = false;
            return;
        }
    for (const auto& o : script_.objects()) {
        if (!portable(o) || inserted_.contains(o.uid)) continue;
        const auto* p = objectPhysics(o.name);
        if (p && std::ranges::any_of(p->engagements, [](const auto& set) { return std::ranges::any_of(set.records, [](const auto& e) { return e.kind == 4; }); }))
            continue;
        const vec3 at = o.pos + o.offset;
        if (glm::length(vec2(at - s.pos)) < 150.0f && std::abs(at.z - s.pos.z) < 100.0f) {
            held_ = o.uid;
            const_cast<script::Object&>(o).loose = false;
            return;
        }
    }
}

namespace {
glm::mat4 markerMatrix(vec3 pos, vec3 rot) {
    return glm::translate(glm::mat4(1), pos) * glm::rotate(glm::mat4(1), rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), rot.y, {0, 1, 0}) *
           glm::rotate(glm::mat4(1), rot.z, {0, 0, 1});
}
// An engagement record's frame: its HInfo marker's (0x46DF20) in `p`'s model, posed by `frame`; else `frame`.
glm::mat4 recordFrame(const physics::ObjectPhysics& p, const glm::mat4& frame, const physics::Engagement& e) {
    if (e.segment >= 0)
        for (const auto& m : p.hmarkers)
            if (m.id == e.segment)
                return frame * (size_t(m.segment) < p.segments.size() ? p.segments[size_t(m.segment)] : glm::mat4(1)) * markerMatrix(m.pos, m.rot);
    return frame;
}
} // namespace

// The receptacle's attach marker (physical record +0x3C: the model marker the portable takes, tru.exe query 0x173 ->
// 0x6B55D0) as the object stands now (intro x body). An uber section's: its record's +0x4C byte, a marker of the
// object's model 0 (0x6D59C0), posed with the section.
std::optional<glm::mat4> World::attachFrame(const script::Object& o, int section) {
    const auto* p = objectPhysics(o.name);
    if (section >= 0) {
        const auto data = uberData_.find(o.uid);
        if (!p || data == uberData_.end() || !data->second || size_t(section) >= data->second->sections.size()) return std::nullopt;
        const int marker = data->second->sections[size_t(section)].attachMarker;
        const auto frame = uberSectionFrame(o, size_t(section));
        if (!frame || marker < 0 || size_t(marker) >= p->markers.size()) return frame;
        return *frame * markerMatrix(p->markers[size_t(marker)].pos, p->markers[size_t(marker)].rot);
    }
    if (!p || p->attachMarker < 0 || size_t(p->attachMarker) >= p->markers.size()) return std::nullopt;
    const auto& m = p->markers[size_t(p->attachMarker)];
    return glm::translate(glm::mat4(1), o.offset) * introModel(o) * o.body * markerMatrix(m.pos, m.rot);
}

// Lara's engagement test (tru.exe 0x719520 -> 0x6F3C60): points up her body, each in the record's frame (its HInfo
// marker's, 0x46DF20, else the instance's), inside its sphere / cylinder (+ the facing cone, 0x6F3A50). [O] The points
// (0x715DD0 on Lara's sensor list, player +0x854: +0x6C entries of 0x60, the last active one of category 3): her
// position + z0, then two more `step` apart, z0 = the shape's +0x10 x 3.1605 and step = its +0x20 x 3.1605 / 2 (a type
// 1 shape; others: one point). Wine (prologue and l4_traprooms, standing): the active category-3 shape is {+0x10 -25,
// +0x14 100, +0x20 225}: -79.01, 276.54, 632.10 above her feet (kEngageZ0 / kEngageStep). [U] Two more category-3
// shapes ({-125, 160, 250}, {150, 60, 90}) were inactive there: crouching / hanging ones, not used.
bool World::engages(const script::Object& o, const physics::Engagement& e) {
    const auto* p = objectPhysics(o.name);
    return p && engagesAt(*p, glm::translate(glm::mat4(1), o.offset) * introModel(o) * o.body, e);
}

bool World::engagesAt(const physics::ObjectPhysics& p, const glm::mat4& frame, const physics::Engagement& e) {
    const glm::mat4 inv = glm::inverse(recordFrame(p, frame, e));
    for (int k = 0; k < 3; ++k) {
        const vec3 q = vec3(inv * glm::vec4(player_.state().pos + vec3(0, 0, kEngageZ0 + float(k) * kEngageStep), 1)) - e.centre;
        if (e.shape == 0) {
            if (glm::dot(q, q) < e.radius * e.radius) return true;
            continue;
        }
        const float b = q.z + e.height * 0.5f;
        if (glm::dot(vec2(q), vec2(q)) >= e.radius * e.radius || b < 0 || b >= e.height) continue;
        if (e.shape == 2 && std::abs(std::remainder(std::atan2(q.y, q.x) - e.heading, glm::two_pi<float>())) >= e.half) continue;
        return true;
    }
    return false;
}

// Uber section `section`'s model physics (each section an instance of its object's model `model`), null none.
const physics::ObjectPhysics* World::uberSectionPhysics(const script::Object& o, size_t section) {
    const auto* data = prepareUber(o);
    const auto drm = objectDrms_.find(o.name);
    if (!data || section >= data->sections.size() || drm == objectDrms_.end() || !drm->second) return nullptr;
    auto mp = modelPhysics_.find({o.name, data->sections[section].model});
    if (mp == modelPhysics_.end()) {
        std::shared_ptr<const physics::ObjectPhysics> p;
        try { p = std::make_shared<physics::ObjectPhysics>(physics::loadObjectPhysics(*drm->second, data->sections[section].model)); } catch (const cdc::FormatError&) {}
        mp = modelPhysics_.emplace(std::pair{o.name, data->sections[section].model}, std::move(p)).first;
    }
    return mp->second.get();
}

// The record Lara engages (tru.exe 0x719520, every frame; player +0x854 +0xA0 / +0xAC). Per instance (query 0x53: an
// anim-graph object's set — the one its script picked, else all [I] —, a portable's +0xA8 sets, an uber section's
// current set) the first record of a nonzero kind that admits her (0x6F3C60: carrying a portable only kinds 3, 5 and
// 7 [I: the portable's flag that narrows it to 5 is not read]; a record naming objects (+0x6C) only while she carries
// one of them [I: tru.exe also accepts one in her inventory, 0x6F3BB0]) and one of her points is inside. Between
// instances (0x704480) the record frame's position with the lower |angle off her facing| / pi + (its horizontal
// distance^2 / the larger of the two) wins; ties keep the first. [I] Not modelled: the overlap list the instances come
// from (sensor +0x7C, category 3) and the line-of-sight check (0x6E7300) for kinds other than 5 and 8.
World::Engaged World::engaged() {
    const auto& s = player_.state();
    const auto* held = held_ ? script_.find(held_) : nullptr;
    Engaged best;
    vec2 bestAt(0);
    const auto score = [&](vec2 d, float most) {
        const float angle = std::abs(std::remainder(std::atan2(d.y, d.x) - s.yaw, glm::two_pi<float>()));
        return std::clamp(angle / glm::pi<float>(), 0.0f, 1.0f) + (most > 0 ? std::clamp(glm::dot(d, d) / most, 0.0f, 1.0f) : 0.0f);
    };
    const auto admits = [&](const physics::Engagement& e) {
        if (!e.kind || (held && e.kind != 3 && e.kind != 5 && e.kind != 7)) return false;
        return e.needs.empty() || (held && std::ranges::any_of(e.needs, [&](uint16_t id) {
                                       const auto* n = script_.objectName(id);
                                       return n && *n == held->name;
                                   }));
    };
    const auto consider = [&](int32_t uid, int section, const physics::ObjectPhysics& p, const glm::mat4& frame, const physics::Engagement& e) {
        if (!admits(e) || !engagesAt(p, frame, e)) return false;
        const vec2 at = vec2(recordFrame(p, frame, e)[3]) - vec2(s.pos);
        const float most = std::max(glm::dot(at, at), glm::dot(bestAt, bestAt));
        if (!best.uid || score(at, most) < score(bestAt, most)) best = {uid, section, &e}, bestAt = at;
        return true; // the instance's first engaging record, whether it won or not
    };
    for (const auto& o : script_.objects()) {
        if (o.dead || o.hidden || o.uid == held_) continue;
        if (const auto* data = prepareUber(o)) {
            for (size_t i = 0; i < data->sections.size(); ++i) {
                const auto* set = uberEngagement(o.uid, i);
                const auto* p = set ? uberSectionPhysics(o, i) : nullptr;
                const auto frame = p ? uberSectionFrame(o, i) : std::nullopt;
                if (!frame) continue;
                for (const auto& e : set->records)
                    if (consider(o.uid, int(i), *p, *frame, e)) break;
            }
            continue;
        }
        const auto* p = objectPhysics(o.name);
        if (!p || p->engagements.empty()) continue;
        const glm::mat4 frame = glm::translate(glm::mat4(1), o.offset) * introModel(o) * o.body;
        bool done = false;
        for (const auto& set : p->engagements) {
            if (o.engagement > 0 && set.id != o.engagement) continue;
            for (const auto& e : set.records)
                if ((done = consider(o.uid, -1, *p, frame, e))) break;
            if (done) break;
        }
    }
    return best;
}

std::vector<vec3> World::engagementCentres(int32_t uid, int kind) {
    std::vector<vec3> out;
    const auto* o = script_.find(uid);
    if (!o) return out;
    const auto add = [&](const physics::ObjectPhysics& p, const glm::mat4& frame, const physics::Engagement& e) {
        if (e.kind && (!kind || e.kind == kind)) out.push_back(vec3(recordFrame(p, frame, e) * glm::vec4(e.centre, 1)));
    };
    if (const auto* data = prepareUber(*o)) {
        for (size_t i = 0; i < data->sections.size(); ++i) {
            const auto* set = uberEngagement(uid, i);
            const auto* p = set ? uberSectionPhysics(*o, i) : nullptr;
            const auto frame = p ? uberSectionFrame(*o, i) : std::nullopt;
            if (frame)
                for (const auto& e : set->records) add(*p, *frame, e);
        }
    } else if (const auto* p = objectPhysics(o->name)) {
        const glm::mat4 frame = glm::translate(glm::mat4(1), o->offset) * introModel(*o) * o->body;
        for (const auto& set : p->engagements)
            if (o->engagement <= 0 || set.id == o->engagement)
                for (const auto& e : set.records) add(*p, frame, e);
    }
    return out;
}

// Uber section `section` of `o` in the world: the object's frame, moved with the joint carrying the section.
std::optional<glm::mat4> World::uberSectionFrame(const script::Object& o, size_t section) {
    if (!prepareUber(o)) return std::nullopt;
    const glm::mat4 base = glm::translate(glm::mat4(1), o.offset) * introModel(o) * o.body;
    const auto body = uberSectionBody(o.uid, int32_t(section));
    return body ? base * *body : base;
}

const physics::EngagementSet* World::uberEngagement(int32_t uid, size_t section) {
    const auto* o = script_.find(uid);
    const auto rt = uberRuntime_.find(uid);
    if (!o || rt == uberRuntime_.end() || section >= rt->second.sections.size()) return nullptr;
    const auto& s = rt->second.sections[section];
    const auto drm = objectDrms_.find(o->name);
    if (s.removed || s.engagement < 0 || drm == objectDrms_.end() || !drm->second) return nullptr;
    auto it = uberEngagements_.find({o->name, s.engagement});
    if (it == uberEngagements_.end())
        it = uberEngagements_.emplace(std::pair{o->name, s.engagement}, physics::loadEngagementSet(*drm->second, uint32_t(s.engagement))).first;
    return it->second ? &*it->second : nullptr;
}

// 0x6D4890 (UberObjectSection_TR8 messages 0x165 / 0x186, 0x6D5AC0): an idle section in a state starts its first
// transition out of it with trigger 20 (insert) / 21 (removal) and parameter 5, or 22 / 23 (any portable). [I] The
// transition's condition check (0x4D0E10) is not run.
bool World::uberInsert(int32_t uid, size_t section, bool inserted) {
    const auto rt = uberRuntime_.find(uid);
    const auto data = uberData_.find(uid);
    if (rt == uberRuntime_.end() || data == uberData_.end() || !data->second || section >= rt->second.sections.size()) return false;
    const auto& s = rt->second.sections[section];
    const auto& sec = data->second->sections[section];
    if (s.removed || s.transition >= 0 || s.state < 0 || size_t(s.state) >= sec.stateOut.size()) return false;
    for (const uint32_t ti : sec.stateOut[size_t(s.state)]) {
        const auto& t = sec.transitions[ti];
        if ((t.trigger == (inserted ? 20u : 21u) && t.param == 5) || t.trigger == (inserted ? 22u : 23u)) {
            uberBegin(uid, section, ti);
            return true;
        }
    }
    return false;
}

// Carrying a portable, interact inserts it (tru.exe: her chart's action 0x4D, 0x716F30, posts INSTANCE message 0x165
// with her engaged object to the portable she holds) into the receptacle she engages (World::engaged): a kind-5 record
// naming the portable's object among its `needs` (0x6F3C60 / 0x6F3BB0; l5 lever 7189 needs ar_pt_pole_a, l4 keyhole
// 6501 ca_pt_key_xibalba_exit, the l4 seal one key per segment 11-14), of an object whose anim graph takes the insert
// stimulus from its state, or of an uber receptacle's section (no anim graph: the shiva arms, calendars, empty throne,
// valgrind cog wheels, statue arms, gem holes; its current set: query 0x53 -> section +0xD4, the record's +0x40, then
// the states' actions 0x1D; its records in the section model's frame). Standing still [I: no insert clip in lara.drm].
bool World::insertHeld(script::Object& held, const Engaged& at) {
    if (!carryReady() || !at.record || at.record->kind != 5) return false;
    const auto* o = script_.find(at.uid);
    if (!o || (at.section < 0 && !script_.graphAccepts(o->uid, formats::kStimulusInsert))) return false;
    held_ = 0;
    attachPortable(held, *o, at.record->segment, at.section);
    return true;
}

// 0x6B5890 message 0x165: the portable attaches at the receptacle's attach marker (0x6B55D0) and posts 0x165 to it:
// an anim-graph receptacle runs its vt 49 and graph stimulus 0x20 (Runtime::insertPortable), an uber section its insert
// transition (uberInsert; 0x6D5AC0 calls no script slot: the uber events reach the script's vt 54).
void World::attachPortable(script::Object& portable, const script::Object& receptacle, int segment, int section) {
    const int32_t uid = portable.uid, rec = receptacle.uid;
    portable.loose = false, portable.offset = vec3(0);
    inserted_[uid] = Inserted{rec, -1, section};
    placed_.insert(uid);
    syncedObjects_ = ~size_t(0); // the receptacle leaves its hinge (syncObjects)
    if (section >= 0) uberInsert(rec, size_t(section), true);
    else script_.insertPortable(uid, rec, segment);
}

// Portables placed in a receptacle (Object::placedIn, from the intro: tru.exe 0x6B5890 message 0x23 -> 0x6A6DC0 ->
// message 0x165) insert themselves once both exist: l2_puzzleroom pole 2967 in hole 2969, l2_maproom pole 2665 in
// track 2664, l3_chapel pole 4823 in hole 4824, l4_temple pole 5240 in hole 5233, the gems and keys in their
// portable_key_preinsert holders (l2_norsehub, l2_puzzleroom, l4_ragnarok_room, l4_temple, l7_arctic_sea).
void World::placePortables() {
    for (const auto& o : script_.objects()) {
        if (!o.placedIn || placed_.contains(o.uid) || inserted_.contains(o.uid) || o.uid == held_) continue;
        const auto* rec = script_.find(o.placedIn);
        if (!rec || !script_.graphAccepts(rec->uid, formats::kStimulusInsert)) continue;
        attachPortable(const_cast<script::Object&>(o), *rec, -1);
    }
}

// Empty-handed (Idle), interact while she engages an inserted portable's pick-up record (kind 4) takes it out (tru.exe:
// her chart action 0x50 posts INSTANCE message 0x186 to her engaged portable; 0x6B5890 posts 0x186 to its receptacle
// and is carried, as a pick-up): Runtime::removePortable runs the receptacle's vt 49 and graph stimulus 0x40; an uber
// section its removal transition (uberInsert; none of the six uber receptacles has one [O data]).
// l2_puzzleroom hole 2969's removal (state 0) starts the elevator: vt 20(0) -> 2960 vt 54, 2961 vt 62.
bool World::removeInserted(const Engaged& at) {
    if (!at.record || at.record->kind != 4 || at.section >= 0) return false;
    const auto it = inserted_.find(at.uid);
    auto* o = it == inserted_.end() ? nullptr : const_cast<script::Object*>(script_.find(at.uid));
    if (!o) return false;
    const int32_t uid = o->uid, rec = it->second.receptacle, section = it->second.section;
    if (it->second.markup >= 0 && size_t(it->second.markup) < col_.markup.size()) col_.markup[size_t(it->second.markup)].type = 0; // "none"
    inserted_.erase(it);
    o->body = glm::mat4(1);
    held_ = uid;
    if (section >= 0) uberInsert(rec, size_t(section), false);
    else script_.removePortable(uid, rec, at.record->segment);
    return true;
}

// An inserted portable takes its receptacle's attach marker frame (0x6B55D0: receptacle instance frame x marker, the
// portable's origin on the marker; Wine l5_valaskjalf_entryb: pole 7190 at lever 7189 + (0, 30.86, 1169.3..1170.0), the
// lever's rotation; [O] the level data's placed pairs: l2
// pole 2967 lies at hole 2969's marker, 713.8 up, within 7 units, the gems and keys at their holders' markers within
// 80). The poles' models run along their own X (sa_*) or Z (ma_*, ca_*, the markers turned 90 degrees about Y): they
// stick out of the hole, horizontal poles (markup type 6) Lara hangs and swings on. A receptacle whose script watches
// its joint (a pole-hole lever's cylindrical joint, marker Z: l5 7189, l4 6502) slides down it under Lara hanging on
// its pole at kSlideSpeed to kSlideBottom [I: Havok weight not measured]; the slider's joint value runs the range
// watches (Runtime::setJointPosition): 7189's -600..-474 -> vt 34 -> reward door 7227 vt 57; 6502's -600..-350 -> its
// loop -> door 6505's limit (vt 54).
void World::tickInserted(float dt) {
    constexpr float kSlideSpeed = 300.0f, kSlideBottom = -600.0f; // [I] units / s; the lowest watched value
    for (auto& [uid, in] : inserted_) {
        auto* pole = const_cast<script::Object*>(script_.find(uid)); // World owns object motion
        auto* rec = const_cast<script::Object*>(script_.find(in.receptacle));
        const auto* pp = pole ? objectPhysics(pole->name) : nullptr;
        const auto* rp = rec ? objectPhysics(rec->name) : nullptr;
        if (!pole || !rec || !pp || !rp) continue; // streamed out: kept
        const auto& s = player_.state();
        const bool hanging = s.state == State::PoleHang && in.markup >= 0 && s.ledge == in.markup;
        if (const auto marker = std::ranges::find(rp->markers, 2u, &physics::Marker::kind);
            in.section < 0 && marker != rp->markers.end() && rec->joint) {
            float& slide = slides_[rec->uid];
            if (hanging) slide = std::max(kSlideBottom, slide - kSlideSpeed * dt);
            const glm::mat4 r = glm::rotate(glm::mat4(1), marker->rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), marker->rot.y, {0, 1, 0}) *
                                glm::rotate(glm::mat4(1), marker->rot.z, {0, 0, 1});
            script_.setBody(rec->uid, glm::translate(glm::mat4(1), vec3(r[2]) * slide));
            if (rec->joint->position != slide) script_.setJointPosition(rec->uid, slide);
            rec = const_cast<script::Object*>(script_.find(in.receptacle)); // scripts may have run
            pole = const_cast<script::Object*>(script_.find(uid));
            if (!rec || !pole) continue;
        }
        const auto frame = attachFrame(*rec, in.section);
        if (!frame) continue;
        pole->offset = vec3(0), pole->loose = false;
        script_.setBody(pole->uid, glm::inverse(introModel(*pole)) * *frame);
        const vec3 ext = pp->boxMax - pp->boxMin;
        const int axis = ext.x >= ext.y && ext.x >= ext.z ? 0 : ext.y >= ext.z ? 1 : 2;
        if (ext[axis] < 3 * ext[(axis + 1) % 3] || ext[axis] < 3 * ext[(axis + 2) % 3]) continue; // not a pole
        vec3 a = (pp->boxMin + pp->boxMax) * 0.5f, b = a;
        a[axis] = pp->boxMin[axis], b[axis] = pp->boxMax[axis];
        collision::Markup m;
        m.type = 6; // horizpole (MUD 0x2000000): Lara hangs and swings on it (Player::pole)
        m.points = {vec3(*frame * glm::vec4(a, 1)), vec3(*frame * glm::vec4(b, 1))};
        if (in.markup < 0 || size_t(in.markup) >= col_.markup.size()) in.markup = int(col_.markup.size()), col_.markup.push_back(m);
        else col_.markup[size_t(in.markup)].type = m.type, col_.markup[size_t(in.markup)].points = m.points;
    }
}

// tru.exe adds the markup items of an instance's current model as markup boxes it owns (0x5D0610 -> 0x5CE4F0: box +0x0C =
// the instance, +0x20 = the item, flags from the type; instance +0x2D8 lists them) for every instance whose model has
// any, and the boxes follow the instance. ca_pu_block_a (l4_ballcourt 6504): one ledgegrab loop round its top
// (+-189.6, 410.8 up, counter-clockwise from above, so the hang side is outside) [O data]: Lara hurdles onto the block
// and hangs from its edges like any ledge. Ours: model 0's items [I: the current model], placed by the object's first
// collision segment (its model frame: intro x motion) so they ride pushes, joints and clips; objects without collision
// segments and portables (their inserted poles: tickInserted) add none [I].
void World::tickObjectMarkup() {
    std::set<int32_t> seen;
    for (size_t i = 0; i < colliders_.size() && i < col_.dynamics.size(); ++i) {
        const auto& c = colliders_[i];
        if (c.section >= 0 || !seen.insert(c.uid).second) continue;
        const auto* o = script_.find(c.uid);
        const auto* p = o ? objectPhysics(o->name) : nullptr;
        if (!p || p->markup.empty() || p->physClass == 50 || p->physClass == kClassBreakable) continue; // breakables: their pieces' (below)
        const glm::mat4 frame = col_.dynamics[i].transform * glm::inverse(c.frame);
        for (int k = 0; k < int(p->markup.size()); ++k) {
            collision::Markup m = p->markup[size_t(k)];
            for (vec3& q : m.points) q = vec3(frame * glm::vec4(q, 1));
            if (o->hidden || o->dead) m.type = 0; // "none"
            auto [it, added] = objectMarkup_.try_emplace({c.uid, k}, int(col_.markup.size()));
            if (added) col_.markup.push_back(std::move(m));
            else col_.markup[size_t(it->second)].points = std::move(m.points), col_.markup[size_t(it->second)].type = m.type;
        }
    }
    for (const auto& [key, index] : objectMarkup_) // gone (streamed out, intangible); breakable pieces' items: below
        if (key.second < 1000 && !seen.contains(key.first) && size_t(index) < col_.markup.size()) col_.markup[size_t(index)].type = 0;
    tickBreakableMarkup();
}

// One physics step on the fixed tick (Jolt, src/physics): rigid-body doors and levers on their hinges, the uber
// objects' joints and links, and loose rigid bodies (Object::loose: grapple-pulled weights, set-down portables) falling,
// tumbling and stacking on the resident units' terrain and the object collision (plates, blocks, doors). Then the
// range watches read the new joint positions. tru.exe: Havok steps every physical object together.
void World::stepPhysics(float dt) {
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
    std::vector<std::pair<uint64_t, physics::Joint1*>> joints;
    std::vector<physics::Scene::JointLink> links;
    const auto key = [](int32_t uid, int32_t index) { return uint64_t(uint32_t(uid)) << 32 | uint32_t(index); };
    for (auto& [uid, rt] : uberRuntime_) {
        for (auto& [index, j] : rt.joints) joints.push_back({key(uid, index), &j});
        const auto data = uberData_.find(uid);
        if (data == uberData_.end() || !data->second) continue;
        for (const int32_t l : rt.links) {
            const auto& lk = data->second->joints[size_t(l)];
            const auto a = rt.joints.find(lk.link1), b = rt.joints.find(lk.link2);
            if (a == rt.joints.end() || b == rt.joints.end()) continue;
            // 0x4D7ED0: degrees per unit when a prismatic joint drives a hinge
            links.push_back({key(uid, lk.link1), key(uid, lk.link2), !a->second.hinge && b->second.hinge ? glm::radians(lk.ratio) : lk.ratio});
        }
    }
    // Pressure plates (Runtime::Object::Joint, docs/formats/PHYSICS.md "Pressure plates"): a Jolt body with the plate's
    // collision on a prismatic joint along its model Z, limited to [lowest range, 0], its motor lifting it at 120 units/s
    // with the force to hold its own weight plus kHold [I]. Weight on it presses it down: loose bodies by their contacts,
    // Lara (her ragdoll record's 55 kg [O], lara ObjectBase +0x7C +4), push blocks and inserted portables (their
    // primitives' mass; tru.exe ties an inserted portable to its receptacle, 0x6B6440, so its weight bears on it: the
    // l4_traprooms empty thrones, plates under their scepter weight) where a ray under them meets it. A new plate starts
    // at its saved position; afterwards x and v are Jolt's (plates_).
    constexpr float kHold = 20.0f, kPlateMass = 50.0f, kLaraMass = 55.0f; // kg; a plate without primitive mass [I]
    std::map<int32_t, physics::Joint1> plates;
    std::map<int32_t, std::vector<collision::Dynamic>> plateMeshes;
    std::vector<physics::Scene::Plate> plateBodies;
    for (const auto& o : script_.objects()) {
        if (!o.joint || !o.joint->prismatic || o.joint->ranges.empty()) continue;
        std::vector<collision::Dynamic> meshes;
        for (size_t i = 0; i < colliders_.size(); ++i)
            if (colliders_[i].uid == o.uid) meshes.push_back({o.uid, col_.dynamics[i].shape, colliders_[i].intro * colliders_[i].frame});
        if (meshes.empty()) continue;
        const auto* p = objectPhysics(o.name);
        auto mp = p ? physics::massProps(*p) : physics::MassProps{};
        if (mp.mass <= 0) mp.mass = kPlateMass, mp.com = vec3(0);
        const glm::mat4 intro = introModel(o);
        physics::Joint1 j;
        j.mass = mp.mass, j.axis = glm::normalize(vec3(intro[2])), j.pivot = vec3(intro[3]), j.arm = glm::mat3(intro) * mp.com;
        j.limited = true, j.hi = 0;
        for (const auto& r : o.joint->ranges) j.lo = std::min(j.lo, r.lo);
        j.motor = true, j.speed = 120.0f / 30.0f, j.force = (mp.mass + kHold) * physics::kJointGravity * 0.5f; // per 1/60 s step
        if (const auto old = plates_.find(o.uid); old != plates_.end()) j.x = old->second.x, j.v = old->second.v;
        else j.x = o.joint->position;
        plates[o.uid] = j;
        plateMeshes[o.uid] = std::move(meshes);
    }
    plates_ = std::move(plates);
    for (const auto& [uid, meshes] : plateMeshes) plateBodies.push_back({key(uid, -2), meshes});
    std::vector<physics::Scene::Weight> weights{{player_.state().pos, kLaraMass}};
    for (const auto& b : script_.objects()) {
        if (b.kind != script::Kind::Block) continue;
        vec3 lo(1e30f), hi(-1e30f);
        for (const auto& d : col_.dynamics)
            if (d.id == b.uid)
                for (const vec3& v : d.shape->vertices) {
                    const vec3 q(d.transform * glm::vec4(v, 1));
                    lo = glm::min(lo, q), hi = glm::max(hi, q);
                }
        if (lo.x > hi.x) continue;
        const auto* p = objectPhysics(b.name);
        const float mass = p ? physics::massProps(*p).mass : 0.0f;
        weights.push_back({vec3(0.5f * (lo.x + hi.x), 0.5f * (lo.y + hi.y), lo.z), mass > 0 ? mass : 666.0f}); // tru.exe's fallback mass
    }
    // ... and at the receptacle's attach marker, riding its plate (an inserted portable has no collider; an uber plate's
    // body is not posed by its plate joint, so the plate's travel replaces whatever the body holds).
    for (const auto& [uid, in] : inserted_) {
        const auto* b = script_.find(uid);
        const auto* rec = script_.find(in.receptacle);
        const auto* p = b ? objectPhysics(b->name) : nullptr;
        const auto frame = rec ? attachFrame(*rec, in.section) : std::nullopt;
        const float mass = p ? physics::massProps(*p).mass : 0.0f;
        if (!frame || mass <= 0) continue;
        vec3 at((*frame)[3]);
        if (const auto plate = plates_.find(in.receptacle); plate != plates_.end())
            at += plate->second.axis * std::clamp(plate->second.x, plate->second.lo, 0.0f) - glm::mat3(introModel(*rec)) * vec3(rec->body[3]);
        weights.push_back({at, mass});
    }
    physics_.setPlates(plateBodies, weights);
    for (auto& [uid, j] : plates_) joints.push_back({key(uid, -2), &j}); // by uid: a fixed constraint order
    // Lara on the rope of a grapple handle (an anchor, not a pull target): the rope pulls its swivelling section toward
    // her by her weight, so the handle hanging at its limit under gravity (tru.exe under Wine: l2 handles 49-52 degrees
    // down at rest) turns to the rope within its limits. [I] the force (her 55 kg along the rope's first segment, at
    // the section's centre of mass) and the damping; tru.exe's Havok rope constraint is not decoded.
    if (const auto& s = player_.state(); s.grappleUid && s.grappleKind != GrappleKind::Mounted)
        if (const auto rt = uberRuntime_.find(s.grappleUid); rt != uberRuntime_.end()) {
            const vec3 toward = s.ropeBends ? s.ropeBend[0] : s.pos + vec3(0, 0, 400.0f); // the hand, as Grapple.cpp kHandZ
            const vec3 rope = toward - s.grappleAnchor;
            const float frames = dt * 30.0f;
            if (glm::length(rope) > 1.0f)
                for (auto& [index, j] : rt->second.joints) {
                    const vec3 f = glm::normalize(rope) * (kLaraMass * physics::kJointGravity); // mass * units / frame^2
                    if (j.hinge && j.inertia > 0) {
                        const vec3 arm = glm::angleAxis(j.x, j.axis) * j.arm;
                        j.v += glm::dot(glm::cross(arm, f), j.axis) / j.inertia * frames;
                    } else if (!j.hinge && j.mass > 0) {
                        j.v += glm::dot(f, j.axis) / j.mass * frames;
                    }
                    j.v *= std::exp(-6.0f * dt);
                }
        }
    physics_.setJoints(std::move(joints), std::move(links));
    // A body the grapple pulled loose that still rides its uber joint (l1 snake head 751 on its rail, held by a
    // counterweight link): the pull drags its watched prismatic joints downhill at the braced pull speed while they are
    // in range [I] (tru.exe: Lara's rope pulls the Havok body); leaving the range, its states drop the link, then the
    // rail (ms_rb_grapple_snake_head section 1), and it falls free as a loose body from where the rail left it.
    std::vector<physics::Scene::Loose> loose;
    std::set<int32_t> looseIds;
    for (const auto& o : script_.objects()) {
        if (!o.loose) continue;
        if (uberModelJoint(o.uid) >= 0) {
            auto& rt = uberRuntime_.at(o.uid);
            for (const auto& w : rt.watches)
                if (const auto j = rt.joints.find(w.joint); w.inside && j != rt.joints.end() && !j->second.hinge)
                    j->second.push = (j->second.axis.z < 0 ? 240.0f : -240.0f) / 30.0f;
            loose.push_back({o.uid, objectPhysics(o.name), introModel(o), key(o.uid, uberModelJoint(o.uid))});
            looseIds.insert(o.uid);
            continue;
        }
        loose.push_back({o.uid, objectPhysics(o.name), glm::translate(glm::mat4(1), o.offset) * introModel(o) * o.body});
        looseIds.insert(o.uid);
    }
    chunkPieces(loose);
    breakableLoose(loose); // broken class-30 objects' pieces (Breakables.cpp)
    physics_.setLoose(loose);
    const auto bikes = bikeVehicles();
    physics_.setVehicles(bikes); // the ridden motorcycle (Vehicle.cpp); readBikes takes the step's result
    // Lara's proxy shoves loose bodies and ragdolls she walks into (by her mass); not while she rides or lies dead.
    const physics::Scene::Character lara{player_.state().pos, player_.tuning().radius, player_.tuning().height, kLaraMass};
    physics_.setCharacter(bikes.empty() && player_.state().state != State::Dead ? &lara : nullptr);
    std::vector<physics::Scene::Terrain> terrain;
    for (const auto& u : units_) terrain.push_back({u.data.name, &u.data.col, u.offset});
    physics_.setTerrain(terrain);
    std::vector<collision::Dynamic> solids; // what loose bodies land on: every collider but their own and the plates'
    for (const auto& d : col_.dynamics)
        if (!looseIds.contains(d.id) && !plates_.contains(d.id)) solids.push_back(d);
    physics_.setColliders(solids);
    physicsWater_.clear(); // buoyancy and currents for loose bodies (Scene::setWater, tru.exe 0x6AD070)
    for (const auto& m : col_.markup)
        if (m.type == 10 && m.points.size() > kWaterIds)
            physicsWater_.push_back({m.points[2], m.points[3], vec2(m.points[4]), m.points[1].z,
                                     [](const void* u, vec3 p) { return waterFlow(*static_cast<const collision::Markup*>(u), p); }, &m});
    physics_.setWater(physicsWater_);
    syncRagdolls(false); // a loaded scene's ragdolls exist before its recorded state applies
    physics_.step(dt);
    syncRagdolls(true);
    for (const auto& o : script_.objects()) // World owns object motion, as for pulled blocks
        if (const auto m = o.loose ? physics_.loose(o.uid) : std::nullopt) {
            auto& w = const_cast<script::Object&>(o);
            w.offset = vec3((*m)[3]) - o.pos; // the pose's displacement, its rotation in model space (Runtime::pose)
            w.body = glm::mat4(glm::transpose(glm::mat3(introModel(o))) * glm::mat3(*m));
        }
    for (auto& [uid, rt] : uberRuntime_)
        if (const auto d = uberData_.find(uid); d != uberData_.end() && d->second) uberWatches(uid);
    readBikes(dt);
}

// Dead enemies' ragdolls (physics::Scene::setRagdolls, ai::EnemyState::ragdollPhase): a starting one (1) gets bodies at
// its death pose, moving as the clip last did, with the killing hit's impulse at the hit point: death record +0x4C x 2.5
// along the shot as 0x696A60 applies it to a dying body, [I] 2000 when the record has none (soldiers, cats). Simulating
// ones (2) keep theirs; settled (3), alive or streamed-out enemies have none. `step`: after a physics step each body's
// pose goes back to its enemy, which settles once they sleep (World owns the motion, as for loose bodies).
void World::syncRagdolls(bool step) {
    const PlayerState& lara = player_.state();
    const glm::mat4 laraModel = player_.modelMatrix(lara.pos, lara.yaw, lara.pitch);
    if (step) {
        for (const auto& e : enemies_.all())
            if (e->state().ragdollPhase == 2) {
                if (const auto p = physics_.ragdoll(e->uid())) e->setRagdoll(p->frames, p->asleep);
            } else if (e->state().ragdollPhase == 4 && !e->state().pieces.empty()) {
                std::vector<glm::mat4> pieces;
                for (size_t k = 0; k < e->state().pieces.size(); ++k)
                    if (const auto m = physics_.loose(pieceId(e->uid(), k))) pieces.push_back(*m);
                if (pieces.size() == e->state().pieces.size()) e->setPieces(pieces);
            }
        if (combat_.ragdollPhase == 2)
            if (const auto p = physics_.ragdoll(kLaraRagdoll)) {
                const glm::mat4 toModel = glm::inverse(laraModel);
                for (size_t i = 0; i < p->frames.size() && i < combat_.ragdoll.size(); ++i) combat_.ragdoll[i] = toModel * p->frames[i];
                if (p->asleep) combat_.ragdollPhase = 3;
            }
        return;
    }
    for (const auto& e : enemies_.all())
        if (e->state().ragdollPhase == 4 && e->state().pieces.empty() && e->type().chunks) startChunks(*e);
    const auto& all = enemies_.all();
    std::vector<physics::Scene::Ragdoll> list;
    std::vector<std::vector<glm::mat4>> frames(all.size());
    std::vector<std::vector<vec3>> velocity(all.size());
    for (size_t k = 0; k < all.size(); ++k) {
        const auto& e = *all[k];
        const auto& s = e.state();
        const auto& t = e.type();
        if (!t.ragdoll || (s.ragdollPhase != 1 && s.ragdollPhase != 2)) continue;
        const glm::mat4 model = ai::Enemy::modelMatrix(s.pos, s.yaw, 0, s.tilt);
        for (const glm::mat4& m : s.ragdoll) frames[k].push_back(model * m);
        physics::Scene::Ragdoll r{e.uid(), &*t.ragdoll, t.bind, frames[k], {}, {}, {}};
        if (s.ragdollPhase == 1) {
            const auto now = e.pose(1.0), before = e.pose(0.0);
            for (const auto& b : t.ragdoll->bones)
                velocity[k].push_back(glm::mat3(model) * vec3(now[size_t(b.segment)][3] - before[size_t(b.segment)][3]) / float(kTickSeconds) +
                                      s.knock);
            r.velocity = velocity[k];
            r.hitPoint = s.pos + s.killAt;
            r.impulse = s.killDir * (2.5f * (t.tuning.deathImpulse > 0 ? t.tuning.deathImpulse : 2000.0f));
        }
        list.push_back(r);
    }
    // Lara's (AnimLibrary::ragdoll, CombatState::ragdollPhase): no hit impulse, at rest (the death clip is done).
    std::vector<glm::mat4> laraFrames;
    if (anims_.ragdoll && (combat_.ragdollPhase == 1 || combat_.ragdollPhase == 2) && combat_.ragdoll.size() == anims_.ragdoll->bones.size()) {
        for (const glm::mat4& m : combat_.ragdoll) laraFrames.push_back(laraModel * m);
        list.push_back({kLaraRagdoll, &*anims_.ragdoll, anims_.bind, laraFrames, {}, {}, {}});
        combat_.ragdollPhase = 2;
    }
    physics_.setRagdolls(list);
    for (size_t k = 0; k < all.size(); ++k)
        if (all[k]->state().ragdollPhase == 1 && !frames[k].empty()) all[k]->setRagdoll(frames[k], false);
}

// Thralls broken into chunks (EnemyType::chunks, tru.exe 0x696F90 / 0x696B90 / 0x696A60): when one starts (phase 4,
// no pieces yet; syncRagdolls right after the enemy tick, so a save between ticks holds them) each piece of its chunk
// object appears at mix(segment B, segment A, t) of the body's pose, rotated as slerp(B, A, t) [I: the pairing], its
// model's box centre there (0x696B00), moving as those segments did over the last tick (0x78DED0 mean velocity;
// 0x78DF80 [I] segment B's spin), plus death record +0x4C x 2.5 along the hit (0x696A60, one impulse per piece; as the
// ragdolls', mass x units/s). Then they are loose bodies like any other (chunkPieces -> Scene::setLoose; World owns
// their pose, syncRagdolls reads it back). [U] piece flags bit 0, the +0x71 mark of the first unflagged piece, how the
// set counter 0xECFC70 drops (ours: live sets among the loaded enemies).
void World::chunkPieces(std::vector<physics::Scene::Loose>& out) {
    for (const auto& e : enemies_.all()) {
        const auto& s = e->state();
        const auto& c = e->type().chunks;
        if (s.ragdollPhase != 4 || !c || s.pieces.size() != c->pieces.size()) continue;
        const glm::mat4 model = ai::Enemy::modelMatrix(s.pos, s.yaw, 0, s.tilt);
        const bool moving = s.pieceVel.size() == 2 * s.pieces.size();
        for (size_t k = 0; k < s.pieces.size(); ++k)
            out.push_back({pieceId(e->uid(), k), &c->models[size_t(c->pieces[k].model)], model * s.pieces[k], 0,
                           moving ? s.pieceVel[2 * k] : vec3(0), moving ? s.pieceVel[2 * k + 1] : vec3(0)});
    }
}

void World::startChunks(ai::Enemy& e) {
    const auto& s = e.state();
    const ai::ChunkDoll& c = *e.type().chunks;
    const glm::mat4 model = ai::Enemy::modelMatrix(s.pos, s.yaw, 0, s.tilt);
    const auto now = e.pose(1.0), before = e.pose(0.0);
    const auto seg = [&](int i) { return size_t(std::clamp(i, 0, int(now.size()) - 1)); };
    std::vector<glm::mat4> world;
    std::vector<vec3> vel;
    for (const ai::ChunkPiece& p : c.pieces) {
        const physics::ObjectPhysics& shape = c.models[size_t(p.model)];
        const glm::mat4 &a = now[seg(p.segA)], &b = now[seg(p.segB)];
        const glm::quat rot = glm::slerp(glm::normalize(glm::quat_cast(glm::mat3(b))), glm::normalize(glm::quat_cast(glm::mat3(a))), p.t);
        glm::mat4 m = glm::mat4_cast(rot);
        m[3] = glm::vec4(glm::mix(vec3(b[3]), vec3(a[3]), p.t), 1);
        world.push_back(model * m * glm::translate(glm::mat4(1), -0.5f * (shape.boxMin + shape.boxMax)));
        const vec3 va = vec3(a[3] - before[seg(p.segA)][3]), vb = vec3(b[3] - before[seg(p.segB)][3]);
        vec3 v = glm::mat3(model) * (0.5f * (va + vb)) / float(kTickSeconds);
        glm::quat spin = glm::normalize(glm::quat_cast(glm::mat3(b)) * glm::inverse(glm::quat_cast(glm::mat3(before[seg(p.segB)]))));
        if (spin.w < 0) spin = -spin;
        if (const float imp = e.type().tuning.deathImpulse; imp > 0) {
            const float mass = physics::massProps(shape).mass;
            v += s.killDir * (2.5f * imp / (mass > 0 ? mass : 666.0f)); // Scene::setLoose's fallback mass
        }
        vel.push_back(v);
        vel.push_back(glm::mat3(model) * (glm::axis(spin) * glm::angle(spin)) / float(kTickSeconds));
    }
    e.setPieces(world, vel);
}

// Script multibody joints (Runtime::Object::Joint) take their position every update and run their range watches
// (Runtime::setJointPosition, 0x4937A0): pressure plates the one the Jolt step left (stepPhysics) within their limits,
// hinge doors their hinge angle in degrees, other joints (l1_baldrs_tomb locks 69 / 70) move at their `multibody` 89
// motor speed.
void World::tickJoints(float dt) {
    std::map<int32_t, int> hinges;
    for (const auto& c : colliders_)
        if (c.hinge >= 0) hinges.emplace(c.uid, c.hinge);
    std::vector<std::tuple<int32_t, float, std::optional<glm::mat4>>> update; // setJointPosition may run scripts
    for (const auto& o : script_.objects()) {
        if (!o.joint) continue;
        const auto& j = *o.joint;
        float x = j.position;
        std::optional<glm::mat4> body;
        if (const auto p = plates_.find(o.uid); p != plates_.end()) {
            x = std::clamp(p->second.x, p->second.lo, 0.0f);
            body = glm::translate(glm::mat4(1), vec3(0, 0, x));
        } else if (const auto h = hinges.find(o.uid); h != hinges.end()) {
            x = glm::degrees(physics_.angle(h->second));
        } else if (j.speed != 0) {
            // Within the body's joint limit (ObjectPhysics::jointLimit; a `multibody` 77 limit, kept in radians, wins).
            // [I] a motor reaching its target speed at once (the Havok joint is not simulated).
            const auto* p = objectPhysics(o.name);
            const auto [lo, hi] = scriptJointLimit(o);
            x = std::clamp(x + j.speed * 30.0f * dt, lo, std::max(lo, hi));
            // One prismatic joint marker (kind 2, as the uber joint records' type byte [I]): the body slides along its
            // Z axis (ms_rb_tomb_lock sinks). [U] bodies with several (ca_rb_crusher_slim's two jaws and their link).
            if (p && std::ranges::count(p->markers, 2u, &physics::Marker::kind) == 1) {
                const auto& m = *std::ranges::find(p->markers, 2u, &physics::Marker::kind);
                const glm::mat4 r = glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                                    glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1});
                body = glm::translate(glm::mat4(1), vec3(r[2]) * x);
            }
        }
        if (body || x != j.position || std::ranges::any_of(j.ranges, [&](const auto& r) { return r.inside != (r.lo <= x && x <= r.hi); }))
            update.push_back({o.uid, x, body}); // a body is set every update: a loaded joint shows where it stands
    }
    for (const auto& [uid, x, body] : update) {
        if (body) script_.setBody(uid, *body);
        script_.setJointPosition(uid, x);
    }
}

std::pair<float, float> World::scriptJointLimit(const script::Object& o) {
    const auto* p = objectPhysics(o.name);
    float lo = p && p->jointLimit ? p->jointLimit->lo : -1e30f, hi = p && p->jointLimit ? p->jointLimit->hi : 1e30f;
    if (const auto l = multibodyLo_.find(o.uid); l != multibodyLo_.end()) lo = glm::degrees(l->second);
    if (const auto u = multibodyHi_.find(o.uid); u != multibodyHi_.end()) hi = glm::degrees(u->second);
    if (multibodyLimitOff_.contains(o.uid)) lo = -1e30f, hi = 1e30f; // `multibody` 75: limit off
    return {lo, hi};
}

// tru.exe's joint-sound manager (docs/formats/AUDIO.md §9.16): every frame 0x4F3120 makes a contact for each registered
// joint (a record with a sound: uber joints 0x4D8000, rigid bodies 0x6B6440) whose bodies are both awake (0x4D9000),
// filled by 0x4F22C0 with the joint's speed (hinge: degrees/s, prismatic: units/s); reaching a limit adds an impact
// (0x4E76A0 -> 0x4F2E40). Havok deactivates a body that rests (no contact, no voice); ours: a joint is awake while it
// moves and 0.5 s after (WakeTimer; our joint bodies never sleep in Jolt) [I: Havok's thresholds and timer].
void World::collectJointSounds(float dt) {
    jointSounds_.clear();
    std::set<std::pair<int32_t, int32_t>> live;
    const auto track = [&](int32_t uid, int32_t joint, float x, float v, bool moving, bool limited, float lo, float hi,
                           float tolerance, const std::string& object, cdc::Pointer sound, vec3 at, bool hinge) {
        auto& m = jointMotion_[{uid, joint}];
        live.insert({uid, joint});
        std::optional<float> impact;
        if (limited) {
            if (!m.seen) m.limit.update(x, lo, hi, tolerance, 0.0f); // a joint found resting at a limit is no impact
            else impact = m.limit.update(x, lo, hi, tolerance, m.v);
        }
        const bool awake = m.wake.update(moving, dt);
        m.x = x, m.v = v, m.seen = true;
        if (!awake) return;
        JointSound s{uid, joint, object, sound, at};
        (hinge ? s.angular : s.linear) = v;
        s.impact = impact;
        jointSounds_.push_back(std::move(s));
    };
    for (const auto& [uid, rt] : uberRuntime_) {
        const auto d = uberData_.find(uid);
        const auto* o = script_.find(uid);
        if (d == uberData_.end() || !d->second || !o) continue;
        for (const auto& [index, j] : rt.joints) {
            if (index < 0 || size_t(index) >= d->second->joints.size()) continue;
            const auto& rec = d->second->joints[size_t(index)];
            if (!rec.sound) continue;
            // 0x4F1E10: 0 the pivot on body A (the world: fixed), 1 on body B (riding a prismatic joint), 2 between.
            const float along = j.hinge ? 0.0f : j.x * (rec.soundAt == 1 ? 1.0f : rec.soundAt == 2 ? 0.5f : 0.0f);
            const float v = j.hinge ? jointAngularSpeed(j.v) : jointLinearSpeed(j.v);
            // Limits in the coordinate: hinge radians (0.25 degrees), prismatic units (0.5) [I: tolerances].
            // Moving: above 1 unit/s or 1 degree/s [I] (a resting joint's solver jitter is far below).
            track(uid, index, j.x, v, std::abs(v) > 1.0f, j.limited, j.lo, j.hi, j.hinge ? 0.0044f : 0.5f, o->name, *rec.sound,
                  j.pivot + j.axis * along, j.hinge);
        }
    }
    std::map<int32_t, int> hinges;
    for (const auto& c : colliders_)
        if (c.hinge >= 0) hinges.emplace(c.uid, c.hinge);
    for (const auto& o : script_.objects()) {
        if (!o.joint) continue;
        const auto* p = objectPhysics(o.name);
        if (!p || !p->jointSound) continue;
        const float x = o.joint->position; // degrees for hinges, units otherwise (tickJoints)
        const auto& m = jointMotion_[{o.uid, -1}];
        const float v = m.seen ? (x - m.x) / dt : 0.0f;
        const bool moving = m.seen && x != m.x;
        const bool hinge = hinges.contains(o.uid) ||
                           (!plates_.contains(o.uid) && std::ranges::count(p->markers, 2u, &physics::Marker::kind) != 1);
        const auto [lo, hi] = plates_.contains(o.uid) ? std::pair(plates_.at(o.uid).lo, 0.0f) : scriptJointLimit(o);
        const glm::mat4 intro = glm::translate(glm::mat4(1), o.offset) * introModel(o);
        const vec3 pivot = vec3(intro[3]), body = vec3(intro * o.body[3]);
        const vec3 at = p->jointSoundAt == 1 ? body : p->jointSoundAt == 2 ? (pivot + body) * 0.5f : pivot;
        track(o.uid, -1, x, v, moving, p->jointLimitOn || plates_.contains(o.uid), lo, hi, hinge ? 0.25f : 0.5f, o.name,
              *p->jointSound, at, hinge);
    }
    std::erase_if(jointMotion_, [&](const auto& e) { return !live.contains(e.first); });
}

} // namespace ou::game
