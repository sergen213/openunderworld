// Script types, intro links, trigger shapes and the object runtime. Evidence: docs/formats/SCRIPTS.md.
#include "script/Script.hpp"
#include "script/Vm.hpp"

#include "collision/Collision.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>

namespace ou::script {
namespace {

using cdc::FormatError;

// One section's bytes, relocated pointers and resource references (type-3/type-5 relocations:
// patch offset -> referenced section type; the id stays in the data, as in formats/Level.cpp).
struct View {
    const cdc::Drm& drm;
    uint32_t index;
    std::unordered_map<uint32_t, cdc::Pointer> ptrs;
    std::unordered_map<uint32_t, uint32_t> refs;

    View(const cdc::Drm& d, uint32_t i) : drm(d), index(i), ptrs(cdc::sectionPointers(d, i)) {
        const auto r = d.sections[i].relocations;
        if (r.size() < 20) return;
        uint32_t c[5];
        std::memcpy(c, r.data(), sizeof c);
        const size_t t3 = 20 + 8 * size_t(c[0]) + 8 * size_t(c[1]), t5 = t3 + 4 * (size_t(c[2]) + c[3]);
        for (const auto [p0, n] : {std::pair{t3, c[2]}, std::pair{t5, c[4]}})
            for (uint32_t k = 0; k < n; ++k) {
                if (p0 + 4 * (k + 1ull) > r.size()) throw FormatError("resource relocation table truncated");
                uint32_t v;
                std::memcpy(&v, r.data() + p0 + 4 * size_t(k), 4);
                refs[(v & 0x1FFFFFF) * 4] = v >> 25;
            }
    }
    template <class T>
    T at(size_t off) const {
        const auto d = drm.sections[index].data;
        if (off > d.size() || d.size() - off < sizeof(T))
            throw FormatError("script section " + std::to_string(index) + ": read past end at " + std::to_string(off));
        T v;
        std::memcpy(&v, d.data() + off, sizeof v);
        return v;
    }
    std::optional<cdc::Pointer> ptr(uint32_t off) const {
        const auto it = ptrs.find(off);
        return it == ptrs.end() ? std::nullopt : std::optional(it->second);
    }
    std::string string(uint32_t off) const {
        const auto d = drm.sections[index].data;
        if (off >= d.size()) throw FormatError("script string past section end");
        const char* s = reinterpret_cast<const char*>(d.data() + off);
        return {s, strnlen(s, d.size() - off)};
    }
    uint32_t ref(uint32_t off) const { return refs.contains(off) ? at<uint32_t>(off) : 0; }
};

// Views of the sections a script type points into (members/functions/code are usually in its own).
struct Views {
    const cdc::Drm& drm;
    std::map<uint32_t, View> v;
    const View& operator[](uint32_t i) {
        if (i >= drm.sections.size()) throw FormatError("script pointer to section " + std::to_string(i));
        return v.try_emplace(i, drm, i).first->second;
    }
    // SArray {T* data} with the element count stored at data - 4.
    std::pair<cdc::Pointer, uint32_t> array(const View& s, uint32_t off) {
        const auto p = s.ptr(off);
        if (!p) return {{}, 0};
        if (p->offset < 4) throw FormatError("script array without a count");
        return {*p, (*this)[p->section].at<uint32_t>(p->offset - 4)};
    }
};

const char* kEventNames[] = {"enter", "firstin", "in", "out", "cross", "use", "activate", "start", "main", "checkpoint",
                             "cinematicend", "playermessage"};

glm::mat3 rotation(vec3 r) { // Rx(x) * Ry(y) * Rz(z) (cdc_Matrix_Build_XYZOrder 0x49D500, LEVELS.md)
    return glm::mat3(glm::rotate(glm::mat4(1), r.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), r.y, {0, 1, 0}) *
                     glm::rotate(glm::mat4(1), r.z, {0, 0, 1}));
}

bool contains(const std::string& s, const char* sub) { return s.find(sub) != std::string::npos; }
bool endsWith(const std::string& s, const char* suf) {
    const size_t n = std::strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

// [I] behaviour classes by objectlist.txt name (the exe binds C++ object types per object; not decoded).
std::optional<Kind> classify(const std::string& n) {
    // triggervolume, soundtriggervolume, firetriggervolume, ...; soundvolume's script derives from
    // triggervolumebase too (soundvolume.drm 3485 -> 3516 -> 3515) [O]
    if (endsWith(n, "triggervolume") || n == "soundvolume") return Kind::Volume;
    if (n == "triggerplane") return Kind::Plane;
    if (contains(n, "cheat")) return std::nullopt;
    if (contains(n, "switch") || contains(n, "lever")) return Kind::Switch;
    if (contains(n, "pu_block") || contains(n, "rb_pushblock") || contains(n, "pushable")) return Kind::Block;
    if (contains(n, "door") || contains(n, "gate")) return Kind::Door;
    return std::nullopt;
}

constexpr float kReach = 200;          // [I] switch / block use distance (horizontal) from Lara's root
constexpr float kReachHeight = 250;    // [I]
constexpr float kBlockSpeed = 120;     // [I] units per second
constexpr double kDt = 1.0 / 60.0;     // ticks are game::kTickSeconds

} // namespace

const char* eventName(EventKind k) { return kEventNames[size_t(k)]; }

// ---- script types ----------------------------------------------------------------------------------

ScriptType parseScriptType(const cdc::Drm& drm, uint32_t index) {
    if (index >= drm.sections.size() || drm.sections[index].type != cdc::SectionType::Script)
        throw FormatError("not a script section");
    Views views{drm, {}};
    const View& s = views[index];
    ScriptType t;
    if (const auto v = s.at<uint32_t>(0); v != 4) throw FormatError("script type version " + std::to_string(v));
    t.id = drm.sections[index].id;
    t.superId = s.ref(0x18);
    t.size = s.at<uint16_t>(0x1C);
    if (const auto p = s.ptr(0x10)) {
        const auto d = drm.sections.at(p->section).data;
        if (p->offset >= d.size()) throw FormatError("native script name out of section");
        const auto* c = reinterpret_cast<const char*>(d.data() + p->offset);
        t.nativeName.assign(c, strnlen(c, d.size() - p->offset));
    }

    // Members: 0x14-byte DataMember {DataType {u8 type, ..., +8 type ref or Compound*}, u16 offset, u16 name, init*}.
    // Function locals use the same record (DXHR cdc::Function +0x0C m_locals).
    auto dataMember = [&](const View& v, uint32_t b) {
        Member m;
        m.type = v.at<uint8_t>(b);
        if (m.type & 0x80) { // Compound {A ref, DataType* B}
            if (const auto c = v.ptr(b + 8)) {
                const View& cv = views[c->section];
                m.typeId = cv.ref(c->offset);
                if (const auto sub = cv.ptr(c->offset + 4)) m.argTypeId = views[sub->section].ref(sub->offset + 8);
            }
        } else {
            m.typeId = v.ref(b + 8);
        }
        m.offset = v.at<uint16_t>(b + 0xC);
        m.name = v.at<uint16_t>(b + 0xE);
        if (const auto ip = v.ptr(b + 0x10)) {
            const View& iv = views[ip->section];
            const uint8_t base = m.type & 0x0F;
            if (base == 10 || base == 11) { // NATIVE/SCRIPT: init points at a pointer to the value
                if (const auto vp = iv.ptr(ip->offset)) {
                    m.init = views[vp->section].at<uint32_t>(vp->offset);
                    m.initAt = *vp;
                    if (const auto tp = views[vp->section].ptr(vp->offset); tp && tp->section < drm.sections.size()) {
                        const auto& d = drm.sections[tp->section].data;
                        for (size_t i = tp->offset; i < d.size() && d[i] >= 0x20 && d[i] < 0x7F && m.initText.size() < 256; ++i)
                            m.initText += char(d[i]);
                    }
                    if (vp->offset <= drm.sections[vp->section].data.size() &&
                        drm.sections[vp->section].data.size() - vp->offset >= 8) {
                        m.init2 = views[vp->section].at<uint32_t>(vp->offset + 4);
                        // messagechannel {mode, name}: the channel is the name (0x4A3680 registers it by name, 0x5D0700)
                        if (const auto np = views[vp->section].ptr(vp->offset + 4); m.initText.empty() && np &&
                            np->section < drm.sections.size())
                            for (size_t i = np->offset, n = drm.sections[np->section].data.size(); i < n; ++i) {
                                const char c = char(drm.sections[np->section].data[i]);
                                if (c < 0x20 || c >= 0x7F || m.initText.size() >= 256) break;
                                m.initText += c;
                            }
                    }
                }
            } else {
                m.init = iv.at<uint32_t>(ip->offset);
            }
        } else if (const uint8_t base = m.type & 0x0F; !(m.type & 0x80) && base >= 1 && base <= 3) {
            m.init = v.at<uint32_t>(b + 0x10); // BOOL/INT/FLOAT: the value itself (lara.drm 205 +40 = 10) [O]
            m.inlineInit = true;
        }
        return m;
    };
    const auto [mp, mn] = views.array(s, 0x2C);
    for (uint32_t k = 0; k < mn; ++k) t.members.push_back(dataMember(views[mp.section], mp.offset + 0x14 * k));
    // Prototypes: 0x1C bytes, +6 = i16 vtIndex (NsDispatcherBase bind 0x7B3E60 reads entries 7/8 by index).
    if (const auto [pp, pn] = views.array(s, 0x34); pn)
        for (uint32_t k = 0; k < pn; ++k) t.prototypes.push_back(views[pp.section].at<int16_t>(pp.offset + 0x1C * k + 6));

    // Functions: 0x1C bytes {Prototype*, ..., +0x18 code SArray of u32 words}; Prototype +6 = i16 vtIndex.
    const auto [fp, fn] = views.array(s, 0x38);
    for (uint32_t k = 0; k < fn; ++k) {
        const View& v = views[fp.section];
        const uint32_t b = fp.offset + 0x1C * k;
        Function f;
        f.self = {fp.section, b};
        f.native = (v.at<uint8_t>(b + 4) & 1) != 0;
        const auto [lp, ln] = views.array(v, b + 0x0C);
        f.localCount = ln;
        for (uint32_t l = 0; l < ln; ++l) f.locals.push_back(dataMember(views[lp.section], lp.offset + 0x14 * l));
        if (const auto pp = v.ptr(b)) {
            f.proto = *pp;
            const View& pv = views[pp->section];
            f.vtIndex = pv.at<int16_t>(pp->offset + 6);
            const auto [ap, an] = views.array(pv, pp->offset + 0x0C);
            f.argCount = uint16_t(an);
            for (uint32_t a = 0; a < an; ++a) f.argOffsets.push_back(views[ap.section].at<uint16_t>(ap.offset + 0x14 * a + 0x0C));
            f.returnType = pv.at<uint8_t>(pp->offset + 0x10) & 0x0F;
        }
        const auto [cp, cn] = views.array(v, b + 0x18);
        f.wordCount = cn;
        if (cn) {
            const View& cv = views[cp.section];
            bool immediate = false; // the word after PUSH is its raw operand
            for (uint32_t w = 0; w < cn; ++w) {
                const uint32_t off = cp.offset + 4 * w;
                const auto word = cv.at<uint32_t>(off);
                const auto p = cv.ptr(off);
                if ((p || cv.refs.contains(off)) && f.code.empty()) throw FormatError("script code starts with an operand");
                if (p) {
                    f.code.back().ptrs.push_back(*p);
                    if (f.code.back().op == 0x05) f.code.back().text = views[p->section].string(p->offset);
                } else if (cv.refs.contains(off)) {
                    f.code.back().ref = word;
                } else if (immediate) {
                    f.code.back().imm = word, immediate = false;
                } else {
                    Instr in;
                    in.op = uint8_t(word >> 24);
                    in.arg = int32_t(word << 8) >> 8;
                    in.word = w;
                    if (in.op >= kOpCount) throw FormatError("unknown script opcode " + std::to_string(in.op));
                    immediate = in.op == kOpPush;
                    f.code.push_back(std::move(in));
                }
            }
        }
        t.functions.push_back(std::move(f));
    }
    if (const uint8_t n = s.at<uint8_t>(0x3E))
        if (const auto sp = s.ptr(0x40))
            for (uint32_t k = 0; k < n; ++k)
                t.states.push_back(views[sp->section].ptr(sp->offset + 4 * k).value_or(cdc::Pointer{}));
    return t;
}

std::vector<int32_t> functionTargets(const ScriptType& t, const Function& f) {
    std::vector<int32_t> out;
    std::set<const Function*> seen;
    auto walk = [&](auto&& self, const Function& fn) -> void {
        if (!seen.insert(&fn).second) return;
        for (const auto& in : fn.code) {
            if (in.op == kOpPushSA) {
                for (const auto& m : t.members)
                    if (m.typeId == kInstanceRefType && m.offset == in.arg && m.init &&
                        std::find(out.begin(), out.end(), int32_t(*m.init)) == out.end())
                        out.push_back(int32_t(*m.init));
            } else if (in.op >= kOpCall1 && in.op <= kOpCall5) {
                for (const auto& p : in.ptrs)
                    for (const auto& g : t.functions)
                        if (g.vtIndex < 0 && g.proto.section == p.section && g.proto.offset == p.offset) self(self, g);
            }
        }
    };
    walk(walk, f);
    return out;
}

// ---- shapes ----------------------------------------------------------------------------------------

std::optional<Shape> parseShape(const cdc::Drm& unit, const cdc::Pointer& data, bool plane) {
    if (data.section >= unit.sections.size()) throw FormatError("intro data section out of range");
    const View v(unit, data.section);
    Shape s;
    const uint32_t b = data.offset;
    s.half = {v.at<float>(b + 0x34), v.at<float>(b + 0x38), v.at<float>(b + 0x3C)};
    s.radius = v.at<float>(b + 0x40);
    if (plane) {
        s.kind = ShapeKind::Plane;
        s.oneSided = v.at<uint8_t>(b + 0x44) != 0;
        return s;
    }
    const uint8_t type = v.at<uint8_t>(b + 0x30);
    if (type != 1 && type != 2) return std::nullopt;
    s.kind = ShapeKind(type);
    return s;
}

bool inside(const Shape& s, vec3 pos, vec3 rot, vec3 p) {
    if (s.kind == ShapeKind::Sphere) {
        const vec3 d = p - pos;
        return glm::dot(d, d) < s.radius * s.radius;
    }
    if (s.kind != ShapeKind::Box) return false;
    const vec3 l = glm::transpose(rotation(rot)) * (p - pos);
    return std::abs(l.x) <= s.half.x && std::abs(l.y) <= s.half.y && std::abs(l.z) <= s.half.z;
}

bool crosses(const Shape& s, vec3 pos, vec3 rot, vec3 a, vec3 b) {
    if (s.kind != ShapeKind::Plane) return false;
    const glm::mat3 rt = glm::transpose(rotation(rot));
    const vec3 la = rt * (a - pos), lb = rt * (b - pos);
    if (s.oneSided ? !(la.y > 0 && lb.y <= 0) : !((la.y > 0) != (lb.y > 0))) return false;
    if (la.y == lb.y) return false;
    const vec3 h = la + (lb - la) * (la.y / (la.y - lb.y));
    return std::abs(h.x) <= s.half.x && std::abs(h.z) <= s.half.z;
}

// ---- runtime ---------------------------------------------------------------------------------------

namespace {
// 0x5C0980 / 0x5C6F60: the path value at t is the instance's position and rotation; ours as Object::body (model space).
void placeSpline(Object& o) {
    const auto& s = *o.spline;
    const auto& p = *s.path;
    vec3 pos = o.pos;
    glm::mat3 rot = rotation(o.rot);
    if (p.position) {
        const auto v = formats::evalSpline(*p.position, s.t, false);
        pos = vec3(v[0], v[1], v[2]) + vec3(p.origin[0], p.origin[1], p.origin[2]) + s.offset;
    }
    if (p.rotation) {
        const auto q = formats::evalSpline(*p.rotation, s.t, true);
        rot = glm::mat3_cast(glm::quat(q[3], q[0], q[1], q[2])); // xyzw, same handedness as the intro Euler angles
    }
    glm::mat4 world(rot), intro(rotation(o.rot));
    world[3] = glm::vec4(pos, 1), intro[3] = glm::vec4(o.pos, 1);
    o.body = glm::inverse(intro) * world;
}

nlohmann::json stateOf(const Object& o) {
    nlohmann::json fields = nlohmann::json::object(), strings = nlohmann::json::object(), properties = nlohmann::json::object();
    if (o.script) {
        for (const auto& [offset, value] : o.script->fields) {
            const auto initial = o.script->initial.find(offset);
            if (initial == o.script->initial.end() || initial->second != value) fields[std::to_string(offset)] = value;
        }
        for (const auto& [offset, value] : o.script->strings) {
            const auto initial = o.script->initialStrings.find(offset);
            if (initial == o.script->initialStrings.end() || initial->second != value) strings[std::to_string(offset)] = value;
        }
        for (const auto& [key, value] : o.script->properties) {
            const auto initial = o.script->initialProperties.find(key);
            if (initial == o.script->initialProperties.end() || initial->second != value)
                properties[std::to_string(key)] = value;
        }
    }
    if (o.state == 0 && !o.inside && !o.firstIn && !o.hidden && o.engagement < 0 && o.offset == vec3(0) && fields.empty() && strings.empty() && properties.empty() && !o.spline &&
        (!o.joint || o.joint->position == 0) && !o.loose) return nullptr; // birth state
    nlohmann::json out = {{"state", o.state}, {"t", o.t}, {"inside", o.inside}, {"firstIn", o.firstIn},
            {"offset", {o.offset.x, o.offset.y, o.offset.z}}, {"hidden", o.hidden}, {"engagement", o.engagement}, {"scriptFields", std::move(fields)},
            {"scriptStrings", std::move(strings)}, {"scriptProperties", std::move(properties)}};
    if (const auto& s = o.spline)
        out["spline"] = {s->t, s->dir, s->mode, s->playing, s->toKey, s->clip, s->target, s->from, s->to, s->count};
    if (o.joint && o.joint->position != 0) out["joint"] = o.joint->position;
    if (o.loose) out["loose"] = true;
    return out;
}
void applyState(Object& o, const nlohmann::json* s) {
    o.state = 0, o.t = 0, o.inside = o.firstIn = false, o.offset = vec3(0);
    o.hidden = false, o.engagement = -1, o.loose = false;
    o.machine.reset();
    o.fibers.clear();
    if (o.script) {
        o.script->fields = o.script->initial;
        o.script->strings = o.script->initialStrings;
        o.script->properties = o.script->initialProperties;
    }
    if (const float p = s && s->is_object() ? s->value("joint", 0.0f) : 0.0f; o.joint || p != 0) {
        auto& j = o.joint ? *o.joint : o.joint.emplace();
        j.position = p;
        for (auto& r : j.ranges) r.inside = r.lo <= p && p <= r.hi;
    }
    if (!s || !s->is_object()) return;
    o.state = s->value("state", 0), o.t = s->value("t", 0.0);
    o.inside = s->value("inside", false), o.firstIn = s->value("firstIn", false);
    o.hidden = s->value("hidden", false), o.engagement = s->value("engagement", -1), o.loose = s->value("loose", false);
    const auto off = s->value("offset", std::vector<float>{0, 0, 0});
    if (off.size() == 3) o.offset = {off[0], off[1], off[2]};
    if (o.script) {
        const auto fields = s->find("scriptFields");
        if (fields != s->end() && fields->is_object())
            for (const auto& [key, value] : fields->items())
                if (value.is_number_unsigned() || value.is_number_integer())
                    o.script->fields[uint32_t(std::stoul(key))] = value.get<uint32_t>();
        const auto strings = s->find("scriptStrings");
        if (strings != s->end() && strings->is_object())
            for (const auto& [key, value] : strings->items())
                if (value.is_string()) o.script->strings[uint32_t(std::stoul(key))] = value.get<std::string>();
        const auto properties = s->find("scriptProperties");
        if (properties != s->end() && properties->is_object())
            for (const auto& [key, value] : properties->items())
                if (value.is_number_unsigned() || value.is_number_integer())
                    o.script->properties[uint64_t(std::stoull(key))] = value.get<uint32_t>();
    }
    if (o.state < 0 || o.state > 2 || (o.state == 1 && !o.clips)) o.state = 2;
    if (const auto sp = s->find("spline"); o.spline && sp != s->end() && sp->is_array() && sp->size() == 10) {
        auto& p = *o.spline;
        const auto& a = *sp;
        p.t = a[0].get<float>(), p.dir = a[1].get<int>() < 0 ? -1 : 1, p.mode = uint8_t(std::min(a[2].get<int>(), 4));
        p.playing = a[3].get<bool>(), p.toKey = a[4].get<bool>(), p.clip = a[5].get<bool>();
        p.target = a[6].get<int16_t>(), p.from = a[7].get<int16_t>(), p.to = a[8].get<int16_t>(), p.count = a[9].get<int16_t>();
        placeSpline(o);
    }
}
} // namespace

void Runtime::setBigfile(const cdc::BigfileSet* bigfile) {
    bigfile_ = bigfile;
    playerScript_.reset();
    playerHiddenGroups_ = playerGearGroups_ = 0;
    playerObjects_.clear();
    if (!bigfile_) return;
    const auto bytes = bigfile_->read("pc-w\\lara.drm");
    if (!bytes) return;
    const auto drm = cdc::parseDrm(*bytes);
    std::vector<ScriptType> types;
    for (uint32_t i = 0; i < drm.sections.size(); ++i)
        if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(drm, i));
    auto program = std::make_shared<vm::Program>(std::move(types));
    playerScript_ = program->instantiate(nextAuxiliaryUid_--, 205); // LaraBase player instance script
    // Gear groups (type 205 ints +40/+44/+48, shown by its vt 62-64 / hidden by vt 65-67 through instance native 61)
    // start hidden: Lara shows Thor's gauntlets and belt only where a script puts them on (GAMEPLAY.md "Thor's gear").
    if (const ScriptType* lara = program->type(205))
        for (const auto& m : lara->members) {
            if (m.inlineInit) playerScript_->fields[m.offset] = playerScript_->initial[m.offset] = *m.init; // floats 1.0, gear ids
            if ((m.offset == 40 || m.offset == 44 || m.offset == 48) && m.init && *m.init >= 1 && *m.init <= 32)
                playerGearGroups_ |= 1u << (*m.init - 1);
        }
    playerHiddenGroups_ = playerGearGroups_;
}

void Runtime::setUnits(const std::vector<UnitRef>& units) {
    std::erase_if(objects_, [&](const Object& o) {
        if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == o.unit; })) return false;
        if (auto s = stateOf(o); !s.is_null()) stash_[std::to_string(o.uid)] = std::move(s); // kept for streaming back
        return true;
    });
    for (auto it = unitScripts_.begin(); it != unitScripts_.end();) { // streamed out: the unit script ends with its unit
        if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == it->first; })) { ++it; continue; }
        if (const auto a = auxiliary_.find(it->second); a != auxiliary_.end()) {
            std::erase_if(threads_, [&](const auto& t) { return t.first == a->second->uid; });
            auxiliary_.erase(a);
        }
        it = unitScripts_.erase(it);
    }
    for (const auto& u : units)
        if (std::none_of(objects_.begin(), objects_.end(), [&](const Object& o) { return o.unit == u.name; })) addUnit(u);
}

void Runtime::addUnit(const UnitRef& u) {
    if (!bigfile_ || !u.drm) return;
    if (objectNames_.empty()) {
        const auto list = bigfile_->read("\\tr8\\pc-w\\objectlist.txt");
        if (!list) throw FormatError("\\tr8\\pc-w\\objectlist.txt not in bigfile");
        objectNames_ = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
    }
    const auto unit = formats::parseUnit(*u.drm);
    const size_t firstNew = objects_.size();
    std::vector<ScriptType> types;
    for (uint32_t i = 0; i < u.drm->sections.size(); ++i)
        if (u.drm->sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(*u.drm, i));
    auto program = std::make_shared<vm::Program>(std::move(types));

    for (const auto& in : unit.intros) {
        const auto n = objectNames_.find(in.objectId);
        if (n == objectNames_.end()) continue;
        auto kind = classify(n->second);
        // The native base decides plane vs volume, not the object name: l3_main_hall "triggervolume" intros 4128 / 4129
        // run triggerplane scripts (3529 -> triggerplanebase) whose vt 54 is the crossing handler [O bytecode].
        if (kind == Kind::Volume || kind == Kind::Plane)
            for (const ScriptType* t = in.scriptId ? program->type(in.scriptId) : nullptr; t; t = program->type(t->superId))
                if (t->nativeName == "triggerplanebase" || t->nativeName == "triggervolumebase") {
                    kind = t->nativeName == "triggerplanebase" ? Kind::Plane : Kind::Volume;
                    break;
                }
        if (!kind && !in.scriptId && !in.spline) continue;
        Object o;
        o.uid = in.uniqueId, o.unit = u.name, o.name = n->second, o.kind = kind.value_or(Kind::Scripted);
        o.pos = vec3(in.position[0], in.position[1], in.position[2]) + u.offset;
        o.rot = {in.rotation[0], in.rotation[1], in.rotation[2]};
        if (in.spline) { // 0x5BA280 (no saved spline state): key 0, then the intro's mode, direction, key / clip
            Object::Spline s;
            s.path = in.spline, s.offset = u.offset;
            const uint32_t m = in.splineMode & 0xF;
            s.mode = m == 1 ? 1 : m == 2 ? 2 : m == 4 ? 3 : m == 8 ? 4 : 0;
            s.playing = s.mode != 0;
            s.dir = in.flags[0] & 1 ? -1 : 1;
            if (in.splineFlags & 1) s.toKey = s.playing = in.splineTo != 0, s.target = in.splineTo;
            if (in.splineFlags & 2) s.clip = true, s.from = in.splineFrom, s.to = in.splineTo;
            o.spline = std::move(s);
            placeSpline(o);
        }
        if (o.kind == Kind::Volume || o.kind == Kind::Plane) {
            if (!in.data) continue;
            const auto shape = parseShape(*u.drm, *in.data, o.kind == Kind::Plane);
            if (!shape) continue;
            o.shape = *shape;
        }
        if (const ScriptType* t = in.scriptId ? program->type(in.scriptId) : nullptr) {
            o.script = program->instantiate(o.uid, in.scriptId);
            if (playerScript_)
                for (const ScriptType* type = t; type; type = program->type(type->superId))
                    for (const auto& member : type->members)
                        if (member.typeId == 205)
                            o.script->fields[member.offset] = o.script->initial[member.offset] = uint32_t(playerScript_->uid);
            for (const auto& f : t->functions)
                if (f.vtIndex >= 0)
                    for (const int32_t target : functionTargets(*t, f)) o.links[f.vtIndex].push_back(target);
        }
        if (o.kind == Kind::Switch || o.kind == Kind::Door) {
            auto& set = animSets_[o.name];
            if (!set) {
                set = std::make_shared<AnimSet>();
                if (auto bytes = bigfile_->read("pc-w\\" + o.name + ".drm")) {
                    const auto drm = cdc::parseDrm(std::move(*bytes));
                    uint32_t model = 0;
                    for (const uint32_t id : formats::objectRenderModels(drm))
                        if ((model = id)) break;
                    std::vector<std::string> warnings;
                    for (auto& sk : anim::decodeSkeletons(drm, warnings))
                        if (model && sk.renderModelId == model) set->skeleton = std::make_shared<scene::Skeleton>(std::move(sk));
                    // [I] clip choice by name: switches pull ("active_pulling") then hold ("active_idle");
                    // doors play their opening clip ("doors_opening", "10_opening", "gate_opening", "open")
                    // then hold the open idle ("idle_door_open", "20_idle_open", "gate_open", "idleopen").
                    const auto list = anim::animList(drm);
                    auto pick = [&](auto pred) -> const anim::AnimEntry* {
                        const auto e = std::find_if(list.begin(), list.end(), pred);
                        return e == list.end() ? nullptr : &*e;
                    };
                    const anim::AnimEntry* tr = nullptr;
                    const anim::AnimEntry* hold = nullptr;
                    if (o.kind == Kind::Switch) {
                        tr = pick([](const anim::AnimEntry& e) { return e.name == "active_pulling"; });
                        hold = pick([](const anim::AnimEntry& e) { return e.name == "active_idle"; });
                    } else {
                        tr = pick([](const anim::AnimEntry& e) { return contains(e.name, "opening"); });
                        if (!tr) tr = pick([](const anim::AnimEntry& e) { return e.name == "open"; });
                        hold = pick([&](const anim::AnimEntry& e) {
                            return &e != tr && contains(e.name, "open") && (contains(e.name, "idle") || endsWith(e.name, "_open"));
                        });
                    }
                    auto clips = std::make_shared<std::vector<anim::Clip>>();
                    auto names = std::make_shared<std::vector<std::string>>();
                    for (const auto* e : {tr, hold}) {
                        if (!e) break;
                        for (const auto& s : drm.sections)
                            if (s.type == cdc::SectionType::Animation && s.id == e->animId) {
                                clips->push_back(anim::decodeClip(s.data)), names->push_back(e->name);
                                break;
                            }
                    }
                    if (set->skeleton && clips->size() == 2) set->clips = std::move(clips), set->names = std::move(names);
                    // [O] Switch reset: physical-class record (object base +0xC8) +0x34 -> its first state record: +4 flags
                    // (bit 0 one-way: ms_ei_switch_oneway_wall, ma_ei_wall_switch_electric), +0xC reset delay in frames
                    // (ma_ei_switch_wall_chapel 1, ca_ei_switch_wall_autoreset 60, ms_ei_resetting_switch 150) [I: units].
                    if (const auto base = formats::objectBaseSection(drm); base && o.kind == Kind::Switch) {
                        const auto& ob = drm.sections[*base];
                        const auto rec = cdc::internalPointer(ob, 0xC8);
                        const auto list = rec ? cdc::internalPointer(ob, *rec + 0x34) : std::nullopt;
                        const auto first = list ? cdc::internalPointer(ob, *list) : std::nullopt;
                        if (first && *first + 0x10 <= ob.data.size()) {
                            uint32_t flags;
                            float delay;
                            std::memcpy(&flags, ob.data.data() + *first + 4, 4);
                            std::memcpy(&delay, ob.data.data() + *first + 0xC, 4);
                            if (!(flags & 1) && delay > 0) set->resetAfter = delay / 30.0f;
                        }
                    }
                }
            }
            o.skeleton = set->skeleton;
            o.clips = set->clips;
            o.clipNames = set->names;
            o.resetAfter = set->resetAfter;
        }
        // [I] Ship hall doors without a script (chapter 6's l6_ship_upper_hallway_a/b copies of the l1 doors): their
        // volumes' door calls ISTYPE-fail on them, so nothing can open them, yet chapter 6 walks through. They stand
        // open, as the l1 doors once opened.
        const bool openDoor = o.kind == Kind::Door && !o.script && o.clipNames &&
                              std::ranges::find(*o.clipNames, "idle_open") != o.clipNames->end();
        const int32_t uid = o.uid;
        addObject(std::move(o), false);
        if (Object* added = openDoor ? findMut(uid) : nullptr; added && added->state == 0) added->state = 2;
    }
    for (size_t i = firstNew; i < objects_.size(); ++i) {
        fire(EventKind::Start, objects_[i]);
        objects_[i].mainPending = true;
        fire(EventKind::Main, objects_[i]);
    }
    // The unit script (Level+0x70 DTP +0x1C0, an NsUnit type; LEVELS.md "Unit script") is created with its unit and
    // enters state 0 as its own thread (e.g. l3_main_bedroom: Run, then the hint_01 help condition via vt 28).
    // unitref.Get returns the same instance. [I: started after the unit's intros so its instancerefs resolve]
    // A unit script without a startup function still listens (l1_cine_ship_sinking 5997: only vt 11 / 13).
    if (program->type(unit.scriptId)) {
        const auto self = auxiliaryInstance(program, unit.scriptId);
        unitScripts_[u.name] = {program.get(), unit.scriptId};
        if (const Function* f = program->startup(unit.scriptId)) {
            const auto thread = makeMachine(self->uid);
            if (!thread->start(*self, *f)) recordFailure(self->uid, u.name + " unit script", *thread);
            if (thread->active()) threads_.push_back({self->uid, thread});
            adopt(thread);
        }
        notifyPlayerUnit(self->uid); // 0x4B0050: a new listener gets the player's current unit(s)
    }
}

void Runtime::addObject(Object o, bool start) {
    const auto key = std::to_string(o.uid);
    if (stash_.contains(key)) applyState(o, &stash_[key]), stash_.erase(key); // restored / streamed back in
    objects_.push_back(std::move(o));
    if (!start) return;
    fire(EventKind::Start, objects_.back());
    objects_.back().mainPending = true;
    fire(EventKind::Main, objects_.back());
}

const Object* Runtime::find(int32_t uid) const {
    const auto it = std::find_if(objects_.begin(), objects_.end(), [&](const Object& o) { return o.uid == uid; });
    return it == objects_.end() ? nullptr : &*it;
}
Object* Runtime::findMut(int32_t uid) { return const_cast<Object*>(find(uid)); }

std::shared_ptr<vm::Instance> Runtime::instance(int32_t uid) const {
    for (const auto& candidate : objects_) if (candidate.uid == uid) return candidate.script;
    if (playerScript_ && playerScript_->uid == uid) return playerScript_;
    for (const auto& [key, instance] : auxiliary_) if (instance->uid == uid) return instance;
    return {};
}

void Runtime::recordFailure(const Object& o) {
    if (o.machine) recordFailure(o.uid, o.name, *o.machine);
}
void Runtime::recordFailure(int32_t uid, const std::string& name, const vm::Machine& m) {
    if (failures_[{uid, m.failure()}]++ == 0)
        std::fprintf(stderr, "script: uid %d (%s) handler stopped: %s\n", uid, name.c_str(), m.failure().c_str());
}

// STATE (0x57C830) ends every thread of the object but the one now running its new state.
void Runtime::endOtherThreads(int32_t uid, const vm::Machine* keep) {
    std::erase_if(threads_, [&](const auto& t) { return t.first == uid && t.second.get() != keep; });
    Object* o = findMut(uid);
    if (!o) return;
    std::erase_if(o->fibers, [&](const auto& m) { return m.get() != keep; });
    if (o->machine.get() != keep) o->machine.reset();
}

// After a thread ran: apply its STATE effects; states entered from another object's call become threads.
void Runtime::adopt(const std::shared_ptr<vm::Machine>& m) {
    if (const auto changed = m->takeStateChange()) endOtherThreads(*changed, m.get());
    for (const auto& [uid, f] : m->takeStarts()) {
        const auto self = instance(uid);
        if (!self) continue;
        const auto thread = makeMachine(uid);
        if (!thread->start(*self, *f)) recordFailure(uid, find(uid) ? find(uid)->name : "unit script", *thread);
        if (thread->active()) threads_.push_back({uid, thread});
        adopt(thread);
    }
}

bool Runtime::grapplePulled(int32_t uid) {
    Object* o = findMut(uid);
    if (!o || !o->script) return false;
    for (int16_t vt = 55; vt <= 60; ++vt) {
        const Function* f = o->script->program->method(o->script->typeId, vt);
        if (!f || f->native || f->returnType || f->wordCount <= 1) continue;
        if (o->machine && o->machine->active()) o->fibers.push_back(std::move(o->machine));
        if (o->name.find("_rb_grapple") != std::string::npos) o->loose = true;
        const auto machine = makeMachine(uid);
        o->machine = machine;
        const bool completed = machine->call(*o->script, vt);
        if (!completed) recordFailure(uid, o->name, *machine);
        adopt(machine);
        return completed;
    }
    return false;
}

void Runtime::endCinematic(int32_t uid) {
    if (Object* object = findMut(uid)) fire(EventKind::CinematicEnd, *object);
}

uint32_t Runtime::cameraSplineValue(const vm::Value& v) {
    if (v.kind != vm::Value::Kind::Member) return v.bits;
    const auto holder = instance(v.uid);
    if (!holder || !holder->program) return 0xFFFFFFFFu;
    const Member* m = holder->program->member(holder->typeId, v.offset);
    const auto field = holder->fields.find(v.offset);
    if (field == holder->fields.end() && (!m || !m->init)) return 0xFFFFFFFFu;
    // The unit is the owner's (0x775760); unit/global scripts (auxiliary instances) use the unit of their program.
    const auto owner = std::find_if(objects_.begin(), objects_.end(), [&](const Object& o) {
        return o.uid == holder->uid || (holder->uid < 0 && o.script && o.script->program == holder->program);
    });
    if (owner == objects_.end()) return 0xFFFFFFFFu;
    auto unit = std::find(cameraUnits_.begin(), cameraUnits_.end(), owner->unit);
    if (unit == cameraUnits_.end()) unit = cameraUnits_.insert(unit, owner->unit);
    const uint32_t index = field != holder->fields.end() ? field->second : *m->init;
    return uint32_t(unit - cameraUnits_.begin()) << 16 | (index & 0xFFFF);
}

void Runtime::uberEvent(int32_t uid, uint32_t event) {
    const auto arg = vm::Value::scalar(event);
    callback(uid, "uberobjectbase", 6, std::span(&arg, 1)); // prototype 6 = vt 54
}

void Runtime::callback(int32_t uid, const char* base, size_t prototype, std::span<const vm::Value> args) {
    const auto self = instance(uid);
    if (!self || !self->program) return;
    for (const ScriptType* t = self->program->type(self->typeId); t; t = self->program->type(t->superId))
        if (t->nativeName == base) {
            if (t->prototypes.size() > prototype && t->prototypes[prototype] >= 0) run(uid, t->prototypes[prototype], args);
            return;
        }
}

void Runtime::run(int32_t uid, int16_t vt, std::span<const vm::Value> args) {
    const auto self = instance(uid);
    if (!self || !self->program) return;
    const Function* f = self->program->method(self->typeId, vt);
    if (!f || f->native || f->wordCount <= 1) return; // not overridden: the base slot does nothing
    const auto machine = makeMachine(uid);
    Object* o = findMut(uid);
    if (o && o->machine && o->machine->active()) o->fibers.push_back(std::move(o->machine));
    if (o) o->machine = machine;
    const bool ok = machine->call(*self, vt, args);
    if (!ok) recordFailure(uid, o ? o->name : "unit script", *machine);
    else if (!o && machine->active()) threads_.emplace_back(uid, machine); // unit/global script: its own thread
    adopt(machine);
}

void Runtime::setPlayerUnit(uint32_t unit, std::optional<uint32_t> previous) {
    playerUnit_ = {unit, previous};
    for (const auto& [name, key] : unitScripts_)
        if (const auto a = auxiliary_.find(key); a != auxiliary_.end()) notifyPlayerUnit(a->second->uid);
}

void Runtime::notifyPlayerUnit(int32_t uid) {
    if (!playerUnit_) return;
    const auto& [unit, previous] = *playerUnit_;
    // gamebase prototypes 17 / 18 / 19 = vt 11 / 12 / 13 (bind 0x7B44A0 -> 0x798370 / 0x7983B0 / 0x7983F0).
    if (previous) {
        const auto now = vm::Value::scalar(unit), before = vm::Value::scalar(*previous);
        callback(uid, "gamebase", 17, std::span(&now, 1));
        callback(uid, "gamebase", 18, std::span(&before, 1));
    } else {
        const auto now = vm::Value::scalar(unit);
        callback(uid, "gamebase", 19, std::span(&now, 1));
    }
}

void Runtime::sendMessage(uint32_t channel, uint32_t id, std::string payload, int32_t sender) {
    messages_.push_back({channel, id, std::move(payload), sender});
}

void Runtime::cinematicEvent(int32_t uid, uint32_t id) {
    const vm::Value arg = vm::Value::scalar(id);
    run(uid, 45, std::span(&arg, 1));
}

void Runtime::cameraSplineDone(uint32_t value) {
    std::vector<int32_t> uids;
    for (const Object& o : objects_) uids.push_back(o.uid);
    const vm::Value arg = vm::Value::scalar(value);
    for (const int32_t uid : uids) callback(uid, "dispatcherbase", 7, std::span(&arg, 1));
}

// Runs the intro's real handler. If an unsupported opcode/binding stops it, the older static-link path is
// retained; any activation already produced by the VM suppresses that fallback to avoid duplicate effects.
void Runtime::fire(EventKind k, Object& o, bool announce) {
    if (announce && k != EventKind::Start && k != EventKind::Main) events_.push_back({k, o.uid, o.uid, tick_});
    int16_t vt = -1;
    switch (k) {
    case EventKind::FirstIn: vt = kVtOnFirstIn; break;
    case EventKind::In: vt = kVtOnIn; break;
    case EventKind::Out: vt = kVtOnOut; break;
    case EventKind::Cross: vt = kVtPlaneCross; break;
    case EventKind::Use:
        vt = contains(o.name, "phole_lever") ? kVtLeverMoved : kVtSwitchUsed;
        // Switch bases (e.g. 3637) receive the state change in vt 20 and dispatch the used state to vt 55; a script that
        // overrides vt 20 below the vt 55 definer handles the use there with state 0 (l2 elevator switch 10112: vt20(0)
        // -> STATE 1 -> the elevator) [I: the state value; our machines start with zeroed arguments].
        if (vt == kVtSwitchUsed && o.script && o.script->program)
            for (const ScriptType* t = o.script->program->type(o.script->typeId); t; t = o.script->program->type(t->superId)) {
                const auto has = [&](int16_t slot) { return std::ranges::any_of(t->functions, [&](const auto& f) { return f.vtIndex == slot; }); };
                if (has(kVtSwitchUsed)) break;
                if (has(20)) { vt = 20; break; }
            }
        break;
    case EventKind::Start: vt = -2; break; // StartObject enters state 0 (Program::startup)
    case EventKind::Main: vt = -3; break;  // function 2 is the object's main/polling script
    case EventKind::CinematicEnd: vt = -1; break; // non-virtual post-stream helper
    default: return;
    }
    const int32_t source = o.uid;
    const size_t eventCount = events_.size();
    bool completed = false;
    if (o.script) {
        if (o.machine && o.machine->active() && k == EventKind::Main) {
            completed = true; // function 2 waits until the constructor's thread finishes (retried in tick)
        } else {
            // Each event runs as its own thread in the exe; a sleeping earlier handler keeps running beside it.
            if (o.machine && o.machine->active()) o.fibers.push_back(std::move(o.machine));
            const auto machine = makeMachine(source);
            o.machine = machine;
            completed = machine->call(*o.script, vt);
            if (!completed) recordFailure(o.uid, o.name, *machine);
            adopt(machine);
            if (k == EventKind::Main) o.mainPending = false;
        }
    }
    if (events_.size() != eventCount || completed) return;
    const auto it = o.links.find(vt);
    if (it == o.links.end() || fallbackDepth_ >= 8) return; // static links can form cycles (survey found l2 plane loops)
    ++fallbackDepth_;
    for (const int32_t target : std::vector<int32_t>(it->second)) { // copy: activation may not invalidate
        events_.push_back({EventKind::Activate, source, target, tick_});
        Object* t = findMut(target);
        if (t) fire(EventKind::Use, *t, false);
        if (t && t->kind == Kind::Door && t->state == 0) t->state = t->clips ? 1 : 2, t->t = 0; // [I] doors only open
    }
    --fallbackDepth_;
}

// Unit/global script instances (unitref/globalsref Get), one per (program, type).
std::shared_ptr<vm::Instance> Runtime::auxiliaryInstance(const std::shared_ptr<vm::Program>& program, uint32_t typeId) {
    auto& slot = auxiliary_[std::make_pair(program.get(), typeId)];
    if (slot) return slot;
    const auto instance = slot = program->instantiate(nextAuxiliaryUid_--, typeId);
    // The globals (gameglobalsbase) hold engine-created script objects in SCRIPT members that have no initial value
    // (type 5 +4/+8/+12/+272/+356/+432/+436 -> types 76/75/74/72/73/60/77; l5 soundvolumes call `globals.member12`
    // vt 20 of type 74) [O bytecode]; ours are auxiliary instances like the globals [I: the engine's construction].
    for (const ScriptType* t = program->type(typeId); t; t = program->type(t->superId))
        if (t->nativeName == "gameglobalsbase") {
            for (const Member& m : program->type(typeId)->members)
                if ((m.type & 0x8F) == 11 && m.typeId && !m.init && m.typeId != typeId)
                    instance->fields[m.offset] = instance->initial[m.offset] = uint32_t(auxiliaryInstance(program, m.typeId)->uid);
            break;
        }
    return instance;
}

// A VM thread whose natives act for `source` (the intro uid that owns the thread).
std::shared_ptr<vm::Machine> Runtime::makeMachine(int32_t source) {
    {
        auto resolve = [this](int32_t uid) { return instance(uid); };
        auto auxiliary = [this](const std::shared_ptr<vm::Program>& program, uint32_t typeId) {
            return auxiliaryInstance(program, typeId);
        };
        auto native = [this, source, resolve, auxiliary](vm::NativeContext& nc) -> bool {
            const char* name = nc.name;
            // Natives with no per-object state (tru.exe thunks that ignore the receiver value).
            if (!std::strcmp(name, "globaleffect") && (nc.function == 5 || nc.function == 6) && nc.args.size() == 1) {
                // FadeOut / FadeIn over N frames (0x4A4EB0 / 0x4A4EF0: GAMELOOP_SetScreenWipe(10, 100 | 0, max(N, 1))).
                fadeTarget_ = nc.function == 5 ? 1.0f : 0.0f;
                fadeStep_ = 1.0f / float(std::max<int32_t>(int32_t(nc.args[0].bits), 1));
                return true;
            }
            if (!std::strcmp(name, "larabase") && (nc.function == 59 || nc.function == 60) && nc.args.empty()) {
                // INSTANCE_Post(player, 0x1D5 | 0x1D6, 0) (0x778020 / 0x778040); the message's meaning is [U].
                events_.push_back({EventKind::PlayerMessage, source, nc.function == 59 ? 0x1D5 : 0x1D6, tick_});
                return true;
            }
            // larabase 52 (thunk 0x79A4B0 -> 0x777DD0): INSTANCE_Post 0x85 to the bike in the instanceref argument and to
            // Lara, who is on it at once (the chapter-start intros' event-1 handlers: l4 5484 -> 5482, l5 7530 -> 7229).
            if (!std::strcmp(name, "larabase") && nc.function == 52 && nc.args.size() == 1) {
                const vm::Value& a = nc.args[0];
                int32_t bike = a.kind == vm::Value::Kind::Object ? a.uid : int32_t(a.bits);
                if (a.kind == vm::Value::Kind::Member)
                    if (const auto holder = resolve(a.uid)) bike = int32_t(holder->fields[a.offset]);
                if (bike) mounts_.push_back(bike);
                return true;
            }
            // Thor's gear objects (g_gauntletleft / g_gauntletright / g_belt) Lara carries in her object slots, set by
            // the type 205 equip helpers vt 59-61 that unit/chapter scripts call: larabase 38 (0x777E90: an object of
            // that name is in one of her 15 slots), 37 (0x7161F0: attach it), 42 (0x6F5A10: drop a slot's object). The
            // show helpers vt 62-64 only unhide a draw group while its object is carried. [I] our 42 drops every gear
            // object (its slot argument is not tracked; the equip helpers call it together) but not Mjolnir
            // ("wp_thorhammer", given by 37 at the end of 250_val_hammer; no script drops it [O string survey]). 14
            // (0x7793F0: post 0x19B/0x146 to a slot) has nothing to stow.
            if (!std::strcmp(name, "larabase") && (nc.function == 14 || nc.function == 37 || nc.function == 38 || nc.function == 42)) {
                const auto text = std::find_if(nc.args.begin(), nc.args.end(), [](const vm::Value& a) { return a.text; });
                if (nc.function == 37 && text != nc.args.end()) playerObjects_.insert(*text->text);
                if (nc.function == 42) std::erase_if(playerObjects_, [](const std::string& o) { return o.starts_with("g_"); });
                nc.result = vm::Value::scalar(nc.function == 38 && text != nc.args.end() && playerObjects_.contains(*text->text));
                return true;
            }
            // microphone (bind 0x7AC8A0): the audio listener's placement between camera and Lara (1/2 two floats ->
            // 0x5852F0 / 0x585320, 3-5 one float, 6 attach to an instance, 7 detach -> 0x585350). Ours: the listener
            // stays on the camera [U: the blend].
            if (!std::strcmp(name, "microphone") && nc.function >= 1 && nc.function <= 7) return true;
            // Receiver-independent bindings whose side effects other engine systems own (sound: below, once bound).
            // reverb 1 (0x4AA750 -> 0x582B70) copies its 13 settings words into the audio environment: our mixer has no
            // reverb, so it is passive too.
            // cinematic (bind 0x7AC050): 0 Create (0x4ABD30 -> 0x4A4D10) and 2 IsCreated (0x4ABD40) the member's stream,
            // which the cinematic base's preload state (3523 fn 13) polls. Ours loads a cutscene when it plays, so it is
            // always ready [I].
            if (!std::strcmp(name, "cinematic") && (nc.function == 0 || nc.function == 2)) {
                nc.result = vm::Value::scalar(1);
                return true;
            }
            // unlockable (bind 0x7AD470): 1 IsUnlocked (0x5DB8A0), 2 Unlock (0x5D6600) the extras item; ours has no extras.
            // animated 1(animation): plays a decorative object's animation member (l4_traprooms plate 5717 -> glyph 5724);
            // not drawn [I].
            for (const char* passive : {"soundgroup", "corebase", "uipopupmgr", "uilocalstrproperty", "globaleffect", "reverb",
                                        "unlockable", "animated"})
                if (!std::strcmp(name, passive)) { nc.result = vm::Value::scalar(0); return true; }
            // gamebase 32 (0x797E50 -> 0x714DE0) frees a global effect list at a cinematic's start (cinematic_object base
            // vt 45 event 0); no effect on play.
            if (!std::strcmp(name, "gamebase") && nc.function == 32 && nc.args.empty()) return true;
            // uberobjectevent 0 (CALLU: receiver = the event vt 54 got, its index; argument = the member {object, index}):
            // does the event name this member's event?
            // A local event member arrives as its index (vm::Machine::push).
            if (!std::strcmp(name, "uberobjectevent") && nc.function == 0 && nc.args.size() == 1) {
                uint32_t index = nc.args[0].bits;
                if (nc.args[0].kind == vm::Value::Kind::Member) {
                    const auto holder = resolve(nc.args[0].uid);
                    const auto f = holder ? holder->fields.find(nc.args[0].offset + 4) : decltype(holder->fields.end()){};
                    index = holder && f != holder->fields.end() ? f->second : 0xFFFFFFFFu;
                }
                nc.result = vm::Value::scalar(index == nc.receiver.bits);
                return true;
            }
            // uberobjectbase 1 on a command held in a local (its index, vm::Machine::push), as the receiver or the argument
            // of the object (l7_arctic_sea 9115's constructor: `PUSHLA 0; CALL1`).
            if (!std::strcmp(name, "uberobjectbase") && nc.function == 1 && uber_) {
                const bool local = nc.receiver.kind == vm::Value::Kind::Scalar && nc.args.empty();
                const bool argument = nc.args.size() == 1 && nc.args[0].kind == vm::Value::Kind::Scalar;
                const int32_t uid = nc.receiver.kind == vm::Value::Kind::Object ? nc.receiver.uid : source;
                if ((local || argument) && find(uid)) {
                    uint32_t result = 0;
                    if (!uber_(uid, 1, local ? nc.receiver.bits : nc.args[0].bits, 0, nc.args, result)) return false;
                    nc.result = vm::Value::scalar(result);
                    return true;
                }
            }
            // Receivers: a member address (object uid + offset), an object, or a native value held in a local.
            auto self = resolve(nc.receiver.uid);
            // An instanceref held in a local may also be an object value (instance 14/15 GetInstanceRef results).
            const bool refObject = nc.receiver.kind == vm::Value::Kind::Object && !std::strcmp(name, "instanceref");
            const bool inLocal = nc.receiver.kind == vm::Value::Kind::Scalar || refObject;
            uint32_t localValue = refObject ? uint32_t(nc.receiver.uid) : nc.receiver.bits;
            if (inLocal && !std::strcmp(name, "instanceref")) {
                auto target = resolve(int32_t(localValue));
                if (nc.function == 7 && nc.args.empty()) { // Get: null when unbound (0x4AC070)
                    nc.result = target ? vm::Value::object(target->uid) : vm::Value::scalar(0);
                    return true;
                }
                if ((nc.function == 3 || nc.function == 6) && nc.args.empty()) { nc.wait = !target; return true; }
            }
            // unitref 0 (0x4A52E0): receiver and argument name the same unit. Values are stream unit ids: a unitref member's
            // initial value, or the unit a gamebase callback passes (l1_cine_ship_sinking unit script vt 11: the player's
            // new unit == its own 41).
            if (!std::strcmp(name, "unitref") && nc.function == 0 && nc.args.size() == 1) {
                const auto unit = [&](const vm::Value& v) -> uint32_t {
                    if (v.kind != vm::Value::Kind::Member) return v.bits;
                    const auto holder = resolve(v.uid);
                    if (!holder || !holder->program) return 0;
                    if (const auto f = holder->fields.find(v.offset); f != holder->fields.end()) return f->second;
                    const Member* m = holder->program->member(holder->typeId, v.offset);
                    return m && m->init ? *m->init : 0;
                };
                const uint32_t a = unit(nc.receiver), b = unit(nc.args[0]);
                nc.result = vm::Value::scalar(a != 0 && (a & 0xFFFF) == (b & 0xFFFF));
                return true;
            }
            // Equality whose receiver is an argument held in a local (OnMessage's channel: `channel == this.channel`,
            // 0x4A2E90 compares the channels) against a member.
            if (inLocal && !std::strcmp(name, "messagechannel") && nc.function == 3 && nc.args.size() == 1) {
                const auto& a = nc.args[0];
                const auto other = a.kind == vm::Value::Kind::Member ? resolve(a.uid) : nullptr;
                nc.result = vm::Value::scalar(localValue == (other ? other->fields[a.offset] : a.bits));
                return true;
            }
            if (!std::strcmp(name, "music") && nc.function >= 1 && nc.function <= 3) {
                MusicRequest r{self ? self->uid : 0, nc.function, {}, -1, nc.args.empty() ? 0.0f : std::bit_cast<float>(nc.args[0].bits)};
                if (self && self->program && nc.receiver.kind == vm::Value::Kind::Member) {
                    if (const Member* m = self->program->member(self->typeId, nc.receiver.offset)) {
                        if (nc.function != 3) r.track = m->initText;
                        if (m->init2) r.slot = int32_t(*m->init2);
                    }
                } else if (nc.receiver.kind == vm::Value::Kind::Scalar && nc.receiver.text) { // a music local (Machine::call)
                    if (nc.function != 3) r.track = *nc.receiver.text;
                    r.slot = int32_t(nc.receiver.bits);
                }
                if (nc.function == 3 || !r.track.empty()) musicRequests_.push_back(std::move(r));
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!self || !self->program) return false;
            const auto program = self->program;
            const uint32_t off = nc.receiver.offset;
            auto field = [&]() -> uint32_t& { return self->fields[off]; };
            if (!std::strcmp(name, "clock") && nc.receiver.kind == vm::Value::Kind::Member) {
                Clock& c = clocks_[(uint64_t(uint32_t(self->uid)) << 32) | off]; // tru.exe 0x4A3450..0x4A34F0, update 0x4A3960
                switch (nc.function) {
                case 0: if (c.state != 2) c = {2, 0, c.alarm, false}; return true;  // Start
                case 1: c.state = 0; return true;                                    // Stop
                case 2: if (c.state == 2) c.state = 1; return true;                  // Pause
                case 3: if (c.state == 1) c.state = 2; return true;                  // Resume
                case 4: nc.result = vm::Value::scalar(c.state == 2); return true;     // IsRunning
                case 5: case 7: nc.result = vm::Value::scalar(std::bit_cast<uint32_t>(c.elapsed)); return true;
                case 6: case 8: nc.result = vm::Value::scalar(std::bit_cast<uint32_t>(c.alarm)); return true;
                case 9:                                                                // SetAlarm
                    if (nc.args.size() != 1) return false;
                    c.alarm = std::bit_cast<float>(nc.args[0].bits), c.fired = false;
                    return true;
                case 10:                                                               // same clock
                    if (nc.args.size() != 1) return false;
                    nc.result = vm::Value::scalar(nc.args[0].kind == vm::Value::Kind::Member && nc.args[0].uid == self->uid &&
                                                  nc.args[0].offset == off);
                    return true;
                default: return false;
                }
            }
            if (!std::strcmp(name, "terraingroup") && (nc.function == 2 || nc.function == 3)) {
                const Member* m = program->member(self->typeId, off);
                if (!m || !m->init || !m->init2 || !terrain_ || (nc.function == 2 ? nc.args.size() != 1 : !nc.args.empty()))
                    return false;
                // Natural enabled semantics: tru.exe 0x5C2480 reports the disabled flag pair instead.
                uint32_t result = 0;
                if (!terrain_(self->uid, uint16_t(*m->init), *m->init2, nc.function, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "light") && (nc.function == 1 || nc.function == 2) && light_) {
                const Member* m = program->member(self->typeId, off);
                if (!m || !m->init || (nc.function == 1 ? nc.args.size() != 1 : !nc.args.empty())) return false;
                uint32_t result = 0;
                if (!light_(self->uid, *m->init, nc.function, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            // health on Lara's script: 9 current value; 1 / 2 hit her (death volumes and planes; non-player targets unsupported);
            // 3 heals her (0x4A5180: INSTANCE_Post(5, amount) = the health pack message, l3_main_hall 4129 passes 400).
            if (!std::strcmp(name, "health") && health_ &&
                (nc.function == 9 || ((nc.function == 1 || nc.function == 2 || nc.function == 3) && playerScript_ && self == playerScript_))) {
                uint32_t result = 0;
                if (!health_(nc.function, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "triggerplanebase") && (nc.function == 0 || nc.function == 1)) return true; // 0x79D8E0: empty
            if (!std::strcmp(name, "triggervolumebase") && (nc.function == 0 || nc.function == 1) && nc.args.size() == 1) {
                // Add (0x4A43E0) / Remove (0x4A4410) an instance the volume tests. Ours test Lara only, so adding or
                // removing her (or null) is satisfied; other instances are not tracked.
                const vm::Value& a = nc.args[0];
                int32_t uid = a.kind == vm::Value::Kind::Object ? a.uid : int32_t(a.bits);
                if (a.kind == vm::Value::Kind::Member) {
                    const auto holder = resolve(a.uid);
                    uid = holder ? int32_t(holder->fields[a.offset]) : 0;
                }
                if (uid != 0 && !(playerScript_ && uid == playerScript_->uid)) return false;
                if (Object* o = uid ? findMut(self->uid) : nullptr) o->tracksLara = nc.function == 0;
                return true;
            }
            if (!std::strcmp(name, "markup") && (nc.function == 0 || nc.function == 1) && markup_) {
                const Member* m = program->member(self->typeId, off);
                // The calling intro's unit; unit/global scripts (auxiliary instances) use the unit of their program.
                const auto owner = std::find_if(objects_.begin(), objects_.end(), [&](const Object& o) {
                    return o.uid == self->uid || (self->uid < 0 && o.script && o.script->program == program);
                });
                if (!m || !m->init || owner == objects_.end() || nc.args.size() != (nc.function == 0 ? 1u : 0u)) return false;
                uint32_t result = 0;
                if (!markup_(owner->unit, *m->init, nc.function, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "gamebase") && nc.function >= 4 && nc.function <= 7) {
                const size_t expected = nc.function == 4 ? 1 : 2;
                if (nc.args.size() != expected || nc.args[0].bits >= 256) return false;
                auto& value = gameValues_[nc.args[0].bits];
                if (nc.function == 4) nc.result = vm::Value::scalar(value);
                else if (nc.function == 5) value = uint8_t(nc.args[1].bits);
                else if (nc.function == 6) value = uint8_t(value + nc.args[1].bits);
                else value = uint8_t(value - nc.args[1].bits);
                return true;
            }
            if (!std::strcmp(name, "playerref")) {
                if (!playerScript_) return false;
                if (nc.function == 0) { nc.result = vm::Value::object(playerScript_->uid); return true; } // Get
                if (nc.function == 2) { nc.result = vm::Value::scalar(0); return true; } // WaitUntilLoaded: already loaded
                return false;
            }
            if (!std::strcmp(name, "messagechannel") && (nc.function == 0 || nc.function == 1)) return true; // debug stubs
            // Send (tru.exe 0x4A2E70): the app's native subscribers (message_), then the script listeners (sendMessage).
            if (!std::strcmp(name, "messagechannel") && nc.function == 2 && nc.args.size() == 2) {
                const std::string* text = nc.args[1].text;
                if (message_ && !message_(field(), nc.args[0].bits, text ? *text : std::string())) return false;
                sendMessage(field(), nc.args[0].bits, text ? *text : std::string(), self->uid);
                return true;
            }
            if (!std::strcmp(name, "modifiable") && nc.function == 3 && nc.args.size() == 2) {
                const auto value = resolve(nc.args[0].uid);
                if (!value) return false;
                field() = nc.args[0].kind == vm::Value::Kind::Member ? value->fields[nc.args[0].offset] : nc.args[0].bits;
                return true;
            }
            if ((!std::strcmp(name, "messagechannel") || !std::strcmp(name, "stickybool") ||
                 !std::strcmp(name, "stickyint") || !std::strcmp(name, "stickyfloat")) && nc.function == 3 &&
                nc.args.size() == 1) {
                const auto other = resolve(nc.args[0].uid);
                if (!other) return false;
                nc.result = vm::Value::scalar(field() == other->fields[nc.args[0].offset]);
                return true;
            }
            // cinematicevent 0: equality with the event id a cinematic_object's vt 45 receives; the members hold the ids
            // as their initial value (0, 1, 2, 5-8, 10-24).
            if (!std::strcmp(name, "cinematicevent") && nc.function == 0 && nc.args.size() == 1 && nc.receiver.kind == vm::Value::Kind::Member) {
                const Member* m = program->member(self->typeId, off);
                const auto it = self->fields.find(off);
                const uint32_t mine = it != self->fields.end() ? it->second : m && m->init ? *m->init : 0;
                nc.result = vm::Value::scalar(mine == nc.args[0].bits);
                return true;
            }
            // modifiable 7 (0x4A5A90) posts message 0x2E with the value to the instance; its receivers are [U].
            if (!std::strcmp(name, "modifiable") && nc.function == 7 && nc.args.size() == 1) return true;
            // uigamedata 1 (0x60D630) always returns 0; 14 (0x77BBF0) sets a global UI byte no gameplay reads [U].
            if (!std::strcmp(name, "uigamedata") && nc.function == 1 && nc.args.empty()) {
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!std::strcmp(name, "uigamedata") && nc.function == 14 && nc.args.size() == 1) return true;
            if (!std::strcmp(name, "instance") && (nc.function == 0 || nc.function == 1)) return true; // debug print
            if (!std::strcmp(name, "instanceref") && nc.function == 9 && nc.args.size() == 2) { // SetProperty
                self->properties[(uint64_t(off) << 32) | nc.args[0].bits] = nc.args[1].bits & 0xFF;
                return true;
            }
            if (!std::strcmp(name, "instanceref") && (nc.function == 12 || nc.function == 15) && nc.args.size() == 1) {
                nc.result = vm::Value::scalar(self->properties[(uint64_t(off) << 32) | nc.args[0].bits]); // GetProperty
                return true;
            }
            if (!std::strcmp(name, "instanceref") && nc.function == 4) {
                nc.result = vm::Value::scalar(resolve(int32_t(field())) ? 1u : 0u); // target-present guard
                return true;
            }
            if (!std::strcmp(name, "instanceref") && (nc.function == 3 || nc.function == 6)) { // loaded/birthed wait
                nc.wait = !resolve(int32_t(field())); // blocks until the target's unit streams in
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!std::strcmp(name, "instanceref") && nc.function == 7) { // Get: null when unbound (0x4AC070)
                auto target = resolve(int32_t(field()));
                nc.result = target ? vm::Value::object(target->uid) : vm::Value::scalar(0);
                return true;
            }
            if ((!std::strcmp(name, "unitref") && nc.function == 3) ||
                (!std::strcmp(name, "globalsref") && nc.function == 0)) { // Get
                const Member* m = program->member(self->typeId, off);
                if (!m || !m->argTypeId) return false;
                nc.result = vm::Value::object(auxiliary(program, m->argTypeId)->uid);
                return true;
            }
            // Experience-adjustment settings (registration 0x7AEDE0; fns 13-18 read the options block at
            // [0xC59264]+0x3EF94..0x3EFAC, e.g. fn 17 -> +0x3EFAC that unit Run() compares with 2 "master
            // survivalist"). We have no such menu: every setting reads 0 [I: the defaults].
            if (!std::strcmp(name, "uiexperienceadjustmentmenu")) {
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!std::strcmp(name, "checkpointbase") && nc.function == 3 && nc.args.empty()) {
                events_.push_back({EventKind::Checkpoint, self->uid, self->uid, tick_});
                return true;
            }
            // Reset water level (0x778430 posts 0x1B3 with the default; static water already sits at it).
            if (!std::strcmp(name, "watervolumebase") && nc.function == 3 && nc.args.size() <= 1) return true;
            if (!std::strcmp(name, "watervolumebase") && nc.function == 4 && nc.args.empty()) {
                nc.result = vm::Value::scalar(0); // static water: zero depth offset
                return true;
            }
            if (!std::strcmp(name, "playersensor") && nc.function == 2) return true; // void native sensor interface call
            if (!std::strcmp(name, "playersensor") && (nc.function == 0 || nc.function == 1)) {
                // SetEnabled / IsEnabled: byte +0xF of the sensor (thunks 0x7ACE40 / 0x7ACE60); what the sensor senses [U].
                if (nc.function == 0 && nc.args.size() == 1) { field() = nc.args[0].bits & 0xFF; return true; }
                if (nc.function == 1 && nc.args.empty()) { nc.result = vm::Value::scalar(field() & 0xFF); return true; }
                return false;
            }
            if (!std::strcmp(name, "spline")) {
                Object* object = findMut(self->uid);
                if (!object || !object->spline) { // every thunk null-checks instance +0x284 (0x4AA8B0..)
                    nc.result = vm::Value::scalar(nc.function == 3 ? 0xFFFFFFFFu : 0u);
                    return true;
                }
                uint32_t result = 0;
                if (!splineNative(*object, nc.function, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            // instance 21 (bool, 0x4A3FD0: clears/sets instance +0x1C flag 0x100), 18 (bool, 0x4A3F50: toggles the
            // instance's +0x2D8 component via 0x5B5A60 / 0x5B5AB0) and 23 (calls vt +0x18 of a native object
            // argument, 0x4A41D0): no modelled system reads them [U: their effect].
            if (!std::strcmp(name, "instance") && (nc.function == 18 || nc.function == 21 || nc.function == 23) &&
                nc.args.size() == 1)
                return true;
            // modifiable 1 (two floats, 0x4A59F0) posts message 0x30 to the instance like 7 does with 0x2E [U: receivers].
            if (!std::strcmp(name, "modifiable") && nc.function == 1 && nc.args.size() == 2) return true;
            if (!std::strcmp(name, "unitref") && nc.function == 2 && nc.args.empty()) return true; // 0x79C670: returns 0
            if (!std::strcmp(name, "larabase") && (nc.function == 55 || nc.function == 56 || nc.function == 29) &&
                nc.args.size() == 1) {
                // 56 SetControlEnabled (0x777FC0: input flag 0x8AB4E6 that zeroes the move axes, "Lara.
                // SetControlEnabledForReveal"); World applies it only while a spline camera plays [I: safety]. 55
                // weapons (0x777F70 holsters when false), 29 a global byte (0x698890) [U].
                if (nc.function == 56) playerControl_ = (nc.args[0].bits & 0xFF) != 0;
                return true;
            }
            if (!std::strcmp(name, "autosplinecamera") && nc.function == 0 && nc.args.size() == 1 && camera_) {
                const uint32_t value = cameraSplineValue(nc.args[0]);
                return value != 0xFFFFFFFFu && camera_(0, cameraUnits_[value >> 16], value & 0xFFFF, value);
            }
            // gamebase 53 (0x7767C0) is 52 through CameraManager +0x54 instead of +0x50; reveals whose flag +44 is set
            // (l1_kraken_room 53) use it [I: the same switch, +0x54's difference unknown].
            if (!std::strcmp(name, "gamebase") && (nc.function == 52 || nc.function == 53 || nc.function == 43) &&
                nc.args.size() == 1 && camera_) {
                const bool r = camera_(nc.function == 43 ? 2 : 1, {}, nc.args[0].bits, 0);
                nc.result = vm::Value::scalar(r);
                return nc.function == 43 || r;
            }
            // gamebase 30 (0x7761B0 -> 0x60F5B0): bool clears/sets game flag 0x4000 of 0xE7F0B4 and UI element 0xC
            // (0x655280); reveal scripts call it with true after their camera [U: what the flag gates].
            if (!std::strcmp(name, "gamebase") && nc.function == 30 && nc.args.size() == 1) return true;
            // gamebase 2 / 24 set / ramp (target, seconds) the global time scale 0xE7F1D4, 23 ramps the second one
            // 0xE7F1A0 (0x4A3DD0, 0x776090, 0x776030 -> ramp records read by 0x78C4E0); the game loop multiplies
            // timeMult by both (reveal planes call 24(0, 0), adrenaline trackers 2). Ours keep one fixed
            // tick rate [U: not applied; our adrenaline window is Combat.cpp's].
            if (!std::strcmp(name, "gamebase") && ((nc.function == 2 && nc.args.size() == 1) ||
                                                   ((nc.function == 23 || nc.function == 24) && nc.args.size() == 2)))
                return true;
            if (!std::strcmp(name, "cameraspline") && nc.function == 0 && nc.args.size() == 1) { // equal (0x775790)
                nc.result = vm::Value::scalar(cameraSplineValue(nc.receiver) == cameraSplineValue(nc.args[0]));
                return true;
            }
            if (!std::strcmp(name, "larabase") && (nc.function == 25 || nc.function == 27 || nc.function == 28) && nc.args.empty()) {
                // 27 posts 0x10D (8) to Lara (0x777C90), 28 posts 0x106 (8) (0x777CB0, a cinematic's end); 25 posts 0x156
                // to a global instance (0x777C10) [U].
                if (nc.function != 25) events_.push_back({EventKind::PlayerMessage, source, nc.function == 27 ? 0x10D : 0x106, tick_});
                return true;
            }
            // 16 (0x79A090 -> 0x779480): hide (false) / show the two models of a weapon slot (flag 0x800); cinematics show
            // her weapons again at their end. Ours draws weapons from the combat state [I].
            if (!std::strcmp(name, "larabase") && nc.function == 16 && nc.args.size() == 2) return true;
            // 30 (0x777CE0) posts 0x146 to the slot-6 object; 68 (0x778150 -> 0x6ED460) and 67 (0x778140 -> 0x6ED510) reset
            // Lara state blocks (67: seven timestamps from a global clock, seven counters zeroed); they run around a
            // cinematic [U: no effect on ours].
            if (!std::strcmp(name, "larabase") && (nc.function == 30 || nc.function == 67 || nc.function == 68) && nc.args.empty())
                return true;
            // uitext 57 (0x7A6D10 -> 0x4C32E0): returns member +0x210 of the text's lazily created widget (a localized
            // string handle) [U]; we have no widget, so null. uigamedata 106 (0x7A14F0 -> 0x77C8B0) stores that string as
            // the level description when it is non-null (globals vt 20, "Setting level description text...") [U: unused].
            // uitext 54 (0x7A6C80 -> 0x4C31F0) is the same kind of getter; uitext 4 is UIText vtable slot 1 [U].
            if (!std::strcmp(name, "uitext") && (nc.function == 54 || nc.function == 57) && nc.args.empty()) {
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!std::strcmp(name, "uitext") && nc.function == 4) return true;
            if (!std::strcmp(name, "uigamedata") && nc.function == 106) return true;
            // Screen presentation the cinematic base and its letterbox / saving screens run (uiscreen 6/7 = UIScreen vtable
            // slots 0x24/0x28 show/hide, 22 colour, 90/92 = 0x4B2030/0x4B2050; uicolortimeline 0/10 set, 12/13 =
            // 0x4BF370/0x4BF3C0 play/stop; uigamedata 3 = 0x77BC00 flag). Ours draws no script UI screens [U].
            if ((!std::strcmp(name, "uiscreen") && (nc.function == 6 || nc.function == 7 || nc.function == 22 ||
                                                    nc.function == 90 || nc.function == 92)) ||
                (!std::strcmp(name, "uicolortimeline") && (nc.function == 0 || nc.function == 10 || nc.function == 12 ||
                                                           nc.function == 13)) ||
                (!std::strcmp(name, "uigamedata") && nc.function == 3) ||
                (!std::strcmp(name, "uivec3timeline") && nc.function == 15)) // 0x4C49E0 play (chapter title animation)
                return true;
            // cinematic 7 (0x79EB10 -> 0x60D630) always returns 0.
            if (!std::strcmp(name, "cinematic") && nc.function == 7 && nc.args.empty()) { nc.result = vm::Value::scalar(0); return true; }
            if (!std::strcmp(name, "stickybool") || !std::strcmp(name, "stickyint") || !std::strcmp(name, "stickyfloat")) {
                if (nc.function == 0 && nc.args.size() == 1) { field() = nc.args[0].bits; return true; } // Set
                if ((nc.function == 1 || nc.function == 2) && nc.args.empty()) {
                    nc.result = vm::Value::scalar(field());
                    return true;
                } // Get
                return false;
            }
            if (!std::strcmp(name, "modifiable")) {
                if (nc.function == 8 && nc.args.empty()) { nc.result = vm::Value::scalar(field()); return true; } // Get
                if (nc.function != 4 || nc.args.size() != 1) return false;
                const uint32_t value = nc.args[0].bits;
                field() = value;
                if (Object* target = findMut(self->uid); target && target->kind == Kind::Door) {
                    // [I] Values per door family: the house door (clips 0_idle, 10_opening, …) opens on 0 and closes on 1
                    // (literals in its type 12171). Ship hall doors (clips open, idle_open, close, idle_close; type 6519
                    // vt 54/55/56 = member defaults 1/2/3) open on 1 and close on 2/3.
                    const bool ship = target->clipNames && std::ranges::find(*target->clipNames, "idle_open") != target->clipNames->end();
                    const bool opens = ship ? value == 1 : value == 0, changes = opens ? target->state == 0 : target->state != 0;
                    if (changes) {
                        events_.push_back({EventKind::Activate, source, target->uid, tick_});
                        target->state = opens ? (target->clips ? 1 : 2) : 0;
                        target->t = 0;
                    }
                }
                return true;
            }
            if (!std::strcmp(name, "instance") && nc.function == 27 && nc.args.empty()) {
                // GetUnit (thunk 0x7B2C20 -> 0x4B06F0): a 12-byte unitref of the instance's unit (+0xE0). Ours: the
                // unit's ordinal + 1 as an identity (mercs pass it to enemybase 23 [U: what the AI does with it]).
                const Object* object = findMut(self->uid);
                if (!object) return false;
                auto unit = std::find(cameraUnits_.begin(), cameraUnits_.end(), object->unit);
                if (unit == cameraUnits_.end()) unit = cameraUnits_.insert(unit, object->unit);
                nc.result = vm::Value::scalar(uint32_t(unit - cameraUnits_.begin()) + 1);
                return true;
            }
            if (!std::strcmp(name, "instance") && nc.function == 25 && nc.args.empty()) {
                if (Object* object = findMut(self->uid)) {
                    self->fields = self->initial;
                    self->strings = self->initialStrings;
                    self->properties = self->initialProperties;
                    object->state = 0, object->t = 0, object->offset = vec3(0), object->body = glm::mat4(1);
                    object->hidden = false, object->engagement = -1, object->mainPending = true;
                    ++visibilityVersion_;
                    return true;
                }
                return false;
            }
            if (!std::strcmp(name, "instance") && nc.function == 59 && nc.args.size() == 1) {
                if (Object* object = findMut(self->uid)) { object->engagement = int32_t(nc.args[0].bits); return true; }
                return false;
            }
            // 60 hides / shows; 12 (0x798D30 -> 0x4A4000) also sets the hide flag 0x800 with its collision and update
            // flags (230_val_intro's event 1 hides the startsnow bike 7528 when Lara rides on in the tunnel).
            if (!std::strcmp(name, "instance") && (nc.function == 60 || nc.function == 12) && nc.args.size() == 1) {
                if (Object* object = findMut(self->uid)) {
                    const bool hidden = nc.args[0].bits != 0;
                    if (object->hidden != hidden) { object->hidden = hidden; ++visibilityVersion_; }
                    return true;
                }
                return false;
            }
            if (!std::strcmp(name, "instance") && nc.function == 10 && nc.args.empty()) {
                // Kill (0x4A3EC0 -> 0x5B5740 sets the dead flag 0x20): the object leaves the scene for good.
                Object* object = findMut(self->uid);
                if (!object) return false;
                if (!object->hidden) object->hidden = true, ++visibilityVersion_;
                return true;
            }
            if (!std::strcmp(name, "instance") && nc.function == 61 && nc.args.size() == 2) {
                // HideUnhideDrawGroup(bit, show) (0x798F10 -> 0x5B3DD0): bit = draw group - 1 of the instance mask
                // +0x23C. Only Lara's model is drawn per group here (SubMesh::drawGroup); other receivers are ignored [I].
                if (self == playerScript_) {
                    const uint32_t bit = 1u << (nc.args[0].bits & 31), mask = nc.args[1].bits & 0xFF
                                                                                   ? playerHiddenGroups_ & ~bit
                                                                                   : playerHiddenGroups_ | bit;
                    if (mask != playerHiddenGroups_) playerHiddenGroups_ = mask, ++visibilityVersion_;
                }
                return true;
            }
            // instance 26 (0x4A4140): place the receiver at the transform its argument holds; an `instance` 4 result
            // (0x4AB2A0: the target's transform, ours the object). l1_ship_sinking_hall plane 1074 moves Lara onto
            // cinematic_object 1005 in l1_cine_ship_sinking, whose unit script then plays it (World::tick).
            if (!std::strcmp(name, "instance") && nc.function == 26 && nc.args.size() == 1 && self == playerScript_ &&
                nc.args[0].kind == vm::Value::Kind::Object && find(nc.args[0].uid)) {
                teleport_ = nc.args[0].uid;
                return true;
            }
            if (!std::strcmp(name, "instance") &&
                (nc.function == 4 || nc.function == 5 || nc.function == 6 || nc.function == 7 || nc.function == 8 ||
                 nc.function == 9 || nc.function == 14 || nc.function == 15 || nc.function == 16 || nc.function == 26 || nc.function == 28)) {
                nc.result = vm::Value::object(self->uid); // native interface of this instance (multibody etc.)
                return true;
            }
            if (!std::strcmp(name, "uberobjectbase") && (nc.function == 1 || nc.function == 2 || nc.function == 5) && uber_) {
                const Object* object = findMut(self->uid);
                if (!object) return false;
                const bool knownObject = std::any_of(objectNames_.begin(), objectNames_.end(), [&](const auto& entry) {
                    return entry.second == object->name;
                });
                if (!knownObject || (nc.function == 5 && !nc.args.empty())) return false; // native "object mismatch" path
                // fn 1 is invoked on the command member itself, or on the object with the command member as argument.
                const uint32_t commandOff = nc.args.size() == 1 && nc.args[0].kind == vm::Value::Kind::Member &&
                                                    nc.args[0].uid == self->uid ? nc.args[0].offset : off;
                const uint32_t command = nc.function == 1 ? self->fields[commandOff + 4] : 0;
                uint32_t commandIndex = 0;
                if (nc.function == 1)
                    for (const ScriptType* type = self->program->type(self->typeId); type; type = self->program->type(type->superId))
                        for (const auto& member : type->members)
                            if (!std::strcmp(self->program->nativeName(member.typeId), "uberobjectcommand")) {
                                if (member.offset == commandOff) goto commandIndexFound;
                                ++commandIndex;
                            }
            commandIndexFound:
                uint32_t result = 0;
                if (!uber_(object->uid, nc.function, command, commandIndex, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            // Pressure-plate joints (Object::Joint): multibody 1 (with the joint member and the body index), joint 17,
            // limit 3 (1, lo, hi, id) and multibody 79 (joint 0, 1, lo, hi, id) record the plate's ranges.
            if (Object* jo = findMut(self->uid); jo && ((!std::strcmp(name, "multibody") && (nc.function == 1 || nc.function == 79)) ||
                                                        (!std::strcmp(name, "joint") && nc.function == 17) ||
                                                        (!std::strcmp(name, "limit") && nc.function == 3))) {
                if (!jo->joint) jo->joint.emplace();
                auto& j = *jo->joint;
                const auto range = [&](const vm::Value& lo, const vm::Value& hi, const vm::Value& id) {
                    const float a = std::bit_cast<float>(lo.bits), b = std::bit_cast<float>(hi.bits);
                    j.ranges.push_back({a, b, int32_t(id.bits), a <= j.position && j.position <= b});
                };
                if (!std::strcmp(name, "joint")) j.prismatic = true;
                if (!std::strcmp(name, "limit") && nc.args.size() == 4) range(nc.args[1], nc.args[2], nc.args[3]);
                const bool plateLimit = !std::strcmp(name, "multibody") && nc.function == 79 && nc.args.size() == 5;
                if (plateLimit) range(nc.args[2], nc.args[3], nc.args[4]);
                nc.result = vm::Value::scalar(0);
                if (!plateLimit || !multibody_) return true;
            }
            if (!std::strcmp(name, "multibody") && multibody_) {
                if (nc.function == 2) self->fields[1] = 1; // Enable sets the script object's state byte
                uint32_t result = 0;
                if (multibody_(self->uid, nc.function, nc.args, result)) {
                    nc.result = vm::Value::scalar(result);
                    return true;
                }
            }
            if (!std::strcmp(name, "sound") && nc.function >= 1 && nc.function <= 5 && sound_ &&
                nc.receiver.kind == vm::Value::Kind::Member) {
                int32_t anchor = 0;
                if ((nc.function == 1 || nc.function == 3) && !nc.args.empty()) { // 3D: an instance(ref) argument
                    const auto& a = nc.args[0];
                    if (a.kind == vm::Value::Kind::Object) anchor = a.uid;
                    else if (a.kind == vm::Value::Kind::Member)
                        if (const auto holder = resolve(a.uid)) anchor = int32_t(holder->fields[a.offset]);
                }
                const Member* m = program->member(self->typeId, off);
                if (m && m->initAt && sound_(self->uid, off, *m->initAt, nc.function, anchor)) {
                    nc.result = vm::Value::scalar(0);
                    return true;
                }
            }
            // Existing engine systems already handle (or intentionally do not yet expose) these side effects.
            // Their return values are zero; unknown gameplay bindings remain unsupported instead of being ignored.
            for (const char* passive : {"sound", "soundgroup", "music", "corebase", "uipopupmgr",
                                        "uilocalstrproperty", "globaleffect"})
                if (!std::strcmp(name, passive)) { nc.result = vm::Value::scalar(0); return true; }
            if (!std::strcmp(name, "unit") && nc.function == 15 && nc.args.size() == 1) {
                unitEffect_ = nc.args[0].bits; // tru.exe 0x6D6F20 writes the global post-effect scalar
                return true;
            }
            if (enemy_ && (!std::strcmp(name, "enemybase") || !std::strcmp(name, "generatorbase") ||
                           (!std::strcmp(name, "health") && self != playerScript_))) {
                uint32_t result = 0;
                // GuardNode's noderef is a native member {record, node} (scripts never write it [I]): its declared value
                std::vector<vm::Value> args(nc.args.begin(), nc.args.end());
                if (!std::strcmp(name, "enemybase") && nc.function == 27 && args.size() == 4 && args[1].kind == vm::Value::Kind::Member) {
                    const vm::Instance* owner = args[1].uid == self->uid ? self.get() : nullptr;
                    const Member* m = owner ? owner->program->member(owner->typeId, args[1].offset) : nullptr;
                    if (!m || !m->init || !m->init2) return false;
                    args[1] = vm::Value::scalar(*m->init << 16 | (*m->init2 & 0xFFFF));
                }
                if (!enemy_(self->uid, name, nc.function, args, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            return false;
        };
        auto cinematic = [this, source](vm::Instance& target, int16_t vt) {
            // vt 57 plays (cinematic base 3523 fn 0 -> STATE 1); vt 63 only preloads (STATE 2 = fn 13 creates the stream
            // and waits) [O bytecode], so it is not an activation (l3_main_hall 4137 OnIn preloads 4108, 4128 plays it).
            if (vt != 57) return false;
            const auto* object = findMut(target.uid);
            if (!object || object->name != "cinematic_object") return false;
            events_.push_back({EventKind::Activate, source, object->uid, tick_});
            return true;
        };
        return std::make_shared<vm::Machine>(resolve, native, cinematic);
    }
}

// 0x5CF260 at 60 Hz: time advances dir * 30 frames/s (timeMult); a key target (flag 1) stops there, the clip range
// (flag 2, else the whole path) ends per mode: once stops, loop wraps, loop-count wraps `count` times, ping-pong
// turns. Then 0x5CC8F0 calls vt 40 (entered) / vt 41 (left) for each range of fn 19 the time crossed, with the
// arguments (spline handle, range id, direction). [O] callback slots: functions 53/54 of script type 3 (0x7120B0
// reads their vt indices). Mode (vt 42), direction (vt 43) and wrap (vt 44) callbacks are not sent.
void Runtime::tickSpline(Object& o) {
    auto& s = *o.spline;
    if (!s.playing) return;
    const float end = s.path->end(), prev = s.t;
    float t = prev + float(s.dir) * float(kDt) * 30.0f;
    if (s.toKey && (s.dir > 0 ? prev <= s.target && s.target <= t : t <= s.target && s.target <= prev)) {
        t = s.target, s.playing = s.toKey = false;
    } else {
        const float lo = s.clip ? std::clamp<float>(s.from, 0, end) : 0, hi = s.clip ? std::clamp<float>(s.to, lo, end) : end;
        if (t > hi || t < lo) {
            const bool high = t > hi;
            if ((s.mode == 2 || (s.mode == 3 && s.count > 0)) && hi > lo) {
                if (s.mode == 3) --s.count;
                t = high ? lo + std::fmod(t - hi, hi - lo) : hi - std::fmod(lo - t, hi - lo);
            } else if (s.mode == 4) {
                t = high ? hi : lo, s.dir = -s.dir;
            } else {
                t = high ? hi : lo, s.playing = false;
            }
        }
    }
    s.t = t;
    placeSpline(o);
    for (const auto range : std::vector(s.ranges)) { // copy: a callback may remove ranges
        const auto in = [&](float x) { return range[0] <= x && x <= range[1]; };
        if (in(prev) == in(t) || !o.script) continue;
        const vm::Value args[] = {vm::Value::scalar(0), vm::Value::scalar(uint32_t(range[2])), vm::Value::scalar(uint32_t(s.dir))};
        if (o.machine && o.machine->active()) o.fibers.push_back(std::move(o.machine));
        const auto machine = makeMachine(o.uid);
        o.machine = machine;
        if (!machine->call(*o.script, in(t) ? 40 : 41, args)) recordFailure(o.uid, o.name, *machine);
        adopt(machine);
    }
}

// `spline` natives (bind 0x7AD0D0, thunks 0x79C750..0x79C970) on the object's own path (instance +0x284).
bool Runtime::splineNative(Object& o, uint32_t function, std::span<const vm::Value> args, uint32_t& result) {
    auto& s = *o.spline;
    const int32_t end = int32_t(std::lround(s.path->end())), now = int32_t(std::lround(s.t));
    const auto arg = [&](size_t i) { return int32_t(args[i].bits); };
    const auto need = [&](size_t n) { return args.size() == n; };
    const auto playMode = [&](uint8_t mode, int32_t from, int32_t to) { // 0x4AA930
        s.mode = mode;
        if (from == 0 && to == -1) return;
        s.clip = true, s.from = int16_t(from), s.to = int16_t(to == -1 ? end : to);
    };
    switch (function) {
    case 0: result = uint32_t(end); return need(0);                                   // KeyCount (last key)
    case 1: result = uint32_t(now); return need(0);                                   // CurrentKey
    case 2:                                                                           // GoToKey (0x5C6F60)
        if (!need(1)) return false;
        s.t = arg(0) < 0 || arg(0) > end ? float(end) : float(arg(0));
        placeSpline(o);
        return true;
    case 3: result = s.mode; return need(0);                                          // PlayMode
    case 4: if (!need(3)) return false; playMode(uint8_t(std::clamp(arg(0), 0, 4)), arg(1), arg(2)); return true;
    case 5: s.playing = true; return need(0);                                         // Play (0x5C0950)
    case 6: s.playing = false; return need(0);                                        // Stop (0x5C0960)
    case 7: result = s.playing; return need(0);                                       // IsPlaying
    case 8: {                                                                         // PlayToKey(key, direction)
        if (!need(2)) return false;
        const int32_t key = arg(0), dir = arg(1);
        if (now == key) { s.t = float(key), s.playing = false; placeSpline(o); return true; } // 0x5CC760 / 0x5CC830
        s.toKey = s.playing = true, s.target = int16_t(key);
        if (!dir) s.dir = key < now ? -1 : 1;
        else if (s.dir = dir < 1 ? -1 : 1; (s.dir == 1) == (key < now)) s.mode = 2; // passes the end: loop round
        return true;
    }
    case 9: result = uint32_t(s.dir); return need(0);                                 // Direction
    case 10: if (!need(1)) return false; s.dir = arg(0) < 1 ? -1 : 1; return true;    // SetDirection (0x5C7080)
    case 11: result = uint32_t(int32_t(s.count)); return need(0);                     // LoopCount
    case 12: if (!need(1)) return false; s.count = int16_t(arg(0)); return true;
    case 13: if (!need(2)) return false; playMode(2, arg(0), arg(1)); return true;     // Loop(from, to)
    case 14: if (!need(3)) return false; playMode(3, arg(1), arg(2)); s.count = int16_t(arg(0)); return true;
    case 15: s.mode = 1; return need(0);                                              // Once
    case 16: if (!need(2)) return false; playMode(4, arg(0), arg(1)); return true;     // PingPong(from, to)
    case 17: result = s.clip; return need(0);
    case 18: s.clip = false; return need(0);
    case 19: {                                                                        // AddRange(from, to, id) 0x5D12D0
        if (!need(3)) return false;
        const auto it = std::ranges::find_if(s.ranges, [&](const auto& r) { return r[2] == arg(2); });
        if (it != s.ranges.end()) (*it)[0] = arg(0), (*it)[1] = arg(1);
        else s.ranges.push_back({arg(0), arg(1), arg(2)});
        return true;
    }
    case 20:                                                                          // HasRange(id) 0x5D1350
        if (!need(1)) return false;
        result = std::ranges::any_of(s.ranges, [&](const auto& r) { return r[2] == arg(0); });
        return true;
    case 21:                                                                          // RemoveRange(id) 0x5D1390
        if (!need(1)) return false;
        std::erase_if(s.ranges, [&](const auto& r) { return r[2] == arg(0); });
        return true;
    default: return false; // 22/23 create NtSplineSequence objects [U]
    }
}

std::string Runtime::helpCondition() const {
    for (const auto& [key, instance] : auxiliary_)
        for (const auto& [offset, text] : instance->strings)
            if (text.size() == 7 && text.starts_with("hint_")) return text;
    return {};
}

void Runtime::tick(const TickInput& in, const collision::CollisionMesh* col) {
    events_.clear();
    ++tick_;
    // Queued messages: the recipients are taken per message (a handler's own Sends wait for the next tick).
    for (auto& msg : std::exchange(messages_, {})) {
        std::vector<int32_t> uids;
        // 0x5D0670: each referrer of the named channel receives when its mode (the member's first init word) is 1 or 2;
        // mode 3 receives only its own posts. Mode 0 only sends.
        auto subscribed = [&](const vm::Instance& in) {
            if (!in.program) return false;
            const bool self = in.uid == msg.sender;
            for (const ScriptType* t = in.program->type(in.typeId); t; t = in.program->type(t->superId))
                for (const Member& m : t->members)
                    if (!std::strcmp(in.program->nativeName(m.typeId), "messagechannel")) {
                        const auto it = in.fields.find(m.offset);
                        const uint32_t mode = m.init.value_or(2);
                        if (it != in.fields.end() && it->second == msg.channel && (self ? mode == 3 : mode == 1 || mode == 2))
                            return true;
                    }
            return false;
        };
        for (const Object& o : objects_) if (o.script && subscribed(*o.script)) uids.push_back(o.uid);
        for (const auto& [key, in] : auxiliary_) if (subscribed(*in)) uids.push_back(in->uid);
        if (playerScript_ && subscribed(*playerScript_)) uids.push_back(playerScript_->uid);
        vm::Value args[3] = {vm::Value::scalar(msg.channel), vm::Value::scalar(msg.id), {}};
        args[2].kind = vm::Value::Kind::Constant, args[2].text = &*payloads_.insert(std::move(msg.payload)).first;
        for (const int32_t uid : uids) run(uid, 0, args);
    }
    // Clocks (NtClockThread update 0x4A3960): a running clock reaching its alarm fires once (Start and SetAlarm re-arm
    // it) and calls its owner's corebase prototype-25 slot with the clock (0x7961A0; NsCoreBase bind 0x7B3920).
    std::vector<uint64_t> alarms;
    for (auto& [key, c] : clocks_)
        if (c.state == 2 && (c.elapsed += float(kDt)) >= c.alarm && !c.fired) c.fired = true, alarms.push_back(key);
    for (const uint64_t key : alarms) {
        vm::Value clock;
        clock.kind = vm::Value::Kind::Member, clock.uid = int32_t(key >> 32), clock.offset = uint32_t(key);
        callback(clock.uid, "corebase", 25, std::span(&clock, 1));
    }
    fade_ = fade_ < fadeTarget_ ? std::min(fadeTarget_, fade_ + fadeStep_) : std::max(fadeTarget_, fade_ - fadeStep_);
    for (size_t i = 0; i < threads_.size(); ++i) { // copy: adopt may add or end threads
        const auto [uid, thread] = threads_[i];
        if (!instance(uid)) continue; // owner unloaded: dropped below
        if (!thread->tick(float(kDt))) recordFailure(uid, find(uid) ? find(uid)->name : "unit script", *thread);
        adopt(thread);
    }
    std::erase_if(threads_, [&](const auto& t) { return !t.second->active() || !instance(t.first); });
    for (auto& o : objects_) {
        if (o.spline) tickSpline(o);
        for (size_t i = 0; i < o.fibers.size(); ++i) { // copy: a STATE may end the object's threads
            const auto fiber = o.fibers[i];
            if (!fiber->tick(float(kDt))) recordFailure(o.uid, o.name, *fiber);
            adopt(fiber);
        }
        std::erase_if(o.fibers, [](const auto& m) { return !m->active(); });
        if (o.machine && o.machine->active()) {
            const auto machine = o.machine;
            if (!machine->tick(float(kDt))) recordFailure(o.uid, o.name, *machine);
            adopt(machine);
        }
        if (o.mainPending && (!o.machine || !o.machine->active())) fire(EventKind::Main, o);
    }
    holding_ = false, carried_ = vec3(0);
    const vec3 up(0, 0, kTestPointHeight), p = in.pos + up, prev = (hasLast_ ? lastPos_ : in.pos) + up;
    const bool pressed = in.interact && !interactHeld_;
    const vec3 fwd(std::cos(in.yaw), std::sin(in.yaw), 0);

    Object* useSwitch = nullptr;
    float best = kReach;
    for (auto& o : objects_) {
        switch (o.kind) {
        case Kind::Volume: {
            const bool now = inside(o.shape, o.pos, o.rot, p);
            if (now && !o.inside) { // Enter (vt 54): OnFirstIn once, then OnIn
                fire(EventKind::Enter, o);
                if (!o.firstIn) o.firstIn = true, fire(EventKind::FirstIn, o);
                o.inside = true;
                fire(EventKind::In, o);
                // A tracked instance entering (0x5CD520 -> 0x5C8A40): Lara calls prototype 10 (vt 58) with reason 1
                // [I: 2/3 = the first update / teleport cases, not distinguished]; soundvolumes set music variable 0x7D.
                if (o.tracksLara) callback(o.uid, "triggervolumebase", 10, std::array{vm::Value::scalar(1)});
            } else if (!now && o.inside) { // Exit (vt 55)
                o.inside = false;
                fire(EventKind::Out, o);
                if (o.tracksLara) callback(o.uid, "triggervolumebase", 11, std::array{vm::Value::scalar(1)});
            }
            break;
        }
        case Kind::Plane:
            if (hasLast_ && crosses(o.shape, o.pos, o.rot, prev, p)) fire(EventKind::Cross, o);
            break;
        case Kind::Switch:
        case Kind::Door:
            if (o.state == 1 && (o.t += kDt) >= (*o.clips)[0].durationSeconds()) {
                o.state = 2, o.t = 0;
                if (o.kind == Kind::Switch) fire(EventKind::Use, o);
            } else if (o.state == 2) {
                o.t += kDt;
                // A resetting switch returns to idle after its delay (resetAfter) and can be used again.
                if (o.kind == Kind::Switch && o.resetAfter >= 0 && o.t >= o.resetAfter)
                    o.state = 0, o.t = 0;
            }
            if (o.kind == Kind::Switch && o.state == 0 && pressed) {
                const float d = glm::length(vec2(o.pos - in.pos));
                if (d < best && std::abs(o.pos.z - in.pos.z) < kReachHeight) best = d, useSwitch = &o;
            }
            break;
        case Kind::Block: {
            if (!in.interact || holding_) break;
            const vec3 c = o.pos + o.offset;
            if (glm::length(vec2(c - in.pos)) > kReach + o.half.x || std::abs(c.z - in.pos.z) > kReachHeight) break;
            holding_ = true; // [I] gripped: World keeps Lara's stick out of the controller
            if (std::abs(in.move.y) <= 0.5f) break;
            // [I] Move along the world axis closest to Lara's facing: away (push) or toward her (pull); Lara
            // moves with it. Blocked when the block (push) or Lara (pull) would run into level collision.
            vec3 axis = std::abs(fwd.x) > std::abs(fwd.y) ? vec3(fwd.x > 0 ? 1 : -1, 0, 0) : vec3(0, fwd.y > 0 ? 1 : -1, 0);
            if (in.move.y < 0) axis = -axis;
            const float step = float(kBlockSpeed * kDt);
            // Terrain only (mask 0x0F): the block's own collision box is a dynamic of the same mesh (src/physics).
            if (col && col->raycast(c + vec3(0, 0, 20), axis, glm::dot(o.half, glm::abs(axis)) + step, 0x0F)) break;
            if (col && in.move.y < 0 && col->raycast(in.pos + vec3(0, 0, 100), axis, 100 + step, 0x0F)) break;
            o.offset += axis * step;
            carried_ = axis * step;
            break;
        }
        case Kind::Scripted:
            break;
        }
    }
    if (useSwitch) {
        useSwitch->state = useSwitch->clips ? 1 : 2, useSwitch->t = 0;
        if (useSwitch->state == 2) fire(EventKind::Use, *useSwitch);
    }
    hasLast_ = true, lastPos_ = in.pos, interactHeld_ = in.interact;
}

std::optional<std::vector<glm::mat4>> Runtime::pose(const Object& o) const {
    if (o.kind == Kind::Block) {
        if (o.offset == vec3(0)) return std::nullopt;
        // Meshes are placed with T*R(intro): a world displacement d is R^-1 d in model space.
        return std::vector<glm::mat4>{glm::translate(glm::mat4(1), glm::transpose(rotation(o.rot)) * o.offset)};
    }
    if (o.offset != vec3(0)) // a fallen or carried body: world displacement in model space, as a block
        return std::vector<glm::mat4>{glm::translate(glm::mat4(1), glm::transpose(rotation(o.rot)) * o.offset) * o.body};
    if (o.body != glm::mat4(1)) return std::vector<glm::mat4>{o.body};
    if (!o.clips || !o.skeleton || o.state == 0) return std::nullopt;
    const auto& clip = (*o.clips)[o.state == 1 ? 0 : 1];
    const auto world = anim::worldMatrices(*o.skeleton, anim::sample(clip, *o.skeleton, o.t, o.state == 2));
    return anim::skinPalette(*o.skeleton, world);
}

void Runtime::setBody(int32_t uid, const glm::mat4& model) {
    if (Object* o = findMut(uid)) o->body = model;
}

void Runtime::setJointPosition(int32_t uid, float position) {
    Object* o = findMut(uid);
    if (!o || !o->joint) return;
    o->joint->position = position;
    std::vector<int32_t> entered;
    for (auto& r : o->joint->ranges) {
        const bool in = r.lo <= position && position <= r.hi;
        if (in && !r.inside) entered.push_back(r.id);
        r.inside = in;
    }
    // instance prototype 47 = vt 34 (type 3's prototype table); args (multibody, joint, range id) [I: the first two].
    for (const int32_t id : entered)
        callback(uid, "instance", 47, std::array{vm::Value::scalar(0), vm::Value::scalar(0), vm::Value::scalar(uint32_t(id))});
}

std::vector<std::pair<std::pair<vec3, vec3>, uint32_t>> Runtime::debugLines() const {
    std::vector<std::pair<std::pair<vec3, vec3>, uint32_t>> out;
    for (const auto& o : objects_) {
        if (o.kind != Kind::Volume && o.kind != Kind::Plane) continue;
        const glm::mat3 r = rotation(o.rot);
        const uint32_t c = o.kind == Kind::Plane ? 0xFFFF00FF : o.inside ? 0xFF00FF00 : 0xFF00C0FF;
        auto line = [&](vec3 a, vec3 b) { out.push_back({{o.pos + r * a, o.pos + r * b}, c}); };
        const vec3 h = o.shape.half;
        if (o.shape.kind == ShapeKind::Sphere) {
            for (int axis = 0; axis < 3; ++axis)
                for (int k = 0; k < 24; ++k) {
                    auto pt = [&](int i) {
                        const float a = float(i) * 6.2831853f / 24;
                        vec3 v(0);
                        v[(axis + 1) % 3] = std::cos(a) * o.shape.radius, v[(axis + 2) % 3] = std::sin(a) * o.shape.radius;
                        return v;
                    };
                    line(pt(k), pt(k + 1));
                }
        } else if (o.shape.kind == ShapeKind::Plane) {
            const vec3 q[4] = {{-h.x, 0, -h.z}, {h.x, 0, -h.z}, {h.x, 0, h.z}, {-h.x, 0, h.z}};
            for (int k = 0; k < 4; ++k) line(q[k], q[(k + 1) % 4]);
            line(q[0], q[2]);
        } else {
            auto corner = [&](int i) { return vec3((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z); };
            for (int i = 0; i < 8; ++i)
                for (const int bit : {1, 2, 4})
                    if (!(i & bit)) line(corner(i), corner(i | bit));
        }
    }
    return out;
}

nlohmann::json Runtime::serialize() const {
    nlohmann::json j = stash_;
    j["gameValues"] = gameValues_;
    for (const auto& o : objects_) {
        if (auto s = stateOf(o); !s.is_null()) j[std::to_string(o.uid)] = std::move(s);
        else j.erase(std::to_string(o.uid));
    }
    // Lara's script (gear stickybools +52/+80/+108) and hidden draw groups.
    nlohmann::json fields = nlohmann::json::object();
    if (playerScript_)
        for (const auto& [offset, value] : playerScript_->fields)
            if (const auto it = playerScript_->initial.find(offset); it == playerScript_->initial.end() || it->second != value)
                fields[std::to_string(offset)] = value;
    j["player"] = {{"hiddenGroups", playerHiddenGroups_}, {"objects", playerObjects_}, {"scriptFields", std::move(fields)}};
    return j;
}

void Runtime::restore(const nlohmann::json& j) {
    gameValues_.fill(0);
    messages_.clear(), mounts_.clear(); // a load replaces a chapter start still being announced
    if (j.is_object() && j.contains("gameValues") && j["gameValues"].is_array())
        for (size_t i = 0; i < gameValues_.size() && i < j["gameValues"].size(); ++i)
            gameValues_[i] = j["gameValues"][i].get<uint8_t>();
    stash_ = j.is_object() ? j : nlohmann::json::object();
    stash_.erase("gameValues");
    const nlohmann::json player = stash_.value("player", nlohmann::json::object());
    stash_.erase("player");
    if (const uint32_t mask = player.value("hiddenGroups", playerGearGroups_); mask != playerHiddenGroups_)
        playerHiddenGroups_ = mask, ++visibilityVersion_;
    playerObjects_ = player.value("objects", std::set<std::string>{});
    if (playerScript_) {
        playerScript_->fields = playerScript_->initial;
        if (const auto f = player.find("scriptFields"); f != player.end() && f->is_object())
            for (const auto& [key, value] : f->items())
                if (value.is_number_unsigned() || value.is_number_integer())
                    playerScript_->fields[uint32_t(std::stoul(key))] = value.get<uint32_t>();
    }
    for (auto& o : objects_) {
        const auto key = std::to_string(o.uid);
        applyState(o, stash_.contains(key) ? &stash_[key] : nullptr);
        stash_.erase(key);
    }
    threads_.clear(); // active sleep frames do not persist
    hasLast_ = false; // no plane crossings across the jump
}

} // namespace ou::script
