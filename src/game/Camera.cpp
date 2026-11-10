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

const std::string& World::cameraUnit() const { return splineCam_ ? splineCam_->unit : currentUnit().data.name; }

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
    cameraSwing_ = 0;
    if (tickSplineCamera(dt)) return;
    if (fieldCamera_) { // from her eyes along the look angles; zoom narrows the view and slows the look (World::tick)
        const float fov = glm::mix(fovDeg * 0.75f, 6.0f, fieldZoom_), slow = fov / (fovDeg * 0.75f);
        cam_.yaw += a.look.x * dt * slow;
        cam_.pitch = std::clamp(cam_.pitch + a.look.y * dt * slow, -1.2f, 1.2f);
        const vec3 dir(std::cos(cam_.pitch) * std::cos(cam_.yaw), std::cos(cam_.pitch) * std::sin(cam_.yaw), std::sin(cam_.pitch));
        cam_.position = player_.state().pos + vec3(0, 0, targeting.eyeHeight) + dir * 30.0f; // [I] in front of her face
        cam_.target = cam_.position + dir * 1000.0f;
        cam_.up = vec3(0, 0, 1), cam_.fovDeg = fov;
        orbit_.valid = false; // the orbit reseeds behind her when it ends
        return;
    }
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

// A playing spline camera is saved as its spline (unit, index) and both tracks' clocks; the rest is rebuilt from the
// unit's spline data as startSplineCamera does. Positions are in the spline unit's frame.
nlohmann::json World::serializeRuntime() const {
    const auto v = [](vec3 p) { return nlohmann::json{p.x, p.y, p.z}; };
    const auto limits = [&](const OrbitState::Limits& l) { return nlohmann::json{v(l.offset), v(l.zones), l.yawRange, l.pitchLo, l.pitchHi}; };
    const auto forces = [](const OrbitState::Forces& f) {
        return nlohmann::json{f.set, f.fromSet, f.fromBase, f.fromSpeed, f.fromSign, f.sign, f.blend};
    };
    const auto& o = orbit_;
    nlohmann::json hint = nullptr; // the markup as (unit, local index): global indices depend on the streamed set
    if (const auto owner = o.hintMarkup >= 0 ? markupOwner(o.hintMarkup) : std::nullopt) hint = {owner->first, owner->second};
    nlohmann::json hair = nlohmann::json::array();
    for (const vec3& p : hair_.state()) hair.push_back(v(p));
    // Lara's crossfade in progress (clips by name), the aim poses' clock and the pose itself (the camera focus reads it).
    const auto clipName = [&](const anim::Clip* c) -> nlohmann::json {
        for (const auto& [name, clip] : anims_.clips)
            if (&clip == c) return name;
        return nullptr;
    };
    nlohmann::json layers = nlohmann::json::array(), bones = nlohmann::json::array();
    for (const auto& l : anim_.layerStates())
        layers.push_back({clipName(l.clip), l.time, l.rate, l.fade, l.elapsed, l.loop, l.additive, clipName(l.mixClip), l.mix, l.mirror});
    for (const glm::mat4& m : bones_)
        for (int k = 0; k < 16; ++k) bones.push_back(m[k / 4][k % 4]);
    nlohmann::json spline = nullptr;
    if (const SplineCamera* c = splineCam_.get(); c && c->spline) {
        const auto track = [&](const SplineCamera::Track& t) {
            return nlohmann::json{t.state, t.resume, t.pause, t.time, t.u, t.node, v(t.pos - c->unitOffset), t.next};
        };
        spline = {{"unit", c->unit}, {"index", c->spline - c->splines.data()}, {"value", c->value}, {"camera", track(c->camera)},
                  {"target", c->hasTarget ? track(c->target) : nlohmann::json(nullptr)}, {"ended", c->ended}, {"notified", c->notified},
                  {"held", c->held}, {"fov", {c->fovMode, c->fov, c->fovTo, c->fovFrom, c->fovTime, c->fovDuration}}};
    }
    const auto pose = [&](const CameraState& c) { return nlohmann::json{v(c.position), v(c.target), v(c.up), c.fovDeg}; };
    return {{"camera", {{"position", v(cam_.position)}, {"target", v(cam_.target)}, {"yaw", cam_.yaw}, {"pitch", cam_.pitch},
                        {"up", v(cam_.up)}, {"fov", cam_.fovDeg},
                        {"focus", focus_ ? v(*focus_) : nlohmann::json(nullptr)},
                        {"orbit", {v(o.desired), o.dist, o.speed, o.valid, o.yaw, o.pitch, limits(o.from), limits(o.now), o.mode,
                                   o.modeTime, o.idle, std::vector<float>(o.moves, o.moves + 10), v(o.lastPos), v(o.hint), hint,
                                   forces(o.yawForce), forces(o.pitchForce), o.along}},
                        {"spline", std::move(spline)}, {"blendOut", blendOut_}, {"blendFrom", pose(blendFrom_)}}},
            {"hair", std::move(hair)}, {"playerUnit", playerUnit_}, {"air", air_}, {"submerged", submerged_},
            {"hingeLimits", {multibodyLo_, multibodyHi_}}, {"limitsOff", multibodyLimitOff_}, {"anim", std::move(layers)}, {"aimTime", aimTime_}, {"bones", std::move(bones)},
            {"water", [&] {
                 nlohmann::json w = nlohmann::json::array();
                 for (const auto& [uid, s] : waterStates_)
                     w.push_back({uid, s.surface, s.target, s.rate, s.source, s.sink, s.strength, v(s.sourceAt), v(s.sinkAt)});
                 return w;
             }()}};
}

void World::restoreRuntime(const nlohmann::json& j) {
    if (!j.is_object()) return;
    try {
        const auto v = [](const nlohmann::json& a) { return vec3(a.at(0).get<float>(), a.at(1).get<float>(), a.at(2).get<float>()); };
        const auto limits = [&](const nlohmann::json& a) {
            return OrbitState::Limits{v(a.at(0)), v(a.at(1)), a.at(2).get<float>(), a.at(3).get<float>(), a.at(4).get<float>()};
        };
        const auto& cam = j.at("camera");
        const auto& a = cam.at("orbit");
        OrbitState o;
        o.desired = v(a.at(0)), o.dist = a.at(1), o.speed = a.at(2), o.valid = a.at(3), o.yaw = a.at(4), o.pitch = a.at(5);
        o.from = limits(a.at(6)), o.now = limits(a.at(7)), o.mode = a.at(8), o.modeTime = a.at(9), o.idle = a.at(10);
        const auto moves = a.at(11).get<std::vector<float>>();
        if (moves.size() != 10) return;
        std::copy(moves.begin(), moves.end(), o.moves);
        o.lastPos = v(a.at(12)), o.hint = v(a.at(13));
        if (const auto& h = a.at(14); h.is_array()) o.hintMarkup = markupIndex(h.at(0).get<std::string>(), h.at(1).get<int>());
        const auto forces = [](const nlohmann::json& f) {
            return OrbitState::Forces{f.at(0).get<int>(),   f.at(1).get<int>(),   f.at(2).get<float>(), f.at(3).get<float>(),
                                      f.at(4).get<float>(), f.at(5).get<float>(), f.at(6).get<float>()};
        };
        if (a.size() > 17) o.yawForce = forces(a.at(15)), o.pitchForce = forces(a.at(16)), o.along = a.at(17);
        else o.mode = -1; // older saves: the next tick picks the mode's force sets afresh
        const auto& f = cam.at("focus");
        const std::optional<vec3> focus = f.is_null() ? std::nullopt : std::optional<vec3>(v(f));
        CameraState c = cam_;
        c.position = v(cam.at("position")), c.target = v(cam.at("target")), c.yaw = cam.at("yaw"), c.pitch = cam.at("pitch");
        std::vector<vec3> hair;
        for (const auto& p : j.at("hair")) hair.push_back(v(p));
        const std::string playerUnit = j.at("playerUnit");
        const float air = j.at("air");
        const bool submerged = j.at("submerged");
        auto lo = j.at("hingeLimits").at(0).get<std::map<int32_t, float>>(), hi = j.at("hingeLimits").at(1).get<std::map<int32_t, float>>();
        std::vector<anim::AnimController::Layer> layers;
        for (const auto& l : j.at("anim")) {
            const auto clip = [&](const nlohmann::json& n) { return n.is_null() ? nullptr : anims_.find(n.get<std::string>()); };
            layers.push_back({clip(l.at(0)), l.at(1).get<double>(), l.at(2).get<float>(), l.at(3).get<float>(), l.at(4).get<float>(),
                              l.at(5).get<bool>(), l.at(6).get<bool>(), clip(l.at(7)), l.at(8).get<float>(),
                              l.size() > 9 && l.at(9).get<bool>()}); // older saves: no mirror
        }
        const double aimTime = j.at("aimTime");
        const auto bones = j.at("bones").get<std::vector<float>>();
        air_ = air, submerged_ = submerged;
        waterStates_.clear(); // scripted water levels (older saves: none)
        for (const auto& e : j.value("water", nlohmann::json::array()))
            waterStates_[e.at(0).get<int32_t>()] = {e.at(1).get<float>(), e.at(2).get<float>(), e.at(3).get<float>(), e.at(4).get<int16_t>(),
                                                    e.at(5).get<int16_t>(), e.at(6).get<float>(), v(e.at(7)), v(e.at(8))};
        for (const auto& [uid, s] : waterStates_)
            if (const auto [m, u] = waterVolume(uid); m) applyWaterState(*m, u->offset);
        multibodyLo_ = std::move(lo), multibodyHi_ = std::move(hi); // the hinges take them when the next sync builds them
        multibodyLimitOff_ = j.value("limitsOff", std::set<int32_t>{}); // older saves: every limit on
        syncedObjects_ = ~size_t(0);
        focus_ = focus, orbit_ = o, cam_ = prevCam_ = c;
        playerUnit_ = playerUnit; // the scripts heard of this unit already: no level-load vt 13 again (World::tick)
        hair_.setState(hair);
        if (bones.size() == bones_.size() * 16)
            for (size_t i = 0; i < bones_.size(); ++i)
                for (int k = 0; k < 16; ++k) bones_[i][k / 4][k % 4] = bones[i * 16 + size_t(k)];
        prevBones_ = bones_;
        anim_.setLayers(std::move(layers));
        aimTime_ = aimTime;
        // Newer saves: the camera's roll / FOV, the blend out of a spline, a playing spline camera.
        if (cam.contains("up")) cam_.up = prevCam_.up = v(cam.at("up")), cam_.fovDeg = prevCam_.fovDeg = cam.at("fov").get<float>();
        if (const auto& b = cam.value("blendFrom", nlohmann::json(nullptr)); b.is_array()) {
            blendFrom_.position = v(b.at(0)), blendFrom_.target = v(b.at(1)), blendFrom_.up = v(b.at(2)), blendFrom_.fovDeg = b.at(3).get<float>();
            blendOut_ = cam.at("blendOut").get<float>();
        }
        splineCam_.reset();
        const auto& sp = cam.value("spline", nlohmann::json(nullptr));
        const LoadedUnit* u = sp.is_object() ? findUnit(sp.at("unit").get<std::string>()) : nullptr;
        const size_t index = u ? sp.at("index").get<size_t>() : 0;
        if (u && index < u->data.cameraSplines.size() && !u->data.cameraSplines[index].camera.keys.empty()) {
            auto s = std::make_shared<SplineCamera>();
            s->unit = u->data.name, s->splines = u->data.cameraSplines, s->unitOffset = u->offset;
            s->spline = &s->splines[index];
            s->value = sp.at("value").get<uint32_t>();
            const vec3 offset = s->unitOffset + v3(s->spline->offset), lara = player_.state().pos;
            const auto track = [&](SplineCamera::Track& t, const formats::CameraSplineTrack& data, float duration, const nlohmann::json& a) {
                setup(t, data, offset, duration, lara);
                t.state = a.at(0), t.resume = a.at(1), t.pause = a.at(2), t.time = a.at(3), t.u = a.at(4);
                t.node = std::min(a.at(5).get<size_t>(), data.keys.size() - 1);
                t.pos = v(a.at(6)) + s->unitOffset;
                t.events = &data.keys[t.node].events, t.next = a.at(7);
            };
            track(s->camera, s->spline->camera, 0, sp.at("camera"));
            s->hasTarget = s->spline->target && sp.at("target").is_array();
            if (s->hasTarget)
                track(s->target, *s->spline->target, s->spline->targetFitsCamera ? trackLength(s->spline->camera) : 0, sp.at("target"));
            s->ended = sp.at("ended"), s->notified = sp.at("notified"), s->held = sp.at("held");
            const auto& f = sp.at("fov");
            s->fovMode = f.at(0), s->fov = f.at(1), s->fovTo = f.at(2), s->fovFrom = f.at(3), s->fovTime = f.at(4), s->fovDuration = f.at(5);
            splineCam_ = std::move(s);
        }
    } catch (const nlohmann::json::exception&) { // older / odd saves: as restore() seeded them
    }
}

} // namespace ou::game
