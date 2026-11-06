// Device mapping (SDL3) and scripted input replays.
#include "game/Game.hpp"
#include "locale/Locale.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ou::game {

namespace {
float deadzone(float v) { return std::abs(v) < 0.15f ? 0.0f : (v - std::copysign(0.15f, v)) / 0.85f; }
constexpr float kPadLookRate = 2.5f; // rad/s at full deflection
int stick(const std::string& s) { return s == "left" ? 0 : s == "right" ? 1 : -1; }
// A stick (0 left, 1 right) with its dead zone, y up; zero for none / no pad.
vec2 stickAxes(const platform::DeviceState& d, int s) {
    if (s < 0 || !d.pad) return vec2(0);
    const auto x = s ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX, y = s ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY;
    return {deadzone(d.padAxes[x] / 32767.0f), -deadzone(d.padAxes[y] / 32767.0f)};
}
} // namespace

Controls::Controls(const config::InputConfig& c)
    : mouseScale(c.mouseSensitivity), invertY(c.invertY), invertX(c.invertX), moveStick(stick(c.moveStick)), lookStick(stick(c.lookStick)) {
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

int Controls::menuNav(const platform::InputRef& r) const {
    for (int n = 0; n < 8; ++n)
        if (bound(Action(MenuUp + n), r)) return n;
    return -1;
}

Actions readDevices(const Controls& c, const platform::DeviceState& d) {
    using A = Controls::Action;
    const auto on = [&](A action) {
        return std::ranges::any_of(c.actions[action], [&](const platform::InputRef& r) { return platform::pressed(r, d); });
    };
    Actions a;
    a.move = vec2(float(on(A::Right) - on(A::Left)), float(on(A::Forward) - on(A::Back))) + stickAxes(d, c.moveStick);
    if (glm::length(a.move) > 1) a.move = glm::normalize(a.move);
    const vec2 look = stickAxes(d, c.lookStick); // right / up turn the camera right / up
    a.look = vec2(c.invertX ? look.x : -look.x, c.invertY ? -look.y : look.y) * kPadLookRate;
    a.walk = on(A::Walk), a.sprint = on(A::Sprint), a.jump = on(A::Jump), a.drop = on(A::Drop), a.interact = on(A::Interact);
    a.draw = on(A::Draw), a.fire = on(A::Fire), a.aim = on(A::Aim), a.switchTarget = on(A::Switch), a.weapon = on(A::Weapon);
    a.heal = on(A::Heal), a.grapple = on(A::Grapple), a.save = on(A::Save), a.load = on(A::Load);
    a.hint = on(A::Hint), a.answer = on(A::Answer), a.pda = on(A::Pda), a.light = on(A::Light), a.grenade = on(A::Grenade);
    a.recenter = on(A::Recenter), a.fieldCamera = on(A::Camera);
    return a;
}

namespace {
std::string utf8(char32_t cp) { // U+0800..U+FFFF
    return std::string{char(0xE0 | cp >> 12), char(0x80 | (cp >> 6 & 0x3F)), char(0x80 | (cp & 0x3F))};
}
// Xbox 360 pad names (locals.bin 5061..5076 [O]): icon per SDL button, Start / Back the text strings 5071 / 5072.
std::string padName(const platform::InputRef& r, const locale::Strings& strings) {
    if (r.kind == platform::InputRef::Kind::PadTrigger) return utf8(r.code == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? 0x1805 : 0x1807);
    switch (r.code) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return utf8(0x1803);
    case SDL_GAMEPAD_BUTTON_EAST: return utf8(0x1802);
    case SDL_GAMEPAD_BUTTON_WEST: return utf8(0x1800);
    case SDL_GAMEPAD_BUTTON_NORTH: return utf8(0x1801);
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return utf8(0x1804);
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return utf8(0x1806);
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return utf8(0x180C);
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return utf8(0x180D);
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return utf8(0x1808);
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return utf8(0x1809);
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return utf8(0x180B);
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return utf8(0x180A);
    case SDL_GAMEPAD_BUTTON_START: return std::string(strings.get(5071));
    case SDL_GAMEPAD_BUTTON_BACK: return std::string(strings.get(5072));
    default: return platform::inputName(r);
    }
}
// Keys and mouse buttons as tru.exe names them ("BackSpace", "L Mouse" = locals.bin 5050..5052) [O].
std::string keyName(const platform::InputRef& r, const locale::Strings& strings) {
    if (r.kind == platform::InputRef::Kind::Mouse && r.code >= 1 && r.code <= 3) return std::string(strings.get(int16_t(5049 + r.code)));
    return r.kind == platform::InputRef::Kind::Key && r.code == SDL_SCANCODE_BACKSPACE ? "BackSpace" : platform::inputName(r);
}
} // namespace

std::array<std::string, 28> promptNames(const Controls& c, bool gamepad, const locale::Strings& strings) {
    using platform::InputRef;
    // Virtual button -> config::bindableActions() index (-1: none of ours), then the menu keys Return / Escape / BackSpace
    // (25..27: menu_accept / menu_back / menu_delete); and the placeholder naming it by default (tru.exe 0x8A9EF8
    // inverted), shown on the pad when it is unbound.
    static constexpr int8_t kAction[28] = {8, 15, 7, 6, 11, 10, 5, 22, 23, 12, 25, 20, 14, 24, 21, 13, 3, 2, 1, 0, 4, 10, 7, 0, 1, 30, 31, 32};
    static constexpr char32_t kIcon[28] = {0x1801, 0x1800, 0x1802, 0x1803, 0x1805, 0x1807, 0x1804, 0x1806, 0x180E, 0x180F, 0x1810, 0x1811, 0x1808,
                                           0x1809, 0x180B, 0x180A, 0x1817, 0x1816, 0x1815, 0x1814, 0, 0x1826, 0x1827, 0x1828, 0x1829,
                                           0x1803, 0x1802, 0x1801};
    std::array<std::string, 28> names;
    for (int b = 0; b < 28; ++b) {
        const auto& refs = kAction[b] >= 0 ? c.actions[size_t(kAction[b])] : std::vector<InputRef>{};
        auto first = [&](auto pred) { return std::find_if(refs.begin(), refs.end(), pred); };
        if (gamepad) {
            const auto it = first(platform::isPad);
            names[size_t(b)] = it != refs.end() ? padName(*it, strings) : kIcon[b] ? utf8(kIcon[b]) : std::string();
        } else if (auto it = first([](const InputRef& r) { return r.kind == InputRef::Kind::Key; }); it != refs.end()) {
            names[size_t(b)] = keyName(*it, strings);
        } else if (it = first([](const InputRef& r) { return r.kind == InputRef::Kind::Mouse; }); it != refs.end()) {
            names[size_t(b)] = keyName(*it, strings);
        }
    }
    return names;
}

std::string boundNames(const config::InputConfig& c, const std::string& action, bool gamepad, const locale::Strings& strings) {
    std::string out;
    for (const auto& n : c.bindings.at(action))
        if (const auto r = platform::parseInput(n); r && platform::isPad(*r) == gamepad)
            out += (out.empty() ? "" : "/") + (gamepad ? padName(*r, strings) : keyName(*r, strings));
    return out;
}

void rebind(config::InputConfig& c, const std::string& action, const platform::InputRef& r) {
    auto& names = c.bindings.at(action);
    std::erase_if(names, [&](const std::string& n) {
        const auto o = platform::parseInput(n);
        return !o || platform::isPad(*o) == platform::isPad(r);
    });
    names.push_back(platform::inputName(r));
}

void unmap(config::InputConfig& c, const std::string& action, bool gamepad) {
    std::erase_if(c.bindings.at(action), [&](const std::string& n) {
        const auto o = platform::parseInput(n);
        return !o || platform::isPad(*o) == gamepad;
    });
}

InputScript InputScript::parse(std::string_view text) {
    static constexpr std::string_view kActions[] = {"forward", "right", "walk", "jump", "drop", "lookx", "looky", "interact", "save",
                                                    "load", "draw", "fire", "aim", "switch", "weapon", "heal", "pause", "grapple", "sprint", "hint", "answer", "pda",
                                                    "light", "grenade", "recenter", "camera"};
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
        else if (e.action == "recenter") a.recenter = e.value != 0;
        else if (e.action == "camera") a.fieldCamera = e.value != 0;
        else if (e.action == "lookx") a.look.x = e.value;
        else a.look.y = e.value;
    }
    return a;
}

} // namespace ou::game
