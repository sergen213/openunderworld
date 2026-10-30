// Progression gates (docs/PROGRESSION.md): per chapter, the critical-path units and the puzzle / switch / uber /
// door / trigger / cinematic gates between them. Survey (OU_PROGRESSION_SURVEY=1) prints the map from the data;
// the gate tests teleport Lara next to each gate, perform the original interaction and assert its effect.
#include "game/Game.hpp"
#include "game/Progress.hpp"

#include "cdc/Cdc.hpp"
#include "formats/Formats.hpp"
#include "script/Vm.hpp"

#include <gtest/gtest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <map>
#include <ranges>
#include <set>

using namespace ou;
using game::vec3;

namespace {

const char* gameDir() { return std::getenv("OU_GAME_DIR"); }

std::vector<std::string> unitNames(const cdc::BigfileSet& bf) {
    const auto list = bf.read("\\tr8\\pc-w\\unitlist.txt");
    std::vector<std::string> units;
    if (!list) return units;
    const std::string text(list->begin(), list->end());
    for (size_t pos = text.find('\n') + 1; pos < text.size();) {
        const size_t nl = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        const std::string name = line.substr(line.rfind('\\') + 1);
        if (bf.read("pc-w\\" + name + ".drm")) units.push_back(name);
    }
    return units;
}

const char* kindName(script::Kind k) {
    switch (k) {
    case script::Kind::Volume: return "volume";
    case script::Kind::Plane: return "plane";
    case script::Kind::Switch: return "switch";
    case script::Kind::Door: return "door";
    case script::Kind::Block: return "block";
    case script::Kind::Scripted: return "scripted";
    }
    return "?";
}

// ---- gate harness ---------------------------------------------------------------------------------------------
// A gate: the original interaction on `trigger` in `unit` and the objects it must move / open / play.
enum class Action : uint8_t {
    Use,    // switch / lever: interact press next to it (Runtime: 200 units, +-250 z)
    Push,   // push block: grip (interact held) and push (stick forward) from one of its four faces
    Pull,   // mounted grapple target: World's grapple pull runs its pull handler (Runtime::grapplePulled)
    Step,   // pressure plate: Lara stands on it
    Weigh,  // pressure plate: Lara carries the nearest portable weight (*_pt_*weight*) onto it and sets it down
    Enter,  // trigger volume: Lara walks into it; trigger plane: Lara crosses it
};
const char* actionName(Action a) {
    constexpr const char* n[] = {"use", "push", "pull", "step", "weigh", "enter"};
    return n[size_t(a)];
}
struct Gate {
    std::string unit;
    int32_t trigger = 0;
    Action action = Action::Use;
    std::vector<int32_t> targets; // empty: the trigger itself must move (push blocks)
    int nextChapter = -1;         // >= 0: the gate is a level end (chapter change asserted)
    std::vector<std::pair<int32_t, Action>> first{}; // steps done before the gate (any loaded unit), 4 s apart
};
struct GateResult {
    bool pass = false;
    std::vector<int32_t> changed, unchanged;
    std::string failure; // first script handler stop on the trigger or a target
    int chapter = -1;
};

// Gating targets in a link list: what a player sees open / move / play (not effects, cameras, checkpoints).
bool significant(const script::Object& o) {
    for (const char* skip : {"fx_", "checkpoint", "cameraanchor", "reveal_object", "dispatcher", "trigger", "soundvolume",
                             "adrenaline_tracker", "ping_map", "watervolume", "marker"})
        if (o.name.find(skip) != std::string::npos) return false;
    return true;
}

// Observable state of an object: animation state, visibility, rigid motion, block offset, uber sections, spline, its
// collision. Two snapshots differ = the object visibly opened / moved / changed.
struct Snapshot {
    int state = 0;
    bool hidden = false;
    glm::mat4 body{1};
    vec3 offset{0};
    std::vector<uint32_t> uber;
    float spline = 0;
    std::vector<glm::mat4> dynamics;
};
Snapshot snapshot(const game::World& w, int32_t uid) {
    Snapshot s;
    if (const auto* o = w.script().find(uid)) {
        s.state = o->state, s.hidden = o->hidden, s.body = o->body, s.offset = o->offset;
        if (o->spline) s.spline = o->spline->t;
    }
    if (const auto* u = w.uberState(uid)) s.uber = *u;
    for (const auto& d : w.collision().dynamics)
        if (d.id == uid) s.dynamics.push_back(d.transform);
    return s;
}
bool moved(const glm::mat4& a, const glm::mat4& b) {
    for (int c = 0; c < 4; ++c)
        if (glm::length(a[c] - b[c]) > 1e-2f) return true;
    return false;
}
bool differs(const Snapshot& a, const Snapshot& b) {
    if (a.state != b.state || a.hidden != b.hidden || moved(a.body, b.body) || glm::length(a.offset - b.offset) > 1.0f ||
        a.uber != b.uber || std::abs(a.spline - b.spline) > 1e-3f || a.dynamics.size() != b.dynamics.size())
        return true;
    for (size_t i = 0; i < a.dynamics.size(); ++i)
        if (moved(a.dynamics[i], b.dynamics[i])) return true;
    return false;
}

void place(game::World& w, vec3 pos, float yaw) {
    game::PlayerState s;
    s.pos = pos, s.yaw = yaw;
    w.restore(s, yaw, -0.25f);
}

// Lara grips uber object `uid` (its model 0 riding a level prismatic joint, index `joint`) from behind and pushes it
// along the joint's +axis for `ticks` ticks (World::pushUberJoints).
void pushUber(game::World& w, int32_t uid, int32_t joint, int ticks) {
    const auto* j = w.uberJoint(uid, joint);
    ASSERT_TRUE(j);
    vec3 lo(1e30f), hi(-1e30f);
    for (const auto& d : w.collision().dynamics)
        if (d.id == uid)
            for (const vec3& v : d.shape->vertices) lo = glm::min(lo, vec3(d.transform * glm::vec4(v, 1))), hi = glm::max(hi, vec3(d.transform * glm::vec4(v, 1)));
    ASSERT_LT(lo.x, hi.x) << uid << " has collision";
    const vec3 axis = j->axis, c = (lo + hi) * 0.5f;
    vec3 at = c - axis * (glm::dot(hi - lo, glm::abs(axis)) * 0.5f + 60.0f);
    at.z = lo.z;
    if (const auto floor = w.collision().groundProbe(at + vec3(0, 0, 300), 1000, 0x0F)) at.z = floor->point.z;
    place(w, at, std::atan2(axis.y, axis.x));
    game::Actions push;
    push.interact = true, push.move.y = 1;
    for (int i = 0; i < ticks; ++i) w.tick(push);
    w.tick({});
}

// Runs one gate in a fresh World of its unit (neighbours streamed, natives bound, no enemies).
GateResult runGate(const cdc::BigfileSet& bf, const game::UnitLoader& load, const game::AnimLibrary& anims, const Gate& g,
                   int chapter) {
    GateResult r;
    game::World w(load, g.unit, anims);
    w.script().setBigfile(&bf);
    w.chapter = chapter;
    std::set<int32_t> activated;
    w.onCinematic = [](const std::string&, bool) {};
    const auto tick = [&](const game::Actions& a) {
        w.tick(a);
        for (const auto& e : w.script().events()) {
            if (e.kind == script::EventKind::Activate) activated.insert(e.target);
            if (std::getenv("OU_PROGRESSION_TRACE"))
                std::printf("  tick %llu %s %d -> %d\n", (unsigned long long)w.ticks(), script::eventName(e.kind), e.source, e.target);
        }
    };
    tick({});
    if (!w.script().find(g.trigger)) {
        r.failure = "trigger not loaded";
        return r;
    }
    // The original interaction with object `uid` (world frame; any loaded unit).
    const auto perform = [&](int32_t uid, Action action) {
        const auto* t = w.script().find(uid);
        if (!t) return;
        const vec3 at = t->pos;
        switch (action) {
        case Action::Use: {
            place(w, at, 0);
            game::Actions use;
            use.interact = true;
            for (int i = 0; i < 10; ++i) tick(use);
            break;
        }
        case Action::Push: { // each face in turn until the block moves
            for (const float yaw : {0.0f, glm::half_pi<float>(), glm::pi<float>(), -glm::half_pi<float>()}) {
                const auto* b = w.script().find(uid);
                const vec3 dir(std::cos(yaw), std::sin(yaw), 0);
                vec3 p = b->pos + b->offset - dir * (std::abs(glm::dot(b->half, dir)) + 60.0f);
                if (const auto floor = w.collision().groundProbe(p + vec3(0, 0, 200), 600, 0x0F)) p.z = floor->point.z;
                place(w, p, yaw);
                game::Actions a;
                a.interact = true;
                for (int i = 0; i < 12; ++i) tick(a);
                a.move.y = 1;
                for (int i = 0; i < 120; ++i) tick(a);
                tick({});
                if (glm::length(w.script().find(uid)->offset) > 50.0f) break;
            }
            break;
        }
        case Action::Pull:
            place(w, at - vec3(600, 0, 0), 0);
            tick({});
            w.script().grapplePulled(uid);
            break;
        case Action::Step:
            place(w, at + vec3(0, 0, 30), 0);
            break;
        case Action::Weigh: {
            const script::Object* weight = nullptr;
            for (const auto& o : w.script().objects())
                if (o.name.find("_pt_") != std::string::npos && o.name.find("weight") != std::string::npos &&
                    o.name.find("break") == std::string::npos &&
                    (!weight || glm::length(o.pos + o.offset - at) < glm::length(weight->pos + weight->offset - at)) &&
                    glm::length(glm::vec2(o.pos + o.offset - at)) > 300.0f)
                    weight = &o;
            if (!weight) return;
            game::Actions press;
            press.interact = true;
            place(w, weight->pos + weight->offset, 0);
            tick(press), tick({});
            place(w, at - vec3(90, 0, -30), 0);
            tick({});
            tick(press), tick({});
            break;
        }
        case Action::Enter: {
            if (t->kind == script::Kind::Plane) {
                const glm::mat4 rot = glm::rotate(glm::mat4(1), t->rot.x, {1, 0, 0}) * glm::rotate(glm::mat4(1), t->rot.y, {0, 1, 0}) *
                                      glm::rotate(glm::mat4(1), t->rot.z, {0, 0, 1});
                const vec3 n = glm::normalize(vec3(rot * glm::vec4(0, 1, 0, 0)));
                const vec3 base = at - vec3(0, 0, script::kTestPointHeight);
                place(w, base + n * 300.0f, 0);
                tick({});
                place(w, base - n * 300.0f, 0);
            } else {
                place(w, at - vec3(0, 0, script::kTestPointHeight), 0);
            }
            break;
        }
        }
    };
    for (const auto& [uid, action] : g.first) { // earlier steps the gate needs (a puzzle's other half, a level flag)
        perform(uid, action);
        for (int i = 0; i < 240; ++i) tick({});
    }
    std::vector<int32_t> targets = g.targets;
    if (targets.empty()) targets.push_back(g.trigger);
    std::map<int32_t, Snapshot> before;
    for (const int32_t id : targets) before[id] = snapshot(w, id);
    perform(g.trigger, g.action);
    // A change at any tick counts: a hazard's swing (l1_kraken_room tentacles 494-496) returns to its rest state.
    std::set<int32_t> seen;
    const auto look = [&] {
        for (const int32_t id : targets)
            if (before.contains(id) && differs(before[id], snapshot(w, id))) seen.insert(id);
    };
    look();
    for (int i = 0; i < 600 && w.chapter == chapter; ++i) tick({}), look();
    r.chapter = w.chapter;
    for (const int32_t id : targets) {
        const auto* o = w.script().find(id);
        const bool cine = o && o->name == "cinematic_object";
        const bool ok = cine ? activated.contains(id) : seen.contains(id);
        (ok ? r.changed : r.unchanged).push_back(id);
    }
    for (const auto& [key, n] : w.script().failures())
        if (r.failure.empty() && (key.first == g.trigger || std::find(targets.begin(), targets.end(), key.first) != targets.end()))
            r.failure = std::to_string(key.first) + ": " + key.second;
    r.pass = g.nextChapter >= 0 ? r.chapter == g.nextChapter : !r.changed.empty();
    return r;
}

// Critical path per chapter (kChapters index): portal BFS order from the start unit, revisits listed where the route
// comes back (docs/PROGRESSION.md).
const std::vector<std::vector<std::string>>& chapterPaths() {
    static const std::vector<std::vector<std::string>> p = {
        {"l3_main_bedroom", "l3_bedroom_hall", "l3_main_hall"},
        {"l1_seatop", "l1_seafloor", "l1_norse_hallway_a", "l1_kraken_entrance", "l1_krakenstairs_a", "l1_krakenstairs_b",
         "l1_kraken_room", "l1_baldrsstairs_a", "l1_baldrsstairs_b", "l1_baldrs_tomb", "l1_baldrs_sideroom", "l1_baldrs_hall",
         "l1_cineseatop", "l1_ship_exterior", "l1_ship_upper_hallway_a", "l1_ship_upper_hallway_b", "l1_ship_upper_hallway",
         "l1_ship_engine_hall", "l1_ship_natla_room", "l1_ship_lower_hallway", "l1_ship_sinking_hall", "l1_cine_ship_sinking"},
        {"l2_cliffclimb", "l2_cliffhall", "l2_cliffhall_b", "l2_junglehub", "l2_hubhall", "l2_puzzleroom", "l2_upperhallway",
         "l2_towerroom", "l2_towerconnector", "l2_norsehub", "l2_norsehall", "l2_shrineroom", "l2_maproom", "l2_flushtunnel"},
        {"l3_caverns_start", "l3_chapel", "l3_stairhall", "l3_secretchamber", "l3_chamberrockhall", "l3_main_bedroom",
         "l3_bedroom_hall", "l3_main_hall", "l3_mansion_exterior_afterburn"},
        {"l4_trailstart", "l4_trailtube", "l4_ballcourt", "l4_templepath", "l4_temple", "l4_staircase_upper",
         "l4_staircase_xibalba", "l4_traprooms", "l4_staircase_mictlan", "l4_ragnarok_room", "l4_pool"},
        {"l5_startsnow", "l5_valgrind_tunnel", "l5_valgrind", "l5_valaskjalf_entry", "l5_valaskjalf_entryb", "l5_valaskjalf"},
        {"l6_ship_exterior", "l6_ship_upper_hallway_a", "l6_ship_upper_hallway_b", "l6_ship_upper_hallway",
         "l6_ship_engine_hall", "l6_ship_natla_room"},
        {"l7_thin_ice", "l7_arctic_sea", "l7_helheim_hall01", "l7_helheim_hall02", "l7_helheim01", "l7_helheim02",
         "l7_helheim03", "l7_yggdrasil", "l7_yggdrasil_hall01", "l7_yggdrasil_hall02", "l7_yggdrasil_end", "l7_monastery"},
        {"l7_himalaya_end"},
    };
    return p;
}

} // namespace

// Every candidate gate of every critical-path unit, found from the data, run once: switches / levers, push blocks,
// mounted grapple targets, pressure plates, and trigger volumes / planes linked to a door, uber object, physics
// object or cinematic. Measurement only (OU_PROGRESSION_SURVEY=1): the per-gate table in docs/PROGRESSION.md.
TEST(ProgressionReal, GateSweep) {
    if (!gameDir() || !std::getenv("OU_PROGRESSION_SURVEY")) GTEST_SKIP() << "OU_GAME_DIR and OU_PROGRESSION_SURVEY not set";
    const cdc::BigfileSet bf(gameDir());
    std::map<std::string, game::UnitData> cache;
    const game::UnitLoader load = [&](const std::string& n) {
        auto& d = cache[n];
        if (!d.drm) d = game::loadUnitData(bf, n);
        return d;
    };
    const auto anims = game::loadLaraAnims(bf);
    const char* only = std::getenv("OU_PROGRESSION_UNIT");
    const char* gateOnly = std::getenv("OU_PROGRESSION_GATE"); // one trigger uid
    int pass = 0, total = 0;
    for (size_t c = 0; c < chapterPaths().size(); ++c)
        for (const auto& unit : chapterPaths()[c]) {
            if (only && unit.find(only) == std::string::npos) continue;
            std::vector<Gate> gates;
            std::map<int32_t, std::string> names;
            {
                game::World w(load, unit, anims);
                w.script().setBigfile(&bf);
                w.tick({});
                const auto& rt = w.script();
                std::set<int32_t> mounted;
                for (const auto& u : w.units())
                    if (u.data.name == unit)
                        for (const auto& gr : u.data.grapples)
                            if (gr.kind == game::GrappleKind::Mounted) mounted.insert(gr.uid);
                for (const auto& o : rt.objects()) {
                    if (o.unit != unit || o.name.starts_with("fx_") || o.name.starts_with("puzzle_cheat")) continue;
                    names[o.uid] = o.name;
                    std::vector<int32_t> targets;
                    for (const auto& [vt, ts] : o.links)
                        for (const int32_t id : ts)
                            if (const auto* to = rt.find(id); to && significant(*to) && id != o.uid &&
                                                              std::find(targets.begin(), targets.end(), id) == targets.end())
                                targets.push_back(id);
                    Action a;
                    if (o.kind == script::Kind::Block) {
                        gates.push_back({unit, o.uid, Action::Push, {}});
                        continue;
                    }
                    if (mounted.contains(o.uid)) a = Action::Pull;
                    else if (o.kind == script::Kind::Switch) a = Action::Use;
                    else if (o.name.find("plate") != std::string::npos) a = Action::Step;
                    else if (o.kind == script::Kind::Volume || o.kind == script::Kind::Plane) a = Action::Enter;
                    else continue;
                    if (targets.empty() && a != Action::Pull) continue;
                    gates.push_back({unit, o.uid, a, targets});
                }
            }
            for (const auto& g : gates) {
                if (gateOnly && g.trigger != std::atoi(gateOnly)) continue;
                const auto r = runGate(bf, load, anims, g, int(c));
                ++total, pass += r.pass;
                std::printf("ch%zu %-26s %-5s %5d %-36s %s ->", c, unit.c_str(), actionName(g.action), g.trigger,
                            names[g.trigger].c_str(), r.pass ? "PASS" : "FAIL");
                for (auto id : r.changed) std::printf(" +%d", id);
                for (auto id : r.unchanged) std::printf(" -%d", id);
                if (r.chapter != int(c)) std::printf(" chapter->%d", r.chapter);
                if (!r.failure.empty()) std::printf(" [%s]", r.failure.c_str());
                std::printf("\n");
                std::fflush(stdout);
            }
        }
    std::printf("== gates passed %d / %d\n", pass, total);
}

// Map survey: portal graph + script-named units per chapter prefix, and per unit the interactive / gating objects with
// their static links. Measurement only (the table in docs/PROGRESSION.md was written from it).
TEST(ProgressionReal, Survey) {
    if (!gameDir() || !std::getenv("OU_PROGRESSION_SURVEY")) GTEST_SKIP() << "OU_GAME_DIR and OU_PROGRESSION_SURVEY not set";
    const cdc::BigfileSet bf(gameDir());
    const auto all = unitNames(bf);
    std::map<std::string, game::UnitData> cache;
    const auto load = [&](const std::string& n) -> game::UnitData& {
        auto& d = cache[n];
        if (!d.drm) d = game::loadUnitData(bf, n);
        return d;
    };
    const auto anims = game::loadLaraAnims(bf);
    const char* only = std::getenv("OU_PROGRESSION_UNIT");
    for (const auto& name : all) {
        if (only && name.find(only) == std::string::npos) continue;
        const auto& u = load(name);
        std::printf("== %s\n  portals:", name.c_str());
        for (const auto& p : u.portals) std::printf(" %s(%d)", p.toUnit.c_str(), p.toStreamId);
        std::printf("\n");
        const auto strings = [&](uint32_t sid) { return game::scriptStrings(*u.drm, sid); };
        for (const auto& in : formats::parseUnit(*u.drm).intros) {
            if (!in.scriptId) continue;
            std::string units;
            for (const auto& s : strings(in.scriptId))
                if (s != name && std::find(all.begin(), all.end(), s) != all.end()) units += " " + s;
            if (!units.empty()) std::printf("  intro %d names units:%s\n", in.uniqueId, units.c_str());
        }
        for (const auto& [uid, st] : u.chapterLinks) {
            std::printf("  level end %d ->", uid);
            for (const auto& s : st) std::printf(" %s", s.c_str());
            std::printf("\n");
        }
        for (const auto& [uid, stream] : u.cinematics) std::printf("  cinematic %d = %s\n", uid, stream.c_str());
        for (const auto& s : u.drm->sections) {
            if (s.type != cdc::SectionType::Script) continue;
            std::string units;
            for (const auto& n : strings(s.id))
                if (n != name && std::find(all.begin(), all.end(), n) != all.end()) units += " " + n;
            if (!units.empty()) std::printf("  script section %u names units:%s\n", s.id, units.c_str());
        }
        game::World w(game::UnitLoader(load), name, anims, game::Spawn{{0, 0, -1e6f}, 0});
        w.script().setBigfile(&bf);
        w.tick({});
        for (const auto& o : w.script().objects()) {
            if (o.unit != name) continue;
            const bool gate = o.kind == script::Kind::Switch || o.kind == script::Kind::Door || o.kind == script::Kind::Block ||
                              o.name.find("_uo_") != std::string::npos || o.name.find("lever") != std::string::npos ||
                              o.name.find("plate") != std::string::npos || o.name.find("cinematic") != std::string::npos ||
                              o.name.find("grapple") != std::string::npos || o.name.find("_rb_") != std::string::npos ||
                              o.name.find("puzzle") != std::string::npos || o.name.find("door") != std::string::npos;
            if (!gate && !std::getenv("OU_PROGRESSION_ALL")) continue; // OU_PROGRESSION_ALL: every scripted object
            std::printf("  %-8s %5d %-40s (%.0f %.0f %.0f)", kindName(o.kind), o.uid, o.name.c_str(), o.pos.x, o.pos.y, o.pos.z);
            for (const auto& [vt, ts] : o.links) {
                std::printf(" vt%d:", vt);
                for (auto t : ts) {
                    const auto* to = w.script().find(t);
                    std::printf(" %d%s%s", t, to ? "=" : "", to ? to->name.c_str() : "");
                }
            }
            std::printf("\n");
        }
    }
}

// Bytecode dump of intro scripts (type chain to the native base): OU_PROGRESSION_DUMP="unit:uid,uid,...". Research aid
// for the gate fixes (opcode names: cdcEngineDXHR ByteCodeEnum.h).
TEST(ProgressionReal, DumpScripts) {
    const char* spec = std::getenv("OU_PROGRESSION_DUMP");
    if (!gameDir() || !spec) GTEST_SKIP() << "OU_GAME_DIR and OU_PROGRESSION_DUMP not set";
    static const char* const ops[] = {
        "NOP", "POP", "PUSH", "PUSHI", "PUSHI8", "PUSHC", "PUSHL8", "PUSHL16", "PUSHL32", "PUSHLA", "PUSHLR", "PUSHS",
        "PUSHS8", "PUSHS16", "PUSHS32", "PUSHSA", "PUSHSR", "PUSHM8", "PUSHM16", "PUSHM32", "PUSHMA", "PUSHMR", "PUSHT8",
        "PUSHT16", "PUSHT32", "PUSHTA", "PUSHTR", "DREF8", "DREF16", "DREF32", "DREFA", "DREFR", "DREFD", "BOUNDS",
        "DBOUNDS", "MAP8", "MAP16", "MAP32", "MAPA", "MAPR", "MAPG", "IMAP8", "IMAP16", "IMAP32", "IMAPA", "IMAPR",
        "MOVE8", "MOVE32", "MOVER", "MOVEC", "MOVEM", "MOVED", "MOVEU", "MOVET", "MOVEN", "MOVEAR", "MOVEAC", "MOVEAM",
        "MOVEAD", "MOVEAU", "MOVEAT", "NEGI", "ADDI", "SUBI", "MULI", "DIVI", "MODI", "LTI", "LTEQI", "EQI", "EQI8",
        "NEGF", "ADDF", "SUBF", "MULF", "DIVF", "LTF", "LTEQF", "EQF", "EQC", "NOTB", "ANDB", "ORB", "XORB", "AND", "OR",
        "XOR", "SHL", "SHR", "INV", "FTOI", "FTOB", "ITOF", "ITOB", "BTOI", "BTOF", "CTOI", "CTOF", "CTOB", "JMP", "JMPF",
        "CALLU", "CALL1", "CALL2", "CALL3", "CALL4", "CALL5", "SSEQU", "SSEQ1", "SSEQ2", "SSEQ3", "SSEQ4", "SSEQ5",
        "PSEQU", "PSEQ1", "PSEQ2", "PSEQ3", "PSEQ4", "PSEQ5", "START", "TSSTART", "TPSTART", "TEND", "RET", "SLEEP",
        "STATE", "ISTYPE", "NEW", "WAIT", "PUSHD8", "PUSHD16", "PUSHD32", "PUSHDA", "MOVEP"};
    const cdc::BigfileSet bf(gameDir());
    const std::string s(spec);
    const std::string unit = s.substr(0, s.find(':'));
    const auto drm = cdc::parseDrm(*bf.read("pc-w\\" + unit + ".drm"));
    std::vector<script::ScriptType> types;
    for (uint32_t i = 0; i < drm.sections.size(); ++i)
        if (drm.sections[i].type == cdc::SectionType::Script) types.push_back(script::parseScriptType(drm, i));
    auto program = std::make_shared<script::vm::Program>(types);
    std::map<int32_t, uint32_t> scriptOf;
    if (drm.sections.at(0).type == cdc::SectionType::General) // generalbank etc. have no unit
        for (const auto& in : formats::parseUnit(drm).intros) scriptOf[in.uniqueId] = in.scriptId;
    std::set<uint32_t> shown;
    std::function<void(uint32_t)> dump = [&](uint32_t id) {
        for (; id && shown.insert(id).second; id = program->type(id) ? program->type(id)->superId : 0) {
            const auto* t = program->type(id);
            if (!t) break;
            std::printf("type %u super %u %s size %u\n", t->id, t->superId, t->nativeName.c_str(), t->size);
            if (!t->prototypes.empty()) {
                std::printf("  prototypes:");
                for (const int16_t vt : t->prototypes) std::printf(" %d", vt);
                std::printf("\n");
            }
            if (!t->nativeName.empty()) break;
            for (size_t i = 0; i < t->states.size(); ++i)
                if (const auto* f = program->method(t->states[i])) std::printf("  state %zu -> fn %u\n", i, program->functionIndex(f));
            for (const auto& m : t->members)
                std::printf("  member +%u type %u ref %u(%s)%s%s%s\n", m.offset, m.type, m.typeId, program->nativeName(m.typeId),
                            m.init ? (" init " + std::to_string(int32_t(*m.init)) + (m.init2 ? " " + std::to_string(int32_t(*m.init2)) : "")).c_str() : "",
                            m.initText.empty() ? "" : " text ", m.initText.c_str());
            for (size_t f = 0; f < t->functions.size(); ++f) {
                const auto& fn = t->functions[f];
                std::printf("  fn %zu vt %d args %u locals %u%s\n", f, fn.vtIndex, fn.argCount, fn.localCount, fn.native ? " native" : "");
                for (const auto& in : fn.code) {
                    std::printf("    %4u %-8s %d", in.word, in.op < std::size(ops) ? ops[in.op] : "?", in.arg);
                    if (in.op == script::kOpPush) std::printf(" imm 0x%X", in.imm);
                    if (!in.text.empty()) std::printf(" \"%s\"", in.text.c_str());
                    if (in.ref) std::printf(" ref %u", in.ref);
                    for (const auto& p : in.ptrs)
                        if (const auto* c = program->method(p))
                            std::printf(" -> type %u fn %u vt %d%s%s", program->owner(c), program->functionIndex(c), c->vtIndex,
                                        c->native ? " native " : "", c->native ? program->nativeName(program->owner(c)) : "");
                    std::printf("\n");
                }
            }
        }
    };
    if (s.substr(s.find(':') + 1) == "*") { // every intro script and every other type of the unit
        for (const auto& [uid, sid] : scriptOf)
            if (sid) std::printf("=== intro %d script %u\n", uid, sid), dump(sid);
        for (const auto& t : types) std::printf("=== type %u\n", t.id), dump(t.id);
        return;
    }
    for (size_t pos = s.find(':') + 1; pos < s.size();) {
        const size_t comma = std::min(s.find(',', pos), s.size());
        const int32_t uid = std::stoi(s.substr(pos, comma - pos));
        pos = comma + 1;
        std::printf("=== intro %d script %u\n", uid, scriptOf[uid]);
        dump(scriptOf[uid]);
    }
}

// Critical-path gates that must open (docs/PROGRESSION.md, status "pass"): per gate a fresh World of its unit, the original
// interaction, then the gated effect. Chapter ends assert the chapter change.
TEST(ProgressionReal, CriticalGates) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    std::map<std::string, game::UnitData> cache;
    const game::UnitLoader load = [&](const std::string& n) {
        auto& d = cache[n];
        if (!d.drm) d = game::loadUnitData(bf, n);
        return d;
    };
    const auto anims = game::loadLaraAnims(bf);
    struct Case { int chapter; Gate gate; };
    const Case cases[] = {
        {0, {"l3_main_bedroom", 4525, Action::Use, {4520}}},             // wall switch -> house door
        {0, {"l3_bedroom_hall", 4363, Action::Push, {}}},                // training push block
        {0, {"l3_main_hall", 4129, Action::Enter, {4106}, 1}},           // prologue end: cinematic 010 -> chapter 1
        {1, {"l1_kraken_entrance", 748, Action::Use, {743}, -1, {{751, Action::Pull}, {753, Action::Weigh}}}}, // both locks
        {1, {"l1_baldrs_tomb", 219, Action::Step, {223}}},               // press plate -> door
        {1, {"l1_baldrs_tomb", 221, Action::Step, {45}}},                // press plate -> gate
        {1, {"l1_kraken_room", 498, Action::Use, {497}}},                // switch -> tomb door
        {1, {"l1_kraken_room", 30, Action::Use, {27}}},                  // switch -> sun-sphere platform
        {1, {"l1_ship_upper_hallway_a", 1531, Action::Enter, {1529}}},   // volume -> ship hall door
        {1, {"l1_ship_sinking_hall", 1074, Action::Enter, {}, 2}},       // teleport into l1_cine_ship_sinking -> 070
        {2, {"l2_puzzleroom", 2961, Action::Use, {2973}}},               // elevator switch -> elevator
        {2, {"l2_puzzleroom", 2982, Action::Pull, {}}},                  // shiva grapple pull
        {2, {"l2_flushtunnel", 2543, Action::Use, {2541}}},              // flush gate switch
        {2, {"l2_flushtunnel", 2564, Action::Enter, {}, 3, {{2542, Action::Enter}}}}, // BeatLevel plane, then the boat
        {3, {"l3_chapel", 4832, Action::Step, {4830}}},                  // press plate -> gate
        {3, {"l3_chapel", 4833, Action::Step, {4831, 4836}}},            // press plate -> gate, gold reward
        // shadow pillars (uber 4837): switches 4819 (section 1, c -> d -> a) and 4822 (section 2, b -> c -> d -> a) line
        // them up, then the plate lights fire 4818 and the pillars' check opens shadow door 4835
        {3, {"l3_chapel", 4834, Action::Step, {4835}, -1, {{4819, Action::Use}, {4819, Action::Use}, {4822, Action::Use},
                                                         {4822, Action::Use}, {4822, Action::Use}}}},
        {3, {"l3_chamberrockhall", 4617, Action::Use, {4528}}},          // electric switch -> secret door
        {3, {"l3_main_hall", 4128, Action::Enter, {}, 4}},               // 160_man_shot, 170_man_exit -> chapter 4
        {4, {"l4_ballcourt", 6502, Action::Use, {6505}}},                // pole lever -> door joint
        {4, {"l4_traprooms", 5884, Action::Push, {}}},                   // grapple push block
        // the Xibalba exit switch, the plane of 190_mex (6410 -> 6389), then the chapter end 5483
        {4, {"l4_ballcourt", 5468, Action::Enter, {}, 5, {{5469, Action::Use}, {6410, Action::Enter}}}},
        {5, {"l5_valaskjalf_entry", 7228, Action::Use, {7227}}},         // switch -> reward door
        {5, {"l5_valaskjalf", 6866, Action::Enter, {}, 6}},              // chapter end 6846
        {6, {"l6_ship_engine_hall", 7565, Action::Enter, {}, 7}},        // chapter end 1387
    };
    for (const auto& c : cases) {
        const auto r = runGate(bf, load, anims, c.gate, c.chapter);
        EXPECT_TRUE(r.pass) << c.gate.unit << " gate " << c.gate.trigger << " chapter " << r.chapter << " " << r.failure;
    }
}

// Chapter 6's ship hall doors (l6_ship_upper_hallway_a/b) have no script, so their volumes' door calls do nothing (the
// ISTYPE on the door fails); they stand open for the walk through the ship.
TEST(ProgressionReal, ChapterSixShipDoorsOpen) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    for (const auto& [unit, doors] : {std::pair{"l6_ship_upper_hallway_a", std::vector{7642, 7643}},
                                      std::pair{"l6_ship_upper_hallway_b", std::vector{7621}}}) {
        game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), unit, anims);
        w.script().setBigfile(&bf);
        w.tick({});
        for (const int32_t uid : doors) {
            const auto* d = w.script().find(uid);
            ASSERT_TRUE(d) << unit << " " << uid;
            EXPECT_EQ(d->state, 2) << unit << " door " << uid;
        }
    }
}

// Research: hex dump of an object DRM's ObjectBase section (`*` = relocated pointer), OU_UBER_DUMP=<object>.
TEST(ProgressionReal, UberDump) {
    const char* name = std::getenv("OU_UBER_DUMP");
    if (!gameDir() || !name) GTEST_SKIP();
    const cdc::BigfileSet bf(gameDir());
    const auto drm = cdc::parseDrm(*bf.read(std::string("pc-w\\") + name + ".drm"));
    const auto& ob = drm.sections[*formats::objectBaseSection(drm)];
    std::printf("size %zu rec %u ptr50 %d\n", ob.data.size(), cdc::internalPointer(ob, 0xC8).value_or(0), int(cdc::internalPointer(ob, 0x50).value_or(-1)));
    for (uint32_t o = 0; o + 4 <= ob.data.size(); o += 4) {
        uint32_t v;
        std::memcpy(&v, ob.data.data() + o, 4);
        const auto p = cdc::internalPointer(ob, o);
        if (o % 32 == 0) std::printf("\n%05X:", o);
        if (p) std::printf(" *%05X", *p);
        else std::printf(" %08X", v);
    }
    std::printf("\n");
}

// Research: a script object's fields after its first tick, OU_OBJ_FIELDS=<unit>:<uid>.
TEST(ProgressionReal, ObjectFields) {
    const char* spec = std::getenv("OU_OBJ_FIELDS");
    if (!gameDir() || !spec) GTEST_SKIP();
    const cdc::BigfileSet bf(gameDir());
    const std::string s(spec);
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), s.substr(0, s.find(':')), anims);
    w.script().setBigfile(&bf);
    w.tick({});
    const auto* o = w.script().find(std::stoi(s.substr(s.find(':') + 1)));
    ASSERT_TRUE(o);
    for (const auto& [k, v] : o->script->fields) std::printf("+%u %08X\n", k, v);
}

// l3_chapel shadow pillars (uber 4837, docs/PROGRESSION.md): wall switches turn pillar sections 0-3 through positions
// a -> b -> c -> d; the solver section 4 follows them by condition lists; lined up (all at a) it reaches "solved", whose
// event makes the pillars' script open the shadow door 4835.
TEST(ProgressionReal, ChapelShadowPillars) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l3_chapel", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 300; ++i) w.tick({}); // the pillars rise (command 4): sections at a, c, b, a
    const auto* u = w.uberState(4837);
    ASSERT_TRUE(u);
    ASSERT_EQ(u->size(), 6u);
    EXPECT_EQ(*u, (std::vector<uint32_t>{0, 2, 1, 0, 0, 1}));
    const auto use = [&](int32_t sw) {
        const auto* o = w.script().find(sw);
        place(w, o->pos, 0);
        game::Actions a;
        a.interact = true;
        for (int i = 0; i < 10; ++i) w.tick(a);
        for (int i = 0; i < 300; ++i) w.tick({});
    };
    const auto* door = w.script().find(4835);
    ASSERT_TRUE(door);
    const glm::mat4 closed = door->body;
    for (const int32_t sw : {4819, 4819}) use(sw); // section 1: c -> d -> a
    EXPECT_EQ((*u)[1], 0u);
    EXPECT_EQ((*u)[4], 2u); // sections 1 and 3 lined up: "left solved"
    for (const int32_t sw : {4822, 4822, 4822}) use(sw); // section 2: b -> c -> d -> a
    EXPECT_EQ(*u, (std::vector<uint32_t>{0, 0, 0, 0, 1, 1}));
    EXPECT_FALSE(moved(door->body, closed)); // the shadows only count in the fire's light
    place(w, w.script().find(4834)->pos + vec3(0, 0, 30), 0); // the plate lights fire 4818: its vt 55 checks the pillars
    for (int i = 0; i < 600; ++i) w.tick({});
    EXPECT_TRUE(door->state != 0 || moved(door->body, closed));
}

// Research: an object's animation list, OU_ANIM_NAMES=<object>.
TEST(ProgressionReal, AnimNames) {
    const char* name = std::getenv("OU_ANIM_NAMES");
    if (!gameDir() || !name) GTEST_SKIP();
    const cdc::BigfileSet bf(gameDir());
    for (const auto& e : anim::animList(cdc::parseDrm(*bf.read(std::string("pc-w\\") + name + ".drm")))) std::printf("%s %u\n", e.name.c_str(), e.animId);
}

// Survey (OU_ANIMGRAPH_SURVEY=1, docs/formats/ANIMGRAPH.md): per unit, its scripted objects running an anim state graph
// (Object::Graph): initial state, whether a clip poses them, state changes in 10 s idle.
TEST(ProgressionReal, AnimGraphSurvey) {
    if (!gameDir() || !std::getenv("OU_ANIMGRAPH_SURVEY")) GTEST_SKIP();
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    std::set<int32_t> seen;
    size_t objects = 0, graphs = 0, posed = 0, changed = 0;
    std::map<std::string, std::array<size_t, 4>> byName; // instances, with a graph, posed, changed
    for (const auto& unit : unitNames(bf)) {
        std::unique_ptr<game::World> w;
        try {
            w = std::make_unique<game::World>(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), unit, anims);
        } catch (const std::exception& e) { std::printf("%s: %s\n", unit.c_str(), e.what()); continue; }
        w->script().setBigfile(&bf);
        w->onCinematic = [](const std::string&, bool) {};
        w->tick({});
        std::map<int32_t, std::pair<int, bool>> start; // uid -> state, posed
        for (const auto& o : w->script().objects())
            if (o.graph && o.unit == unit) start[o.uid] = {o.graph->state, bool(w->script().pose(o))};
        for (int i = 0; i < 600; ++i) {
            w->tick({});
            for (const auto& o : w->script().objects())
                if (const auto it = start.find(o.uid); it != start.end() && !it->second.second && w->script().pose(o)) it->second.second = true;
        }
        for (const auto& o : w->script().objects()) {
            if (o.unit != unit || !seen.insert(o.uid).second || o.kind != script::Kind::Scripted) continue;
            ++objects;
            auto& n = byName[o.name];
            ++n[0];
            if (!o.graph || !o.graph->set->graph) continue; // without a graph only `animated` 1 poses it
            const auto& s = start[o.uid];
            const bool ch = o.graph->state != s.first;
            ++graphs, ++n[1], posed += s.second, n[2] += s.second, changed += ch, n[3] += ch;
            std::printf("%s %d %s initial %d state %d posed %d\n", unit.c_str(), o.uid, o.name.c_str(), o.graph->initial,
                        o.graph->state, int(s.second));
        }
    }
    for (const auto& [name, n] : byName)
        if (n[1]) std::printf("object %s instances %zu graph %zu posed %zu changed %zu\n", name.c_str(), n[0], n[1], n[2], n[3]);
    std::printf("scripted objects %zu, with an anim graph %zu, posed by a clip %zu, changed state idle %zu\n", objects, graphs, posed, changed);
}

// l4_traprooms seal 5701: with the seal raised (sticky +268) and the four trap rooms done (sticky int +240 = 4), plates
// 5717 / 5718 each set their sticky +124 and hear of it in vt 6 (sticky notification), which counts them on the seal
// (+304) and runs its vt 58: both held opens it (seal sticky +20 of the unit's globals, modifiable +64). Lara stands on
// one, the scepter weight 5883 is carried onto the other.
TEST(ProgressionReal, TraproomsSeal) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l4_traprooms", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 60; ++i) w.tick({});
    const auto* seal = w.script().find(5701);
    ASSERT_TRUE(seal && seal->script);
    seal->script->fields[268] = 1, seal->script->fields[240] = 4; // earlier progress: raised, trap rooms done
    const auto* weight = w.script().find(5883);
    const auto* a = w.script().find(5717);
    const auto* b = w.script().find(5718);
    ASSERT_TRUE(weight && a && b);
    game::Actions press;
    press.interact = true;
    place(w, weight->pos + weight->offset, 0);
    w.tick(press), w.tick({});
    place(w, b->pos - vec3(90, 0, -30), 0);
    w.tick({});
    w.tick(press), w.tick({});
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_EQ(b->script->fields[124], 1u) << "the weight holds plate 5718";
    EXPECT_EQ(seal->script->fields[304], 1u);
    EXPECT_NE(seal->script->fields[64], 0xFFFFFFFFu);
    const auto before = seal->script->fields;
    place(w, a->pos + vec3(0, 0, 30), 0);
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_EQ(a->script->fields[124], 1u) << "Lara holds plate 5717";
    EXPECT_EQ(seal->script->fields[304], 2u);
    EXPECT_NE(seal->script->fields, before) << "both held: vt 58 opens the seal";
}

// The ending (chapter 7 -> the epilogue): l7_yggdrasil_end's dispatcher 8360 counts the buttresses broken in the Natla
// fight (A from 320_hel_boss's end, B / C / D from the buttress mounts' vt 54), each revealed by a camera; C's reveal plays
// 330_hel_thralls, the last one runs vt 66 -> vt 67 "AllButtressesBroken_BreakGun" -> 340_hel_endgame; its end warps Lara
// to l7_monastery (instance 26 on 8138) and plays 342_hel_endgame, whose end makes the end game save (uisavemenu 6) and
// plays the credits movie (unlockablemovie 3): ou-game's ending. Played as in the game: no handler is called directly.
TEST(ProgressionReal, EndgameCredits) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l7_yggdrasil_end", anims);
    w.script().setBigfile(&bf);
    w.chapter = 7;
    std::vector<std::string> played;
    w.onCinematic = [&](const std::string& name, bool levelEnd) {
        played.push_back(name);
        EXPECT_FALSE(levelEnd) << name;
    };
    std::vector<std::string> movies;
    const auto run = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            w.tick({});
            for (auto& m : w.script().takeMovies()) movies.push_back(std::move(m));
        }
    };
    run(60);
    const uint32_t saves = w.autosaveRequests();
    // Buttress A: Lara enters the arena volume 8303, whose vt 72 plays 320_hel_boss (8261); its end breaks A (8360
    // vt 62). The fight's start and the pushed Thor towers lock the buttress mounts B-D (anim graph state 2:
    // modifiable 7 (1), then the towers' state 2 event's modifiable 4 (2)).
    const auto* arena = w.script().find(8303);
    ASSERT_TRUE(arena);
    place(w, arena->pos, 0);
    run(60 * 60);
    // The fight's start retracts the Thor towers 8398-8400 on their joint motors (command 0; trigger 14 at the bottom,
    // state 3). Lara pushes each back out along its prismatic joint until it enters [1074.6, 1137.8): trigger 14, state
    // 2, whose event ("block_pushed") locks its mount (modifiable 4 (2)).
    for (const int32_t tower : {8398, 8399, 8400}) {
        ASSERT_TRUE(w.uberState(tower));
        EXPECT_EQ((*w.uberState(tower))[0], 3u) << "tower " << tower << " retracted";
        pushUber(w, tower, 0, 12 * 60);
        EXPECT_EQ((*w.uberState(tower))[0], 2u) << "tower " << tower << " pushed out";
    }
    run(120);
    const auto graphState = [&](int32_t uid) {
        const auto* o = w.script().find(uid);
        return o && o->graph && o->graph->set->graph ? int(o->graph->set->graph->states[size_t(o->graph->state)].index) : -1;
    };
    // B, C, D: Lara uses each locked mount at its outer end (bone 2, 8041 units out from the intro on the trunk;
    // stimulus 1: transition 3 "break" -> state 3; vt 20 sends buttress_mount_broken and the mount's vt 54 tells
    // dispatcher 8360). Each reveal camera runs ~7 s.
    for (const int32_t mount : {8362, 8363, 8364}) {
        const auto* m = w.script().find(mount);
        ASSERT_TRUE(m && m->graph);
        EXPECT_EQ(graphState(mount), 2) << "mount " << mount << " locked";
        const auto points = script::Runtime::usePoints(*m);
        ASSERT_EQ(points.size(), 3u);
        EXPECT_GT(glm::length(glm::vec2(points[2] - m->pos)), 8000.0f) << "used at its end, not on the trunk";
        place(w, points[2], 0);
        game::Actions use;
        use.interact = true;
        w.tick(use);
        run(1200);
        EXPECT_EQ(graphState(mount), 3) << "mount " << mount << " broken";
    }
    EXPECT_EQ(played, (std::vector<std::string>{"320_hel_boss", "330_hel_thralls", "340_hel_endgame", "342_hel_endgame"}));
    EXPECT_EQ(w.currentUnit().data.name, "l7_monastery");
    EXPECT_GT(w.autosaveRequests(), saves) << "the end game save";
    EXPECT_EQ(movies, std::vector<std::string>{"credits"});
    for (const int32_t uid : {8263, 8138, 8360, 8401}) // their handlers run to the end (8401: uberobjectbase 3)
        for (const auto& [key, n] : w.script().failures()) EXPECT_NE(key.first, uid) << key.second;
}

// l1_kraken_room: switch 57 only turns tentacle gear 32 once the column (its section 12) is shot down: the bullet's
// trigger 15 (World::uberShot) drops the column; before that 57's use is the "attempt" reaction.
TEST(ProgressionReal, KrakenColumnShot) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l1_kraken_room", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 60; ++i) w.tick({});
    const auto* sw = w.script().find(57);
    ASSERT_TRUE(sw && w.uberState(32));
    EXPECT_TRUE(std::ranges::any_of(w.collision().dynamics, [](const auto& d) { return d.id == 32; })) << "bullets can hit the gear";
    const auto use = [&] {
        place(w, sw->pos, 0);
        game::Actions a;
        a.interact = true;
        for (int i = 0; i < 10; ++i) w.tick(a);
        for (int i = 0; i < 600; ++i) w.tick({});
    };
    const auto rest = *w.uberState(32);
    use();
    EXPECT_EQ(*w.uberState(32), rest) << "the column still stands: 57 only tries";
    w.uberShot(32);
    for (int i = 0; i < 60; ++i) w.tick({});
    EXPECT_EQ((*w.uberState(32))[12], 1u) << "the shot column fell (section 12: 0 -> 2 -> 1)";
    const auto fallen = *w.uberState(32);
    use();
    EXPECT_NE(*w.uberState(32), fallen) << "57 turns the gear";
}

// l4_staircase_upper hallway ramps 6045 (ca_rb_hallway_ramps_b): volume 6046 switches section 0's motor off and its
// body falls on the prismatic joint to the lower limit (-948), where the range watch [-979.8, -916.6) sends trigger 14
// (3 -> 2, the ramps tilt: section 1 follows by condition list); plane 6078 raises it on the motor (316 units/s) until
// [-31.6, 31.6) is entered again (4 -> 1). Triggers 14 / 15 come from the joint simulation, not timers.
TEST(ProgressionReal, HallwayRampsJoint) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l4_staircase_upper", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 60; ++i) w.tick({});
    ASSERT_TRUE(w.uberState(6045));
    const auto* ramp = w.uberJoint(6045, 0);
    ASSERT_TRUE(ramp);
    EXPECT_EQ((*w.uberState(6045))[0], 1u);
    EXPECT_EQ(ramp->x, 0.0f) << "the motor holds it at its upper limit";
    const auto enter = [&](int32_t uid) {
        const auto* t = w.script().find(uid);
        ASSERT_TRUE(t);
        place(w, t->pos - vec3(0, 0, script::kTestPointHeight), 0);
    };
    enter(6046);
    for (int i = 0; i < 600 && (*w.uberState(6045))[0] != 2; ++i) w.tick({});
    EXPECT_EQ((*w.uberState(6045))[0], 2u);
    EXPECT_NEAR(w.uberJoint(6045, 0)->x, -948.167f, 0.01f) << "fell to its lower limit";
    for (int i = 0; i < 120; ++i) w.tick({});
    EXPECT_EQ((*w.uberState(6045))[1], 2u) << "the ramps tilted";
    enter(6078);
    for (int i = 0; i < 600 && (*w.uberState(6045))[0] != 1; ++i) w.tick({});
    EXPECT_EQ((*w.uberState(6045))[0], 1u);
    EXPECT_GE(w.uberJoint(6045, 0)->x, -31.6056f) << "raised back into its top range";
}

// l4_ballcourt Xibalba exit switch 5469 reopens the temple (docs/formats/ANIMGRAPH.md): its vt 55 calls the temple
// reveal 5464's vt 54 "ReopenFromXibalba": modifiable 7 enters state 3 (BothSidesOpen), the 0x20000 stimulus starts
// transition 4 (TimerA) -> state 5, whose 300-frame timer plays TimerB -> 6 -> TimerB_Done -> 7.
TEST(ProgressionReal, TempleRevealReopens) {
    if (!gameDir()) GTEST_SKIP() << "OU_GAME_DIR not set";
    const cdc::BigfileSet bf(gameDir());
    const auto anims = game::loadLaraAnims(bf);
    game::World w(game::UnitLoader([&](const std::string& n) { return game::loadUnitData(bf, n); }), "l4_ballcourt", anims);
    w.script().setBigfile(&bf);
    w.onCinematic = [](const std::string&, bool) {};
    for (int i = 0; i < 60; ++i) w.tick({});
    const auto* temple = w.script().find(5464);
    const auto* sw = w.script().find(5469);
    ASSERT_TRUE(temple && temple->graph && temple->graph->set->graph && sw);
    EXPECT_EQ(temple->graph->state, 0);
    const auto closed = *w.script().pose(*temple);
    place(w, sw->pos, 0);
    game::Actions use;
    use.interact = true;
    w.tick(use);
    std::vector<int> states{temple->graph->state};
    for (int i = 0; i < 60 * 180 && states.back() != 7; ++i) {
        w.tick({});
        if (temple->graph->state != states.back()) states.push_back(temple->graph->state);
    }
    EXPECT_EQ(states, (std::vector<int>{0, 3, 5, 6, 7}));
    const auto open = w.script().pose(*temple);
    ASSERT_TRUE(open);
    EXPECT_TRUE(std::ranges::any_of(std::views::iota(size_t(0), open->size()), [&](size_t i) { return moved((*open)[i], closed[i]); }))
        << "the temple stands open";
}
