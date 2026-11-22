#include "app/game/Autopilot.hpp"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
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
                                                                         {"ride", 2}, {"map", 1}, {"swim", 3}, {"fight", 0}};
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
    // A step ending on a release the stick filter holds a tick (centre(): StickRateFilter, tru.exe 0x483520) ends once it
    // has landed, as it did on its own tick without the filter; the next step then starts with her stopped.
    if (!why && releaseUntil_ > tick + 1 && !finishAt_) {
        finishAt_ = tick + 1;
        return;
    }
    if (why) std::fprintf(stderr, "ou-game: autopilot plan line %d (%s) failed at tick %llu: %s\n", steps_[step_].line,
                          steps_[step_].op.c_str(), (unsigned long long)tick, why);
    if (why) failed_ = true, failure_ = "line " + std::to_string(steps_[step_].line) + " (" + steps_[step_].op + "): " + why, step_ = steps_.size();
    else ++step_;
    started_ = reported_ = false;
    runLag_ = -1, aimPhase_ = -1, pressSettled_ = false, settleWait_ = 0;
}

game::Actions Autopilot::step(uint64_t tick, game::World& w) {
    game::Actions a = script_.empty() ? game::Actions{} : game::InputScript::parse(script_).at(tick);
    if (done()) return a;
    if (finishAt_ && tick >= finishAt_) { // the finished step's release has landed: the next step starts on the next tick
        finishAt_ = 0;
        next(tick, nullptr);
        return a;
    }
    const Step& s = steps_[step_];
    if (!started_) started_ = true, stepStart_ = tick;
    const uint64_t t = tick - stepStart_;
    const auto& lara = w.player().state();
    // Her motion this tick; the stick lands a tick late through StickRateFilter (tru.exe 0x483520) when it steps by more
    // than 0.2, so `go` / `engage` aim from where she will be a tick on (pos + this tick's motion).
    // A unit switch re-expresses the world around the new unit (World::streamAround): her position jumps by the origin
    // change, so a plan's points (written in the unit frame of the step) are shifted along with it.
    const std::string& unitNow = w.currentUnit().data.name;
    glm::vec2 jump = hasLastPos_ ? glm::vec2(lara.pos) - lastPos_ : glm::vec2(0);
    if (hasLastPos_ && unitNow != lastUnit_ && glm::length(jump) > 200.0f) rebase_ += jump, jump = glm::vec2(0);
    lastUnit_ = unitNow;
    const glm::vec2 motion = jump;
    const float groundStep = glm::length(motion); // units this tick
    const float yawStep = hasLastPos_ ? std::remainder(lara.yaw - lastYaw_, glm::two_pi<float>()) : 0.0f; // turn this tick
    lastPos_ = glm::vec2(lara.pos), lastYaw_ = lara.yaw, hasLastPos_ = true;
    const auto has = [&](const char* word) { return std::ranges::find(s.args, word) != s.args.end(); };
    // A walk change made with a stick step over 0.2 waits a tick: the step lands a tick late (StickRateFilter), and
    // the walk button would otherwise act on the old stick (releasing it a tick early starts a run along the old
    // heading, then a wide turn).
    if (walkDue_ && tick >= walkDue_->first) set(tick, "walk", walkDue_->second, a), walkDue_.reset();
    const auto stickTo = [&](float fwd, float right, float walk) {
        const auto held = [&](const char* k) { const auto it = held_.find(k); return it == held_.end() ? 0.0f : it->second; };
        const bool big = glm::length(glm::vec2(right - held("right"), fwd - held("forward"))) > 0.2f;
        set(tick, "forward", fwd, a), set(tick, "right", right, a);
        if (big && walk != held("walk")) walkDue_ = std::pair{tick + 1, walk};
        else walkDue_.reset(), set(tick, "walk", walk, a);
        return big;
    };
    // A release held a tick by the filter would be overwritten by the next step's first aim before it ever landed (she
    // would never stop); the next aim waits until the release has been the stick for a tick, as without the filter.
    const auto centre = [&] {
        if (stickTo(0, 0, 0)) releaseUntil_ = tick + 2;
    };
    // Camera-relative stick towards a world heading (World::tick: move = right * x + fwd * y, by World::stickYaw);
    // re-aimed every 4 ticks in 0.05 steps so the script stays short. Returns -1 when it did not aim this tick, else 1
    // when the aim lands a tick late (a step over 0.2), 0 when at once.
    // Re-aims follow the first aim's landing tick: an aim stepping over 0.2 lands a tick late, the small re-aims after it
    // at once, so they keep their 4-tick phase to her motion.
    const auto steer = [&](glm::vec2 dir, bool walk) -> int {
        if (tick < releaseUntil_) return -1;
        if (aimPhase_ >= 0 && tick != releaseUntil_ && (int64_t(t) <= aimPhase_ || (int64_t(t) - aimPhase_) % 4 != 0)) return -1;
        const float cy = w.stickYaw();
        const glm::vec2 fwd(std::cos(cy), std::sin(cy)), right(std::sin(cy), -std::cos(cy));
        const auto q = [](float v) { return std::round(v * 20) / 20; };
        const bool late = stickTo(q(glm::dot(dir, fwd)), q(glm::dot(dir, right)), walk ? 1.0f : 0.0f);
        if (aimPhase_ < 0 || tick == releaseUntil_) aimPhase_ = int64_t(t) + (late ? 1 : 0);
        return late ? 1 : 0;
    };
    if (s.op == "go" || s.op == "engage") {
        glm::vec2 to(0);
        int32_t uid = 0;
        size_t walkArg = 2;
        if (s.op == "go") {
            to = glm::vec2(std::stof(s.args[0]), std::stof(s.args[1])) + rebase_;
        } else {
            uid = std::stoi(s.args[0]);
            if (const auto spot = w.bikeMountSpot(uid)) { // a parked motorcycle: to where Interact mounts it
                to = glm::vec2(*spot);
                if (glm::distance(to, glm::vec2(lara.pos)) <= 40.0f) {
                    centre();
                    std::fprintf(stderr, "ou-game: autopilot tick %llu at bike %d's mount spot %.1f %.1f\n", (unsigned long long)tick, uid, to.x, to.y);
                    next(tick, nullptr);
                    return a;
                }
            } else if (s.args.size() >= 3 && s.args[1] != "walk") {
                to = {std::stof(s.args[1]), std::stof(s.args[2])};
            } else if (const auto* o = w.script().find(uid)) {
                to = glm::vec2(o->pos); // the nearest of its engagement records, where it has them
                float best = -1;
                const auto nearest = [&](int kind) {
                    for (const glm::vec3& c : w.engagementCentres(uid, kind))
                        if (const float d = glm::distance(glm::vec2(c), glm::vec2(lara.pos)); best < 0 || d < best) best = d, to = glm::vec2(c);
                };
                // Carrying a portable only records of kinds 3, 5 and 7 admit her (World::engaged): the l5_valgrind cog
                // wheel's push handle (kind 1, section 0) lies nearer the third power block's way in than its niche.
                if (w.heldPortable())
                    for (const int kind : {3, 5, 7}) nearest(kind);
                if (best < 0) nearest(0);
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
        const glm::vec2 d = to - (glm::vec2(lara.pos) + motion); // from a tick on: the stick's change lands then
        const float dist = glm::length(d);
        if (uid && w.engaged().uid == uid) {
            centre();
            std::fprintf(stderr, "ou-game: autopilot tick %llu engages %d at %.1f %.1f %.1f yaw %.1f\n", (unsigned long long)tick, uid,
                         lara.pos.x, lara.pos.y, lara.pos.z, glm::degrees(lara.yaw));
            next(tick, nullptr);
        } else if (dist <= radius) {
            // Another coarse point (both radii >= 80) straight on: within 30 deg of both her heading and her motion (when
            // she moves). Then keep the stick and re-aim at once: without the filter the release lasted one tick and she
            // ran on; held (StickRateFilter, tru.exe 0x483520) and then left to land it would stop her for two. Fine
            // placements (either point) and turns still stop her first (ch05_container_deck's 1600 -> 900 leg ends at a
            // radius-60 point; ch01_bedroom_new_game's point 3 slides her along a wall toward point 4 while she faces 66
            // deg off it, and running on put her into the corner).
            bool chained = false;
            const Step* nx = step_ + 1 < steps_.size() ? &steps_[step_ + 1] : nullptr;
            if (!uid && radius >= 80.0f && nx && nx->op == "go" && nx->args.size() >= 3 && nx->args[2] != "walk" &&
                std::stof(nx->args[2]) >= 80.0f) {
                const glm::vec2 nextTo = glm::vec2(std::stof(nx->args[0]), std::stof(nx->args[1])) + rebase_;
                const glm::vec2 dn = nextTo - glm::vec2(lara.pos);
                const float toNext = std::atan2(dn.y, dn.x);
                const auto within = [&](float a) { return std::abs(std::remainder(toNext - a, glm::two_pi<float>())) < glm::radians(30.0f); };
                chained = glm::length(dn) > 1.0f && within(lara.yaw) && (groundStep <= 2.0f || within(std::atan2(motion.y, motion.x)));
            }
            if (!chained) centre();
            if (uid) next(tick, "reached the point without engaging");
            else next(tick, nullptr);
        } else if (t > kTimeout) {
            centre();
            next(tick, "timed out");
        } else {
            // In water she turns toward the stick at 1.755 rad/s whatever her speed (Player::swim, swimTurn), so at full
            // speed she circles ~770 units wide and can be carried past the point, over a sill she then cannot swim back
            // across (l7_arctic_sea's niches); a player eases off the stick until she faces it, as this does.
            // On land a run turns ~1.7 deg a tick at ~21 units a tick (a circle ~720 units across), so a point behind her
            // and nearer than that is orbited for good; a player lets go until she stops, then pushes towards it (she
            // turns on the spot from a stand). The stick's one-tick hold (StickRateFilter, tru.exe 0x483520) can leave her
            // still running when a step starts (the last step's release lands a tick late).
            const float err = std::abs(std::remainder(std::atan2(d.y, d.x) - lara.yaw, glm::two_pi<float>()));
            float ease = 1;
            if (lara.water >= 0) ease = std::clamp(std::cos(err), 0.25f, 1.0f);
            if (lara.water < 0 && err > glm::half_pi<float>() && groundStep > 10.0f) centre();
            else steer(d / dist * ease, has("walk") || dist < 250);
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
        // The release lands a tick late (StickRateFilter, tru.exe 0x483520) and she turns on meanwhile: stop once the
        // yaw a tick on is within 8 deg while the current one is within 12, else (a slowing turn) on the current one.
        const float yaw = glm::radians(std::stof(s.args[0]));
        const float now = std::abs(std::remainder(lara.yaw - yaw, glm::two_pi<float>()));
        const float soon = std::abs(std::remainder(lara.yaw + yawStep - yaw, glm::two_pi<float>()));
        if (now < glm::radians(8.0f) || (soon < glm::radians(8.0f) && now < glm::radians(12.0f))) centre(), next(tick, nullptr);
        else if (t > kTimeout) centre(), next(tick, "timed out");
        else steer({std::cos(yaw), std::sin(yaw)}, true);
    } else if (s.op == "press" || s.op == "hold") {
        const std::string action = s.args.empty() ? "interact" : s.args[0];
        const uint64_t length = s.op == "press" ? kPressTicks : ticks(s.args[1]);
        // A press made while she is still moving or stopping goes nowhere in tru.exe's chart (run-stop S57 has no
        // Interact edge; only the idle S40's ActionFlag 32 -> S741 takes it): wait until she has stopped, then press.
        if (t == 0 && !pressSettled_) {
            const bool moving = lara.state == game::State::Walk || lara.state == game::State::Run || lara.state == game::State::Stop ||
                                lara.state == game::State::Turn;
            if (moving && settleWait_++ < kTimeout) {
                stepStart_ = tick + 1; // t stays 0 until she stops
                return a;
            }
            pressSettled_ = true;
        }
        if (t == 0) set(tick, action, 1, a);
        else if (t == length) set(tick, action, 0, a);
        else if (t >= length + (s.op == "press" ? kPressTicks : 0)) {
            std::fprintf(stderr, "ou-game: autopilot tick %llu %s %s: state %s, holding %d\n", (unsigned long long)tick, s.op.c_str(),
                         action.c_str(), std::string(game::stateName(lara.state)).c_str(), w.heldPortable());
            next(tick, nullptr);
        }
    } else if (s.op == "stick") { // stick FORWARD RIGHT SECONDS [ACTION AT]: ACTION pressed AT seconds in (running jumps)
        // AT counts from when the stick lands (a step over 0.2 lands a tick late: StickRateFilter, tru.exe 0x483520)
        if (t == 0) {
            const auto held = [&](const char* k) { const auto it = held_.find(k); return it == held_.end() ? 0.0f : it->second; };
            const float f = std::stof(s.args[0]), r = std::stof(s.args[1]);
            runLag_ = glm::length(glm::vec2(r - held("right"), f - held("forward"))) > 0.2f ? 1 : 0;
            set(tick, "forward", f, a), set(tick, "right", r, a);
        }
        if (s.args.size() >= 5 && runLag_ >= 0 && t == ticks(s.args[4]) + uint64_t(runLag_)) set(tick, s.args[3], 1, a);
        if (s.args.size() >= 5 && runLag_ >= 0 && t == ticks(s.args[4]) + uint64_t(runLag_) + kPressTicks) set(tick, s.args[3], 0, a);
        if (t >= ticks(s.args[2]) + uint64_t(std::max(runLag_, 0))) {
            centre();
            if (s.args.size() >= 5) set(tick, s.args[3], 0, a);
            std::fprintf(stderr, "ou-game: autopilot tick %llu stick: state %s at %.1f %.1f %.1f\n", (unsigned long long)tick,
                         std::string(game::stateName(lara.state)).c_str(), lara.pos.x, lara.pos.y, lara.pos.z);
            next(tick, nullptr);
        }
    } else if (s.op == "run") { // run YAWDEG SECONDS [ACTION AT]: run along a world heading, ACTION pressed AT seconds in
        // AT counts from when the stick lands: a first aim stepping over 0.2 lands a tick late (StickRateFilter), and an
        // action pressed with it would read the old stick (a jump on the press tick goes the way she faced).
        const int aimed = steer({std::cos(glm::radians(std::stof(s.args[0]))), std::sin(glm::radians(std::stof(s.args[0])))}, false);
        if (aimed >= 0 && runLag_ < 0) runLag_ = int(t) + aimed;
        if (s.args.size() >= 4 && runLag_ >= 0 && t == ticks(s.args[3]) + uint64_t(runLag_)) set(tick, s.args[2], 1, a);
        if (s.args.size() >= 4 && runLag_ >= 0 && t == ticks(s.args[3]) + uint64_t(runLag_) + kPressTicks) set(tick, s.args[2], 0, a);
        if (runLag_ >= 0 && t >= ticks(s.args[1]) + uint64_t(runLag_)) {
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
        if (t == kPressTicks) { // the push counts from when the stick lands (a step over 0.2 lands a tick late)
            const auto held = [&](const char* k) { const auto it = held_.find(k); return it == held_.end() ? 0.0f : it->second; };
            const float f = std::stof(s.args[0]), r = std::stof(s.args[1]);
            runLag_ = glm::length(glm::vec2(r - held("right"), f - held("forward"))) > 0.2f ? 1 : 0;
            set(tick, "forward", f, a), set(tick, "right", r, a);
        }
        if (t >= length + uint64_t(std::max(runLag_, 0))) {
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
    } else if (s.op == "fight") { // fight [R] [SECONDS] [all]: until no foe is within R (default 3000) for a second
        std::vector<std::string> nums; // R and SECONDS: the arguments other than "all"
        std::ranges::copy_if(s.args, std::back_inserter(nums), [](const std::string& w) { return w != "all"; });
        const float range = nums.size() > 0 ? std::stof(nums[0]) : 3000.0f;
        const uint64_t limit = nums.size() > 1 ? ticks(nums[1]) : 60 * 60;
        if (t == 0) clearSince_ = 0, healAt_ = 0, lostSince_ = 0, foeUid_ = 0, foeHp_ = 0, hitSince_ = tick;
        const game::ai::Enemy* foe = nullptr;
        float dist = 0;
        // Foes: enemies aware of her (not idle, patrolling, asleep, going home or still clinging to a wall), or the one the
        // target lock holds; an unaware one behind a wall does not keep the fight going. `all`: every living enemy within
        // R (hunting them down).
        using M = game::ai::Mode;
        for (const auto& e : w.enemies().all()) {
            const M m = e->state().mode;
            const bool unaware = m == M::Idle || m == M::Patrol || m == M::Sleep || m == M::Cling || m == M::Return;
            if (!e->alive() || (unaware && !has("all") && w.combat().target != e.get())) continue;
            if (const float d = glm::distance(e->state().pos, lara.pos); d <= range && (!foe || d < dist)) foe = e.get(), dist = d;
        }
        const auto release = [&] { set(tick, "aim", 0, a), set(tick, "fire", 0, a), set(tick, "interact", 0, a), set(tick, "heal", 0, a); };
        const auto& c = w.combat();
        if (!foe) { // the lock stays held for the second: an unaware enemy in sight is still locked onto (and fought)
            if (!clearSince_) clearSince_ = tick;
            set(tick, "fire", 0, a), set(tick, "interact", 0, a), set(tick, "heal", 0, a), set(tick, "aim", 1, a), centre();
            if (tick - clearSince_ >= 60) {
                release();
                std::fprintf(stderr, "ou-game: autopilot tick %llu fight: none left within %.0f, health %.0f, shots %d\n", (unsigned long long)tick,
                             range, w.health().value(), c.shotsFired);
                next(tick, nullptr);
            }
        } else if (t > limit) {
            release(), centre();
            next(tick, ("timed out: enemy " + std::to_string(foe->uid()) + " alive " + std::to_string(int(dist)) + " away").c_str());
        } else {
            clearSince_ = 0;
            if (foe->uid() != foeUid_ || foe->state().health < foeHp_) hitSince_ = tick; // a new foe, or this one hurt
            foeUid_ = foe->uid(), foeHp_ = foe->state().health;
            const glm::vec2 to = glm::vec2(foe->state().pos) - glm::vec2(lara.pos);
            const glm::vec2 dir = glm::length(to) > 1 ? glm::normalize(to) : glm::vec2(std::cos(lara.yaw), std::sin(lara.yaw));
            // A health pack when she is low (one press, then 2 s before the next).
            const bool heal = w.health().value() < 0.35f * w.health().max() && w.found[size_t(game::PickupKind::HealthPack)] > 0 &&
                              (!healAt_ || tick - healAt_ > 120);
            if (heal) healAt_ = tick;
            set(tick, "heal", heal ? 1.0f : 0.0f, a);
            if (foe->regenerating()) { // a thrall knocked down: the finishing move (Interact by it, weapons off the lock)
                set(tick, "aim", 0, a), set(tick, "fire", 0, a);
                if (glm::length(to) > 130) steer(dir, glm::length(to) < 400);
                else centre();
                set(tick, "interact", glm::length(to) < 260 && t % 12 < 6 ? 1.0f : 0.0f, a);
            } else {
                set(tick, "interact", 0, a);
                set(tick, "aim", 1, a); // target lock (draws the weapons)
                set(tick, "fire", c.drawn && c.target ? 1.0f : 0.0f, a);
                if (c.target) lostSince_ = 0;
                else if (!lostSince_) lostSince_ = tick;
                // Too close: back off. No sight of it for 1.5 s, or 4 s of fire that did not hurt it (cover, a railing in
                // the way): go nearer for a while.
                const bool blocked = (lostSince_ && tick - lostSince_ > 90) || tick - hitSince_ > 240;
                if (dist < 350) steer(-dir, false);
                else if (blocked && dist > 500) steer(dir, false);
                else centre();
                if (tick - hitSince_ > 420) hitSince_ = tick; // 3 s nearer, then fire from there again
            }
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
