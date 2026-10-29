// Device mapping (SDL3) and scripted input replays.
#include "game/Game.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ou::game {

namespace {
float deadzone(float v) { return std::abs(v) < 0.15f ? 0.0f : (v - std::copysign(0.15f, v)) / 0.85f; }
float axis(SDL_Gamepad* g, SDL_GamepadAxis a) { return g ? deadzone(SDL_GetGamepadAxis(g, a) / 32767.0f) : 0.0f; }
constexpr float kPadLookRate = 2.5f; // rad/s at full deflection
} // namespace

Controls::Controls(const config::InputConfig& c) : mouseScale(c.mouseSensitivity), invertY(c.invertY) {
    for (const auto& action : config::bindableActions()) {
        auto& refs = actions.emplace_back();
        if (const auto it = c.bindings.find(action); it != c.bindings.end())
            for (const auto& name : it->second) {
                const auto r = platform::parseInput(name);
                if (!r) throw std::runtime_error("control '" + action + "': unknown input '" + name + "'");
                refs.push_back(*r);
            }
    }
}

Actions readDevices(const Controls& c, const bool* keys, SDL_Gamepad* pad, uint32_t mouseButtons) {
    enum { Forward, Back, Left, Right, Walk, Sprint, Jump, Drop, Interact, Draw, Fire, Aim, Switch, Weapon, Heal, Grapple, Save, Load, Hint, Answer, Pda, Light, Grenade };
    const auto on = [&](int action) {
        return std::any_of(c.actions[action].begin(), c.actions[action].end(),
                           [&](const platform::InputRef& r) { return platform::pressed(r, keys, pad, mouseButtons); });
    };
    Actions a;
    a.move.y = float(on(Forward) - on(Back)) - axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    a.move.x = float(on(Right) - on(Left)) + axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    if (glm::length(a.move) > 1) a.move = glm::normalize(a.move);
    a.look = {-axis(pad, SDL_GAMEPAD_AXIS_RIGHTX) * kPadLookRate, -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY) * kPadLookRate};
    if (c.invertY) a.look.y = -a.look.y;
    a.walk = on(Walk), a.sprint = on(Sprint), a.jump = on(Jump), a.drop = on(Drop), a.interact = on(Interact);
    a.draw = on(Draw), a.fire = on(Fire), a.aim = on(Aim), a.switchTarget = on(Switch), a.weapon = on(Weapon);
    a.heal = on(Heal), a.grapple = on(Grapple), a.save = on(Save), a.load = on(Load);
    a.hint = on(Hint), a.answer = on(Answer), a.pda = on(Pda), a.light = on(Light), a.grenade = on(Grenade);
    return a;
}

InputScript InputScript::parse(std::string_view text) {
    static constexpr std::string_view kActions[] = {"forward", "right", "walk", "jump", "drop", "lookx", "looky", "interact", "save",
                                                    "load", "draw", "fire", "aim", "switch", "weapon", "heal", "pause", "grapple", "sprint", "hint", "answer", "pda",
                                                    "light", "grenade"};
    InputScript s;
    std::istringstream in{std::string(text)};
    std::string line;
    for (int no = 1; std::getline(in, line); ++no) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::istringstream ls(line);
        double seconds;
        std::string action;
        if (!(ls >> seconds)) {
            if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
            throw std::runtime_error("input script line " + std::to_string(no) + ": expected '<seconds> <action> [value]'");
        }
        if (!(ls >> action) || std::find(std::begin(kActions), std::end(kActions), action) == std::end(kActions) || seconds < 0)
            throw std::runtime_error("input script line " + std::to_string(no) + ": unknown action or negative time");
        float value = 1;
        ls >> value;
        s.events_.push_back({uint64_t(std::llround(seconds / kTickSeconds)), action, value});
    }
    std::stable_sort(s.events_.begin(), s.events_.end(), [](const Event& a, const Event& b) { return a.tick < b.tick; });
    return s;
}

InputScript InputScript::load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read input script " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

Actions InputScript::at(uint64_t tick) const {
    Actions a;
    for (const auto& e : events_) {
        if (e.tick > tick) break;
        if (e.action == "forward") a.move.y = e.value;
        else if (e.action == "right") a.move.x = e.value;
        else if (e.action == "walk") a.walk = e.value != 0;
        else if (e.action == "jump") a.jump = e.value != 0;
        else if (e.action == "drop") a.drop = e.value != 0;
        else if (e.action == "interact") a.interact = e.value != 0;
        else if (e.action == "save") a.save = e.value != 0;
        else if (e.action == "load") a.load = e.value != 0;
        else if (e.action == "draw") a.draw = e.value != 0;
        else if (e.action == "fire") a.fire = e.value != 0;
        else if (e.action == "aim") a.aim = e.value != 0;
        else if (e.action == "switch") a.switchTarget = e.value != 0;
        else if (e.action == "weapon") a.weapon = e.value != 0;
        else if (e.action == "heal") a.heal = e.value != 0;
        else if (e.action == "pause") a.pause = e.value != 0;
        else if (e.action == "grapple") a.grapple = e.value != 0;
        else if (e.action == "sprint") a.sprint = e.value != 0;
        else if (e.action == "hint") a.hint = e.value != 0;
        else if (e.action == "answer") a.answer = e.value != 0;
        else if (e.action == "pda") a.pda = e.value != 0;
        else if (e.action == "light") a.light = e.value != 0;
        else if (e.action == "grenade") a.grenade = e.value != 0;
        else if (e.action == "lookx") a.look.x = e.value;
        else a.look.y = e.value;
    }
    return a;
}

} // namespace ou::game
