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
constexpr float kModelSprite = 1.5f; // half-size of a model particle's stand-in sprite, units [I]

// MaterialParams float (c100 + i / 4) that scales the refraction offset, by composite pixel ShaderLib (material +0x30),
// from the disassembled shaders' `mad r.yz, n, c10x.s, vPos * ScreenExtents`: lib 174 (Thor's hammer) c100.x, 159 / 163
// (fireball, explosions) c101.z, 487 / 1624 (Amanda, thralls) c100.w, 21 / 167 / 228 / 483 c100.z. Others: c100.z [I].
int distortParam(uint32_t pixelLib) {
    switch (pixelLib) {
    case 174: return 0;
    case 159: case 163: return 6;
    case 487: case 1624: return 3;
    default: return 2;
    }
}

ParticleMaterial particleMaterial(const cdc::Drm& drm, uint32_t id) {
    ParticleMaterial pm;
    pm.id = id;
    const auto s = std::find_if(drm.sections.begin(), drm.sections.end(),
                                [&](const cdc::Section& x) { return x.type == cdc::SectionType::Material && x.id == id; });
    if (s == drm.sections.end()) return pm;
    formats::MaterialShaderLibs libs;
    const auto m = formats::decodeMaterial(*s, &libs);
    pm.additive = m.blendMode == 5;
    bool backBuffer = false, depth = false;
    for (const auto& t : m.samplers) {
        if (t.source == 1) backBuffer |= t.cls == 9, depth |= t.cls == 10; // engine-global targets (SHADERS.md)
        else if (t.source == 0 && t.category == 3) pm.normal = pm.normal ? pm.normal : t.textureId;
        else if (t.source == 0) pm.texture = pm.texture ? pm.texture : t.textureId;
    }
    const auto param = [&](size_t i) { return i < m.psParams.size() ? m.psParams[i] : 0.0f; };
    // Without a normal map the back-buffer shaders (lib 17) only blend texture x colour over the scene: a plain sprite.
    pm.distort = backBuffer && pm.normal;
    if (pm.distort) pm.distortScale = param(size_t(distortParam(libs.pixel)));
    // Soft shaders (651 fire, 657 steam, 628): the negative MaterialParams entry scales, c100.y biases by 1 - alpha.
    if (depth && !pm.distort) {
        float lo = 0;
        for (size_t i = 0; i < 4; ++i) lo = std::min(lo, param(i));
        pm.softScale = -lo, pm.softBias = param(1);
    }
    return pm;
}

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

// Records until 0xFFFF, 8 + (info >> 12) * 4 bytes each, from `o`.
static std::vector<Event> parseEvents(std::span<const uint8_t> d, size_t o) {
    std::vector<Event> out;
    while (o + 2 <= d.size()) {
        const auto info = rd<uint16_t>(d, o);
        if (info == 0xFFFF) break;
        out.push_back({info, rd<uint16_t>(d, o + 2), rd<uint8_t>(d, o + 4),
                       {rd<uint16_t>(d, o + 8), rd<uint16_t>(d, o + 10), rd<uint16_t>(d, o + 12), rd<uint16_t>(d, o + 14)}});
        o += 8 + size_t(info >> 12) * 4;
    }
    return out;
}

// Event list `ev`, GenericFXObject `gen` and ObjectBase `base` (section indices, -1 = none) of `drm`.
static ObjectFx parseFxSections(const cdc::Drm& drm, int ev, int gen, int base, std::vector<std::string>* warnings) {
    ObjectFx fx;
    if (ev >= 0) fx.events = parseEvents(drm.sections[size_t(ev)].data, 0);
    if (base >= 0) { // animation list: {i16 anim, i16, u32 event-list DTP id, char* name}
        const auto& s = drm.sections[size_t(base)];
        const auto ptrs = cdc::sectionPointers(drm, uint32_t(base));
        const uint32_t n = rd<uint32_t>(s.data, 0x54);
        if (const auto list = ptrs.find(0x58); list != ptrs.end() && list->second.section == uint32_t(base) && n < 4096)
            for (uint32_t i = 0; i < n; ++i) {
                const uint32_t e = list->second.offset + 12 * i;
                const auto name = ptrs.find(e + 8);
                const int list19 = dtpIndex(drm, rd<uint32_t>(s.data, e + 4));
                if (name == ptrs.end() || list19 < 0 || name->second.section != uint32_t(base)) continue;
                std::string nm;
                for (size_t k = name->second.offset; k < s.data.size() && s.data[k]; ++k) nm += char(s.data[k]);
                auto events = parseEvents(drm.sections[size_t(list19)].data, 4); // u32 header
                if (std::any_of(events.begin(), events.end(), [](const Event& x) { return x.effect(); }))
                    fx.clips.emplace(std::move(nm), std::move(events));
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
                // Materials: +0x168 one, else +0x16C {u32 n, material ids[n]} picked per frame like a texture list.
                if (cdc::resourceRefType(arr, uint32_t(off + 0x168)) == int(cdc::SectionType::Material))
                    def.materials.push_back(particleMaterial(drm, rd<uint32_t>(arr.data, off + 0x168)));
                else if (const auto t = aptrs.find(uint32_t(off + 0x16C)); t != aptrs.end()) {
                    const auto td = drm.sections[t->second.section].data;
                    for (uint32_t i = 0, cnt = rd<uint32_t>(td, t->second.offset); i < cnt && i < 256; ++i)
                        def.materials.push_back(particleMaterial(drm, rd<uint32_t>(td, t->second.offset + 4 + 4 * i)));
                }
                fx.particles.push_back(std::move(def));
            }
        }
        // Records `size` bytes each of the array the pointer at +ptrOff names (count at +countOff): f(record, the
        // array section's pointers, record offset).
        auto records = [&](uint32_t countOff, uint32_t ptrOff, size_t size, auto&& f) {
            const uint32_t n = rd<uint32_t>(s.data, countOff);
            const auto it = ptrs.find(ptrOff);
            if (!n || n >= 4096 || it == ptrs.end()) return;
            const auto& arr = drm.sections[it->second.section];
            const auto aptrs = it->second.section == uint32_t(gen) ? ptrs : cdc::sectionPointers(drm, it->second.section);
            for (uint32_t k = 0; k < n && it->second.offset + size * (k + 1) <= arr.data.size(); ++k)
                f(arr.data.subspan(it->second.offset + size * k, size), aptrs, uint32_t(it->second.offset + size * k));
        };
        auto list = [&](const std::unordered_map<uint32_t, cdc::Pointer>& p, uint32_t at) { // {u32 n, u32[n]} at a pointer
            std::vector<uint32_t> out;
            if (const auto t = p.find(at); t != p.end()) {
                const auto td = drm.sections[t->second.section].data;
                for (uint32_t i = 0, cnt = rd<uint32_t>(td, t->second.offset); i < cnt && i < 256; ++i)
                    out.push_back(rd<uint32_t>(td, t->second.offset + 4 + 4 * i));
            }
            return out;
        };
        records(0x1C, 0x20, 0x58, [&](std::span<const uint8_t> r, const auto& p, uint32_t at) {
            GlowDef g;
            g.flags = rd<uint8_t>(r, 0), g.kind = rd<int8_t>(r, 1);
            g.segment = rd<int16_t>(r, 2), g.segment2 = rd<int16_t>(r, 4);
            g.colours = list(p, at + 8);
            g.id = rd<uint8_t>(r, 0x0C), g.lifeVar = rd<uint8_t>(r, 0x0F);
            g.period = std::max<int16_t>(1, rd<int16_t>(r, 0x10));
            g.facing = float(rd<int16_t>(r, 0x12)) / 180.0f;
            g.width = rd<uint16_t>(r, 0x14), g.height = rd<uint16_t>(r, 0x16);
            g.endWidth = rd<int16_t>(r, 0x18), g.endHeight = rd<int16_t>(r, 0x1A);
            g.life = rd<int32_t>(r, 0x1C);
            g.fadeIn = rd<uint16_t>(r, 0x20), g.fadeOut = rd<uint16_t>(r, 0x22);
            g.depthBias = rd<int16_t>(r, 0x24);
            g.facingMode = int8_t(uint8_t(rd<uint8_t>(r, 0x26) << 4)) >> 4;
            g.axisSegment = rd<int16_t>(r, 0x28), g.axisSegment2 = rd<int16_t>(r, 0x2A);
            g.rot0 = rd<float>(r, 0x2C), g.rotRange = rd<float>(r, 0x30), g.spin = rd<float>(r, 0x34), g.spinRange = rd<float>(r, 0x38);
            g.texRate = rd<uint8_t>(r, 0x3C), g.condition = rd<uint8_t>(r, 0x3D);
            g.texture = rd<uint32_t>(r, 0x40);
            g.textures = list(p, at + 0x44);
            g.offset = {float(rd<int16_t>(r, 0x48)), float(rd<int16_t>(r, 0x4A)), float(rd<int16_t>(r, 0x4C))};
            g.fadeFar = rd<uint16_t>(r, 0x4E), g.fadeNear = rd<uint16_t>(r, 0x50);
            fx.glows.push_back(std::move(g));
        });
        records(0x3C, 0x40, 0x24, [&](std::span<const uint8_t> r, const auto&, uint32_t) {
            LightDef l;
            l.kind = rd<uint8_t>(r, 0), l.life = rd<int16_t>(r, 2), l.segment = rd<int16_t>(r, 6);
            for (int c = 0; c < 3; ++c)
                l.colour[c] = rd<uint8_t>(r, 0x0A + c) / 255.0f, l.colour2[c] = rd<uint8_t>(r, 0x0E + c) / 255.0f;
            l.birth = rd<uint8_t>(r, 0x11) & 1;
            l.radius = rd<uint16_t>(r, 0x12), l.radius2 = rd<uint16_t>(r, 0x14);
            l.duration = rd<int16_t>(r, 0x16), l.fadeIn = rd<int16_t>(r, 0x18), l.fadeOut = rd<int16_t>(r, 0x1A);
            if (const uint32_t id = rd<uint32_t>(r, 0x20); id) // plain DTP id (formats::objectLights)
                if (const int i = dtpIndex(drm, id); i >= 0) l.dtp = formats::lightFromDtp(drm, uint32_t(i));
            fx.lights.push_back(std::move(l));
        });
        if (const auto it = ptrs.find(0x08); it != ptrs.end()) { // effect map: u16 n at +4, {u8 id, i8 class, u8 index, pad}
            const auto md = drm.sections[it->second.section].data;
            for (uint32_t k = 0, n = rd<uint16_t>(s.data, 4); k < n && k < 256; ++k)
                fx.effectMap.push_back({rd<uint8_t>(md, it->second.offset + 4 * k), rd<int8_t>(md, it->second.offset + 4 * k + 1),
                                        rd<uint8_t>(md, it->second.offset + 4 * k + 2)});
        }
    }
    if (base >= 0) try { // segments / HInfo markers of the object's model (unit FX point sets bring their own)
        const auto phys = physics::loadObjectPhysics(drm);
        fx.segments = phys.segments;
        for (const auto& m : phys.hmarkers) {
            glm::mat4 r = glm::translate(glm::mat4(1), m.pos);
            r = glm::rotate(r, m.rot.x, {1, 0, 0}), r = glm::rotate(r, m.rot.y, {0, 1, 0}), r = glm::rotate(r, m.rot.z, {0, 0, 1});
            fx.markers.emplace(m.id, ObjectFx::Marker{m.segment, r});
        }
    } catch (const std::exception& e) {
        if (warnings) warnings->push_back(std::string("effect segments: ") + e.what());
    }
    return fx;
}

std::optional<ObjectFx> parseObjectFx(const cdc::Drm& drm, std::vector<std::string>* warnings) {
    const auto obj = std::find_if(drm.sections.begin(), drm.sections.end(), [](const auto& s) { return s.type == cdc::SectionType::Object; });
    if (obj == drm.sections.end() || obj->data.size() < 12) return std::nullopt;
    const int ev = objectDtp(drm, *obj, 4), gen = objectDtp(drm, *obj, 8), base = objectDtp(drm, *obj, 0);
    auto fx = parseFxSections(drm, ev, gen, base, warnings);
    if (ev < 0 && gen < 0 && fx.clips.empty()) return std::nullopt;
    return fx;
}

std::vector<PointSet> parsePointSets(const cdc::Drm& unit, std::vector<std::string>* warnings) {
    std::vector<PointSet> out;
    if (unit.sections.empty()) return out;
    const auto level = cdc::sectionPointers(unit, 0);
    const auto ud = level.find(0x74); // Level+0x74 -> unit data (LEVELS.md)
    if (ud == level.end()) return out;
    const auto& us = unit.sections[ud->second.section];
    const auto uptrs = cdc::sectionPointers(unit, ud->second.section);
    const uint32_t n = rd<uint32_t>(us.data, ud->second.offset + 0x120);
    const auto arr = uptrs.find(ud->second.offset + 0x124);
    if (!n || n >= 65536 || arr == uptrs.end()) return out;
    const auto& as = unit.sections[arr->second.section];
    const auto aptrs = arr->second.section == ud->second.section ? uptrs : cdc::sectionPointers(unit, arr->second.section);
    std::map<uint32_t, std::shared_ptr<const ObjectFx>> byGfx; // GenericFXObject DTP id -> parsed once
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t r = arr->second.offset + 0x70 * k;
        if (r + 0x70 > as.data.size()) break;
        const int list = dtpIndex(unit, rd<uint32_t>(as.data, r + 0x58));
        if (list < 0) continue;
        const auto ld = unit.sections[size_t(list)].data;
        const uint32_t gfxId = rd<uint32_t>(ld, 0);
        auto& fx = byGfx[gfxId];
        if (!fx) {
            const int gen = dtpIndex(unit, gfxId);
            if (gen < 0) {
                if (warnings) warnings->push_back("FX point set list " + std::to_string(unit.sections[size_t(list)].id) +
                                                  ": GenericFXObject " + std::to_string(gfxId) + " not in the unit");
                continue;
            }
            fx = std::make_shared<const ObjectFx>(parseFxSections(unit, -1, gen, -1, warnings));
        }
        PointSet ps;
        ps.id = rd<int32_t>(as.data, r + 0x44);
        if (const auto nm = aptrs.find(r + 0x40); nm != aptrs.end())
            for (size_t i = nm->second.offset; i < unit.sections[nm->second.section].data.size() && unit.sections[nm->second.section].data[i]; ++i)
                ps.name += char(unit.sections[nm->second.section].data[i]);
        ps.fx = fx;
        ps.events = parseEvents(ld, 4);
        ps.distance = rd<float>(as.data, r + 0x5C);
        glm::mat4 m;
        std::memcpy(&m[0][0], as.data.data() + r, sizeof m); // D3D row-major = column-major memory (LEVELS.md IMFs)
        ps.segments.push_back(m);
        if (const auto pts = aptrs.find(r + 0x64); pts != aptrs.end()) {
            const auto pd = unit.sections[pts->second.section].data;
            for (uint32_t i = 0, cnt = rd<uint32_t>(as.data, r + 0x60); i < cnt && i < 64; ++i) {
                if (pts->second.offset + 0x50 * (i + 1) > pd.size()) break;
                std::memcpy(&m[0][0], pd.data() + pts->second.offset + 0x50 * i, sizeof m);
                ps.segments.push_back(m);
            }
        }
        out.push_back(std::move(ps));
    }
    return out;
}

glm::mat4 ObjectFx::frame(int16_t s, std::span<const glm::mat4> bones) const {
    auto seg = [&](int i) {
        if (i >= 0 && size_t(i) < bones.size()) return bones[size_t(i)];
        return i >= 0 && size_t(i) < segments.size() ? segments[size_t(i)] : glm::mat4(1);
    };
    if (s >= 0) return seg(s);
    const auto it = markers.find(-s);
    return it != markers.end() ? seg(it->second.segment) * it->second.local : glm::mat4(1); // -125 (0x538570), ids >= 100: root [U]
}

std::vector<uint32_t> ObjectFx::textureIds() const {
    std::vector<uint32_t> out;
    for (const auto& p : particles) {
        if (p.model()) continue;
        if (!p.materials.empty()) {
            for (const auto& m : p.materials)
                for (const uint32_t id : {m.texture, m.normal}) if (id) out.push_back(id);
        } else if (!p.textures.empty()) out.insert(out.end(), p.textures.begin(), p.textures.end());
        else if (p.texture) out.push_back(p.texture);
    }
    for (const auto& g : glows) {
        out.insert(out.end(), g.textures.begin(), g.textures.end());
        if (g.texture) out.push_back(g.texture);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool ObjectFx::usesGlowTexture() const {
    return std::any_of(glows.begin(), glows.end(), [](const GlowDef& g) { return !g.texture && g.textures.empty(); });
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
                   int16_t segment2, std::span<const glm::mat4> bones) {
    if (!fx || def < 0 || size_t(def) >= fx->particles.size()) return false;
    const auto& d = fx->particles[size_t(def)];
    auto at = [&](int16_t s) { return world * fx->frame(s, bones); };
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

bool System::runCommand(const std::shared_ptr<const ObjectFx>& fx, const Command& c, const glm::mat4& world, int32_t owner,
                        std::span<const glm::mat4> bones, bool birth) {
    // a2 = segment (0xFF: the record's own), a3 = second segment (0x547B40). At birth glows / lights need their
    // record's birth bit (0x536B70 +0 & 1, 0x546710 +0x11 & 1).
    const int16_t seg = int16_t(c.a2 == 0xFF ? 0 : c.a2);
    auto glow = [&](int i, int16_t s) {
        return size_t(i) < fx->glows.size() && (!birth || fx->glows[size_t(i)].flags & 1) && startGlow(fx, i, world, owner, s, bones);
    };
    auto light = [&](int i, int16_t s) {
        return size_t(i) < fx->lights.size() && (!birth || fx->lights[size_t(i)].birth) && startLight(fx, i, world, owner, s, bones);
    };
    switch (c.type) {
    case 3: return start(fx, c.a1, world, owner, seg, int16_t(c.a3), bones); // particle (seg, seg2)
    case 0x1D: return start(fx, c.a1, world, owner, 0, 0, bones);           // particle, root
    case 0x20: return start(fx, c.a1, world, owner, 0, 0, bones);
    case 4: { // the particle on every segment from a2 (0x547C8E)
        bool any = false;
        const int n = c.a3 ? c.a3 : int(fx->segments.size());
        for (int s = c.a2; s < n; ++s) any |= start(fx, c.a1, world, owner, int16_t(s), 0, bones);
        return any;
    }
    case 6: return glow(c.a1, seg);
    case 0x11: return light(c.a1, seg);
    case 0x1C: case 0x2F: // effect map id a1 -> {class, index} (classes 1, 3-5, 7, 8: not implemented)
        for (const auto& m : fx->effectMap)
            if (m.id == c.a1) {
                if (m.cls == 0) return start(fx, m.index, world, owner, 0, 0, bones);
                if (m.cls == 2) return glow(m.index, 0);
                if (m.cls == 6) return light(m.index, 0);
                return false;
            }
        return false;
    case 0x15: case 0x1A: stopId(owner, int16_t(c.a1), false); return false; // 0x53D070: prims with id a1 fade out
    case 0x18: case 0x1B: stopId(owner, int16_t(c.a1), true); return false;  // 0x52F4D0: removed
    default: return false; // other effect classes: docs/formats/EFFECTS.md "Missing"
    }
}

int System::startPointSet(const PointSet& set, const glm::mat4& world, int32_t owner) {
    int n = 0;
    if (!set.fx) return 0;
    for (const auto& e : set.events)
        if (e.effect() && e.event == 0 && !(e.info & 0x3F0)) n += runCommand(set.fx, e.cmd, world, owner, set.segments, true);
    return n;
}

int System::startBirth(const std::shared_ptr<const ObjectFx>& fx, const glm::mat4& world, int32_t owner) {
    int n = 0;
    if (!fx) return 0;
    for (const auto& e : fx->events)
        if (e.effect() && e.event == 0 && !(e.info & 0x3F0)) n += runCommand(fx, e.cmd, world, owner, {}, true);
    return n;
}

bool System::startGlow(const std::shared_ptr<const ObjectFx>& fx, int def, const glm::mat4& world, int32_t owner,
                       int16_t segment, std::span<const glm::mat4> bones) {
    const GlowDef& d = fx->glows[size_t(def)];
    if (d.kind != 1) return false;
    auto at = [&](int16_t s) { return world * fx->frame(s, bones); };
    Glow g;
    g.fx = fx, g.def = &d, g.owner = owner;
    const glm::mat4 m = at(segment ? segment : d.segment);
    g.pos = glm::vec3(m * glm::vec4(d.offset, 1));
    if (d.facingMode && d.facingMode != 4) { // 0x52F660: axis of axisSegment, or axisSegment -> axisSegment2
        const glm::mat4 a = at(d.axisSegment);
        g.axisFrom = a[3];
        if (d.axisSegment == d.axisSegment2) g.axis = glm::vec3(a[std::abs(d.facingMode) - 1]) * (d.facingMode < 0 ? -1.0f : 1.0f);
        else g.axis = glm::vec3(at(d.axisSegment2)[3]) - g.axisFrom;
        if (glm::dot(g.axis, g.axis) > 0) g.axis = glm::normalize(g.axis);
    }
    g.period = d.period;
    if (d.lifeVar) g.period = int16_t(std::max(1, int(d.period) + int(rand15() % (2u * d.lifeVar + 1u)) - d.lifeVar));
    if (d.flags & 0x10) g.phase = uint16_t(rand15() & 0xFFF);
    // Transient owners (< 0: one-shot sequences, characters) have no instance to outlive: endless glows last 1 s [I].
    g.left = d.life > 0 ? 2 * d.life : owner < 0 ? 60 : -1;
    // Size growth per tick: ((end / start) - 1) / life (0x536B70 -> prim +0x54 / +0x5C).
    if (d.life > 0 && d.endWidth >= 0 && d.width > 0) g.growW = (d.endWidth / d.width - 1.0f) / float(g.left);
    if (d.life > 0 && d.endHeight >= 0 && d.height > 0) g.growH = (d.endHeight / d.height - 1.0f) / float(g.left);
    constexpr float kDeg = 3.14159265f / 180.0f;
    g.rot = (d.rot0 + rand01() * d.rotRange - d.rotRange * 0.5f) * kDeg;
    g.spin = (d.spin + rand01() * d.spinRange - d.spinRange * 0.5f) * kDeg * 0.5f;
    if (!d.textures.empty() && !d.texRate) g.frame = uint16_t(rand15() % d.textures.size());
    glows_.push_back(std::move(g));
    return true;
}

bool System::startLight(const std::shared_ptr<const ObjectFx>& fx, int def, const glm::mat4& world, int32_t owner,
                        int16_t segment, std::span<const glm::mat4> bones) {
    const LightDef& d = fx->lights[size_t(def)];
    FxLight l;
    l.fx = fx, l.def = &d, l.owner = owner;
    const glm::mat4 m = world * fx->frame(segment ? segment : d.segment, bones);
    l.pos = glm::vec3(m[3]), l.dir = glm::normalize(glm::vec3(m[1])); // spots point along the segment's Y [I]
    const int frames = d.life ? d.life : d.duration; // timed prim (0x5463E0) or the attached light's duration
    l.life = frames < 0 ? (owner < 0 ? 60 : -1) : 2 * std::max(1, frames);
    lights_.push_back(std::move(l));
    return true;
}

void System::stopId(int32_t owner, int id, bool now) {
    for (auto& g : glows_)
        if (g.owner == owner && (id < 0 || g.def->id == id) && g.fadeOut < 0) g.fadeOut = now ? 0 : 2 * g.def->fadeOut;
    for (auto& l : lights_)
        if (l.owner == owner && l.fadeOut < 0) l.fadeOut = now ? 0 : 2 * std::max<int16_t>(0, l.def->fadeOut);
}

int System::runEvents(const std::shared_ptr<const ObjectFx>& fx, const std::vector<Event>& events, const glm::mat4& world,
                      int32_t owner, uint8_t material, float from, float to, std::span<const glm::mat4> bones) {
    int n = 0;
    for (const auto& e : events)
        if (e.runsOn(material) && float(e.frame) >= from && float(e.frame) < to) n += runCommand(fx, e.cmd, world, owner, bones);
    return n;
}

void System::play(std::shared_ptr<const ObjectFx> fx, const std::vector<Event>& events, const glm::mat4& world, int32_t owner,
                  uint8_t material) {
    if (!fx) return;
    Sequence s{std::move(fx), &events, world, owner, 0, material};
    runEvents(s.fx, events, world, owner, material, 0.0f, 0.5f); // frame 0 now, later frames as ticks pass
    sequences_.push_back(std::move(s));
}

void System::stopOwner(int32_t owner) {
    for (auto& e : emitters_)
        if (e.owner == owner) e.stopped = true;
    stopId(owner, -1, false);
}

bool System::hasOwner(int32_t owner) const {
    return std::any_of(emitters_.begin(), emitters_.end(), [&](const Emitter& e) { return e.owner == owner && !e.stopped; }) ||
           std::any_of(glows_.begin(), glows_.end(), [&](const Glow& g) { return g.owner == owner && g.fadeOut < 0; }) ||
           std::any_of(lights_.begin(), lights_.end(), [&](const FxLight& l) { return l.owner == owner && l.fadeOut < 0; });
}

void System::clear() { emitters_.clear(), particles_.clear(), sequences_.clear(), glows_.clear(), lights_.clear(); }

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
    const size_t list = d.materials.empty() ? d.textures.size() : d.materials.size() > 1 ? d.materials.size() : 0;
    const size_t frames = list ? list : cells ? (d.atlasFrames ? d.atlasFrames : cells) : 1;
    p.frame = uint16_t(rand15() % frames);
    (void)camera;
    particles_.push_back(std::move(p));
    if (d.mode == 2) e.persistent = particles_.back().id;
}

void System::tick(const glm::vec3& camera) {
    for (auto& s : sequences_) { // frame f fires at tick 2f (30 Hz authoring)
        ++s.age;
        runEvents(s.fx, *s.events, s.world, s.owner, s.material, 0.5f * float(s.age), 0.5f * float(s.age + 1));
    }
    std::erase_if(sequences_, [](const Sequence& s) {
        return std::none_of(s.events->begin(), s.events->end(), [&](const Event& e) { return 2 * int(e.frame) > s.age; });
    });
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
    for (auto& g : glows_) { // 0x535FA0: age, spin, growth, life; 0x536280: texture-list step every texRate frames
        ++g.age, g.rot += g.spin, g.scaleW += g.growW, g.scaleH += g.growH;
        if (g.left > 0) --g.left;
        if (g.fadeOut > 0) --g.fadeOut;
        if (g.def->texRate && !g.def->textures.empty() && g.age % (2 * g.def->texRate) == 0)
            g.frame = uint16_t((g.frame + 1) % g.def->textures.size());
    }
    std::erase_if(glows_, [](const Glow& g) { return g.left == 0 || g.fadeOut == 0; });
    for (auto& l : lights_) {
        ++l.age;
        if (l.life > 0) --l.life;
        if (l.fadeOut > 0) --l.fadeOut;
    }
    std::erase_if(lights_, [](const FxLight& l) { return l.life == 0 || l.fadeOut == 0; });
    std::erase_if(emitters_, [&](const Emitter& e) {
        return e.stopped && std::none_of(particles_.begin(), particles_.end(), [&](const Particle& p) { return p.def == e.def && p.fx == e.fx; });
    });
}

void System::build(const render::Camera& camera, float aspect, std::vector<render::FxVertex>& vertices,
                   std::vector<render::FxBatch>& batches) const {
    vertices.clear(), batches.clear();
    struct Sprite { float depth; glm::vec3 c; float hw, hh, rot; uint32_t rgba, tex; float u0, v0, u1, v1; uint8_t blend; float pull;
                    const ParticleMaterial* mat = nullptr; };
    std::vector<Sprite> sprites;
    sprites.reserve(particles_.size());
    const glm::vec3 fwd = camera.forward(), right = camera.right(), up = glm::cross(right, fwd);
    const float cot = 1.0f / std::tan(glm::radians(camera.verticalFovDeg) * 0.5f);
    (void)aspect;
    for (const auto& p : particles_) {
        const ParticleDef& d = *p.def;
        const float depth = glm::dot(p.pos - camera.position, fwd);
        if (depth < 16.0f || d.keys.empty()) continue; // 0x54B0A0: 16 <= z
        // Neither texture, list nor material: nothing to draw.
        if (!d.model() && !d.texture && d.textures.empty() && d.materials.empty()) continue;
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
        // Model particles (def +0x04 & 0x80: shell casings, debris meshes) are not decoded as meshes; drawn as a small
        // sprite in their key colour, alpha-blended [I] (ponytail: no mesh, upgrade = the particle's model segment).
        const float size = d.model() ? kModelSprite : s0 < 0 ? 0.0f : t1 > t0 ? s0 + (s1 - s0) * (t - t0) / (t1 - t0) : s0;
        if (size <= 0) continue;
        if (d.model()) rgba[3] = 255;
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
        Sprite sp{biased, p.pos, size, size * (ys > 0 ? ys : 1), p.rot, 0, 0, 0, 0, 1, 1,
                  uint8_t(d.model() ? Blend::Alpha : blendFor(d.keys[0].blend)),
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
        if (!d.materials.empty()) { // material path: the material's blend, texture and screen reads; atlas UVs as below
            sp.mat = &d.materials[d.materials.size() > 1 ? frameOf(d.materials.size()) : 0];
            sp.tex = sp.mat->texture;
            sp.blend = uint8_t(sp.mat->distort || !sp.mat->additive ? Blend::Alpha : Blend::Additive);
        }
        if (d.materials.empty() && !d.textures.empty()) sp.tex = d.textures[frameOf(d.textures.size())];
        else if (d.materials.size() <= 1) {
            if (d.materials.empty()) sp.tex = d.model() ? 0 : d.texture;
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
    // Glows (0x536280): one sprite each at the projected position, depth >= 20 after the bias.
    for (const auto& g : glows_) {
        const GlowDef& d = *g.def;
        if (d.condition == 1) continue; // drawn only while 0x5EA390 (a camera mode [U]) is set: taken as unset
        const float depth = glm::dot(g.pos - camera.position, fwd);
        if (depth < 20.0f) continue;
        // Colour: one, or the list cycled with `period` frames per entry, prev -> current lerp (0x548D40) [I: weights].
        const size_t n = d.colours.size();
        uint32_t c = n ? d.colours[0] : 0x80808080u;
        if (n >= 2) {
            const uint32_t per = uint32_t(g.period), f = g.phase + uint32_t(g.age / 2);
            const uint32_t cur = (f + per) % uint32_t(n * per) / per % uint32_t(n), prev = (cur ? cur : uint32_t(n)) - 1;
            const float w = float(f % per) / float(per);
            c = 0;
            for (int k = 0; k < 32; k += 8)
                c |= uint32_t(std::lround(float((d.colours[prev] >> k) & 0xFF) * (1 - w) + float((d.colours[cur] >> k) & 0xFF) * w)) << k;
        }
        float fade = 1;
        if (g.age < 2 * d.fadeIn) fade = float(g.age) / float(2 * d.fadeIn);
        else if (g.fadeOut >= 0 && d.fadeOut) fade = float(g.fadeOut) / float(2 * d.fadeOut);
        else if (g.left >= 0 && g.left < 2 * d.fadeOut) fade = float(g.left) / float(2 * d.fadeOut);
        if (d.facingMode && d.facingMode != 4 && glm::dot(g.axis, g.axis) > 0) { // 0x52F660 facing threshold
            const float t = glm::dot(glm::normalize(camera.position - g.axisFrom), g.axis) - 1.0f + d.facing;
            if (t <= 0) continue;
            fade *= std::min(1.0f, t / std::max(d.facing, 1e-3f)); // [I] the exe's scale (0x814D20) is not decoded
        }
        if (d.fadeFar > d.fadeNear && depth < d.fadeFar) { // prim +0x78 / +0x7A band
            if (depth <= d.fadeNear) continue;
            fade *= (depth - d.fadeNear) / float(d.fadeFar - d.fadeNear);
        }
        // Mode 1 fades RGB (0x52AE20; drawn additive), mode 2 alpha [I: which D3D blend each mode is].
        float rgba[4];
        for (int k = 0; k < 4; ++k) rgba[k] = float((c >> (8 * k)) & 0xFF);
        if (d.alphaBlend()) rgba[3] *= fade;
        else for (int k = 0; k < 3; ++k) rgba[k] *= fade;
        if (rgba[3] < 0.5f || (!d.alphaBlend() && rgba[0] + rgba[1] + rgba[2] < 1.5f)) continue;
        const float hw = d.width * g.scaleW, hh = d.height * g.scaleH;
        if (hw <= 0 || hh <= 0) continue;
        const float biased = std::max(20.0f, depth + float(d.depthBias));
        Sprite sp{biased, g.pos, hw, hh, g.rot, 0, 0, 0, 0, 1, 1, uint8_t(d.alphaBlend() ? Blend::Alpha : Blend::Additive), biased / depth};
        for (int k = 0; k < 4; ++k) sp.rgba |= uint32_t(std::clamp(std::lround(rgba[k]), 0L, 255L)) << (8 * k);
        sp.tex = !d.textures.empty() ? d.textures[g.frame % d.textures.size()] : d.texture ? d.texture : kGlowTexture;
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
        render::FxBatch b{uint32_t(vertices.size()), 0, s.tex, s.blend};
        if (s.mat && s.mat->distort) b.normalId = s.mat->normal, b.distort = s.mat->distortScale;
        else if (s.mat) b.softScale = s.mat->softScale, b.softBias = s.mat->softBias;
        const auto& l = batches.empty() ? b : batches.back();
        if (batches.empty() || l.textureId != b.textureId || l.blend != b.blend || l.normalId != b.normalId ||
            l.distort != b.distort || l.softScale != b.softScale || l.softBias != b.softBias)
            batches.push_back(b);
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
    for (const auto& g : glows_) mix(&g.pos, sizeof g.pos), mix(&g.age, sizeof g.age), mix(&g.rot, sizeof g.rot);
    for (const auto& l : lights_) mix(&l.pos, sizeof l.pos), mix(&l.age, sizeof l.age);
    return h;
}

void System::lights(std::vector<LiveLight>& out) const {
    out.clear();
    for (const auto& l : lights_) {
        const LightDef& d = *l.def;
        // Record colour / radius win over the DTP's (0x5C35D0 takes them as arguments); colour -> colour2 and radius ->
        // radius2 over a timed light's life, fades by +0x18 / +0x1A frames [I: the timed update 0x5463E0 not decoded].
        const float total = float(l.age + std::max(0, l.life)), t = l.life < 0 || total <= 0 ? 0.0f : float(l.age) / total;
        glm::vec3 c = glm::mix(d.colour, d.colour2 != glm::vec3(0) ? d.colour2 : d.colour, t);
        float k = 1;
        if (d.fadeIn > 0 && l.age < 2 * d.fadeIn) k = float(l.age) / float(2 * d.fadeIn);
        if (l.fadeOut >= 0) k *= d.fadeOut > 0 ? float(l.fadeOut) / float(2 * d.fadeOut) : 0.0f;
        else if (d.fadeOut > 0 && l.life >= 0 && l.life < 2 * d.fadeOut) k *= float(l.life) / float(2 * d.fadeOut);
        const float r = d.radius > 0 ? (d.radius2 > 0 ? glm::mix(d.radius, d.radius2, t) : d.radius) : d.dtp ? d.dtp->radius : 0;
        out.push_back({l.pos, l.dir, c * k, r, d.dtp ? &*d.dtp : nullptr, l.owner, l.life < 0});
    }
}

void Library::loadTextures(const cdc::Drm& drm, const std::string& name, const std::vector<uint32_t>& ids,
                           std::vector<std::string>* warnings) {
    for (const uint32_t id : ids) {
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
}

std::shared_ptr<const ObjectFx> Library::get(const std::string& name, std::vector<std::string>* warnings) {
    if (const auto it = objects_.find(name); it != objects_.end()) return it->second.fx;
    Entry& e = objects_[name];
    auto bytes = bigfile_.read("pc-w\\" + name + ".drm");
    if (!bytes) return nullptr;
    try {
        const auto drm = cdc::parseDrm(std::move(*bytes));
        e.flags = formats::objectFlags(drm);
        // Object data block (object section +0 -> ObjectBase, +0xC8 -> data, as game::loadWeapon reads it).
        if (const auto obj = std::find_if(drm.sections.begin(), drm.sections.end(), [](const auto& s) { return s.type == cdc::SectionType::Object; });
            obj != drm.sections.end())
            if (const int base = objectDtp(drm, *obj, 0); base >= 0) {
                const auto ptrs = cdc::sectionPointers(drm, uint32_t(base));
                if (const auto it = ptrs.find(0xC8); it != ptrs.end()) {
                    const auto d = drm.sections[it->second.section].data;
                    if (it->second.offset < d.size()) e.data.assign(d.begin() + it->second.offset, d.end());
                }
            }
        auto fx = parseObjectFx(drm, warnings);
        if (!fx || (fx->particles.empty() && fx->glows.empty() && fx->lights.empty() && fx->events.empty() && fx->clips.empty()))
            return nullptr;
        loadTextures(drm, name, fx->textureIds(), warnings);
        if (fx->usesGlowTexture() && !textureIds_.contains(kGlowTexture)) // particle.drm: always loaded by the exe (0x5D4090)
            if (auto p = bigfile_.read("pc-w\\particle.drm")) loadTextures(cdc::parseDrm(std::move(*p)), "particle", {kGlowTexture}, warnings);
        e.fx = std::make_shared<const ObjectFx>(std::move(*fx));
    } catch (const std::exception& ex) {
        if (warnings) warnings->push_back("pc-w\\" + name + ".drm effects: " + ex.what());
    }
    return e.fx;
}

const std::vector<PointSet>& Library::pointSets(const cdc::Drm& unit, const std::string& unitName,
                                               std::vector<std::string>* warnings) {
    if (const auto it = pointSets_.find(unitName); it != pointSets_.end()) return it->second;
    auto& sets = pointSets_[unitName];
    sets = parsePointSets(unit, warnings);
    std::set<const ObjectFx*> seen;
    for (const auto& s : sets)
        if (seen.insert(s.fx.get()).second) {
            loadTextures(unit, unitName, s.fx->textureIds(), warnings);
            if (s.fx->usesGlowTexture() && !textureIds_.contains(kGlowTexture))
                if (auto p = bigfile_.read("pc-w\\particle.drm")) loadTextures(cdc::parseDrm(std::move(*p)), "particle", {kGlowTexture}, warnings);
        }
    return sets;
}

uint32_t Library::flags(const std::string& name) const {
    const auto it = objects_.find(name);
    return it == objects_.end() ? 0 : it->second.flags;
}

uint16_t Library::dataU16(const std::string& name, uint32_t off) {
    get(name);
    const auto& d = objects_[name].data;
    return rd<uint16_t>(d, off);
}

} // namespace ou::fx
