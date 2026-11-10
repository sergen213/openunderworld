// ou-game --autopilot PLAN --record-script OUT: a developer tool that steers Lara through a plan of waypoints and
// presses and writes the actions it gave as an input script (tests/replays/*_insert.txt, docs/GAMEPLAY.md "Receptacle
// replays"). It reads World state only to choose the stick; the script it writes replays the same ticks.
#pragma once

#include "game/Game.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ou::app {

class Autopilot {
public:
    // Plan lines ('#' comments):
    //   go X Y [R] [walk]           steer to (X, Y) until within R (default 40) units
    //   engage UID [X Y] [walk]     steer to (X, Y) (default: its nearest engagement record) until Lara engages UID
    //   face YAWDEG                 walk-turn towards the heading until within 8 degrees
    //   press [ACTION]              hold ACTION (interact) 6 ticks, release it 6 ticks
    //   hold ACTION SECONDS         hold ACTION, then release it
    //   stick FORWARD RIGHT SECONDS [ACTION AT] raw camera-relative stick, ACTION pressed AT seconds in, then centre it
    //   run YAWDEG SECONDS [ACTION AT] run along a world heading, ACTION pressed AT seconds in (running jumps)
    //   wait SECONDS
    //   push FORWARD RIGHT SECONDS [ACTION] hold ACTION (interact), after 6 ticks the raw stick too (push blocks, handles)
    //   near R                      log the markup polylines within R units of Lara (poles, ledges)
    //   map R [CELL] [UP]           log the floor within R of Lara as a character grid, one cell per CELL (100) units: its
    //                               height against her feet in hundreds ('.', '1'..'9' up, 'a'..'i' down), '#' a wall
    //   ride X Y [R] [THROTTLE] [SPEED] on the motorcycle: steer to (X, Y) with the raw stick until within R (default 300),
    //                               (or once past it), throttle THROTTLE (default 1) while slower than SPEED units/s,
    //                               coasting above it, braking above 1.25 SPEED
    //   swim X Y Z [R]              in water: the stick towards (X, Y), Jump / Drop held while the point lies above /
    //                               below her, until within R (default 150) units in space; fails after 60 s
    // A step that does not finish in 20 s fails the plan (logged; the script so far is still written).
    static Autopilot load(const std::filesystem::path& plan, std::filesystem::path out);
    // The same plan syntax from text, nothing written (ou-game --self-test steps, app/game/SelfTest.hpp). Throws
    // std::runtime_error naming the line.
    static Autopilot parse(std::string_view plan);
    game::Actions step(uint64_t tick, game::World& w); // the actions for this tick (recorded when they change)
    bool done() const { return step_ >= steps_.size(); }
    bool failed() const { return failed_; } // a step timed out / could not run: the plan stopped there
    const std::string& failure() const { return failure_; } // "line N (op): why", empty when it has not failed
    void finish(uint64_t tick); // centre everything and write the script (load()'s `out`; parse(): no file)

private:
    struct Step {
        std::string op;
        std::vector<std::string> args;
        int line = 0;
    };
    void set(uint64_t tick, const std::string& action, float value, game::Actions& a);
    void next(uint64_t tick, const char* why);

    std::vector<Step> steps_;
    size_t step_ = 0;
    uint64_t stepStart_ = 0;
    bool started_ = false, reported_ = false, failed_ = false;
    std::string failure_;
    std::filesystem::path out_;
    std::map<std::string, float> held_; // the recorded value of each action
    glm::vec2 rideFrom_{0};             // `ride`: where the step began (a waypoint passed wide still counts)
    std::string script_;
    bool written_ = false;
};

} // namespace ou::app
