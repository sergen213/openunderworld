// Binding names (ou::config::InputConfig) <-> SDL3 devices. Shared by ou-game and the launcher's control editor.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

struct SDL_Gamepad;

namespace ou::platform {

struct InputRef {
    enum class Kind { Key, Mouse, PadButton, PadTrigger } kind;
    int code; // SDL_Scancode | SDL mouse button index (1 left, 2 middle, 3 right) | SDL_GamepadButton | SDL_GamepadAxis
    bool operator==(const InputRef&) const = default;
};

// Case-insensitive; see config::InputConfig for the name forms. nullopt = unknown name.
std::optional<InputRef> parseInput(std::string_view name);
// Canonical name ("Left Shift", "Mouse Right", "Pad dpright"), parseInput(inputName(r)) == r.
std::string inputName(const InputRef& r);
// keys = SDL_GetKeyboardState, mouseButtons = SDL_GetMouseState mask; pad may be null.
bool pressed(const InputRef& r, const bool* keys, SDL_Gamepad* pad, uint32_t mouseButtons);

} // namespace ou::platform
