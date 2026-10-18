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

Actions readDevices(const bool* keys, SDL_Gamepad* pad) {
    Actions a;
    a.move.y = float((keys[SDL_SCANCODE_W] || keys[SDL_SCANCODE_UP]) - (keys[SDL_SCANCODE_S] || keys[SDL_SCANCODE_DOWN])) -
               axis(pad, SDL_GAMEPAD_AXIS_LEFTY);
    a.move.x = float((keys[SDL_SCANCODE_D] || keys[SDL_SCANCODE_RIGHT]) - (keys[SDL_SCANCODE_A] || keys[SDL_SCANCODE_LEFT])) +
               axis(pad, SDL_GAMEPAD_AXIS_LEFTX);
    if (glm::length(a.move) > 1) a.move = glm::normalize(a.move);
    a.look = {-axis(pad, SDL_GAMEPAD_AXIS_RIGHTX) * kPadLookRate, -axis(pad, SDL_GAMEPAD_AXIS_RIGHTY) * kPadLookRate};
    a.walk = keys[SDL_SCANCODE_LSHIFT] || keys[SDL_SCANCODE_RSHIFT] || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER));
    a.jump = keys[SDL_SCANCODE_SPACE] || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH));
    a.drop = keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL] || keys[SDL_SCANCODE_C] ||
             (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST));
    a.interact = keys[SDL_SCANCODE_E] || (pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_WEST));
    a.save = keys[SDL_SCANCODE_F5];
    a.load = keys[SDL_SCANCODE_F9];
    return a;
}

InputScript InputScript::parse(std::string_view text) {
    static constexpr std::string_view kActions[] = {"forward", "right", "walk", "jump", "drop", "lookx", "looky", "interact", "save", "load"};
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
        else if (e.action == "lookx") a.look.x = e.value;
        else a.look.y = e.value;
    }
    return a;
}

} // namespace ou::game
