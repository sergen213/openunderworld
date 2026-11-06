#include "platform/Input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace ou::platform {

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}
constexpr std::pair<const char*, int> kMouse[] = {{"mouse left", SDL_BUTTON_LEFT}, {"mouse middle", SDL_BUTTON_MIDDLE},
                                                  {"mouse right", SDL_BUTTON_RIGHT}};
constexpr int16_t kHalf = 16384; // a trigger "pressed" / an axis taking over a pad
bool trigger(int axis) { return axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER; }
} // namespace

std::optional<InputRef> parseInput(std::string_view name) {
    const std::string n = lower(name);
    for (auto [m, b] : kMouse)
        if (n == m) return InputRef{InputRef::Kind::Mouse, b};
    if (n.starts_with("pad ")) {
        const std::string rest = n.substr(4);
        if (const auto b = SDL_GetGamepadButtonFromString(rest.c_str()); b != SDL_GAMEPAD_BUTTON_INVALID)
            return InputRef{InputRef::Kind::PadButton, int(b)};
        if (const auto a = SDL_GetGamepadAxisFromString(rest.c_str()); trigger(a)) return InputRef{InputRef::Kind::PadTrigger, int(a)};
        return std::nullopt;
    }
    const std::string s(name);
    if (const auto k = SDL_GetScancodeFromName(s.c_str()); k != SDL_SCANCODE_UNKNOWN) return InputRef{InputRef::Kind::Key, int(k)};
    return std::nullopt;
}

std::string inputName(const InputRef& r) {
    switch (r.kind) {
    case InputRef::Kind::Key: return SDL_GetScancodeName(SDL_Scancode(r.code));
    case InputRef::Kind::Mouse: return r.code == SDL_BUTTON_LEFT ? "Mouse Left" : r.code == SDL_BUTTON_RIGHT ? "Mouse Right" : "Mouse Middle";
    case InputRef::Kind::PadButton: return std::string("Pad ") + SDL_GetGamepadStringForButton(SDL_GamepadButton(r.code));
    case InputRef::Kind::PadTrigger: return std::string("Pad ") + SDL_GetGamepadStringForAxis(SDL_GamepadAxis(r.code));
    }
    return {};
}

bool DeviceState::apply(const SDL_Event& e) {
    auto clearPad = [&] { padButtons.fill(false), padAxes.fill(0); };
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        if (e.key.scancode > SDL_SCANCODE_UNKNOWN && e.key.scancode < SDL_SCANCODE_COUNT) keys[e.key.scancode] = e.key.down;
        return false;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button >= 1 && e.button.button <= 32)
            mouseButtons = e.button.down ? mouseButtons | SDL_BUTTON_MASK(e.button.button) : mouseButtons & ~SDL_BUTTON_MASK(e.button.button);
        return false;
    case SDL_EVENT_GAMEPAD_ADDED:
        if (pad) return false;
        pad = e.gdevice.which, clearPad();
        return true;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (e.gdevice.which != pad) return false;
        pad = 0, clearPad();
        return false;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        if (!pad && e.gbutton.down) pad = e.gbutton.which, clearPad(); // the pad in use went: this one takes over
        if (e.gbutton.which != pad || e.gbutton.button >= SDL_GAMEPAD_BUTTON_COUNT) return false;
        padButtons[e.gbutton.button] = e.gbutton.down;
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (!pad && std::abs(int(e.gaxis.value)) > kHalf) pad = e.gaxis.which, clearPad();
        if (e.gaxis.which != pad || e.gaxis.axis >= SDL_GAMEPAD_AXIS_COUNT) return false;
        padAxes[e.gaxis.axis] = e.gaxis.value;
        return true;
    default: return false;
    }
}

bool pressed(const InputRef& r, const DeviceState& d) {
    switch (r.kind) {
    case InputRef::Kind::Key: return r.code > 0 && r.code < SDL_SCANCODE_COUNT && d.keys[size_t(r.code)];
    case InputRef::Kind::Mouse: return d.mouseButtons & SDL_BUTTON_MASK(r.code);
    case InputRef::Kind::PadButton: return d.pad && r.code >= 0 && r.code < SDL_GAMEPAD_BUTTON_COUNT && d.padButtons[size_t(r.code)];
    case InputRef::Kind::PadTrigger: return d.pad && trigger(r.code) && d.padAxes[size_t(r.code)] > kHalf;
    }
    return false;
}

std::optional<InputRef> pressedBy(const SDL_Event& e) {
    using K = InputRef::Kind;
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
        if (e.key.scancode == SDL_SCANCODE_UNKNOWN) return std::nullopt;
        return InputRef{K::Key, int(e.key.scancode)};
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        if (e.button.button < SDL_BUTTON_LEFT || e.button.button > SDL_BUTTON_RIGHT) return std::nullopt;
        return InputRef{K::Mouse, int(e.button.button)};
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN: return InputRef{K::PadButton, int(e.gbutton.button)};
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        // ponytail: a trigger reads as pressed on every motion event past half; callers use it on press edges (menus, captures).
        if (trigger(e.gaxis.axis) && e.gaxis.value > kHalf) return InputRef{K::PadTrigger, int(e.gaxis.axis)};
        return std::nullopt;
    default: return std::nullopt;
    }
}

std::string eventLine(uint64_t tick, const SDL_Event& e) {
    std::ostringstream o;
    o << tick << ' ';
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: o << "key " << int(e.key.scancode) << ' ' << int(e.key.down) << (e.key.repeat ? " repeat" : ""); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: o << "mouse " << int(e.button.button) << ' ' << int(e.button.down); break;
    case SDL_EVENT_MOUSE_MOTION: o << "motion " << e.motion.xrel << ' ' << e.motion.yrel; break;
    case SDL_EVENT_GAMEPAD_ADDED: o << "padadd " << e.gdevice.which; break;
    case SDL_EVENT_GAMEPAD_REMOVED: o << "padremove " << e.gdevice.which; break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: o << "button " << e.gbutton.which << ' ' << int(e.gbutton.button) << ' ' << int(e.gbutton.down); break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: o << "axis " << e.gaxis.which << ' ' << int(e.gaxis.axis) << ' ' << e.gaxis.value; break;
    default: return {};
    }
    return o.str();
}

EventLog EventLog::parse(std::string_view text) {
    EventLog log;
    std::istringstream in{std::string(text)};
    std::string line;
    uint64_t last = 0;
    for (int no = 1; std::getline(in, line); ++no) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        if (line.find_first_not_of(" \t\r") == std::string::npos) continue;
        const auto bad = [&](const char* why) { return std::runtime_error("input recording line " + std::to_string(no) + ": " + why); };
        std::istringstream ls(line);
        uint64_t tick;
        std::string kind;
        if (!(ls >> tick >> kind)) throw bad("expected '<tick> <event> ...'");
        if (tick < last) throw bad("ticks go backwards");
        last = tick;
        SDL_Event e{};
        e.common.timestamp = 0;
        long long a = 0, b = 0, c = 0;
        if (kind == "key") {
            std::string repeat;
            if (!(ls >> a >> b) || a <= 0 || a >= SDL_SCANCODE_COUNT) throw bad("key <scancode> <0|1> [repeat]");
            ls >> repeat;
            e.type = b ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            e.key.scancode = SDL_Scancode(a), e.key.down = b != 0, e.key.repeat = repeat == "repeat";
            e.key.key = SDL_GetKeyFromScancode(e.key.scancode, SDL_KMOD_NONE, false);
        } else if (kind == "mouse") {
            if (!(ls >> a >> b) || a < 1 || a > 32) throw bad("mouse <button> <0|1>");
            e.type = b ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            e.button.button = uint8_t(a), e.button.down = b != 0, e.button.clicks = 1;
        } else if (kind == "motion") {
            float x, y;
            if (!(ls >> x >> y)) throw bad("motion <dx> <dy>");
            e.type = SDL_EVENT_MOUSE_MOTION, e.motion.xrel = x, e.motion.yrel = y;
        } else if (kind == "padadd" || kind == "padremove") {
            if (!(ls >> a) || a <= 0) throw bad("padadd|padremove <id>");
            e.type = kind == "padadd" ? SDL_EVENT_GAMEPAD_ADDED : SDL_EVENT_GAMEPAD_REMOVED, e.gdevice.which = SDL_JoystickID(a);
        } else if (kind == "button") {
            if (!(ls >> a >> b >> c) || a <= 0 || b < 0 || b >= SDL_GAMEPAD_BUTTON_COUNT) throw bad("button <id> <button> <0|1>");
            e.type = c ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
            e.gbutton.which = SDL_JoystickID(a), e.gbutton.button = uint8_t(b), e.gbutton.down = c != 0;
        } else if (kind == "axis") {
            if (!(ls >> a >> b >> c) || a <= 0 || b < 0 || b >= SDL_GAMEPAD_AXIS_COUNT || c < -32768 || c > 32767)
                throw bad("axis <id> <axis> <value>");
            e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
            e.gaxis.which = SDL_JoystickID(a), e.gaxis.axis = uint8_t(b), e.gaxis.value = int16_t(c);
        } else {
            throw bad("unknown event");
        }
        log.events_.emplace_back(tick, e);
    }
    return log;
}

EventLog EventLog::load(const std::filesystem::path& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read input recording " + path.string());
    std::stringstream ss;
    ss << f.rdbuf();
    return parse(ss.str());
}

std::optional<SDL_Event> EventLog::next(uint64_t tick) {
    if (next_ == events_.size() || events_[next_].first > tick) return std::nullopt;
    return events_[next_++].second;
}

} // namespace ou::platform
