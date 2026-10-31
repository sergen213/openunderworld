// Pose blending, crossfading AnimController. Evidence: docs/formats/ANIMATION.md "Transitions".
#include "anim/Anim.hpp"

#include <glm/gtc/quaternion.hpp>

#include <cmath>

namespace ou::anim {

namespace {

// Axis-angle vector (length = angle, the sampler's convention) <-> unit quaternion.
glm::quat toQuat(glm::vec3 v) {
    const float angle = glm::length(v);
    if (angle < 1e-8f) return glm::quat(1, 0, 0, 0);
    return glm::angleAxis(angle, v / angle);
}
glm::vec3 toVector(glm::quat q) {
    if (q.w < 0) q = -q; // angle in [0, pi]
    const float s = std::sqrt(std::max(0.0f, 1.0f - q.w * q.w));
    if (s < 1e-7f) return glm::vec3(q.x, q.y, q.z) * 2.0f; // small-angle limit
    return glm::vec3(q.x, q.y, q.z) / s * (2.0f * std::atan2(s, q.w));
}

} // namespace

LocalTransform blend(const LocalTransform& a, const LocalTransform& b, float w) {
    if (w <= 0) return a;
    if (w >= 1) return b;
    LocalTransform o;
    o.rot = toVector(glm::slerp(toQuat(a.rot), toQuat(b.rot), w)); // glm::slerp takes the short arc
    o.scale = glm::mix(a.scale, b.scale, w);
    o.trans = glm::mix(a.trans, b.trans, w);
    return o;
}

void blendPoses(std::span<const LocalTransform> a, std::span<const LocalTransform> b, float w, std::span<LocalTransform> out) {
    for (size_t i = 0; i < out.size() && i < a.size() && i < b.size(); ++i) out[i] = blend(a[i], b[i], w);
}

double syncedStartTime(const Clip& from, double fromTime, const Clip& to) {
    const double df = from.durationSeconds(), dt = to.durationSeconds();
    if (df <= 0 || dt <= 0) return 0;
    const double phase = fromTime / df - std::floor(fromTime / df);
    return phase * dt;
}

AnimController::AnimController(const scene::Skeleton& skeleton) : skeleton_(&skeleton) {}

void AnimController::play(const Clip* clip, float blendSeconds, bool loop, float rate, double startTime, bool additive) {
    // ponytail: at most 4 layers; the oldest is dropped (a small jump) if transitions pile up faster than they fade.
    if (layers_.size() >= 4) layers_.erase(layers_.begin());
    layers_.push_back({clip, startTime, rate, layers_.empty() ? 0.0f : blendSeconds, 0.0f, loop, additive});
}

void AnimController::update(double dt) {
    for (auto& l : layers_) {
        l.time += double(l.rate) * dt;
        if (!l.loop && l.clip) l.time = std::clamp(l.time, 0.0, l.clip->durationSeconds());
        l.elapsed += float(dt);
    }
    // A layer at full weight hides everything under it (an additive one builds on it).
    for (size_t i = layers_.size(); i-- > 1;)
        if (layers_[i].weight() >= 1 && !layers_[i].additive) {
            layers_.erase(layers_.begin(), layers_.begin() + ptrdiff_t(i));
            break;
        }
}

void AnimController::setTime(double seconds) {
    if (!layers_.empty()) layers_.back().time = seconds;
}

float AnimController::weight() const { return layers_.empty() ? 1.0f : layers_.back().weight(); }

std::vector<LocalTransform> AnimController::sampleLayer(const Layer& l) const {
    return l.clip ? sample(*l.clip, *skeleton_, l.time, l.loop) : bindPose(*skeleton_);
}

std::vector<LocalTransform> AnimController::pose() const {
    if (layers_.empty()) return bindPose(*skeleton_);
    auto out = sampleLayer(layers_.front());
    for (size_t i = 1; i < layers_.size(); ++i) {
        const Layer& l = layers_[i];
        if (!l.additive) {
            blendPoses(out, sampleLayer(l), l.weight(), out);
            continue;
        }
        if (!l.clip) continue;
        // Overlay clips key only the segments they move (lara.drm hit_light_*: 12-23 of 113, the arms un-keyed) with
        // values relative to a reference pose: the spine / arm channels end at exactly 0, the rest repeat full-clip
        // constants (seg 57, 109, 111, 112) or a +-pi flip (76, 101). Taking the change from the clip's last key
        // keeps the motion and cancels the constants [I: the original's overlay node is not decoded].
        const auto now = sample(*l.clip, *skeleton_, l.time, l.loop), ref = sample(*l.clip, *skeleton_, l.clip->durationSeconds(), false);
        const float w = l.weight();
        for (size_t s = 0; s < out.size() && s < l.clip->tracks.size(); ++s) {
            const auto& t = l.clip->tracks[s];
            if (!t[0].empty() || !t[1].empty() || !t[2].empty())
                out[s].rot = toVector(toQuat(out[s].rot) * glm::slerp(glm::quat(1, 0, 0, 0), glm::inverse(toQuat(ref[s].rot)) * toQuat(now[s].rot), w));
            out[s].trans += w * (now[s].trans - ref[s].trans);
        }
    }
    return out;
}

} // namespace ou::anim
