#include "app/game/Autopilot.hpp"

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ou::app {
namespace {
constexpr uint64_t kTimeout = 20 * 60; // ticks
constexpr int kPressTicks = 6;
uint64_t ticks(const std::string& seconds) { return uint64_t(std::llround(std::stod(seconds) / game::kTickSeconds)); }
} // namespace

Autopilot Autopilot::load(const std::filesystem::path& plan, std::filesystem::path out) {
    std::ifstream f(plan);
    if (!f) throw std::runtime_error("cannot read autopilot plan " + plan.string());
    Autopilot p;
    p.out_ = std::move(out);
    std::string line;
    for (int no = 1; std::getline(f, line); ++no) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        Step s;
        s.line = no;
        if (!(ls >> s.op)) continue;
        for (std::string w; ls >> w;) s.args.push_back(w);
        static const std::vector<std::pair<std::string, size_t>> kOps = {{"go", 2}, {"engage", 1}, {"face", 1}, {"press", 0},
                                                                         {"hold", 2}, {"stick", 3}, {"wait", 1}, {"near", 1}};
        const auto it = std::ranges::find(kOps, s.op, &std::pair<std::string, size_t>::first);
        if (it == kOps.end() || s.args.size() < it->second)
            throw std::runtime_error("autopilot plan line " + std::to_string(no) + ": unknown step or missing arguments");
        p.steps_.push_back(std::move(s));
    }
    return p;
}

// Records the action's new value as a script line and returns the actions the script gives at `tick`, so the plan
// sees exactly what the replay will (values round-trip through their text).
void Autopilot::set(uint64_t tick, const std::string& action, float value, game::Actions& a) {
    char text[32];
    std::snprintf(text, sizeof text, "%.2f", value);
    value = std::strtof(text, nullptr);
    if (const auto it = held_.find(action); it != held_.end() ? it->second == value : value == 0) return;
    held_[action] = value;
    char line[96];
    std::snprintf(line, sizeof line, "%.4f %s %s\n", double(tick) * game::kTickSeconds, action.c_str(), text);
    script_ += line;
    a = game::InputScript::parse(script_).at(tick);
}

void Autopilot::next(uint64_t tick, const char* why) {
    if (why) std::fprintf(stderr, "ou-game: autopilot plan line %d (%s) failed at tick %llu: %s\n", steps_[step_].line,
                          steps_[step_].op.c_str(), (unsigned long long)tick, why);
    if (why) step_ = steps_.size();
    else ++step_;
    started_ = reported_ = false;
}

game::Actions Autopilot::step(uint64_t tick, game::World& w) {
    game::Actions a = script_.empty() ? game::Actions{} : game::InputScript::parse(script_).at(tick);
    if (done()) return a;
    const Step& s = steps_[step_];
    if (!started_) started_ = true, stepStart_ = tick;
    const uint64_t t = tick - stepStart_;
    const auto& lara = w.player().state();
    const auto has = [&](const char* word) { return std::ranges::find(s.args, word) != s.args.end(); };
    const auto centre = [&] {
        set(tick, "forward", 0, a), set(tick, "right", 0, a), set(tick, "walk", 0, a);
    };
    // Camera-relative stick towards a world heading (World::tick: move = right * x + fwd * y, by the camera's yaw);
    // re-aimed every 4 ticks in 0.05 steps so the script stays short.
    const auto steer = [&](glm::vec2 dir, bool walk) {
        if (t % 4 != 0) return;
        const float cy = w.camera().yaw;
        const glm::vec2 fwd(std::cos(cy), std::sin(cy)), right(std::sin(cy), -std::cos(cy));
        const auto q = [](float v) { return std::round(v * 20) / 20; };
        set(tick, "forward", q(glm::dot(dir, fwd)), a), set(tick, "right", q(glm::dot(dir, right)), a);
        set(tick, "walk", walk ? 1.0f : 0.0f, a);
    };
    if (s.op == "go" || s.op == "engage") {
        glm::vec2 to(0);
        int32_t uid = 0;
        size_t walkArg = 2;
        if (s.op == "go") {
            to = {std::stof(s.args[0]), std::stof(s.args[1])};
        } else {
            uid = std::stoi(s.args[0]);
            if (s.args.size() >= 3 && s.args[1] != "walk") {
                to = {std::stof(s.args[1]), std::stof(s.args[2])};
            } else if (const auto* o = w.script().find(uid)) {
                to = glm::vec2(o->pos); // the nearest of its engagement records, where it has them
                float best = -1;
                for (const glm::vec3& c : w.engagementCentres(uid))
                    if (const float d = glm::distance(glm::vec2(c), glm::vec2(lara.pos)); best < 0 || d < best) best = d, to = glm::vec2(c);
            } else {
                if (t > 120) next(tick, "no such object"); // the unit's objects appear on its first tick
                return a;
            }
        }
        if (uid && !reported_)
            if (const auto* o = w.script().find(uid))
                reported_ = true, std::fprintf(stderr, "ou-game: autopilot tick %llu engage %d (%s) at %.1f %.1f %.1f, Lara at %.1f %.1f %.1f\n",
                             (unsigned long long)tick, uid, o->name.c_str(), o->pos.x, o->pos.y, o->pos.z, lara.pos.x, lara.pos.y, lara.pos.z);
        const float radius = s.op == "go" && s.args.size() > walkArg && s.args[walkArg] != "walk" ? std::stof(s.args[walkArg]) : s.op == "go" ? 40.0f : 30.0f;
        const glm::vec2 d = to - glm::vec2(lara.pos);
        const float dist = glm::length(d);
        if (uid && w.engaged().uid == uid) {
            centre();
            std::fprintf(stderr, "ou-game: autopilot tick %llu engages %d at %.1f %.1f %.1f yaw %.1f\n", (unsigned long long)tick, uid,
                         lara.pos.x, lara.pos.y, lara.pos.z, glm::degrees(lara.yaw));
            next(tick, nullptr);
        } else if (dist <= radius) {
            centre();
            if (uid) next(tick, "reached the point without engaging");
            else next(tick, nullptr);
        } else if (t > kTimeout) {
            centre();
            next(tick, "timed out");
        } else {
            steer(d / dist, has("walk") || dist < 250);
        }
    } else if (s.op == "face") {
        const float yaw = glm::radians(std::stof(s.args[0]));
        if (std::abs(std::remainder(lara.yaw - yaw, glm::two_pi<float>())) < glm::radians(8.0f)) centre(), next(tick, nullptr);
        else if (t > kTimeout) centre(), next(tick, "timed out");
        else steer({std::cos(yaw), std::sin(yaw)}, true);
    } else if (s.op == "press" || s.op == "hold") {
        const std::string action = s.args.empty() ? "interact" : s.args[0];
        const uint64_t length = s.op == "press" ? kPressTicks : ticks(s.args[1]);
        if (t == 0) set(tick, action, 1, a);
        else if (t == length) set(tick, action, 0, a);
        else if (t >= length + (s.op == "press" ? kPressTicks : 0)) {
            std::fprintf(stderr, "ou-game: autopilot tick %llu %s %s: state %s, holding %d\n", (unsigned long long)tick, s.op.c_str(),
                         action.c_str(), std::string(game::stateName(lara.state)).c_str(), w.heldPortable());
            next(tick, nullptr);
        }
    } else if (s.op == "stick") {
        if (t == 0) set(tick, "forward", std::stof(s.args[0]), a), set(tick, "right", std::stof(s.args[1]), a);
        else if (t >= ticks(s.args[2])) centre(), next(tick, nullptr);
    } else if (s.op == "near") { // log the markup polylines within R of Lara (finding poles / ledges for a plan)
        const float r = std::stof(s.args[0]);
        for (const auto& m : w.collision().markup)
            if (std::ranges::any_of(m.points, [&](const glm::vec3& p) { return glm::distance(p, lara.pos) <= r; })) {
                std::string pts;
                for (const auto& p : m.points) pts += " (" + std::to_string(int(p.x)) + " " + std::to_string(int(p.y)) + " " + std::to_string(int(p.z)) + ")";
                std::fprintf(stderr, "ou-game: autopilot markup %d %s:%s\n", m.id, std::string(collision::markupTypeName(m.type)).c_str(), pts.c_str());
            }
        next(tick, nullptr);
    } else if (s.op == "wait") {
        if (t >= ticks(s.args[0])) next(tick, nullptr);
    }
    if (done()) finish(tick + 1);
    return a;
}

void Autopilot::finish(uint64_t tick) {
    if (written_) return;
    written_ = true;
    game::Actions a;
    for (auto& [action, value] : std::map(held_))
        if (value != 0) set(tick, action, 0, a);
    std::ofstream f(out_);
    f << script_;
    std::fprintf(stderr, "ou-game: autopilot wrote %s (%zu bytes, %zu of %zu steps)\n", out_.string().c_str(), script_.size(),
                 step_ > steps_.size() ? steps_.size() : step_, steps_.size());
}

} // namespace ou::app
