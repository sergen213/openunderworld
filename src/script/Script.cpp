// Script types, intro links, trigger shapes and the object runtime. Evidence: docs/formats/SCRIPTS.md.
#include "script/Script.hpp"
#include "script/Vm.hpp"

#include "collision/Collision.hpp"
#include "formats/Formats.hpp"

#include <glm/gtc/matrix_transform.hpp>

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
                if (const auto vp = iv.ptr(ip->offset)) {
                    m.init = views[vp->section].at<uint32_t>(vp->offset);
                    if (vp->offset <= drm.sections[vp->section].data.size() &&
                        drm.sections[vp->section].data.size() - vp->offset >= 8)
                        m.init2 = views[vp->section].at<uint32_t>(vp->offset + 4);
                }
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
        f.self = {fp.section, b};
        f.native = (v.at<uint8_t>(b + 4) & 1) != 0;
        f.localCount = views.array(v, b + 0x0C).second;
        if (const auto pp = v.ptr(b)) {
            f.proto = *pp;
            const View& pv = views[pp->section];
            f.vtIndex = pv.at<int16_t>(pp->offset + 6);
            f.argCount = views.array(pv, pp->offset + 0x0C).second;
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
    if (o.state == 0 && !o.inside && !o.firstIn && !o.hidden && o.engagement < 0 && o.offset == vec3(0) && fields.empty() && strings.empty() && properties.empty()) return nullptr; // birth state
    return {{"state", o.state}, {"t", o.t}, {"inside", o.inside}, {"firstIn", o.firstIn},
            {"offset", {o.offset.x, o.offset.y, o.offset.z}}, {"hidden", o.hidden}, {"engagement", o.engagement}, {"scriptFields", std::move(fields)},
            {"scriptStrings", std::move(strings)}, {"scriptProperties", std::move(properties)}};
}
void applyState(Object& o, const nlohmann::json* s) {
    o.state = 0, o.t = 0, o.inside = o.firstIn = false, o.offset = vec3(0);
    o.hidden = false, o.engagement = -1;
    o.machine.reset();
    if (o.script) {
        o.script->fields = o.script->initial;
        o.script->strings = o.script->initialStrings;
        o.script->properties = o.script->initialProperties;
    }
    if (!s || !s->is_object()) return;
    o.state = s->value("state", 0), o.t = s->value("t", 0.0);
    o.inside = s->value("inside", false), o.firstIn = s->value("firstIn", false);
    o.hidden = s->value("hidden", false), o.engagement = s->value("engagement", -1);
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
}
} // namespace

void Runtime::setBigfile(const cdc::BigfileSet* bigfile) {
    bigfile_ = bigfile;
    playerScript_.reset();
    if (!bigfile_) return;
    const auto bytes = bigfile_->read("pc-w\\lara.drm");
    if (!bytes) return;
    const auto drm = cdc::parseDrm(*bytes);
    std::vector<ScriptType> types;
    for (uint32_t i = 0; i < drm.sections.size(); ++i)
        if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(drm, i));
    auto program = std::make_shared<vm::Program>(std::move(types));
    playerScript_ = program->instantiate(nextAuxiliaryUid_--, 205); // LaraBase player instance script
}

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
    const size_t firstNew = objects_.size();
    std::vector<ScriptType> types;
    for (uint32_t i = 0; i < u.drm->sections.size(); ++i)
        if (u.drm->sections[i].type == cdc::SectionType::Script) types.push_back(parseScriptType(*u.drm, i));
    auto program = std::make_shared<vm::Program>(std::move(types));

    for (const auto& in : unit.intros) {
        const auto n = objectNames_.find(in.objectId);
        if (n == objectNames_.end()) continue;
        const auto kind = classify(n->second);
        if (!kind && !in.scriptId) continue;
        Object o;
        o.uid = in.uniqueId, o.unit = u.name, o.name = n->second, o.kind = kind.value_or(Kind::Scripted);
        o.pos = vec3(in.position[0], in.position[1], in.position[2]) + u.offset;
        o.rot = {in.rotation[0], in.rotation[1], in.rotation[2]};
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
        addObject(std::move(o), false);
    }
    for (size_t i = firstNew; i < objects_.size(); ++i) {
        fire(EventKind::Start, objects_[i]);
        objects_[i].mainPending = true;
        fire(EventKind::Main, objects_[i]);
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

void Runtime::recordFailure(const Object& o) {
    if (!o.machine) return;
    if (failures_[{o.uid, o.machine->failure()}]++ == 0)
        std::fprintf(stderr, "script: uid %d (%s) handler stopped: %s\n", o.uid, o.name.c_str(), o.machine->failure().c_str());
}

void Runtime::endCinematic(int32_t uid) {
    if (Object* object = findMut(uid)) fire(EventKind::CinematicEnd, *object);
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
    case EventKind::Use: vt = contains(o.name, "phole_lever") ? kVtLeverMoved : kVtSwitchUsed; break;
    case EventKind::Start: vt = -2; break; // StartObject invokes the first function, not a fixed virtual slot
    case EventKind::Main: vt = -3; break;  // function 2 is the object's main/polling script
    case EventKind::CinematicEnd: vt = -1; break; // non-virtual post-stream helper
    default: return;
    }
    const int32_t source = o.uid;
    const size_t eventCount = events_.size();
    bool completed = false;
    if (o.script) {
        auto resolve = [this](int32_t uid) -> std::shared_ptr<vm::Instance> {
            for (auto& candidate : objects_) if (candidate.uid == uid) return candidate.script;
            if (playerScript_ && playerScript_->uid == uid) return playerScript_;
            for (auto& [key, instance] : auxiliary_) if (instance->uid == uid) return instance;
            return {};
        };
        auto auxiliary = [this](const std::shared_ptr<vm::Program>& program, uint32_t typeId) {
            const auto key = std::make_pair(program.get(), typeId);
            auto& instance = auxiliary_[key];
            if (!instance) instance = program->instantiate(nextAuxiliaryUid_--, typeId);
            return instance;
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
            // Receiver-independent bindings whose side effects other engine systems own (sound: below, once bound).
            for (const char* passive : {"soundgroup", "music", "corebase", "uipopupmgr", "uilocalstrproperty", "globaleffect"})
                if (!std::strcmp(name, passive)) { nc.result = vm::Value::scalar(0); return true; }
            // Receivers: a member address (object uid + offset), an object, or a native value held in a local.
            auto self = resolve(nc.receiver.uid);
            const bool inLocal = nc.receiver.kind == vm::Value::Kind::Scalar;
            uint32_t localValue = nc.receiver.bits;
            if (inLocal && !std::strcmp(name, "instanceref")) {
                auto target = resolve(int32_t(localValue));
                if (nc.function == 7 && nc.args.empty()) { // Get: null when unbound (0x4AC070)
                    nc.result = target ? vm::Value::object(target->uid) : vm::Value::scalar(0);
                    return true;
                }
                if ((nc.function == 3 || nc.function == 6) && nc.args.empty()) { nc.wait = !target; return true; }
            }
            if (!self || !self->program) return false;
            const auto program = self->program;
            const uint32_t off = nc.receiver.offset;
            auto field = [&]() -> uint32_t& { return self->fields[off]; };
            if (!std::strcmp(name, "clock") && nc.receiver.kind == vm::Value::Kind::Member) {
                Clock& c = clocks_[(uint64_t(uint32_t(self->uid)) << 32) | off]; // tru.exe 0x4A3450..0x4A34F0, update 0x4A3380
                switch (nc.function) {
                case 0: if (c.state != 2) c = {2, 0, c.alarm}; return true;         // Start
                case 1: c.state = 0; return true;                                    // Stop
                case 2: if (c.state == 2) c.state = 1; return true;                  // Pause
                case 3: if (c.state == 1) c.state = 2; return true;                  // Resume
                case 4: nc.result = vm::Value::scalar(c.state == 2); return true;     // IsRunning
                case 5: case 7: nc.result = vm::Value::scalar(std::bit_cast<uint32_t>(c.elapsed)); return true;
                case 6: case 8: nc.result = vm::Value::scalar(std::bit_cast<uint32_t>(c.alarm)); return true;
                case 9:                                                                // SetAlarm
                    if (nc.args.size() != 1) return false;
                    c.alarm = std::bit_cast<float>(nc.args[0].bits);
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
            // health on Lara's script: 9 current value; 1 / 2 hit her (death volumes and planes; non-player targets unsupported).
            if (!std::strcmp(name, "health") && health_ &&
                (nc.function == 9 || ((nc.function == 1 || nc.function == 2) && playerScript_ && self == playerScript_))) {
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
                return uid == 0 || (playerScript_ && uid == playerScript_->uid);
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
            if (!std::strcmp(name, "messagechannel") && nc.function == 2 && nc.args.size() == 2 &&
                nc.args[1].kind == vm::Value::Kind::Constant && nc.args[1].text && message_)
                return message_(field(), nc.args[0].bits, *nc.args[1].text); // Send (tru.exe 0x4A2E70)
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
                    const bool opens = value == 0, changes = opens ? target->state == 0 : target->state != 0;
                    if (changes) {
                        events_.push_back({EventKind::Activate, source, target->uid, tick_});
                        target->state = opens ? (target->clips ? 1 : 2) : 0;
                        target->t = 0;
                    }
                }
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
            if (!std::strcmp(name, "instance") && nc.function == 60 && nc.args.size() == 1) {
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
            if (!std::strcmp(name, "instance") &&
                (nc.function == 4 || nc.function == 5 || nc.function == 6 || nc.function == 7 || nc.function == 8 ||
                 nc.function == 9 || nc.function == 14 || nc.function == 15 || nc.function == 16 || nc.function == 26 || nc.function == 28 ||
                 nc.function == 61)) {
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
                const uint32_t command = nc.function == 1 ? self->fields[off + 4] : 0;
                uint32_t commandIndex = 0;
                if (nc.function == 1)
                    for (const ScriptType* type = self->program->type(self->typeId); type; type = self->program->type(type->superId))
                        for (const auto& member : type->members)
                            if (!std::strcmp(self->program->nativeName(member.typeId), "uberobjectcommand")) {
                                if (member.offset == off) goto commandIndexFound;
                                ++commandIndex;
                            }
            commandIndexFound:
                uint32_t result = 0;
                if (!uber_(object->uid, nc.function, command, commandIndex, nc.args, result)) return false;
                nc.result = vm::Value::scalar(result);
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
            // Existing engine systems already handle (or intentionally do not yet expose) these side effects.
            // Their return values are zero; unknown gameplay bindings remain unsupported instead of being ignored.
            for (const char* passive : {"sound", "soundgroup", "music", "corebase", "uipopupmgr",
                                        "uilocalstrproperty", "globaleffect"})
                if (!std::strcmp(name, passive)) { nc.result = vm::Value::scalar(0); return true; }
            if (!std::strcmp(name, "unit") && nc.function == 15 && nc.args.size() == 1) {
                unitEffect_ = nc.args[0].bits; // tru.exe 0x6D6F20 writes the global post-effect scalar
                return true;
            }
            return false;
        };
        auto cinematic = [this, source](vm::Instance& target, int16_t vt) {
            if (vt != 57 && vt != 63) return false;
            const auto* object = findMut(target.uid);
            if (!object || object->name != "cinematic_object") return false;
            events_.push_back({EventKind::Activate, source, object->uid, tick_});
            return true;
        };
        if (o.machine && o.machine->active()) {
            completed = true; // a sleeping handler owns the object until it finishes
        } else {
            o.machine = std::make_shared<vm::Machine>(resolve, native, cinematic);
            completed = o.machine->call(*o.script, vt);
            if (!completed) recordFailure(o);
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

void Runtime::tick(const TickInput& in, const collision::CollisionMesh* col) {
    events_.clear();
    ++tick_;
    for (auto& [key, c] : clocks_) // 0x4A3380; alarm callbacks (fn 11 handler) are not run [U]
        if (c.state == 2) c.elapsed += float(kDt);
    fade_ = fade_ < fadeTarget_ ? std::min(fadeTarget_, fade_ + fadeStep_) : std::max(fadeTarget_, fade_ - fadeStep_);
    for (auto& o : objects_) {
        if (o.machine && o.machine->active() && !o.machine->tick(float(kDt))) recordFailure(o);
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
    if (o.body != glm::mat4(1)) return std::vector<glm::mat4>{o.body};
    if (!o.clips || !o.skeleton || o.state == 0) return std::nullopt;
    const auto& clip = (*o.clips)[o.state == 1 ? 0 : 1];
    const auto world = anim::worldMatrices(*o.skeleton, anim::sample(clip, *o.skeleton, o.t, o.state == 2));
    return anim::skinPalette(*o.skeleton, world);
}

void Runtime::setBody(int32_t uid, const glm::mat4& model) {
    if (Object* o = findMut(uid)) o->body = model;
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
    return j;
}

void Runtime::restore(const nlohmann::json& j) {
    gameValues_.fill(0);
    if (j.is_object() && j.contains("gameValues") && j["gameValues"].is_array())
        for (size_t i = 0; i < gameValues_.size() && i < j["gameValues"].size(); ++i)
            gameValues_[i] = j["gameValues"][i].get<uint8_t>();
    stash_ = j.is_object() ? j : nlohmann::json::object();
    stash_.erase("gameValues");
    for (auto& o : objects_) {
        const auto key = std::to_string(o.uid);
        applyState(o, stash_.contains(key) ? &stash_[key] : nullptr);
        stash_.erase(key);
    }
    hasLast_ = false; // no plane crossings across the jump
}

} // namespace ou::script
