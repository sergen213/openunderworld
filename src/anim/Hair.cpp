// Hair: position-based Verlet chains on the render model's cloth particles and constraints.
// Evidence and deviations from tru.exe's cloth solver (0x45FAB0): docs/formats/ANIMATION.md "Hair".
#include "anim/Anim.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ou::anim {

namespace {

glm::vec3 closestOnSegment(glm::vec3 a, glm::vec3 b, glm::vec3 p) {
    const glm::vec3 ab = b - a;
    const float l2 = glm::dot(ab, ab);
    return l2 < 1e-8f ? a : a + ab * std::clamp(glm::dot(p - a, ab) / l2, 0.0f, 1.0f);
}

// Shortest rotation taking unit `from` to unit `to`.
glm::quat swing(glm::vec3 from, glm::vec3 to) {
    const float d = glm::dot(from, to);
    if (d > 1 - 1e-6f) return glm::quat(1, 0, 0, 0);
    if (d < -1 + 1e-6f) { // opposite: any perpendicular axis
        glm::vec3 axis = glm::cross(from, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(from, glm::vec3(0, 1, 0));
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    const glm::vec3 c = glm::cross(from, to);
    return glm::normalize(glm::quat(1 + d, c.x, c.y, c.z));
}

} // namespace

HairSim::HairSim(const HairRig& rig, const scene::Skeleton& sk, HairParams params) : rig_(rig), p_(std::move(params)) {
    bind_ = bindPositions(sk);
    for (const auto& pt : rig_.particles)
        if (pt.bone >= bind_.size()) { rig_ = {}; return; }
    child_.assign(rig_.particles.size(), -1);
    for (size_t i = rig_.particles.size(); i-- > 0;)
        if (const int32_t p = rig_.particles[i].pred; p >= 0) child_[size_t(p)] = int32_t(i);
    // Clamp each body capsule so that no free particle starts inside it (bind pose, model space).
    const std::vector<glm::mat4> bindBones = [&] {
        std::vector<glm::mat4> m(bind_.size(), glm::mat4(1));
        for (size_t i = 0; i < m.size(); ++i) m[i][3] = glm::vec4(bind_[i], 1);
        return m;
    }();
    for (auto& c : p_.body) {
        if (c.boneA >= bind_.size() || c.boneB >= bind_.size()) { c.radius = 0; continue; }
        const glm::vec3 a = capsulePoint(c, bindBones, glm::mat4(1), false), b = capsulePoint(c, bindBones, glm::mat4(1), true);
        for (const auto& pt : rig_.particles)
            if (!pt.pinned) c.radius = std::min(c.radius, 0.9f * glm::length(bind_[pt.bone] - closestOnSegment(a, b, bind_[pt.bone])));
    }
}

glm::vec3 HairSim::capsulePoint(const HairCapsule& c, std::span<const glm::mat4> bones, const glm::mat4& toWorld, bool end) const {
    const glm::vec4 local = end ? bones[c.boneB] * glm::vec4(c.offsetB, 1) : bones[c.boneA][3];
    return glm::vec3(toWorld * local);
}

void HairSim::reset(std::span<const glm::mat4> bones, const glm::mat4& toWorld) {
    pos_.resize(rig_.particles.size());
    for (size_t i = 0; i < pos_.size(); ++i) pos_[i] = glm::vec3(toWorld * bones[rig_.particles[i].bone][3]);
    prev_ = target_ = pos_;
}

void HairSim::step(std::span<const glm::mat4> bones, const glm::mat4& toWorld, float dt) {
    if (empty()) return;
    if (pos_.size() != rig_.particles.size()) reset(bones, toWorld);
    for (size_t i = 0; i < pos_.size(); ++i) {
        const glm::vec3 target(toWorld * bones[rig_.particles[i].bone][3]);
        const glm::vec3 carried = (target - target_[i]) * p_.follow; // moved with the body, no velocity change
        target_[i] = target;
        if (rig_.particles[i].pinned) {
            prev_[i] = pos_[i] = target;
            continue;
        }
        const HairGroupParams& gp = p_.group(rig_.particles[i].group);
        const glm::vec3 v = (pos_[i] - prev_[i]) * (1 - gp.damping);
        prev_[i] = pos_[i] + carried;
        pos_[i] += carried + v + glm::vec3(0, 0, -gp.gravity * dt * dt);
        pos_[i] += (target - pos_[i]) * gp.stiffness;
    }
    std::vector<std::pair<glm::vec3, glm::vec3>> caps;
    for (const auto& c : p_.body)
        if (c.radius > 0) caps.emplace_back(capsulePoint(c, bones, toWorld, false), capsulePoint(c, bones, toWorld, true));
    for (int it = 0; it < p_.iterations; ++it) {
        for (const auto& l : rig_.links) {
            const float wa = rig_.particles[l.a].pinned ? 0.0f : 1.0f, wb = rig_.particles[l.b].pinned ? 0.0f : 1.0f;
            if (wa + wb == 0) continue;
            const glm::vec3 d = pos_[l.b] - pos_[l.a];
            const float len = glm::length(d);
            if (len < 1e-6f) continue;
            const glm::vec3 corr = d * ((len - l.rest) / (len * (wa + wb)));
            pos_[l.a] += corr * wa;
            pos_[l.b] -= corr * wb;
        }
        size_t k = 0;
        for (const auto& c : p_.body) {
            if (c.radius <= 0) continue;
            const auto [a, b] = caps[k++];
            for (size_t i = 0; i < pos_.size(); ++i) {
                if (rig_.particles[i].pinned) continue;
                const glm::vec3 q = closestOnSegment(a, b, pos_[i]), d = pos_[i] - q;
                const float len = glm::length(d);
                if (len < c.radius && len > 1e-6f) pos_[i] = q + d * (c.radius / len);
            }
        }
    }
}

void HairSim::apply(std::span<glm::mat4> bones, const glm::mat4& toWorld) const {
    if (pos_.size() != rig_.particles.size()) return;
    const glm::mat4 toModel = glm::inverse(toWorld);
    std::vector<glm::vec3> m(pos_.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = glm::vec3(toModel * glm::vec4(pos_[i], 1));
    for (size_t i = 0; i < m.size(); ++i) {
        const auto& pt = rig_.particles[i];
        glm::mat4& out = bones[pt.bone];
        // Swing the animated orientation so the bone's strand segment (to its child; tips: from their
        // predecessor) points where it was simulated.
        const int32_t c = child_[i], from = c >= 0 ? int32_t(i) : pt.pred, to = c >= 0 ? c : int32_t(i);
        if (from >= 0) {
            const glm::vec3 animDir = glm::mat3(out) * (bind_[rig_.particles[size_t(to)].bone] - bind_[rig_.particles[size_t(from)].bone]);
            const glm::vec3 simDir = m[size_t(to)] - m[size_t(from)];
            if (glm::dot(animDir, animDir) > 1e-8f && glm::dot(simDir, simDir) > 1e-8f)
                out = glm::mat4(glm::mat3_cast(swing(glm::normalize(animDir), glm::normalize(simDir))) * glm::mat3(out));
        }
        out[3] = glm::vec4(m[i], 1);
    }
}

} // namespace ou::anim
