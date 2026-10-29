// See Fx.hpp and docs/formats/EFFECTS.md. tru.exe references: particle start 0x543F70, emitter update 0x540B80,
// particle update 0x54C300, sprite draw 0x54AEC0, effect command switch 0x547B40, birth events 0x5BA480 -> 0x5BA000.
#include "fx/Fx.hpp"

#include "formats/Formats.hpp"
#include "physics/Physics.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>

namespace ou::fx {

namespace {

template <class T>
T rd(std::span<const uint8_t> d, size_t off) {
    T v{};
    if (off + sizeof(T) <= d.size()) std::memcpy(&v, d.data() + off, sizeof v);
    return v;
}
glm::vec3 vec3At(std::span<const uint8_t> d, size_t off) { return {rd<float>(d, off), rd<float>(d, off + 4), rd<float>(d, off + 8)}; }

// DTP section index with this id, or -1.
int dtpIndex(const cdc::Drm& drm, uint32_t id) {
    for (size_t i = 0; i < drm.sections.size(); ++i)
        if (drm.sections[i].type == cdc::SectionType::DtpData && drm.sections[i].id == id) return int(i);
    return -1;
}

// Resource reference to a DTP at `off` of the object section (the id stays in the data), resolved in the DRM.
int objectDtp(const cdc::Drm& drm, const cdc::Section& obj, uint32_t off) {
    if (cdc::resourceRefType(obj, off) != int(cdc::SectionType::DtpData)) return -1;
    return dtpIndex(drm, rd<uint32_t>(obj.data, off));
}

float frac(int16_t percent) { return std::clamp(float(percent) / 100.0f, 0.0f, 1.0f); }

} // namespace

ParticleDef parseParticleDef(std::span<const uint8_t> d) {
    ParticleDef p;
    p.size0 = rd<float>(d, 0x00);
    p.flags4 = rd<uint8_t>(d, 0x04), p.flags5 = rd<uint8_t>(d, 0x05), p.mode = rd<int8_t>(d, 0x06);
    p.segment = rd<int16_t>(d, 0x0A), p.segment2 = rd<int16_t>(d, 0x0C);
    p.radiusMax = rd<float>(d, 0x10), p.radiusMin = rd<float>(d, 0x6C); // emitter +0x90 / +0x92 (0x5445B8)
    p.velocity = vec3At(d, 0x14), p.velocityRandom = vec3At(d, 0x20), p.accel = vec3At(d, 0x2C), p.offset = vec3At(d, 0x4C);
    p.colour0 = rd<uint32_t>(d, 0x38), p.colourLast = rd<uint32_t>(d, 0x3C);
    p.duration = rd<int16_t>(d, 0x44), p.life = std::max<uint16_t>(1, rd<uint16_t>(d, 0x46)), p.lifeVar = rd<uint8_t>(d, 0x7C);
    p.rate = rd<float>(d, 0x48);
    p.fadeFar = rd<uint16_t>(d, 0x5A), p.fadeNear = rd<uint16_t>(d, 0x5C);
    p.screenFadeStart = rd<float>(d, 0x60), p.screenFadeEnd = rd<float>(d, 0x64);
    p.shape = rd<uint8_t>(d, 0x68);
    p.maxDistance = rd<float>(d, 0x74), p.minDistance = rd<float>(d, 0x78);
    p.radialSegment = rd<int16_t>(d, 0x94), p.velocitySegment = rd<int16_t>(d, 0x96), p.offsetSegment = rd<int16_t>(d, 0xAA);
    p.animStart = rd<int8_t>(d, 0x91), p.animRate = rd<uint8_t>(d, 0xB1);
    p.rot0 = rd<int16_t>(d, 0xF4), p.rotRange = rd<int16_t>(d, 0xF6);
    p.spin = rd<float>(d, 0xF8), p.spinRange = rd<float>(d, 0xFC);
    p.texture = rd<uint32_t>(d, 0x110);
    p.depthBias = rd<int16_t>(d, 0x15E);
    p.atlasCols = rd<uint8_t>(d, 0x174), p.atlasRows = rd<uint8_t>(d, 0x175);
    p.atlasFrames = rd<uint8_t>(d, 0x176), p.atlasFirst = rd<uint8_t>(d, 0x177);
    // Keys (0x544078..0x5442B9): key 0 always; middle keys when their colour is set or their size is >= 0; last key.
    const auto size = [](float s) { return s >= 0 ? std::trunc(s) : -1.0f; };
    p.keys.push_back({p.colour0, std::trunc(p.size0), rd<int16_t>(d, 0x86), 0.0f, rd<uint8_t>(d, 0x11C), rd<float>(d, 0xE0)});
    for (const auto [c, s, v, t, k] : {std::tuple{0xB8, 0xBC, 0xC0, 0xCE, 1}, std::tuple{0xC4, 0xC8, 0xCC, 0xD0, 2}})
        if (rd<uint32_t>(d, c) != 0 || rd<float>(d, s) >= 0)
            p.keys.push_back({rd<uint32_t>(d, c), size(rd<float>(d, s)), rd<int16_t>(d, v), frac(rd<int16_t>(d, t)),
                              rd<uint8_t>(d, 0x11C + k), rd<float>(d, 0xE0 + 4 * k)});
    const float last = rd<float>(d, 0x80);
    p.keys.push_back({p.colourLast, size(last), last >= 0 ? rd<int16_t>(d, 0x84) : rd<int16_t>(d, 0x86), 1.0f,
                      rd<uint8_t>(d, 0x11F), rd<float>(d, 0xEC)});
    for (size_t k = 1; k < p.keys.size(); ++k) p.keys[k].time = std::max(p.keys[k].time, p.keys[k - 1].time);
    // Middle colour 0 = interpolated between its neighbours (0x5441F6 loop over emitter +0x148 keys, 0x548D40).
    for (size_t k = 1; k + 1 < p.keys.size(); ++k)
        if (!p.keys[k].rgba) {
            const auto& a = p.keys[k - 1];
            size_t n = k + 1;
            while (n + 1 < p.keys.size() && !p.keys[n].rgba) ++n;
            const auto& b = p.keys[n];
            const float f = b.time > a.time ? (p.keys[k].time - a.time) / (b.time - a.time) : 0.0f;
            uint32_t out = 0;
            for (int c = 0; c < 32; c += 8)
                out |= uint32_t(std::lround(float((a.rgba >> c) & 0xFF) * (1 - f) + float((b.rgba >> c) & 0xFF) * f)) << c;
            p.keys[k].rgba = out;
        }
    return p;
}

std::optional<ObjectFx> parseObjectFx(const cdc::Drm& drm, std::vector<std::string>* warnings) {
    const auto obj = std::find_if(drm.sections.begin(), drm.sections.end(), [](const auto& s) { return s.type == cdc::SectionType::Object; });
    if (obj == drm.sections.end() || obj->data.size() < 12) return std::nullopt;
    const int ev = objectDtp(drm, *obj, 4), gen = objectDtp(drm, *obj, 8);
    if (ev < 0 && gen < 0) return std::nullopt;
    ObjectFx fx;
    if (ev >= 0) { // records until 0xFFFF, 8 + (info >> 12) * 4 bytes each
        const auto d = drm.sections[size_t(ev)].data;
        for (size_t o = 0; o + 2 <= d.size();) {
            const auto info = rd<uint16_t>(d, o);
            if (info == 0xFFFF) break;
            Event e{info, rd<uint16_t>(d, o + 2), rd<uint8_t>(d, o + 4),
                    {rd<uint16_t>(d, o + 8), rd<uint16_t>(d, o + 10), rd<uint16_t>(d, o + 12), rd<uint16_t>(d, o + 14)}};
            fx.events.push_back(e);
            o += 8 + size_t(info >> 12) * 4;
        }
    }
    if (gen >= 0) {
        const auto& s = drm.sections[size_t(gen)];
        const auto ptrs = cdc::sectionPointers(drm, uint32_t(gen));
        const uint32_t n = rd<uint32_t>(s.data, 0x0C);
        if (const auto it = ptrs.find(0x10); n && it != ptrs.end()) {
            const auto& arr = drm.sections[it->second.section];
            const auto aptrs = it->second.section == uint32_t(gen) ? ptrs : cdc::sectionPointers(drm, it->second.section);
            for (uint32_t k = 0; k < n && n < 4096; ++k) {
                const size_t off = it->second.offset + kParticleDefSize * k;
                if (off + kParticleDefSize > arr.data.size()) {
                    if (warnings) warnings->push_back("particle definition " + std::to_string(k) + " past the end of its section");
                    break;
                }
                auto def = parseParticleDef(arr.data.subspan(off, kParticleDefSize));
                if (const auto t = aptrs.find(uint32_t(off + 0x114)); t != aptrs.end()) {
                    const auto td = drm.sections[t->second.section].data;
                    const auto cnt = rd<uint32_t>(td, t->second.offset);
                    for (uint32_t i = 0; i < cnt && i < 256; ++i) def.textures.push_back(rd<uint32_t>(td, t->second.offset + 4 + 4 * i));
                }
                fx.particles.push_back(std::move(def));
            }
        }
    }
    try {
        const auto phys = physics::loadObjectPhysics(drm);
        fx.segments = phys.segments;
        for (const auto& m : phys.hmarkers) {
            const glm::mat4 seg = m.segment < int(phys.segments.size()) ? phys.segments[size_t(m.segment)] : glm::mat4(1);
            glm::mat4 r = glm::translate(glm::mat4(1), m.pos);
            r = glm::rotate(r, m.rot.x, {1, 0, 0}), r = glm::rotate(r, m.rot.y, {0, 1, 0}), r = glm::rotate(r, m.rot.z, {0, 0, 1});
            fx.markers.emplace(m.id, seg * r);
        }
    } catch (const std::exception& e) {
        if (warnings) warnings->push_back(std::string("effect segments: ") + e.what());
    }
    return fx;
}

glm::mat4 ObjectFx::frame(int16_t s) const {
    if (s >= 0) return size_t(s) < segments.size() ? segments[size_t(s)] : glm::mat4(1);
    const auto it = markers.find(-s);
    return it != markers.end() ? it->second : glm::mat4(1); // -125 (0x538570) and ids >= 100 (runtime list): root [U]
}

std::vector<uint32_t> ObjectFx::textureIds() const {
    std::vector<uint32_t> out;
    for (const auto& p : particles) {
        if (p.model()) continue;
        if (!p.textures.empty()) out.insert(out.end(), p.textures.begin(), p.textures.end());
        else if (p.texture) out.push_back(p.texture);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

Blend blendFor(uint8_t keyBlend) {
    switch (keyBlend & 7) {
    case 1: return Blend::Alpha;    // mode 4
    case 3: return Blend::Alpha;    // mode 0 (opaque) [I] drawn as alpha
    case 4: return Blend::Subtract; // mode 6
    case 5: return Blend::Multiply; // mode 7
    default: return Blend::Additive; // mode 5
    }
}

uint32_t System::rand15() {
    seed_ = seed_ * 0x41C64E6Du + 0x3039u;
    return (seed_ >> 16) & 0x7FFF;
}

bool System::start(std::shared_ptr<const ObjectFx> fx, int def, const glm::mat4& world, int32_t owner, int16_t segment,
                   int16_t segment2) {
    if (!fx || def < 0 || size_t(def) >= fx->particles.size() || fx->particles[size_t(def)].model()) return false;
    const auto& d = fx->particles[size_t(def)];
    auto at = [&](int16_t s) { return world * fx->frame(s); };
    Emitter e;
    e.def = &d, e.owner = owner;
    const int16_t s1 = segment ? segment : d.segment, s2 = segment2 ? segment2 : d.segment2;
    e.origin = at(s1)[3], e.origin2 = d.shape == 7 && s2 != kNoSegment ? glm::vec3(at(s2)[3]) : e.origin;
    if (d.velocitySegment != kNoSegment) e.velocityFrame = glm::mat3(at(d.velocitySegment));
    if (d.offsetSegment != kNoSegment) e.offsetFrame = glm::mat3(at(d.offsetSegment));
    e.burst = d.duration == 0;
    e.ticksLeft = d.duration < 0 ? -1 : 2 * d.duration;
    e.fx = std::move(fx);
    emitters_.push_back(std::move(e));
    return true;
}

bool System::runCommand(const std::shared_ptr<const ObjectFx>& fx, const Command& c, const glm::mat4& world, int32_t owner) {
    switch (c.type) {
    case 3: return start(fx, c.a1, world, owner, int16_t(c.a2 == 0xFF ? 0 : c.a2), int16_t(c.a3)); // particle (seg, seg2)
    case 0x1D: return start(fx, c.a1, world, owner);                                                // particle, root
    case 0x20: return start(fx, c.a1, world, owner);
    case 4: { // the particle on every segment from a2 (0x547C8E)
        bool any = false;
        const int n = c.a3 ? c.a3 : int(fx->segments.size());
        for (int s = c.a2; s < n; ++s) any |= start(fx, c.a1, world, owner, int16_t(s));
        return any;
    }
    default: return false; // other effect classes: docs/formats/EFFECTS.md "Missing"
    }
}

int System::startBirth(const std::shared_ptr<const ObjectFx>& fx, const glm::mat4& world, int32_t owner) {
    int n = 0;
    if (!fx) return 0;
    for (const auto& e : fx->events)
        if (e.effect() && e.event == 0 && !(e.info & 0x3F0)) n += runCommand(fx, e.cmd, world, owner);
    return n;
}

void System::stopOwner(int32_t owner) {
    for (auto& e : emitters_)
        if (e.owner == owner) e.stopped = true;
}

bool System::hasOwner(int32_t owner) const {
    return std::any_of(emitters_.begin(), emitters_.end(), [&](const Emitter& e) { return e.owner == owner && !e.stopped; });
}

void System::clear() { emitters_.clear(), particles_.clear(); }

void System::spawn(Emitter& e, const glm::vec3& camera) {
    const ParticleDef& d = *e.def;
    Particle p;
    p.def = &d, p.fx = e.fx, p.id = nextId_++;
    // Shell radius in [min, max] (0x5416E0), shape offset (switch on emitter +0x3C = def +0x68).
    const float r = d.radiusMin + rand01() * (d.radiusMax - d.radiusMin);
    glm::vec3 off(0);
    if (r != 0) switch (d.shape) {
        case 0: case 1: case 7: {
            glm::vec3 v(randSigned(), randSigned(), randSigned());
            const float len = std::max(glm::length(v), 1e-4f);
            off = v * (r / len);
            if (d.shape == 1) off.z = std::abs(off.z);
            break;
        }
        case 2: case 8: { const float a = rand01() * 6.2831853f; off = {std::cos(a) * r, std::sin(a) * r, 0}; break; }
        case 3: { const float a = rand01() * 6.2831853f; off = {std::cos(a) * r, 0, std::sin(a) * r}; break; }
        case 4: { const float a = rand01() * 6.2831853f; off = {0, std::cos(a) * r, std::sin(a) * r}; break; }
        default: break; // 5/6 mesh surface, 9 camera: [U]
        }
    const glm::vec3 base = e.offsetFrame * (d.offset + off); // def +0xAA: offset in that segment's frame
    const float t = rand01();
    p.pos = e.origin + t * (e.origin2 - e.origin) + base;
    // Velocity: base + random range, in a segment frame when +0x96 is set, halved (30 Hz authoring -> 60 Hz ticks).
    glm::vec3 v = d.velocity + glm::vec3(randSigned() * d.velocityRandom.x, randSigned() * d.velocityRandom.y,
                                         randSigned() * d.velocityRandom.z);
    p.vel = e.velocityFrame * v * 0.5f;
    p.acc = d.accel * 0.25f; // emitter +0xA8 = def +0x2C * 0.25 (0x544331)
    int life = d.life;
    if (d.lifeVar) life += int(rand15() % (2u * d.lifeVar + 1u)) - d.lifeVar;
    p.life = std::max(1, life) * 2; // key times scale by 2 * life (0x54223F >> 11)
    for (size_t k = 0; k < d.keys.size() && k < 4; ++k) {
        const auto& key = d.keys[k];
        p.sizes[k] = key.size;
        if (key.size >= 0 && key.sizeVar) p.sizes[k] = std::max(0.0f, key.size + float(int(rand15() % (2u * key.sizeVar + 1u)) - key.sizeVar));
    }
    constexpr float kDeg = 3.14159265f / 180.0f;
    p.rot = (d.rot0 + (rand01() - 0.5f) * d.rotRange) * kDeg;
    p.spin = (d.spin + (rand01() - 0.5f) * d.spinRange) * kDeg * 0.5f; // [I] degrees per 30 Hz frame
    const size_t cells = size_t(d.atlasCols) * d.atlasRows;
    const size_t frames = !d.textures.empty() ? d.textures.size() : cells ? (d.atlasFrames ? d.atlasFrames : cells) : 1;
    p.frame = uint16_t(rand15() % frames);
    (void)camera;
    particles_.push_back(std::move(p));
    if (d.mode == 2) e.persistent = particles_.back().id;
}

void System::tick(const glm::vec3& camera) {
    for (auto& p : particles_) {
        ++p.age;
        p.pos += p.vel, p.vel += p.acc, p.rot += p.spin;
    }
    for (auto& e : emitters_) {
        if (e.stopped) continue;
        const ParticleDef& d = *e.def;
        const float dist2 = glm::dot(camera - e.origin, camera - e.origin);
        // Emitter +0x2C / +0x30 = def +0x74 / +0x78 squared: outside that camera range nothing spawns (0x540D82).
        const bool inRange = !(d.maxDistance > 0 && dist2 > d.maxDistance * d.maxDistance) &&
                             !(d.minDistance > 0 && dist2 < d.minDistance * d.minDistance);
        if (d.mode == 2) { // one persistent particle (emitter +0x11 == 2) [I]: spawned once, kept alive
            auto it = std::find_if(particles_.begin(), particles_.end(), [&](const Particle& p) { return p.id == e.persistent; });
            if (it == particles_.end()) spawn(e, camera);
            else it->age = std::min(it->age, it->life / 2);
        } else if (inRange) {
            int n = 0;
            if (e.burst) n = std::max(1, int(d.rate)), e.stopped = true;
            else {
                const float rate = d.rate < 0 ? 0.5f / -d.rate : d.rate * 0.5f; // emitter +0x34 (0x5446EB)
                if (rate >= 0.5f) {
                    const float a = e.accum + rate;
                    n = int(a), e.accum = a - float(n);
                } else n = float(rand15()) <= rate * 32767.0f;
            }
            for (int k = 0; k < n && particles_.size() < 20000; ++k) spawn(e, camera);
        }
        if (e.ticksLeft > 0 && --e.ticksLeft == 0) e.stopped = true;
    }
    std::erase_if(particles_, [](const Particle& p) { return p.age >= p.life; });
    std::erase_if(emitters_, [&](const Emitter& e) {
        return e.stopped && std::none_of(particles_.begin(), particles_.end(), [&](const Particle& p) { return p.def == e.def && p.fx == e.fx; });
    });
}

void System::build(const render::Camera& camera, float aspect, std::vector<render::FxVertex>& vertices,
                   std::vector<render::FxBatch>& batches) const {
    vertices.clear(), batches.clear();
    struct Sprite { float depth; glm::vec3 c; float hw, hh, rot; uint32_t rgba, tex; float u0, v0, u1, v1; uint8_t blend; float pull; };
    std::vector<Sprite> sprites;
    sprites.reserve(particles_.size());
    const glm::vec3 fwd = camera.forward(), right = camera.right(), up = glm::cross(right, fwd);
    const float cot = 1.0f / std::tan(glm::radians(camera.verticalFovDeg) * 0.5f);
    (void)aspect;
    for (const auto& p : particles_) {
        const ParticleDef& d = *p.def;
        const float depth = glm::dot(p.pos - camera.position, fwd);
        if (depth < 16.0f || d.keys.empty()) continue; // 0x54B0A0: 16 <= z
        const float t = std::clamp(float(p.age) / float(p.life), 0.0f, 1.0f);
        size_t k = 0;
        while (k + 2 < d.keys.size() && t > d.keys[k + 1].time) ++k;
        const auto& a = d.keys[k];
        const auto& b = d.keys[std::min(k + 1, d.keys.size() - 1)];
        const float f = b.time > a.time ? std::clamp((t - a.time) / (b.time - a.time), 0.0f, 1.0f) : 0.0f;
        float rgba[4];
        for (int c = 0; c < 4; ++c) rgba[c] = float((a.rgba >> (8 * c)) & 0xFF) * (1 - f) + float((b.rgba >> (8 * c)) & 0xFF) * f;
        // Size: lerp between the nearest keys that define one.
        float s0 = -1, s1 = -1, t0 = 0, t1 = 1;
        for (size_t i = 0; i < d.keys.size() && i < 4; ++i) {
            if (p.sizes[i] < 0) continue;
            if (d.keys[i].time <= t) s0 = p.sizes[i], t0 = d.keys[i].time;
            if (d.keys[i].time >= t && s1 < 0) s1 = p.sizes[i], t1 = d.keys[i].time;
        }
        if (s0 < 0) s0 = s1;
        if (s1 < 0) s1 = s0;
        const float size = s0 < 0 ? 0.0f : t1 > t0 ? s0 + (s1 - s0) * (t - t0) / (t1 - t0) : s0;
        if (size <= 0) continue;
        const float ys = a.yScale * (1 - f) + b.yScale * f;
        // Camera-distance fade band (particle +0xB8 / +0xBA from def +0x5A / +0x5C, 0x54B1A6).
        if (d.fadeFar > d.fadeNear) {
            if (depth < d.fadeNear) continue;
            if (depth < d.fadeFar) rgba[3] *= (depth - d.fadeNear) / float(d.fadeFar - d.fadeNear);
        }
        // On-screen size fade (particle +0xA4 / +0xA5 = def +0x60 / +0x64 * 255; 2x / 255 in NDC, 0x54B130).
        const float half = size * cot / depth, s4 = d.screenFadeStart * 2, s5 = d.screenFadeEnd * 2;
        if (s5 > s4) {
            if (half > s5) continue;
            if (half > s4) rgba[3] *= 1.0f - (half - s4) / (s5 - s4);
        }
        if (rgba[3] < 0.5f) continue;
        // Depth bias (def +0x15E -> emitter +0x10C -> particle +0x28, flag 0x200; 0x54B0E8): z += bias, >= 16. Drawn by
        // scaling the quad about the eye so its screen footprint stays put [I] (0x400 = biased projection depth).
        const float biased = std::max(16.0f, depth + float(d.depthBias));
        Sprite sp{biased, p.pos, size, size * (ys > 0 ? ys : 1), p.rot, 0, 0, 0, 0, 1, 1, uint8_t(blendFor(d.keys[0].blend)),
                  biased / depth};
        for (int c = 0; c < 4; ++c) sp.rgba |= uint32_t(std::clamp(std::lround(rgba[c]), 0L, 255L)) << (8 * c);
        // Texture frame: list or atlas; animRate 0 = the particle's random frame, 255 = over its life [I], else
        // one frame per animRate 30 Hz frames [I].
        auto frameOf = [&](size_t n) -> size_t {
            if (n <= 1) return 0;
            if (d.animRate == 0) return p.frame % n;
            if (d.animRate == 255) return std::min(n - 1, size_t(t * float(n)));
            return (size_t(std::max<int>(0, d.animStart)) + size_t(p.age / (2 * d.animRate))) % n;
        };
        if (!d.textures.empty()) sp.tex = d.textures[frameOf(d.textures.size())];
        else {
            sp.tex = d.texture;
            if (d.atlasCols && d.atlasRows) {
                const size_t cells = size_t(d.atlasCols) * d.atlasRows;
                const size_t n = std::clamp<size_t>(d.atlasFrames ? d.atlasFrames : cells, 1, cells);
                const size_t first = d.atlasFirst ? std::min<size_t>(d.atlasFirst - 1u, cells - 1) : 0;
                const size_t cell = (first + frameOf(n)) % cells;
                sp.u0 = float(cell % d.atlasCols) / d.atlasCols, sp.v0 = float(cell / d.atlasCols) / d.atlasRows;
                sp.u1 = sp.u0 + 1.0f / d.atlasCols, sp.v1 = sp.v0 + 1.0f / d.atlasRows;
            }
        }
        sprites.push_back(sp);
    }
    std::sort(sprites.begin(), sprites.end(), [](const Sprite& a, const Sprite& b) { return a.depth > b.depth; });
    vertices.reserve(sprites.size() * 6);
    for (const auto& s : sprites) {
        const float cs = std::cos(s.rot), sn = std::sin(s.rot);
        const glm::vec3 rx = (right * cs + up * sn) * s.hw, ry = (up * cs - right * sn) * s.hh;
        glm::vec3 q[4] = {s.c - rx - ry, s.c + rx - ry, s.c + rx + ry, s.c - rx + ry};
        if (s.pull != 1.0f)
            for (auto& v : q) v = camera.position + (v - camera.position) * s.pull;
        const float uv[4][2] = {{s.u0, s.v1}, {s.u1, s.v1}, {s.u1, s.v0}, {s.u0, s.v0}};
        if (batches.empty() || batches.back().textureId != s.tex || batches.back().blend != s.blend)
            batches.push_back({uint32_t(vertices.size()), 0, s.tex, s.blend});
        for (int i : {0, 1, 2, 0, 2, 3}) vertices.push_back({{q[i].x, q[i].y, q[i].z}, {uv[i][0], uv[i][1]}, s.rgba});
        batches.back().count += 6;
    }
}

uint64_t System::checksum() const {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    };
    for (const auto& p : particles_) mix(&p.pos, sizeof p.pos), mix(&p.age, sizeof p.age), mix(&p.frame, sizeof p.frame);
    return h;
}

std::shared_ptr<const ObjectFx> Library::get(const std::string& name, std::vector<std::string>* warnings) {
    if (const auto it = objects_.find(name); it != objects_.end()) return it->second.fx;
    Entry& e = objects_[name];
    auto bytes = bigfile_.read("pc-w\\" + name + ".drm");
    if (!bytes) return nullptr;
    try {
        const auto drm = cdc::parseDrm(std::move(*bytes));
        e.flags = formats::objectFlags(drm);
        auto fx = parseObjectFx(drm, warnings);
        if (!fx || (fx->particles.empty() && fx->events.empty())) return nullptr;
        for (const uint32_t id : fx->textureIds()) {
            if (textureIds_.contains(id)) continue;
            for (const auto& s : drm.sections)
                if (s.type == cdc::SectionType::Texture && s.id == id) {
                    std::string why;
                    if (auto t = formats::decodeTexture(s, &why)) {
                        t->origin = "pc-w\\" + name + ".drm#fx texture " + std::to_string(id);
                        textures_.push_back(std::move(*t));
                        textureIds_.insert(id);
                    } else if (warnings) warnings->push_back(name + ": fx texture " + std::to_string(id) + ": " + why);
                    break;
                }
        }
        e.fx = std::make_shared<const ObjectFx>(std::move(*fx));
    } catch (const std::exception& ex) {
        if (warnings) warnings->push_back("pc-w\\" + name + ".drm effects: " + ex.what());
    }
    return e.fx;
}

uint32_t Library::flags(const std::string& name) const {
    const auto it = objects_.find(name);
    return it == objects_.end() ? 0 : it->second.flags;
}

} // namespace ou::fx
