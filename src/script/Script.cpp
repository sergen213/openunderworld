// Script types, intro links, trigger shapes and the object runtime. Evidence: docs/formats/SCRIPTS.md.
#include "script/Script.hpp"
#include "script/Vm.hpp"

#include "collision/Collision.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <unordered_map>
#include <cctype>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

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
        for (const auto& [p0, n] : {std::pair{t3, c[2]}, std::pair{t5, c[4]}})
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

constexpr uint32_t kAnchorBit = 0x80000000u; // uberobjectbase 2 result: Object value bits = this | HInfo marker id

const char* kEventNames[] = {"enter", "firstin", "in", "out", "cross", "use", "activate", "start", "main", "checkpoint",
                             "cinematicend", "playermessage", "autosave", "wetness"};

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
    // "swich": l7_helheim01's fourth rune symbol is spelled ar_uo_rune_wall_swich_a in the object list [O data].
    if (contains(n, "switch") || contains(n, "swich") || contains(n, "lever")) return Kind::Switch;
    if (contains(n, "door") || contains(n, "gate")) return Kind::Door;
    return std::nullopt;
}

constexpr float kReach = 200;          // [I] switch use distance (horizontal) from Lara's root
constexpr float kReachHeight = 250;    // [I]
constexpr double kDt = 1.0 / 60.0;     // ticks are game::kTickSeconds
constexpr uint32_t kTriggerMover = 0x100; // ObjectBase oflags: the instance is on the triggers' list (0x5BA810 -> 0x5CF680)

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
                        for (size_t w = 0; w < m.initWords.size() && drm.sections[vp->section].data.size() - vp->offset >= 8 + 4 * w; ++w)
                            m.initWords[w] = views[vp->section].at<uint32_t>(uint32_t(vp->offset + 4 + 4 * w));
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
    // One-sided (data +0x44 != 0): tru.exe tests the segment as it is against the quad's triangles (v0 (-x, -z), v1 (x, -z),
    // v2 (x, z); 0x461640 culls by -(dir . (v1 - v0) x (v2 - v0)) > 0, the normal being local -Y), so only a move toward
    // local +Y counts [O decomp 0x5CCC20; data: l3_bedroom_hall 4375 faces the hallway she comes along, l3_main_hall 4129
    // lies flat and fires as she drops through it, l1_ship_sinking_hall 1074 as she climbs up through it].
    if (s.oneSided ? !(la.y <= 0 && lb.y > 0) : !((la.y > 0) != (lb.y > 0))) return false;
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
    if (o.state == 0 && !o.inside && !o.firstIn && !o.hidden && !o.dead && o.engagement < 0 && o.offset == vec3(0) && fields.empty() && strings.empty() && properties.empty() && !o.spline &&
        (!o.joint || o.joint->position == 0) && !o.loose && o.holds.empty() && (!o.lastAt || *o.lastAt == o.pos + o.offset) &&
        (!o.graph || (o.graph->state == o.graph->initial && o.graph->transition < 0 && o.graph->frames == 0 && o.graph->t == 0 &&
                      (o.graph->set->graph || o.graph->clip < 0)))) // a graphless object's single clip (`animated` 1) just started is state
        return nullptr; // birth state
    nlohmann::json out = {{"state", o.state}, {"t", o.t}, {"inside", o.inside}, {"firstIn", o.firstIn},
            {"offset", {o.offset.x, o.offset.y, o.offset.z}}, {"hidden", o.hidden}, {"dead", o.dead}, {"engagement", o.engagement}, {"scriptFields", std::move(fields)},
            {"scriptStrings", std::move(strings)}, {"scriptProperties", std::move(properties)}};
    if (const auto& s = o.spline)
        out["spline"] = {s->t, s->dir, s->mode, s->playing, s->toKey, s->clip, s->target, s->from, s->to, s->count};
    if (o.joint && o.joint->position != 0) out["joint"] = o.joint->position;
    if (o.loose) { // its tumbled rotation too (World poses a loose body by offset + body)
        out["loose"] = true;
        out["body"] = nlohmann::json::array();
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) out["body"].push_back(o.body[c][r]);
    }
    if (const auto& g = o.graph) out["graph"] = {g->state, g->transition, g->frames, g->clip, g->t, g->loop};
    if (!o.holds.empty()) out["holds"] = o.holds;
    if (o.lastAt && *o.lastAt != o.pos + o.offset) out["lastAt"] = {o.lastAt->x, o.lastAt->y, o.lastAt->z};
    return out;
}
void applyState(Object& o, const nlohmann::json* s) {
    o.state = 0, o.t = 0, o.inside = o.firstIn = false, o.offset = vec3(0);
    o.hidden = o.dead = false, o.engagement = -1, o.loose = false;
    o.holds.clear(), o.lastAt.reset(); // a missing lastAt is the current point (Runtime::tickTrackedInstances)
    o.machine.reset();
    o.fibers.clear();
    if (o.script) {
        o.script->fields = o.script->initial;
        o.script->strings = o.script->initialStrings;
        o.script->properties = o.script->initialProperties;
    }
    if (const float p = s && s->is_object() ? s->value("joint", 0.0f) : 0.0f; o.joint || p != 0) {
        auto& j = o.joint ? *o.joint : o.joint.emplace(Object::Joint{});
        j.position = p, j.speed = 0; // a saved object's watches and motor: its setup (resume)
        for (auto& r : j.ranges) r.inside = true;
    }
    if (auto& g = o.graph) { // the intro's state (Runtime::addUnit), or the saved one
        g->state = g->initial, g->transition = -1, g->frames = 0, g->clip = -1, g->t = 0, g->loop = false;
        const auto* G = g->set->graph ? &*g->set->graph : nullptr;
        // (An object without a graph plays single clips, `animated` 1: its clip and time are saved the same way.)
        if (const auto a = s && s->is_object() ? s->find("graph") : nlohmann::json::const_iterator{}; s && s->is_object() && a != s->end() &&
            a->is_array() && a->size() == 6) {
            const int state = (*a)[0], transition = (*a)[1], clip = (*a)[3];
            if (G && state >= 0 && size_t(state) < G->states.size()) g->state = state;
            if (G && transition >= 0 && size_t(transition) < G->transitions.size()) g->transition = transition;
            g->frames = (*a)[2], g->t = (*a)[4], g->loop = (*a)[5];
            if (clip >= 0 && size_t(clip) < g->set->clips.size()) g->clip = clip;
        } else if (G && G->states[size_t(g->state)].anim >= 0) {
            g->clip = G->states[size_t(g->state)].anim, g->loop = G->states[size_t(g->state)].flags & 2;
        }
    }
    if (!s || !s->is_object()) return;
    o.state = s->value("state", 0), o.t = s->value("t", 0.0);
    o.inside = s->value("inside", false), o.firstIn = s->value("firstIn", false);
    o.hidden = s->value("hidden", false), o.dead = s->value("dead", false), o.engagement = s->value("engagement", -1), o.loose = s->value("loose", false);
    const auto off = s->value("offset", std::vector<float>{0, 0, 0});
    if (off.size() == 3) o.offset = {off[0], off[1], off[2]};
    o.holds = s->value("holds", std::set<int32_t>{});
    if (const auto at = s->value("lastAt", std::vector<float>{}); at.size() == 3) o.lastAt = vec3(at[0], at[1], at[2]);
    if (const auto body = s->value("body", std::vector<float>{}); o.loose && body.size() == 16)
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) o.body[c][r] = body[size_t(c * 4 + r)];
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
// The offset of the first `modifiable` member in the instance's type chain (its anim component).
std::optional<uint16_t> modifiableMember(const vm::Instance& in) {
    if (!in.program) return std::nullopt;
    for (const ScriptType* t = in.program->type(in.typeId); t; t = in.program->type(t->superId))
        for (const Member& m : t->members)
            if (!std::strcmp(in.program->nativeName(m.typeId), "modifiable")) return m.offset;
    return std::nullopt;
}
} // namespace

void Runtime::setBigfile(const cdc::BigfileSet* bigfile) {
    bigfile_ = bigfile;
    playerScript_.reset();
    playerHiddenGroups_ = playerGearGroups_ = 0;
    playerObjects_.clear();
    pullStrength_ = 1;
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
    residentOnly_.clear();
    for (const auto& u : units)
        if (u.resident && std::none_of(units.begin(), units.end(), [&](const UnitRef& v) { return !v.resident && v.name == u.name; }))
            residentOnly_.insert(u.name);
    if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return !u.resident && u.name == carriedUnit_; }))
        for (const int32_t uid : withLara_) // with Lara: into her unit rather than out with the one they were in
            if (Object* o = findMut(uid); o && o->unit != carriedUnit_) rehome(*o, carriedUnit_);
    std::erase_if(objects_, [&](const Object& o) {
        if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == o.unit; })) return false;
        if (auto s = stateOf(o); !s.is_null()) stash_[std::to_string(o.uid)] = std::move(s); // kept for streaming back
        if (const auto a = away_.find(o.uid); a != away_.end()) { // where it lay in the frame of the unit it leaves with
            const auto home = unitRefs_.find(o.unit);
            a->second.at = o.pos + o.offset - (home == unitRefs_.end() ? vec3(0) : home->second.offset);
        }
        return true;
    });
    unitRefs_.clear();
    for (const auto& u : units)
        if (!u.resident) unitRefs_.insert_or_assign(u.name, u);
    for (auto it = unitScripts_.begin(); it != unitScripts_.end();) { // streamed out: the unit script ends with its unit
        if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == it->first; })) { ++it; continue; }
        if (const auto a = auxiliary_.find(it->second); a != auxiliary_.end()) {
            std::erase_if(threads_, [&](const auto& t) { return t.first == a->second->uid; });
            auxiliary_.erase(a);
        }
        it = unitScripts_.erase(it);
    }
    std::erase_if(units_, [&](const std::string& n) { return std::none_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == n; }); });
    std::erase_if(dormant_, [&](const auto& kv) { return !units_.contains(kv.second.unit.name); });
    for (const auto& u : units)
        if (!units_.contains(u.name) &&
            std::none_of(objects_.begin(), objects_.end(), [&](const Object& o) { return o.unit == u.name && !away_.contains(o.uid); }))
            addUnit(u); // (an instance that came there with Lara is not the unit's own)
    introduceBorn(true);
    for (const auto& [uid, a] : std::map<int32_t, Away>(away_))
        if (!find(uid)) materialize(uid, a);
    applySavedOrder(); // right after a load: the objects as the saved world had them
    attachSavedThreads();
    for (const auto& u : units)
        if (!u.resident && deferredUnits_.erase(u.name)) startUnitScript(u.name);
}

void Runtime::travel(int32_t uid, const std::string& unit) {
    Object* o = findMut(uid);
    if (o && o->unit != unit && unitRefs_.contains(unit)) rehome(*o, unit);
}

void Runtime::rehome(Object& o, const std::string& unit) {
    const auto a = away_.find(o.uid);
    const std::string birth = a == away_.end() ? o.unit : a->second.birth;
    if (birth == unit) {
        if (a != away_.end()) away_.erase(a); // home again
    } else {
        away_[o.uid] = {birth, unit, vec3(0)};
    }
    o.unit = unit;
}

void Runtime::setCarried(std::vector<int32_t> uids, const std::string& unit) {
    withLara_ = std::move(uids), carriedUnit_ = unit;
    for (const int32_t uid : withLara_) travel(uid, unit);
}

std::shared_ptr<const cdc::Drm> Runtime::birthDrm(const std::string& unit) {
    if (const auto u = unitRefs_.find(unit); u != unitRefs_.end() && u->second.drm) return u->second.drm;
    if (awayDrm_.first == unit && awayDrm_.second) return awayDrm_.second;
    auto bytes = bigfile_ ? bigfile_->read("pc-w\\" + unit + ".drm") : std::nullopt;
    if (!bytes) return nullptr;
    awayDrm_ = {unit, std::make_shared<const cdc::Drm>(cdc::parseDrm(std::move(*bytes)))};
    return awayDrm_.second;
}

std::vector<int32_t> Runtime::summon(const std::string& object, const std::string& birth, const std::string& unit) {
    std::vector<int32_t> out;
    if (!bigfile_ || unitRefs_.contains(birth) || !unitRefs_.contains(unit)) return out;
    try {
        const auto drm = birthDrm(birth);
        if (!drm) return out;
        if (objectNames_.empty()) {
            const auto list = bigfile_->read("\\tr8\\pc-w\\objectlist.txt");
            if (!list) return out;
            objectNames_ = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        }
        for (const auto& in : formats::parseUnit(*drm).intros) {
            const auto n = objectNames_.find(in.objectId);
            if (n == objectNames_.end() || n->second != object || find(in.uniqueId) || away_.contains(in.uniqueId)) continue;
            const Away a{birth, unit, vec3(in.position[0], in.position[1], in.position[2])};
            away_[in.uniqueId] = a;
            if (materialize(in.uniqueId, a)) out.push_back(in.uniqueId);
            else away_.erase(in.uniqueId);
        }
    } catch (const FormatError&) {
    }
    return out;
}

void Runtime::setHidden(int32_t uid, bool hidden) {
    if (Object* o = findMut(uid); o && o->hidden != hidden) o->hidden = hidden, ++visibilityVersion_;
}

// The object of an instance that lives in a loaded unit other than its birth unit (it streamed out with that unit, or
// a save holds it there): made from its intro, which is in its birth unit's DRM (the loaded one, else read again), with
// its stashed state, where it lay.
bool Runtime::materialize(int32_t uid, const Away& a) {
    const auto home = unitRefs_.find(a.unit);
    if (home == unitRefs_.end() || !bigfile_) return false;
    UnitRef birth{a.birth, nullptr, vec3(0)};
    std::shared_ptr<vm::Program> program;
    if (const auto b = unitRefs_.find(a.birth); b != unitRefs_.end()) {
        birth.drm = b->second.drm;
        for (const auto& o : objects_)
            if (o.unit == a.birth && o.script && o.script->program && o.script->program->unit == a.birth) {
                program = o.script->program;
                break;
            }
    }
    try {
        if (!birth.drm) birth.drm = birthDrm(a.birth);
        if (!birth.drm) return false;
        if (!program) {
            std::vector<ScriptType> types;
            for (uint32_t i = 0; i < birth.drm->sections.size(); ++i)
                if (birth.drm->sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(*birth.drm, i));
            program = std::make_shared<vm::Program>(std::move(types));
            program->unit = a.birth;
        }
        if (objectNames_.empty()) {
            const auto list = bigfile_->read("\\tr8\\pc-w\\objectlist.txt");
            if (!list) return false;
            objectNames_ = formats::parseObjectList({reinterpret_cast<const char*>(list->data()), list->size()});
        }
        const auto unit = formats::parseUnit(*birth.drm);
        const auto in = std::ranges::find_if(unit.intros, [&](const formats::Intro& i) { return i.uniqueId == uid; });
        if (in == unit.intros.end()) return false;
        materializing_ = true;
        introduce(birth, program, *in, false);
        materializing_ = false;
    } catch (const FormatError&) {
        materializing_ = false;
        return false;
    }
    Object* o = findMut(uid);
    if (!o) return false;
    o->unit = a.unit;
    o->offset = home->second.offset + a.at - o->pos;
    if (!resume(*o)) { // streamed back in during play: its constructor runs, as every object's of that unit
        fire(EventKind::Start, *o);
        if (Object* again = findMut(uid)) again->mainPending = true, fire(EventKind::Main, *again);
    }
    return true;
}

void Runtime::addUnit(const UnitRef& u) {
    if (!bigfile_ || !u.drm) return;
    units_.insert(u.name); // before parsing: a unit that fails is not retried on every call
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
    program->unit = u.name;
    const bool unitResumed = savedAux_.contains({u.name, unit.scriptId});
    { // a load: the unit / global scripts of the save exist before the intros resume (their threads address them)
        std::vector<uint32_t> saved;
        for (const auto& [key, a] : savedAux_)
            if (key.first == u.name && program->type(key.second)) saved.push_back(key.second);
        for (const uint32_t typeId : saved) auxiliaryInstance(program, typeId);
    }

    for (const auto& in : unit.intros) {
        if (formats::dormant(in)) { // not introduced at unit load (0x5C2360); born ones: introduceBorn
            dormant_.insert_or_assign(in.uniqueId, Dormant{u, program, in});
            continue;
        }
        introduce(u, program, in, false);
    }
    std::vector<bool> resumed(objects_.size() - firstNew);
    for (size_t i = firstNew; i < objects_.size(); ++i) resumed[i - firstNew] = resume(objects_[i]);
    for (size_t i = firstNew; i < objects_.size(); ++i) {
        if (resumed[i - firstNew]) continue;
        fire(EventKind::Start, objects_[i]);
        objects_[i].mainPending = true;
        fire(EventKind::Main, objects_[i]);
    }
    // The unit script (Level+0x70 DTP +0x1C0, an NsUnit type; LEVELS.md "Unit script") is created with its unit and
    // enters state 0 as its own thread (e.g. l3_main_bedroom: Run, then the hint_01 help condition via vt 28).
    // unitref.Get returns the same instance. [I: started after the unit's intros so its instancerefs resolve]
    // A unit script without a startup function still listens (l1_cine_ship_sinking 5997: only vt 11 / 13).
    // A load resumes the saved one instead (its threads: attachSavedThreads). A resident unit's (UnitRef::resident) startup
    // waits until the stream loads the unit (setUnits): tru.exe runs it only then, and l3_mansion_exterior_afterburn's
    // (11388: globaleffect 11 Rain 20) set every level's rain at the game start.
    if (program->type(unit.scriptId)) {
        const auto self = auxiliaryInstance(program, unit.scriptId);
        unitScripts_[u.name] = {program.get(), unit.scriptId};
        if (!unitResumed) {
            if (u.resident) deferredUnits_.insert(u.name);
            else startUnitScript(u.name);
            notifyPlayerUnit(self->uid); // 0x4B0050: a new listener gets the player's current unit(s)
        }
    }
    attachSavedThreads();
}

void Runtime::introduce(const UnitRef& u, const std::shared_ptr<vm::Program>& program, const formats::Intro& in, bool start) {
    const auto n = objectNames_.find(in.objectId);
    if (n == objectNames_.end()) return;
    // An instance that left this unit is alive elsewhere, or saved with the unit it is in (tru.exe's Birth wants it
    // "not alive", 0x5B3950): its birth unit does not introduce it again.
    if (!materializing_ && away_.contains(in.uniqueId)) return;
    auto kind = classify(n->second);
    // The native base decides plane vs volume, not the object name: l3_main_hall "triggervolume" intros 4128 / 4129
    // run triggerplane scripts (3529 -> triggerplanebase) whose vt 54 is the crossing handler [O bytecode].
    if (kind == Kind::Volume || kind == Kind::Plane)
        for (const ScriptType* t = in.scriptId ? program->type(in.scriptId) : nullptr; t; t = program->type(t->superId))
            if (t->nativeName == "triggerplanebase" || t->nativeName == "triggervolumebase") {
                kind = t->nativeName == "triggerplanebase" ? Kind::Plane : Kind::Volume;
                break;
            }
    // Scriptless receptacles stay (l4_temple pole holes 5230-5232: a pole inserted there is a swing pole) [I: "_pt_"
    // objects only, to spare loading every prop's DRM].
    if (!kind && !in.scriptId && !in.spline && !(contains(n->second, "_pt_") && receptacleGraph(n->second))) return;
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
        if (!in.data) return;
        const auto shape = parseShape(*u.drm, *in.data, o.kind == Kind::Plane);
        if (!shape) return;
        o.shape = *shape;
    }
    if (in.data) { // [O] l2_puzzleroom pole 2967 -> hole 2969, l2_norsehub gems -> their portable_key_preinsert holders
        const auto& d = u.drm->sections.at(in.data->section).data;
        uint32_t w[2] = {};
        if (in.data->offset + 8 <= d.size()) std::memcpy(w, d.data() + in.data->offset, 8);
        if (w[0] == 0x405 && (w[1] & 0xFFFF) == 1) o.placedIn = int32_t(w[1] >> 16);
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
                // [O] Lara's clip for the switch is in its DRM (113-track lara skeleton animations; ids shared between
                // DRMs). [I] choice by name: the lever pull, else the button press, else the rune switches' interact.
                if (o.kind == Kind::Switch)
                    for (const std::string_view want : {"obj_pull_lever", "obj_pushbutton", "hosted_push_button", "interact"})
                        if (const auto* e = pick([&](const anim::AnimEntry& x) { return x.name == want; }); e && !set->laraClip)
                            set->laraClip = e->animId;
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
                    // [O] Engagement list (tru.exe 0x719620: instance query 0x53, 0x8C-byte records, shape test
                    // 0x6F3C60): record +4 -> records, +8 count, at physical record +0x70 on the switches. [I] record
                    // +0x44 = where Lara stands, units in front of the switch (wall switch 135; 139 from her hand arc).
                    const auto recs = rec ? cdc::internalPointer(ob, *rec + 0x74) : std::nullopt;
                    uint32_t count = 0;
                    if (recs && *rec + 0x7C <= ob.data.size()) std::memcpy(&count, ob.data.data() + *rec + 0x78, 4);
                    if (recs && count && *recs + 0x48 <= ob.data.size()) std::memcpy(&set->useDistance, ob.data.data() + *recs + 0x44, 4);
                }
            }
        }
        o.skeleton = set->skeleton;
        o.clips = set->clips;
        o.clipNames = set->names;
        o.resetAfter = set->resetAfter;
        o.laraClip = set->laraClip;
        o.useDistance = set->useDistance;
    }
    // Objects whose scripts reach their anim component (a `modifiable` / `animated` member) run its state graph
    // (docs/formats/ANIMGRAPH.md). [O] The intro's physob data word 15 picks the initial state (record index; <= 0:
    // the first), 0x6B5890 message 0x23 -> 0x6B4440 -> 0x6A5660; its vt 20 is not sent [I: no script yet].
    // [I] Switches keep their own use flow (Object::state) and get no Object::graph, but their scripts' modifiable
    // member answers fn 8 (state id, query 0xE8) from its field: it holds the intro's state id, and each use moves
    // it along the use transition (Runtime::switchUsed). l5_valaskjalf_entryb lever 7188 acts only while switch
    // 7192 is not in its "on" state 0; 7192 starts in state 1.
    if ((o.kind == Kind::Scripted || o.kind == Kind::Switch) && (o.script || receptacleGraph(o.name))) {
        bool uses = false;
        const auto modifiable = o.script ? modifiableMember(*o.script) : std::nullopt;
        for (const ScriptType* t = o.script ? program->type(o.script->typeId) : nullptr; t && !uses; t = program->type(t->superId))
            for (const Member& m : t->members) {
                const char* native = program->nativeName(m.typeId);
                uses |= !std::strcmp(native, "modifiable") || !std::strcmp(native, "animated");
            }
        // [O] 0x6B4B10 gives every instance with graph data an anim component; ours those whose scripts reach it and
        // every receptacle: a graph taking the insert stimulus (0x6C2530 posts it; World::insertHeld), switches too
        // (l4_ballcourt pole lever 6502 polls its modifiable state id), scriptless ones too (l4_temple holes 5230-5232).
        auto set = uses ? graphSet(o.name) : nullptr;
        const auto receptacle = receptacleGraph(o.name);
        if (receptacle && !(set && set->graph)) set = receptacle;
        // An intro without data starts in the graph's own start state (record +0x2C; l3_bedroom_hall floor 4359: `idle`,
        // not its first record `idle_collapsed`); with data, message 0x23 picks the record below.
        int initial = set && set->graph && !in.data ? set->graph->initial : 0;
        if (set && set->graph && in.data) {
            const auto& d = u.drm->sections.at(in.data->section).data;
            int32_t first = 0;
            if (in.data->offset + 0x40 <= d.size()) std::memcpy(&first, d.data() + in.data->offset + 0x3C, 4);
            if (first > 0 && size_t(first) < set->graph->states.size()) initial = first;
        }
        // [I] A receptacle whose start record is the one its insert transition enters (the pole holes and levers, keyholes,
        // l1 eye pieces, portable_key_preinsert) starts empty, in the state that transition leaves; a portable placed in it
        // (World: the portable's intro reference, 0x6B5890 message 0x23 -> 0x6A6DC0) then inserts itself.
        if (receptacle) {
            const auto& G = *set->graph;
            const auto inserts = [&](uint32_t ti) { return (G.transitions[ti].mask & formats::kStimulusInsert) && G.transitions[ti].target == initial; };
            for (size_t si = 0; si < G.states.size(); ++si)
                if (std::ranges::any_of(G.states[si].out, inserts)) { initial = int(si); break; }
        }
        if (set && o.kind == Kind::Switch && !receptacle) {
            if (set->graph && modifiable)
                o.script->fields[*modifiable] = o.script->initial[*modifiable] = uint32_t(int32_t(set->graph->states[size_t(initial)].index));
        } else if (set) {
            auto& g = o.graph.emplace(Object::Graph{});
            g.set = set;
            g.state = g.initial = initial;
            if (set->graph && set->graph->states[size_t(g.state)].anim >= 0) {
                const auto& s = set->graph->states[size_t(g.state)];
                g.clip = s.anim, g.loop = s.flags & 2;
            }
        }
    }
    if (o.kind != Kind::Volume && o.kind != Kind::Plane) o.mover = objectFlags(o.name) & kTriggerMover;
    // [I] Ship hall doors without a script (chapter 6's l6_ship_upper_hallway_a/b copies of the l1 doors): their
    // volumes' door calls ISTYPE-fail on them, so nothing can open them, yet chapter 6 walks through. They stand
    // open, as the l1 doors once opened.
    const bool openDoor = o.kind == Kind::Door && !o.script && o.clipNames &&
                          std::ranges::find(*o.clipNames, "idle_open") != o.clipNames->end();
    const int32_t uid = o.uid;
    addObject(std::move(o), start);
    if (Object* added = openDoor ? findMut(uid) : nullptr; added && added->state == 0) added->state = 2;
}

void Runtime::birth(int32_t uid) {
    if (dormant_.contains(uid) && !born(uid) && std::ranges::find(births_, uid) == births_.end()) births_.push_back(uid);
}

void Runtime::introduceBorn(bool start) {
    for (const int32_t uid : born_)
        if (const auto d = dormant_.find(uid); d != dormant_.end() && !find(uid)) introduce(d->second.unit, d->second.program, d->second.intro, start);
}

void Runtime::startUnitScript(const std::string& unit) {
    const auto it = unitScripts_.find(unit);
    const auto a = it == unitScripts_.end() ? auxiliary_.end() : auxiliary_.find(it->second);
    if (a == auxiliary_.end()) return;
    if (const Function* f = it->second.first->startup(it->second.second)) {
        const auto thread = makeMachine(a->second->uid);
        if (!thread->start(*a->second, *f)) recordFailure(a->second->uid, unit + " unit script", *thread);
        if (thread->active()) threads_.push_back({a->second->uid, thread});
        adopt(thread);
    }
}

bool Runtime::resume(Object& o) {
    if (!savedLive_.erase(o.uid)) return false;
    o.machine.reset(), o.fibers.clear(), o.mainPending = false;
    const auto it = savedMachines_.find(o.uid);
    if (it == savedMachines_.end()) return true;
    const auto machine = [&](const nlohmann::json& m) {
        auto out = makeMachine(o.uid);
        return out->load(m) && out->active() ? out : nullptr;
    };
    try {
        if (!it->second.at(0).is_null()) o.machine = machine(it->second.at(0));
        for (const auto& f : it->second.at(1))
            if (auto m = machine(f)) o.fibers.push_back(std::move(m));
        o.mainPending = it->second.at(2);
        const auto& setup = it->second.at(3);
        if (const auto j = setup.find("joint"); j != setup.end()) {
            auto& joint = o.joint ? *o.joint : o.joint.emplace(Object::Joint{}); // its position: the object state (applyState)
            joint.prismatic = j->at(0), joint.ranges.clear();
            for (const auto& r : j->at(1)) joint.ranges.push_back({r.at(0).get<float>(), r.at(1).get<float>(), r.at(2).get<int32_t>(), r.at(3).get<bool>()});
            joint.speed = j->size() > 2 ? j->at(2).get<float>() : 0.0f;
        }
        o.tracksLara = setup.value("tracksLara", false);
        o.added = setup.value("added", std::vector<int32_t>{});
        if (const auto s = setup.find("splineRanges"); s != setup.end() && o.spline) o.spline->ranges = s->get<std::vector<std::array<int32_t, 3>>>();
    } catch (const nlohmann::json::exception&) { // checked by restoreRuntime; an odd entry resumes idle
    }
    savedMachines_.erase(it);
    return true;
}

void Runtime::attachSavedThreads() {
    std::erase_if(savedThreads_, [&](const auto& t) {
        if (!instance(t.first)) return false; // its unit script is not created yet
        auto m = makeMachine(t.first);
        if (m->load(t.second) && m->active()) threads_.emplace_back(t.first, std::move(m));
        return true;
    });
}

void Runtime::addObject(Object o, bool start) {
    const auto key = std::to_string(o.uid);
    if (stash_.contains(key)) applyState(o, &stash_[key]), stash_.erase(key); // restored / streamed back in
    objects_.push_back(std::move(o));
    if (!start || resume(objects_.back())) return;
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
    if (flushingStickies_) return; // the outer adopt delivers what handlers queue
    flushingStickies_ = true;
    for (size_t i = 0; i < stickyChanges_.size(); ++i) {
        const StickyChange c = stickyChanges_[i];
        vm::Value args[2] = {{}, vm::Value::scalar(c.value)};
        args[0].kind = vm::Value::Kind::Member, args[0].uid = c.uid, args[0].offset = c.offset;
        stickyNotifying_.insert({c.uid, c.offset});
        run(c.uid, c.vt, args);
        stickyNotifying_.erase({c.uid, c.offset});
    }
    stickyChanges_.clear();
    flushingStickies_ = false;
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

void Runtime::contactDamage(int32_t attacker) {
    if (!playerScript_) return;
    const std::array args{vm::Value::object(attacker), vm::Value::object(playerScript_->uid), vm::Value::scalar(0), vm::Value::scalar(0)};
    callback(attacker, "instance", 34, args);
}

void Runtime::weaponHit(int32_t victim, float damage, int damageClass) {
    if (!playerScript_) return;
    const std::array args{vm::Value::object(playerScript_->uid), vm::Value::object(victim),
                          vm::Value::scalar(std::bit_cast<uint32_t>(damage)), vm::Value::scalar(uint32_t(damageClass))};
    callback(victim, "instance", 35, args);
}

bool Runtime::takesHits(int32_t uid) const {
    const auto self = instance(uid);
    if (!self || !self->program) return false;
    for (const ScriptType* t = self->program->type(self->typeId); t; t = self->program->type(t->superId))
        if (t->nativeName == "instance") {
            if (t->prototypes.size() <= 35 || t->prototypes[35] < 0) return false;
            const Function* f = self->program->method(self->typeId, t->prototypes[35]);
            return f && !f->native && f->wordCount > 1;
        }
    return false;
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
    case EventKind::Use: {
        const SwitchUse use = o.kind == Kind::Switch ? switchUsed(o) : SwitchUse{};
        if (use.stayed) {
            // A use that leaves the switch in its state (the locked state 2 of the one-way switches, transition 2 back
            // into it) is a failed try: the exe starts the transition (vt 19 with its id; the l7_arctic_sea drawbridge
            // wheel 9103's vt 19 (2) posts "reaction=interactFail") and the state is not entered anew. The switch
            // stays free for another try.
            o.state = 0, o.t = 0;
            const vm::Value arg = vm::Value::scalar(uint32_t(use.transition));
            run(o.uid, 19, std::span(&arg, 1));
            return;
        }
        // [I] vt 20 keeps running with a zero argument (the "on" id) whatever state id the use arrived at: ours has no
        // return transitions (the shadow pillars' switches are pulled again and again and each pull must read "on"),
        // so the id the exe would pass (type 3637 vt 20: 0 -> vt 55, 1 -> vt 54) is not passed on yet.
        vt = kVtSwitchUsed;
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
    }
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
    const auto saved = savedAux_.find({program->unit, typeId}); // a load: the saved uid, then the saved state below
    const int32_t uid = saved != savedAux_.end() ? saved->second.at(0).get<int32_t>() : nextAuxiliaryUid_--;
    const auto instance = slot = program->instantiate(uid, typeId);
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
    if (const auto again = savedAux_.find({program->unit, typeId}); again != savedAux_.end()) {
        const auto& a = again->second;
        instance->fields = a.at(1).get<std::map<uint32_t, uint32_t>>();
        instance->strings = a.at(2).get<std::map<uint32_t, std::string>>();
        instance->properties = a.at(3).get<std::map<uint64_t, uint32_t>>();
        savedAux_.erase(again);
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
            // Tutorial popups (takePopups): uilocalstrproperty 1 Get (0x7A1D70) yields the property's locals.bin id (the
            // exe's string object; ours keeps the id), uipopupmgr 0 Show(text, condition, seconds) and 8 Hide.
            if (!std::strcmp(name, "uilocalstrproperty") && nc.function == 1) {
                uint32_t id = 0;
                if (nc.receiver.kind == vm::Value::Kind::Member)
                    if (const auto holder = resolve(nc.receiver.uid))
                        if (const Member* m = holder->program->member(holder->typeId, nc.receiver.offset); m && m->init) id = *m->init;
                nc.result = vm::Value::scalar(id);
                return true;
            }
            if (!std::strcmp(name, "uipopupmgr") && (nc.function == 0 || nc.function == 8)) {
                if (nc.function == 8) popups_.push_back({});
                else if (nc.args.size() == 3) popups_.push_back({int(nc.args[0].bits), std::bit_cast<float>(nc.args[2].bits)});
                return true;
            }
            // Natives with no per-object state (tru.exe thunks that ignore the receiver value).
            if (!std::strcmp(name, "globaleffect") && (nc.function == 5 || nc.function == 6) && nc.args.size() == 1) {
                // FadeOut / FadeIn over N frames (0x4A4EB0 / 0x4A4EF0: GAMELOOP_SetScreenWipe(10, 100 | 0, max(N, 1))).
                fadeTarget_ = nc.function == 5 ? 1.0f : 0.0f;
                fadeStep_ = 1.0f / float(std::max<int32_t>(int32_t(nc.args[0].bits), 1));
                return true;
            }
            // globaleffect 8 Snow(%) (0x4A4F90 -> 0x52E130) and 11 Rain(%) (0x4A4FC0 -> 0x53AB30): the weather amounts
            // (one clears the other); 12 Lightning(min s, max s) (0x4A4FD0 -> 0x52E080). fx::Precipitation runs them.
            if (!std::strcmp(name, "globaleffect") && (nc.function == 8 || nc.function == 11) && nc.args.size() == 1) {
                const int32_t p = int32_t(nc.args[0].bits);
                (nc.function == 8 ? weather_.snow : weather_.rain) = p;
                if (p) (nc.function == 8 ? weather_.rain : weather_.snow) = 0;
                return true;
            }
            if (!std::strcmp(name, "globaleffect") && nc.function == 12 && nc.args.size() == 2) {
                weather_.lightningMin = int32_t(nc.args[0].bits), weather_.lightningMax = int32_t(nc.args[1].bits);
                return true;
            }
            if (!std::strcmp(name, "larabase") && (nc.function == 58 || nc.function == 59 || nc.function == 60) && nc.args.empty()) {
                // INSTANCE_Post(player, 0x1D4 | 0x1D5 | 0x1D6, 0) (0x778000 / 0x778020 / 0x778040); the messages' meaning
                // is [U]. 0x1D4 comes from l5_valgrind's steam columns (fx_steam_val_column vt 14) just before they take
                // 250 health: [I] the scald that makes her let go; ours only records it.
                events_.push_back({EventKind::PlayerMessage, source, int32_t(0x1D4 + nc.function - 58), tick_});
                return true;
            }
            // larabase 52 (thunk 0x79A4B0 -> 0x777DD0): INSTANCE_Post 0x85 to the bike in the instanceref argument and to
            // Lara, who is on it at once (the chapter-start intros' event-1 handlers: l4 5484 -> 5482, l5 7530 -> 7229).
            if (!std::strcmp(name, "larabase") && nc.function == 52 && nc.args.size() == 1) {
                const vm::Value& a = nc.args[0];
                int32_t bike = a.kind == vm::Value::Kind::Object ? a.uid : int32_t(a.bits);
                if (a.kind == vm::Value::Kind::Member)
                    if (const auto holder = resolve(a.uid)) bike = int32_t(holder->fields[a.offset]);
                if (bike) mounts_.push_back(bike), lastBike_ = bike;
                return true;
            }
            // larabase 53 (0x79A4D0 -> 0x777E10): post 0x85 again to the bike 52 last named (Lara +0xF00), if any:
            // l4_ballcourt's 190_mex end handler (6389 vt 58) puts her back on bike 5482 for the ride out.
            if (!std::strcmp(name, "larabase") && nc.function == 53 && nc.args.empty()) {
                if (lastBike_) mounts_.push_back(lastBike_);
                return true;
            }
            // larabase 54 (0x79A4E0 -> 0x777E40): true when the instance in the instanceref argument is her vehicle
            // (Lara +0xF00, non-null): the bike 52 last named here, else the one the world says she rides (the world
            // binding below; one binding answered alone before, so l4_trailstart's reveal 6677 never set her down beside
            // bike 5482). l5_valgrind's 240_val_gate (7464 vt 58) remembers with it that she rode in on bike 7229, to put
            // her back on it afterwards.
            if (!std::strcmp(name, "larabase") && nc.function == 54 && nc.args.size() == 1) {
                const vm::Value& a = nc.args[0];
                int32_t bike = a.kind == vm::Value::Kind::Object ? a.uid : int32_t(a.bits);
                if (a.kind == vm::Value::Kind::Member)
                    if (const auto holder = resolve(a.uid)) bike = int32_t(holder->fields[a.offset]);
                if (!water_ || (bike != 0 && bike == lastBike_)) {
                    nc.result = vm::Value::scalar(bike != 0 && bike == lastBike_);
                    return true;
                }
            }
            // larabase 19 / 20 (0x79A0E0 / 0x79A100 -> 0x777A30 / 0x777A40 -> 0x698850 / 0x698860): set / get the global
            // byte 0xECFC88, "gauntlet charged" (l5_valgrind volume 7511: charged outside the gate's area, not charged
            // inside it until the gate is open). 0x6A6530 passes it (1 / 0) in the instance's shader constant 6 (the
            // gear's glow); the player save holds it (0x6F5910 +0xD4). Ours keeps and saves it (Runtime::gauntletCharged);
            // nothing draws by it yet.
            if (!std::strcmp(name, "larabase") && nc.function == 19 && nc.args.size() == 1) {
                gauntletCharged_ = (nc.args[0].bits & 0xFF) != 0;
                return true;
            }
            if (!std::strcmp(name, "larabase") && nc.function == 20 && nc.args.empty()) {
                nc.result = vm::Value::scalar(gauntletCharged_);
                return true;
            }
            // gamebase 50 (0x7B2BB0 -> 0x779C60): the camera data named by its first argument (a DTP id whose record is
            // of type 4, 0x54DF80) becomes the data of CameraManager's type-4 camera (vt 0x3C, +0xD0; 0x5E1180), which
            // the towers' vt 49 then switches to (gamebase 53 (4)) while Lara pushes them. Ours hands the id to the
            // camera (op 3); the type-4 camera itself is not modelled, so the view stays the gameplay camera's.
            if (!std::strcmp(name, "gamebase") && nc.function == 50 && !nc.args.empty()) {
                const vm::Value& a = nc.args[0];
                uint32_t id = a.bits;
                if (a.kind == vm::Value::Kind::Member)
                    if (const auto holder = resolve(a.uid)) id = holder->fields[a.offset];
                if (camera_ && id != 0xFFFFFFFFu) camera_(3, {}, id, 0);
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
            // cinematic 9 / 10 (thunks 0x797020 / 0x797040 -> 0x4A4C70 / 0x4A4CC0): a latent NtCineSequence on the
            // receiver's stream; its update 0x4ADAA0 first waits until that stream is the one playing, then until the
            // player's time passes the float given (9) or FLT_MAX (10, constant 0x845884): until it has finished [O].
            // Ours plays (or skips) a cutscene at the end of the tick that activated it (World::checkLevelEnd), so
            // the call blocks through the tick of an activation and returns from the next on [I: a time inside the
            // stream (9) is not told apart from its end]; a receiver not activated this tick does not block (ours
            // has nothing later to wait for: l4_trailstart's reveal 6677 calls it on a stream that is never ended
            // here, and waiting stopped it before it set her down). l2_maproom volume 2636 waits so for cinematic
            // 2624 before it hands the left gauntlet over.
            if (!std::strcmp(name, "cinematic") && ((nc.function == 9 && nc.args.size() == 1) || (nc.function == 10 && nc.args.empty())) &&
                (nc.receiver.kind == vm::Value::Kind::Member || nc.receiver.kind == vm::Value::Kind::Object)) {
                const int32_t uid = nc.receiver.uid;
                nc.wait = std::any_of(events_.begin(), events_.end(), [&](const Event& e) {
                    return e.kind == EventKind::Activate && e.target == uid && e.tick == tick_;
                });
                return true;
            }
            // microphone (bind 0x7AC8A0): the audio listener's placement between camera and Lara (1/2 two floats ->
            // 0x5852F0 / 0x585320, 3-5 one float, 6 attach to an instance, 7 detach -> 0x585350). Ours: the listener
            // stays on the camera [U: the blend].
            if (!std::strcmp(name, "microphone") && nc.function >= 1 && nc.function <= 7) return true;
            // Receiver-independent bindings whose side effects other engine systems own (sound: below, once bound).
            // reverb 1 (0x4AA750 -> 0x582B70) copies the 13 SFX-reverb settings its value points to into the audio
            // environment (globalsoundinfo +0x2C; AUDIO.md §9.15). The value is a member of a unit's globals script
            // (soundvolumes pass e.g. globals +16 [O]); its initial value lives in that unit's DRM. A null receiver is skipped.
            if (!std::strcmp(name, "reverb") && nc.function == 1 && reverb_ && nc.receiver.kind == vm::Value::Kind::Member)
                if (const auto owner = resolve(nc.receiver.uid); owner && owner->program)
                    if (const Member* m = owner->program->member(owner->typeId, nc.receiver.offset); m && m->initAt) {
                        const auto o = std::find_if(objects_.begin(), objects_.end(),
                                                    [&](const Object& x) { return x.script && x.script->program == owner->program; });
                        if (o != objects_.end()) audio_.reverbUnit = o->unit, audio_.reverbAt = *m->initAt, reverb_(o->unit, *m->initAt);
                        nc.result = vm::Value::scalar(0);
                        return true;
                    }
            // cinematic (bind 0x7AC050): 0 Create (0x4ABD30 -> 0x4A4D10) and 2 IsCreated (0x4ABD40) the member's stream,
            // which the cinematic base's preload state (3523 fn 13) polls; 1 Play(create) (0x796F90 -> 0x4ADB50: plays the
            // stream, creating it first when asked; 1) from the play state 3523 fn 12 after its fade. Ours loads a cutscene
            // when it plays and plays it at the vt 57 activation (World::checkLevelEnd), so it is always ready [I].
            if (!std::strcmp(name, "cinematic") && (nc.function == 0 || nc.function == 2 || (nc.function == 1 && nc.args.size() == 1))) {
                nc.result = vm::Value::scalar(1);
                return true;
            }
            // unlockable (bind 0x7AD470): 1 IsUnlocked (0x5DB8A0), 2 Unlock (0x5D6600) the extras item; ours has no extras.
            for (const char* passive : {"soundgroup", "corebase", "uipopupmgr", "uilocalstrproperty", "globaleffect", "reverb",
                                        "unlockable"})
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
            // uberobjectbase 3 (0x79D0D0 -> 0x4A4490): AttachTo(anchor): the receiver's uber instance (0x4D12D0) goes to the
            // anchor's vtable slot 6 (ModelMarkerAnchor 0x4D3180 / cdc 0x5BE250 -> INSTANCE link 0x5B4DE0 at the anchor's
            // marker); a null anchor does nothing. l1 ship props and l7's endgame gun 8401 (a section onto marker 33) run it
            // in their constructors. Ours draws an uber object as one model, its sections in place [I: no visible change].
            if (!std::strcmp(name, "uberobjectbase") && nc.function == 3 && nc.args.size() == 2) return true; // the anchor value: two words
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
                if (nc.function == 3 && nc.args.empty()) { birth(int32_t(localValue)); return true; } // Birth (0x4AC0D0)
                if (nc.function == 6 && nc.args.empty()) { nc.wait = !target; return true; }
            }
            // instanceref 0 (0x79A940 -> 0x4A52E0, as unitref 0): receiver and argument name the same instance; either may
            // be a member, a local's value or an object (instance 14 GetInstanceRef results; l1_seafloor 978, l7_arctic_sea
            // 9280 compare the entering instance with their refs).
            if (!std::strcmp(name, "instanceref") && nc.function == 0 && nc.args.size() == 1) {
                const auto ref = [&](const vm::Value& v) -> uint32_t {
                    if (v.kind == vm::Value::Kind::Object) return uint32_t(v.uid);
                    if (v.kind != vm::Value::Kind::Member) return v.bits;
                    const auto holder = resolve(v.uid);
                    if (!holder || !holder->program) return 0;
                    if (const auto f = holder->fields.find(v.offset); f != holder->fields.end()) return f->second;
                    const Member* m = holder->program->member(holder->typeId, v.offset);
                    return m && m->init ? *m->init : 0;
                };
                const uint32_t a = refObject ? localValue : ref(nc.receiver), b = ref(nc.args[0]);
                nc.result = vm::Value::scalar(a != 0 && a == b);
                return true;
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
                MusicRequest r{self ? self->uid : 0, nc.function, {}, -1, nc.args.empty() ? 0.0f : std::bit_cast<float>(nc.args[0].bits),
                               nc.function == 2 && nc.args.size() > 1 ? std::bit_cast<float>(nc.args[1].bits) : 0.0f};
                if (self && self->program && nc.receiver.kind == vm::Value::Kind::Member) {
                    if (const Member* m = self->program->member(self->typeId, nc.receiver.offset)) {
                        if (nc.function != 3) r.track = m->initText;
                        if (m->init2) r.slot = int32_t(*m->init2);
                    }
                } else if (nc.receiver.kind == vm::Value::Kind::Scalar && nc.receiver.text) { // a music local (Machine::call)
                    if (nc.function != 3) r.track = *nc.receiver.text;
                    r.slot = int32_t(nc.receiver.bits);
                }
                if (nc.function == 3 || !r.track.empty()) audio_.apply(r), musicRequests_.push_back(std::move(r));
                nc.result = vm::Value::scalar(0);
                return true;
            }
            // damagetype 0 (bind 0x7AC4F0: 0x796FD0 -> 0x4AA4D0): equality of the two 4-byte values, the damage class of
            // a hit (instance prototype 35's last argument) against a script's constant.
            if (!std::strcmp(name, "damagetype") && nc.function == 0 && nc.args.size() == 1) {
                const auto value = [&](const vm::Value& v) -> uint32_t {
                    if (v.kind != vm::Value::Kind::Member) return v.bits;
                    const auto holder = resolve(v.uid);
                    if (!holder) return 0;
                    if (const auto f = holder->fields.find(v.offset); f != holder->fields.end()) return f->second;
                    const Member* m = holder->program ? holder->program->member(holder->typeId, v.offset) : nullptr;
                    return m && m->init ? *m->init : 0;
                };
                nc.result = vm::Value::scalar(value(nc.receiver) == value(nc.args[0]));
                return true;
            }
            if ((!self || !self->program) && !std::strcmp(name, "animated")) { nc.result = vm::Value::scalar(0); return true; }
            if (!self || !self->program) return false;
            const auto program = self->program;
            const uint32_t off = nc.receiver.offset;
            auto field = [&]() -> uint32_t& { return self->fields[off]; };
            // modifiable (bind 0x7AC970) / animated on an object with an anim graph: the member is its owner's anim
            // component (Instance +0x294; docs/formats/ANIMGRAPH.md). 0 same component (0x4A52E0), 1 post 0x30 (two
            // floats, not modelled), 2 the state's flag 0x40 (query 0xED), 3 stimulus(stimulus, value) (0x4AC390:
            // message 0x33, value < 0 = any), 4 start transition by id (0x32), 5 / 6 running transition id or -1 (0xE9),
            // 7 enter state by id (0x2E), 8 state id (0xE8). animated 1(animation {object, list index}) plays the clip.
            if (Object* owner = (!std::strcmp(name, "modifiable") || !std::strcmp(name, "animated")) ? findMut(self->uid) : nullptr;
                owner && owner->graph && (owner->graph->set->graph || name[0] == 'a')) {
                auto& g = *owner->graph;
                const auto memberValue = [&](const vm::Value& v, bool second) -> std::optional<uint32_t> {
                    if (v.kind != vm::Value::Kind::Member) return v.bits;
                    const auto holder = resolve(v.uid);
                    if (!holder || !holder->program) return std::nullopt;
                    if (const auto f = holder->fields.find(v.offset + (second ? 4 : 0)); f != holder->fields.end()) return f->second;
                    const Member* m = holder->program->member(holder->typeId, v.offset);
                    return m ? (second ? m->init2 : m->init) : std::nullopt;
                };
                if (name[0] == 'a') {
                    if (nc.function != 1 || nc.args.size() != 1) return false;
                    const auto index = memberValue(nc.args[0], true);
                    if (index && *index < g.set->clips.size()) g.clip = int(*index), g.t = 0, g.loop = false;
                    return true;
                }
                const auto& G = *g.set->graph;
                switch (nc.function) {
                case 0:
                    if (nc.args.size() != 1) return false;
                    nc.result = vm::Value::scalar(nc.args[0].kind == vm::Value::Kind::Member && nc.args[0].uid == self->uid);
                    return true;
                case 1: return true;
                case 2: nc.result = vm::Value::scalar((G.states[size_t(g.state)].flags & 0x40) != 0); return true;
                case 3: {
                    if (nc.args.size() != 2) return false;
                    const auto mask = memberValue(nc.args[0], false);
                    if (!mask) return false;
                    const int32_t value = int32_t(nc.args[1].bits);
                    graphStimulus(owner->uid, *mask, value < 0 ? -1.0f : float(value));
                    return true;
                }
                case 4: case 7: {
                    if (nc.args.size() != 1) return false;
                    const int i = nc.function == 4 ? G.transitionById(int32_t(nc.args[0].bits)) : G.stateById(int32_t(nc.args[0].bits));
                    if (i >= 0) nc.function == 4 ? graphBegin(owner->uid, i) : graphEnter(owner->uid, i);
                    return true;
                }
                case 5: case 6:
                    nc.result = vm::Value::scalar(uint32_t(g.transition >= 0 ? int32_t(G.transitions[size_t(g.transition)].index) : -1));
                    return true;
                case 8: { // [I] a running transition reports its target (the exe: the state it left, query 0xE8 = comp
                    // +4); l3_chapel's plate lights fire 4818 and checks it in the same handler (vt 55 -> pillars vt 60)
                    const int t = g.transition >= 0 ? G.transitions[size_t(g.transition)].target : -1;
                    nc.result = vm::Value::scalar(uint32_t(int32_t(G.states[size_t(t >= 0 ? t : g.state)].index)));
                    return true;
                }
                default: return false;
                }
            }
            // animated 1 on an object without clips (no skeleton) plays nothing.
            if (!std::strcmp(name, "animated")) { nc.result = vm::Value::scalar(0); return true; }
            if (!std::strcmp(name, "clock") && nc.receiver.kind == vm::Value::Kind::Member) {
                // tru.exe 0x4A3450..0x4A34F0, update 0x4A3960. A clock's alarm (+0x14) starts as its member's second
                // init value [I: the adrenaline trackers' time-out clock {0, 6.0} is started without a SetAlarm and
                // must not fire at once; the first value would be the repeat flag +0x19, 0 in every clock seen].
                const uint64_t clockKey = (uint64_t(uint32_t(self->uid)) << 32) | off;
                if (!clocks_.contains(clockKey))
                    if (const Member* m = program->member(self->typeId, off); m && m->init2) clocks_[clockKey].alarm = std::bit_cast<float>(uint32_t(*m->init2));
                Clock& c = clocks_[clockKey];
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
                // Add (0x4A43E0) / Remove (0x4A4410) an instance the volume tests: Lara (Object::tracksLara) or another
                // instance (Object::added, tickTrackedInstances; l4_ballcourt 6519 adds push block 6504). Null: nothing.
                const vm::Value& a = nc.args[0];
                int32_t uid = a.kind == vm::Value::Kind::Object ? a.uid : int32_t(a.bits);
                if (a.kind == vm::Value::Kind::Member) {
                    const auto holder = resolve(a.uid);
                    uid = holder ? int32_t(holder->fields[a.offset]) : 0;
                }
                Object* o = uid ? findMut(self->uid) : nullptr;
                if (!o) return true;
                if (playerScript_ && uid == playerScript_->uid) o->tracksLara = nc.function == 0;
                else if (nc.function == 1) std::erase(o->added, uid), o->holds.erase(uid);
                else if (std::ranges::find(o->added, uid) == o->added.end()) o->added.push_back(uid);
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
            // Sticky equality (0x7B3390 -> 0x7B2940) compares identity (owner key, sticky id), not values.
            if ((!std::strcmp(name, "stickybool") || !std::strcmp(name, "stickyint") || !std::strcmp(name, "stickyfloat")) &&
                nc.function == 3 && nc.args.size() == 1) {
                nc.result = vm::Value::scalar(nc.args[0].kind == vm::Value::Kind::Member && nc.args[0].uid == self->uid &&
                                              nc.args[0].offset == off);
                return true;
            }
            if (!std::strcmp(name, "messagechannel") && nc.function == 3 && nc.args.size() == 1) {
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
            // modifiable 4 (message 0x32: start the transition with that id) on a switch, whose graph ours keeps as the
            // state id in its modifiable member (switchUsed): the id moves to the transition's target; when that state
            // can be used again (it leaves by the use stimulus) the switch is free for another pull. The one-way wall
            // switches' "off reaction" (type 3637 vt 58: modifiable 4 (1)) after a jammed try: l1_kraken_room switch 55,
            // reset by gear 36's event 1 [O bytecode]. [I] the reaction's clip is not played.
            if (!std::strcmp(name, "modifiable") && nc.function == 4 && nc.args.size() == 1)
                if (Object* o = findMut(self->uid); o && o->kind == Kind::Switch && !o->graph) {
                    const auto set = graphSet(o->name);
                    if (set && set->graph) {
                        const auto& G = *set->graph;
                        if (const int ti = G.transitionById(int32_t(nc.args[0].bits)); ti >= 0 && G.transitions[size_t(ti)].target >= 0) {
                            const int target = G.transitions[size_t(ti)].target;
                            field() = uint32_t(int32_t(G.states[size_t(target)].index));
                            if (G.pickUse(target, formats::kStimulusUse, 0, graphSeed_) >= 0 && o->state == 2) o->state = 0, o->t = 0;
                        }
                    }
                    return true;
                }
            // modifiable 7 (0x4A5A90) posts message 0x2E with the value to the instance: an anim-graph object (handler
            // 0x6B4440) enters the state with that id at once (state list query 0xE6, 0x6A5660); other receivers [U].
            if (!std::strcmp(name, "modifiable") && nc.function == 7 && nc.args.size() == 1) {
                // A switch keeps its graph state as the id in this member (switchUsed): the unit scripts lock and free
                // their wheels with it (l7_arctic_sea's constructor: 9103 vt 63 -> 7 (2), free again by vt 62 -> 7 (1)
                // once four stones and four swords are counted). A pulled switch whose new state can be used is free.
                // [I] Honoured for the Arctic Sea's wheels only (`ar_ei_switch_oneway`), whose lock and release are decoded and
                // played. Elsewhere scripts put switches into state 2 too (l1_kraken_room switch 30) and nothing in ours frees
                // them again yet: honouring it there stops self-test kraken_kill and the critical gates at the first pull.
                if (Object* o = findMut(self->uid); o && o->kind == Kind::Switch && !o->graph && o->name == "ar_ei_switch_oneway") {
                    if (const auto set = graphSet(o->name); set && set->graph)
                        if (const int state = set->graph->stateById(int32_t(nc.args[0].bits)); state >= 0) {
                            // (its own member, whichever of the chain's `modifiable` members the caller reached it by)
                            self->fields[modifiableMember(*self).value_or(off)] = nc.args[0].bits;
                            if (o->state == 2 && set->graph->pickUse(state, formats::kStimulusUse, 0, graphSeed_) >= 0) o->state = 0, o->t = 0;
                        }
                    return true;
                }
                if (const Object* o = find(self->uid); o && o->graph && o->graph->set->graph)
                    if (const int state = o->graph->set->graph->stateById(int32_t(nc.args[0].bits)); state >= 0) graphEnter(self->uid, state);
                return true;
            }
            // uigamedata 1 (0x60D630) always returns 0; 14 (0x77BBF0) sets a global UI byte no gameplay reads [U].
            if (!std::strcmp(name, "uigamedata") && nc.function == 1 && nc.args.empty()) {
                nc.result = vm::Value::scalar(0);
                return true;
            }
            if (!std::strcmp(name, "uigamedata") && nc.function == 14 && nc.args.size() == 1) return true;
            // padshock (NtPadshock: gamepad rumble) and camerashake -> feedback_ (game/Feedback.hpp); 0 = same member
            // (0x4AE670 compares the instances). Anything unmodelled returns 0 and runs on (l5_valaskjalf_entryb lever
            // 7188's vt 55 continues past them to its sound and WAIT).
            if (!std::strcmp(name, "padshock") || !std::strcmp(name, "camerashake")) {
                const bool shake = name[0] == 'c';
                nc.result = vm::Value::scalar(0);
                if (!shake && nc.function == 0 && nc.args.size() == 1) {
                    nc.result = vm::Value::scalar(nc.args[0].kind == vm::Value::Kind::Member && nc.args[0].uid == self->uid &&
                                                  nc.args[0].offset == off);
                    return true;
                }
                uint32_t result = 0;
                const Member* m = program->member(self->typeId, off);
                const auto it = self->fields.find(off); // an assigned value, else the initial one (no field inserted)
                const uint32_t value = it != self->fields.end() ? it->second : m && m->init ? *m->init : 0;
                if (m && feedback_ && feedback_(shake, (uint64_t(uint32_t(self->uid)) << 32) | off, *m, value, nc.function, nc.args, result))
                    nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "instance") && (nc.function == 0 || nc.function == 1)) return true; // debug print
            if (!std::strcmp(name, "instanceref") && nc.function == 9 && nc.args.size() == 2) { // SetProperty
                self->properties[(uint64_t(off) << 32) | nc.args[0].bits] = nc.args[1].bits & 0xFF;
                return true;
            }
            // instanceref 10 (thunk 0x799B30, between SetProperty 9 and GetProperty 12): one float to the referenced
            // instance, result unread. Seen only with the god-ray strength beside `unit` 15 of the same value
            // (l7_arctic_sea's constructor and vt 13: 0.8 to dispatcher 9089, whose own thread feeds `unit` 15) [U: what the
            // receiver does with it]; ours keeps nothing and runs on, so the constructor reaches its puzzle set-up.
            if (!std::strcmp(name, "instanceref") && nc.function == 10 && nc.args.size() == 1) {
                nc.result = vm::Value::scalar(0);
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
            // Birth (thunk 0x799AB0 -> 0x4AC0D0): introduces the intro if it is in a loaded unit and neither alive nor dead
            // (0x555A10 dead bits, 0x5B3950 alive -> 0x5B38F0 -> 0x5BD080); else it prints "InstanceRef::Birth FAILED" and
            // returns. Not latent. Ours: dormant intros only (every other one is alive while its unit is loaded).
            if (!std::strcmp(name, "instanceref") && nc.function == 3) {
                birth(int32_t(field()));
                return true;
            }
            if (!std::strcmp(name, "instanceref") && nc.function == 6) { // birthed wait (latent, 0x4A53B0)
                nc.wait = !resolve(int32_t(field())); // blocks until the target exists
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
            // Experience-adjustment settings (registration 0x7AEDE0, thunks 0x7A06B0..0x7A07F0 -> 0x77BA80..0x77BB60) [O]:
            // 4 SetTrainingText (+0x3EFB1), 5 set +0x3EFB0, 6 EnemyHP (+0x3EF98), 7 AmmoAdj (+0x3EF9C), 8 PlayerHP
            // (+0x3EFA4), 9 SavingGrabAdj (+0x3EFA8), 10 the fifth (+0x3EFAC); 11..17 read them in that order, 18 the
            // preset (+0x3EF94). Levels above 2 are refused (0x6E75A0..). Unit Run() scripts compare 17 with 2.
            // 0..3 [U] (the menu's own entries; ours is src/ui) read 0.
            if (!std::strcmp(name, "uiexperienceadjustmentmenu")) {
                Experience& e = experience_;
                const uint32_t fn = nc.function, arg = nc.args.empty() ? 0 : nc.args[0].bits;
                uint32_t result = 0;
                if (fn == 4 || fn == 5) (fn == 4 ? e.trainingText : e.metaIcons) = arg != 0, experienceSet_ = true;
                else if (fn >= 6 && fn <= 10 && !nc.args.empty() && arg < 3) e.levels[fn - 6] = int(arg), experienceSet_ = true;
                else if (fn == 11) result = e.trainingText;
                else if (fn == 12) result = e.metaIcons;
                else if (fn >= 13 && fn <= 17) result = uint32_t(e.levels[fn - 13]);
                else if (fn == 18) result = uint32_t(e.preset);
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "checkpointbase") && nc.function == 3 && nc.args.empty()) {
                events_.push_back({EventKind::Checkpoint, self->uid, self->uid, tick_});
                return true;
            }
            // watervolumebase (thunks 0x79E080..0x79E130, bind 0x7B6930): 0 SetFlow(source, sink, strength), 1 SetLevel(depth,
            // speed), 2 SetLevelToMarker(instance, speed), 3 SetFlowStrength(strength), 4 GetDepth. The volume is the
            // receiver (its own script) or the calling object. larabase 3 (0x777780): Lara's water volume (+0x8DC) is the
            // argument and its surface is over her floor (+0x914 < 0); World answers both as type "larabase".
            const auto uidOf = [&](const vm::Value& a) -> int32_t {
                if (a.kind == vm::Value::Kind::Object) return a.uid;
                if (a.kind != vm::Value::Kind::Member) return int32_t(a.bits);
                const auto holder = resolve(a.uid);
                if (!holder) return 0;
                if (const auto f = holder->fields.find(a.offset); f != holder->fields.end()) return int32_t(f->second);
                const Member* m = holder->program ? holder->program->member(holder->typeId, a.offset) : nullptr;
                return m && m->init ? int32_t(*m->init) : 0;
            };
            if (water_ && !std::strcmp(name, "larabase") && (nc.function == 3 || nc.function == 6 || nc.function == 54) && nc.args.size() == 1) {
                const int32_t target = uidOf(nc.args[0]); // 3: a water volume, 6: an instance (contact force), 54: a bike
                uint32_t result = 0;
                if (target && !water_(target, name, nc.function, {}, result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (water_ && !std::strcmp(name, "watervolumebase") && nc.function <= 4) {
                static constexpr uint8_t kInstanceArgs[] = {2 | 1, 0, 1, 0, 0}; // per function: argument bits holding instances
                std::array<int32_t, 3> args{};
                for (size_t i = 0; i < nc.args.size() && i < args.size(); ++i)
                    args[i] = kInstanceArgs[nc.function] >> i & 1 ? uidOf(nc.args[i]) : int32_t(nc.args[i].bits);
                const int32_t volume = nc.receiver.kind == vm::Value::Kind::Object ? nc.receiver.uid : self->uid;
                uint32_t result = 0;
                const size_t n = std::min(nc.args.size(), args.size());
                if (!water_(volume, name, nc.function, std::span<const int32_t>(args.data(), n), result)) return false;
                nc.result = vm::Value::scalar(result);
                return true;
            }
            if (!std::strcmp(name, "playersensor") && nc.receiver.kind == vm::Value::Kind::Member) {
                // bind 0x7ACDD0.. [O]: 0 SetEnabled / 1 IsEnabled (byte +0xF, thunks 0x7ACE40 / 0x7ACE60), 2 attach to an
                // anchor (0x4A3420 -> sensor +0x18: an instance or a uberobjectbase 2 marker anchor), 3 the same sensor.
                // The enable byte starts as the member's init (word 1 byte 3); a sensor that was never attached never
                // fires (0x7799F0 tests +0x18).
                const Member* m = program->member(self->typeId, off);
                const uint64_t key = (uint64_t(uint32_t(self->uid)) << 32) | off;
                const bool initial = !m || !m->init2 || (*m->init2 >> 24 & 0xFF) != 0;
                const auto it = sensors_.find(key);
                switch (nc.function) {
                case 0:
                    if (nc.args.size() != 1) return false;
                    field() = nc.args[0].bits & 0xFF;
                    if (it != sensors_.end()) it->second.enabled = nc.args[0].bits & 0xFF;
                    else if (bool(nc.args[0].bits & 0xFF) != initial) sensors_[key].enabled = nc.args[0].bits & 0xFF;
                    return true;
                case 1:
                    if (!nc.args.empty()) return false;
                    nc.result = vm::Value::scalar(it != sensors_.end() ? it->second.enabled : initial);
                    return true;
                case 2: {
                    if (nc.args.size() != 1) return true; // no anchor argument: nothing to attach
                    Sensor& sn = sensors_.try_emplace(key, Sensor{0, -1, initial, false}).first->second;
                    const vm::Value& a = nc.args[0];
                    sn.anchor = a.kind == vm::Value::Kind::Object ? a.uid : uidOf(a);
                    sn.marker = a.kind == vm::Value::Kind::Object && (a.bits & kAnchorBit) ? int32_t(a.bits & 0xFFFF) : -1;
                    return true;
                }
                case 3:
                    if (nc.args.size() != 1) return false;
                    nc.result = vm::Value::scalar(nc.args[0].kind == vm::Value::Kind::Member && nc.args[0].uid == self->uid &&
                                                  nc.args[0].offset == off);
                    return true;
                default: return false;
                }
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
            // instance's +0x2D8 component via 0x5B5A60 / 0x5B5AB0): no modelled system reads them [U: their effect].
            // 23 (0x4A41D0) calls vt +0x18 of its native object argument with the instance: given a `uberobjectbase` 2
            // marker anchor (0x4D3110 ModelMarkerAnchor) the instance rides that marker from then on (anchor()); other
            // arguments [U].
            if (!std::strcmp(name, "instance") && nc.function == 23 && nc.args.size() == 1 && nc.args[0].kind == vm::Value::Kind::Object &&
                (nc.args[0].bits & kAnchorBit) && find(nc.args[0].uid))
                anchors_[self->uid] = {nc.args[0].uid, int32_t(nc.args[0].bits & 0xFFFF)};
            if (!std::strcmp(name, "instance") && (nc.function == 18 || nc.function == 21 || nc.function == 23) &&
                nc.args.size() == 1)
                return true;
            // modifiable 1 (two floats, 0x4A59F0) posts message 0x30 {short, short} to the instance like 7 does with 0x2E.
            // A water volume takes it as its damage (0x6AC340 case 0x30: water record +0xBA amount, +0xBC time left;
            // World::waterNative "modifiable"): the eitr of l7_helheim01 8937 and the l7_yggdrasil halls' pit 8742,
            // whose scripts send (6000, 30) once a second. [U] other receivers.
            if (!std::strcmp(name, "modifiable") && nc.function == 1 && nc.args.size() == 2) {
                if (const Object* volume = find(self->uid); water_ && volume && volume->name == "watervolume") {
                    const std::array<int32_t, 2> args{int32_t(std::lround(std::bit_cast<float>(uint32_t(nc.args[0].bits)))),
                                                      int32_t(std::lround(std::bit_cast<float>(uint32_t(nc.args[1].bits))))};
                    uint32_t result = 0;
                    water_(self->uid, name, 1, args, result);
                }
                return true;
            }
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
            // gamebase 2 / 24 set / ramp (target, seconds) the global time scale 0xE7F1D4, 1 / 23 the second one 0xE7F1A0
            // (0x4A3DD0 / 0x776090, 0x4A3DC0 [I: 1 by its pairing with 2 in every script that restores both] /
            // 0x776030 -> ramp records read by 0x78C4E0); the game loop multiplies timeMult by both and Lara's update
            // divides the second back out. The adrenaline trackers ramp both (situational adrenaline) and set them
            // back to 1; World applies them (CombatState::timeScale / worldScale).
            if (!std::strcmp(name, "gamebase") && (((nc.function == 1 || nc.function == 2) && nc.args.size() == 1) ||
                                                   ((nc.function == 23 || nc.function == 24) && nc.args.size() == 2))) {
                const auto f = [](const vm::Value& v) { return std::bit_cast<float>(v.bits); };
                if (time_) time_(nc.function == 1 || nc.function == 23, f(nc.args[0]), nc.args.size() == 2 ? std::max(0.0f, f(nc.args[1])) : -1.0f);
                return true;
            }
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
            // 30 (0x777CE0) posts 0x146 to the slot-6 object [U: no effect on ours]. 67 (0x778140 -> 0x6ED510) soaks Lara:
            // every wetness region at its maximum, dirt 0; 68 (0x778150 -> 0x6ED460) resets wetness and dirt (they run
            // around a cinematic; docs/GAMEPLAY.md "Wetness").
            if (!std::strcmp(name, "larabase") && (nc.function == 30 || nc.function == 67 || nc.function == 68) && nc.args.empty()) {
                if (nc.function != 30) events_.push_back({EventKind::Wetness, source, nc.function == 67, tick_});
                return true;
            }
            // 31 (0x7975F0 -> 0x776EF0) posts INSTANCE message 0xC (!arg) to Lara; 61 (0x79A590 -> 0x778060) copies a global
            // clock into Lara +0x6DC and zeroes +0x6E0. The endgame warps (340 / 342 end handlers) run them [U: no effect on ours].
            // 35 (0x79A270 -> 0x777DA0 -> 0x6EE920) stores its float in player +0x1240, her pull strength (0x74AC00 /
            // 0x73F390 read it): the calendars' vt 54 and the Thor mechanisms' vt 49 / 50 run it.
            if (!std::strcmp(name, "larabase") && nc.function == 35 && nc.args.size() == 1) { // her pull strength (Script.hpp)
                pullStrength_ = std::bit_cast<float>(nc.args[0].bits);
                return true;
            }
            if (!std::strcmp(name, "larabase") && ((nc.function == 31 && nc.args.size() == 1) || (nc.function == 61 && nc.args.empty())))
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
            // uigamedata 17 / 18 (0x7A0C90 / 0x7A0CB0 -> 0x77BB70 / 0x77BB90): set / get byte 0xFAE8DB; while set, level
            // changes fade to white instead of black (0x5DAB40, GAMELOOP_RequestLevelChangeByName) [U: ours always black].
            if (!std::strcmp(name, "uigamedata") && nc.function == 17 && nc.args.size() == 1) {
                uiFlag_ = (nc.args[0].bits & 0xFF) != 0;
                return true;
            }
            if (!std::strcmp(name, "uigamedata") && nc.function == 18 && nc.args.empty()) {
                nc.result = vm::Value::scalar(uiFlag_);
                return true;
            }
            // uisavemenu 5 (0x7A4620 -> 0x77D880 -> 0x788A70(arg, 0, 1, -1)): copies the save header, stores the byte
            // argument at +0x1F8 and requests the autosave (0x789E40(1)); returns 0. Cinematic bases call it after
            // their cutscene.
            if (!std::strcmp(name, "uisavemenu") && nc.function == 5 && nc.args.size() == 1) {
                events_.push_back({EventKind::Autosave, source, source, tick_});
                nc.result = vm::Value::scalar(0);
                return true;
            }
            // uisavemenu 6 (0x7A4640 -> 0x77D8A0 -> 0x788A70(arg, 0, 1, unit)): the same request with the unitref argument
            // as the save's unit (0x4AA530), when that is set: the endgame cinematics' "end game save" (l7_monastery 8138
            // vt 60) the Treasure Hunt starts from [I]. Ours: an autosave request.
            if (!std::strcmp(name, "uisavemenu") && nc.function == 6 && nc.args.size() == 2) {
                events_.push_back({EventKind::Autosave, source, source, tick_});
                nc.result = vm::Value::scalar(0);
                return true;
            }
            // unlockablemovie 3 (0x79DE20 -> 0x778F50 -> 0x5D6A80): queues Movies\<name>.bik (global 0xE7ECE0) and leaves
            // the game for the frontend movie state (0x77BBC0(5)); the endgame plays "credits" so [O bytecode].
            if (!std::strcmp(name, "unlockablemovie") && nc.function == 3 && nc.args.size() == 1 && nc.args[0].text) {
                movies_.push_back(*nc.args[0].text);
                return true;
            }
            // hudscreenmgr 4 (0x79E3E0 -> 0x77DAF0): HUD manager vtable slot 0x10 [U: ours has no script HUD]; uigamedata 43
            // (0x7A0EF0 -> 0x77C160) is a debug print; uisavemenu 9 (0x7A4680 -> 0x788D10) stores its byte at save header
            // +0x1F9 [U: no field of ours].
            if ((!std::strcmp(name, "hudscreenmgr") && nc.function == 4) || (!std::strcmp(name, "uigamedata") && nc.function == 43) ||
                (!std::strcmp(name, "uisavemenu") && nc.function == 9 && nc.args.size() == 1))
                return true;
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
                if (nc.function == 0 && nc.args.size() == 1) { // Set (a bool keeps one bit); the owner hears of it
                    const bool flag = !std::strcmp(name, "stickybool");
                    field() = flag ? uint32_t((nc.args[0].bits & 0xFF) != 0) : nc.args[0].bits;
                    if (nc.receiver.kind == vm::Value::Kind::Member && !stickyNotifying_.contains({self->uid, off}))
                        stickyChanges_.push_back({self->uid, off, int16_t(flag ? 6 : name[6] == 'i' ? 7 : 5), field()});
                    return true;
                }
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
                // [I] A switch's member holds its graph state id (Runtime::addUnit): the transition's target replaces it.
                if (const Object* sw = findMut(self->uid); sw && sw->kind == Kind::Switch)
                    if (const auto set = graphSet(sw->name); set && set->graph)
                        if (const int ti = set->graph->transitionById(int32_t(value)); ti >= 0 && set->graph->transitions[size_t(ti)].target >= 0)
                            field() = uint32_t(int32_t(set->graph->states[size_t(set->graph->transitions[size_t(ti)].target)].index));
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
                // Lara's (l7_arctic_sea's constructor asks whether she came from the unit before): her stream unit
                // id, what unitref 0 compares; 0 before the World has told us.
                if (!object && self == playerScript_) {
                    nc.result = vm::Value::scalar(playerUnit_ ? playerUnit_->first : 0);
                    return true;
                }
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
                object->dead = true;
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
            // (0x4AB2A0: the target's transform, ours the object) or a `uberobjectbase` 2 marker anchor. l1_ship_sinking_hall
            // plane 1074 moves Lara onto cinematic_object 1005 in l1_cine_ship_sinking, whose unit script then plays it
            // (World::tick); 030_med_yacht's end (997 fn 5) puts her on ms_uo_yacht's stern marker 13.
            if (!std::strcmp(name, "instance") && nc.function == 26 && nc.args.size() == 1 && self == playerScript_ &&
                nc.args[0].kind == vm::Value::Kind::Object && find(nc.args[0].uid)) {
                const uint32_t b = nc.args[0].bits;
                teleport_ = {nc.args[0].uid, b & kAnchorBit ? int32_t(b & 0xFFFF) : -1};
                return true;
            }
            // The same call on another instance is queued for the World (takePlacements), which places what it simulates
            // itself: l5_valgrind's 235_val_shaft end handler (7403 vt 60) stands bike 7229 on marker object 7404 when she
            // rode through the plane [O bytecode]. The receiver's script result stays the generic one below.
            if (!std::strcmp(name, "instance") && nc.function == 26 && nc.args.size() == 1 && self != playerScript_ &&
                nc.args[0].kind == vm::Value::Kind::Object && find(nc.args[0].uid)) {
                const uint32_t b = nc.args[0].bits;
                placements_.push_back({self->uid, nc.args[0].uid, b & kAnchorBit ? int32_t(b & 0xFFFF) : -1});
            }
            // uberobjectbase 2 (0x79D090 -> 0x4AB800): a ModelMarkerAnchor (0x4D3110) on section `args[0]` at HInfo
            // marker id `args[1]` (0x4D4820 -> 0x46DF20); instance 26 / 23 take it. Ours: the object, tagged with the
            // marker id [I: section 0 = the object itself; every caller passes an unset section id].
            if (!std::strcmp(name, "uberobjectbase") && nc.function == 2 && nc.args.size() == 2) {
                nc.result = vm::Value::object(self->uid);
                nc.result.bits = kAnchorBit | (nc.args[1].bits & 0xFFFF);
                return true;
            }
            // instance 8 (thunk 0x798CA0 -> 0x4AB320 -> 0x5BE1A0): a marker anchor on this instance's HInfo marker `args[0]`,
            // as uberobjectbase 2 makes for a section; instance 23 / 26 and playersensor 2 take it (l7_yggdrasil_end:
            // each natla_end hangs its fireball weapon on its marker 13).
            if (!std::strcmp(name, "instance") && nc.function == 8 && nc.args.size() == 1) {
                nc.result = vm::Value::object(self->uid);
                nc.result.bits = kAnchorBit | (nc.args[0].bits & 0xFFFF);
                return true;
            }
            // weaponbase 0 (setWeaponNative): Fire at an instance; a null target is skipped (thunk 0x79E2E0).
            if (!std::strcmp(name, "weaponbase") && nc.function == 0 && nc.args.size() == 1 && weapon_) {
                const vm::Value& t = nc.args[0];
                if (t.kind != vm::Value::Kind::Object || !t.uid) return true;
                weapon_(self->uid, playerScript_ && t.uid == playerScript_->uid ? 0 : t.uid);
                return true;
            }
            if (!std::strcmp(name, "instance") &&
                (nc.function == 4 || nc.function == 5 || nc.function == 6 || nc.function == 7 || nc.function == 8 ||
                 nc.function == 9 || nc.function == 14 || nc.function == 15 || nc.function == 16 || nc.function == 26 || nc.function == 28)) {
                nc.result = vm::Value::object(self->uid); // native interface of this instance (multibody etc.)
                return true;
            }
            if (!std::strcmp(name, "uberobjectbase") && (nc.function == 0 || nc.function == 1 || nc.function == 5) && uber_) {
                const Object* object = findMut(self->uid);
                if (!object) return false;
                const bool knownObject = std::any_of(objectNames_.begin(), objectNames_.end(), [&](const auto& entry) {
                    return entry.second == object->name;
                });
                if (!knownObject || (nc.function == 5 && !nc.args.empty())) return false; // native "object mismatch" path
                // fn 1 is invoked on the command member itself, or on the object with the command member as argument.
                const uint32_t commandOff = nc.args.size() == 1 && nc.args[0].kind == vm::Value::Kind::Member &&
                                                    nc.args[0].uid == self->uid ? nc.args[0].offset : off;
                // fn 0 (0x79D040 -> 0x4AB6A0) queries the same way: {object, query index} member, receiver or argument.
                const uint32_t command = nc.function <= 1 ? self->fields[commandOff + 4] : 0;
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
                if (!jo->joint) jo->joint.emplace(Object::Joint{});
                auto& j = *jo->joint;
                const auto range = [&](const vm::Value& lo, const vm::Value& hi, const vm::Value& id) {
                    const float a = std::bit_cast<float>(lo.bits), b = std::bit_cast<float>(hi.bits);
                    j.ranges.push_back({a, b, int32_t(id.bits), true}); // 0x49A5C0: a new watch starts inside
                };
                if (!std::strcmp(name, "joint")) j.prismatic = true;
                if (!std::strcmp(name, "limit") && nc.args.size() == 4) range(nc.args[1], nc.args[2], nc.args[3]);
                const bool plateLimit = !std::strcmp(name, "multibody") && nc.function == 79 && nc.args.size() == 5;
                if (plateLimit) range(nc.args[2], nc.args[3], nc.args[4]);
                nc.result = vm::Value::scalar(0);
                if (!plateLimit || !multibody_) return true;
            }
            // multibody 80 (0x4A9D90 -> 0x493A80): drops the range watches of joint args[0], limit args[1] (1..3) with id
            // args[2] (-1: all) — l5_valaskjalf jammed gear 6985 stops its 45..180 watch (id 3) once it ran vt 34. Ours
            // keep one degree of freedom's watches: limit 1.
            if (!std::strcmp(name, "multibody") && nc.function == 80 && nc.args.size() == 3) {
                nc.result = vm::Value::scalar(0);
                if (nc.args[1].bits - 1u >= 3u || nc.args[0].bits != 0 || nc.args[1].bits != 1) return true; // out of range: no-op
                if (Object* jo = findMut(self->uid); jo && jo->joint)
                    std::erase_if(jo->joint->ranges, [&](const auto& r) { return int32_t(nc.args[2].bits) == -1 || r.id == int32_t(nc.args[2].bits); });
                return true;
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
                // 1 / 2 as a sequence (setSoundBusy): the first call plays, the retries (one a tick, stack untouched) only
                // ask whether the sound still plays.
                const bool waits = nc.function == 1 || nc.function == 2;
                const auto key = std::make_pair(self->uid, uint32_t(off));
                const auto w = soundWaits_.find(key);
                const bool retry = waits && w != soundWaits_.end() && w->second + 1 >= tick_;
                if (m && m->initAt && (retry || sound_(self->uid, off, *m->initAt, nc.function, anchor))) {
                    nc.result = vm::Value::scalar(0);
                    if (waits && soundBusy_ && soundBusy_(self->uid, off)) soundWaits_[key] = tick_, nc.wait = true;
                    else if (waits) soundWaits_.erase(key);
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
            // unit 12 (0x776BA0): the instance's model markers flagged 4 join Lara's marker list (0x7164D0, her grapple
            // attach points); 13 (0x776C30 -> 0x71DB90) drops them. l4_pool volume 5625 turns invisible ceiling grapple
            // 5511 off while its instance is in, on again 3 s after. Ours: World::grappleTarget skips grappleOff().
            if (!std::strcmp(name, "unit") && (nc.function == 12 || nc.function == 13) && nc.args.size() == 1) {
                const int32_t target = uidOf(nc.args[0]);
                if (nc.function == 12) grappleOff_.erase(target);
                else grappleOff_.insert(target);
                nc.result = vm::Value::scalar(0);
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
            if (onCall) onCall(source, target.uid, vt);
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

// Anim state graph and clips of an object (Object::GraphSet), parsed once per object name. Null without a skeleton.
std::shared_ptr<const Object::GraphSet> Runtime::graphSet(const std::string& name, bool graphOnly) {
    if (const auto it = graphSets_.find(name); it != graphSets_.end() && (it->second || !graphOnly)) return it->second;
    if (graphOnly)
        if (const auto it = graphSets_.find(name + "#graph"); it != graphSets_.end()) return it->second;
    auto& slot = graphSets_[graphOnly ? name + "#graph" : name];
    auto bytes = bigfile_ ? bigfile_->read("pc-w\\" + name + ".drm") : std::nullopt;
    if (!bytes) return slot;
    const auto drm = cdc::parseDrm(std::move(*bytes));
    auto set = std::make_shared<Object::GraphSet>();
    try { set->graph = formats::loadAnimGraph(drm); } catch (const FormatError& e) {
        std::fprintf(stderr, "script: %s anim graph: %s\n", name.c_str(), e.what());
    }
    uint32_t model = 0;
    for (const uint32_t id : formats::objectRenderModels(drm))
        if ((model = id)) break;
    std::vector<std::string> warnings;
    for (auto& sk : anim::decodeSkeletons(drm, warnings))
        if (model && sk.renderModelId == model) set->skeleton = std::make_shared<scene::Skeleton>(std::move(sk));
    if (!set->skeleton) {
        if (graphOnly && set->graph) slot = std::move(set); // a graph without clips (states and transitions only)
        return slot;
    }
    for (const auto& e : anim::animList(drm)) {
        auto& clip = set->clips.emplace_back();
        set->names.push_back(e.name);
        for (const auto& s : drm.sections)
            if (s.type == cdc::SectionType::Animation && s.id == e.animId) {
                try { clip = anim::decodeClip(s.data); } catch (const FormatError&) {}
                break;
            }
    }
    return slot = std::move(set);
}

// Receptacles: objects whose anim graph takes the insert stimulus 0x20 (pole holes and levers, keyholes, the l1 eye
// pieces, Kali's crown, the l4 traproom seal, portable_key_preinsert holders; docs/GAMEPLAY.md "Inserting a portable").
std::shared_ptr<const Object::GraphSet> Runtime::receptacleGraph(const std::string& name) {
    auto g = graphSet(name, true);
    if (!g || !g->graph || std::ranges::none_of(g->graph->transitions, [](const auto& t) { return t.mask & formats::kStimulusInsert; }))
        return nullptr;
    return g;
}

uint32_t Runtime::objectFlags(const std::string& name) {
    if (const auto it = objectFlags_.find(name); it != objectFlags_.end()) return it->second;
    uint32_t flags = 0;
    if (auto bytes = bigfile_ ? bigfile_->read("pc-w\\" + name + ".drm") : std::nullopt) {
        try { flags = formats::objectFlags(cdc::parseDrm(std::move(*bytes))); } catch (const FormatError&) {}
    }
    return objectFlags_[name] = flags;
}

// Instances besides Lara on the triggers (0x5CD520, after Lara's tests): a volume tests the instances it added
// (triggervolumebase Add) and every mover (oflags 0x100); one entering / leaving runs its prototype 8 / 9 (vt 56 / 57,
// 0x79CD00 / 0x79CD40) with the instance, an added one then prototype 16 / 17 (0x5C8A40 -> 0x79CEC0 / 0x79CF10) with
// reason 1 [I: as Lara's] and the instance. A plane runs prototype 7 (vt 55, 0x79CB60) with each mover crossing it
// (l4_temple 5247: the falling statue head 5236 breaks floor 5234). [I] An instance's point is its origin as it is
// drawn: displaced (Object::offset) and moved by its body (Object::body: a joint, a pushed block); enemies' AI
// positions are not mirrored here.
void Runtime::tickTrackedInstances() {
    const auto origin = [](const Object& o) { return o.pos + o.offset + rotation(o.rot) * vec3(o.body[3]); };
    const auto at = [&](int32_t uid) -> std::optional<vec3> {
        const Object* o = find(uid);
        return o && !o->dead ? std::optional(origin(*o)) : std::nullopt;
    };
    std::vector<int32_t> movers;
    for (const auto& o : objects_)
        if (o.mover && !o.dead) movers.push_back(o.uid);
    for (size_t i = 0; i < objects_.size(); ++i) {
        if (objects_[i].dead || (objects_[i].kind != Kind::Volume && objects_[i].kind != Kind::Plane)) continue;
        const int32_t uid = objects_[i].uid;
        std::vector<int32_t> tested = objects_[i].kind == Kind::Volume ? objects_[i].added : std::vector<int32_t>{};
        for (const int32_t m : movers)
            if (std::ranges::find(tested, m) == tested.end()) tested.push_back(m);
        for (const int32_t id : tested) {
            const auto p = at(id);
            Object* t = findMut(uid);
            if (!p || !t) continue;
            const std::array arg{vm::Value::object(id)};
            if (t->kind == Kind::Plane) {
                if (const Object* m = find(id); m && m->lastAt && crosses(t->shape, t->pos, t->rot, *m->lastAt, *p))
                    callback(uid, "triggerplanebase", 7, arg);
                continue;
            }
            const bool now = inside(t->shape, t->pos, t->rot, *p);
            if (now == t->holds.contains(id)) continue;
            now ? (void)t->holds.insert(id) : (void)t->holds.erase(id);
            const bool added = std::ranges::find(t->added, id) != t->added.end();
            callback(uid, "triggervolumebase", now ? 8 : 9, arg);
            if (added) callback(uid, "triggervolumebase", now ? 16 : 17, std::array{vm::Value::scalar(1), vm::Value::object(id)});
        }
    }
    for (const int32_t id : movers)
        if (Object* m = findMut(id)) m->lastAt = origin(*m);
}

// 0x6A5660: the state's clip starts (once, or looping with flag 2; none keeps the pose), its timer and stimulus totals
// restart, then the script's vt 20 (instance prototype 33, 0x799060) gets the state id. Not modelled: the entry posts
// INSTANCE message 0x1E2 (state +8, kept in the anim record +0x80; no reader found) and, for state flags 0x800 / 0x400
// / 0x200 / 0x100 / 0x80, message 0x138 (1 / 3 / 5 / 0 / -1) to the burn component (0x6B0C00; ma_ei_ignitable only).
void Runtime::graphEnter(int32_t uid, int state) {
    Object* o = findMut(uid);
    if (!o || !o->graph || !o->graph->set->graph) return;
    auto& g = *o->graph;
    const auto& s = g.set->graph->states[size_t(state)];
    g.state = state, g.transition = -1, g.frames = 0, g.sums = {}, g.total = 0;
    if (s.anim >= 0) g.clip = s.anim, g.t = 0, g.loop = s.flags & 2;
    const auto arg = vm::Value::scalar(uint32_t(int32_t(s.index)));
    callback(uid, "instance", 33, std::span(&arg, 1));
}

// 0x6A5C50 / 0x6B3DD0: the transition's clip plays once (its end enters the target, World ticks: tickGraph); vt 19
// (instance prototype 32, 0x799020) gets its id. Without a clip the target is entered at once.
void Runtime::graphBegin(int32_t uid, int transition) {
    Object* o = findMut(uid);
    if (!o || !o->graph || !o->graph->set->graph) return;
    auto& g = *o->graph;
    const auto& t = g.set->graph->transitions[size_t(transition)];
    g.transition = transition;
    if (t.anim >= 0) g.clip = t.anim, g.t = 0, g.loop = false;
    const auto arg = vm::Value::scalar(uint32_t(int32_t(t.index)));
    callback(uid, "instance", 32, std::span(&arg, 1));
    o = findMut(uid);
    if (o && o->graph && o->graph->transition == transition && t.anim < 0) graphEnter(uid, t.target < 0 ? o->graph->state : t.target);
}

// 0x6B3DD0 / 0x6B3BD0: a value >= 0 is accepted only between transitions (the accepted mask is cleared while one
// runs) and starts AnimGraph::pick's draw among the reached transitions; a value < 0 (scripts' "any") starts
// pickAny's, also while one runs (then among the running transition's target state's).
bool Runtime::graphStimulus(int32_t uid, uint32_t mask, float value) {
    const Object* o = find(uid);
    if (!o || !o->graph || !o->graph->set->graph) return false;
    const auto& g = *o->graph;
    const auto& G = *g.set->graph;
    int ti = -1;
    if (value < 0) {
        const int target = g.transition >= 0 ? G.transitions[size_t(g.transition)].target : -1;
        ti = G.pickAny(target >= 0 ? target : g.state, mask, graphSeed_);
    } else if (g.transition < 0) {
        ti = G.pick(g.state, mask, value, g.total, graphSeed_);
    }
    if (ti < 0) return false;
    graphBegin(uid, ti);
    return true;
}

// 0x699D60 then 0x6B3DD0: an engine stimulus adds `amount` to its lowest bit's total (also while a transition runs),
// which becomes the last posted total, then `value` is offered.
bool Runtime::graphPost(int32_t uid, uint32_t mask, float amount, float value) {
    Object* o = findMut(uid);
    if (!o || !o->graph || !mask) return false;
    auto& g = *o->graph;
    g.total = g.sums[size_t(std::countr_zero(mask))] += amount;
    return graphStimulus(uid, mask, value);
}

bool Runtime::insertPortable(int32_t portable, int32_t receptacle, int32_t segment) {
    graphStimulus(portable, formats::kStimulusInsert, -1.0f);
    const vm::Value args[] = {vm::Value::scalar(uint32_t(segment)), vm::Value::scalar(5)};
    callback(receptacle, "instance", 67, args);
    return graphStimulus(receptacle, formats::kStimulusInsert, -1.0f);
}

bool Runtime::removePortable(int32_t portable, int32_t receptacle, int32_t segment) {
    const vm::Value args[] = {vm::Value::scalar(uint32_t(segment)), vm::Value::scalar(5)};
    callback(receptacle, "instance", 67, args);
    const bool started = graphStimulus(receptacle, formats::kStimulusRemove, -1.0f);
    graphStimulus(portable, formats::kStimulusRemove, -1.0f);
    return started;
}

void Runtime::engage(int32_t uid, int32_t segment, int32_t kind, bool on) {
    const vm::Value args[] = {vm::Value::scalar(uint32_t(segment)), vm::Value::scalar(uint32_t(kind))};
    callback(uid, "instance", on ? 67 : 68, args);
}

bool Runtime::graphAccepts(int32_t uid, uint32_t mask) const {
    const Object* o = find(uid);
    if (!o || !o->graph || !o->graph->set->graph || o->graph->transition >= 0) return false;
    const auto& G = *o->graph->set->graph;
    return std::ranges::any_of(G.states[size_t(o->graph->state)].out, [&](uint32_t ti) { return G.transitions[ti].mask & mask; });
}

// 0x6B4090 (messages 0x166 / 0x168): mask 1 [U: 2 while Lara's +0x68 flag 0x40000 is set], AnimGraph::pickUse.
bool Runtime::graphUse(int32_t uid, float value) {
    const Object* o = find(uid);
    if (!o || !o->graph || !o->graph->set->graph || o->graph->transition >= 0) return false;
    const int ti = o->graph->set->graph->pickUse(o->graph->state, formats::kStimulusUse, value, graphSeed_);
    if (ti < 0) return false;
    graphBegin(uid, ti);
    return true;
}

// A switch's use moves the state id its modifiable member holds (Runtime::addUnit) along that state's use transition
// (0x6B4090, AnimGraph::pickUse); a null target stays. No callbacks: the Use event dispatches the switch's handlers
// with what this returns: the state id now held, and whether the transition taken led back into the same state.
Runtime::SwitchUse Runtime::switchUsed(Object& o) {
    const auto off = o.script ? modifiableMember(*o.script) : std::nullopt;
    const auto set = off ? graphSet(o.name) : nullptr;
    if (!set || !set->graph) return {};
    const auto& G = *set->graph;
    uint32_t& id = o.script->fields[*off];
    const int state = G.stateById(int32_t(id));
    const int ti = state >= 0 ? G.pickUse(state, formats::kStimulusUse, 0, graphSeed_) : -1;
    if (ti < 0) return {};
    const int target = G.transitions[size_t(ti)].target;
    if (target >= 0) id = uint32_t(int32_t(G.states[size_t(target)].index));
    return {int32_t(id), G.transitions[size_t(ti)].index, target < 0 || target == state};
}

// 0x6B4440 message 0x11C: the event mask (+0xA8, the state's transitions' damage classes; cleared while one runs)
// gates 0x6A52E0. [I] A transition without a clip enters its target at once (the exe flags the clip done, 0x100000).
bool Runtime::graphHit(int32_t uid, uint32_t damageClass, float damage) {
    Object* o = findMut(uid);
    if (!o || !o->graph || !o->graph->set->graph) return false;
    o->graph->damage += damage;
    if (o->graph->transition >= 0) return true;
    const int ti = o->graph->set->graph->pickHit(o->graph->state, damageClass, damage, o->graph->total, graphSeed_);
    if (ti >= 0) graphBegin(uid, ti);
    return true;
}

// Per tick: the clip advances; the hits' damage is posted as stimulus 0x800 (0x6B4390); a transition ends with its
// clip; else the timer stimulus (0x8000) carries the frames spent in the state.
void Runtime::tickGraph(int32_t uid) {
    Object* o = findMut(uid);
    if (!o || !o->graph) return;
    if (o->graph->clip >= 0) o->graph->t += dt_;
    if (!o->graph->set->graph) return;
    if (const float d = std::exchange(o->graph->damage, 0.0f); d > 0) {
        graphPost(uid, formats::kStimulusEvent, d, d);
        if (!(o = findMut(uid)) || !o->graph) return;
    }
    auto& g = *o->graph;
    const auto& clips = g.set->clips;
    if (g.transition >= 0) {
        const bool done = g.clip < 0 || size_t(g.clip) >= clips.size() || g.t >= clips[size_t(g.clip)].durationSeconds();
        const int target = g.set->graph->transitions[size_t(g.transition)].target;
        if (done) graphEnter(uid, target < 0 ? g.state : target);
        return;
    }
    const float step = float(dt_ * 30);
    g.frames += step;
    graphPost(uid, formats::kStimulusTimer, step, g.frames);
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
    float t = prev + float(s.dir) * float(dt_) * 30.0f;
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

int32_t Runtime::unitScriptUid(const std::string& unit) const {
    const auto it = unitScripts_.find(unit);
    const auto a = it == unitScripts_.end() ? auxiliary_.end() : auxiliary_.find(it->second);
    return a == auxiliary_.end() ? 0 : a->second->uid;
}

std::string Runtime::helpCondition() const {
    for (const auto& [key, instance] : auxiliary_)
        for (const auto& [offset, text] : instance->strings)
            if (text.size() == 7 && text.starts_with("hint_")) return text;
    return {};
}

// NtPlayerSensor update (0x77A020): each enabled sensor tests Lara's root against its anchor (0x7799F0: distance^2 <=
// radius^2, the radius straight from the member's init, in world units) and calls the owner's vt 14 (0x798430: sensor,
// inside) on entering when its onEnter byte is set and on leaving when onExit is; a oneShot sensor disables itself as
// it fires (0x779950). [I] tru.exe tests a sensor every other frame; ours every tick.
void Runtime::tickSensors(vec3 lara) {
    if (!anchorAt_ || sensors_.empty()) return;
    std::vector<std::pair<uint64_t, bool>> fired;
    for (auto& [key, sn] : sensors_) {
        const int32_t uid = int32_t(uint32_t(key >> 32));
        const auto self = instance(uid);
        if (!self || !self->program || !sn.enabled || !sn.anchor) continue;
        const Member* m = self->program->member(self->typeId, uint32_t(key));
        if (!m || m->init.value_or(0) != 0) continue;
        const auto at = anchorAt_(sn.anchor, sn.marker);
        if (!at) continue;
        const float r = std::bit_cast<float>(m->initWords[1]);
        const vec3 d = lara - *at;
        const bool now = glm::dot(d, d) <= r * r;
        const uint32_t flags = m->init2.value_or(0);
        if (now != sn.inside && (now ? flags & 0xFF : flags >> 8 & 0xFF)) {
            if (flags >> 16 & 0xFF) sn.enabled = false;
            fired.emplace_back(key, now);
        }
        sn.inside = now;
    }
    for (const auto& [key, now] : fired) {
        vm::Value args[2] = {{}, vm::Value::scalar(now)};
        args[0].kind = vm::Value::Kind::Member, args[0].uid = int32_t(uint32_t(key >> 32)), args[0].offset = uint32_t(key);
        run(args[0].uid, 14, args);
    }
}

void Runtime::tick(const TickInput& in) {
    events_.clear();
    ++tick_;
    savedOrder_.clear(); // a load's object order is put back by now (restore, setUnits)
    for (const auto& [uid, vt] : std::exchange(calls_, {})) run(uid, vt, {});
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
        if (c.state == 2 && (c.elapsed += float(dt_)) >= c.alarm && !c.fired) c.fired = true, alarms.push_back(key);
    for (const uint64_t key : alarms) {
        vm::Value clock;
        clock.kind = vm::Value::Kind::Member, clock.uid = int32_t(key >> 32), clock.offset = uint32_t(key);
        callback(clock.uid, "corebase", 25, std::span(&clock, 1));
    }
    tickSensors(in.pos);
    fade_ = fade_ < fadeTarget_ ? std::min(fadeTarget_, fade_ + fadeStep_) : std::max(fadeTarget_, fade_ - fadeStep_);
    for (size_t i = 0; i < threads_.size(); ++i) { // copy: adopt may add or end threads
        const auto [uid, thread] = threads_[i];
        if (!instance(uid)) continue; // owner unloaded: dropped below
        if (!thread->tick(float(dt_))) recordFailure(uid, find(uid) ? find(uid)->name : "unit script", *thread);
        adopt(thread);
    }
    std::erase_if(threads_, [&](const auto& t) { return !t.second->active() || !instance(t.first); });
    for (auto& o : objects_) {
        if (o.spline) tickSpline(o);
        if (o.graph) tickGraph(o.uid);
        for (size_t i = 0; i < o.fibers.size(); ++i) { // copy: a STATE may end the object's threads
            const auto fiber = o.fibers[i];
            if (!fiber->tick(float(dt_))) recordFailure(o.uid, o.name, *fiber);
            adopt(fiber);
        }
        std::erase_if(o.fibers, [](const auto& m) { return !m->active(); });
        if (o.machine && o.machine->active()) {
            const auto machine = o.machine;
            if (!machine->tick(float(dt_))) recordFailure(o.uid, o.name, *machine);
            adopt(machine);
        }
        if (o.mainPending && (!o.machine || !o.machine->active())) fire(EventKind::Main, o);
    }
    used_ = 0;
    const vec3 up(0, 0, kTestPointHeight), p = in.pos + up, prev = (hasLast_ ? lastPos_ : in.pos) + up;
    const bool pressed = in.interact && !interactHeld_;

    Object* useSwitch = nullptr;
    float best = kReach;
    for (auto& o : objects_) {
        if (o.dead || residentOnly_.contains(o.unit)) continue; // a resident unit is not loaded: nothing in it meets her
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
            if (o.state == 1 && (o.t += dt_) >= (*o.clips)[0].durationSeconds()) {
                o.state = 2, o.t = 0;
                if (o.kind == Kind::Switch) fire(EventKind::Use, o);
            } else if (o.state == 2) {
                o.t += dt_;
                // A resetting switch returns to idle after its delay (resetAfter) and can be used again.
                if (o.kind == Kind::Switch && o.resetAfter >= 0 && o.t >= o.resetAfter)
                    o.state = 0, o.t = 0;
            }
            // Receptacles have no use engagement (only kind-5 insert records): l4_ballcourt pole lever 6502 waits for its pole.
            if (o.kind == Kind::Switch && o.state == 0 && pressed && !receptacleGraph(o.name)) {
                // By the "use" record she engages [O 0x719520 / 0x6F3C60: the record's cylinder and facing cone, three
                // points of hers; World::engagedUse], whatever is nearer. The reach below is the rule of a Runtime
                // without a World (TickInput::engagedUse -1) and of switches without a record [I].
                if (o.useRecords && in.engagedUse >= 0) {
                    if (in.engagedUse == o.uid) best = -1.0f, useSwitch = &o;
                    break;
                }
                // Reach is measured from the switch or from where she stands to use it (useDistance along its local
                // horizontal axes, World::useSwitchClip), whichever is nearer [I]: a wall switch's own collision
                // keeps her farther from its origin than kReach (l1_kraken_room 30: 217 with her back to the hall).
                float d = glm::length(vec2(o.pos - in.pos));
                const float stand = o.useDistance > 20 && o.useDistance < 500 ? o.useDistance : 135.0f;
                const vec2 lx(std::cos(o.rot.z), std::sin(o.rot.z)), ly(-lx.y, lx.x);
                for (const vec2 at : {lx * stand, -lx * stand, ly * stand, -ly * stand}) d = std::min(d, glm::length(vec2(o.pos) + at - vec2(in.pos)));
                if (d < best && std::abs(o.pos.z - in.pos.z) < kReachHeight) best = d, useSwitch = &o;
            }
            break;
        case Kind::Scripted:
            // Lara uses an anim-graph object whose state leaves by the use stimulus (buttress mounts; tru.exe messages
            // 0x166 / 0x168 -> 0x6B4090 mask 1) [I: reach as switches', measured from any bone's bind position].
            if (pressed && o.graph && o.graph->set->graph && o.graph->transition < 0 && !o.hidden) {
                const auto& G = *o.graph->set->graph;
                const bool usable = std::ranges::any_of(G.states[size_t(o.graph->state)].out,
                                                        [&](uint32_t ti) { return G.transitions[ti].mask & formats::kStimulusUse; });
                if (o.useRecords && in.engagedUse >= 0) { // by the record she engages [O 0x719520], whatever is nearer
                    if (usable && in.engagedUse == o.uid) best = -1.0f, useSwitch = &o;
                    break;
                }
                for (const vec3 q : usable ? usePoints(o) : std::vector<vec3>{})
                    if (const float d = glm::length(vec2(q - in.pos)); d < best && std::abs(q.z - in.pos.z) < kReachHeight)
                        best = d, useSwitch = &o;
            }
            break;
        }
    }
    if (useSwitch && useSwitch->graph) {
        graphUse(useSwitch->uid, 0); // [I] the message's value (0x166 +4)
    } else if (useSwitch) {
        useSwitch->state = useSwitch->clips ? 1 : 2, useSwitch->t = 0;
        used_ = useSwitch->uid;
        if (useSwitch->state == 2) fire(EventKind::Use, *useSwitch);
    }
    tickTrackedInstances();
    // Births (instanceref Birth): the objects exist from now on, their constructors run (which may birth more).
    while (!births_.empty()) {
        const int32_t uid = births_.front();
        births_.erase(births_.begin());
        const auto d = dormant_.find(uid);
        if (d == dormant_.end() || born(uid)) continue; // its unit streamed out meanwhile
        born_.push_back(uid);
        if (onBirth) onBirth(uid);
        introduce(d->second.unit, d->second.program, d->second.intro, true);
        ++visibilityVersion_;
    }
    hasLast_ = true, lastPos_ = in.pos, interactHeld_ = in.interact;
}

std::vector<vec3> Runtime::usePoints(const Object& o) {
    std::vector<vec3> out;
    if (!o.graph || !o.graph->set->skeleton) return out;
    std::vector<vec3> bind;
    for (const auto& b : o.graph->set->skeleton->bones) {
        bind.push_back(vec3(b.pivot[0], b.pivot[1], b.pivot[2]) + (b.parent >= 0 ? bind[size_t(b.parent)] : vec3(0)));
        out.push_back(o.pos + rotation(o.rot) * bind.back());
    }
    return out;
}

std::optional<std::vector<glm::mat4>> Runtime::pose(const Object& o) const {
    glm::mat4 rigid = o.body;
    if (o.offset != vec3(0)) // a fallen or carried body: world displacement in model space, as a block
        rigid = glm::translate(glm::mat4(1), glm::transpose(rotation(o.rot)) * o.offset) * o.body;
    if (!o.animPose.empty()) { // an uber section's clip under its rigid motion
        auto out = o.animPose;
        for (auto& m : out) m = rigid * m;
        return out;
    }
    // A rider (instance 23: its body is its host's marker frame, World::poseAnchored) still plays its own clip under
    // that frame: l1_ship_sinking_hall door 1167 opens on the rolling hallway 1056. Other rigid motions replace the clip.
    const bool rider = anchors_.contains(o.uid);
    if (rigid != glm::mat4(1) && !rider) return std::vector<glm::mat4>{rigid};
    const auto under = [&](std::optional<std::vector<glm::mat4>> clip) -> std::optional<std::vector<glm::mat4>> {
        if (rigid == glm::mat4(1)) return clip;
        if (!clip) return std::vector<glm::mat4>{rigid};
        for (auto& m : *clip) m = rigid * m;
        return clip;
    };
    if (o.graph) {
        const auto& g = *o.graph;
        if (g.clip < 0 || size_t(g.clip) >= g.set->clips.size() || !g.set->clips[size_t(g.clip)].keyCount) return under(std::nullopt);
        const auto& sk = *g.set->skeleton;
        return under(anim::skinPalette(sk, anim::worldMatrices(sk, anim::sample(g.set->clips[size_t(g.clip)], sk, g.t, g.loop))));
    }
    if (!o.clips || !o.skeleton || o.state == 0) return under(std::nullopt);
    const auto& clip = (*o.clips)[o.state == 1 ? 0 : 1];
    const auto world = anim::worldMatrices(*o.skeleton, anim::sample(clip, *o.skeleton, o.t, o.state == 2));
    return under(anim::skinPalette(*o.skeleton, world));
}

void Runtime::setBody(int32_t uid, const glm::mat4& model) {
    if (Object* o = findMut(uid)) o->body = model;
}

void Runtime::setAnimPose(int32_t uid, std::vector<glm::mat4> palette) {
    if (Object* o = findMut(uid)) o->animPose = std::move(palette);
}

void Runtime::setJointPosition(int32_t uid, float position) {
    Object* o = findMut(uid);
    if (!o || !o->joint) return;
    o->joint->position = position;
    std::vector<std::pair<int32_t, bool>> changed; // {range id, entered}
    for (auto& r : o->joint->ranges) {
        const bool in = r.lo <= position && position <= r.hi;
        if (in != r.inside) changed.push_back({r.id, in});
        r.inside = in;
    }
    // instance prototypes 47 / 50 = vt 34 / 37 (type 3's prototype table); args (multibody, joint, range id) [I: the
    // first two].
    for (const auto& [id, entered] : changed)
        callback(uid, "instance", entered ? 47 : 50,
                 std::array{vm::Value::scalar(0), vm::Value::scalar(0), vm::Value::scalar(uint32_t(id))});
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

void Runtime::AudioState::apply(const MusicRequest& r) {
    if (r.function == 3) {
        if (r.slot < 0) slots.fill({});
        else if (size_t(r.slot) < slots.size()) slots[size_t(r.slot)] = {};
        return;
    }
    if (r.track.empty() || r.slot >= int32_t(slots.size())) return;
    Slot& s = slots[size_t(std::max(0, r.slot))];
    if (r.function == 2) s = {r.track, true};
    else if (!std::equal(s.track.begin(), s.track.end(), r.track.begin(), r.track.end(),
                         [](char a, char b) { return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b)); }))
        s = {r.track, false}; // a Load of the slot's own track leaves it as it is (0x593EB0)
}

nlohmann::json Runtime::serialize() const {
    nlohmann::json j = stash_;
    j["gameValues"] = gameValues_;
    j["graphSeed"] = graphSeed_;
    nlohmann::json music = nlohmann::json::array();
    for (size_t i = 0; i < audio_.slots.size(); ++i)
        if (!audio_.slots[i].track.empty())
            music.push_back({{"slot", i}, {"track", audio_.slots[i].track}, {"playing", audio_.slots[i].playing}});
    j["audio"] = {{"music", std::move(music)}, {"reverbUnit", audio_.reverbUnit},
                  {"reverbAt", {audio_.reverbAt.section, audio_.reverbAt.offset}}};
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
    j["player"] = {{"hiddenGroups", playerHiddenGroups_}, {"objects", playerObjects_}, {"scriptFields", std::move(fields)}, {"lastBike", lastBike_}};
    if (gauntletCharged_) j["player"]["gauntletCharged"] = true; // larabase 19 (absent: not charged, as in saves before it)
    // The running script itself (version 1): sleeping threads, unit / global script instances (by unit and type: their
    // uids depend on load order, so a load gives them the saved ones), the objects then alive (a load resumes them rather
    // than constructing them again), clocks, queued messages, the fade. A load's entries still waiting for their unit
    // are kept. Older saves: objects and unit scripts start afresh when their unit loads.
    std::map<std::pair<std::string, uint32_t>, nlohmann::json> auxOf = savedAux_;
    for (const auto& [key, in] : auxiliary_)
        if (!key.first->unit.empty()) auxOf[{key.first->unit, key.second}] = {in->uid, in->fields, in->strings, in->properties};
    nlohmann::json aux = nlohmann::json::array(), threads = nlohmann::json::array();
    nlohmann::json clocks = nlohmann::json::array(), messages = nlohmann::json::array();
    for (const auto& [key, a] : auxOf) aux.push_back({key.first, key.second, a.at(0), a.at(1), a.at(2), a.at(3)});
    std::set<int32_t> live = savedLive_;
    std::map<int32_t, nlohmann::json> machinesOf = savedMachines_;
    for (const auto& o : objects_) {
        live.insert(o.uid);
        nlohmann::json fibers = nlohmann::json::array();
        for (const auto& f : o.fibers)
            if (f->active()) fibers.push_back(f->save());
        const bool running = o.machine && o.machine->active();
        // What its constructor set up that a resumed object does not build again: joint ranges, Lara tracking, spline
        // ranges (natives multibody 1 / 79, joint 17, limit 3; triggervolumebase Add; spline AddRange).
        nlohmann::json setup = nlohmann::json::object();
        if (o.joint) {
            nlohmann::json ranges = nlohmann::json::array();
            for (const auto& r : o.joint->ranges) ranges.push_back({r.lo, r.hi, r.id, r.inside});
            setup["joint"] = {o.joint->prismatic, std::move(ranges), o.joint->speed};
        }
        if (o.tracksLara) setup["tracksLara"] = true;
        if (!o.added.empty()) setup["added"] = o.added;
        if (o.spline && !o.spline->ranges.empty()) setup["splineRanges"] = o.spline->ranges;
        if (running || !fibers.empty() || o.mainPending || !setup.empty())
            machinesOf[o.uid] = {running ? o.machine->save() : nlohmann::json(nullptr), std::move(fibers), o.mainPending, std::move(setup)};
    }
    nlohmann::json machines = nlohmann::json::object();
    for (auto& [uid, m] : machinesOf) machines[std::to_string(uid)] = std::move(m);
    for (const auto& [uid, m] : threads_)
        if (m->active()) threads.push_back({uid, m->save()});
    for (const auto& [uid, m] : savedThreads_) threads.push_back({uid, m});
    for (const auto& [key, c] : clocks_) clocks.push_back({int32_t(uint32_t(key >> 32)), uint32_t(key), c.state, c.elapsed, c.alarm, c.fired});
    for (const auto& m : messages_) messages.push_back({m.channel, m.id, m.payload, m.sender});
    nlohmann::json sensors = nlohmann::json::array();
    for (const auto& [key, sn] : sensors_)
        sensors.push_back({int32_t(uint32_t(key >> 32)), uint32_t(key), sn.anchor, sn.marker, sn.enabled, sn.inside});
    nlohmann::json playerUnit = nullptr;
    if (playerUnit_) playerUnit = {playerUnit_->first, playerUnit_->second ? nlohmann::json(*playerUnit_->second) : nlohmann::json(nullptr)};
    j["runtime"] = {{"version", 1}, {"aux", std::move(aux)}, {"player", playerScript_ ? playerScript_->uid : 0},
                    {"nextAux", nextAuxiliaryUid_}, {"live", live}, {"machines", std::move(machines)}, {"threads", std::move(threads)},
                    {"clocks", std::move(clocks)}, {"sensors", std::move(sensors)}, {"messages", std::move(messages)}, {"tick", tick_}, {"playerUnit", playerUnit},
                    {"fade", {fade_, fadeTarget_, fadeStep_}}, {"control", playerControl_}, {"uiFlag", uiFlag_},
                    {"unitEffect", unitEffect_}, {"last", {lastPos_.x, lastPos_.y, lastPos_.z, hasLast_}}, {"interactHeld", interactHeld_},
                    {"weather", {weather_.snow, weather_.rain, weather_.lightningMin, weather_.lightningMax}},
                    {"deferredUnits", deferredUnits_}, {"grappleOff", grappleOff_}, {"born", born_}, {"anchors", anchors_}};
    if (!away_.empty()) { // instances away from their birth unit; the loaded ones where they are now
        nlohmann::json away = nlohmann::json::array();
        for (const auto& [uid, a] : away_) {
            vec3 at = a.at;
            if (const Object* o = find(uid))
                if (const auto home = unitRefs_.find(o->unit); home != unitRefs_.end()) at = o->pos + o->offset - home->second.offset;
            away.push_back({uid, a.birth, a.unit, at.x, at.y, at.z});
        }
        j["runtime"]["away"] = std::move(away);
    }
    // The object list's order (update and hash order): a load makes the units' own objects first, then born dormant
    // intros, then away instances, which need not be the running world's order (the summoned bike's instance before
    // poachers born later); restore puts them back in this order. Saved again before the first tick after a load (its
    // objects may not be made yet): the loaded order, then the objects it does not name.
    std::vector<int32_t> order = savedOrder_;
    order.reserve(order.size() + objects_.size());
    for (const auto& o : objects_)
        if (savedOrder_.empty() || std::ranges::find(savedOrder_, o.uid) == savedOrder_.end()) order.push_back(o.uid);
    j["runtime"]["order"] = std::move(order);
    return j;
}

void Runtime::restore(const nlohmann::json& j) {
    gameValues_.fill(0);
    messages_.clear(), mounts_.clear(); // a load replaces a chapter start still being announced
    savedAux_.clear(), savedMachines_.clear(), savedLive_.clear(), savedThreads_.clear();
    if (j.is_object() && j.contains("gameValues") && j["gameValues"].is_array())
        for (size_t i = 0; i < gameValues_.size() && i < j["gameValues"].size(); ++i)
            gameValues_[i] = j["gameValues"][i].get<uint8_t>();
    stash_ = j.is_object() ? j : nlohmann::json::object();
    stash_.erase("gameValues");
    graphSeed_ = stash_.value("graphSeed", 0u);
    stash_.erase("graphSeed");
    audio_ = {};
    if (const auto a = stash_.find("audio"); a != stash_.end() && a->is_object()) {
        audio_.slots.fill({});
        if (const auto m = a->find("music"); m != a->end() && m->is_array()) {
            for (const auto& e : *m)
                if (const int slot = e.value("slot", -1); slot >= 0 && size_t(slot) < audio_.slots.size())
                    audio_.slots[size_t(slot)] = {e.value("track", std::string()), e.value("playing", true)};
        } else if (const int slot = a->value("slot", 0); slot >= 0 && size_t(slot) < audio_.slots.size()) {
            audio_.slots[size_t(slot)] = {a->value("track", std::string()), true}; // older saves: one playing track
        }
        audio_.reverbUnit = a->value("reverbUnit", std::string());
        if (const auto at = a->value("reverbAt", std::array<uint32_t, 2>{}); !audio_.reverbUnit.empty())
            audio_.reverbAt = {at[0], at[1]};
    }
    stash_.erase("audio");
    const nlohmann::json player = stash_.value("player", nlohmann::json::object());
    stash_.erase("player");
    const nlohmann::json runtime = stash_.value("runtime", nlohmann::json::object());
    stash_.erase("runtime");
    if (const uint32_t mask = player.value("hiddenGroups", playerGearGroups_); mask != playerHiddenGroups_)
        playerHiddenGroups_ = mask, ++visibilityVersion_;
    playerObjects_ = player.value("objects", std::set<std::string>{});
    lastBike_ = player.value("lastBike", 0);
    gauntletCharged_ = player.value("gauntletCharged", false);
    if (playerScript_) {
        playerScript_->fields = playerScript_->initial;
        if (const auto f = player.find("scriptFields"); f != player.end() && f->is_object())
            for (const auto& [key, value] : f->items())
                if (value.is_number_unsigned() || value.is_number_integer())
                    playerScript_->fields[uint32_t(std::stoul(key))] = value.get<uint32_t>();
    }
    // Born dormant intros as saved (older saves: none): their objects follow the others, created after the state below.
    births_.clear();
    if (auto born = runtime.value("born", std::vector<int32_t>{}); born != born_) {
        std::erase_if(objects_, [&](const Object& o) { return dormant_.contains(o.uid); });
        born_ = std::move(born), ++visibilityVersion_;
    }
    // Instances away from their birth unit as saved: the ones living in a loaded unit are made after the runtime
    // block (resumed there), the others wait for their unit.
    {
        std::map<int32_t, Away> away;
        if (const auto a = runtime.find("away"); a != runtime.end() && a->is_array())
            for (const auto& e : *a)
                if (e.is_array() && e.size() == 6 && e[0].is_number_integer() && e[1].is_string() && e[2].is_string() && e[3].is_number() &&
                    e[4].is_number() && e[5].is_number())
                    away[e[0].get<int32_t>()] = {e[1].get<std::string>(), e[2].get<std::string>(), vec3(e[3].get<float>(), e[4].get<float>(), e[5].get<float>())};
        // objects that were away in the world being replaced and are not in the save: back at their birth
        std::vector<int32_t> home;
        for (const auto& [uid, a] : away_)
            if (!away.contains(uid)) home.push_back(uid);
        const size_t before = objects_.size();
        std::erase_if(objects_, [&](const Object& o) {
            if (const auto a = away.find(o.uid); a != away.end()) return true; // made again below, where the save has it
            return std::ranges::find(home, o.uid) != home.end();
        });
        if (objects_.size() != before) ++visibilityVersion_;
        away_ = std::move(away);
        for (const int32_t uid : home) // their birth unit's intro again (its saved state follows in the loop below)
            for (const auto& [name, u] : unitRefs_) {
                if (find(uid)) break;
                if (!u.drm) continue;
                const auto unit = formats::parseUnit(*u.drm);
                const auto in = std::ranges::find_if(unit.intros, [&](const formats::Intro& i) { return i.uniqueId == uid; });
                if (in == unit.intros.end()) continue;
                std::shared_ptr<vm::Program> program;
                for (const auto& o : objects_)
                    if (o.unit == name && o.script && o.script->program && o.script->program->unit == name) {
                        program = o.script->program;
                        break;
                    }
                if (!program) continue;
                const auto key = std::to_string(uid); // addObject takes the stashed state: the loop below applies it
                const nlohmann::json kept = stash_.contains(key) ? stash_[key] : nlohmann::json(nullptr);
                introduce(u, program, *in, false);
                if (!kept.is_null()) stash_[key] = kept;
            }
    }
    for (auto& o : objects_) {
        const auto key = std::to_string(o.uid);
        applyState(o, stash_.contains(key) ? &stash_[key] : nullptr);
        stash_.erase(key);
    }
    introduceBorn(false); // resumed below (live)
    threads_.clear();
    hasLast_ = false; // older saves: no plane crossings across the jump
    if (runtime.value("version", 0) == 1) {
        try {
            restoreRuntime(runtime);
        } catch (const std::exception& e) { // a bad block: threads start afresh, as for older saves
            savedAux_.clear(), savedMachines_.clear(), savedLive_.clear(), savedThreads_.clear();
            threads_.clear(), clocks_.clear(), sensors_.clear(), messages_.clear();
            for (auto& o : objects_) o.machine.reset(), o.fibers.clear();
            std::fprintf(stderr, "script: saved runtime state not restored: %s\n", e.what());
        }
    }
    for (const auto& [uid, a] : std::map<int32_t, Away>(away_))
        if (!find(uid)) materialize(uid, a);
    savedOrder_ = runtime.value("order", std::vector<int32_t>{});
    applySavedOrder(); // and again after the next setUnits, which makes the loaded units' objects
}

void Runtime::applySavedOrder() {
    if (savedOrder_.empty()) return;
    std::unordered_map<int32_t, size_t> rank; // objects the save does not name keep their order, last
    for (size_t i = 0; i < savedOrder_.size(); ++i) rank.try_emplace(savedOrder_[i], i);
    const auto at = [&](const Object& o) {
        const auto it = rank.find(o.uid);
        return it == rank.end() ? savedOrder_.size() : it->second;
    };
    if (!std::ranges::is_sorted(objects_, {}, at)) std::ranges::stable_sort(objects_, {}, at), ++visibilityVersion_;
}

void Runtime::restoreRuntime(const nlohmann::json& r) {
    // Validate first: what waits for later units is read without checks.
    const auto machineShape = [](const nlohmann::json& m) {
        if (!m.is_null()) (void)m.at("frames").get<std::vector<nlohmann::json>>(), (void)m.at("steps").get<size_t>(), (void)m.at("wait").get<float>();
    };
    for (const auto& a : r.at("aux")) {
        (void)a.at(0).get<std::string>(), (void)a.at(1).get<uint32_t>(), (void)a.at(2).get<int32_t>();
        (void)a.at(3).get<std::map<uint32_t, uint32_t>>(), (void)a.at(4).get<std::map<uint32_t, std::string>>();
        (void)a.at(5).get<std::map<uint64_t, uint32_t>>();
    }
    for (const auto& [key, m] : r.at("machines").items()) {
        (void)std::stoi(key), machineShape(m.at(0)), (void)m.at(2).get<bool>();
        if (!m.at(3).is_object()) throw std::runtime_error("object setup of " + key);
        for (const auto& f : m.at(1)) machineShape(f);
    }
    for (const auto& t : r.at("threads")) (void)t.at(0).get<int32_t>(), machineShape(t.at(1));
    // Unit / global script instances and Lara's take their saved uids, the ones not in the save go below them all.
    int32_t lowest = std::min(r.at("nextAux").get<int32_t>(), r.at("player").get<int32_t>() - 1);
    for (const auto& a : r.at("aux")) {
        savedAux_[{a.at(0).get<std::string>(), a.at(1).get<uint32_t>()}] = {a.at(2), a.at(3), a.at(4), a.at(5)};
        lowest = std::min(lowest, a.at(2).get<int32_t>() - 1);
    }
    savedLive_ = r.at("live").get<std::set<int32_t>>();
    for (const auto& [key, m] : r.at("machines").items()) savedMachines_[std::stoi(key)] = m;
    for (const auto& t : r.at("threads")) savedThreads_.emplace_back(t.at(0).get<int32_t>(), t.at(1));
    if (playerScript_ && r.at("player").get<int32_t>() < 0) playerScript_->uid = r.at("player");
    for (auto& [key, in] : auxiliary_) { // alive already (a load into a running World)
        const auto a = savedAux_.find({key.first->unit, key.second});
        if (a == savedAux_.end()) {
            in->uid = lowest--;
            continue;
        }
        in->uid = a->second.at(0);
        in->fields = a->second.at(1).get<std::map<uint32_t, uint32_t>>();
        in->strings = a->second.at(2).get<std::map<uint32_t, std::string>>();
        in->properties = a->second.at(3).get<std::map<uint64_t, uint32_t>>();
        savedAux_.erase(a);
    }
    nextAuxiliaryUid_ = lowest;
    for (auto& o : objects_) resume(o);
    attachSavedThreads();
    clocks_.clear();
    for (const auto& c : r.at("clocks"))
        clocks_[uint64_t(uint32_t(c.at(0).get<int32_t>())) << 32 | c.at(1).get<uint32_t>()] =
            Clock{c.at(2).get<int>(), c.at(3).get<float>(), c.at(4).get<float>(), c.at(5).get<bool>()};
    sensors_.clear();
    if (const auto sn = r.find("sensors"); sn != r.end())
        for (const auto& c : *sn)
            sensors_[uint64_t(uint32_t(c.at(0).get<int32_t>())) << 32 | c.at(1).get<uint32_t>()] =
                Sensor{c.at(2).get<int32_t>(), c.at(3).get<int32_t>(), c.at(4).get<bool>(), c.at(5).get<bool>()};
    for (const auto& m : r.at("messages"))
        messages_.push_back({m.at(0).get<uint32_t>(), m.at(1).get<uint32_t>(), m.at(2).get<std::string>(), m.at(3).get<int32_t>()});
    tick_ = r.at("tick");
    if (const auto& p = r.at("playerUnit"); p.is_array())
        playerUnit_ = {p.at(0).get<uint32_t>(), p.at(1).is_null() ? std::nullopt : std::optional<uint32_t>(p.at(1).get<uint32_t>())};
    else
        playerUnit_.reset();
    fade_ = r.at("fade").at(0), fadeTarget_ = r.at("fade").at(1), fadeStep_ = r.at("fade").at(2);
    playerControl_ = r.at("control"), uiFlag_ = r.at("uiFlag"), unitEffect_ = r.at("unitEffect");
    const auto& last = r.at("last");
    lastPos_ = vec3(last.at(0).get<float>(), last.at(1).get<float>(), last.at(2).get<float>()), hasLast_ = last.at(3);
    interactHeld_ = r.at("interactHeld");
    weather_ = {};
    if (const auto it = r.find("weather"); it != r.end()) // saves from before 0.30 have none
        weather_ = {it->at(0).get<int32_t>(), it->at(1).get<int32_t>(), it->at(2).get<int32_t>(), it->at(3).get<int32_t>()};
    deferredUnits_ = r.value("deferredUnits", std::set<std::string>{});
    grappleOff_ = r.value("grappleOff", std::set<int32_t>{}); // older saves have none
    anchors_ = r.value("anchors", std::map<int32_t, std::pair<int32_t, int32_t>>{});
}

} // namespace ou::script
