#include "platform/Input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>

namespace ou::platform {

namespace {
std::string lower(std::string_view s) {
    std::string r(s);
    std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return r;
}
constexpr std::pair<const char*, int> kMouse[] = {{"mouse left", SDL_BUTTON_LEFT}, {"mouse middle", SDL_BUTTON_MIDDLE},
                                                  {"mouse right", SDL_BUTTON_RIGHT}};
} // namespace

std::optional<InputRef> parseInput(std::string_view name) {
    const std::string n = lower(name);
    for (auto [m, b] : kMouse)
        if (n == m) return InputRef{InputRef::Kind::Mouse, b};
    if (n.starts_with("pad ")) {
        const std::string rest = n.substr(4);
        if (const auto b = SDL_GetGamepadButtonFromString(rest.c_str()); b != SDL_GAMEPAD_BUTTON_INVALID)
            return InputRef{InputRef::Kind::PadButton, int(b)};
        if (const auto a = SDL_GetGamepadAxisFromString(rest.c_str());
            a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || a == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER)
            return InputRef{InputRef::Kind::PadTrigger, int(a)};
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

bool pressed(const InputRef& r, const bool* keys, SDL_Gamepad* pad, uint32_t mouseButtons) {
    switch (r.kind) {
    case InputRef::Kind::Key: return keys && keys[r.code];
    case InputRef::Kind::Mouse: return mouseButtons & SDL_BUTTON_MASK(r.code);
    case InputRef::Kind::PadButton: return pad && SDL_GetGamepadButton(pad, SDL_GamepadButton(r.code));
    case InputRef::Kind::PadTrigger: return pad && SDL_GetGamepadAxis(pad, SDL_GamepadAxis(r.code)) > 16384;
    }
    return false;
}

} // namespace ou::platform
