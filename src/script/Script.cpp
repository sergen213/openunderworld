// Script types, intro links, trigger shapes and the object runtime. Evidence: docs/formats/SCRIPTS.md.
#include "script/Script.hpp"

#include "collision/Collision.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
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

const char* kEventNames[] = {"enter", "firstin", "in", "out", "cross", "use", "activate"};

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
    if (endsWith(n, "triggervolume")) return Kind::Volume; // triggervolume, soundtriggervolume, firetriggervolume, ...
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
    const auto [mp, mn] = views.array(s, 0x2C);
    for (uint32_t k = 0; k < mn; ++k) {
        const View& v = views[mp.section];
        const uint32_t b = mp.offset + 0x14 * k;
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
                if (const auto vp = iv.ptr(ip->offset)) m.init = views[vp->section].at<uint32_t>(vp->offset);
            } else {
                m.init = iv.at<uint32_t>(ip->offset);
            }
        }
        t.members.push_back(m);
    }

    // Functions: 0x1C bytes {Prototype*, ..., +0x18 code SArray of u32 words}; Prototype +6 = i16 vtIndex.
    const auto [fp, fn] = views.array(s, 0x38);
    for (uint32_t k = 0; k < fn; ++k) {
        const View& v = views[fp.section];
        const uint32_t b = fp.offset + 0x1C * k;
        Function f;
        if (const auto pp = v.ptr(b)) {
            f.proto = *pp;
            f.vtIndex = views[pp->section].at<int16_t>(pp->offset + 6);
        }
        const auto [cp, cn] = views.array(v, b + 0x18);
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
                } else if (cv.refs.contains(off)) {
                    f.code.back().ref = word;
                } else if (immediate) {
                    f.code.back().imm = word, immediate = false;
                } else {
                    Instr in;
                    in.op = uint8_t(word >> 24);
                    in.arg = int32_t(word << 8) >> 8;
                    if (in.op >= kOpCount) throw FormatError("unknown script opcode " + std::to_string(in.op));
                    immediate = in.op == kOpPush;
                    f.code.push_back(std::move(in));
                }
            }
        }
        t.functions.push_back(std::move(f));
    }
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
nlohmann::json stateOf(const Object& o) {
    if (o.state == 0 && !o.inside && !o.firstIn && o.offset == vec3(0)) return nullptr; // birth state
    return {{"state", o.state}, {"t", o.t}, {"inside", o.inside}, {"firstIn", o.firstIn},
            {"offset", {o.offset.x, o.offset.y, o.offset.z}}};
}
void applyState(Object& o, const nlohmann::json* s) {
    o.state = 0, o.t = 0, o.inside = o.firstIn = false, o.offset = vec3(0);
    if (!s || !s->is_object()) return;
    o.state = s->value("state", 0), o.t = s->value("t", 0.0);
    o.inside = s->value("inside", false), o.firstIn = s->value("firstIn", false);
    const auto off = s->value("offset", std::vector<float>{0, 0, 0});
    if (off.size() == 3) o.offset = {off[0], off[1], off[2]};
    if (o.state < 0 || o.state > 2 || (o.state == 1 && !o.clips)) o.state = 2;
}
} // namespace

void Runtime::setUnits(const std::vector<UnitRef>& units) {
    std::erase_if(objects_, [&](const Object& o) {
        if (std::any_of(units.begin(), units.end(), [&](const UnitRef& u) { return u.name == o.unit; })) return false;
        if (auto s = stateOf(o); !s.is_null()) stash_[std::to_string(o.uid)] = std::move(s); // kept for streaming back
        return true;
    });
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
    std::map<uint32_t, uint32_t> scripts; // section id -> index
    for (uint32_t i = 0; i < u.drm->sections.size(); ++i)
        if (u.drm->sections[i].type == cdc::SectionType::Script) scripts.emplace(u.drm->sections[i].id, i);

    for (const auto& in : unit.intros) {
        const auto n = objectNames_.find(in.objectId);
        if (n == objectNames_.end()) continue;
        const auto kind = classify(n->second);
        if (!kind) continue;
        Object o;
        o.uid = in.uniqueId, o.unit = u.name, o.name = n->second, o.kind = *kind;
        o.pos = vec3(in.position[0], in.position[1], in.position[2]) + u.offset;
        o.rot = {in.rotation[0], in.rotation[1], in.rotation[2]};
        if (o.kind == Kind::Volume || o.kind == Kind::Plane) {
            if (!in.data) continue;
            const auto shape = parseShape(*u.drm, *in.data, o.kind == Kind::Plane);
            if (!shape) continue;
            o.shape = *shape;
        }
        if (const auto it = scripts.find(in.scriptId); in.scriptId && it != scripts.end()) {
            const auto t = parseScriptType(*u.drm, it->second);
            for (const auto& f : t.functions)
                if (f.vtIndex >= 0)
                    for (const int32_t target : functionTargets(t, f)) o.links[f.vtIndex].push_back(target);
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
                    for (const auto* e : {tr, hold}) {
                        if (!e) break;
                        for (const auto& s : drm.sections)
                            if (s.type == cdc::SectionType::Animation && s.id == e->animId) {
                                clips->push_back(anim::decodeClip(s.data));
                                break;
                            }
                    }
                    if (set->skeleton && clips->size() == 2) set->clips = std::move(clips);
                }
            }
            o.skeleton = set->skeleton;
            o.clips = set->clips;
        }
        addObject(std::move(o));
    }
}

void Runtime::addObject(Object o) {
    const auto key = std::to_string(o.uid);
    if (stash_.contains(key)) applyState(o, &stash_[key]), stash_.erase(key); // restored / streamed back in
    objects_.push_back(std::move(o));
}

const Object* Runtime::find(int32_t uid) const {
    const auto it = std::find_if(objects_.begin(), objects_.end(), [&](const Object& o) { return o.uid == uid; });
    return it == objects_.end() ? nullptr : &*it;
}
Object* Runtime::findMut(int32_t uid) { return const_cast<Object*>(find(uid)); }

// Pushes the event, then (depth-first, as the script VM runs a handler's calls inline) an Activate for
// every target of the script function that handles it.
void Runtime::fire(EventKind k, Object& o) {
    events_.push_back({k, o.uid, o.uid, tick_});
    int16_t vt = -1;
    switch (k) {
    case EventKind::FirstIn: vt = kVtOnFirstIn; break;
    case EventKind::In: vt = kVtOnIn; break;
    case EventKind::Out: vt = kVtOnOut; break;
    case EventKind::Cross: vt = kVtPlaneCross; break;
    case EventKind::Use: vt = kVtSwitchUsed; break;
    default: return;
    }
    const auto it = o.links.find(vt);
    if (it == o.links.end()) return;
    const int32_t source = o.uid;
    for (const int32_t target : std::vector<int32_t>(it->second)) { // copy: activation may not invalidate
        events_.push_back({EventKind::Activate, source, target, tick_});
        Object* t = findMut(target);
        if (t && t->kind == Kind::Door && t->state == 0) t->state = t->clips ? 1 : 2, t->t = 0; // [I] doors only open
    }
}

void Runtime::tick(const TickInput& in, const collision::CollisionMesh* col) {
    events_.clear();
    ++tick_;
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
            } else if (!now && o.inside) { // Exit (vt 55)
                o.inside = false;
                fire(EventKind::Out, o);
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
            if (col && col->raycast(c + vec3(0, 0, 20), axis, glm::dot(o.half, glm::abs(axis)) + step)) break;
            if (col && in.move.y < 0 && col->raycast(in.pos + vec3(0, 0, 100), axis, 100 + step)) break;
            o.offset += axis * step;
            carried_ = axis * step;
            break;
        }
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
    if (!o.clips || !o.skeleton || o.state == 0) return std::nullopt;
    const auto& clip = (*o.clips)[o.state == 1 ? 0 : 1];
    const auto world = anim::worldMatrices(*o.skeleton, anim::sample(clip, *o.skeleton, o.t, o.state == 2));
    return anim::skinPalette(*o.skeleton, world);
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
    for (const auto& o : objects_) {
        if (auto s = stateOf(o); !s.is_null()) j[std::to_string(o.uid)] = std::move(s);
        else j.erase(std::to_string(o.uid));
    }
    return j;
}

void Runtime::restore(const nlohmann::json& j) {
    stash_ = j.is_object() ? j : nlohmann::json::object();
    for (auto& o : objects_) {
        const auto key = std::to_string(o.uid);
        applyState(o, stash_.contains(key) ? &stash_[key] : nullptr);
        stash_.erase(key);
    }
    hasLast_ = false; // no plane crossings across the jump
}

} // namespace ou::script
