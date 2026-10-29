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
    Weigh,  // pressure plate: Lara carries the nearest portable weight (*_pt_weight*) onto it and sets it down
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
                if (o.name.find("_pt_weight") != std::string::npos && o.name.find("break") == std::string::npos &&
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
    for (int i = 0; i < 600 && w.chapter == chapter; ++i) tick({});
    r.chapter = w.chapter;
    for (const int32_t id : targets) {
        const auto* o = w.script().find(id);
        const bool cine = o && o->name == "cinematic_object";
        const bool ok = cine ? activated.contains(id) : before.contains(id) && differs(before[id], snapshot(w, id));
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
        {4, {"l4_ballcourt", 5468, Action::Enter, {}, 5}},               // chapter end 5483
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
