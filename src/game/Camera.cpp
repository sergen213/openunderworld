// Scripted reveal cameras: tru.exe AutoSplineCamera playing a unit camera spline (formats::CameraSpline) through two
// CameraSpline players, the camera track and the look-at target track. docs/GAMEPLAY.md "Cameras".
#include "game/Game.hpp"

#include <algorithm>
#include <cmath>

#include <glm/gtc/quaternion.hpp>

namespace ou::game {

namespace {
constexpr float kCameraBlendSeconds = 0.5f; // [I] blend from the last spline pose back to the orbit camera
constexpr float kCameraHoldSeconds = 10.0f; // [I] safety: a finished spline nobody switches away from ends anyway
constexpr uint32_t kAutoSplineCamera = 8;    // camera type of the AutoSplineCamera (0x7759F0: CameraManager +0xE4(8))
}

struct SplineCamera {
    // CameraSpline (0x60A000..0x60AF90): state 1 active, 2 paused, 3 done (names: 0x60A030).
    struct Track {
        const formats::CameraSplineTrack* data = nullptr;
        vec3 offset{0}; // unit offset + spline offset (0x60A3A0)
        int state = 1, resume = 1;
        float pause = 0, time = 0, u = 0;
        size_t node = 0;
        std::vector<float> lengths; // constant-speed mode: arc length of each segment (0x60A3F0, 128 steps)
        float length = 0, duration = 0, scale = 1; // constant-speed seconds; key-time scale (0x60A270)
        vec3 pos{0};
        const std::vector<formats::CameraSplineEvent>* events = nullptr; // pending key events (0x5E3100 / 0x5E25E0)
        size_t next = 0;
    };
    std::string unit;
    std::vector<formats::CameraSpline> splines; // the unit's
    vec3 unitOffset{0};
    const formats::CameraSpline* spline = nullptr;
    uint32_t value = 0; // the cameraspline script value reported when it finishes
    Track camera, target;
    bool hasTarget = false, ended = false, notified = false;
    float held = 0; // seconds since it finished
    int fovMode = 0; // 0 spline FOV, 1 towards fovTo, 2 back from fovFrom (0x5E2950)
    float fov = 50, fovTo = 0, fovFrom = 0, fovTime = 0, fovDuration = 0;
};

namespace {

vec3 v3(const float* f) { return {f[0], f[1], f[2]}; }

// 0x5F7340: cubic Hermite through p0, p1 with tangents m0, m1.
vec3 hermite(vec3 p0, vec3 m0, vec3 p1, vec3 m1, float u) {
    const float u2 = u * u, u3 = u2 * u;
    return (2 * u3 - 3 * u2 + 1) * p0 + (u3 - 2 * u2 + u) * m0 + (u3 - u2) * m1 + (3 * u2 - 2 * u3) * p1;
}

// 0x60A880: the segment's 64-entry u16 ease curve at u.
float ease(const formats::CameraSplineKey& k, float u) {
    const float x = std::clamp(u, 0.0f, 1.0f) * 63;
    const int i = int(x), j = std::min(i + 1, 63);
    return float(int(float(k.ease[j] - k.ease[i]) * (x - float(i)) + float(k.ease[i])) & 0xFFFF) / 65535.0f;
}

// 0x60A100: a target key may follow Lara.
vec3 nodePos(const SplineCamera::Track& t, size_t i, vec3 lara) {
    const auto& k = t.data->keys[i];
    return k.followPlayer ? lara : v3(k.pos) + t.offset;
}

vec3 segment(const SplineCamera::Track& t, vec3 lara) {
    const auto& k = t.data->keys;
    if (t.node + 1 >= k.size()) return nodePos(t, k.size() - 1, lara);
    return hermite(nodePos(t, t.node, lara), v3(k[t.node].tangent), nodePos(t, t.node + 1, lara),
                   v3(k[t.node + 1].tangent), t.u);
}

float trackLength(const formats::CameraSplineTrack& d) { // 0x60A240, seconds
    return d.mode == 2 ? d.duration : d.keys.empty() ? 0.0f : d.keys.back().time;
}

// 0x60AC00 / 0x60ADA0 setup, 0x60A310 reset to key 0, 0x60A350 play. `duration` > 0: 0x60A270 rescales the track
// to that length (a target track fitted to its camera track).
void setup(SplineCamera::Track& t, const formats::CameraSplineTrack& data, vec3 offset, float duration, vec3 lara) {
    t = {};
    t.data = &data;
    t.offset = offset;
    t.duration = data.duration;
    if (data.mode == 2)
        for (size_t i = 0; i + 1 < data.keys.size(); ++i) {
            float len = 0;
            vec3 prev = v3(data.keys[i].pos);
            for (int s = 1; s <= 128; ++s) {
                const vec3 p = hermite(v3(data.keys[i].pos), v3(data.keys[i].tangent), v3(data.keys[i + 1].pos),
                                       v3(data.keys[i + 1].tangent), float(s) / 128);
                len += glm::length(p - prev), prev = p;
            }
            t.lengths.push_back(len);
            t.length += len;
        }
    if (duration > 0 && data.mode == 2) t.duration = duration;
    else if (duration > 0 && trackLength(data) > 0) t.scale = duration / trackLength(data);
    t.pos = nodePos(t, 0, lara);
    t.events = &data.keys[0].events;
}

} // namespace

bool World::startSplineCamera(const std::string& unit, uint32_t index, uint32_t value) {
    const LoadedUnit* u = findUnit(unit);
    if (!u || index >= u->data.cameraSplines.size() || u->data.cameraSplines[index].camera.keys.empty()) return false;
    const bool fresh = !splineCam_;
    if (!splineCam_ || splineCam_->unit != unit) {
        splineCam_ = std::make_shared<SplineCamera>();
        splineCam_->unit = unit;
        splineCam_->splines = u->data.cameraSplines;
        splineCam_->unitOffset = u->offset;
    }
    SplineCamera& c = *splineCam_;
    // 0x5E26F0: both tracks get unit offset + spline offset; a fitting target track takes the camera track's length.
    c.spline = &c.splines[index];
    c.value = value;
    c.ended = c.notified = false, c.held = 0;
    const vec3 offset = c.unitOffset + v3(c.spline->offset), lara = player_.state().pos;
    setup(c.camera, c.spline->camera, offset, 0, lara);
    c.hasTarget = bool(c.spline->target);
    if (c.hasTarget)
        setup(c.target, *c.spline->target, offset, c.spline->targetFitsCamera ? trackLength(c.spline->camera) : 0, lara);
    c.fov = c.spline->fovDeg, c.fovMode = 0, c.fovTime = 0;
    if (fresh) cameraCut_ = true, blendOut_ = 0;
    return true;
}

bool World::cameraNative(int op, const std::string& unit, uint32_t index, uint32_t value) {
    if (op == 0) return startSplineCamera(unit, index, value);
    if (op == 2) return (splineCam_ ? kAutoSplineCamera : 1u) == index;
    // op 1, CameraManager +0x50 (0x5EDFC0): switching to the AutoSplineCamera keeps the spline autosplinecamera 0
    // started (ours plays from there [I]: the exe updates it only while current); any other type leaves it.
    if (index != kAutoSplineCamera && splineCam_) splineCam_->ended = true;
    return true;
}

bool World::tickSplineCamera(float dt) {
    if (!splineCam_) return false;
    const auto self = splineCam_;
    SplineCamera& c = *self;
    const vec3 lara = player_.state().pos;
    // Key event lists (camera 0x5E3100, target 0x5E25E0) run until one pauses the track. False: the camera spline
    // switched (event 1, 0x5E26F0 with the event's spline) or ended.
    auto runEvents = [&](SplineCamera::Track& t, bool camera) {
        for (int guard = 0; t.events && t.next < t.events->size() && t.state != 2 && guard < 64; ++guard) {
            const auto e = (*t.events)[t.next++];
            if (camera && e.type == 1) {
                if (!startSplineCamera(c.unit, e.arg, (c.value & 0xFFFF0000u) | (e.arg & 0xFFFF))) c.ended = true;
                return false;
            }
            if (camera && e.type == 2) { c.ended = true; return false; } // camera manager +0x50(1, 0) [I: back to gameplay]
            if (!camera && e.type == 1) c.fovMode = 1, c.fovTo = e.value, c.fovDuration = e.value2, c.fovTime = 0;
            if (!camera && e.type == 2) c.fovMode = 2, c.fovFrom = c.fov, c.fovDuration = e.value, c.fovTime = 0;
            if (e.type == 3 && (t.state == 1 || t.state == 3)) t.resume = t.state, t.pause = e.value, t.state = 2; // 0x609FD0
            // 0 none, 7 [U] (0x5EE890 on its argument), 5/6 screen wipes: no shipped spline has them
        }
        return true;
    };
    // 0x60AF90: paused tracks count down (0x60A000 resumes and the listener continues the event list); active ones
    // advance by key times (0x60A950, modes 1/3) or at constant speed (0x60A540, mode 2).
    auto advance = [&](SplineCamera::Track& t, bool camera) {
        if (t.state == 2) {
            if (t.pause < dt) t.pause = 0, t.state = t.resume, runEvents(t, camera);
            else t.pause -= dt;
            return true;
        }
        if (t.state != 1) return true;
        const auto& keys = t.data->keys;
        const size_t n = keys.size();
        bool finished = n < 2;
        auto enter = [&](size_t node) { t.node = node, t.events = &keys[node].events, t.next = 0; return runEvents(t, camera); };
        if (!finished && t.data->mode != 2) {
            const float t0 = keys[t.node].time * t.scale, t1 = keys[t.node + 1].time * t.scale;
            t.u = t1 > t0 ? (t.time - t0) / (t1 - t0) : 1;
            if (t.u > 1) {
                if (t.node + 2 < n) {
                    if (!enter(t.node + 1)) return false;
                    t.u -= 1; // as the exe: not recomputed for the new segment
                } else {
                    t.u = 1, finished = true;
                }
            }
            if (t.data->mode == 3) t.u = ease(keys[t.node], t.u);
        } else if (!finished) {
            float d = t.duration > 0 ? t.length * dt / t.duration : t.length + 1;
            while (d > 0) {
                const float len = std::max(t.lengths[t.node], 1e-3f), left = (1 - t.u) * len;
                if (d < left) { t.u += d / len; break; }
                d -= left;
                if (t.node + 2 >= n) { t.u = 1, finished = true; break; }
                if (!enter(t.node + 1)) return false;
                t.u = 0;
            }
        }
        t.pos = segment(t, lara);
        t.time += dt;
        if (finished) {
            t.state = 3;
            if (!enter(n - 1)) return false; // 0x5E3430: the last key's events (a final hold)
        }
        return true;
    };

    auto end = [&] {
        blendFrom_ = cam_;
        blendOut_ = kCameraBlendSeconds;
        splineCam_.reset();
        return false;
    };
    if (c.ended) return end(); // a script switched cameras (gamebase 52)
    // Key 0's events (Play reports key 0) or a resumed list; a switch restarts this camera on another spline.
    for (int i = 0; i < 8 && !runEvents(c.camera, true); ++i)
        if (c.ended) return end();
    if (c.camera.state == 3) {
        // 0x5E2CA0: dispatchers are told once; the camera holds its last pose until a script switches cameras
        // (gamebase 52, the reveal scripts' done callbacks).
        if (!c.notified) {
            c.notified = true;
            script_.cameraSplineDone(c.value);
            if (splineCam_ != self || !c.notified) return bool(splineCam_); // a callback started another spline
        }
        if (c.ended || (c.held += dt) > kCameraHoldSeconds) return end();
        return true;
    }
    if (!advance(c.camera, true)) return c.ended ? end() : true;
    // Orientation: slerp of the keys' quaternions over the segment (0x5E2510 -> 0x49D860); local x right, y down,
    // z forward. A target track replaces the view direction (look-at, 0x5E2AB0) keeping the roll reference.
    const auto& keys = c.camera.data->keys;
    auto quat = [](const formats::CameraSplineKey& k) { return glm::quat(k.rotation[3], k.rotation[0], k.rotation[1], k.rotation[2]); };
    const glm::quat q = c.camera.node + 1 < keys.size() ? glm::slerp(quat(keys[c.camera.node]), quat(keys[c.camera.node + 1]), c.camera.u)
                                                       : quat(keys.back());
    vec3 forward = q * vec3(0, 0, 1);
    const vec3 up = -(q * vec3(0, 1, 0));
    if (c.hasTarget) {
        advance(c.target, false);
        if (const vec3 d = c.target.pos - c.camera.pos; glm::length(d) > 1e-3f) forward = glm::normalize(d);
    }
    // FOV (0x5E2950): the spline's, or blending towards / back from a target-key FOV event.
    const float base = c.spline->fovDeg;
    const float f = c.fovDuration < 1e-6f ? 1.0f : std::min(1.0f, c.fovTime / c.fovDuration);
    if (c.fovMode == 1) c.fov = base + f * (c.fovTo - base);
    else if (c.fovMode == 2) c.fov = (base - c.fovFrom) * f + c.fovFrom, c.fovMode = c.fovDuration < c.fovTime ? 0 : 2;
    else c.fov = base;
    c.fovTime += dt;
    cam_.position = c.camera.pos;
    cam_.target = c.camera.pos + forward * 1000.0f;
    cam_.up = glm::length(glm::cross(forward, up)) > 1e-3f ? up : vec3(0, 0, 1);
    cam_.fovDeg = c.fov;
    return true;
}

void World::updateCamera(const Actions& a, float dt) {
    if (tickSplineCamera(dt)) return;
    updateOrbitCamera(a, dt);
    cam_.up = vec3(0, 0, 1), cam_.fovDeg = 0;
    if (blendOut_ <= 0) return;
    blendOut_ = std::max(0.0f, blendOut_ - dt);
    const float s = glm::smoothstep(0.0f, 1.0f, 1 - blendOut_ / kCameraBlendSeconds);
    cam_.position = glm::mix(blendFrom_.position, cam_.position, s);
    cam_.target = glm::mix(blendFrom_.target, cam_.target, s);
    cam_.up = glm::normalize(glm::mix(blendFrom_.up, cam_.up, s));
    cam_.fovDeg = blendOut_ > 0 ? glm::mix(blendFrom_.fovDeg, fovDeg, s) : 0;
}

} // namespace ou::game
