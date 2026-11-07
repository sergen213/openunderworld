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
    //   stick FORWARD RIGHT SECONDS raw camera-relative stick, then centre it
    //   wait SECONDS
    //   push FORWARD RIGHT SECONDS [ACTION] hold ACTION (interact), after 6 ticks the raw stick too (push blocks, handles)
    //   near R                      log the markup polylines within R units of Lara (poles, ledges)
    // A step that does not finish in 20 s fails the plan (logged; the script so far is still written).
    static Autopilot load(const std::filesystem::path& plan, std::filesystem::path out);
    game::Actions step(uint64_t tick, game::World& w); // the actions for this tick (recorded when they change)
    bool done() const { return step_ >= steps_.size(); }
    void finish(uint64_t tick); // centre everything and write the script

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
    bool started_ = false, reported_ = false;
    std::filesystem::path out_;
    std::map<std::string, float> held_; // the recorded value of each action
    std::string script_;
    bool written_ = false;
};

} // namespace ou::app
