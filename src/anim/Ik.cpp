// Inverse kinematics on a sampled pose: bone turns in model space and the two-bone limb solver the limb locks use.
// The original's rules (which limbs are locked when, the probe numbers): docs/GAMEPLAY.md "Animation layers and IK".
// tru.exe solves its IK chains joint by joint (0x443D00, chain records of the instance's IK system at +0x2E0); the
// chain records are not decoded, so ours solves the three-segment limbs analytically [I].
#include "anim/Anim.hpp"

#include <cmath>

namespace ou::anim {

namespace {

glm::quat toQuat(glm::vec3 v) {
    const float angle = glm::length(v);
    if (angle < 1e-8f) return glm::quat(1, 0, 0, 0);
    return glm::angleAxis(angle, v / angle);
}
glm::vec3 toVector(glm::quat q) {
    if (q.w < 0) q = -q;
    const float s = std::sqrt(std::max(0.0f, 1.0f - q.w * q.w));
    if (s < 1e-7f) return glm::vec3(q.x, q.y, q.z) * 2.0f;
    return glm::vec3(q.x, q.y, q.z) / s * (2.0f * std::atan2(s, q.w));
}
// Rotation part of a bone matrix (scale removed).
glm::quat rotationOf(const glm::mat4& m) {
    return glm::normalize(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                                                   glm::normalize(glm::vec3(m[2])))));
}
// Model matrices of `from` and every bone after it (parents come first in a skeleton).
void refresh(const scene::Skeleton& sk, std::span<const LocalTransform> local, std::span<glm::mat4> world, uint32_t from) {
    for (size_t i = from; i < world.size() && i < local.size(); ++i) {
        glm::mat4 l(rotationFromVector(local[i].rot));
        l[0] *= local[i].scale.x;
        l[1] *= local[i].scale.y;
        l[2] *= local[i].scale.z;
        l[3] = glm::vec4(local[i].trans, 1.0f);
        const int32_t p = sk.bones[i].parent;
        world[i] = p >= 0 ? world[size_t(p)] * l : l;
    }
}
// Shortest rotation taking unit `a` to unit `b` (a half turn goes about any axis across `a`).
glm::quat between(glm::vec3 a, glm::vec3 b) {
    const float d = glm::dot(a, b);
    if (d > 0.999999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.999999f) {
        glm::vec3 axis = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::dot(axis, axis) < 1e-6f) axis = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::angleAxis(3.14159265f, glm::normalize(axis));
    }
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}

} // namespace

void rotateBoneModel(const scene::Skeleton& sk, std::span<LocalTransform> local, std::span<glm::mat4> world, uint32_t bone,
                     const glm::quat& delta) {
    if (bone >= local.size() || bone >= world.size() || bone >= sk.bones.size()) return;
    const int32_t p = sk.bones[bone].parent;
    const glm::quat parent = p >= 0 ? rotationOf(world[size_t(p)]) : glm::quat(1, 0, 0, 0);
    // world = parent * local: delta * parent * local = parent * (parent^-1 * delta * parent) * local
    local[bone].rot = toVector(glm::normalize(glm::inverse(parent) * delta * parent * toQuat(local[bone].rot)));
    refresh(sk, local, world, bone);
}

void translateBoneModel(const scene::Skeleton& sk, std::span<LocalTransform> local, std::span<glm::mat4> world, uint32_t bone,
                        glm::vec3 delta) {
    if (bone >= local.size() || bone >= world.size() || bone >= sk.bones.size()) return;
    const int32_t p = sk.bones[bone].parent;
    // the parent's frame may be scaled: take the offset through its full inverse
    local[bone].trans += p >= 0 ? glm::vec3(glm::inverse(world[size_t(p)]) * glm::vec4(delta, 0)) : delta;
    refresh(sk, local, world, bone);
}

bool solveTwoBone(const scene::Skeleton& sk, std::span<LocalTransform> local, std::span<glm::mat4> world, uint32_t end,
                  glm::vec3 target, float weight) {
    if (end >= sk.bones.size() || end >= local.size() || end >= world.size() || weight <= 0) return false;
    const int32_t mid = sk.bones[end].parent, upper = mid >= 0 ? sk.bones[size_t(mid)].parent : -1;
    if (upper < 0) return false;
    const glm::vec3 a(world[size_t(upper)][3]), b(world[size_t(mid)][3]), c(world[end][3]);
    const float l1 = glm::distance(a, b), l2 = glm::distance(b, c);
    if (l1 < 1e-3f || l2 < 1e-3f) return false;
    const glm::quat endRot = rotationOf(world[end]);
    target = glm::mix(c, target, std::min(weight, 1.0f));
    glm::vec3 to = target - a;
    float d = glm::length(to);
    if (d < 1e-4f) return false;
    to /= d;
    d = std::clamp(d, std::abs(l1 - l2) + 1e-3f, (l1 + l2) * 0.999f);
    // 1. the middle joint: its inner angle for a chain of length d, turned in the plane the limb is bent in (a
    //    straight limb: the plane of the limb and the way to the target, else across model X as a knee / elbow)
    const glm::vec3 ab = (a - b) / l1, cb = (c - b) / l2;
    glm::vec3 axis = glm::cross(ab, cb);
    if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(ab, to);
    if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(ab, glm::vec3(1, 0, 0));
    if (glm::dot(axis, axis) < 1e-8f) axis = glm::cross(ab, glm::vec3(0, 1, 0));
    axis = glm::normalize(axis);
    const float now = std::acos(std::clamp(glm::dot(ab, cb), -1.0f, 1.0f));
    const float want = std::acos(std::clamp((l1 * l1 + l2 * l2 - d * d) / (2 * l1 * l2), -1.0f, 1.0f));
    rotateBoneModel(sk, local, world, uint32_t(mid), glm::angleAxis(want - now, axis));
    // 2. the upper bone: swing the end onto the line to the target
    const glm::vec3 reach = glm::vec3(world[end][3]) - a;
    if (const float rl = glm::length(reach); rl > 1e-4f) rotateBoneModel(sk, local, world, uint32_t(upper), between(reach / rl, to));
    // 3. the end keeps the way it faced
    rotateBoneModel(sk, local, world, end, glm::normalize(endRot * glm::inverse(rotationOf(world[end]))));
    return true;
}

} // namespace ou::anim
