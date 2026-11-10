#include "app/game/SelfTest.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ou::app {
using nlohmann::json;
using selftest::Condition;
using selftest::Measurements;
using selftest::Over;
using selftest::Tracker;
using selftest::Value;

namespace {
[[noreturn]] void bad(const std::string& where, const std::string& what) { throw std::runtime_error("self-test " + where + ": " + what); }

std::vector<std::string_view> split(std::string_view s, char by = '.') {
    std::vector<std::string_view> out;
    for (size_t at = 0;;) {
        const size_t dot = s.find(by, at);
        out.push_back(s.substr(at, dot == std::string_view::npos ? dot : dot - at));
        if (dot == std::string_view::npos) break;
        at = dot + 1;
    }
    return out;
}
std::optional<int32_t> integer(std::string_view s) {
    int32_t v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size() && !s.empty() ? std::optional(v) : std::nullopt;
}
Value component(glm::vec3 v, std::string_view axis) {
    return axis == "x" ? Value(double(v.x)) : axis == "y" ? Value(double(v.y)) : axis == "z" ? Value(double(v.z)) : Value();
}
bool axisName(std::string_view a) { return a == "x" || a == "y" || a == "z"; }
double degrees(float rad) { return double(glm::degrees(std::remainder(rad, glm::two_pi<float>()))); }

bool equal(const Value& v, const json& j) {
    if (const auto* b = std::get_if<bool>(&v)) return j.is_boolean() && j.get<bool>() == *b;
    if (const auto* d = std::get_if<double>(&v)) return j.is_number() && std::abs(j.get<double>() - *d) <= 1e-6 * std::max(1.0, std::abs(*d));
    if (const auto* s = std::get_if<std::string>(&v)) return j.is_string() && j.get<std::string>() == *s;
    return false;
}
std::vector<std::string> strings(const json& j, const std::string& where, const char* key) {
    if (!j.is_array() || j.empty()) bad(where, std::string("'") + key + "' wants a non-empty array of strings");
    std::vector<std::string> out;
    for (const auto& e : j) {
        if (!e.is_string()) bad(where, std::string("'") + key + "' wants strings");
        out.push_back(e.get<std::string>());
    }
    return out;
}
// "input" / "plan": a string of lines or an array of line strings.
std::string lines(const json& j, const std::string& where, const char* key) {
    if (j.is_string()) return j.get<std::string>();
    if (!j.is_array()) bad(where, std::string("'") + key + "' wants a string or an array of lines");
    std::string out;
    for (const auto& l : j) {
        if (!l.is_string()) bad(where, std::string("'") + key + "' wants an array of strings");
        out += l.get<std::string>() + "\n";
    }
    return out;
}
std::vector<Condition> conditions(const json& j, const std::string& where) {
    std::vector<Condition> out;
    if (j.is_object()) return {Condition::parse(j, where)};
    if (!j.is_array()) bad(where, "wants a condition object or an array of them");
    for (size_t i = 0; i < j.size(); ++i) out.push_back(Condition::parse(j[i], where + "[" + std::to_string(i) + "]"));
    return out;
}

bool grounded(game::State s) {
    using S = game::State;
    for (const S g : {S::Idle, S::Walk, S::Run, S::Stop, S::Turn, S::JumpPrep, S::Land, S::Crouch, S::Roll, S::Slide, S::Beam, S::Use, S::Block, S::WallHug})
        if (s == g) return true;
    return false;
}
bool airborne(game::State s) { return s == game::State::Jump || s == game::State::Fall || s == game::State::HangJump; }
bool swimming(game::State s) { return (s >= game::State::SwimSplash && s <= game::State::SwimClimbOut) || s == game::State::SwimDodge; }
bool roped(game::State s) {
    using S = game::State;
    return s == S::GrappleThrow || s == S::GrappleSwing || s == S::GrapplePull || s == S::GrappleRappel;
}

// Probe names with no id in them.
const std::set<std::string_view>& fixedProbes() {
    static const std::set<std::string_view> k = {
        "player.state", "player.clip", "player.clip_time", "player.pos.x", "player.pos.y", "player.pos.z", "player.unit_pos.x",
        "player.unit_pos.y", "player.unit_pos.z", "player.yaw", "player.pitch", "player.vel.x", "player.vel.y", "player.vel.z",
        "player.speed", "player.vspeed", "player.grounded", "player.airborne", "player.swimming", "player.hanging", "player.dead",
        "player.health", "player.health_max", "player.air", "player.adrenaline", "player.sprint_used", "player.sprint_left",
        "player.saving_grab", "player.ledge", "player.fall_height",
        "weapon.current", "weapon.secondary", "weapon.drawn", "weapon.clip", "weapon.reserve", "weapon.grenades", "weapon.shots_fired",
        "weapon.health_packs_used",
        "ik.feet", "ik.hands", "ik.look", "ik.pelvis", "ik.foot_planted.l", "ik.foot_planted.r", "ik.foot_error.l", "ik.foot_error.r",
        "ik.hand_before.l", "ik.hand_before.r", "ik.hand_error.l", "ik.hand_error.r", "ik.look_yaw", "ik.look_pitch", "ik.second_arm",
        "combat.has_target", "combat.locked", "combat.target", "combat.target_health", "combat.adrenaline_mode", "combat.headshot_window",
        "combat.time_scale", "combat.world_scale", "combat.manual_aim", "combat.headshot", "combat.headshot_ready", "combat.second_target",
        "grapple.attached", "grapple.state", "grapple.target", "grapple.offered", "grapple.rope", "grapple.rope_max", "grapple.kind",
        "grapple.bends", "grapple.on_wall", "grapple.rappel", "grapple.wall_run", "grapple.swinging", "grapple.pulling",
        "water.in", "water.under", "water.air",
        "bike.count", "bike.mounted", "bike.uid", "bike.phase", "bike.speed", "bike.airborne", "bike.wheels", "bike.steer", "bike.stunt",
        "bike.throttle", "bike.rpm", "bike.yaw", "bike.pitch", "bike.roll", "bike.brake_time", "bike.mounts", "bike.pos.x", "bike.pos.y",
        "bike.pos.z",
        "camera.yaw", "camera.pitch", "camera.distance", "camera.mode", "camera.spline", "camera.field", "camera.pos.x", "camera.pos.y",
        "camera.pos.z",
        "world.unit", "world.units", "world.chapter", "world.checkpoint", "world.checkpoint_unit", "world.ticks", "world.treasures",
        "world.relics", "world.health_packs", "world.streaming", "world.autosaves", "world.flashlight", "world.screen_fade",
        "portable.held", "enemies.count", "enemies.alive", "enemies.nearest", "enemies.nearest_uid"};
    return k;
}
} // namespace

// ---- values and conditions --------------------------------------------------------------------------
json selftest::toJson(const Value& v) {
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    if (const auto* d = std::get_if<double>(&v)) return std::round(*d * 1000.0) / 1000.0; // reports: 3 decimals
    if (const auto* s = std::get_if<std::string>(&v)) return *s;
    if (const auto* l = std::get_if<std::vector<std::string>>(&v)) return *l;
    return nullptr;
}

Condition Condition::parse(const json& j, const std::string& where) {
    if (!j.is_object()) bad(where, "a condition is an object");
    Condition c;
    c.expected = json::object();
    const auto number = [&](const char* key) {
        if (!j.at(key).is_number()) bad(where, std::string("'") + key + "' wants a number");
        c.expected[key] = j.at(key);
        return j.at(key).get<double>();
    };
    const auto scalar = [&](const json& v, const char* key) {
        if (!v.is_boolean() && !v.is_number() && !v.is_string()) bad(where, std::string("'") + key + "' wants a bool, number or string");
    };
    for (const auto& [key, value] : j.items()) {
        if (key == "probe") {
            if (!value.is_string()) bad(where, "'probe' wants a string");
            c.probe = value.get<std::string>();
        } else if (key == "visits" || key == "clips" || key == "calls") { // shorthands over the step's lists
            if (!c.probe.empty() || j.contains("probe")) bad(where, "'" + key + "' is its own probe");
            c.probe = key == "visits" ? "step.states" : key == "clips" ? "step.clips" : "step.calls";
            (key == "calls" ? c.has : c.seq) = strings(value, where, key.c_str());
            c.expected[key == "calls" ? "has" : "seq"] = value;
        } else if (key == "over") {
            static constexpr std::pair<std::string_view, Over> k[] = {{"end", Over::End}, {"max", Over::Max}, {"min", Over::Min}, {"any", Over::Any}, {"all", Over::All}};
            const auto it = std::ranges::find(k, value.is_string() ? value.get<std::string>() : std::string(), &std::pair<std::string_view, Over>::first);
            if (it == std::end(k)) bad(where, "'over' is one of end, max, min, any, all");
            c.over = it->second;
        } else if (key == "eq") { scalar(value, "eq"), c.eq = value, c.expected[key] = value;
        } else if (key == "ne") { scalar(value, "ne"), c.ne = value, c.expected[key] = value;
        } else if (key == "min") { c.min = number("min");
        } else if (key == "max") { c.max = number("max");
        } else if (key == "near") { c.near = number("near");
        } else if (key == "tol") { c.tol = number("tol");
        } else if (key == "in") {
            if (!value.is_array() || value.empty()) bad(where, "'in' wants a non-empty array");
            for (const auto& v : value) scalar(v, "in");
            c.in = value.get<std::vector<json>>(), c.expected[key] = value;
        } else if (key == "seq") { c.seq = strings(value, where, "seq"), c.expected[key] = value;
        } else if (key == "has") { c.has = strings(value, where, "has"), c.expected[key] = value;
        } else if (key != "note") {
            bad(where, "unknown key '" + key + "'");
        }
    }
    if (c.probe.empty()) bad(where, "no 'probe' (or visits / clips / calls)");
    if (!SelfTest::probeKnown(c.probe)) bad(where, "unknown probe '" + c.probe + "'");
    if (!c.eq && !c.ne && !c.min && !c.max && !c.near && !c.in && !c.seq && !c.has) bad(where, "probe '" + c.probe + "' has no operator");
    if (c.near && !j.contains("tol")) bad(where, "'near' wants a 'tol'");
    if (j.contains("tol") && (!c.near || c.tol < 0)) bad(where, "'tol' goes with 'near' and is >= 0");
    if (c.probe.starts_with("step.") && c.over != Over::End) bad(where, "step.* measurements cover the whole step: no 'over'");
    return c;
}

bool Condition::holds(const Value& v) const {
    if (std::holds_alternative<std::monostate>(v)) return false;
    if (eq && !equal(v, *eq)) return false;
    if (ne && equal(v, *ne)) return false;
    if (min || max || near) {
        const auto* d = std::get_if<double>(&v);
        if (!d || std::isnan(*d)) return false;
        if (min && *d < *min) return false;
        if (max && *d > *max) return false;
        if (near && std::abs(*d - *near) > tol) return false;
    }
    if (in && std::ranges::none_of(*in, [&](const json& j) { return equal(v, j); })) return false;
    if (seq) {
        const auto* l = std::get_if<std::vector<std::string>>(&v);
        if (!l) return false;
        size_t at = 0;
        for (const auto& e : *l)
            if (at < seq->size() && e == (*seq)[at]) ++at;
        if (at < seq->size()) return false;
    }
    if (has) {
        if (const auto* s = std::get_if<std::string>(&v)) {
            if (std::ranges::find(*has, *s) == has->end()) return false;
        } else if (const auto* l = std::get_if<std::vector<std::string>>(&v)) {
            for (const auto& h : *has)
                if (std::ranges::find(*l, h) == l->end()) return false;
        } else {
            return false;
        }
    }
    return true;
}

void Tracker::sample(const Value& v) {
    const auto* d = std::get_if<double>(&v);
    const auto* s = std::get_if<double>(&seen_);
    switch (c_.over) {
    case Over::End: break;
    case Over::Max:
        if (d && (!s || *d > *s)) seen_ = v;
        break;
    case Over::Min:
        if (d && (!s || *d < *s)) seen_ = v;
        break;
    case Over::Any:
        if (!any_) seen_ = v, any_ = c_.holds(v);
        break;
    case Over::All:
        if (all_) seen_ = v, all_ = c_.holds(v);
        break;
    }
    sampled_ = true;
}

Tracker::Result Tracker::finish(const Value& end) const {
    switch (c_.over) {
    case Over::End: return {c_.holds(end), end};
    case Over::Max:
    case Over::Min: return {sampled_ && c_.holds(seen_), seen_};
    case Over::Any: return {any_, seen_};
    case Over::All: return {sampled_ && all_, seen_};
    }
    return {};
}

// ---- measurements -----------------------------------------------------------------------------------
void Measurements::begin(glm::vec3 pos, std::string_view state, std::string_view clip, float health) {
    *this = {};
    start = end = pos, peakZ = minZ = pos.z;
    healthStart = healthEnd = healthMin = health;
    states.emplace_back(state), clips.emplace_back(clip);
}

void Measurements::tick(glm::vec3 pos, std::string_view state, std::string_view clip, bool air, float health) {
    const glm::vec3 d = pos - end;
    ++ticks;
    if (glm::length(d) > kTeleport || (!states.empty() && states.back() == "dead" && state != "dead")) {
        ++teleports, speed = vspeed = 0;
    } else {
        travelled += double(glm::length(d));
        speed = glm::length(glm::vec2(d)) * 60.0f, vspeed = d.z * 60.0f;
        peakSpeed = std::max(peakSpeed, speed), peakRise = std::max(peakRise, vspeed), peakFall = std::max(peakFall, -vspeed);
    }
    end = pos, peakZ = std::max(peakZ, pos.z), minZ = std::min(minZ, pos.z);
    airTicks += air;
    healthEnd = health, healthMin = std::min(healthMin, health);
    if (states.empty() || states.back() != state) states.emplace_back(state);
    if (clips.empty() || clips.back() != clip) clips.emplace_back(clip);
}

void Measurements::note(std::vector<std::string>& list, std::string entry) {
    if (std::ranges::find(list, entry) == list.end()) list.push_back(std::move(entry));
}

const std::vector<std::string_view>& selftest::measurementNames() {
    static const std::vector<std::string_view> k = {
        "start.x", "start.y", "start.z", "end.x", "end.y", "end.z", "moved", "distance", "horizontal", "height_gain", "peak_height", "drop",
        "peak_speed", "peak_rise_speed", "peak_fall_speed", "air_time", "air_ticks", "ticks", "seconds", "teleports", "health_start",
        "health_end", "health_min", "health_lost", "states", "clips", "calls", "events", "pickups"};
    return k;
}

Value Measurements::get(std::string_view n) const {
    if (n.starts_with("start.")) return component(start, n.substr(6));
    if (n.starts_with("end.")) return component(end, n.substr(4));
    if (n == "moved") return travelled;
    if (n == "distance") return double(glm::distance(start, end));
    if (n == "horizontal") return double(glm::distance(glm::vec2(start), glm::vec2(end)));
    if (n == "height_gain") return double(end.z - start.z);
    if (n == "peak_height") return double(peakZ - start.z);
    if (n == "drop") return double(start.z - minZ);
    if (n == "peak_speed") return double(peakSpeed);
    if (n == "peak_rise_speed") return double(peakRise);
    if (n == "peak_fall_speed") return double(peakFall);
    if (n == "air_time") return airTicks * game::kTickSeconds;
    if (n == "air_ticks") return double(airTicks);
    if (n == "ticks") return double(ticks);
    if (n == "seconds") return ticks * game::kTickSeconds;
    if (n == "teleports") return double(teleports);
    if (n == "health_start") return double(healthStart);
    if (n == "health_end") return double(healthEnd);
    if (n == "health_min") return double(healthMin);
    if (n == "health_lost") return double(healthStart - healthMin);
    if (n == "states") return states;
    if (n == "clips") return clips;
    if (n == "calls") return calls;
    if (n == "events") return events;
    if (n == "pickups") return pickups;
    return {};
}

json Measurements::json() const {
    nlohmann::json j = nlohmann::json::object();
    const auto r = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    j["start"] = {r(start.x), r(start.y), r(start.z)};
    j["end"] = {r(end.x), r(end.y), r(end.z)};
    for (const auto name : measurementNames())
        if (!name.starts_with("start.") && !name.starts_with("end.")) j[std::string(name)] = toJson(get(name));
    return j;
}

// ---- scenario ---------------------------------------------------------------------------------------
SelfTest SelfTest::load(const std::filesystem::path& file) {
    std::ifstream f(file);
    if (!f) throw std::runtime_error("self-test: cannot read " + file.string());
    json j;
    try {
        j = json::parse(f, nullptr, true, true); // comments allowed
    } catch (const std::exception& e) {
        throw std::runtime_error("self-test " + file.string() + ": " + e.what());
    }
    return parse(j, file.string());
}

SelfTest SelfTest::parse(const json& j, std::string file) {
    SelfTest t;
    t.file_ = std::move(file);
    const std::string top = "scenario";
    if (!j.is_object()) bad(top, "the scenario is an object");
    static const std::set<std::string> kTop = {"version", "name", "chapter", "walkthrough", "level", "spawn", "start_chapter", "load",
                                               "cinematics", "stop_on_fail", "fixtures", "steps", "note"};
    for (const auto& [key, value] : j.items())
        if (!kTop.contains(key)) bad(top, "unknown key '" + key + "'");
    if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1) bad(top, "'version' must be 1");
    if (!j.contains("name") || !j["name"].is_string() || j["name"].get<std::string>().empty()) bad(top, "'name' wants a string");
    t.name_ = j["name"].get<std::string>();
    const auto text = [&](const char* key) {
        if (!j.contains(key)) return std::string();
        if (!j[key].is_string()) bad(top, std::string("'") + key + "' wants a string");
        return j[key].get<std::string>();
    };
    t.chapterLabel_ = text("chapter"), t.walkthrough_ = text("walkthrough");
    t.entry_.level = text("level"), t.entry_.load = text("load");
    if (j.contains("start_chapter")) {
        if (!j["start_chapter"].is_number_integer()) bad(top, "'start_chapter' wants a chapter index (game::kChapters)");
        t.entry_.chapter = j["start_chapter"].get<int>();
        if (t.entry_.chapter < 0 || t.entry_.chapter >= int(game::kChapters.size()))
            bad(top, "'start_chapter' is 0.." + std::to_string(game::kChapters.size() - 1));
    }
    if (j.contains("spawn")) {
        const auto& s = j["spawn"];
        if (!s.is_array() || s.size() != 4 || !std::ranges::all_of(s, [](const json& v) { return v.is_number(); })) bad(top, "'spawn' wants [x, y, z, yawDeg]");
        t.entry_.spawn = std::array<float, 4>{s[0].get<float>(), s[1].get<float>(), s[2].get<float>(), s[3].get<float>()};
    }
    const int entries = int(!t.entry_.level.empty()) + int(t.entry_.chapter >= 0) + int(!t.entry_.load.empty());
    if (entries != 1) bad(top, "exactly one of 'level', 'start_chapter', 'load' enters the game");
    if (t.entry_.spawn && t.entry_.level.empty()) bad(top, "'spawn' goes with 'level'");
    for (const char* key : {"cinematics", "stop_on_fail"})
        if (j.contains(key) && !j[key].is_boolean()) bad(top, std::string("'") + key + "' wants true / false");
    t.entry_.cinematics = j.value("cinematics", false), t.stopOnFail_ = j.value("stop_on_fail", true);
    const auto fixtures = [&](const json& list, const std::string& where) {
        if (!list.is_array()) bad(where, "'fixtures' wants an array");
        std::vector<json> out;
        for (const auto& f : list) {
            static const std::set<std::string> kFixtures = {"weapon", "ammo", "health", "adrenaline", "give", "target", "kill", "tailoring", "teleport", "camera"};
            if (!f.is_object() || f.size() != 1 || !kFixtures.contains(f.begin().key()))
                bad(where, "a fixture is an object with one of weapon, ammo, health, adrenaline, give, target, kill, tailoring, teleport, camera");
            out.push_back(f);
        }
        return out;
    };
    if (j.contains("fixtures")) t.fixtures_ = fixtures(j["fixtures"], top);
    if (!j.contains("steps") || !j["steps"].is_array() || j["steps"].empty()) bad(top, "'steps' wants a non-empty array");
    std::set<std::string> ids;
    for (const auto& s : j["steps"]) {
        Step step;
        if (!s.is_object() || !s.contains("step") || !s["step"].is_string() || s["step"].get<std::string>().empty()) bad("step", "every step wants a 'step' id");
        step.id = s["step"].get<std::string>();
        const std::string where = "step '" + step.id + "'";
        if (!ids.insert(step.id).second) bad(where, "the id is used twice");
        static const std::set<std::string> kStep = {"step", "input", "plan", "duration", "until", "expect", "fixtures", "note"};
        for (const auto& [key, value] : s.items())
            if (!kStep.contains(key)) bad(where, "unknown key '" + key + "'");
        if (s.contains("input")) step.input = lines(s["input"], where, "input");
        if (s.contains("plan")) step.plan = lines(s["plan"], where, "plan");
        try {
            (void)game::InputScript::parse(step.input);
            if (!step.plan.empty()) (void)Autopilot::parse(step.plan);
        } catch (const std::exception& e) {
            bad(where, e.what());
        }
        if (!s.contains("duration") || !s["duration"].is_number() || !(s["duration"].get<double>() > 0)) bad(where, "'duration' wants seconds > 0");
        step.ticks = std::max<uint64_t>(1, uint64_t(std::llround(s["duration"].get<double>() / game::kTickSeconds)));
        if (s.contains("until")) {
            step.until = conditions(s["until"], where + " until");
            for (const auto& c : step.until)
                if (c.over != Over::End) bad(where + " until", "'over' is for expectations ('until' is tested every tick)");
        }
        if (!s.contains("expect")) bad(where, "no 'expect': a step must say what has to be true");
        step.expect = conditions(s["expect"], where + " expect");
        if (step.expect.empty()) bad(where, "'expect' is empty: a step must say what has to be true");
        if (s.contains("fixtures")) step.fixtures = fixtures(s["fixtures"], where);
        t.steps_.push_back(std::move(step));
    }
    t.results_.resize(t.steps_.size());
    return t;
}

bool SelfTest::probeKnown(std::string_view name) {
    if (fixedProbes().contains(name)) return true;
    const auto p = split(name);
    if (p[0] == "step") return std::ranges::find(selftest::measurementNames(), name.substr(std::min<size_t>(5, name.size()))) != selftest::measurementNames().end();
    if (p[0] == "pose") return p.size() == 4 && p[1] == "bone" && integer(p[2]) && axisName(p[3]);
    if (p.size() < 3 || !integer(p[1])) return false;
    const std::string_view f = p[2];
    if (p[0] == "object") {
        if (p.size() == 3) return f == "exists" || f == "name" || f == "graph" || f == "joint" || f == "hidden" || f == "dead" || f == "in";
        return p.size() == 4 && ((f == "pos" || f == "body") ? axisName(p[3]) : (f == "uber" || f == "uberjoint") && integer(p[3]));
    }
    if (p[0] == "enemy") {
        if (p.size() == 3) return f == "exists" || f == "health" || f == "mode" || f == "alive" || f == "distance" || f == "clip" || f == "type";
        return p.size() == 4 && f == "pos" && axisName(p[3]);
    }
    if (p[0] == "target") return p.size() == 3 && (f == "health" || f == "hits" || f == "alive");
    if (p[0] == "portable") return p.size() == 3 && f == "in";
    return false;
}

Value SelfTest::probe(game::World& w, std::string_view name) const {
    using game::State;
    const auto p = split(name);
    const auto& s = w.player().state();
    const auto num = [](auto v) { return Value(double(v)); };
    const auto str = [](std::string_view v) { return Value(std::string(v)); };
    if (p[0] == "step") return m_.get(name.substr(5));
    if (p[0] == "player") {
        const std::string_view f = p[1];
        if (f == "state") return str(game::stateName(s.state));
        if (f == "clip") return s.clip;
        if (f == "clip_time") return s.clipTime;
        if (f == "pos") return component(s.pos, p[2]);
        if (f == "unit_pos") return w.units().empty() ? component(s.pos, p[2]) : component(s.pos - w.currentUnit().offset, p[2]);
        if (f == "yaw") return degrees(s.yaw);
        if (f == "pitch") return degrees(s.pitch);
        if (f == "vel") return component(s.vel, p[2]);
        if (f == "speed") return num(m_.speed);
        if (f == "vspeed") return num(m_.vspeed);
        if (f == "grounded") return grounded(s.state);
        if (f == "airborne") return airborne(s.state);
        if (f == "swimming") return swimming(s.state);
        if (f == "hanging") return s.state == State::Hang || s.state == State::Shimmy;
        if (f == "dead") return s.state == State::Dead || w.health().dead();
        if (f == "health") return num(w.health().value());
        if (f == "health_max") return num(w.health().max());
        if (f == "air") return num(w.air());
        if (f == "adrenaline") return num(w.combat().adrenaline);
        if (f == "sprint_used") return num(s.sprintUsed);
        if (f == "sprint_left") return num(w.player().tuning().sprintSeconds - s.sprintUsed);
        if (f == "saving_grab") return num(s.savingGrab);
        if (f == "ledge") return num(s.ledge >= 0 && size_t(s.ledge) < w.collision().markup.size() ? int(w.collision().markup[size_t(s.ledge)].id) : -1);
        if (f == "fall_height") return num(s.fallHeight);
        return {};
    }
    if (p[0] == "weapon") {
        const auto& c = w.combat();
        const auto i = size_t(c.weapon);
        const std::string_view f = p[1];
        if (f == "current") return str(game::weaponName(c.weapon));
        if (f == "secondary") return str(game::weaponName(c.secondary));
        if (f == "drawn") return c.drawn;
        if (f == "clip") return num(c.clip[i][0] + c.clip[i][1]);
        if (f == "reserve") return num(c.reserve[i]);
        if (f == "grenades") return num(w.grenades);
        if (f == "shots_fired") return num(c.shotsFired);
        if (f == "health_packs_used") return num(c.healthPacksUsed);
        return {};
    }
    if (p[0] == "ik") { // the limb locks / head look-at of the last tick (game::IkReport)
        const game::IkReport& k = w.ik();
        const std::string_view f = p[1];
        const size_t side = p.size() > 2 && p[2] == "r" ? 1 : 0;
        if (f == "feet") return k.feet;
        if (f == "hands") return k.hands;
        if (f == "look") return k.look;
        if (f == "pelvis") return num(k.plant.pelvis);
        if (f == "foot_planted") return bool(k.plant.planted[side]);
        if (f == "foot_error") return k.feet && k.plant.planted[side] ? num(k.plant.error[side]) : Value();
        if (f == "hand_before") return k.hands ? num(k.handBefore[side]) : Value();
        if (f == "hand_error") return k.hands ? num(k.handAfter[side]) : Value();
        if (f == "look_yaw") return k.look ? degrees(k.lookAngles.x) : Value();
        if (f == "look_pitch") return k.look ? degrees(k.lookAngles.y) : Value();
        if (f == "second_arm") return degrees(k.secondArm);
        return {};
    }
    if (p[0] == "pose") { // pose.bone.<segment>.x|y|z: the bone's origin in the world
        const auto b = w.boneWorld(int(std::strtol(std::string(p[2]).c_str(), nullptr, 10)));
        return b ? component(*b, p[3]) : Value();
    }
    if (p[0] == "combat") {
        const auto& c = w.combat();
        const std::string_view f = p[1];
        if (f == "has_target") return c.target != nullptr;
        if (f == "locked") return c.locked && c.target != nullptr;
        if (f == "target") { // an enemy's intro uid; a "target" fixture: -(its index + 1); none 0
            for (const auto& e : w.enemies().all())
                if (static_cast<const game::Damageable*>(e.get()) == c.target) return num(e->uid());
            for (size_t i = 0; i < dummies_.size(); ++i)
                if (dummies_[i].get() == c.target) return num(-int(i) - 1);
            return num(0);
        }
        if (f == "target_health") return c.target ? num(c.target->health()) : Value();
        if (f == "adrenaline_mode") return c.adrenalineMode;
        if (f == "headshot_window") return c.slowMo > 0;
        if (f == "time_scale") return num(c.timeScale);
        if (f == "world_scale") return num(c.worldScale);
        if (f == "manual_aim") return c.manualAim;
        if (f == "headshot") return c.headShot >= 0;
        if (f == "headshot_ready") return c.headShotReady;
        if (f == "second_target") return c.second != nullptr;
        return {};
    }
    if (p[0] == "grapple") {
        const std::string_view f = p[1];
        const bool on = roped(s.state);
        if (f == "attached") return on;
        if (f == "state") return str(on ? game::stateName(s.state) : "none");
        if (f == "target") return num(on ? s.grappleUid : 0);
        if (f == "offered") { // the target a press would throw at now
            const auto t = w.grappleTarget();
            return num(t ? t->uid : 0);
        }
        if (f == "rope") return on ? num(s.ropeLength) : Value();
        if (f == "rope_max") return on ? num(w.player().maxRope()) : Value();
        if (f == "kind") {
            static constexpr std::string_view k[] = {"ceiling", "wall", "post", "mounted"};
            return on ? str(k[size_t(s.grappleKind) & 3]) : Value();
        }
        if (f == "bends") return num(on ? s.ropeBends : 0);
        if (f == "on_wall" || f == "rappel") return s.state == State::GrappleRappel;
        if (f == "wall_run") // feet on the wall running along it: the ground-to-wall-run clips and the sideways rappel clips
            return s.state == State::GrappleRappel && (s.clip.starts_with("ng_ground_to_wallrun") || s.clip.starts_with("grapple_rappel_left") ||
                                                       s.clip.starts_with("grapple_rappel_right"));
        if (f == "swinging") return s.state == State::GrappleSwing;
        if (f == "pulling") return s.state == State::GrapplePull;
        return {};
    }
    if (p[0] == "water") {
        if (p[1] == "in") return swimming(s.state) || s.water >= 0;
        if (p[1] == "under") return w.submerged();
        if (p[1] == "air") return num(w.air());
        return {};
    }
    if (p[0] == "bike") {
        const std::string_view f = p[1];
        if (f == "count") return num(w.bikes().size());
        if (f == "mounts") return num(w.bikeMounts);
        const game::Bike* b = w.ridden();
        if (f == "mounted") return b != nullptr;
        if (!b) return {};
        if (f == "uid") return num(b->uid);
        if (f == "phase") return str(game::bikePhaseName(b->phase));
        if (f == "speed") return num(b->speed);
        if (f == "airborne") return b->air;
        if (f == "wheels") { // wheels touching the ground as of the last physics step
            const auto st = w.physics().vehicle(b->uid);
            return st ? num(std::popcount(st->contacts)) : Value();
        }
        if (f == "steer") return degrees(b->steer);
        if (f == "stunt") {
            static constexpr std::string_view k[] = {"none", "burnout", "powerslide", "donut"};
            return str(k[size_t(b->stunt) & 3]);
        }
        if (f == "throttle") return num(b->throttle);
        if (f == "rpm") return num(b->rpm);
        if (f == "yaw") return degrees(b->yaw);
        if (f == "pitch") return degrees(b->pitch);
        if (f == "roll") return degrees(b->roll);
        if (f == "brake_time") return num(b->brakeT);
        if (f == "pos") return component(s.pos, p[2]); // Lara rides it: her root
        return {};
    }
    if (p[0] == "camera") {
        const auto& c = w.camera();
        const std::string_view f = p[1];
        if (f == "yaw") return degrees(c.yaw);
        if (f == "pitch") return degrees(c.pitch);
        if (f == "distance") return num(w.cameraDistance());
        if (f == "mode") return num(w.cameraMode());
        if (f == "spline") return w.splineCameraActive();
        if (f == "field") return w.fieldCamera();
        if (f == "pos") return component(c.position, p[2]);
        return {};
    }
    if (p[0] == "world") {
        const std::string_view f = p[1];
        if (f == "unit") return w.units().empty() ? Value() : Value(w.currentUnit().data.name);
        if (f == "units") {
            std::vector<std::string> names;
            for (const auto& u : w.units()) names.push_back(u.data.name);
            return names;
        }
        if (f == "chapter") return num(w.chapter);
        if (f == "checkpoint") return num(w.checkpoint() ? w.checkpoint()->id : 0);
        if (f == "checkpoint_unit") return w.checkpoint() ? Value(w.checkpoint()->unit) : Value(std::string());
        if (f == "ticks") return num(w.ticks());
        if (f == "treasures") return num(w.found[size_t(game::PickupKind::Treasure)]);
        if (f == "relics") return num(w.found[size_t(game::PickupKind::Relic)]);
        if (f == "health_packs") return num(w.found[size_t(game::PickupKind::HealthPack)]);
        if (f == "streaming") return w.streaming();
        if (f == "autosaves") return num(w.autosaveRequests());
        if (f == "flashlight") return w.flashlight;
        if (f == "screen_fade") return num(w.screenFade());
        return {};
    }
    if (name == "portable.held") return num(w.heldPortable());
    if (p[0] == "enemies") {
        int alive = 0, nearestUid = 0;
        float nearest = -1;
        for (const auto& e : w.enemies().all()) {
            if (!e->alive()) continue;
            ++alive;
            if (const float d = glm::distance(e->state().pos, s.pos); nearest < 0 || d < nearest) nearest = d, nearestUid = e->uid();
        }
        if (p[1] == "count") return num(w.enemies().all().size());
        if (p[1] == "alive") return num(alive);
        if (p[1] == "nearest") return nearest < 0 ? Value() : num(nearest);
        if (p[1] == "nearest_uid") return num(nearestUid);
        return {};
    }
    const auto id = p.size() >= 3 ? integer(p[1]) : std::nullopt;
    if (!id) return {};
    const std::string_view f = p[2];
    if (p[0] == "portable") return num(w.insertedIn(*id));
    if (p[0] == "target") {
        if (*id < 0 || size_t(*id) >= dummies_.size()) return {};
        const auto& d = *dummies_[size_t(*id)];
        return f == "health" ? num(d.hp) : f == "hits" ? num(d.hits) : Value(d.alive());
    }
    if (p[0] == "enemy") {
        const auto& all = w.enemies().all();
        const auto it = std::ranges::find_if(all, [&](const auto& e) { return e->uid() == *id; });
        if (f == "exists") return it != all.end();
        if (it == all.end()) return {};
        const auto& e = **it;
        if (f == "health") return num(e.state().health);
        if (f == "alive") return e.alive();
        if (f == "mode") return str(game::ai::modeName(e.state().mode));
        if (f == "clip") return e.state().clip;
        if (f == "type") return e.type().name;
        if (f == "distance") return num(glm::distance(e.state().pos, s.pos));
        if (f == "pos") return component(e.state().pos, p[3]);
        return {};
    }
    if (p[0] == "object") {
        const auto* o = w.script().find(*id);
        if (f == "exists") return o != nullptr;
        if (!o) return {};
        if (f == "name") return o->name;
        if (f == "hidden") return o->hidden;
        if (f == "dead") return o->dead;
        if (f == "in") return num(w.insertedIn(*id));
        if (f == "pos") return component(o->pos + o->offset, p[3]);
        if (f == "body") return component(glm::vec3(o->body[3]), p[3]); // a door's / slider's own motion from rest
        if (f == "graph") return o->graph && o->graph->set->graph ? num(o->graph->set->graph->states[size_t(o->graph->state)].index) : Value();
        if (f == "joint") return o->joint ? num(o->joint->position) : Value();
        const auto n = integer(p[3]);
        if (f == "uber" && n) { // section state; -1 while it is in a transition
            const auto* st = w.uberState(*id);
            if (!st || *n < 0 || size_t(*n) >= st->size()) return {};
            return num((*st)[size_t(*n)] == 0xFFFFFFFFu ? -1 : int((*st)[size_t(*n)]));
        }
        if (f == "uberjoint" && n) {
            const auto* jt = w.uberJoint(*id, *n);
            return jt ? num(jt->x) : Value();
        }
    }
    return {};
}

// ---- running ----------------------------------------------------------------------------------------
void SelfTest::applyFixture(game::World& w, const json& f, const std::string& step) {
    const std::string key = f.begin().key();
    const json& v = f.begin().value();
    const std::string where = (step.empty() ? std::string("scenario") : "step '" + step + "'") + " fixture '" + key + "'";
    const auto vec = [&](const json& a, size_t n) {
        if (!a.is_array() || a.size() != n || !std::ranges::all_of(a, [](const json& e) { return e.is_number(); })) bad(where, "wants " + std::to_string(n) + " numbers");
        return a.get<std::vector<float>>();
    };
    const auto weapon = [&](const json& n) {
        const auto x = n.is_string() ? game::weaponByName(n.get<std::string>()) : std::nullopt;
        if (!x) bad(where, "wants a weapon name (pistols, uzi, shotgun, speargun, thorhammer, rifle, tranquilizer)");
        return *x;
    };
    const glm::vec3 offset = w.units().empty() ? glm::vec3(0) : w.currentUnit().offset;
    if (key == "weapon") {
        const auto x = weapon(v);
        w.giveWeapon(x, w.maxReserve(x));
    } else if (key == "ammo") { // {"weapon": name, "clip": rounds per gun, "reserve": spare rounds}
        if (!v.is_object()) bad(where, "wants {weapon, clip, reserve}");
        const auto name = std::string(game::weaponName(weapon(v.value("weapon", json()))));
        json c = w.serializeCombat();
        if (v.contains("clip")) c["guns"][name]["clip"] = {v["clip"].get<int>(), v["clip"].get<int>()};
        if (v.contains("reserve")) c["guns"][name]["reserve"] = v["reserve"].get<int>();
        w.restoreCombat(c);
    } else if (key == "health") {
        if (!v.is_number() || !(v.get<float>() > 0)) bad(where, "wants health points > 0");
        w.setHealth(std::min(v.get<float>(), w.health().max() * w.health().tuning().overhealMax));
    } else if (key == "adrenaline") {
        if (!v.is_number()) bad(where, "wants the meter 0..1");
        json c = w.serializeCombat();
        c["adrenaline"] = std::clamp(v.get<float>(), 0.0f, 1.0f);
        w.restoreCombat(c);
    } else if (key == "give") { // a carried object (g_belt, g_gauntlet_*, wp_thorhammer, ...)
        if (!v.is_string()) bad(where, "wants an object name");
        auto objects = w.script().playerObjects();
        objects.insert(v.get<std::string>());
        w.script().setPlayerObjects(std::move(objects));
    } else if (key == "target") { // a standing dummy (game::DummyTarget) at [x, y, z] of the current unit, or {pos, health}
        auto& d = *dummies_.emplace_back(std::make_unique<game::DummyTarget>());
        const auto xyz = vec(v.is_object() ? v.value("pos", json()) : v, 3);
        d.pos = glm::vec3(xyz[0], xyz[1], xyz[2]) + offset;
        if (v.is_object() && v.contains("health")) d.hp = v["health"].get<float>();
        w.targets.push_back(&d);
    } else if (key == "kill") {
        w.kill();
    } else if (key == "tailoring") {
        if (!v.is_number_integer() || v.get<int>() < 0 || v.get<int>() > 2) bad(where, "wants a PLAYER TAILORING level 0..2");
        w.setTailoring(game::Tailoring::preset(v.get<int>()));
    } else if (key == "teleport" || key == "camera") {
        // teleport [x, y, z, yawDeg]: Lara standing there (current unit's frame), the camera behind her;
        // camera [yawDeg, pitchDeg]: the orbit camera turned. Health is kept (World::restore alone would refill it).
        const auto a = vec(v, key == "teleport" ? 4 : 2);
        const float health = w.health().value();
        game::PlayerState st = w.player().state();
        float yaw = glm::radians(a[0]), pitch = w.camera().pitch;
        if (key == "teleport") {
            st = {};
            st.pos = glm::vec3(a[0], a[1], a[2]) + offset, st.yaw = yaw = glm::radians(a[3]);
            st.clip = "ng_idle", st.fallStartZ = st.pos.z;
            pitch = game::CameraState{}.pitch;
        } else {
            pitch = glm::radians(a[1]);
        }
        w.restore(st, yaw, pitch);
        w.setHealth(health);
    }
    fixturesUsed_.push_back({{"step", step.empty() ? json(nullptr) : json(step)}, {"fixture", f}});
    std::fprintf(stderr, "ou-game: self-test fixture %s%s%s\n", f.dump().c_str(), step.empty() ? "" : " in step ", step.c_str());
}

void SelfTest::attach(game::World& w) {
    w.script().onCall = [this, prev = w.script().onCall](int32_t from, int32_t to, int16_t vt) {
        if (prev) prev(from, to, vt);
        m_.note(m_.calls, std::to_string(to) + ":" + std::to_string(vt));
    };
    for (const auto& f : fixtures_) applyFixture(w, f, {});
    if (!trace) return;
    // --self-test-trace: what a scenario can aim at in the loaded units (world frame), for writing steps.
    for (const auto& u : w.units()) {
        for (const auto& c : u.data.checkpoints)
            std::fprintf(stderr, "ou-game: self-test %s checkpoint %d at %.0f %.0f %.0f\n", u.data.name.c_str(), c.id, c.pos.x + u.offset.x,
                         c.pos.y + u.offset.y, c.pos.z + u.offset.z);
        for (const auto& g : u.data.grapples)
            std::fprintf(stderr, "ou-game: self-test %s grapple target %d kind %d at %.0f %.0f %.0f\n", u.data.name.c_str(), g.uid, int(g.kind),
                         g.pos.x + u.offset.x, g.pos.y + u.offset.y, g.pos.z + u.offset.z);
        for (const auto& v : u.data.vehicles)
            std::fprintf(stderr, "ou-game: self-test %s bike %d at %.0f %.0f %.0f\n", u.data.name.c_str(), v.uid, v.pos.x + u.offset.x,
                         v.pos.y + u.offset.y, v.pos.z + u.offset.z);
        for (const auto& p : u.data.pickups)
            std::fprintf(stderr, "ou-game: self-test %s pickup %s %d\n", u.data.name.c_str(), game::pickupKindName(p.kind), p.id);
    }
}

void SelfTest::begin(game::World& w) {
    const Step& s = steps_[cur_];
    for (const auto& f : s.fixtures) applyFixture(w, f, s.id);
    const auto& st = w.player().state();
    m_.begin(st.pos, game::stateName(st.state), st.clip, w.health().value());
    script_ = game::InputScript::parse(s.input);
    pilot_.reset();
    if (!s.plan.empty()) pilot_ = Autopilot::parse(s.plan);
    trackers_.clear();
    for (const auto& c : s.expect) trackers_.emplace_back(c);
    stepTick_ = 0, started_ = true;
    std::fprintf(stderr, "ou-game: self-test step '%s' starts at tick %llu: %s at %.1f %.1f %.1f\n", s.id.c_str(), (unsigned long long)w.ticks(),
                 std::string(game::stateName(st.state)).c_str(), st.pos.x, st.pos.y, st.pos.z);
}

game::Actions SelfTest::actions(game::World& w) {
    if (done()) return {};
    if (!started_) begin(w);
    game::Actions a = script_.at(stepTick_);
    if (pilot_) { // the plan steers and presses; "input" lines add to it (a held button, a look rate)
        const game::Actions p = pilot_->step(stepTick_, w);
        if (a.move == game::vec2(0)) a.move = p.move;
        if (a.look == game::vec2(0)) a.look = p.look;
        a.walk |= p.walk, a.jump |= p.jump, a.drop |= p.drop, a.interact |= p.interact, a.draw |= p.draw, a.fire |= p.fire, a.aim |= p.aim;
        a.switchTarget |= p.switchTarget, a.weapon |= p.weapon, a.heal |= p.heal, a.grapple |= p.grapple, a.sprint |= p.sprint;
        a.light |= p.light, a.grenade |= p.grenade, a.recenter |= p.recenter, a.fieldCamera |= p.fieldCamera;
    }
    a.save = a.load = a.pause = a.pda = false; // the scenario owns the run: no quicksaves or menus from its input
    return a;
}

void SelfTest::observe(game::World& w) {
    if (done() || !started_) return;
    const Step& s = steps_[cur_];
    const auto& st = w.player().state();
    ++stepTick_, ++totalTicks_;
    m_.tick(st.pos, game::stateName(st.state), st.clip, airborne(st.state), w.health().value());
    for (const auto& e : w.script().events())
        m_.note(m_.events, std::string(script::eventName(e.kind)) + ":" + std::to_string(e.source) + ">" + std::to_string(e.target));
    for (const auto& pk : w.pickedUp()) m_.note(m_.pickups, std::string(game::pickupKindName(pk.kind)) + ":" + std::to_string(pk.id));
    for (auto& t : trackers_)
        if (t.condition().over != Over::End) t.sample(probe(w, t.condition().probe));
    if (trace)
        std::fprintf(stderr, "ou-game: self-test %s t %llu %s %s pos %.1f %.1f %.1f yaw %.1f speed %.0f vz %.0f hp %.0f\n", s.id.c_str(),
                     (unsigned long long)stepTick_, std::string(game::stateName(st.state)).c_str(), st.clip.c_str(), st.pos.x, st.pos.y, st.pos.z,
                     glm::degrees(st.yaw), m_.speed, m_.vspeed, w.health().value());
    const bool planOver = pilot_ && pilot_->done();
    if (!s.until.empty() && std::ranges::all_of(s.until, [&](const Condition& c) { return c.holds(probe(w, c.probe)); }))
        close(w, "until", true, {});
    else if (planOver && pilot_->failed())
        close(w, "plan", false, "the plan failed at " + pilot_->failure());
    else if (planOver && s.until.empty())
        close(w, "plan", true, {});
    else if (stepTick_ >= s.ticks) {
        if (!s.until.empty()) close(w, "timeout", false, "'until' did not hold within " + std::to_string(s.ticks) + " ticks");
        else if (pilot_) close(w, "timeout", false, "the plan did not finish within " + std::to_string(s.ticks) + " ticks");
        else close(w, "duration", true, {});
    }
}

void SelfTest::paused() {
    if (done() || !started_) return;
    if (++stepTick_ >= steps_[cur_].ticks + 600) { // a menu the scenario cannot leave: give up on the whole run
        for (size_t i = cur_; i < steps_.size(); ++i) results_[i].detail = "not run: the game stayed paused in step '" + steps_[cur_].id + "'";
        cur_ = steps_.size();
    }
}

void SelfTest::close(game::World& w, const std::string& endedBy, bool endedOk, const std::string& why) {
    const Step& s = steps_[cur_];
    StepResult& r = results_[cur_];
    r.ran = true, r.endedBy = endedBy, r.m = m_;
    int failed = 0;
    std::string first;
    for (const auto& t : trackers_) {
        const auto& c = t.condition();
        const auto res = t.finish(probe(w, c.probe));
        static constexpr std::string_view kOver[] = {"end", "max", "min", "any", "all"};
        json e = {{"probe", c.probe}, {"expected", c.expected}, {"actual", selftest::toJson(res.actual)}, {"passed", res.passed}};
        if (c.over != Over::End) e["over"] = kOver[size_t(c.over)];
        if (!res.passed && !failed++)
            first = c.probe + (c.over != Over::End ? " (over " + std::string(kOver[size_t(c.over)]) + ")" : "") + ": expected " + c.expected.dump() +
                    ", actual " + selftest::toJson(res.actual).dump();
        r.expectations.push_back(std::move(e));
    }
    for (const char* name : {"player.state", "player.clip", "player.health", "player.air", "player.yaw", "world.unit", "world.checkpoint", "grapple.state",
                             "grapple.target", "grapple.rope", "bike.mounted", "bike.speed", "bike.wheels", "weapon.current", "combat.target",
                             "enemies.alive", "portable.held"})
        r.end[name] = selftest::toJson(probe(w, name));
    r.passed = endedOk && failed == 0;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%zu of %zu expectations held after %llu ticks (ended by %s)", trackers_.size() - size_t(failed), trackers_.size(),
                  (unsigned long long)stepTick_, endedBy.c_str());
    r.detail = buf;
    if (!endedOk) r.detail += "; " + why;
    if (failed) r.detail += "; " + first;
    std::fprintf(stderr, "ou-game: self-test step '%s' %s: %s\n", s.id.c_str(), r.passed ? "passed" : "FAILED", r.detail.c_str());
    simHash_ = w.stateHash();
    started_ = false;
    if (!r.passed && stopOnFail_) {
        for (size_t i = cur_ + 1; i < steps_.size(); ++i) results_[i].detail = "not run: step '" + s.id + "' failed";
        cur_ = steps_.size();
    } else {
        ++cur_;
    }
}

bool SelfTest::passed() const {
    return done() && std::ranges::all_of(results_, [](const StepResult& r) { return r.ran && r.passed; });
}

json SelfTest::report() const {
    json steps = json::array();
    for (size_t i = 0; i < steps_.size(); ++i) {
        const auto& r = results_[i];
        json s = {{"step", steps_[i].id}, {"passed", r.ran && r.passed}, {"detail", r.detail}};
        if (!r.ran) {
            s["skipped"] = true;
        } else {
            s["ended_by"] = r.endedBy, s["measurements"] = r.m.json(), s["end"] = r.end, s["expectations"] = r.expectations;
        }
        steps.push_back(std::move(s));
    }
    json entry = json::object();
    if (!entry_.level.empty()) entry["level"] = entry_.level;
    if (entry_.spawn) entry["spawn"] = *entry_.spawn;
    if (entry_.chapter >= 0) entry["start_chapter"] = entry_.chapter;
    if (!entry_.load.empty()) entry["load"] = entry_.load;
    char hash[24];
    std::snprintf(hash, sizeof hash, "%016llx", (unsigned long long)simHash_);
    json j = {{"version", 1}, {"game", "Tomb Raider: Underworld"}, {"scenario", name_}, {"file", file_}, {"entry", entry},
              {"fixtures", fixturesUsed_}, {"steps", steps}, {"ticks", totalTicks_}, {"sim", hash}, {"passed", passed()}};
    if (!chapterLabel_.empty()) j["chapter"] = chapterLabel_;
    if (!walkthrough_.empty()) j["walkthrough"] = walkthrough_;
    return j;
}

std::string SelfTest::summary() const {
    std::string out;
    size_t ok = 0;
    for (size_t i = 0; i < steps_.size(); ++i) {
        const auto& r = results_[i];
        ok += r.ran && r.passed;
        out += std::string(!r.ran ? "SKIP " : r.passed ? "PASS " : "FAIL ") + steps_[i].id + ": " + r.detail + "\n";
    }
    out += "self-test " + name_ + ": " + (passed() ? "PASSED" : "FAILED") + " (" + std::to_string(ok) + " of " + std::to_string(steps_.size()) + " steps)\n";
    return out;
}

} // namespace ou::app
