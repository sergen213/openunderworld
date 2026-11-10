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
    std::stringstream ss;
    ss << f.rdbuf();
    Autopilot p = parse(ss.str());
    p.out_ = std::move(out);
    return p;
}

Autopilot Autopilot::parse(std::string_view plan) {
    std::istringstream f{std::string(plan)};
    Autopilot p;
    std::string line;
    for (int no = 1; std::getline(f, line); ++no) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        Step s;
        s.line = no;
        if (!(ls >> s.op)) continue;
        for (std::string w; ls >> w;) s.args.push_back(w);
        static const std::vector<std::pair<std::string, size_t>> kOps = {{"go", 2}, {"engage", 1}, {"face", 1}, {"press", 0},
                                                                         {"hold", 2}, {"stick", 3}, {"run", 2}, {"push", 3}, {"wait", 1}, {"near", 1},
                                                                         {"ride", 2}, {"map", 1}, {"swim", 3}};
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
    if (why) failed_ = true, failure_ = "line " + std::to_string(steps_[step_].line) + " (" + steps_[step_].op + "): " + why, step_ = steps_.size();
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
                reported_ = true, std::fprintf(stderr, "ou-game: autopilot tick %llu engage %d (%s) at %.1f %.1f %.1f (target %.1f %.1f), Lara at %.1f %.1f %.1f\n",
                             (unsigned long long)tick, uid, o->name.c_str(), o->pos.x, o->pos.y, o->pos.z, to.x, to.y, lara.pos.x, lara.pos.y, lara.pos.z);
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
    } else if (s.op == "swim") { // swim X Y Z [R]: in water, the stick towards (X, Y), Jump / Drop held to rise / sink
        const glm::vec3 d = glm::vec3(std::stof(s.args[0]), std::stof(s.args[1]), std::stof(s.args[2])) - lara.pos;
        const float radius = s.args.size() > 3 ? std::stof(s.args[3]) : 150.0f, flat = glm::length(glm::vec2(d));
        if (glm::length(d) <= radius || t > 3 * kTimeout) {
            centre(), set(tick, "jump", 0, a), set(tick, "drop", 0, a);
            std::fprintf(stderr, "ou-game: autopilot tick %llu swim: state %s at %.1f %.1f %.1f\n", (unsigned long long)tick,
                         std::string(game::stateName(lara.state)).c_str(), lara.pos.x, lara.pos.y, lara.pos.z);
            next(tick, glm::length(d) <= radius ? nullptr : "timed out");
        } else {
            // The stick's share shrinks as the point comes to lie above / below her, so she does not circle it.
            if (flat > 30) steer(glm::vec2(d) / flat * std::clamp(flat / std::max(std::abs(d.z), 1.0f), 0.0f, 1.0f), false);
            else centre();
            set(tick, "jump", d.z > radius * 0.5f ? 1.0f : 0.0f, a), set(tick, "drop", d.z < -radius * 0.5f ? 1.0f : 0.0f, a);
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
    } else if (s.op == "stick") { // stick FORWARD RIGHT SECONDS [ACTION AT]: ACTION pressed AT seconds in (running jumps)
        if (t == 0) set(tick, "forward", std::stof(s.args[0]), a), set(tick, "right", std::stof(s.args[1]), a);
        if (s.args.size() >= 5 && t == ticks(s.args[4])) set(tick, s.args[3], 1, a);
        if (s.args.size() >= 5 && t == ticks(s.args[4]) + kPressTicks) set(tick, s.args[3], 0, a);
        if (t >= ticks(s.args[2])) {
            centre();
            if (s.args.size() >= 5) set(tick, s.args[3], 0, a);
            std::fprintf(stderr, "ou-game: autopilot tick %llu stick: state %s at %.1f %.1f %.1f\n", (unsigned long long)tick,
                         std::string(game::stateName(lara.state)).c_str(), lara.pos.x, lara.pos.y, lara.pos.z);
            next(tick, nullptr);
        }
    } else if (s.op == "run") { // run YAWDEG SECONDS [ACTION AT]: run along a world heading, ACTION pressed AT seconds in
        steer({std::cos(glm::radians(std::stof(s.args[0]))), std::sin(glm::radians(std::stof(s.args[0])))}, false);
        if (s.args.size() >= 4 && t == ticks(s.args[3])) set(tick, s.args[2], 1, a);
        if (s.args.size() >= 4 && t == ticks(s.args[3]) + kPressTicks) set(tick, s.args[2], 0, a);
        if (t >= ticks(s.args[1])) {
            centre();
            if (s.args.size() >= 4) set(tick, s.args[2], 0, a);
            std::fprintf(stderr, "ou-game: autopilot tick %llu run: state %s at %.1f %.1f %.1f\n", (unsigned long long)tick,
                         std::string(game::stateName(lara.state)).c_str(), lara.pos.x, lara.pos.y, lara.pos.z);
            next(tick, nullptr);
        }
    } else if (s.op == "push") { // push FORWARD RIGHT SECONDS [ACTION]: grip with ACTION (interact), then the raw stick
        const std::string action = s.args.size() > 3 ? s.args[3] : "interact";
        const uint64_t length = ticks(s.args[2]);
        if (t == 0) set(tick, action, 1, a);
        if (t == kPressTicks) set(tick, "forward", std::stof(s.args[0]), a), set(tick, "right", std::stof(s.args[1]), a);
        if (t >= length) {
            centre(), set(tick, action, 0, a);
            std::fprintf(stderr, "ou-game: autopilot tick %llu push: state %s at %.1f %.1f %.1f\n", (unsigned long long)tick,
                         std::string(game::stateName(lara.state)).c_str(), lara.pos.x, lara.pos.y, lara.pos.z);
            next(tick, nullptr);
        }
    } else if (s.op == "near") { // log the markup polylines within R of Lara (finding poles / ledges for a plan)
        const float r = std::stof(s.args[0]);
        for (const auto& m : w.collision().markup)
            if (std::ranges::any_of(m.points, [&](const glm::vec3& p) { return glm::distance(p, lara.pos) <= r; })) {
                std::string pts;
                for (const auto& p : m.points) pts += " (" + std::to_string(int(p.x)) + " " + std::to_string(int(p.y)) + " " + std::to_string(int(p.z)) + ")";
                std::fprintf(stderr, "ou-game: autopilot markup %d %s:%s\n", m.id, std::string(collision::markupTypeName(m.type)).c_str(), pts.c_str());
            }
        next(tick, nullptr);
    } else if (s.op == "map") { // map R [CELL] [UP]: log the floor round Lara as a character grid (finding a route for a plan)
        // One row per CELL (default 100) of y, north (+y) first, x growing to the right. A cell is the floor found under
        // UP (default 300) over her feet: '.' within 50 of her level, '1'..'9' / 'a'..'i' = that many hundreds above /
        // below (rounded), 'z' deeper, ' ' none within 2500; '#' = a wall within half a cell at 150 over that floor.
        const float r = std::stof(s.args[0]), cell = s.args.size() > 1 ? std::stof(s.args[1]) : 100.0f, up = s.args.size() > 2 ? std::stof(s.args[2]) : 300.0f;
        const int n = int(r / cell);
        std::fprintf(stderr, "ou-game: autopilot map centre %.0f %.0f %.0f cell %.0f (x %.0f..%.0f, y %.0f..%.0f)\n", lara.pos.x, lara.pos.y, lara.pos.z, cell,
                     lara.pos.x - n * cell, lara.pos.x + n * cell, lara.pos.y + n * cell, lara.pos.y - n * cell);
        for (int j = n; j >= -n; --j) {
            std::string row;
            for (int i = -n; i <= n; ++i) {
                const glm::vec3 from(lara.pos.x + i * cell, lara.pos.y + j * cell, lara.pos.z + up);
                const auto floor = w.collision().raycast(from, {0, 0, -1}, 2500 + up);
                char c = ' ';
                if (floor) {
                    const float d = floor->point.z - lara.pos.z;
                    const int h = int(std::lround(std::abs(d) / 100));
                    c = std::abs(d) < 50 ? '.' : d > 0 ? char('0' + std::min(h, 9)) : h > 9 ? 'z' : char('a' + h - 1);
                    for (const glm::vec3 dir : {glm::vec3(1, 0, 0), {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}})
                        if (w.collision().raycast(floor->point + glm::vec3(0, 0, 150), dir, cell / 2)) c = '#';
                }
                row += i == 0 && j == 0 ? '@' : c;
            }
            std::fprintf(stderr, "ou-game: autopilot map %7.0f %s\n", lara.pos.y + j * cell, row.c_str());
        }
        next(tick, nullptr);
    } else if (s.op == "ride") { // ride X Y [R] [THROTTLE] [SPEED] [BRAKESTEER]: steer the ridden bike to (X, Y) with the raw stick
        const game::Bike* b = w.ridden();
        const glm::vec2 to(std::stof(s.args[0]), std::stof(s.args[1]));
        const float radius = s.args.size() > 2 ? std::stof(s.args[2]) : 300.0f, throttle = s.args.size() > 3 ? std::stof(s.args[3]) : 1.0f;
        const float limit = s.args.size() > 4 ? std::stof(s.args[4]) : 1e9f;
        const glm::vec2 d = to - glm::vec2(lara.pos);
        if (t == 0) rideFrom_ = glm::vec2(lara.pos);
        const bool passed = glm::dot(d, to - rideFrom_) < 0; // rode past it wide of the radius
        if (!b || b->phase != game::BikePhase::Ride) {
            if (t > 300) centre(), next(tick, "not riding");
        } else if (glm::length(d) <= radius || passed) {
            centre();
            std::fprintf(stderr, "ou-game: autopilot tick %llu ride: at %.1f %.1f %.1f, %.0f units/s\n", (unsigned long long)tick, lara.pos.x,
                         lara.pos.y, lara.pos.z, b->speed);
            next(tick, nullptr);
        } else if (t > kTimeout) {
            centre(), next(tick, "timed out");
        } else if (t % 2 == 0) { // the bike's stick is raw: forward = throttle, right = steering (- = left)
            const float err = std::remainder(std::atan2(d.y, d.x) - lara.yaw, glm::two_pi<float>());
            const auto q = [](float v) { return std::round(v * 20) / 20; };
            // braking with the stick past half is the power slide (World::driveBike): BRAKESTEER 0.5 keeps the stick inside it
            const bool brake = b->speed > 1.25f * limit;
            const float lock = brake && s.args.size() > 5 ? std::stof(s.args[5]) : 1.0f;
            set(tick, "forward", brake ? -1.0f : b->speed > limit ? 0.0f : throttle, a), set(tick, "right", q(std::clamp(-2.5f * err, -lock, lock)), a);
        }
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
    if (out_.empty()) return; // parse(): a plan run in place
    std::ofstream f(out_);
    f << script_;
    std::fprintf(stderr, "ou-game: autopilot wrote %s (%zu bytes, %zu of %zu steps)\n", out_.string().c_str(), script_.size(),
                 step_ > steps_.size() ? steps_.size() : step_, steps_.size());
}

} // namespace ou::app
