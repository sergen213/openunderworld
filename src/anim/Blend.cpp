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

std::vector<float> layerBlendWeights(uint8_t type, float x, float y, size_t count) {
    // tru.exe 0x437BA0, case by case; the slots each case writes (0x433330 zeroes the rest).
    std::vector<float> w;
    switch (type) {
    case 2: case 8: w = {1 - x, x}; break;
    case 3: w = {(1 - x) * (1 - y), (1 - y) * x, y}; break;
    case 4: w = {(1 - x) * (1 - y), (1 - y) * x, (1 - x) * y, x * y}; break;
    case 5:
        if (x >= 0) w = {0, (1 - x) * (1 - y), x * (1 - y), 0, y * (1 - x), y * x};
        else {
            const float f = x + 1;
            w = {(1 - f) * (1 - y), f * (1 - y), 0, y * (1 - f), y * f, 0};
        }
        break;
    case 6: w = x < 0 ? std::vector<float>{-x, x + 1, 0} : std::vector<float>{0, 1 - x, x}; break;
    case 7: w = x < 0 ? std::vector<float>{x + 1, -x, 0} : std::vector<float>{1 - x, 0, x}; break;
    case 9: {
        float a = x, b = y, s = std::abs(x) + std::abs(y);
        if (s > 1) a /= s, b /= s, s = 1;
        w = {1 - s, std::max(-a, 0.0f), std::max(a, 0.0f), std::max(-b, 0.0f), std::max(b, 0.0f)};
        break;
    }
    case 10: {
        w.assign(9, 0.0f);
        float v = y + y;
        const bool high = v >= 1;
        if (high) v -= 1;
        const size_t row = high ? 3 : 0; // the 2 x 2 cell's lower row in the 3 x 3 grid
        if (x >= 0) w[row + 1] = (1 - x) * (1 - v), w[row + 2] = x * (1 - v), w[row + 4] = v * (1 - x), w[row + 5] = x * v;
        else {
            const float f = x + 1;
            w[row] = (1 - f) * (1 - v), w[row + 1] = f * (1 - v), w[row + 3] = v * (1 - f), w[row + 4] = v * f;
        }
        break;
    }
    case 11:
        if (x < 0) w = {1 - y, 0, 0, y};
        else if (x < 0.5f) w = {(1 - y) * (1 - (x + x)), (1 - y) * (x + x), 0, y};
        else if (x < 1) {
            const float f = (x - 0.5f) * 2;
            w = {0, (1 - y) * (1 - f), (1 - y) * f, y};
        } else w = {0, 0, 1 - y, y};
        break;
    default: w = {1}; break; // 0, 1, 12..0x13; >= 0x14: the layer's callback +0xDC [U]
    }
    w.resize(count, 0.0f);
    return w;
}

bool layerBlendSyncsRates(uint8_t type) { return type == 2 || type == 3 || type == 4 || type == 9; }

std::vector<LocalTransform> blendPosesWeighted(std::span<const std::vector<LocalTransform>> poses, std::span<const float> weights) {
    if (poses.empty()) return {};
    std::vector<LocalTransform> out;
    float total = 0;
    for (size_t k = 0; k < poses.size() && k < weights.size(); ++k) {
        const float w = weights[k];
        if (w <= 0) continue;
        if (out.empty()) out = poses[k];
        else blendPoses(out, poses[k], w / (total + w), out);
        total += w;
    }
    return out.empty() ? poses[0] : out;
}

std::vector<uint32_t> mirrorBones(const scene::Skeleton& skeleton) {
    const auto bind = bindPositions(skeleton);
    std::vector<uint32_t> twins(bind.size());
    for (size_t i = 0; i < bind.size(); ++i) {
        twins[i] = uint32_t(i);
        const glm::vec3 want(-bind[i].x, bind[i].y, bind[i].z);
        // Coincident bones (helpers at one spot): the one under the parent's twin (parents come first).
        const int32_t p = skeleton.bones[i].parent, wantParent = p >= 0 ? int32_t(twins[size_t(p)]) : -1;
        float best = 1.0f;
        for (size_t j = 0; j < bind.size(); ++j)
            if (const float d = glm::distance(bind[j], want) + (skeleton.bones[j].parent == wantParent ? 0.0f : 0.5f); d < best)
                best = d, twins[i] = uint32_t(j);
    }
    for (size_t i = 0; i < twins.size(); ++i) // one-sided matches (asymmetric helpers, hair) stay themselves
        if (twins[twins[i]] != i) twins[i] = uint32_t(i);
    return twins;
}

void mirrorPose(std::span<LocalTransform> pose, std::span<const uint32_t> twins) {
    const std::vector<LocalTransform> in(pose.begin(), pose.end());
    for (size_t i = 0; i < pose.size() && i < twins.size(); ++i) {
        const LocalTransform& t = in[twins[i] < in.size() ? twins[i] : i];
        pose[i] = {{t.rot.x, -t.rot.y, -t.rot.z}, t.scale, {-t.trans.x, t.trans.y, t.trans.z}};
    }
}

AnimController::AnimController(const scene::Skeleton& skeleton) : skeleton_(&skeleton) {}

void AnimController::play(const Clip* clip, float blendSeconds, bool loop, float rate, double startTime, bool additive,
                          const Clip* mixClip, float mix, bool mirror, std::vector<const Clip*> partners,
                          std::vector<float> weights) {
    // ponytail: at most 4 layers; the oldest is dropped (a small jump) if transitions pile up faster than they fade.
    if (layers_.size() >= 4) layers_.erase(layers_.begin());
    layers_.push_back({clip, startTime, rate, layers_.empty() ? 0.0f : blendSeconds, 0.0f, loop, additive, mixClip, mix, mirror,
                       std::move(partners), std::move(weights)});
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

void AnimController::setWeights(std::vector<float> weights) {
    if (!layers_.empty()) layers_.back().weights = std::move(weights);
}

float AnimController::weight() const { return layers_.empty() ? 1.0f : layers_.back().weight(); }

std::vector<LocalTransform> AnimController::sampleLayer(const Layer& l) const {
    if (!l.clip) return bindPose(*skeleton_);
    auto out = sample(*l.clip, *skeleton_, l.time, l.loop);
    if (!l.weights.empty()) {
        // The anim layer blend: every sub-layer at its own phase-synced time, weighted (layerBlendWeights).
        std::vector<std::vector<LocalTransform>> poses{std::move(out)};
        for (const Clip* p : l.partners)
            poses.push_back(p ? sample(*p, *skeleton_, partnerTime(*l.clip, *p, l.time), l.loop) : poses.front());
        out = blendPosesWeighted(poses, l.weights);
    }
    // Blend space (a multi-clip anim request): the other clip at the same phase (tru.exe 0x4428A0: t_i = t_0/len_0 * len_i).
    else if (l.mixClip && l.mix > 0)
        blendPoses(out, sample(*l.mixClip, *skeleton_, l.time / l.clip->durationSeconds() * l.mixClip->durationSeconds(), l.loop), l.mix, out);
    if (l.mirror) {
        if (twins_.size() != skeleton_->bones.size()) twins_ = mirrorBones(*skeleton_); // the skeleton may be re-rigged
        mirrorPose(out, twins_);
    }
    return out;
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
