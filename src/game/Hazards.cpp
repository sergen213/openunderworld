// Contact hazards: collision primitives that hurt what touches them.
//
// [O] tru.exe: every HInfo collision primitive carries a damage amount (sphere +0x1C, box +0x3E, capsule +0x32, s16;
// 0x48E040). When the collision pair handlers of Lara's category meet a primitive whose amount is not 0 they call
// 0x4923C0: the primitive's owner (not flagged +0x1C & 0x100) is the attacker, the other instance the victim; the
// damage manager's slot +0x58 (0x5D4C60) posts the hit message 0x11C with the amount and damage class 6 to the victim
// (not to one flagged +0x14 & 0x20), then both instances' scripts get prototype 34 (vt 21: attacker, victim, ...; the
// trap scripts play their impact sound there, the Xibalba spear / blade rows also lock her weapons) and prototype 35.
// Lara takes the message like an enemy's blow (0x6F5030): class 6 has priority 4, so a second touch inside the
// invulnerability window (player data +0x60, 30 frames) is ignored.
//
// The amounts are the campaign's trap numbers (survey: docs/GAMEPLAY.md "Hazards"): 12345 = dead at once (Thor's
// hammers, the crusher jaws, the falling snake head), 1000 the Xibalba blades and spears, 800 the flame jets, 500 the
// kraken's tentacles and Valaskjalf's weights, 400 the grinders, 9999 the sinking ship's barrel run.
//
// Two ways in [O]: the handlers of overlap pairs (0x4928C0 / 0x492980) call 0x4923C0 for every overlap; the rigid
// contact handler (0x496220, pair categories 1-1 and 11-1) only when one of the pair's contact points moves at 5 or
// more (v.v >= 25.0), then posts the collision message 0x3B, and Lara's handler of 0x3B (0x75A020) calls 0x4923C0. [I]
// which primitives take which way: those whose flag bytes 2-3 are not 0 (the flame jets, the roller sets, the weights'
// under-plates, the ship's barrel run, the squib spheres) are overlap volumes; the others (crusher and door plates,
// blades, spears, tentacles, the hammer) are solid collision and hurt only while they move at 5 units a second or
// more [I: the unit, and that her own movement into a resting one does not count: the gate of Baldr's tomb is one
// 2000 plate she walks up to; open: a Wine check of a walk into it].
//
// Ours: each primitive is posed with its segment (the collider's transform where the segment collides, else the uber
// section's joint and clip, else the anim graph's pose) and tested against Lara's capsule. [I] kContactSkin: a solid
// primitive keeps her capsule out, so touching counts from that far off its surface.
#include "game/Game.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace ou::game {

namespace {
constexpr float kContactSpeed = 5.0f; // [O] 0x496220: 25.0 = its square; units a second [I]
constexpr float kContactSkin = 6.0f; // [I] units; the controller rests about a skin width off solid collision

glm::mat4 introModel(const script::Object& o) { // T * Rx * Ry * Rz, as the intros (formats::introMatrix)
    return glm::translate(glm::mat4(1), o.pos) * glm::rotate(glm::mat4(1), o.rot.x, {1, 0, 0}) *
           glm::rotate(glm::mat4(1), o.rot.y, {0, 1, 0}) * glm::rotate(glm::mat4(1), o.rot.z, {0, 0, 1});
}

// Distance between the segment a-b and the axis-aligned box of half extents `h` at the origin (convex in the segment
// parameter: ternary search).
float segmentBoxDistance(vec3 a, vec3 b, vec3 h) {
    const auto dist = [&](float t) {
        const vec3 p = glm::mix(a, b, t);
        return glm::length(glm::max(glm::abs(p) - h, vec3(0)));
    };
    float lo = 0, hi = 1;
    for (int i = 0; i < 40; ++i) {
        const float m1 = lo + (hi - lo) / 3, m2 = hi - (hi - lo) / 3;
        if (dist(m1) < dist(m2)) hi = m2;
        else lo = m1;
    }
    return dist(0.5f * (lo + hi));
}
float pointSegmentDistance(vec3 p, vec3 a, vec3 b) {
    const vec3 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    const float t = l2 > 0 ? std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f) : 0.0f;
    return glm::distance(p, a + ab * t);
}
// Distance between the segments p1-q1 and p2-q2 (Ericson, Real-Time Collision Detection 5.1.9).
float segmentSegmentDistance(vec3 p1, vec3 q1, vec3 p2, vec3 q2) {
    const vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    const float a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
    float s = 0, t = 0;
    if (a <= 1e-6f && e <= 1e-6f) return glm::length(r);
    if (a <= 1e-6f) t = std::clamp(f / e, 0.0f, 1.0f);
    else {
        const float c = glm::dot(d1, r);
        if (e <= 1e-6f) s = std::clamp(-c / a, 0.0f, 1.0f);
        else {
            const float b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-9f ? std::clamp((b * f - c * e) / denom, 0.0f, 1.0f) : 0.0f;
            t = (b * s + f) / e;
            if (t < 0) t = 0, s = std::clamp(-c / a, 0.0f, 1.0f);
            else if (t > 1) t = 1, s = std::clamp((b - c) / a, 0.0f, 1.0f);
        }
    }
    return glm::length(p1 + d1 * s - (p2 + d2 * t));
}
} // namespace

float hazardDistance(const HazardShape& h, vec3 a, vec3 b) {
    // The primitive's frame without its scale (a clip may scale a bone: a flame's bone shrinks to nothing while off).
    const glm::mat3 m(h.world);
    const vec3 scale(glm::length(m[0]), glm::length(m[1]), glm::length(m[2]));
    if (std::min({scale.x, scale.y, scale.z}) < 1e-3f) return 1e9f;
    const glm::mat3 r(m[0] / scale.x, m[1] / scale.y, m[2] / scale.z);
    const vec3 c(h.world[3]);
    const vec3 la = glm::transpose(r) * (a - c), lb = glm::transpose(r) * (b - c);
    switch (h.kind) {
    case physics::PrimKind::Box: return segmentBoxDistance(la, lb, 0.5f * h.size * scale);
    case physics::PrimKind::Sphere: return pointSegmentDistance(vec3(0), la, lb) - h.radius * scale.x;
    case physics::PrimKind::Capsule: {
        const vec3 half(0, 0, 0.5f * h.length * scale.z);
        return segmentSegmentDistance(la, lb, -half, half) - h.radius * scale.x;
    }
    }
    return 1e9f;
}

// The damaging primitives of the loaded objects (after syncObjects: the colliders are known).
nlohmann::json World::serializeHazards() const {
    nlohmann::json out = nlohmann::json::array();
    std::map<int32_t, int> nth;
    for (const HazardShape& h : hazards_) {
        const int k = nth[h.uid]++;
        if (h.placed) out.push_back({h.uid, k, h.last.x, h.last.y, h.last.z});
    }
    return out;
}

void World::restoreHazards(const nlohmann::json& j) {
    hazardRestored_.clear();
    for (const auto& e : j)
        hazardRestored_[{e.at(0).get<int32_t>(), e.at(1).get<int>()}] = vec3(e.at(2).get<float>(), e.at(3).get<float>(), e.at(4).get<float>());
    hazards_.clear(), hazardObjects_ = ~size_t(0);
}

void World::syncHazards() {
    {
        std::map<int32_t, int> nth; // a rebuilt list (units came or went) keeps what it knew
        for (const HazardShape& h : hazards_) {
            const int k = nth[h.uid]++;
            if (h.placed) hazardRestored_.try_emplace({h.uid, k}, h.last);
        }
    }
    hazards_.clear();
    const auto add = [&](const script::Object& o, const physics::ObjectPhysics& p, int section) {
        for (const physics::Prim& prim : p.prims) {
            if (prim.damage == 0) continue;
            HazardShape h;
            h.uid = o.uid, h.section = section, h.segment = prim.segment, h.kind = prim.kind;
            h.size = prim.size, h.radius = prim.radius, h.length = prim.length, h.damage = float(prim.damage);
            h.local = glm::translate(glm::mat4(1), prim.pos) * glm::mat4_cast(prim.rot);
            h.bind = size_t(prim.segment) < p.segments.size() ? p.segments[size_t(prim.segment)] : glm::mat4(1);
            h.sensor = prim.head[2] != 0 || prim.head[3] != 0;
            h.collider = -1;
            for (size_t i = 0; i < colliders_.size(); ++i)
                if (colliders_[i].uid == o.uid && colliders_[i].segment == prim.segment && colliders_[i].section == section && colliders_[i].jaw == 0) h.collider = int(i);
            h.world = introModel(o) * h.bind * h.local;
            hazards_.push_back(h);
        }
    };
    for (const auto& o : script_.objects()) {
        if (o.kind == script::Kind::Volume || o.kind == script::Kind::Plane) continue;
        if (const formats::UberObjectData* uber = prepareUber(o)) {
            size_t main = uber->sections.size();
            for (size_t k = 0; k < uber->sections.size(); ++k)
                if (uber->sections[k].model == 0) { main = k; break; }
            for (size_t k = 0; k < uber->sections.size(); ++k)
                if (k != main)
                    if (const physics::ObjectPhysics* sp = uberSectionPhysics(o, k)) add(o, *sp, int(k));
        }
        if (const physics::ObjectPhysics* p = objectPhysics(o.name)) add(o, *p, -1);
    }
    std::map<int32_t, int> nth;
    for (HazardShape& h : hazards_)
        if (const auto it = hazardRestored_.find({h.uid, nth[h.uid]++}); it != hazardRestored_.end()) h.last = it->second, h.placed = true;
    hazardRestored_.clear();
    hazardObjects_ = script_.objects().size();
}

void World::tickHazards() {
    hazardHits_.clear();
    if (hazardObjects_ != script_.objects().size()) syncHazards();
    if (hazards_.empty()) return;
    const PlayerState& s = player_.state();
    const float r = player_.tuning().radius, top = player_.tuning().capsuleTop;
    const vec3 a = s.pos + vec3(0, 0, r), b = s.pos + vec3(0, 0, std::max(top - r, r));
    int32_t struck = 0; // one message per attacker a tick
    for (HazardShape& h : hazards_) {
        const script::Object* o = script_.find(h.uid);
        h.active = false;
        const vec3 before = h.last;
        const bool placed = std::exchange(h.placed, false);
        if (!o || o->dead || o->hidden) continue; // Kill / hide: instance flag +0x1C & 0x100 [I: the same flag]
        if (h.collider >= 0 && size_t(h.collider) < colliders_.size() && size_t(h.collider) < col_.dynamics.size() &&
            colliders_[size_t(h.collider)].uid == h.uid) {
            h.world = col_.dynamics[size_t(h.collider)].transform * h.local; // intro * motion * bind segment frame
        } else if (h.section >= 0) {
            bool removed = false;
            glm::mat4 model = uberSectionBody(h.uid, h.section, &removed).value_or(glm::mat4(1));
            if (removed) continue;
            if (const auto palette = uberSectionPose(h.uid, h.section); palette && !palette->empty())
                model = model * (*palette)[std::min<size_t>(size_t(h.segment), palette->size() - 1)];
            h.world = introModel(*o) * model * h.bind * h.local;
        } else {
            glm::mat4 model = o->body;
            if (const auto rt = uberRuntime_.find(h.uid); rt != uberRuntime_.end()) {
                if (rt->second.intangible) continue;
                if (!o->animPose.empty()) model = o->animPose[std::min<size_t>(size_t(h.segment), o->animPose.size() - 1)];
            } else if (const auto palette = script_.pose(*o); palette && !palette->empty()) {
                model = (*palette)[std::min<size_t>(size_t(h.segment), palette->size() - 1)];
            }
            h.world = glm::translate(glm::mat4(1), o->offset) * introModel(*o) * model * h.bind * h.local;
        }
        h.active = true;
        h.last = vec3(h.world[3]), h.placed = true;
        if (health_.dead() || h.uid == struck) continue;
        if (hazardDistance(h, a, b) > r + kContactSkin) continue;
        if (!h.sensor && (!placed || glm::length(h.last - before) < kContactSpeed * float(kTickSeconds))) continue; // resting
        struck = h.uid;
        // [I] the script hears of the touches that hurt her: tru.exe calls it from every contact callback, whose rate
        // while she stays in touch is not known; the scripts' impact sounds would otherwise restart every tick.
        if (!damageLara(h.damage, vec3(h.world[3]), kDamageContact)) continue;
        hazardHits_.push_back({h.uid, h.damage, ticks_});
        script_.contactDamage(h.uid);
    }
}

} // namespace ou::game
