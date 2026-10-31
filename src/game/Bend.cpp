// bend_* plants and the unit wind (game/Bend.hpp). Evidence: docs/GAMEPLAY.md "Bend plants".
#include "game/Bend.hpp"

#include "physics/Physics.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ou::game {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kDt = 1.0f / 60.0f; // tru.exe: timeMult / 30 per frame; our tick is half a 30 Hz frame

template <class T> T rd(const cdc::Section& s, uint32_t off) {
    if (off + sizeof(T) > s.data.size()) throw cdc::FormatError("bend data: read past the section end");
    T v;
    std::memcpy(&v, s.data.data() + off, sizeof v);
    return v;
}

glm::mat3 introRotation(glm::vec3 r) { // Rx * Ry * Rz (formats::Intro)
    return glm::mat3(glm::rotate(glm::mat4(1), r.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), r.y, {0, 1, 0}) *
                     glm::rotate(glm::mat4(1), r.z, {0, 0, 1}));
}

} // namespace

std::shared_ptr<const BendType> loadBendType(const cdc::Drm& object) {
    const auto base = formats::objectBaseSection(object);
    if (!base) return nullptr;
    const auto ptrs = cdc::sectionPointers(object, *base);
    const auto dp = ptrs.find(0xC8);
    if (dp == ptrs.end()) return nullptr;
    const auto& s = object.sections[dp->second.section];
    const uint32_t o = dp->second.offset;
    if (rd<uint16_t>(s, o + 4) != 0xB00B || rd<uint16_t>(s, o + 2) != 57) return nullptr;
    auto t = std::make_shared<BendType>();
    t->physical = rd<uint8_t>(s, o + 8) != 0;
    const uint32_t n = rd<uint32_t>(s, o + 0x28);
    const auto dataPtrs = cdc::sectionPointers(object, dp->second.section);
    const auto bp = dataPtrs.find(o + 0x24);
    if (!n || n > 64 || bp == dataPtrs.end()) return nullptr;
    const auto phys = physics::loadObjectPhysics(object);
    t->segments = int(phys.segments.size());
    const auto& bs = object.sections[bp->second.section];
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t b = bp->second.offset + 0x48 * k;
        BendType::Bone bone;
        bone.mass = rd<float>(bs, b + 0x08), bone.linear = rd<float>(bs, b + 0x0C), bone.quadratic = rd<float>(bs, b + 0x10);
        bone.omega = rd<float>(bs, b + 0x14), bone.damping = rd<float>(bs, b + 0x18), bone.push = rd<float>(bs, b + 0x1C);
        bone.pushRadius = rd<float>(bs, b + 0x20), bone.pushPad = rd<float>(bs, b + 0x24), bone.wind = rd<float>(bs, b + 0x28);
        bone.segment = rd<int16_t>(bs, b + 0x3C);
        // The bone's first hit primitive (instance +0x160 list, byte 3 = segment): a box {size, centre} (0x48FD70). Its
        // two corners, rotated by the box quaternion [I], bound the bend (+0xC0 min / +0xD0 max, from +-10000).
        const auto box = std::find_if(phys.prims.begin(), phys.prims.end(), [&](const physics::Prim& p) { return p.segment == bone.segment; });
        if (box == phys.prims.end() || box->kind != physics::PrimKind::Box || bone.mass == 0) return nullptr;
        const glm::mat3 r = glm::mat3_cast(box->rot);
        const glm::vec3 c0 = r * (box->pos - box->size * 0.5f), c1 = r * (box->pos + box->size * 0.5f);
        bone.lo = glm::min(glm::vec3(10000), glm::min(c0, c1)), bone.hi = glm::max(glm::vec3(-10000), glm::max(c0, c1));
        bone.inv2Size = 1.0f / (2.0f * glm::vec2(box->size));
        const glm::mat4 seg = size_t(bone.segment) < phys.segments.size() ? phys.segments[size_t(bone.segment)] : glm::mat4(1);
        bone.centre = glm::vec3(seg * glm::vec4(box->pos, 1));
        t->segments = std::max(t->segments, bone.segment + 1);
        t->bones.push_back(bone);
    }
    return t;
}

// ---- wind ------------------------------------------------------------------------------------------------------------

Wind::Wind(std::vector<formats::Unit::Wind> records) {
    for (auto& r : records) sources_.push_back({r});
}

float Wind::random(float lo, float hi) {
    seed_ = seed_ * 0x41C64E6Du + 0x3039u;
    return (hi - lo) * std::sin(float((seed_ >> 16) & 0x7FFF) / 32767.0f * kPi) + lo;
}

// 0x53C3F0 with one tick elapsed: when the countdown runs out, new targets (strength x 8, heading degrees -> radians,
// wavelength x 100.60183) and a new countdown (seconds x 60) are drawn; each channel then moves to its target at
// |target - current| / countdown per tick. The frequency is set directly (Hz x 2 pi / 60).
void Wind::tick() {
    ++ticks_;
    for (auto& s : sources_) {
        if (!s.rec.enabled) continue;
        if (--s.countdown < 0) {
            s.countdown = int(random(s.rec.retarget[0], s.rec.retarget[1]) * 60.0f); // ftol truncates
            const float ticks = float(std::max(s.countdown, 1));
            auto aim = [&](Channel& c, float target) { c.target = target, c.rate = std::abs(target - c.cur) / ticks; };
            aim(s.strength, random(s.rec.strength[0], s.rec.strength[1]) * 8.0f);
            aim(s.heading, random(s.rec.heading[0], s.rec.heading[1]) * 0.017453292f);
            aim(s.wavelength, random(s.rec.wavelength[0], s.rec.wavelength[1]) * 100.60183f);
            s.frequency = s.rec.frequency * 0.10471976f;
        }
        for (Channel* c : {&s.strength, &s.heading, &s.wavelength})
            c->cur = c->cur < c->target ? std::min(c->cur + c->rate, c->target) : std::max(c->cur - c->rate, c->target);
    }
}

// 0x52E460: strength x |sin| of phase = ticks x frequency + |x + y| / wavelength (the phase is reduced modulo pi by
// truncation, so the wind pulses along its heading and never reverses), along (cos, sin) heading.
glm::vec2 Wind::at(glm::vec3 pos) const {
    glm::vec2 w(0);
    for (const auto& s : sources_) {
        if (!s.rec.enabled) continue;
        float phase = float(ticks_) * s.frequency, k = s.strength.cur;
        if (s.wavelength.cur > 0) phase += std::abs(pos.x + pos.y) / s.wavelength.cur;
        if (phase > 0) k *= std::sin(phase - kPi * std::trunc(phase / kPi));
        w += k * glm::vec2(std::cos(s.heading.cur), std::sin(s.heading.cur));
    }
    return w;
}

// ---- bend instances --------------------------------------------------------------------------------------------------

void Bends::sync(std::span<const Unit> units) {
    std::vector<Instance> next;
    std::map<std::string, Wind> winds;
    for (const auto& u : units) {
        if (auto w = winds_.find(u.name); w != winds_.end()) winds.emplace(u.name, std::move(w->second));
        else winds.emplace(u.name, Wind(u.wind ? *u.wind : std::vector<formats::Unit::Wind>{}));
        if (!u.intros) continue;
        for (const auto& in : *u.intros) {
            if (const auto it = index_.find(in.uid); it != index_.end()) {
                next.push_back(std::move(instances_[it->second]));
                continue;
            }
            Instance i;
            i.uid = in.uid, i.type = in.type, i.unit = u.name, i.pos = in.pos + u.offset;
            const glm::mat3 r = introRotation(in.rot);
            i.toLocal = glm::conjugate(glm::quat_cast(r));
            for (const auto& b : in.type->bones) i.bones.push_back({glm::vec2(0), glm::vec2(0), glm::vec2(0), i.pos + r * b.centre});
            i.out.assign(size_t(in.type->segments), glm::vec4(0));
            next.push_back(std::move(i));
        }
    }
    instances_ = std::move(next);
    winds_ = std::move(winds);
    index_.clear();
    for (size_t k = 0; k < instances_.size(); ++k) index_[instances_[k].uid] = k;
}

// 0x499B40 per bone: wind (0x490900: unit wind in instance axes x gain; rain gusts are not modelled) and Lara's push,
// then F -= 2 w zeta v + w^2 x, a = F / mass, v += a dt, x += a dt^2 / 2 + v dt, x clamped to the unit square (the
// outward velocity zeroed) and disc; the bend vector a = |x| x quadratic / span, b = linear x - |x| x quadratic / 2 -
// (min + max) a / 2 with span = min - max for x >= 0, else max - min.
// ponytail: tru.exe updates a plant only while it was drawn last frame (instance flag 0x200) or within data +0x14 of
// Lara; we update every loaded one (off-screen plants keep swaying instead of freezing; nobody sees the difference).
void Bends::tick(glm::vec3 lara, glm::vec3 laraPrev) {
    for (auto& [name, w] : winds_) w.tick();
    const glm::vec3 step = (lara - laraPrev) * 2.0f; // per 30 Hz frame, as instance +0x20 - +0xC0
    const float speed2 = glm::dot(step, step);
    for (auto& inst : instances_) {
        const BendType& t = *inst.type;
        const auto wi = winds_.find(inst.unit);
        const glm::vec2 wind = wi == winds_.end() ? glm::vec2(0) : glm::vec2(inst.toLocal * glm::vec3(wi->second.at(inst.pos), 0));
        const glm::vec3 local = inst.toLocal * (lara - inst.pos);
        for (size_t k = 0; k < t.bones.size(); ++k) {
            const BendType::Bone& p = t.bones[k];
            Bone& s = inst.bones[k];
            s.force += p.wind * wind;
            // Push: away from Lara in the plane, scaled by 1 - reach while the bend is below that. Distance plants
            // (0x490450, data +0x08 clear) within the push radius, by (speed^2 + 1); physical ones by contact
            // (0x490590 from the collision callback 0x491CF0, 0.75 speed^2 + 150) [I: contact = Lara within 100 of the box].
            const glm::vec3 d = s.pos - lara;
            const float d2 = glm::length(glm::vec2(d)), reach = std::min(d2, d2 - 3.1f * p.pushPad);
            const glm::vec2 dir = d2 > 1e-6f ? glm::vec2(d) / d2 : glm::vec2(0);
            const bool touching = t.physical && glm::all(glm::greaterThan(local, p.lo - 100.0f)) && glm::all(glm::lessThan(local, p.hi + 100.0f));
            if ((t.physical ? touching : glm::length(d) < p.pushRadius) && reach < p.pushRadius) {
                const float f = 1.0f - reach / p.pushRadius;
                if (glm::length(s.x) < f) {
                    const float scale = t.physical ? 0.75f * speed2 + 150.0f : speed2 + 1.0f;
                    s.force += glm::vec2(inst.toLocal * glm::vec3(p.push * p.inv2Size * dir * f * scale, 0));
                }
            }
            s.force -= 2.0f * p.omega * p.damping * s.v + p.omega * p.omega * s.x;
            const glm::vec2 a = s.force / p.mass;
            s.v += a * kDt;
            s.x += 0.5f * a * kDt * kDt + kDt * s.v;
            for (int c = 0; c < 2; ++c) {
                if (s.x[c] > 1) s.x[c] = 1, s.v[c] = std::min(s.v[c], 0.0f);
                if (s.x[c] < -1) s.x[c] = -1, s.v[c] = std::max(s.v[c], 0.0f);
            }
            if (const float len = glm::length(s.x); len > 1) s.x /= len;
            s.force = glm::vec2(0);
            const glm::vec2 q = glm::abs(s.x) * s.x * p.quadratic;
            const glm::vec2 span(s.x.x >= 0 ? p.lo.x - p.hi.x : p.hi.x - p.lo.x, s.x.y >= 0 ? p.lo.y - p.hi.y : p.hi.y - p.lo.y);
            const glm::vec2 ba = q / span;
            const glm::vec2 bb = p.linear * s.x - 0.5f * q - 0.5f * (glm::vec2(p.hi) + glm::vec2(p.lo)) * ba;
            if (size_t(p.segment) < inst.out.size()) inst.out[size_t(p.segment)] = glm::vec4(ba, bb);
        }
    }
}

std::span<const glm::vec4> Bends::bend(int32_t uid) const {
    const auto it = index_.find(uid);
    return it == index_.end() ? std::span<const glm::vec4>{} : std::span<const glm::vec4>(instances_[it->second].out);
}

} // namespace ou::game
