// World <-> src/physics: collision of scripted objects (doors, push blocks) and rigid-body hinge doors.
// Evidence and inferences: docs/formats/PHYSICS.md.
#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>

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

void World::syncObjects() {
    std::map<int32_t, float> angles; // doors that stay loaded keep their swing
    for (const auto& c : colliders_)
        if (c.hinge >= 0) angles[c.uid] = physics_.angle(c.hinge);
    colliders_.clear();
    col_.dynamics.clear();
    physics_ = {};
    std::map<int32_t, int> hingeOf;
    for (const auto& o : script_.objects()) {
        if (o.kind != script::Kind::Door && o.kind != script::Kind::Block) continue;
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
        // [I] A rigid body (class 34) door without clips swings about the X axis of its first model marker (the
        // joint frame tru.exe's joints are built from; ca_rb_wall_hpole_door: the pole along its top, z = 952.9).
        if (o.kind == script::Kind::Door && !o.clips && p.physClass == physics::kClassRigidBody && !p.markers.empty()) {
            if (const auto mp = physics::massProps(p); mp.mass > 0) {
                const auto& m = p.markers[0];
                const glm::mat3 r(glm::rotate(glm::mat4(1), m.rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), m.rot.y, {0, 1, 0}) *
                                  glm::rotate(glm::mat4(1), m.rot.z, {0, 0, 1}));
                hinge = physics_.addHinge(physics_.addBody(mp, intro), m.pos, r * glm::vec3(1, 0, 0), 0, kDoorOpen + 0.02f);
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
    for (const auto& c : colliders_) // the motor follows the scripted door: opened (state 2) -> swing open [I]
        if (c.hinge >= 0) {
            const auto* o = script_.find(c.uid);
            physics_.hinges[c.hinge].target = o && o->state == 2 ? kDoorOpen : 0.0f;
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
