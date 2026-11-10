// Limb locks and the head look-at over a sampled pose (Ik.hpp). Evidence: docs/GAMEPLAY.md "Animation layers and IK".
#include "game/Ik.hpp"

#include <algorithm>
#include <cmath>

namespace ou::game {

namespace {

constexpr float kPi = 3.14159265358979f;

glm::quat rotationOf(const glm::mat4& m) {
    return glm::normalize(glm::quat_cast(glm::mat3(glm::normalize(glm::vec3(m[0])), glm::normalize(glm::vec3(m[1])),
                                                   glm::normalize(glm::vec3(m[2])))));
}
glm::quat between(glm::vec3 a, glm::vec3 b) {
    const float d = glm::dot(a, b);
    if (d > 0.999999f) return glm::quat(1, 0, 0, 0);
    if (d < -0.999999f) return glm::angleAxis(kPi, glm::vec3(0, 0, 1));
    return glm::angleAxis(std::acos(d), glm::normalize(glm::cross(a, b)));
}
float wrap(float a) { return a - 2 * kPi * std::floor((a + kPi) / (2 * kPi)); }
// Model forward is -Y, left +X, up +Z: yaw (+ = left) and pitch (+ = up) of a unit direction.
float yawOf(glm::vec3 d) { return std::atan2(d.x, -d.y); }
float pitchOf(glm::vec3 d) { return std::asin(std::clamp(d.z, -1.0f, 1.0f)); }

bool has(const scene::Skeleton& sk, std::span<const glm::mat4> world, int bone) {
    return bone >= 0 && size_t(bone) < sk.bones.size() && size_t(bone) < world.size();
}

} // namespace

const LimbLock* IkTuning::lock(uint32_t type, uint16_t limb) const {
    const auto it = std::ranges::find_if(locks, [&](const LimbLock& l) { return l.type == type && l.limb == limb; });
    return it != locks.end() ? &*it : nullptr;
}

FootPlant plantFeet(const scene::Skeleton& sk, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
                    const glm::mat4& toWorld, const collision::CollisionMesh& col, const IkTuning& tuning, float minNormalZ,
                    float weight) {
    FootPlant out;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0 || local.size() != sk.bones.size() || world.size() != sk.bones.size()) return out;
    const glm::vec3 up = glm::normalize(glm::vec3(toWorld[2])), origin(toWorld[3]);
    const glm::mat4 toModel = glm::inverse(toWorld);
    struct Foot {
        const LimbLock* rec = nullptr;
        int bone = -1;
        glm::vec3 ankle{0}, normal{0, 0, 1};
        float clipHeight = 0, move = 0; // ankle over the root plane in the clip; how far the ankle goes along `up`
        float w = 0;                    // this foot's share of `weight`
    } feet[2];
    for (int f = 0; f < 2; ++f) {
        Foot& ft = feet[f];
        ft.rec = tuning.lock(1, uint16_t(0x30 + f));
        if (!ft.rec) continue;
        ft.bone = ft.rec->segment > 0 ? ft.rec->segment : tuning.foot[size_t(f)];
        if (!has(sk, world, ft.bone)) continue;
        ft.ankle = glm::vec3(toWorld * world[size_t(ft.bone)][3]);
        // 0x438F60: one ray from `up` over the effector to `down` under it, along the instance's up axis
        const auto hit = col.raycast(ft.ankle + up * ft.rec->up, -up, ft.rec->up + ft.rec->down);
        const float nz = hit ? glm::dot(hit->normal, up) : 0.0f;
        if (!hit || nz < minNormalZ) continue;
        out.planted[size_t(f)] = true;
        out.ground[size_t(f)] = hit->point;
        ft.normal = hit->normal;
        ft.clipHeight = glm::dot(ft.ankle - origin, up);
        // The floor's height over the root plane, and the room a tilted sole needs: the record holds the ankle
        // offset.z off the floor along its normal (0x438F60: hit + normal * offset.z), which is offset.z / nz above it.
        ft.move = glm::dot(hit->point - origin, up) + ft.rec->offset.z * (1.0f / nz - 1.0f);
        // A lifted foot is the clip's: full hold while the floor is in the nearer half of the probe under the ankle,
        // none where the probe ends [I: tru.exe locks a foot for its support phase (anim events 0x34..0x36 -> player
        // +0xF3C; controller messages 0x37 / 0x38), which the clips' events would give; the fade keeps the
        // pose continuous without them].
        const float gap = glm::dot(ft.ankle - hit->point, up);
        ft.w = weight * std::clamp((ft.rec->down - gap) / (ft.rec->down * 0.5f), 0.0f, 1.0f);
    }
    float drop = 0;
    for (int f = 0; f < 2; ++f)
        if (out.planted[size_t(f)]) drop = std::min(drop, feet[f].move * feet[f].w);
    const glm::vec3 upModel = glm::normalize(glm::vec3(toModel * glm::vec4(up, 0)));
    if (drop < -1e-3f && has(sk, world, tuning.pelvis)) {
        anim::translateBoneModel(sk, local, world, uint32_t(tuning.pelvis), glm::vec3(toModel * glm::vec4(up * drop, 0)));
        out.pelvis = -drop;
    }
    for (int f = 0; f < 2; ++f) {
        const Foot& ft = feet[f];
        if (!out.planted[size_t(f)]) continue;
        const bool level = glm::dot(ft.normal, up) > 0.99999f;
        if (std::abs(ft.move) > 1e-3f || out.pelvis > 0)
            anim::solveTwoBone(sk, local, world, uint32_t(ft.bone), glm::vec3(toModel * glm::vec4(ft.ankle + up * (ft.move * ft.w), 1)));
        if ((ft.rec->flags & 0x10) && !level) { // 0x443B50: the effector turns with the surface
            const glm::vec3 n = glm::normalize(glm::vec3(toModel * glm::vec4(ft.normal, 0)));
            anim::rotateBoneModel(sk, local, world, uint32_t(ft.bone), glm::slerp(glm::quat(1, 0, 0, 0), between(upModel, n), ft.w));
        }
        const glm::vec3 now(toWorld * world[size_t(ft.bone)][3]);
        out.error[size_t(f)] = glm::dot(now - origin, up) - (ft.clipHeight + ft.move * ft.w);
    }
    return out;
}

std::array<float, 2> lockHands(const scene::Skeleton& sk, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
                               const glm::mat4& toWorld, std::span<const glm::vec3> line, const IkTuning& tuning, float weight) {
    std::array<float, 2> out{};
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (line.size() < 2 || local.size() != sk.bones.size() || world.size() != sk.bones.size()) return out;
    const glm::mat4 toModel = glm::inverse(toWorld);
    float total = 0;
    for (size_t i = 0; i + 1 < line.size(); ++i) total += glm::distance(line[i], line[i + 1]);
    const auto at = [&](float s) { // the polyline's point `s` along it
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const float l = glm::distance(line[i], line[i + 1]);
            if (s <= l || i + 2 == line.size()) return l > 0 ? glm::mix(line[i], line[i + 1], std::clamp(s / l, 0.0f, 1.0f)) : line[i];
            s -= l;
        }
        return line.back();
    };
    for (int h = 0; h < 2; ++h) {
        const int bone = tuning.hand[size_t(h)];
        if (!tuning.lock(7, uint16_t(0x40 + h)) || !has(sk, world, bone)) continue;
        const glm::vec3 p(toWorld * world[size_t(bone)][3]);
        float best = 1e30f, bestS = 0, s0 = 0;
        for (size_t i = 0; i + 1 < line.size(); ++i) {
            const glm::vec3 ab = line[i + 1] - line[i];
            const float l = glm::length(ab);
            const float t = l > 0 ? std::clamp(glm::dot(p - line[i], ab) / (l * l), 0.0f, 1.0f) : 0.0f;
            if (const float d = glm::distance(p, line[i] + ab * t); d < best) best = d, bestS = s0 + t * l;
            s0 += l;
        }
        if (total > 2 * tuning.ledgeEndGap) bestS = std::clamp(bestS, tuning.ledgeEndGap, total - tuning.ledgeEndGap);
        const glm::vec3 q = at(bestS);
        out[size_t(h)] = glm::distance(p, q);
        if (weight > 0 && out[size_t(h)] > 1e-3f) anim::solveTwoBone(sk, local, world, uint32_t(bone), glm::vec3(toModel * glm::vec4(q, 1)), weight);
    }
    return out;
}

glm::vec2 lookAt(const scene::Skeleton& sk, std::span<anim::LocalTransform> local, std::span<glm::mat4> world,
                 const glm::mat4& toWorld, glm::vec3 target, const IkTuning& tuning, float weight) {
    if (!has(sk, world, tuning.head) || !has(sk, world, tuning.neck) || local.size() != sk.bones.size()) return {0, 0};
    const glm::mat4 toModel = glm::inverse(toWorld);
    const glm::vec3 to = glm::vec3(toModel * glm::vec4(target, 1)) - glm::vec3(world[size_t(tuning.head)][3]);
    if (glm::dot(to, to) < 1e-6f) return {0, 0};
    const glm::vec3 d = glm::normalize(to), fwd(0, -1, 0);
    const glm::vec3 nf = rotationOf(world[size_t(tuning.neck)]) * fwd, hf = rotationOf(world[size_t(tuning.head)]) * fwd;
    // 0x6FD080: the angles to the target less the neck's own, each clamped to its limit
    const float yaw = std::clamp(wrap(yawOf(d) - yawOf(nf)), -tuning.lookYaw, tuning.lookYaw);
    const float pitch = std::clamp(pitchOf(d) - pitchOf(nf), -tuning.lookPitch, tuning.lookPitch);
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight <= 0) return {yaw, pitch};
    const float wantYaw = yawOf(nf) + yaw, wantPitch = std::clamp(pitchOf(nf) + pitch, -1.5f, 1.5f);
    const glm::vec3 want(std::sin(wantYaw) * std::cos(wantPitch), -std::cos(wantYaw) * std::cos(wantPitch), std::sin(wantPitch));
    const glm::quat turn = glm::angleAxis(wrap(wantYaw - yawOf(hf)), glm::vec3(0, 0, 1)); // about her up axis, then the nod
    const glm::quat delta = glm::normalize(between(glm::normalize(turn * hf), want) * turn);
    anim::rotateBoneModel(sk, local, world, uint32_t(tuning.head), glm::slerp(glm::quat(1, 0, 0, 0), delta, weight));
    return {yaw, pitch};
}

float aimArm(const scene::Skeleton& sk, std::span<anim::LocalTransform> local, std::span<glm::mat4> world, const glm::mat4& toWorld,
             int hand, glm::vec3 from, glm::vec3 to, float maxAngle, float weight) {
    if (!has(sk, world, hand) || local.size() != sk.bones.size()) return 0;
    const int32_t fore = sk.bones[size_t(hand)].parent, upper = fore >= 0 ? sk.bones[size_t(fore)].parent : -1;
    if (upper < 0) return 0;
    const glm::mat4 toModel = glm::inverse(toWorld);
    const glm::vec3 shoulder(world[size_t(upper)][3]);
    const glm::vec3 a = glm::vec3(toModel * glm::vec4(from, 1)) - shoulder, b = glm::vec3(toModel * glm::vec4(to, 1)) - shoulder;
    if (glm::dot(a, a) < 1e-6f || glm::dot(b, b) < 1e-6f) return 0;
    glm::quat q = between(glm::normalize(a), glm::normalize(b));
    float angle = glm::angle(q);
    if (angle < 1e-5f) return 0;
    if (angle > maxAngle) q = glm::angleAxis(maxAngle, glm::axis(q)), angle = maxAngle;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (weight > 0) anim::rotateBoneModel(sk, local, world, uint32_t(upper), glm::slerp(glm::quat(1, 0, 0, 0), q, weight));
    return angle;
}

} // namespace ou::game
